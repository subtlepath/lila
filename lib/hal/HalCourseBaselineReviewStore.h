#pragma once

#include <mbedtls/sha256.h>

#include "CompanionCourseBaselineReview.h"
#include "HalCourseRemovalMetadata.h"

namespace companion {
enum class CourseBaselineReviewStoreResult { Ok, Missing, Invalid, Busy, Conflict, Corrupt, IoError };
// Retain off stack after admission. Caller prepares the companion parent,
// supplies a native captured review, excludes writers, and keeps input/output
// disjoint from comparison scratch. This seals review bytes, not learner state.
class HalCourseBaselineReviewStore final {
 public:
  using Permission = bool (*)(void*);
  HalCourseBaselineReviewStore(std::span<uint8_t> comparison, Permission permitted, void* context)
      : comparison(comparison), permitted(permitted), context(context), metadata(permitted, context) {
    mbedtls_sha256_init(&digest);
  }
  ~HalCourseBaselineReviewStore() {
    closeReaders();
    mbedtls_sha256_free(&digest);
  }
  HalCourseBaselineReviewStore(const HalCourseBaselineReviewStore&) = delete;
  HalCourseBaselineReviewStore& operator=(const HalCourseBaselineReviewStore&) = delete;
  CourseBaselineReviewStoreResult publish(std::span<const uint8_t> input, const Digest& expected) {
    if (operating) return CourseBaselineReviewStoreResult::Busy;
    ready = false;
    CourseBaselineReviewView view;
    if (!arguments(input, expected) || !view.decode(input)) return invalid("publish arguments");
    operating = true;
    selected = expected;
    if (!hash(input) || actual != selected || !paths()) return finish(CourseBaselineReviewStoreResult::Corrupt);
    auto result = compare(canonical.data(), input, false);
    if (result == CourseBaselineReviewStoreResult::Ok) {
      uint64_t size = 0;
      result = status(stage.data(), size);
      return finish(result == CourseBaselineReviewStoreResult::Missing ? CourseBaselineReviewStoreResult::Ok
                    : result == CourseBaselineReviewStoreResult::Ok    ? CourseBaselineReviewStoreResult::Conflict
                                                                       : result);
    }
    if (result != CourseBaselineReviewStoreResult::Missing) return finish(result);
    result = compare(stage.data(), input, true);
    if (result != CourseBaselineReviewStoreResult::Ok && result != CourseBaselineReviewStoreResult::Missing)
      return finish(result);
    const auto offset = result == CourseBaselineReviewStoreResult::Missing ? 0 : extent;
    if (offset != input.size()) {
      if (!guard()) return finish(CourseBaselineReviewStoreResult::Busy);
      if (result == CourseBaselineReviewStoreResult::Missing) {
        if (!Storage.openFileForWriteReusing("COMPANION", stage.data(), file))
          return finish(CourseBaselineReviewStoreResult::IoError);
      } else {
        // Append once to a byte-proven prefix; retain the handle through sync.
        file = Storage.open(stage.data(), O_WRONLY);
        if (!file) return finish(CourseBaselineReviewStoreResult::IoError);
      }
      const auto tail = input.subspan(static_cast<size_t>(offset));
      const bool written = guard() && !file.isDirectory() && file.fileSize64() == offset && file.seek64(offset) &&
                           file.write(tail.data(), tail.size()) == tail.size() && file.truncate(input.size()) &&
                           file.sync();
      const bool closed = file.close();
      if (!written || !closed || !guard()) return finish(CourseBaselineReviewStoreResult::IoError);
    }
    result = compare(stage.data(), input, false);
    if (result != CourseBaselineReviewStoreResult::Ok) return finish(result);
    uint64_t size = 0;
    result = status(canonical.data(), size);
    if (result != CourseBaselineReviewStoreResult::Missing)
      return finish(result == CourseBaselineReviewStoreResult::Ok ? CourseBaselineReviewStoreResult::Conflict : result);
    if (!guard()) return finish(CourseBaselineReviewStoreResult::Busy);
    if (!Storage.rename(stage.data(), canonical.data())) return finish(CourseBaselineReviewStoreResult::IoError);
    result = compare(canonical.data(), input, false);
    return finish(result);
  }
  // Buffer contents may change on failure; loans are offered only on success.
  CourseBaselineReviewStoreResult open(const Digest& expected, const Identity& reader, const Identity& generation,
                                       const Identity& course, std::span<uint8_t> output) {
    if (operating) return CourseBaselineReviewStoreResult::Busy;
    ready = false;
    if (output.size() < COURSE_BASELINE_REVIEW_MAX_SIZE || !arguments(output, expected) || reader == Identity{} ||
        generation == Identity{} || course == Identity{})
      return invalid("open arguments");
    operating = true;
    selected = expected;
    selectedReader = reader;
    selectedGeneration = generation;
    selectedCourse = course;
    if (!paths()) return finish(CourseBaselineReviewStoreResult::Invalid);
    uint64_t size = 0;
    auto result = status(stage.data(), size);
    if (result != CourseBaselineReviewStoreResult::Missing)
      return finish(result == CourseBaselineReviewStoreResult::Ok ? CourseBaselineReviewStoreResult::Busy : result);
    result = status(canonical.data(), size);
    if (result != CourseBaselineReviewStoreResult::Ok) return finish(result);
    if (size < 64 || size > COURSE_BASELINE_REVIEW_MAX_SIZE) return finish(CourseBaselineReviewStoreResult::Corrupt);
    if (!Storage.openFileForReadReusing("COMPANION", canonical.data(), file) || !guard() || file.isDirectory() ||
        file.fileSize64() != size)
      return finish(CourseBaselineReviewStoreResult::IoError);
    const auto bytes = output.first(static_cast<size_t>(size));
    const bool read =
        file.read(bytes.data(), bytes.size()) == static_cast<int>(bytes.size()) && file.fileSize64() == size;
    const bool synced = read && file.sync(), closed = file.close();
    if (!read || !synced || !closed || !guard()) return finish(CourseBaselineReviewStoreResult::IoError);
    CourseBaselineReviewView view;
    if (!hash(bytes) || actual != selected || !view.decode(bytes) ||
        !std::equal(view.reader().begin(), view.reader().end(), selectedReader.begin()) ||
        !std::equal(view.generation().begin(), view.generation().end(), selectedGeneration.begin()) ||
        !std::equal(view.course().begin(), view.course().end(), selectedCourse.begin()))
      return finish(CourseBaselineReviewStoreResult::Corrupt);
    return finish(CourseBaselineReviewStoreResult::Ok);
  }
  const char* path() const { return loan() ? canonical.data() : nullptr; }
  bool closeReaders() {
    ready = false;
    const bool fileClosed = !file.isOpen() || file.close();
    const bool metadataClosed = metadata.closeReaders();
    return fileClosed && metadataClosed;
  }

 private:
  std::span<uint8_t> comparison;
  Permission permitted;
  void* context;
  HalCourseRemovalMetadata metadata;
  HalFile file;
  mbedtls_sha256_context digest;
  std::array<char, 112> canonical{}, stage{};
  Digest selected{}, actual{};
  Identity selectedReader{}, selectedGeneration{}, selectedCourse{};
  uint64_t extent = 0;
  bool operating = false;
  mutable bool ready = false;
  bool guard() const { return permitted && permitted(context); }
  bool loan() const {
    if (!guard()) ready = false;
    return ready;
  }
  bool arguments(std::span<const uint8_t> bytes, const Digest& expected) const {
    return comparison.size() >= 64 && course_review_detail::nonzero(expected) &&
           !course_baseline_detail::overlaps(bytes.data(), bytes.size(), comparison.data(), comparison.size()) &&
           !course_baseline_detail::overlaps(bytes.data(), bytes.size(), this, sizeof(*this)) &&
           !course_baseline_detail::overlaps(comparison.data(), comparison.size(), this, sizeof(*this));
  }
  bool hash(std::span<const uint8_t> bytes) {
    return guard() && mbedtls_sha256_starts(&digest, 0) == 0 &&
           mbedtls_sha256_update(&digest, bytes.data(), bytes.size()) == 0 &&
           mbedtls_sha256_finish(&digest, actual.data()) == 0 && guard();
  }
  bool paths() {
    static constexpr std::string_view PREFIX = "/.crosspoint/companion/course-review-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    static_assert(PREFIX.size() + 64 + sizeof(".tmp") <= 112);
    std::copy(PREFIX.begin(), PREFIX.end(), canonical.begin());
    size_t at = PREFIX.size();
    for (const auto byte : selected) {
      canonical[at++] = HEX_DIGITS[byte >> 4];
      canonical[at++] = HEX_DIGITS[byte & 15];
    }
    canonical[at] = 0;
    std::copy_n(canonical.begin(), at, stage.begin());
    static constexpr char SUFFIX[] = ".tmp";
    std::copy_n(SUFFIX, sizeof(SUFFIX), stage.begin() + at);
    return true;
  }
  CourseBaselineReviewStoreResult status(const char* path, uint64_t& size) {
    if (!guard()) return CourseBaselineReviewStoreResult::Busy;
    const auto status = metadata.stat(path, size);
    if (!guard()) return CourseBaselineReviewStoreResult::Busy;
    return status == FileStatus::Missing   ? CourseBaselineReviewStoreResult::Missing
           : status == FileStatus::Present ? CourseBaselineReviewStoreResult::Ok
                                           : CourseBaselineReviewStoreResult::IoError;
  }
  CourseBaselineReviewStoreResult compare(const char* path, std::span<const uint8_t> expected, bool prefix) {
    uint64_t size = 0;
    const auto result = status(path, size);
    if (result != CourseBaselineReviewStoreResult::Ok) return result;
    if (size > expected.size() || (!prefix && size != expected.size())) return CourseBaselineReviewStoreResult::Corrupt;
    if (!Storage.openFileForReadReusing("COMPANION", path, file) || !guard() || file.isDirectory() ||
        file.fileSize64() != size)
      return CourseBaselineReviewStoreResult::IoError;
    for (size_t at = 0; at < size;) {
      const auto count = std::min<uint64_t>(comparison.size(), size - at);
      if (!guard() || file.read(comparison.data(), count) != static_cast<int>(count))
        return CourseBaselineReviewStoreResult::IoError;
      if (!std::equal(comparison.begin(), comparison.begin() + count, expected.begin() + at))
        return CourseBaselineReviewStoreResult::Conflict;
      at += count;
      vTaskDelay(1);
    }
    const bool synced = file.fileSize64() == size && file.sync(), closed = file.close();
    if (!synced || !closed || !guard()) return CourseBaselineReviewStoreResult::IoError;
    extent = size;
    return CourseBaselineReviewStoreResult::Ok;
  }
  CourseBaselineReviewStoreResult finish(CourseBaselineReviewStoreResult result) {
    if (!closeReaders()) result = CourseBaselineReviewStoreResult::IoError;
    if (!guard()) result = CourseBaselineReviewStoreResult::Busy;
    operating = false;
    ready = result == CourseBaselineReviewStoreResult::Ok;
    if (!ready && result != CourseBaselineReviewStoreResult::Missing)
      LOG_ERR("COMPANION", "Course review storage refused: %u", static_cast<unsigned>(result));
    return result;
  }
  static CourseBaselineReviewStoreResult invalid(const char* operation) {
    LOG_ERR("COMPANION", "Invalid course review storage %s", operation);
    return CourseBaselineReviewStoreResult::Invalid;
  }
};
}  // namespace companion
