#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <array>

#include "lib/hal/HalContentReadSource.h"
using namespace companion;
class HalContentReadSourceTest : public testing::Test {
 protected:
  std::array<uint8_t, 127> scratch{};
  ContentReadRequest request;
  HalFile file;
  void SetUp() override {
    inventory_hal_test::state = {};
    auto& bytes = inventory_hal_test::state.files["/reader-file"];
    bytes.resize(2003);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = i % 251;
    file = HalFile("/reader-file");
    request.generation.fill(1);
    request.manifest.length = bytes.size();
    request.manifest.formatVersion = 1;
    request.maximumBytes = MAX_CONTENT_READ_BYTES;
    ASSERT_EQ(
        EVP_Digest(bytes.data(), bytes.size(), request.manifest.contentHash.data(), nullptr, EVP_sha256(), nullptr), 1);
  }
};
TEST_F(HalContentReadSourceTest, HashOnceAndReadRepeatedAndLastChunksWithoutOpeningOrClosingHandle) {
  HalContentReadSource source;
  ASSERT_EQ(source.attach(request, file, scratch), ContentReadResult::Ok);
  const auto hashReads = inventory_hal_test::state.reads;
  std::array<uint8_t, MAX_CONTENT_READ_BYTES> output{};
  ASSERT_EQ(source.read(request, output), ContentReadResult::Ok);
  EXPECT_TRUE(std::equal(output.begin(), output.end(), inventory_hal_test::state.files["/reader-file"].begin()));
  ASSERT_EQ(source.read(request, output), ContentReadResult::Ok);
  EXPECT_EQ(inventory_hal_test::state.reads, hashReads + 2);
  request.offset = 1922;
  ASSERT_EQ(source.read(request, std::span(output).first(81)), ContentReadResult::Ok);
  EXPECT_EQ(output[0], uint8_t(1922 % 251));
  EXPECT_EQ(output[80], uint8_t(2002 % 251));
  EXPECT_EQ(inventory_hal_test::state.opens, 0U);
  EXPECT_EQ(inventory_hal_test::state.closes, 0U);
  source.detach();
  EXPECT_TRUE(file);
  EXPECT_FALSE(source.matches(request));
}
TEST_F(HalContentReadSourceTest, ChangedSizeTimestampOrClosedFileInvalidatesAttachment) {
  for (unsigned fault = 0; fault < 3; ++fault) {
    SetUp();
    HalContentReadSource source;
    ASSERT_EQ(source.attach(request, file, scratch), ContentReadResult::Ok);
    if (fault == 0) inventory_hal_test::state.files["/reader-file"].push_back(1);
    if (fault == 1) inventory_hal_test::state.modificationTimes["/reader-file"] = 1;
    if (fault == 2) ASSERT_TRUE(file.close());
    std::array<uint8_t, MAX_CONTENT_READ_BYTES> output{};
    EXPECT_EQ(source.read(request, output), fault == 2 ? ContentReadResult::NotFound : ContentReadResult::Corrupt);
    EXPECT_FALSE(source.matches(request));
  }
}
TEST_F(HalContentReadSourceTest, ForeignManifestCardAndInvalidBoundsRefuseRead) {
  for (unsigned fault = 0; fault < 4; ++fault) {
    HalContentReadSource source;
    ASSERT_EQ(source.attach(request, file, scratch), ContentReadResult::Ok);
    auto foreign = request;
    if (fault == 0) foreign.generation[0] ^= 1;
    if (fault == 1) foreign.manifest.contentHash[0] ^= 1;
    if (fault == 2) foreign.offset = foreign.manifest.length;
    std::array<uint8_t, MAX_CONTENT_READ_BYTES> output{};
    EXPECT_EQ(source.read(foreign, fault == 3 ? std::span(output).first(960) : std::span(output)),
              fault == 0 ? ContentReadResult::WrongStorage : ContentReadResult::Invalid);
    EXPECT_FALSE(source.matches(request));
  }
}
TEST_F(HalContentReadSourceTest, CorruptHashShortReadAndFailedReadDoNotAttachOrKeepSource) {
  auto& state = inventory_hal_test::state;
  HalContentReadSource source;
  state.files["/reader-file"][100] ^= 1;
  EXPECT_EQ(source.attach(request, file, scratch), ContentReadResult::Corrupt);
  EXPECT_FALSE(source.matches(request));
  state.files["/reader-file"][100] ^= 1;
  state.failRead = state.reads + 1;
  EXPECT_EQ(source.attach(request, file, scratch), ContentReadResult::IoError);
  EXPECT_FALSE(source.matches(request));
  state.failRead = 0;
  ASSERT_EQ(source.attach(request, file, scratch), ContentReadResult::Ok);
  state.shortRead = state.reads + 1;
  std::array<uint8_t, MAX_CONTENT_READ_BYTES> output{};
  EXPECT_EQ(source.read(request, output), ContentReadResult::IoError);
  EXPECT_FALSE(source.matches(request));
}
TEST_F(HalContentReadSourceTest, CancellationDuringHashOrAfterChunkPreventsOkAndRetryReattaches) {
  unsigned remaining = 1;
  auto progress = [](void* context) {
    auto& remaining = *static_cast<unsigned*>(context);
    return remaining && --remaining;
  };
  HalContentReadSource source(progress, &remaining);
  EXPECT_EQ(source.attach(request, file, scratch), ContentReadResult::Busy);
  EXPECT_FALSE(source.matches(request));
  remaining = 100;
  ASSERT_EQ(source.attach(request, file, scratch), ContentReadResult::Ok);
  remaining = 2;
  std::array<uint8_t, MAX_CONTENT_READ_BYTES> output{};
  EXPECT_EQ(source.read(request, output), ContentReadResult::Busy);
  EXPECT_FALSE(source.matches(request));
  EXPECT_TRUE(file);
  remaining = 100;
  ASSERT_EQ(source.attach(request, file, scratch), ContentReadResult::Ok);
  EXPECT_EQ(source.read(request, output), ContentReadResult::Ok);
}
