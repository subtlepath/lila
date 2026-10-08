#include <gtest/gtest.h>

#include <array>
#include <fstream>

#include "lib/Companion/CompanionFirmwareReaderInfo.h"
TEST(CompanionFirmwareReaderInfo, SharedFixtureAndInvalidFieldsPreserveOutput) {
  std::array<uint8_t, companion::FIRMWARE_READER_INFO_SIZE> bytes{}, encoded{};
  std::ifstream input(FIRMWARE_INFO_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()));
  ASSERT_EQ(input.peek(), std::char_traits<char>::eof());
  companion::FirmwareReaderInfo info;
  ASSERT_TRUE(companion::decodeFirmwareReaderInfo(bytes, info));
  EXPECT_EQ(info.partitionBytes, 0x900000);
  EXPECT_EQ(info.chip, 5);
  EXPECT_EQ(info.stateSchema, 1);
  EXPECT_EQ(info.journalVersions, 3);
  EXPECT_EQ(info.battery, 60);
  ASSERT_TRUE(companion::encodeFirmwareReaderInfo(info, encoded));
  EXPECT_EQ(encoded, bytes);
  for (size_t offset : {size_t{3}, size_t{4}, size_t{5}, size_t{58}, size_t{59}, size_t{72}}) {
    auto invalid = bytes;
    invalid[offset] = 0xff;
    auto output = info;
    EXPECT_FALSE(companion::decodeFirmwareReaderInfo(invalid, output));
    EXPECT_EQ(output, info);
  }
  auto invalid = info;
  invalid.generation.fill(0);
  EXPECT_FALSE(companion::encodeFirmwareReaderInfo(invalid, encoded));
  EXPECT_EQ(encoded, bytes);
}
