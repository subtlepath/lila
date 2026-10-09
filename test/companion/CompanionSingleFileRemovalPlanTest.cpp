#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/Companion/CompanionSingleFileRemovalPlan.h"
using namespace companion;
namespace {
ContentRemovalRequest request(ContentKind kind = ContentKind::Epub, uint32_t format = 1) {
  ContentRemovalRequest result;
  result.transaction.fill(1);
  result.owner.fill(2);
  result.generation.fill(3);
  result.manifest.contentHash.fill(4);
  result.manifest.kind = kind;
  result.manifest.length = 1234;
  result.manifest.formatVersion = format;
  return result;
}
}  // namespace
TEST(SingleFileRemovalPlan, RoundtripPreservesExactNativePathsAndInventoryFormat) {
  for (const auto path : {"/book.epub", "/Books/Renamed.EPUB", "/Books/caf\xc3\xa9.epub"}) {
    const SingleFileRemovalPlan input{request(), path};
    std::array<uint8_t, SINGLE_FILE_REMOVAL_PLAN_MAX_SIZE + 2> bytes{};
    bytes.front() = 0xAA;
    bytes.back() = 0xBB;
    const auto length = encodeSingleFileRemovalPlan(input, std::span(bytes).subspan(1, bytes.size() - 2));
    ASSERT_GT(length, 0);
    SingleFileRemovalPlan output;
    ASSERT_TRUE(decodeSingleFileRemovalPlan(std::span(bytes).subspan(1, length), output));
    EXPECT_EQ(output.request, input.request);
    EXPECT_EQ(output.path, path);
    EXPECT_EQ(output.request.manifest.formatVersion, 1);
    EXPECT_EQ(bytes.front(), 0xAA);
    EXPECT_EQ(bytes.back(), 0xBB);
    for (size_t size = 0; size < length; ++size) {
      EXPECT_FALSE(decodeSingleFileRemovalPlan(std::span(bytes).subspan(1, size), output));
      EXPECT_EQ(output.path, path);
      EXPECT_EQ(output.request, input.request);
    }
  }
}
TEST(SingleFileRemovalPlan, IntegrityAndSemanticChecksPreserveCallerOutput) {
  const SingleFileRemovalPlan input{request(), "/Books/book.epub"};
  std::vector<uint8_t> bytes(SINGLE_FILE_REMOVAL_PLAN_PREFIX + input.path.size() + 4);
  ASSERT_EQ(encodeSingleFileRemovalPlan(input, bytes), bytes.size());
  SingleFileRemovalPlan output = input;
  for (size_t at = 0; at < bytes.size(); ++at) {
    auto invalid = bytes;
    invalid[at] ^= 1;
    EXPECT_FALSE(decodeSingleFileRemovalPlan(invalid, output));
    EXPECT_EQ(output.request, input.request);
    EXPECT_EQ(output.path, input.path);
  }
  for (const size_t at : std::array<size_t, 3>{5, 123, SINGLE_FILE_REMOVAL_PLAN_PREFIX}) {
    auto invalid = bytes;
    invalid[at] = 9;
    inventory_detail::write(invalid, invalid.size() - 4,
                            inventoryIndexCrc(std::span(invalid).first(invalid.size() - 4)), 4);
    EXPECT_FALSE(decodeSingleFileRemovalPlan(invalid, output));
  }
}
TEST(SingleFileRemovalPlan, RejectsPrivatePathsTraversalAndUnsupportedContentKinds) {
  for (const auto path :
       {"/.crosspoint/book.epub", "/.CROSSPOINT/companion/book.epub", "book.epub", "/Books/../book.epub",
        "/Books//book.epub", "/Books/book.epub/", "/Books/book.dict", "/Books/book\\.epub"})
    EXPECT_FALSE(validSingleFileRemovalPlan({request(), path}));
  auto course = request(ContentKind::Course);
  course.manifest.logicalIdentity.fill(5);
  EXPECT_FALSE(validSingleFileRemovalPlan({course, "/tinta/course.pack"}));
  EXPECT_FALSE(validSingleFileRemovalPlan({request(ContentKind::Dictionary), "/dictionaries/es/dictionary.ifo"}));
  EXPECT_TRUE(validSingleFileRemovalPlan({request(ContentKind::Font, 4), "/fonts/Font_14.cpfont"}));
  EXPECT_TRUE(validSingleFileRemovalPlan({request(ContentKind::Font, 1), "/.fonts/Font.ttf"}));
  EXPECT_FALSE(validSingleFileRemovalPlan({request(ContentKind::Font, 4), "/Books/Font_14.cpfont"}));
  EXPECT_FALSE(validSingleFileRemovalPlan({request(ContentKind::Font, 1), "/fonts/Font_14.cpfont"}));
  EXPECT_FALSE(validSingleFileRemovalPlan({request(ContentKind::Font, 4), "/fonts/Font.ttf"}));
  EXPECT_TRUE(validSingleFileRemovalPlan({request(ContentKind::Epub, 0), "/Books/book.epub"}));
  EXPECT_FALSE(validSingleFileRemovalPlan({request(ContentKind::Epub, 2), "/Books/book.epub"}));
}
TEST(SingleFileRemovalPlan, EnforcesPathLimitWithoutModifyingShortOutput) {
  std::string path = "/" + std::string(INVENTORY_PATH_LIMIT - 6, 'a') + ".epub";
  ASSERT_EQ(path.size(), INVENTORY_PATH_LIMIT);
  const SingleFileRemovalPlan plan{request(), path};
  std::array<uint8_t, SINGLE_FILE_REMOVAL_PLAN_MAX_SIZE> bytes;
  bytes.fill(0xAA);
  EXPECT_EQ(encodeSingleFileRemovalPlan(plan, std::span(bytes).first(bytes.size() - 1)), 0);
  EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte == 0xAA; }));
  ASSERT_EQ(encodeSingleFileRemovalPlan(plan, bytes), bytes.size());
  SingleFileRemovalPlan decoded;
  EXPECT_TRUE(decodeSingleFileRemovalPlan(bytes, decoded));
  path.insert(1, "a");
  EXPECT_FALSE(validSingleFileRemovalPlan({request(), path}));
}

TEST(SingleFileRemovalPlan, RejectsMalformedUtf8WithoutNormalizingNativeNames) {
  for (const auto path :
       {"/Books/\xc0\xaf"
        "book.epub",
        "/Books/\xed\xa0\x80.epub", "/Books/\xf4\x90\x80\x80.epub", "/Books/\x80.epub", "/Books/\xe2.epub"})
    EXPECT_FALSE(validSingleFileRemovalPlan({request(), path}));
  const SingleFileRemovalPlan decomposed{request(), "/Books/cafe\xcc\x81.epub"};
  std::array<uint8_t, SINGLE_FILE_REMOVAL_PLAN_MAX_SIZE> bytes{};
  const auto size = encodeSingleFileRemovalPlan(decomposed, bytes);
  ASSERT_GT(size, 0);
  SingleFileRemovalPlan decoded;
  ASSERT_TRUE(decodeSingleFileRemovalPlan(std::span(bytes).first(size), decoded));
  EXPECT_EQ(decoded.path, decomposed.path);
}

TEST(SingleFileRemovalPlan, FontExtensionAndManifestFormatMustAgreeAcrossEncodingAndDecoding) {
  std::array<uint8_t, SINGLE_FILE_REMOVAL_PLAN_MAX_SIZE> bytes{};
  for (const auto path : {"/fonts/Font.cpfont", "/.fonts/Font.TTF", "/fonts/Font.otf", "/.fonts/Font.ttc"}) {
    for (const auto format : {0U, 1U, 2U, 3U, 4U, 5U}) {
      const bool bitmap = std::string_view(path).ends_with(".cpfont");
      const bool accepted = bitmap ? format == 4 : format == 1;
      const SingleFileRemovalPlan plan{request(ContentKind::Font, format), path};
      EXPECT_EQ(validSingleFileRemovalPlan(plan), accepted);
      const auto length = encodeSingleFileRemovalPlan(plan, bytes);
      EXPECT_EQ(length != 0, accepted);
      if (accepted) {
        SingleFileRemovalPlan decoded;
        ASSERT_TRUE(decodeSingleFileRemovalPlan(std::span(bytes).first(length), decoded));
        EXPECT_EQ(decoded.request, plan.request);
        EXPECT_EQ(decoded.path, plan.path);
      }
    }
  }
}
