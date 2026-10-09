#pragma once

#include "CompanionBookmarkIdentityCursor.h"
#include "HalVerifiedFileStage.h"

namespace companion {
// Checked off-stack owner. Consumers stage output until verified End; scratch
// and exclusive ownership of the private spool outlive the complete read.
class HalBookmarkBodyStage final {
 public:
  static constexpr char PATH[] = "/.crosspoint/companion/bookmark-bodies-next";
  static constexpr size_t RECORD_SIZE = MAX_BOOKMARK_BODY_SIZE + 2;
  explicit HalBookmarkBodyStage(std::span<uint8_t> scratch, InventoryHashProgress guard = nullptr,
                                void* context = nullptr)
      : stage(scratch, guard, context, TRANSFER_DIRECTORY, guard, context), guard(guard), context(context) {
    mbedtls_sha256_init(&readHash);
  }
  ~HalBookmarkBodyStage() {
    cleanup();
    mbedtls_sha256_free(&readHash);
  }
  HalBookmarkBodyStage(const HalBookmarkBodyStage&) = delete;
  HalBookmarkBodyStage& operator=(const HalBookmarkBodyStage&) = delete;
  bool begin(uint32_t maximumBodies) {
    if (!cleanup()) return false;
    maximum = maximumBodies;
    count = readCount = 0;
    previous = {};
    if (!stage.begin(PATH, static_cast<uint64_t>(maximumBodies) * RECORD_SIZE)) return false;
    failed = false;
    writing = true;
    return true;
  }
  bool append(std::span<const uint8_t> body) {
    BookmarkBodyView bookmark;
    if (failed || !writing || count >= maximum || !decodeBookmarkBody(body, bookmark) ||
        (count && !(previous < bookmark.identity)))
      return failure("body bounds/order");
    bytes.fill(0);
    bytes[0] = static_cast<uint8_t>(body.size());
    bytes[1] = static_cast<uint8_t>(body.size() >> 8);
    std::copy(body.begin(), body.end(), bytes.begin() + 2);
    if (!stage.write(static_cast<uint64_t>(count) * RECORD_SIZE, bytes)) return failure("body write");
    previous = bookmark.identity;
    ++count;
    return true;
  }
  bool seal() {
    if (failed || !writing || !stage.seal(static_cast<uint64_t>(count) * RECORD_SIZE)) return failure("body seal");
    writing = false;
    sealedOwned = true;
    return true;
  }
  BookmarkCursorResult next(std::span<const uint8_t>& output) {
    if (guard && !guard(context)) return readFailure("read authority");
    if (failed || !sealedOwned || writing) return BookmarkCursorResult::Error;
    if (ended) return BookmarkCursorResult::End;
    if (!reading) {
      if (!Storage.openFileForRead("COMPANION", PATH, reader) || reader.isDirectory() ||
          reader.fileSize64() != static_cast<uint64_t>(count) * RECORD_SIZE || mbedtls_sha256_starts(&readHash, 0) != 0)
        return readFailure("body read open");
      reading = true;
      previous = {};
    }
    if (readCount == count) {
      Digest actual{};
      const bool hashValid = mbedtls_sha256_finish(&readHash, actual.data()) == 0 && actual == stage.contentHash();
      const bool closed = reader.close();
      if (!hashValid || !closed) return readFailure("body read hash/close");
      reading = false;
      ended = true;
      return BookmarkCursorResult::End;
    }
    if (reader.read(bytes.data(), bytes.size()) != static_cast<int>(bytes.size())) return readFailure("body read");
    const size_t length = bytes[0] | static_cast<size_t>(bytes[1]) << 8;
    BookmarkBodyView bookmark;
    if (length > MAX_BOOKMARK_BODY_SIZE || !decodeBookmarkBody(std::span(bytes).subspan(2, length), bookmark) ||
        (readCount && !(previous < bookmark.identity)) ||
        std::any_of(bytes.begin() + 2 + length, bytes.end(), [](uint8_t byte) { return byte != 0; }) ||
        mbedtls_sha256_update(&readHash, bytes.data(), bytes.size()) != 0)
      return readFailure("body read validation");
    previous = bookmark.identity;
    ++readCount;
    output = std::span(bytes).subspan(2, length);
    return BookmarkCursorResult::Found;
  }
  uint32_t size() const { return count; }
  bool cleanup() {
    failed = true;
    writing = reading = ended = false;
    if (reader.isOpen() && !reader.close()) return failure("body cleanup close");
    if (!stage.cleanup()) return failure("body writer cleanup");
    if (sealedOwned) {
      if (guard && !guard(context)) return failure("cleanup authority");
      if (!Storage.ready() || !Storage.remove(PATH)) return failure("body cleanup remove");
      sealedOwned = false;
    }
    return true;
  }

 private:
  bool failure(const char* operation) {
    failed = true;
    LOG_ERR("COMPANION", "Bookmark body staging failed: %s", operation);
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
  std::array<uint8_t, RECORD_SIZE> bytes{};
  Identity previous{};
  uint32_t maximum = 0, count = 0, readCount = 0;
  bool failed = true, writing = false, reading = false, ended = false, sealedOwned = false;
};
}  // namespace companion
