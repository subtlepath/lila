#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "lib/Companion/CompanionZipRangeValidation.h"
using namespace companion;
namespace {
struct Stage : ZipRangeStorage {
  std::vector<uint8_t> bytes;
  unsigned calls = 0, fail = 0;
  bool sealed = false;
  bool step() { return ++calls != fail; }
  bool reset() override {
    if (!step()) return false;
    bytes.clear();
    sealed = false;
    return true;
  }
  bool read(uint64_t at, std::span<uint8_t> out) override {
    if (!step() || at > bytes.size() || out.size() > bytes.size() - at) return false;
    std::copy_n(bytes.begin() + at, out.size(), out.begin());
    return true;
  }
  bool write(uint64_t at, std::span<const uint8_t> in) override {
    if (!step()) return false;
    bytes.resize(std::max<uint64_t>(bytes.size(), at + in.size()));
    std::copy(in.begin(), in.end(), bytes.begin() + at);
    return true;
  }
  bool seal(uint64_t size) override {
    if (!step() || size != bytes.size()) return false;
    sealed = true;
    return true;
  }
};
TEST(ZipRangeTest, UnorderedAdjacentRangesSortWithoutOverlap) {
  for (uint64_t count : {0u, 1u, 2u, 3u, 30u, 301u}) {
    Stage stage;
    ZipRangeValidation audit(stage);
    ASSERT_TRUE(audit.begin(10000));
    for (uint64_t i = count; i-- > 0;) ASSERT_TRUE(audit.add(i * 20, i * 20 + 20));
    ASSERT_TRUE(audit.finish());
    EXPECT_TRUE(stage.sealed);
    for (uint64_t i = 0; i < count; ++i) EXPECT_EQ(inventory_detail::read(stage.bytes, i * 16, 8), i * 20);
  }
}
TEST(ZipRangeTest, DuplicateNestedAndCrossingRangesRejectPublication) {
  for (auto pair : {std::array<uint64_t, 4>{1, 10, 1, 10}, {1, 10, 2, 3}, {5, 15, 1, 10}}) {
    Stage stage;
    ZipRangeValidation audit(stage);
    ASSERT_TRUE(audit.begin(100));
    ASSERT_TRUE(audit.add(pair[0], pair[1]));
    ASSERT_TRUE(audit.add(pair[2], pair[3]));
    EXPECT_FALSE(audit.finish());
    EXPECT_FALSE(stage.sealed);
  }
}
TEST(ZipRangeTest, EveryStageFailureAndCancellationRequiresRestart) {
  auto run = [](ZipRangeValidation& audit) {
    return audit.begin(100) && audit.add(20, 30) && audit.add(0, 10) && audit.add(10, 20) && audit.finish();
  };
  Stage stage;
  ZipRangeValidation audit(stage);
  ASSERT_TRUE(run(audit));
  const auto calls = stage.calls;
  for (unsigned fail = 1; fail <= calls; ++fail) {
    stage.calls = 0;
    stage.fail = fail;
    EXPECT_FALSE(run(audit));
    EXPECT_FALSE(audit.finish());
  }
  stage.fail = 0;
  ASSERT_TRUE(run(audit));
  struct Control {
    unsigned calls = 0, fail = 0;
  } control;
  ZipRangeValidation cancellable(
      stage,
      [](void* ptr) {
        auto& c = *static_cast<Control*>(ptr);
        return ++c.calls != c.fail;
      },
      &control);
  ASSERT_TRUE(run(cancellable));
  const auto ticks = control.calls;
  for (unsigned fail = 1; fail <= ticks; ++fail) {
    control.calls = 0;
    control.fail = fail;
    EXPECT_FALSE(run(cancellable));
  }
  control.fail = 0;
  EXPECT_TRUE(run(cancellable));
}
TEST(ZipRangeTest, InvalidBoundsAndCountLimitsFailClosed) {
  Stage stage;
  ZipRangeValidation audit(stage);
  ASSERT_TRUE(audit.begin(100, 1));
  EXPECT_FALSE(audit.add(10, 10));
  EXPECT_FALSE(audit.finish());
  ASSERT_TRUE(audit.begin(100, 1));
  EXPECT_FALSE(audit.add(0, 101));
  ASSERT_TRUE(audit.begin(100, 1));
  ASSERT_TRUE(audit.add(0, 10));
  EXPECT_FALSE(audit.add(10, 20));
  EXPECT_FALSE(audit.begin(100, UINT64_MAX));
}
}  // namespace
