#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>

#include "lib/Companion/CompanionFirmwareImageExtent.h"
namespace {
struct Image {
  std::array<uint8_t, 160> bytes{};
  unsigned reads = 0;
  bool fail = false;
  Image() {
    bytes[0] = 0xe9;
    bytes[1] = 2;
    bytes[28] = 16;
    bytes[52] = 20;
  }
  static bool read(void* context, uint64_t offset, std::span<uint8_t> out) {
    auto& image = *static_cast<Image*>(context);
    ++image.reads;
    if (image.fail || offset > image.bytes.size() || out.size() > image.bytes.size() - offset) return false;
    std::memcpy(out.data(), image.bytes.data() + offset, out.size());
    return true;
  }
};
}  // namespace
TEST(CompanionFirmwareImageExtent, IncludesChecksumPaddingAndOptionalShaTrailer) {
  Image image;
  uint64_t extent = 0;
  ASSERT_TRUE(companion::firmwareImageExtent(Image::read, &image, image.bytes.size(), extent));
  EXPECT_EQ(extent, 80);
  EXPECT_EQ(image.reads, 3);
  image.bytes[23] = 1;
  ASSERT_TRUE(companion::firmwareImageExtent(Image::read, &image, image.bytes.size(), extent));
  EXPECT_EQ(extent, 112);
  EXPECT_TRUE(companion::firmwareImageExtent(Image::read, &image, 112, extent));
}
TEST(CompanionFirmwareImageExtent, RejectedBoundsAndHeadersPreserveOutput) {
  for (unsigned variant = 0; variant < 8; ++variant) {
    Image image;
    uint64_t capacity = image.bytes.size();
    switch (variant) {
      case 0:
        image.bytes[0] = 0;
        break;
      case 1:
        image.bytes[1] = 0;
        break;
      case 2:
        image.bytes[1] = 17;
        break;
      case 3:
        image.bytes[23] = 2;
        break;
      case 4:
        capacity = 23;
        break;
      case 5:
        capacity = 31;
        break;
      case 6:
        std::fill_n(image.bytes.begin() + 28, 4, 0xff);
        break;
      case 7:
        image.fail = true;
        break;
    }
    uint64_t extent = 987;
    EXPECT_FALSE(companion::firmwareImageExtent(Image::read, &image, capacity, extent));
    EXPECT_EQ(extent, 987);
  }
  Image image;
  image.bytes[23] = 1;
  uint64_t extent = 987;
  EXPECT_FALSE(companion::firmwareImageExtent(Image::read, &image, 111, extent));
  EXPECT_EQ(extent, 987);
  EXPECT_FALSE(companion::firmwareImageExtent(nullptr, &image, 160, extent));
  EXPECT_EQ(extent, 987);
}
