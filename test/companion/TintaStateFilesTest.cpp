#include <gtest/gtest.h>

#include <array>

#include "platform/StateFiles.h"

namespace tinta::platform {
void log(const char*, ...) {}
}  // namespace tinta::platform

TEST(TintaStateFiles, SwitchingRootsClosesCachedFilesAndIsolatesProgress) {
  inventory_hal_test::state = {};
  tinta::platform::StateFiles files;
  files.begin();
  const uint8_t legacy = 7, first = 11, second = 13;
  ASSERT_TRUE(files.write("items.bin", 0, &legacy, 1));
  std::array<uint8_t, 16> course{};
  course[0] = 1;
  ASSERT_TRUE(files.beginCourse(course));
  EXPECT_EQ(files.size("items.bin"), -1);
  ASSERT_TRUE(files.write("items.bin", 0, &first, 1));
  course[0] = 2;
  ASSERT_TRUE(files.beginCourse(course));
  EXPECT_EQ(files.size("items.bin"), -1);
  ASSERT_TRUE(files.write("items.bin", 0, &second, 1));
  course[0] = 1;
  ASSERT_TRUE(files.beginCourse(course));
  uint8_t value = 0;
  ASSERT_EQ(files.read("items.bin", 0, &value, 1), 1);
  EXPECT_EQ(value, first);
  EXPECT_FALSE(files.write("../items.bin", 0, &second, 1));
  course.fill(0);
  EXPECT_FALSE(files.beginCourse(course));
  EXPECT_FALSE(files.available());
  EXPECT_FALSE(files.write("items.bin", 0, &second, 1));
  files.begin();
  ASSERT_EQ(files.read("items.bin", 0, &value, 1), 1);
  EXPECT_EQ(value, legacy);
}
TEST(TintaStateFiles, FailedSyncNeverRemovesPreviousState) {
  inventory_hal_test::state = {};
  tinta::platform::StateFiles files;
  files.begin();
  const uint8_t previous = 7, next = 9;
  ASSERT_TRUE(files.write("profile.bin", 0, &previous, 1));
  auto& hal = inventory_hal_test::state;
  hal.failSync = true;
  EXPECT_FALSE(files.replace("profile.bin", &next, 1));
  EXPECT_EQ(hal.files.at("/tinta/profile.bin"), std::vector<uint8_t>({previous}));
  EXPECT_FALSE(files.write("items.bin", 0, &next, 1));
  EXPECT_FALSE(files.append("reviews.log", &next, 1));
}
TEST(TintaStateFiles, FailedTemporaryRecoveryDisablesWritesUntilReopened) {
  inventory_hal_test::state = {};
  tinta::platform::StateFiles files;
  std::array<uint8_t, 16> course{};
  course[0] = 1;
  ASSERT_TRUE(files.beginCourse(course));
  auto& hal = inventory_hal_test::state;
  const char* temporary = "/tinta/courses/01000000000000000000000000000000/profile.bin.tmp";
  const char* destination = "/tinta/courses/01000000000000000000000000000000/profile.bin";
  hal.files[temporary] = {17};
  hal.failRename = hal.renames + 1;
  EXPECT_EQ(files.size("profile.bin"), -1);
  EXPECT_TRUE(files.failed());
  EXPECT_FALSE(files.available());
  const uint8_t value = 23;
  EXPECT_FALSE(files.write("profile.bin", 0, &value, 1));
  EXPECT_FALSE(hal.files.contains(destination));
  EXPECT_EQ(hal.files.at(temporary), std::vector<uint8_t>({17}));
  hal.failRename = 0;
  ASSERT_TRUE(files.beginCourse(course));
  uint8_t restored = 0;
  ASSERT_EQ(files.read("profile.bin", 0, &restored, 1), 1);
  EXPECT_EQ(restored, 17);
  EXPECT_FALSE(hal.files.contains(temporary));
}
TEST(TintaStateFiles, CourseStartupRecoversAllLearnerFilesBeforeOpening) {
  auto& hal = inventory_hal_test::state;
  static constexpr const char* NAMES[] = {"items.bin",   "reviews.log", "profile.bin", "days.bin",
                                          "session.bin", "starred.bin", "read.bin"};
  for (const auto* name : NAMES) {
    SCOPED_TRACE(name);
    hal = {};
    const std::string path = std::string("/tinta/courses/01000000000000000000000000000000/") + name;
    hal.files[path + ".tmp"] = {17};
    hal.failRename = 1;
    tinta::platform::StateFiles files;
    std::array<uint8_t, 16> course{};
    course[0] = 1;
    EXPECT_FALSE(files.beginCourse(course));
    EXPECT_TRUE(files.failed());
    EXPECT_FALSE(files.available());
    EXPECT_FALSE(hal.files.contains(path));
    EXPECT_EQ(hal.files.at(path + ".tmp"), std::vector<uint8_t>({17}));
    const uint8_t value = 23;
    EXPECT_FALSE(files.write(name, 0, &value, 1));
    hal.failRename = 0;
    ASSERT_TRUE(files.beginCourse(course));
    EXPECT_EQ(hal.files.at(path), std::vector<uint8_t>({17}));
    EXPECT_FALSE(hal.files.contains(path + ".tmp"));
  }
}
TEST(TintaStateFiles, FailedTemporaryRemovalCannotReportSuccessfulDeletion) {
  inventory_hal_test::state = {};
  tinta::platform::StateFiles files;
  files.begin();
  auto& hal = inventory_hal_test::state;
  hal.files["/tinta/session.bin.tmp"] = {17};
  hal.failRemove = true;
  EXPECT_FALSE(files.remove("session.bin"));
  EXPECT_EQ(hal.files.at("/tinta/session.bin.tmp"), std::vector<uint8_t>({17}));
  hal.failRemove = false;
  ASSERT_TRUE(files.remove("session.bin"));
  EXPECT_FALSE(hal.files.contains("/tinta/session.bin.tmp"));
  EXPECT_EQ(files.size("session.bin"), -1);
  EXPECT_TRUE(files.available());
}
TEST(TintaStateFiles, DirectoryFailureDoesNotWriteThroughPreviousCourseCache) {
  inventory_hal_test::state = {};
  tinta::platform::StateFiles files;
  std::array<uint8_t, 16> course{};
  course[0] = 1;
  ASSERT_TRUE(files.beginCourse(course));
  const uint8_t previous = 7, next = 9;
  ASSERT_TRUE(files.write("items.bin", 0, &previous, 1));
  auto& hal = inventory_hal_test::state;
  course[0] = 2;
  hal.failDirectory = true;
  EXPECT_FALSE(files.beginCourse(course));
  EXPECT_FALSE(files.available());
  EXPECT_FALSE(files.write("items.bin", 0, &next, 1));
  hal.failDirectory = false;
  ASSERT_TRUE(files.beginCourse(course));
  EXPECT_EQ(files.size("items.bin"), -1);
  ASSERT_TRUE(files.write("items.bin", 0, &next, 1));
  course[0] = 1;
  ASSERT_TRUE(files.beginCourse(course));
  uint8_t value = 0;
  ASSERT_EQ(files.read("items.bin", 0, &value, 1), 1);
  EXPECT_EQ(value, previous);
}
TEST(TintaStateFiles, InterruptedReplaceRecoversOnlyInsideItsCourse) {
  inventory_hal_test::state = {};
  tinta::platform::StateFiles files;
  std::array<uint8_t, 16> course{};
  course[0] = 1;
  ASSERT_TRUE(files.beginCourse(course));
  const uint8_t previous = 7, next = 9;
  ASSERT_TRUE(files.write("profile.bin", 0, &previous, 1));
  auto& hal = inventory_hal_test::state;
  hal.failRename = hal.renames + 1;
  ASSERT_FALSE(files.replace("profile.bin", &next, 1));
  EXPECT_FALSE(hal.files.contains("/tinta/courses/01000000000000000000000000000000/profile.bin"));
  EXPECT_EQ(hal.files.at("/tinta/courses/01000000000000000000000000000000/profile.bin.tmp"),
            std::vector<uint8_t>({next}));
  hal.failRename = 0;
  course[0] = 2;
  ASSERT_TRUE(files.beginCourse(course));
  EXPECT_EQ(files.size("profile.bin"), -1);
  ASSERT_TRUE(files.write("profile.bin", 0, &previous, 1));
  course[0] = 1;
  ASSERT_TRUE(files.beginCourse(course));
  uint8_t value = 0;
  ASSERT_EQ(files.read("profile.bin", 0, &value, 1), 1);
  EXPECT_EQ(value, next);
  course[0] = 2;
  ASSERT_TRUE(files.beginCourse(course));
  ASSERT_EQ(files.read("profile.bin", 0, &value, 1), 1);
  EXPECT_EQ(value, previous);
}
TEST(TintaStateFiles, CourseRootSupportsEveryLearnerFileAndLongestTemporaryName) {
  inventory_hal_test::state = {};
  tinta::platform::StateFiles files;
  std::array<uint8_t, 16> course{};
  course.fill(255);
  ASSERT_TRUE(files.beginCourse(course));
  static constexpr const char* NAMES[] = {"items.bin",   "reviews.log", "profile.bin",    "days.bin",
                                          "session.bin", "usage.seq",   "usage-0001.log", "12345678901234567890123"};
  const uint8_t value = 17;
  for (const auto* name : NAMES) {
    SCOPED_TRACE(name);
    ASSERT_TRUE(files.replace(name, &value, 1));
    uint8_t restored = 0;
    ASSERT_EQ(files.read(name, 0, &restored, 1), 1);
    EXPECT_EQ(restored, value);
    ASSERT_TRUE(files.remove(name));
    EXPECT_EQ(files.size(name), -1);
    EXPECT_TRUE(files.available());
  }
}
