#pragma once

#include "CompanionCourseBinding.h"

namespace companion {
inline constexpr size_t COURSE_CONTEXT_REQUEST_SIZE = 20;
inline constexpr size_t COURSE_CONTEXT_HEADER_SIZE = 22;
inline constexpr size_t COURSE_CONTEXT_REPLY_SIZE = COURSE_CONTEXT_HEADER_SIZE + CONTENT_MANIFEST_SIZE;
enum class CourseContextResult : uint8_t {
  Ok,
  Missing,
  WrongStorage,
  Busy,
  Unsupported,
  IoError,
  Unauthorized,
  Corrupt
};
enum class CourseContextSource : uint8_t { None, Live, Removed };
struct CourseContextReply {
  CourseContextResult result = CourseContextResult::Missing;
  CourseContextSource source = CourseContextSource::None;
  Identity generation{};
  ContentManifest manifest{};
  bool operator==(const CourseContextReply&) const = default;
};
static_assert(sizeof(CourseContextReply) < 256);
namespace course_context_detail {
inline bool nonzero(std::span<const uint8_t> bytes) {
  return std::any_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte != 0; });
}
inline bool valid(const CourseContextReply& reply) {
  if (reply.result > CourseContextResult::Corrupt || !nonzero(reply.generation)) return false;
  if (reply.result != CourseContextResult::Ok) return reply.source == CourseContextSource::None;
  return (reply.source == CourseContextSource::Live || reply.source == CourseContextSource::Removed) &&
         validCourseBinding(reply.manifest) && reply.manifest.formatVersion == 1 && nonzero(reply.manifest.contentHash);
}
}  // namespace course_context_detail
inline size_t encodeCourseContextRequest(const Identity& generation, std::span<uint8_t> output) {
  if (output.size() < COURSE_CONTEXT_REQUEST_SIZE || !course_context_detail::nonzero(generation)) return 0;
  static constexpr uint8_t PREFIX[] = {'L', 'C', 'Q', 1};
  std::copy_n(PREFIX, sizeof(PREFIX), output.begin());
  std::copy(generation.begin(), generation.end(), output.begin() + 4);
  return COURSE_CONTEXT_REQUEST_SIZE;
}
inline bool decodeCourseContextRequest(std::span<const uint8_t> bytes, Identity& output) {
  static constexpr uint8_t PREFIX[] = {'L', 'C', 'Q', 1};
  if (bytes.size() != COURSE_CONTEXT_REQUEST_SIZE || !std::equal(std::begin(PREFIX), std::end(PREFIX), bytes.begin()))
    return false;
  Identity generation;
  std::copy_n(bytes.begin() + 4, generation.size(), generation.begin());
  if (!course_context_detail::nonzero(generation)) return false;
  output = generation;
  return true;
}
inline size_t encodeCourseContextReply(const CourseContextReply& reply, std::span<uint8_t> output) {
  const size_t size = reply.result == CourseContextResult::Ok ? COURSE_CONTEXT_REPLY_SIZE : COURSE_CONTEXT_HEADER_SIZE;
  if (!course_context_detail::valid(reply) || output.size() < size) return 0;
  static constexpr uint8_t PREFIX[] = {'L', 'C', 'X', 1};
  std::copy_n(PREFIX, sizeof(PREFIX), output.begin());
  output[4] = static_cast<uint8_t>(reply.result);
  output[5] = static_cast<uint8_t>(reply.source);
  std::copy(reply.generation.begin(), reply.generation.end(), output.begin() + 6);
  if (reply.result == CourseContextResult::Ok)
    return encodeRecord(reply.manifest, output.subspan(COURSE_CONTEXT_HEADER_SIZE)) == CONTENT_MANIFEST_SIZE ? size : 0;
  return size;
}
inline bool decodeCourseContextReply(std::span<const uint8_t> bytes, const Identity& expected,
                                     CourseContextReply& output) {
  static constexpr uint8_t PREFIX[] = {'L', 'C', 'X', 1};
  if (bytes.size() < COURSE_CONTEXT_HEADER_SIZE || !course_context_detail::nonzero(expected) ||
      !std::equal(std::begin(PREFIX), std::end(PREFIX), bytes.begin()))
    return false;
  CourseContextReply parsed;
  parsed.result = static_cast<CourseContextResult>(bytes[4]);
  parsed.source = static_cast<CourseContextSource>(bytes[5]);
  const size_t size = parsed.result == CourseContextResult::Ok ? COURSE_CONTEXT_REPLY_SIZE : COURSE_CONTEXT_HEADER_SIZE;
  if (bytes.size() != size) return false;
  std::copy_n(bytes.begin() + 6, parsed.generation.size(), parsed.generation.begin());
  if ((parsed.result == CourseContextResult::WrongStorage) == (parsed.generation == expected)) return false;
  if (parsed.result == CourseContextResult::Ok &&
      !decodeRecord(bytes.subspan(COURSE_CONTEXT_HEADER_SIZE), parsed.manifest))
    return false;
  if (!course_context_detail::valid(parsed)) return false;
  output = parsed;
  return true;
}
}  // namespace companion
