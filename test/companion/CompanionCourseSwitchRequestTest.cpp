#include <gtest/gtest.h>

#include <array>
#include <fstream>

#include "lib/Companion/CompanionCourseSwitchRequest.h"

TEST(CompanionCourseSwitchRequest, SharedFixtureAndRejectedConsentPreserveOutputs) {
  std::array<uint8_t, companion::COURSE_SWITCH_REQUEST_SIZE> bytes{}, encoded{};
  std::ifstream input(COURSE_SWITCH_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()));
  ASSERT_EQ(input.peek(), std::char_traits<char>::eof());
  companion::CourseSwitchRequest request;
  ASSERT_TRUE(companion::decodeCourseSwitchRequest(bytes, request));
  EXPECT_EQ(request.generation[0], 1);
  EXPECT_EQ(request.transaction[0], 2);
  EXPECT_EQ(request.previousCourse[0], 3);
  EXPECT_EQ(request.nextCourse[0], 4);
  EXPECT_EQ(request.previousHash[0], 5);
  EXPECT_EQ(request.nextHash[0], 6);
  ASSERT_TRUE(companion::encodeCourseSwitchRequest(request, encoded));
  EXPECT_EQ(encoded, bytes);
  for (const auto& range : {std::pair<size_t, size_t>{4, 16}, {20, 16}, {36, 16}, {52, 16}, {68, 32}, {100, 32}}) {
    auto invalid = bytes;
    std::fill_n(invalid.begin() + range.first, range.second, 0);
    auto unchanged = request;
    EXPECT_FALSE(companion::decodeCourseSwitchRequest(invalid, unchanged));
    EXPECT_EQ(unchanged, request);
  }
  auto invalid = request;
  invalid.nextCourse = invalid.previousCourse;
  EXPECT_FALSE(companion::encodeCourseSwitchRequest(invalid, encoded));
  EXPECT_EQ(encoded, bytes);
  invalid = request;
  invalid.nextHash = invalid.previousHash;
  EXPECT_FALSE(companion::encodeCourseSwitchRequest(invalid, encoded));
  EXPECT_EQ(encoded, bytes);
  EXPECT_FALSE(companion::decodeCourseSwitchRequest(std::span(bytes).first(bytes.size() - 1), invalid));
  bytes[3] = 2;
  EXPECT_FALSE(companion::decodeCourseSwitchRequest(bytes, invalid));
}

TEST(CompanionCourseSwitchRequest, ConsentCannotBeRetargeted) {
  companion::CourseSwitchRequest request;
  request.generation.fill(1);
  request.transaction.fill(2);
  request.previousCourse.fill(3);
  request.nextCourse.fill(4);
  request.previousHash.fill(5);
  request.nextHash.fill(6);
  companion::ContentManifest current;
  current.kind = companion::ContentKind::Course;
  current.length = 100;
  current.formatVersion = 1;
  current.logicalIdentity = request.previousCourse;
  current.contentHash = request.previousHash;
  companion::TransferDeclaration next;
  next.manifest = current;
  next.manifest.logicalIdentity = request.nextCourse;
  next.manifest.contentHash = request.nextHash;
  next.state.transaction = request.transaction;
  next.state.storageGeneration = request.generation;
  next.state.contentHash = request.nextHash;
  next.state.length = next.manifest.length;
  EXPECT_TRUE(companion::matchesCourseSwitchRequest(request, request.generation, current, next));
  auto generation = request.generation;
  generation[0] ^= 1;
  EXPECT_FALSE(companion::matchesCourseSwitchRequest(request, generation, current, next));
  for (unsigned field = 0; field < 10; ++field) {
    auto alteredCurrent = current;
    auto alteredNext = next;
    switch (field) {
      case 0:
        alteredCurrent.logicalIdentity[0] ^= 1;
        break;
      case 1:
        alteredCurrent.contentHash[0] ^= 1;
        break;
      case 2:
        alteredCurrent.kind = companion::ContentKind::Epub;
        break;
      case 3:
        alteredCurrent.length = 0;
        break;
      case 4:
        alteredNext.manifest.logicalIdentity[0] ^= 1;
        break;
      case 5:
        alteredNext.manifest.contentHash[0] ^= 1;
        break;
      case 6:
        alteredNext.state.storageGeneration[0] ^= 1;
        break;
      case 7:
        alteredNext.state.transaction[0] ^= 1;
        break;
      case 8:
        alteredNext.state.length++;
        break;
      case 9:
        alteredNext.state.durableOffset = 1;
        break;
    }
    EXPECT_FALSE(companion::matchesCourseSwitchRequest(request, request.generation, alteredCurrent, alteredNext));
  }
}
