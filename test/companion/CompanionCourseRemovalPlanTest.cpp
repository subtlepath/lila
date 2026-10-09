#include <gtest/gtest.h>

#include "lib/Companion/CompanionCourseRemovalPlan.h"

using namespace companion;
namespace {
CourseRemovalPlan plan() {
  CourseRemovalPlan value;
  value.request.transaction.fill(1);
  value.request.owner.fill(2);
  value.request.generation.fill(3);
  value.request.manifest.contentHash.fill(4);
  value.request.manifest.logicalIdentity.fill(5);
  value.request.manifest.kind = ContentKind::Course;
  value.request.manifest.length = 1000;
  value.request.manifest.formatVersion = 1;
  return value;
}
}  // namespace
TEST(CourseRemovalPlan, RetainsExactRequestAndOnlyTargetsActivePack) {
  const auto input = plan();
  std::array<uint8_t, COURSE_REMOVAL_PLAN_SIZE + 1> bytes;
  bytes.fill(0xa5);
  ASSERT_EQ(CourseRemovalPlanCodec::encode(input, bytes), COURSE_REMOVAL_PLAN_SIZE);
  EXPECT_EQ(bytes.back(), 0xa5);
  CourseRemovalPlan output;
  CourseRemovalPlanCodec codec;
  ASSERT_TRUE(codec.decode(std::span(bytes).first(COURSE_REMOVAL_PLAN_SIZE), output));
  EXPECT_EQ(output, input);
  EXPECT_EQ(courseRemovalPath(output), "/tinta/course.pack");
  EXPECT_FALSE(codec.decode(bytes, output));
  EXPECT_FALSE(codec.decode(std::span(bytes).first(COURSE_REMOVAL_PLAN_SIZE - 1), output));
  EXPECT_EQ(output, input);
}
TEST(CourseRemovalPlan, CorruptionCannotRetargetRequestOrChangeOutput) {
  const auto input = plan();
  std::array<uint8_t, COURSE_REMOVAL_PLAN_SIZE> bytes;
  ASSERT_EQ(CourseRemovalPlanCodec::encode(input, bytes), bytes.size());
  CourseRemovalPlanCodec codec;
  auto output = input;
  output.request.transaction.fill(9);
  const auto unchanged = output;
  for (size_t at = 0; at < bytes.size(); ++at) {
    auto corrupted = bytes;
    corrupted[at] ^= 1;
    EXPECT_FALSE(codec.decode(corrupted, output));
    EXPECT_EQ(output, unchanged);
  }
}
TEST(CourseRemovalPlan, RefusesUnboundCoursesForeignKindsAndUnsupportedFormats) {
  for (unsigned fault = 0; fault < 7; ++fault) {
    auto input = plan();
    switch (fault) {
      case 0:
        input.request.manifest.logicalIdentity = {};
        break;
      case 1:
        input.request.manifest.kind = ContentKind::Epub;
        break;
      case 2:
        input.request.manifest.formatVersion = 2;
        break;
      case 3:
        input.request.manifest.length = 0;
        break;
      case 4:
        input.request.transaction = {};
        break;
      case 5:
        input.request.owner = {};
        break;
      case 6:
        input.request.generation = {};
        break;
    }
    std::array<uint8_t, COURSE_REMOVAL_PLAN_SIZE> bytes;
    bytes.fill(0xa5);
    const auto unchanged = bytes;
    EXPECT_FALSE(validCourseRemovalPlan(input));
    EXPECT_TRUE(courseRemovalPath(input).empty());
    EXPECT_EQ(CourseRemovalPlanCodec::encode(input, bytes), 0U);
    EXPECT_EQ(bytes, unchanged);
  }
}
