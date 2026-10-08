#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lib/Companion/CompanionUnicodeNfc.h"
using namespace companion;
TEST(UnicodeNfcTest, Unicode151NormalizationConformanceRows) {
  std::ifstream file(UNICODE_NFC_FIXTURE);
  ASSERT_TRUE(file);
  std::string line;
  unsigned rows = 0;
  std::array<uint32_t, 256> workspace{};
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#' || line[0] == '@') continue;
    std::array<std::vector<uint32_t>, 5> columns;
    std::istringstream fields(line);
    for (auto& column : columns) {
      std::string field;
      ASSERT_TRUE(static_cast<bool>(std::getline(fields, field, ';')));
      std::istringstream scalars(field);
      uint32_t scalar;
      column.reserve(32);
      while (scalars >> std::hex >> scalar) column.push_back(scalar);
    }
    for (unsigned i = 0; i < 5; ++i) {
      size_t count = 99;
      ASSERT_TRUE(UnicodeNfc::normalize(columns[i], workspace, count)) << rows;
      const auto& expected = columns[i < 3 ? 1 : 3];
      ASSERT_EQ(count, expected.size()) << rows << ":" << i;
      ASSERT_TRUE(std::equal(expected.begin(), expected.end(), workspace.begin())) << rows << ":" << i;
    }
    ++rows;
  }
  EXPECT_GT(rows, 19000u);
}
TEST(UnicodeNfcTest, CapacityInvalidScalarsAndEmptyInput) {
  std::array<uint32_t, 4> workspace{};
  size_t count = 99;
  const std::array<uint32_t, 1> hangul{0xac01};
  EXPECT_FALSE(UnicodeNfc::normalize(hangul, std::span(workspace).first(2), count));
  EXPECT_EQ(count, 99u);
  ASSERT_TRUE(UnicodeNfc::normalize(hangul, workspace, count));
  EXPECT_EQ(count, 1u);
  EXPECT_EQ(workspace[0], 0xac01u);
  for (uint32_t invalid : {0xd800u, 0xdfffu, 0x110000u}) {
    const std::array<uint32_t, 1> input{invalid};
    count = 99;
    EXPECT_FALSE(UnicodeNfc::normalize(input, workspace, count));
    EXPECT_EQ(count, 99u);
  }
  EXPECT_TRUE(UnicodeNfc::normalize({}, {}, count));
  EXPECT_EQ(count, 0u);
}
