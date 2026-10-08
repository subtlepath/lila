#include <gtest/gtest.h>

#include <array>

#include "lib/hal/HalInventoryIndexStorage.h"
using namespace companion;
using inventory_hal_test::state;
class InventoryHal : public testing::Test {
  void SetUp() override { state = inventory_hal_test::State{}; }
};
TEST_F(InventoryHal, ReusesHandleYieldsAndClosesBeforeReopen) {
  state.bytes.assign(100, 7);
  std::array<uint8_t, 1> byte{};
  {
    HalInventoryIndexStorage storage;
    ASSERT_TRUE(storage.open("/snapshot"));
    for (unsigned i = 0; i < 32; ++i) ASSERT_TRUE(storage.read(i, byte));
    EXPECT_EQ(byte[0], 7);
    EXPECT_EQ(state.opens, 1);
    EXPECT_EQ(state.yields, 1);
    ASSERT_TRUE(storage.open("/next"));
    EXPECT_EQ(state.opens, 2);
    EXPECT_EQ(state.closes, 1);
  }
  EXPECT_EQ(state.opens, state.closes);
}
TEST_F(InventoryHal, ShortReadsAndSeekFailuresStayFailedUntilReopen) {
  state.bytes.assign(100, 7);
  HalInventoryIndexStorage storage;
  std::array<uint8_t, 2> output{};
  for (bool shortRead : {false, true}) {
    ASSERT_TRUE(storage.open("/snapshot"));
    state.shortRead = shortRead;
    state.failSeek = !shortRead;
    EXPECT_FALSE(storage.read(0, output));
    state.shortRead = false;
    state.failSeek = false;
    const unsigned reads = state.reads;
    EXPECT_FALSE(storage.read(0, output));
    EXPECT_EQ(state.reads, reads);
    uint64_t size = 42;
    EXPECT_FALSE(storage.size(size));
    EXPECT_EQ(size, 42);
  }
  EXPECT_GT(state.errors, 0);
}
TEST_F(InventoryHal, RejectsUnavailableDirectoryAndOutOfBoundsReads) {
  HalInventoryIndexStorage storage;
  EXPECT_FALSE(storage.open(nullptr));
  state.failOpen = true;
  EXPECT_FALSE(storage.open("/snapshot"));
  state.failOpen = false;
  state.directory = true;
  EXPECT_FALSE(storage.open("/directory"));
  EXPECT_EQ(state.opens, state.closes);
  state.directory = false;
  state.bytes.assign(10, 7);
  ASSERT_TRUE(storage.open("/snapshot"));
  std::array<uint8_t, 2> output{9, 9};
  EXPECT_FALSE(storage.read(9, output));
  EXPECT_FALSE(storage.read(UINT64_MAX, output));
  EXPECT_EQ(state.reads, 0);
  EXPECT_EQ(output[0], 9);
  EXPECT_TRUE(storage.read(10, {}));
}
