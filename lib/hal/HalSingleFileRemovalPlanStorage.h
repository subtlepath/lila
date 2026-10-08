#pragma once

#include <Logging.h>
#include <mbedtls/sha256.h>

#include "CompanionSingleFileRemovalPlan.h"
#include "HalCompanionFileLookup.h"

namespace companion {
enum class RemovalPlanStorageResult { Ok, Missing, Invalid, Corrupt, IoError };
// Session-owned. Input/retained-plan bytes and comparison scratch must be disjoint.
// The serialized owner excludes concurrent plan and content changes.
class HalSingleFileRemovalPlanStorage final {
 public:
  explicit HalSingleFileRemovalPlanStorage(std::span<uint8_t> comparison) : comparison(comparison) {
    mbedtls_sha256_init(&hashContext);
  }
  ~HalSingleFileRemovalPlanStorage() {
    close();
    mbedtls_sha256_free(&hashContext);
  }
  HalSingleFileRemovalPlanStorage(const HalSingleFileRemovalPlanStorage&) = delete;
  HalSingleFileRemovalPlanStorage& operator=(const HalSingleFileRemovalPlanStorage&) = delete;
  const char* publishedPath() const { return ready ? target.data() : nullptr; }
  RemovalPlanStorageResult publish(std::span<const uint8_t> bytes, Digest& output) {
    ready = false;
    if (comparison.empty() || !disjoint(bytes, comparison) || !decodeSingleFileRemovalPlan(bytes, parsed))
      return RemovalPlanStorageResult::Invalid;
    if (!close() || !prepare() || !digest(bytes) || !paths(actual)) return io("publish preparation");
    const auto existing = inspect(target.data(), bytes);
    if (existing == RemovalPlanStorageResult::Ok) {
      output = actual;
      ready = true;
      return existing;
    }
    if (existing != RemovalPlanStorageResult::Missing) return existing;
    // Only this digest-addressed unpublished stage may be replaced; live targets
    // and directory collisions are preserved. No content file is touched here.
    const auto presence = lookup.inspect(stage.data());
    if (presence == CompanionFilePresence::Error) return io("stage lookup");
    if (presence == CompanionFilePresence::Present) {
      if (!Storage.openFileForReadReusing("COMPANION", stage.data(), file)) return io("stage open");
      const bool regular = !file.isDirectory() && file.fileSize64() <= SINGLE_FILE_REMOVAL_PLAN_MAX_SIZE;
      const bool closed = close();
      if (!regular || !closed) return io("stage collision or close");
    }
    if (!Storage.openFileForWriteReusing("COMPANION", stage.data(), file)) return io("stage write open");
    const bool written = !file.isDirectory() && file.write(bytes.data(), bytes.size()) == bytes.size() &&
                         file.truncate(bytes.size()) && file.sync();
    const bool closed = close();
    if (!written || !closed) return io("stage write/sync");
    const auto sealed = inspect(stage.data(), bytes);
    if (sealed != RemovalPlanStorageResult::Ok) return sealed;
    if (lookup.inspect(target.data()) != CompanionFilePresence::Missing) return io("publication target changed");
    if (!Storage.rename(stage.data(), target.data())) return io("publication rename");
    const auto published = inspect(target.data(), bytes);
    if (published != RemovalPlanStorageResult::Ok) return published;
    output = actual;
    ready = true;
    return published;
  }
  // Returned path view borrows planBuffer, which must survive subsequent journal
  // and hash operations. Output fields are assigned only on success. Reusing
  // planBuffer invalidates previous borrowed views, including on read failure.
  RemovalPlanStorageResult load(const Digest& expected, std::span<uint8_t> planBuffer, SingleFileRemovalPlan& output) {
    ready = false;
    if (planBuffer.size() < SINGLE_FILE_REMOVAL_PLAN_MAX_SIZE ||
        !std::any_of(expected.begin(), expected.end(), [](uint8_t byte) { return byte != 0; }))
      return RemovalPlanStorageResult::Invalid;
    if (!close() || !prepare() || !paths(expected)) return io("load preparation");
    const auto presence = lookup.inspect(target.data());
    if (presence == CompanionFilePresence::Missing) return RemovalPlanStorageResult::Missing;
    if (presence != CompanionFilePresence::Present || !Storage.openFileForReadReusing("COMPANION", target.data(), file))
      return io("load open");
    if (file.isDirectory()) {
      close();
      return io("load directory");
    }
    const auto length = file.fileSize64();
    if (length < SINGLE_FILE_REMOVAL_PLAN_PREFIX + 6 || length > SINGLE_FILE_REMOVAL_PLAN_MAX_SIZE) {
      if (!close()) return io("load close");
      return RemovalPlanStorageResult::Corrupt;
    }
    auto bytes = planBuffer.first(static_cast<size_t>(length));
    if (file.read(bytes.data(), bytes.size()) != static_cast<int>(bytes.size())) {
      close();
      return io("load read");
    }
    if (!digest(bytes)) {
      close();
      return io("load SHA");
    }
    const bool valid = actual == expected && decodeSingleFileRemovalPlan(bytes, parsed);
    const bool synced = valid && file.sync();
    const bool closed = close();
    if (!closed || (valid && !synced)) return io("load sync/close");
    if (!valid) return RemovalPlanStorageResult::Corrupt;
    output = parsed;
    ready = true;
    return RemovalPlanStorageResult::Ok;
  }

 private:
  std::span<uint8_t> comparison;
  HalFile file;
  HalCompanionFileLookup lookup;
  mbedtls_sha256_context hashContext;
  Digest actual{};
  std::array<char, 112> target{}, stage{};
  SingleFileRemovalPlan parsed;
  bool ready = false;
  static bool disjoint(std::span<const uint8_t> a, std::span<const uint8_t> b) {
    const auto first = reinterpret_cast<uintptr_t>(a.data());
    const auto second = reinterpret_cast<uintptr_t>(b.data());
    return first <= second ? a.size() <= second - first : b.size() <= first - second;
  }
  bool prepare() { return Storage.ready() && Storage.ensureDirectoryExists(TRANSFER_DIRECTORY); }
  bool close() { return !file.isOpen() || file.close() || failure("close"); }
  bool digest(std::span<const uint8_t> bytes) {
    return mbedtls_sha256_starts(&hashContext, 0) == 0 &&
           mbedtls_sha256_update(&hashContext, bytes.data(), bytes.size()) == 0 &&
           mbedtls_sha256_finish(&hashContext, actual.data()) == 0;
  }
  bool paths(const Digest& hash) {
    static constexpr char PREFIX[] = "/.crosspoint/companion/removal-plan-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    static_assert(sizeof(PREFIX) + 64 + 4 <= 112);
    std::copy_n(PREFIX, sizeof(PREFIX) - 1, target.begin());
    size_t at = sizeof(PREFIX) - 1;
    for (const auto byte : hash) {
      target[at++] = HEX_DIGITS[byte >> 4];
      target[at++] = HEX_DIGITS[byte & 15];
    }
    target[at] = 0;
    std::copy_n(target.begin(), at, stage.begin());
    std::copy_n(".tmp", 5, stage.begin() + at);
    return true;
  }
  RemovalPlanStorageResult inspect(const char* path, std::span<const uint8_t> expected) {
    if (!close()) return io("inspection close");
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Missing) return RemovalPlanStorageResult::Missing;
    if (presence != CompanionFilePresence::Present || !Storage.openFileForReadReusing("COMPANION", path, file))
      return io("inspection open");
    if (file.isDirectory()) {
      close();
      return io("inspection directory");
    }
    bool equal = file.fileSize64() == expected.size();
    size_t offset = 0;
    while (equal && offset < expected.size()) {
      auto part = comparison.first(std::min(comparison.size(), expected.size() - offset));
      if (file.read(part.data(), part.size()) != static_cast<int>(part.size())) {
        close();
        return io("inspection read");
      }
      equal = std::equal(part.begin(), part.end(), expected.begin() + offset);
      offset += part.size();
    }
    const bool synced = equal && file.sync();
    const bool closed = close();
    if (!closed || (equal && !synced)) return io("inspection sync/close");
    return equal ? RemovalPlanStorageResult::Ok : RemovalPlanStorageResult::Corrupt;
  }
  static bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Removal plan %s failed", operation);
    return false;
  }
  static RemovalPlanStorageResult io(const char* operation) {
    failure(operation);
    return RemovalPlanStorageResult::IoError;
  }
};
}  // namespace companion
