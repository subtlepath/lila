#include <gtest/gtest.h>

#include <algorithm>

#include "lib/Companion/CompanionPairings.h"
using namespace companion;
namespace {
struct Store : PairingsStorage {
  std::array<uint8_t, PAIRINGS_RECORD_SIZE> data{};
  bool exists = false, fail = false, commitThenFail = false;
  int writes = 0;
  PairingsRead read(std::span<uint8_t> out) override {
    if (fail) return PairingsRead::Error;
    if (!exists) return PairingsRead::Missing;
    std::copy(data.begin(), data.end(), out.begin());
    return PairingsRead::Present;
  }
  bool write(std::span<const uint8_t> bytes) override {
    ++writes;
    if (fail) return false;
    std::copy(bytes.begin(), bytes.end(), data.begin());
    exists = true;
    return !commitThenFail;
  }
};
Identity id(uint8_t value) {
  Identity result{};
  result[0] = value;
  return result;
}
PairingSecret secret(uint8_t value) {
  PairingSecret result{};
  result[0] = value;
  return result;
}
PairingPeer peer(uint8_t value) {
  PairingPeer result{};
  result[0] = value;
  return result;
}
}  // namespace
TEST(CompanionPairings, DurableBindingsAuthorizeOnlyExactInstallationAndSecret) {
  Store storage;
  Pairings registry(storage);
  std::array<uint8_t, PAIRINGS_RECORD_SIZE> scratch{};
  EXPECT_FALSE(registry.authenticate(id(1), secret(2)));
  ASSERT_EQ(registry.load(scratch), PairingResult::Ok);
  ASSERT_EQ(registry.add(id(1), secret(2), peer(3), scratch), PairingResult::Ok);
  EXPECT_TRUE(registry.authenticate(id(1), secret(2)));
  EXPECT_TRUE(registry.boundTo(id(1), peer(3)));
  EXPECT_FALSE(registry.boundTo(id(1), peer(4)));
  EXPECT_FALSE(registry.authenticate(id(2), secret(2)));
  for (size_t i = 0; i < 32; ++i) {
    auto wrong = secret(2);
    wrong[i] ^= 1;
    EXPECT_FALSE(registry.authenticate(id(1), wrong));
  }
  Pairings restarted(storage);
  ASSERT_EQ(restarted.load(scratch), PairingResult::Ok);
  EXPECT_TRUE(restarted.authenticate(id(1), secret(2)));
  EXPECT_EQ(restarted.add(id(1), secret(9), peer(9), scratch), PairingResult::Exists);
  EXPECT_EQ(storage.writes, 1);
}
TEST(CompanionPairings, BoundsCapacityAndForgetRemovesAllInstallationsForPeer) {
  Store storage;
  Pairings registry(storage);
  std::array<uint8_t, PAIRINGS_RECORD_SIZE> scratch{};
  ASSERT_EQ(registry.load(scratch), PairingResult::Ok);
  for (uint8_t i = 1; i <= MAX_PAIRINGS; ++i)
    ASSERT_EQ(registry.add(id(i), secret(i), peer(i < 3 ? 1 : i), scratch), PairingResult::Ok);
  EXPECT_EQ(registry.add(id(9), secret(9), peer(9), scratch), PairingResult::Full);
  ASSERT_EQ(registry.forget(peer(1), scratch), PairingResult::Ok);
  EXPECT_FALSE(registry.authenticate(id(1), secret(1)));
  EXPECT_FALSE(registry.authenticate(id(2), secret(2)));
  EXPECT_TRUE(registry.authenticate(id(3), secret(3)));
  const auto writes = storage.writes;
  EXPECT_EQ(registry.forget(peer(1), scratch), PairingResult::Ok);
  EXPECT_EQ(storage.writes, writes);
  EXPECT_EQ(registry.add(id(9), secret(9), peer(9), scratch), PairingResult::Ok);
}
TEST(CompanionPairings, UncertainCommitBlocksUntilDurableReload) {
  Store storage;
  Pairings registry(storage);
  std::array<uint8_t, PAIRINGS_RECORD_SIZE> scratch{};
  ASSERT_EQ(registry.load(scratch), PairingResult::Ok);
  storage.commitThenFail = true;
  EXPECT_EQ(registry.add(id(1), secret(2), peer(3), scratch), PairingResult::IoError);
  EXPECT_FALSE(registry.authenticate(id(1), secret(2)));
  EXPECT_EQ(registry.add(id(2), secret(3), peer(4), scratch), PairingResult::Unavailable);
  ASSERT_EQ(registry.load(scratch), PairingResult::Ok);
  EXPECT_TRUE(registry.authenticate(id(1), secret(2)));
  EXPECT_EQ(registry.forget(peer(3), scratch), PairingResult::IoError);
  EXPECT_FALSE(registry.authenticate(id(1), secret(2)));
  ASSERT_EQ(registry.load(scratch), PairingResult::Ok);
  EXPECT_FALSE(registry.authenticate(id(1), secret(2)));
}
TEST(CompanionPairings, EveryCorruptedByteFailsClosed) {
  Store storage;
  Pairings registry(storage);
  std::array<uint8_t, PAIRINGS_RECORD_SIZE> scratch{};
  ASSERT_EQ(registry.load(scratch), PairingResult::Ok);
  ASSERT_EQ(registry.add(id(1), secret(2), peer(3), scratch), PairingResult::Ok);
  const auto original = storage.data;
  for (size_t i = 0; i < original.size(); ++i) {
    storage.data = original;
    storage.data[i] ^= 1;
    EXPECT_EQ(registry.load(scratch), PairingResult::Corrupt) << i;
    EXPECT_FALSE(registry.authenticate(id(1), secret(2)));
  }
}
TEST(CompanionPairings, InvalidInputsAndUnavailableStorageDoNotWrite) {
  Store storage;
  Pairings registry(storage);
  std::array<uint8_t, PAIRINGS_RECORD_SIZE> scratch{};
  EXPECT_EQ(registry.load(std::span(scratch).first(231)), PairingResult::Invalid);
  ASSERT_EQ(registry.load(scratch), PairingResult::Ok);
  EXPECT_EQ(registry.add(id(0), secret(1), peer(1), scratch), PairingResult::Invalid);
  EXPECT_EQ(registry.add(id(1), secret(0), peer(1), scratch), PairingResult::Invalid);
  EXPECT_EQ(registry.add(id(1), secret(1), peer(1), std::span(scratch).first(231)), PairingResult::Invalid);
  EXPECT_EQ(storage.writes, 0);
  storage.fail = true;
  EXPECT_EQ(registry.load(scratch), PairingResult::IoError);
  EXPECT_EQ(registry.add(id(1), secret(1), peer(1), scratch), PairingResult::Unavailable);
}
