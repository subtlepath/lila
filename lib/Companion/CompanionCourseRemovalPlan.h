#pragma once

#include <string_view>

#include "CompanionContentRemovalRequest.h"
#include "CompanionCourseBinding.h"

namespace companion {
// The bound manifest proves the active pack. Learner state remains owned by its
// course identity; the participant must verify isolation before quarantine.
struct CourseRemovalPlan {
  ContentRemovalRequest request;
  bool operator==(const CourseRemovalPlan&) const = default;
};
static_assert(sizeof(CourseRemovalPlan) < 256);
inline constexpr size_t COURSE_REMOVAL_PLAN_SIZE = 8 + CONTENT_REMOVAL_REQUEST_SIZE + 4;
inline bool validCourseRemovalPlan(const CourseRemovalPlan& plan) {
  return validContentRemovalRequest(plan.request) && validCourseBinding(plan.request.manifest);
}
inline std::string_view courseRemovalPath(const CourseRemovalPlan& plan) {
  return validCourseRemovalPlan(plan) ? std::string_view(ACTIVE_COURSE_PATH) : std::string_view{};
}
class CourseRemovalPlanCodec final {
 public:
  static size_t encode(const CourseRemovalPlan& plan, std::span<uint8_t> output) {
    if (!validCourseRemovalPlan(plan) || output.size() < COURSE_REMOVAL_PLAN_SIZE) return 0;
    auto bytes = output.first(COURSE_REMOVAL_PLAN_SIZE);
    std::copy(PREFIX.begin(), PREFIX.end(), bytes.begin());
    if (encodeContentRemovalRequest(plan.request, bytes.subspan(8, CONTENT_REMOVAL_REQUEST_SIZE)) !=
        CONTENT_REMOVAL_REQUEST_SIZE)
      return 0;
    const auto checksum = courseBindingCrc(bytes.first(COURSE_REMOVAL_PLAN_SIZE - 4));
    for (unsigned at = 0; at < 4; ++at)
      bytes[COURSE_REMOVAL_PLAN_SIZE - 4 + at] = static_cast<uint8_t>(checksum >> (at * 8));
    return COURSE_REMOVAL_PLAN_SIZE;
  }
  bool decode(std::span<const uint8_t> bytes, CourseRemovalPlan& output) {
    if (bytes.size() != COURSE_REMOVAL_PLAN_SIZE || !std::equal(PREFIX.begin(), PREFIX.end(), bytes.begin()))
      return false;
    uint32_t checksum = 0;
    for (unsigned at = 0; at < 4; ++at) checksum |= uint32_t(bytes[COURSE_REMOVAL_PLAN_SIZE - 4 + at]) << (at * 8);
    if (checksum != courseBindingCrc(bytes.first(COURSE_REMOVAL_PLAN_SIZE - 4)) ||
        !decodeContentRemovalRequest(bytes.subspan(8, CONTENT_REMOVAL_REQUEST_SIZE), candidate.request) ||
        !validCourseRemovalPlan(candidate))
      return false;
    output = candidate;
    return true;
  }

 private:
  static constexpr std::array<uint8_t, 8> PREFIX = {'C', 'R', 'M', 'V', 1, 0, 0, 0};
  CourseRemovalPlan candidate;
};
}  // namespace companion
