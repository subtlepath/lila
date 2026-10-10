#pragma once

#include "CompanionUnboundCourseMigrationRequest.h"
#include "HalCompanionHeapAdmission.h"
#include "HalCourseBaselineReviewStore.h"
#include "HalInventoryFileHash.h"

namespace companion {
enum class UnboundReviewedFileResult { Present, Missing, Invalid, Unavailable };
// Read-only loan of one immutable reviewed learner copy, not migration authority.
// Admit off stack; caller excludes writers, verifies the complete backup cohort
// separately and passes the same permission callback to learner validators.
class HalUnboundCourseReviewedFile final {
 public:
  using Permission = bool (*)(void*);
  HalUnboundCourseReviewedFile(const Identity& reader, const Identity& generation, std::span<uint8_t> scratch,
                               Permission permitted, void* context)
      : reader(reader),
        generation(generation),
        scratch(scratch),
        permitted(permitted),
        context(context),
        review(scratch.size() > COURSE_BASELINE_REVIEW_MAX_SIZE ? scratch.subspan(COURSE_BASELINE_REVIEW_MAX_SIZE)
                                                                : std::span<uint8_t>{},
               allowed, this),
        metadata(allowed, this) {}
  ~HalUnboundCourseReviewedFile() { closeReaders(); }
  UnboundReviewedFileResult open(const UnboundCourseMigrationRequest& request, std::string_view name) {
    if (operating) return UnboundReviewedFileResult::Unavailable;
    ready = false;
    if (reader == Identity{} || generation == Identity{} || request.original.generation != generation ||
        !validCourseBaselineImportRequest(request.original) || !learnerName(name) ||
        scratch.size() < COURSE_BASELINE_REVIEW_MAX_SIZE + 512 ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &request, sizeof(request)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), name.data(), name.size())) {
      closeReaders();
      return invalid();
    }
    selected = request;
    operating = true;
    if (!closeReaders() || !guard() ||
        review.open(selected.original.reviewHash, reader, generation, selected.original.manifest.logicalIdentity,
                    scratch.first(COURSE_BASELINE_REVIEW_MAX_SIZE)) != CourseBaselineReviewStoreResult::Ok)
      return finish(UnboundReviewedFileResult::Unavailable);
    CourseBaselineReviewView view;
    const auto count = course_review_detail::number(scratch, 56, 2);
    if (count > COURSE_BASELINE_REVIEW_MAX_FILES ||
        !view.decode(scratch.first(64 + count * COURSE_BASELINE_REVIEW_ENTRY_SIZE), true) || view.isolated())
      return finish(UnboundReviewedFileResult::Invalid);
    size_t index = view.count();
    for (size_t i = 0; i < view.count(); ++i) {
      const auto entry = view.entry(i);
      if (entry[0] == uint8_t(CourseBaselineReviewDomain::Learner) &&
          std::string_view(reinterpret_cast<const char*>(entry.data() + 4), entry[2]) == name) {
        index = i;
        std::copy(entry.begin(), entry.end(), record.begin());
        break;
      }
    }
    if (!review.closeReaders() || !guard()) return finish(UnboundReviewedFileResult::Unavailable);
    if (index == count) return finish(UnboundReviewedFileResult::Missing);
    paths(index);
    const auto expectedLength = course_review_detail::number(record, 28, 8);
    uint64_t size = 0, length = 0;
    Digest actual{};
    if (!guard() || metadata.stat(stage.data(), size) != FileStatus::Missing ||
        metadata.stat(path.data(), size) != FileStatus::Present || size != expectedLength || !guard() ||
        !Storage.openFileForReadReusing("COMPANION", path.data(), file))
      return finish(UnboundReviewedFileResult::Unavailable);
    if (file.isDirectory() || !hashInventoryFile(file, scratch, length, actual, allowed, this) ||
        length != expectedLength || !std::equal(actual.begin(), actual.end(), record.begin() + 36) || !file.sync() ||
        !file.seek64(0) || file.fileSize64() != expectedLength || !guard() ||
        metadata.stat(stage.data(), size) != FileStatus::Missing || !metadata.closeReaders())
      return finish(UnboundReviewedFileResult::Unavailable);
    return finish(UnboundReviewedFileResult::Present);
  }
  HalFile* borrowed() {
    if (!guard() || !file.isOpen()) ready = false;
    return ready ? &file : nullptr;
  }
  bool closeReaders() {
    ready = false;
    const bool fileClosed = !file.isOpen() || file.close();
    const bool reviewClosed = review.closeReaders(), metadataClosed = metadata.closeReaders();
    return fileClosed && reviewClosed && metadataClosed;
  }

 private:
  Identity reader, generation;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  HalCourseBaselineReviewStore review;
  HalCourseRemovalMetadata metadata;
  HalFile file;
  UnboundCourseMigrationRequest selected;
  std::array<uint8_t, COURSE_BASELINE_REVIEW_ENTRY_SIZE> record{};
  std::array<char, 128> path{}, stage{};
  bool operating = false, ready = false;
  bool guard() const { return permitted && permitted(context) && admitCompanionHeap(); }
  static bool allowed(void* raw) { return static_cast<HalUnboundCourseReviewedFile*>(raw)->guard(); }
  static bool learnerName(std::string_view name) {
    static constexpr std::string_view NAMES[] = {"items.bin",   "reviews.log", "profile.bin", "days.bin",
                                                 "session.bin", "starred.bin", "read.bin"};
    return std::find(std::begin(NAMES), std::end(NAMES), name) != std::end(NAMES);
  }
  void paths(size_t index) {
    static constexpr std::string_view PREFIX = "/.crosspoint/companion/course-review-state-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    static_assert(PREFIX.size() + 64 + 3 + sizeof(".tmp") <= 128);
    std::copy(PREFIX.begin(), PREFIX.end(), path.begin());
    size_t at = PREFIX.size();
    for (const auto byte : selected.original.reviewHash) {
      path[at++] = HEX_DIGITS[byte >> 4];
      path[at++] = HEX_DIGITS[byte & 15];
    }
    path[at++] = '-';
    path[at++] = HEX_DIGITS[(index >> 4) & 15];
    path[at++] = HEX_DIGITS[index & 15];
    path[at] = 0;
    std::copy_n(path.begin(), at, stage.begin());
    std::copy_n(".tmp", 5, stage.begin() + at);
  }
  static UnboundReviewedFileResult invalid() {
    LOG_ERR("COMPANION", "Unbound reviewed file arguments refused");
    return UnboundReviewedFileResult::Invalid;
  }
  UnboundReviewedFileResult finish(UnboundReviewedFileResult result) {
    if (!guard()) result = UnboundReviewedFileResult::Unavailable;
    if (result != UnboundReviewedFileResult::Present) {
      if (!closeReaders()) result = UnboundReviewedFileResult::Unavailable;
      if (result != UnboundReviewedFileResult::Missing) LOG_ERR("COMPANION", "Unbound reviewed file refused");
    }
    ready = result == UnboundReviewedFileResult::Present;
    operating = false;
    return result;
  }
};
}  // namespace companion
