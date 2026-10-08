#include "HalTintaJournalStorage.h"

#include <Logging.h>
#include <mbedtls/sha256.h>

#include "CompanionTintaJournalPaths.h"

namespace companion {
namespace {
bool failure(const char* operation) {
  LOG_ERR("COMPANION", "Tinta journal %s failed", operation);
  return false;
}
}  // namespace

HalTintaJournalStorage::HalTintaJournalStorage(TintaJournalLocation location)
    : paths(tintaJournalPaths(location)), headerLookup(nullptr, nullptr, paths ? paths->directory : nullptr) {}
HalTintaJournalStorage::~HalTintaJournalStorage() { close(); }
bool HalTintaJournalStorage::close() {
  const bool closed = !events.isOpen() || events.close();
  readsSinceYield = 0;
  return closed || failure("close");
}
bool HalTintaJournalStorage::prepare() {
  if (!paths) return failure("invalid journal location");
  if (!Storage.ready()) return failure("SD unavailable");
  if (events) return true;
  if (!Storage.ensureDirectoryExists(paths->directory)) return failure("mkdir");
  // Retain the mutex-wrapped handle so scans do not allocate one handle per record.
  events = Storage.open(paths->events, O_RDWR | O_CREAT);
  if (!events || events.isDirectory()) {
    close();
    return failure("open");
  }
  return true;
}
bool HalTintaJournalStorage::size(uint32_t& bytes) {
  if (!prepare()) return false;
  const uint64_t length = events.fileSize64();
  if (length > UINT32_MAX) return failure("size overflow");
  bytes = static_cast<uint32_t>(length);
  return true;
}
bool HalTintaJournalStorage::read(uint32_t offset, std::span<uint8_t> bytes) {
  if (!prepare() || bytes.size() > TintaJournal::EXTENDED_RECORD_SIZE || !events.seek64(offset) ||
      events.read(bytes.data(), bytes.size()) != static_cast<int>(bytes.size()))
    return failure("read");
  if (++readsSinceYield == 32) {
    readsSinceYield = 0;
    vTaskDelay(1);
  }
  return true;
}
bool HalTintaJournalStorage::write(uint32_t offset, std::span<const uint8_t> bytes) {
  if (!prepare() || (bytes.size() != TintaJournal::RECORD_SIZE && bytes.size() != TintaJournal::EXTENDED_RECORD_SIZE) ||
      offset % bytes.size() != 0 || offset != events.fileSize64() || !events.seek64(offset) ||
      events.write(bytes.data(), bytes.size()) != bytes.size() || !events.sync())
    return failure("append/sync");
  return true;
}
bool HalTintaJournalStorage::truncate(uint32_t bytes) {
  if (!prepare() || bytes > events.fileSize64() || !events.truncate(bytes) || !events.sync())
    return failure("truncate/sync");
  return true;
}
bool HalTintaJournalStorage::readHeader(uint8_t slot, std::span<uint8_t> bytes, size_t& length) {
  length = 0;
  if (slot > 1 || bytes.size() != TintaJournal::HEADER_SIZE || !prepare()) return failure("header arguments");
  const char* path = (slot == 0 ? paths->headerA : paths->headerB);
  const auto presence = headerLookup.inspect(path);
  if (presence == CompanionFilePresence::Error) return failure("header lookup");
  if (presence == CompanionFilePresence::Missing) return true;
  HalFile file;
  if (!Storage.openFileForRead("COMPANION", path, file) || file.isDirectory()) return failure("header open");
  const uint64_t actual = file.fileSize64();
  if (actual != bytes.size()) {
    length = bytes.size() + 1;
    return true;
  }
  length = bytes.size();
  if (file.read(bytes.data(), bytes.size()) != static_cast<int>(bytes.size())) return failure("header read");
  return true;
}
bool HalTintaJournalStorage::writeHeader(uint8_t slot, std::span<const uint8_t> bytes) {
  if (slot > 1 || bytes.size() != TintaJournal::HEADER_SIZE || !prepare()) return failure("header arguments");
  auto file = Storage.open((slot == 0 ? paths->headerA : paths->headerB), O_WRONLY | O_CREAT);
  if (!file || file.isDirectory() || !file.seek64(0) || file.write(bytes.data(), bytes.size()) != bytes.size() ||
      !file.truncate(bytes.size()) || !file.sync())
    return failure("header write/sync");
  return true;
}
bool HalTintaJournalStorage::digest(std::span<const uint8_t> bytes, Digest& output) {
  Digest computed{};
  if (mbedtls_sha256(bytes.data(), bytes.size(), computed.data(), 0) != 0) return failure("SHA-256");
  output = computed;
  return true;
}

}  // namespace companion
