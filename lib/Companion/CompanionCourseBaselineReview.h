#pragma once

#include "CompanionCourseBaselineImportRequest.h"

namespace companion {
inline constexpr size_t COURSE_BASELINE_REVIEW_HEADER_SIZE = 60;
inline constexpr size_t COURSE_BASELINE_REVIEW_ENTRY_SIZE = 68;
inline constexpr size_t COURSE_BASELINE_REVIEW_MAX_FILES = 64;
inline constexpr size_t COURSE_BASELINE_REVIEW_MAX_SIZE =
    64 + COURSE_BASELINE_REVIEW_MAX_FILES * COURSE_BASELINE_REVIEW_ENTRY_SIZE;
enum class CourseBaselineReviewDomain : uint8_t { Learner = 1, Journal = 2, Isolation = 3 };
struct CourseBaselineReviewFile {
  CourseBaselineReviewDomain domain = CourseBaselineReviewDomain::Learner;
  bool present = false;
  std::array<char, 24> name{};
  uint64_t length = 0;
  Digest hash{};
};
static_assert(sizeof(CourseBaselineReviewFile) < 256);
namespace course_review_detail {
inline constexpr std::array<uint8_t, 4> MAGIC = {'T', 'C', 'B', 'V'};
inline bool nonzero(std::span<const uint8_t> bytes) {
  return std::any_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte != 0; });
}
inline bool validName(std::string_view name) {
  return !name.empty() && name.size() < 24 && name != "." && name != ".." &&
         std::all_of(
             name.begin(), name.end(),
             [](unsigned char value) {
               return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '.' ||
                      value == '-' || value == '_';
             });
}
inline bool stableLearnerName(std::string_view name) {
  return validName(name) && !name.starts_with("pack-") && !name.ends_with(".tmp") && !name.ends_with(".sync") &&
         !name.ends_with(".sync-old") && !name.ends_with(".proof") && name != "sync-intent" &&
         name != "sync-intent-stage" && name != "sync-receipt-stage";
}
inline uint64_t number(std::span<const uint8_t> bytes, size_t at, unsigned count) {
  uint64_t value = 0;
  for (unsigned index = 0; index < count; ++index) value |= uint64_t(bytes[at + index]) << (index * 8);
  return value;
}
inline void number(std::span<uint8_t> bytes, size_t at, uint64_t value, unsigned count) {
  for (unsigned index = 0; index < count; ++index) bytes[at + index] = static_cast<uint8_t>(value >> (index * 8));
}
}  // namespace course_review_detail
inline bool encodeCourseBaselineReviewFile(const CourseBaselineReviewFile& file, std::span<uint8_t> output) {
  const auto end = std::find(file.name.begin(), file.name.end(), '\0');
  const std::string_view name(file.name.data(), end - file.name.begin());
  if (output.size() != COURSE_BASELINE_REVIEW_ENTRY_SIZE || !course_review_detail::validName(name) ||
      file.domain < CourseBaselineReviewDomain::Learner || file.domain > CourseBaselineReviewDomain::Isolation ||
      file.length > UINT32_MAX ||
      (file.present ? !course_review_detail::nonzero(file.hash)
                    : file.length || course_review_detail::nonzero(file.hash)) ||
      course_baseline_detail::overlaps(&file, sizeof(file), output.data(), output.size()))
    return false;
  std::fill(output.begin(), output.end(), 0);
  output[0] = static_cast<uint8_t>(file.domain);
  output[1] = file.present;
  output[2] = name.size();
  std::copy(name.begin(), name.end(), output.begin() + 4);
  course_review_detail::number(output, 28, file.length, 8);
  std::copy(file.hash.begin(), file.hash.end(), output.begin() + 36);
  return true;
}
// Borrows canonical review bytes until the owner/scratch is reused.
class CourseBaselineReviewView final {
 public:
  bool decode(std::span<const uint8_t> input) {
    bytes = {};
    if (input.size() < 64 ||
        !std::equal(course_review_detail::MAGIC.begin(), course_review_detail::MAGIC.end(), input.begin()) ||
        input[4] != 1 || input[5] > 1 || input[6] || input[7] || input[58] || input[59])
      return false;
    const auto count = course_review_detail::number(input, 56, 2);
    if (count < 8 || count > COURSE_BASELINE_REVIEW_MAX_FILES ||
        input.size() != 64 + count * COURSE_BASELINE_REVIEW_ENTRY_SIZE ||
        course_review_detail::number(input, input.size() - 4, 4) !=
            binary_record::crc32(input.data(), input.size() - 4))
      return false;
    for (size_t at = 8; at < 56; at += 16)
      if (!course_review_detail::nonzero(input.subspan(at, 16))) return false;
    unsigned learners = 0, journals = 0, isolation = 0, presentJournal = 0;
    uint8_t previousDomain = 0;
    std::string_view previousName;
    for (size_t at = 0; at < count; ++at) {
      const auto entry = input.subspan(COURSE_BASELINE_REVIEW_HEADER_SIZE + at * COURSE_BASELINE_REVIEW_ENTRY_SIZE,
                                       COURSE_BASELINE_REVIEW_ENTRY_SIZE);
      const auto nameSize = entry[2];
      if (!nameSize || nameSize >= 24 || entry[3] || entry[0] < 1 || entry[0] > 3 || entry[1] > 1) return false;
      const std::string_view name(reinterpret_cast<const char*>(entry.data() + 4), nameSize);
      if (!course_review_detail::validName(name) ||
          course_review_detail::nonzero(entry.subspan(4 + nameSize, 24 - nameSize)) || entry[0] < previousDomain ||
          (entry[0] == previousDomain && name <= previousName))
        return false;
      const auto length = course_review_detail::number(entry, 28, 8);
      const auto hash = entry.subspan(36, 32);
      if (length > UINT32_MAX ||
          (entry[1] ? !course_review_detail::nonzero(hash) : length || course_review_detail::nonzero(hash)))
        return false;
      if (entry[0] == 1) {
        if (!entry[1] || !course_review_detail::stableLearnerName(name)) return false;
        ++learners;
      } else if (entry[0] == 2) {
        static constexpr std::string_view NAMES[] = {"events.bin", "header-a.bin", "header-b.bin"};
        if (journals >= 3 || name != NAMES[journals]) return false;
        if (entry[1]) presentJournal |= 1u << journals;
        ++journals;
      } else {
        static constexpr std::string_view NAMES[] = {"mark-done", "mark-intent", "state-done", "state-intent"};
        if (isolation >= 4 || name != NAMES[isolation] || !entry[1] || length != (isolation % 2 ? 28 : 24))
          return false;
        ++isolation;
      }
      previousDomain = entry[0];
      previousName = name;
    }
    if (!learners || journals != 3 || isolation != 4 ||
        (input[5] ? !(presentJournal & 1) || !(presentJournal & 6) : presentJournal != 0))
      return false;
    bytes = input;
    return true;
  }
  size_t count() const { return bytes.empty() ? 0 : course_review_detail::number(bytes, 56, 2); }
  std::span<const uint8_t> reader() const { return bytes.empty() ? bytes : bytes.subspan(8, 16); }
  std::span<const uint8_t> generation() const { return bytes.empty() ? bytes : bytes.subspan(24, 16); }
  std::span<const uint8_t> course() const { return bytes.empty() ? bytes : bytes.subspan(40, 16); }
  std::span<const uint8_t> entry(size_t index) const {
    return index < count()
               ? bytes.subspan(60 + index * COURSE_BASELINE_REVIEW_ENTRY_SIZE, COURSE_BASELINE_REVIEW_ENTRY_SIZE)
               : std::span<const uint8_t>{};
  }

 private:
  std::span<const uint8_t> bytes;
};
}  // namespace companion
