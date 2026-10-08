#pragma once

#include <Logging.h>
#include <mbedtls/sha256.h>

#include "CompanionRemovalStageClaim.h"
#include "HalCompanionFileLookup.h"

namespace companion {
enum class RemovalStageClaimResult { Ok, Missing, Invalid, Conflict, Corrupt, IoError };
// Session-owned off stack. The caller authenticates and admits requests, excludes all
// claim/stage writers, and retains scratch separate from other IO buffers.
class HalRemovalStageClaimStorage final {
 public:
  explicit HalRemovalStageClaimStorage(std::span<uint8_t> scratch) : scratch(scratch) { mbedtls_sha256_init(&hash); }
  ~HalRemovalStageClaimStorage() {
    close();
    mbedtls_sha256_free(&hash);
  }
  HalRemovalStageClaimStorage(const HalRemovalStageClaimStorage&) = delete;
  HalRemovalStageClaimStorage& operator=(const HalRemovalStageClaimStorage&) = delete;
  const char* markerPath() const { return ready ? target.data() : nullptr; }
  const char* planStagePath() const { return ready ? planStage.data() : nullptr; }
  RemovalStageClaimResult load(const RemovalStageClaim& claim) {
    ready = false;
    if (!validRemovalStageClaim(claim) || scratch.size() < REMOVAL_STAGE_CLAIM_SIZE) return invalid();
    expected = claim;
    if (!prepare()) return io("load preparation");
    const auto result = inspect(target.data());
    if (result != RemovalStageClaimResult::Ok) return result;
    ready = true;
    return result;
  }
  RemovalStageClaimResult persist(const RemovalStageClaim& claim) {
    ready = false;
    if (!validRemovalStageClaim(claim) || scratch.size() < REMOVAL_STAGE_CLAIM_SIZE) return invalid();
    expected = claim;
    if (!prepare()) return io("persist preparation");
    auto result = inspect(target.data());
    if (result == RemovalStageClaimResult::Ok) {
      ready = true;
      return result;
    }
    if (result != RemovalStageClaimResult::Missing) return result;
    // Only bounded unpublished marker bytes under this exact claim digest may
    // be rebuilt. Valid foreign claims and oversized/directory collisions stay.
    result = inspect(temporary.data(), true);
    if (result != RemovalStageClaimResult::Ok && result != RemovalStageClaimResult::Missing &&
        result != RemovalStageClaimResult::Corrupt)
      return result;
    if (result != RemovalStageClaimResult::Ok) {
      if (encodeRemovalStageClaim(expected, scratch) != REMOVAL_STAGE_CLAIM_SIZE ||
          !Storage.openFileForWriteReusing("COMPANION", temporary.data(), file))
        return io("marker stage open");
      const bool written = !file.isDirectory() &&
                           file.write(scratch.data(), REMOVAL_STAGE_CLAIM_SIZE) == REMOVAL_STAGE_CLAIM_SIZE &&
                           file.truncate(REMOVAL_STAGE_CLAIM_SIZE) && file.sync();
      const bool closed = close();
      if (!written || !closed) return io("marker stage write/sync");
      result = inspect(temporary.data());
      if (result != RemovalStageClaimResult::Ok) return result;
    }
    if (lookup.inspect(target.data()) != CompanionFilePresence::Missing ||
        !Storage.rename(temporary.data(), target.data()))
      return io("marker publication rename");
    result = inspect(target.data());
    if (result != RemovalStageClaimResult::Ok) return result;
    ready = true;
    return result;
  }

 private:
  std::span<uint8_t> scratch;
  RemovalStageClaim expected, decoded;
  HalFile file;
  HalCompanionFileLookup lookup;
  mbedtls_sha256_context hash;
  Digest digest{};
  std::array<char, 112> target{}, temporary{}, planStage{};
  bool ready = false;
  bool close() { return !file.isOpen() || file.close() || failure("close"); }
  bool prepare() {
    if (!close() || !Storage.ready() || !Storage.ensureDirectoryExists(TRANSFER_DIRECTORY) ||
        encodeRemovalStageClaim(expected, scratch) != REMOVAL_STAGE_CLAIM_SIZE || mbedtls_sha256_starts(&hash, 0) ||
        mbedtls_sha256_update(&hash, scratch.data(), REMOVAL_STAGE_CLAIM_SIZE) ||
        mbedtls_sha256_finish(&hash, digest.data()))
      return false;
    static constexpr char PREFIX[] = "/.crosspoint/companion/removal-multi-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    static_assert(sizeof(PREFIX) + 64 + 10 <= 112);
    size_t at = sizeof(PREFIX) - 1;
    std::copy_n(PREFIX, at, target.begin());
    for (uint8_t byte : digest) {
      target[at++] = HEX_DIGITS[byte >> 4];
      target[at++] = HEX_DIGITS[byte & 15];
    }
    std::copy_n(target.begin(), at, planStage.begin());
    std::copy_n(".stage", 7, planStage.begin() + at);
    std::copy_n(".owner", 7, target.begin() + at);
    std::copy_n(target.begin(), at + 6, temporary.begin());
    std::copy_n(".tmp", 5, temporary.begin() + at + 6);
    return true;
  }
  RemovalStageClaimResult inspect(const char* path, bool allowPartial = false) {
    if (!close()) return io("inspect close");
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Missing) return RemovalStageClaimResult::Missing;
    if (presence != CompanionFilePresence::Present || !Storage.openFileForReadReusing("COMPANION", path, file))
      return io("inspect open");
    if (file.isDirectory()) {
      close();
      return io("marker directory");
    }
    const auto size = file.fileSize64();
    if (size > REMOVAL_STAGE_CLAIM_SIZE) {
      close();
      return io("marker collision");
    }
    if (size != REMOVAL_STAGE_CLAIM_SIZE) {
      if (!close()) return io("partial close");
      if (!allowPartial) return corrupt();
      return RemovalStageClaimResult::Corrupt;
    }
    const bool read = file.read(scratch.data(), REMOVAL_STAGE_CLAIM_SIZE) == static_cast<int>(REMOVAL_STAGE_CLAIM_SIZE);
    const bool decodedOk = read && decodeRemovalStageClaim(scratch.first(REMOVAL_STAGE_CLAIM_SIZE), decoded);
    const bool synced = decodedOk && file.sync();
    const bool closed = close();
    if (!read || !closed || (decodedOk && !synced)) return io("marker read/sync/close");
    if (!decodedOk) return corrupt();
    if (decoded != expected) {
      failure("foreign marker");
      return RemovalStageClaimResult::Conflict;
    }
    return RemovalStageClaimResult::Ok;
  }
  static bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Removal stage claim %s failed", operation);
    return false;
  }
  static RemovalStageClaimResult io(const char* operation) {
    failure(operation);
    return RemovalStageClaimResult::IoError;
  }
  static RemovalStageClaimResult invalid() {
    failure("invalid claim or scratch");
    return RemovalStageClaimResult::Invalid;
  }
  static RemovalStageClaimResult corrupt() {
    failure("corrupt marker");
    return RemovalStageClaimResult::Corrupt;
  }
};
}  // namespace companion
