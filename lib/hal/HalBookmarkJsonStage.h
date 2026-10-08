#pragma once

#include "CompanionBookmarkJsonWriter.h"
#include "HalVerifiedFileStage.h"

namespace companion {
// Checked off-stack owner. Candidate is private until a publication coordinator
// records ownership; source spool must reach verified End before finish.
class HalBookmarkJsonStage final : private BookmarkJsonSink {
 public:
  static constexpr char PATH[] = "/.crosspoint/companion/bookmark-json-next";
  static constexpr uint64_t MAX_BYTES_PER_BOOKMARK = 4096;
  explicit HalBookmarkJsonStage(std::span<uint8_t> scratch) : stage(scratch), writer(*this) {}
  ~HalBookmarkJsonStage() override { cleanup(); }
  HalBookmarkJsonStage(const HalBookmarkJsonStage&) = delete;
  HalBookmarkJsonStage& operator=(const HalBookmarkJsonStage&) = delete;
  bool begin(uint32_t expectedBookmarkRecords) {
    if (used) return failure("JSON stage reused");
    used = true;
    expected = expectedBookmarkRecords;
    if (!stage.begin(PATH, static_cast<uint64_t>(expectedBookmarkRecords) * MAX_BYTES_PER_BOOKMARK + 32))
      return failure("JSON stage begin");
    failed = false;
    writing = true;
    return writer.begin() ? true : failure("JSON writer begin");
  }
  bool append(std::span<const uint8_t> body) {
    if (failed || !writing || records >= expected || !writer.append(body)) return failure("JSON append");
    ++records;
    return true;
  }
  bool finish() {
    if (failed || !writing || records != expected || !writer.finish() || !stage.seal(offset))
      return failure("JSON stage seal");
    writing = false;
    sealedOwned = true;
    return true;
  }
  bool isSealed() const { return !failed && sealedOwned && stage.isSealed(); }
  uint64_t bytesWritten() const { return offset; }
  const Digest& contentHash() const { return stage.contentHash(); }
  bool cleanup() {
    failed = true;
    writing = false;
    if (!stage.cleanup()) return failure("JSON writer cleanup");
    if (sealedOwned) {
      if (!Storage.ready() || !Storage.remove(PATH)) return failure("JSON candidate cleanup");
      sealedOwned = false;
    }
    return true;
  }

 private:
  bool write(std::span<const uint8_t> bytes) override {
    if (failed || !writing || !stage.write(offset, bytes)) return failure("JSON write");
    offset += bytes.size();
    return true;
  }
  bool failure(const char* operation) {
    failed = true;
    LOG_ERR("COMPANION", "Bookmark JSON staging failed: %s", operation);
    return false;
  }
  HalVerifiedFileStage stage;
  BookmarkJsonWriter writer;
  uint64_t offset = 0;
  uint32_t expected = 0, records = 0;
  bool used = false, failed = true, writing = false, sealedOwned = false;
};
}  // namespace companion
