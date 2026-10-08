#include <gtest/gtest.h>

#include <fstream>
#include <iterator>

#include "HalCompanionFontInstallation.h"

using namespace companion;

TEST(HalCompanionFontInstallation, VectorSupportFollowsBoardCapabilityAndRejectsInvalidDestinations) {
  for (unsigned mode = 0; mode < 4; ++mode) {
    inventory_hal_test::state = {};
    std::ifstream input(VECTOR_FONT_FIXTURE, std::ios::binary);
    ASSERT_TRUE(input.good());
    auto& bytes = inventory_hal_test::state.files["/incoming-font"];
    bytes = {std::istreambuf_iterator<char>(input), {}};
    ASSERT_FALSE(bytes.empty());
    if (mode == 1) bytes[0] ^= 1;
    ContentManifest manifest;
    manifest.kind = ContentKind::Font;
    manifest.formatVersion = 1;
    manifest.length = bytes.size();
    manifest.contentHash.fill(1);
    if (mode == 3) manifest.logicalIdentity.fill(2);
    const char* destination = mode == 2 ? "/Books/Fixture.ttf" : "/fonts/Fixture.ttf";
    std::array<uint8_t, 128> scratch{};
    const bool accepted = validateCompanionFontInstallation(destination, "/incoming-font", manifest, scratch, true);
#if CROSSPOINT_VECTOR_FONTS
    EXPECT_EQ(accepted, mode == 0);
#else
    EXPECT_FALSE(accepted);
#endif
  }
}
