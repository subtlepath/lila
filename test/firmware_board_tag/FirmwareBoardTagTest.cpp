#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "FirmwareBoardTag.h"

namespace {
void feed(board_tag::Scanner& scanner, std::string_view bytes) {
  scanner.feed(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}
std::string matchingTag() {
  return "CROSSPOINT-BOARD-V1:" + std::string(board_tag::boardName(), board_tag::boardNameLen()) + ";";
}
}  // namespace

TEST(FirmwareBoardTagTest, RequiresCompleteMatchingTagAcrossEveryChunkBoundary) {
  const auto tag = matchingTag();
  for (size_t split = 0; split <= tag.size(); ++split) {
    board_tag::Scanner scanner;
    feed(scanner, "unrelated image data");
    feed(scanner, std::string_view(tag).substr(0, split));
    EXPECT_EQ(scanner.compatible(), split == tag.size()) << split;
    feed(scanner, std::string_view(tag).substr(split));
    feed(scanner, "image suffix");
    EXPECT_TRUE(scanner.compatible()) << split;
    EXPECT_FALSE(scanner.mismatch());
  }
}

TEST(FirmwareBoardTagTest, MissingOrPartialTagCannotProveBoard) {
  board_tag::Scanner scanner;
  feed(scanner, "untagged image");
  EXPECT_FALSE(scanner.compatible());
  feed(scanner, "CROSSPOINT-BOARD-V1:");
  EXPECT_FALSE(scanner.compatible());
  feed(scanner, std::string_view(board_tag::boardName(), board_tag::boardNameLen()));
  EXPECT_FALSE(scanner.compatible());
  feed(scanner, ";");
  EXPECT_TRUE(scanner.compatible());
}

TEST(FirmwareBoardTagTest, WrongBoardCannotBeHiddenByLaterMatchingTag) {
  board_tag::Scanner scanner;
  feed(scanner, "CROSSPOINT-BOARD-V1:unknown-board;");
  EXPECT_TRUE(scanner.mismatch());
  EXPECT_STREQ(scanner.foundName(), "unknown-board");
  feed(scanner, matchingTag());
  EXPECT_FALSE(scanner.compatible());
}

TEST(FirmwareBoardTagTest, MatchingTagCannotHideLaterWrongOrIncompleteTag) {
  for (const auto suffix : {"CROSSPOINT-BOARD-V1:other;", "CROSSPOINT-BOARD-V1:", "CROSSPOINT-BOARD-V1:other"}) {
    board_tag::Scanner scanner;
    feed(scanner, matchingTag());
    ASSERT_TRUE(scanner.compatible());
    feed(scanner, suffix);
    EXPECT_FALSE(scanner.compatible()) << suffix;
  }
}

TEST(FirmwareBoardTagTest, MalformedTagCannotBeHiddenByMatchingTag) {
  for (const auto& name :
       {std::string(24, 'x'), std::string("bad name"), std::string("bad\0name", 8), std::string("bad\xFFname", 8)}) {
    for (const bool validFirst : {false, true}) {
      board_tag::Scanner scanner;
      if (validFirst) feed(scanner, matchingTag());
      feed(scanner, "CROSSPOINT-BOARD-V1:" + name + ";");
      if (!validFirst) feed(scanner, matchingTag());
      EXPECT_FALSE(scanner.compatible()) << validFirst;
    }
  }
}

TEST(FirmwareBoardTagTest, RepeatedMatchingTagsAndBytewiseFeedsRemainCompatible) {
  board_tag::Scanner scanner;
  const auto bytes = matchingTag() + matchingTag();
  for (const char& byte : bytes) feed(scanner, {&byte, 1});
  EXPECT_TRUE(scanner.compatible());
  EXPECT_FALSE(scanner.mismatch());
}
