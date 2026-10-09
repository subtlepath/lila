#pragma once

#include "CompanionBookmarkIdentityCursor.h"
#include "HalVerifiedFileStage.h"

namespace companion {
// Retain off-stack. One serialized recovery owner controls this private spool.
// Borrowed scratch outlives the owner; discard staged results unless reading ends.
class HalBookmarkIdentityStage final {
 public:
  static constexpr char PATH[] = "/.crosspoint/companion/bookmark-ids-next";
  explicit HalBookmarkIdentityStage(std::span<uint8_t> scratch, InventoryHashProgress guard = nullptr,
                                    void* context = nullptr)
      : stage(scratch, guard, context, TRANSFER_DIRECTORY, guard, context), guard(guard), context(context) {
    mbedtls_sha256_init(&readHash);
  }
  ~HalBookmarkIdentityStage() {
    cleanup();
    mbedtls_sha256_free(&readHash);
  }
  HalBookmarkIdentityStage(const HalBookmarkIdentityStage&) = delete;
  HalBookmarkIdentityStage& operator=(const HalBookmarkIdentityStage&) = delete;
  bool begin(uint32_t maximumIds) {
    if (!cleanup()) return false;
    maximum = maximumIds;
    count = readCount = 0;
    previous = {};
    failed = true;
    if (!stage.begin(PATH, static_cast<uint64_t>(maximumIds) * Identity{}.size())) return false;
    failed = false;
    writing = true;
    return true;
  }
  bool append(const Identity& identity) {
    if (failed || !writing || count >= maximum || !tinta_body_detail::nonzero(identity) ||
        (count && !(previous < identity)))
      return failure("ID bounds/order");
    if (!stage.write(static_cast<uint64_t>(count) * identity.size(), identity)) return failure("ID write");
    previous = identity;
    ++count;
    return true;
  }
  bool seal() {
    if (failed || !writing || !stage.seal(static_cast<uint64_t>(count) * Identity{}.size())) return failure("ID seal");
    writing = false;
    sealedOwned = true;
    return true;
  }
  BookmarkCursorResult next(Identity& output) {
    if (guard && !guard(context)) return readFailure("read authority");
    if (failed || !sealedOwned || writing) return BookmarkCursorResult::Error;
    if (ended) return BookmarkCursorResult::End;
    if (!reading) {
      if (!Storage.openFileForRead("COMPANION", PATH, reader) || reader.isDirectory() ||
          reader.fileSize64() != static_cast<uint64_t>(count) * Identity{}.size() ||
          mbedtls_sha256_starts(&readHash, 0) != 0)
        return readFailure("ID read open");
      reading = true;
      previous = {};
    }
    if (readCount == count) {
      Digest actual{};
      const bool hashValid = mbedtls_sha256_finish(&readHash, actual.data()) == 0 && actual == stage.contentHash();
      const bool closed = reader.close();
      if (!hashValid || !closed) return readFailure("ID read hash/close");
      reading = false;
      ended = true;
      return BookmarkCursorResult::End;
    }
    Identity candidate{};
    if (reader.read(candidate.data(), candidate.size()) != candidate.size() || !tinta_body_detail::nonzero(candidate) ||
        (readCount && !(previous < candidate)) ||
        mbedtls_sha256_update(&readHash, candidate.data(), candidate.size()) != 0)
      return readFailure("ID read/order");
    previous = candidate;
    ++readCount;
    output = candidate;
    return BookmarkCursorResult::Found;
  }
  uint32_t size() const { return count; }
  bool cleanup() {
    failed = true;
    writing = reading = ended = false;
    if (reader.isOpen() && !reader.close()) return failure("ID cleanup close");
    if (!stage.cleanup()) return failure("ID writer cleanup");
    if (sealedOwned) {
      if (guard && !guard(context)) return failure("cleanup authority");
      if (!Storage.ready() || !Storage.remove(PATH)) return failure("ID cleanup remove");
      sealedOwned = false;
    }
    return true;
  }

 private:
  bool failure(const char* operation) {
    failed = true;
    LOG_ERR("COMPANION", "Bookmark ID staging failed: %s", operation);
    return false;
  }
  BookmarkCursorResult readFailure(const char* operation) {
    failure(operation);
    return BookmarkCursorResult::Error;
  }
  HalVerifiedFileStage stage;
  InventoryHashProgress guard;
  void* context;
  HalFile reader;
  mbedtls_sha256_context readHash;
  Identity previous{};
  uint32_t maximum = 0, count = 0, readCount = 0;
  bool failed = true, writing = false, reading = false, ended = false, sealedOwned = false;
};
}  // namespace companion
