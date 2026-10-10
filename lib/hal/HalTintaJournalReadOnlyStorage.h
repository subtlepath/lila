#pragma once

#include <mbedtls/sha256.h>

#include "HalTintaJournalStorage.h"

namespace companion {
// Admit off stack under an exclusive journal/workspace lease. Callbacks preserve
// that lease. No directories, events, headers, or uncommitted tails are changed.
class HalTintaJournalReadOnlyStorage final : public TintaJournalStorage {
 public:
  using Permission = bool (*)(void*);
  HalTintaJournalReadOnlyStorage(TintaJournalLocation location, Permission permitted, void* context)
      : paths(tintaJournalPaths(location)),
        permitted(permitted),
        context(context),
        lookup(permitted, context, paths ? paths->directory : nullptr) {}
  ~HalTintaJournalReadOnlyStorage() override { close(); }
  HalTintaJournalReadOnlyStorage(const HalTintaJournalReadOnlyStorage&) = delete;
  HalTintaJournalReadOnlyStorage& operator=(const HalTintaJournalReadOnlyStorage&) = delete;
  bool close() {
    const bool eventsClosed = !events.isOpen() || events.close();
    const bool headerClosed = !header.isOpen() || header.close();
    readsSinceYield = 0;
    return (eventsClosed && headerClosed) || failure("close");
  }
  bool size(uint32_t& output) override {
    if (!prepare()) return false;
    const uint64_t length = events.fileSize64();
    if (length > UINT32_MAX || !guard()) return failure("size");
    output = static_cast<uint32_t>(length);
    return true;
  }
  bool read(uint32_t offset, std::span<uint8_t> bytes) override {
    if (!prepare() || bytes.size() > TintaJournal::EXTENDED_RECORD_SIZE || !events.seek64(offset) ||
        events.read(bytes.data(), bytes.size()) != static_cast<int>(bytes.size()) || !guard())
      return failure("read");
    if (++readsSinceYield == 32) {
      readsSinceYield = 0;
      vTaskDelay(1);
    }
    return guard();
  }
  bool readHeader(uint8_t slot, std::span<uint8_t> bytes, size_t& length) override {
    if (slot > 1 || bytes.size() != TintaJournal::HEADER_SIZE || !prepare() || (header.isOpen() && !header.close()))
      return failure("header arguments/close");
    const char* path = slot == 0 ? paths->headerA : paths->headerB;
    const auto presence = lookup.inspect(path);
    if (!guard() || presence == CompanionFilePresence::Error) return failure("header lookup");
    if (presence == CompanionFilePresence::Missing) {
      length = 0;
      return true;
    }
    if (!Storage.openFileForRead("COMPANION", path, header) || header.isDirectory()) return failure("header open");
    const auto actual = header.fileSize64();
    const bool read =
        actual != bytes.size() || header.read(bytes.data(), bytes.size()) == static_cast<int>(bytes.size());
    const bool closed = header.close();
    if (!read || !closed || !guard()) return failure("header read/close");
    length = actual == bytes.size() ? bytes.size() : bytes.size() + 1;
    return true;
  }
  bool digest(std::span<const uint8_t> bytes, Digest& output) override {
    Digest computed{};
    if (!guard() || mbedtls_sha256(bytes.data(), bytes.size(), computed.data(), 0) != 0 || !guard())
      return failure("SHA-256");
    output = computed;
    return true;
  }
  bool write(uint32_t, std::span<const uint8_t>) override { return failure("append refused"); }
  bool truncate(uint32_t) override { return failure("truncate refused"); }
  bool writeHeader(uint8_t, std::span<const uint8_t>) override { return failure("header write refused"); }

 private:
  const TintaJournalPaths* paths;
  Permission permitted;
  void* context;
  HalCompanionFileLookup lookup;
  HalFile events, header;
  uint8_t readsSinceYield = 0;
  bool guard() const { return permitted && permitted(context); }
  bool prepare() {
    if (!paths || !guard() || !Storage.ready()) return failure("admission");
    if (events.isOpen()) return true;
    if (lookup.inspect(paths->events) != CompanionFilePresence::Present || !guard() ||
        !Storage.openFileForRead("COMPANION", paths->events, events) || events.isDirectory() || !guard()) {
      close();
      return failure("events open");
    }
    return true;
  }
  static bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Read-only Tinta journal %s failed", operation);
    return false;
  }
};
}  // namespace companion
