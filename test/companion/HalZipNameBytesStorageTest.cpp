#include <gtest/gtest.h>

#include "lib/hal/HalZipNameBytesStorage.h"
#include "lib/hal/HalZipRangeStorage.h"
using namespace companion;
namespace {
class HalZipNamesTest : public testing::Test {
 protected:
  void SetUp() override { inventory_hal_test::state = {}; }
};
TEST_F(HalZipNamesTest, DuplicateAuditComposesBothHalProvidersAndReusesHandles) {
  HalZipRangeStorage index(HalZipRangeStorage::Purpose::NameIndex);
  HalZipNameBytesStorage names;
  std::array<uint8_t, 64> scratch{};
  ZipNameDuplicateValidation audit(index, names, scratch);
  const std::array<uint8_t, 2> first{'b', 'b'}, second{'a', 'a'};
  ASSERT_TRUE(audit.begin());
  ASSERT_TRUE(audit.add(first));
  ASSERT_TRUE(audit.add(second));
  ASSERT_TRUE(audit.finish());
  EXPECT_EQ(inventory_hal_test::state.files.at(HalZipNameBytesStorage::PATH).size(), 4u);
  ASSERT_TRUE(names.discard());
  ASSERT_TRUE(index.discard());
  ASSERT_TRUE(audit.begin());
  ASSERT_TRUE(audit.add(first));
  ASSERT_TRUE(audit.add(first));
  EXPECT_FALSE(audit.finish());
  ASSERT_TRUE(names.discard());
  ASSERT_TRUE(index.discard());
  EXPECT_EQ(inventory_hal_test::state.preparations, 6u);
}
TEST_F(HalZipNamesTest, LongAppendReadbackCorruptionAndRetry) {
  HalZipNameBytesStorage names;
  std::array<uint8_t, 3072> bytes{};
  bytes.fill('a');
  ASSERT_TRUE(names.reset());
  ASSERT_TRUE(names.append(0, bytes));
  EXPECT_GT(inventory_hal_test::state.yields, 0u);
  ASSERT_TRUE(names.seal(bytes.size()));
  ASSERT_TRUE(names.discard());
  ASSERT_TRUE(names.reset());
  inventory_hal_test::state.corruptWrite = true;
  EXPECT_FALSE(names.append(0, bytes));
  EXPECT_FALSE(names.seal(bytes.size()));
  inventory_hal_test::state.corruptWrite = false;
  ASSERT_TRUE(names.discard());
  ASSERT_TRUE(names.reset());
  ASSERT_TRUE(names.append(0, bytes));
  ASSERT_TRUE(names.seal(bytes.size()));
}
TEST_F(HalZipNamesTest, UnknownStageAndCleanupFailurePreserveOwnership) {
  const std::vector<uint8_t> original{1, 2};
  inventory_hal_test::state.files[HalZipNameBytesStorage::PATH] = original;
  {
    HalZipNameBytesStorage names;
    EXPECT_FALSE(names.reset());
  }
  EXPECT_EQ(inventory_hal_test::state.files.at(HalZipNameBytesStorage::PATH), original);
  inventory_hal_test::state.files.clear();
  HalZipNameBytesStorage names;
  ASSERT_TRUE(names.reset());
  inventory_hal_test::state.failRemove = true;
  EXPECT_FALSE(names.discard());
  inventory_hal_test::state.failRemove = false;
  EXPECT_TRUE(names.discard());
}
TEST_F(HalZipNamesTest, SealFailuresCannotPublish) {
  for (unsigned fault = 0; fault < 3; ++fault) {
    inventory_hal_test::state = {};
    HalZipNameBytesStorage names;
    ASSERT_TRUE(names.reset());
    const std::array<uint8_t, 1> byte{'a'};
    ASSERT_TRUE(names.append(0, byte));
    if (fault == 0) inventory_hal_test::state.failSync = true;
    if (fault == 1) inventory_hal_test::state.failTruncate = true;
    if (fault == 2) inventory_hal_test::state.failClose = true;
    EXPECT_FALSE(names.seal(1));
    inventory_hal_test::state.failSync = inventory_hal_test::state.failTruncate = inventory_hal_test::state.failClose =
        false;
    EXPECT_TRUE(names.discard());
  }
}
}  // namespace
