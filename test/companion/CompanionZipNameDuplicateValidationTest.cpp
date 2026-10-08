#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "lib/Companion/CompanionZipNameDuplicateValidation.h"
using namespace companion;
namespace {
struct Stage : ZipRangeStorage, ZipNameBytesStorage {
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
  bool append(uint64_t at, std::span<const uint8_t> in) override { return at == bytes.size() && write(at, in); }
  bool seal(uint64_t size) override {
    if (!step() || size != bytes.size()) return false;
    sealed = true;
    return true;
  }
};
bool add(ZipNameDuplicateValidation& audit, const std::string& value) {
  return audit.add(std::span(reinterpret_cast<const uint8_t*>(value.data()), value.size()));
}
TEST(ZipNameDuplicateTest, ArbitraryOrderPrefixesAndLongCommonBanksAreUnique) {
  for (size_t width = 2; width <= 64; ++width) {
    Stage index, names;
    std::array<uint8_t, 64> scratch{};
    ZipNameDuplicateValidation audit(index, names, std::span(scratch).first(width));
    ASSERT_TRUE(audit.begin());
    for (const auto& name :
         std::array<std::string, 6>{"b", "aa", "a", "A", std::string(2048, 'x') + "b", std::string(2048, 'x') + "a"})
      ASSERT_TRUE(add(audit, name));
    EXPECT_TRUE(audit.finish());
    EXPECT_TRUE(index.sealed);
    EXPECT_TRUE(names.sealed);
  }
}
TEST(ZipNameDuplicateTest, ExactDuplicateCanonicalBytesRejectSealing) {
  Stage index, names;
  std::array<uint8_t, 8> scratch{};
  ZipNameDuplicateValidation audit(index, names, scratch);
  ASSERT_TRUE(audit.begin());
  ASSERT_TRUE(add(audit, "caf\xc3\xa9"));
  ASSERT_TRUE(add(audit, "other"));
  ASSERT_TRUE(add(audit, "caf\xc3\xa9"));
  EXPECT_FALSE(audit.finish());
  EXPECT_FALSE(index.sealed);
  EXPECT_FALSE(names.sealed);
}
TEST(ZipNameDuplicateTest, EveryStorageFailureAndCancellationRequireRestart) {
  Stage index, names;
  std::array<uint8_t, 8> scratch{};
  ZipNameDuplicateValidation audit(index, names, scratch);
  auto run = [](ZipNameDuplicateValidation& a) {
    return a.begin() && add(a, "b") && add(a, "a") && add(a, "aa") && a.finish();
  };
  ASSERT_TRUE(run(audit));
  const auto indexCalls = index.calls, nameCalls = names.calls;
  for (bool which : {false, true})
    for (unsigned fail = 1; fail <= (which ? nameCalls : indexCalls); ++fail) {
      index.calls = names.calls = 0;
      index.fail = which ? 0 : fail;
      names.fail = which ? fail : 0;
      EXPECT_FALSE(run(audit));
      EXPECT_FALSE(audit.finish());
    }
  index.fail = names.fail = 0;
  EXPECT_TRUE(run(audit));
  ZipNameDuplicateValidation cancelled(index, names, scratch, [](void*) { return false; });
  EXPECT_FALSE(run(cancelled));
}
TEST(ZipNameDuplicateTest, SingleRecordReferenceBoundsAndCountLimitFailClosed) {
  Stage index, names;
  std::array<uint8_t, 8> scratch{};
  ZipNameDuplicateValidation audit(index, names, scratch);
  ASSERT_TRUE(audit.begin(1));
  ASSERT_TRUE(add(audit, "a"));
  inventory_detail::write(index.bytes, 8, UINT64_MAX, 8);
  EXPECT_FALSE(audit.finish());
  EXPECT_FALSE(index.sealed);
  ASSERT_TRUE(audit.begin(1));
  ASSERT_TRUE(add(audit, "a"));
  EXPECT_FALSE(add(audit, "b"));
  EXPECT_FALSE(audit.finish());
}

}  // namespace
