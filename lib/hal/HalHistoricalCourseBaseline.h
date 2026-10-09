#pragma once

#include "CompanionHistoricalCourseBaseline.h"
#include "HalCourseRemovalMetadata.h"
#include "HalCourseStateIsolation.h"
#include "HalInventoryFileHash.h"
#include "HalRemovalCohortAddress.h"

namespace companion {
// Retain off stack after heap admission. Caller excludes all namespace/state
// writers and lends scratch. A loan ends on close, reopen or permission loss.
class HalHistoricalCourseBaseline final {
 public:
  using Permission = bool (*)(void*);
  HalHistoricalCourseBaseline(const Identity& generation, const Identity& course, std::span<uint8_t> scratch,
                              Permission permitted, void* context)
      : generation(generation),
        course(course),
        scratch(scratch),
        permitted(permitted),
        context(context),
        metadata(permitted, context),
        isolation(metadata, scratch, permitted, context) {
    mbedtls_sha256_init(&addressHash);
  }
  ~HalHistoricalCourseBaseline() {
    closeReaders();
    mbedtls_sha256_free(&addressHash);
  }
  HalHistoricalCourseBaseline(const HalHistoricalCourseBaseline&) = delete;
  HalHistoricalCourseBaseline& operator=(const HalHistoricalCourseBaseline&) = delete;
  bool open(const ContentRemovalRecord& expected) {
    ready = false;
    if (!closeHandles() || !guard() || generation == Identity{} || course == Identity{} ||
        scratch.size() < CONTENT_REMOVAL_RECORD_SIZE || !validContentRemovalRecord(expected) ||
        expected.phase != ContentRemovalPhase::Retired || expected.request.generation != generation ||
        expected.request.manifest.logicalIdentity != course || !validCourseBinding(expected.request.manifest) ||
        expected.request.manifest.formatVersion != 1)
      return fail("admission");
    selected = expected;
    uint64_t length = 0;
    for (const auto* path : CONTENT_REMOVAL_JOURNALS)
      if (metadata.stat(path, length) != FileStatus::Missing || !guard()) return fail("unfinished removal");
    if (!address("/.crosspoint/companion/removal-done-", selected.request.transaction, receiptPath) ||
        metadata.stat(receiptPath.data(), length) != FileStatus::Present || length != CONTENT_REMOVAL_RECORD_SIZE ||
        !metadata.read(receiptPath.data(), 0, scratch.first(CONTENT_REMOVAL_RECORD_SIZE)) || !guard() ||
        !decodeContentRemovalRecord(scratch.first(CONTENT_REMOVAL_RECORD_SIZE), receipt) || receipt != selected)
      return fail("completed receipt");
    if (!address("/.crosspoint/companion/removal-course-plan-", selected.planHash, planPath) ||
        !verifyFile(planPath.data(), COURSE_REMOVAL_PLAN_SIZE, selected.planHash) ||
        !metadata.read(planPath.data(), 0, planBytes) || !guard() || !codec.decode(planBytes, plan) ||
        !historicalCourseRemovalCachePath(receipt, plan, selected.planHash, generation, course, cached))
      return fail("sealed plan");
    if (!removalCohortAddress(addressHash, selected.planHash, 0, backup) ||
        metadata.stat(backup.data(), length) != FileStatus::Missing || !guard() || !isolation.verify(course) ||
        !verifyFile(cached.data(), receipt.request.manifest.length, receipt.request.manifest.contentHash) ||
        !closeHandles() || !guard())
      return fail("cache or isolation");
    ready = true;
    return true;
  }
  const char* path() const { return loan() ? cached.data() : nullptr; }
  const ContentManifest* manifest() const { return loan() ? &receipt.request.manifest : nullptr; }
  bool closeReaders() {
    ready = false;
    return closeHandles();
  }

 private:
  Identity generation, course;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  HalCourseRemovalMetadata metadata;
  HalCourseStateIsolation isolation;
  HalFile file;
  mbedtls_sha256_context addressHash;
  ContentRemovalRecord selected, receipt;
  CourseRemovalPlanCodec codec;
  CourseRemovalPlan plan;
  std::array<uint8_t, COURSE_REMOVAL_PLAN_SIZE> planBytes{};
  std::array<char, 112> receiptPath{}, planPath{}, backup{};
  std::array<char, COURSE_REMOVAL_CACHE_PATH_CAPACITY> cached{};
  Digest actual{};
  mutable bool ready = false;
  bool guard() const { return permitted && permitted(context); }
  bool loan() const {
    if (!guard()) ready = false;
    return ready;
  }
  bool closeHandles() {
    const bool isolationClosed = isolation.closeReaders();
    const bool metadataClosed = metadata.closeReaders();
    const bool fileClosed = !file.isOpen() || file.close();
    return isolationClosed && metadataClosed && fileClosed;
  }
  static bool address(std::string_view prefix, std::span<const uint8_t> bytes, std::span<char> output) {
    if (prefix.size() + bytes.size() * 2 + 1 > output.size()) return false;
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    std::copy(prefix.begin(), prefix.end(), output.begin());
    size_t at = prefix.size();
    for (const auto byte : bytes) {
      output[at++] = HEX_DIGITS[byte >> 4];
      output[at++] = HEX_DIGITS[byte & 15];
    }
    output[at] = 0;
    return true;
  }
  bool verifyFile(const char* path, uint64_t length, const Digest& expected) {
    uint64_t found = 0;
    if (!closeHandles() || !guard() || metadata.stat(path, found) != FileStatus::Present || found != length ||
        !Storage.openFileForReadReusing("COMPANION", path, file) || !guard() || file.isDirectory() ||
        file.fileSize64() != length)
      return false;
    const bool hashed = hashInventoryFile(file, scratch, found, actual, progress, this);
    const bool synced = hashed && file.sync();
    const bool closed = closeHandles();
    return hashed && synced && closed && guard() && found == length && actual == expected;
  }
  static bool progress(void* context) { return static_cast<HalHistoricalCourseBaseline*>(context)->guard(); }
  bool fail([[maybe_unused]] const char* reason) {
    ready = false;
    closeHandles();
    LOG_ERR("COMPANION", "Historical course baseline refused: %s", reason);
    return false;
  }
};
}  // namespace companion
