#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

#include "lib/Companion/CompanionIdentity.h"
#include "lib/Serialization/CredentialIntegrity.h"

using namespace companion;

namespace {
class FakeIdentityStorage final : public IdentityStorage {
 public:
  Identity device{};
  Identity card{};
  Identity marker{};
  std::vector<uint8_t> binding;
  bool hasMarker = false;
  bool failMarker = false;
  bool failBinding = false;
  bool writeThenFail = false;
  bool emptyEntropy = false;
  uint8_t nonce = 10;
  FakeIdentityStorage() {
    device[0] = 1;
    card[0] = 2;
  }
  bool hardwareIdentity(Identity& value) override {
    value = device;
    return true;
  }
  bool cardIdentity(Identity& value) override {
    value = card;
    return true;
  }
  IdentityRead readBinding(std::span<uint8_t> output) override {
    if (binding.empty()) return IdentityRead::Missing;
    if (binding.size() != output.size()) return IdentityRead::Corrupt;
    std::copy(binding.begin(), binding.end(), output.begin());
    return IdentityRead::Present;
  }
  bool writeBinding(std::span<const uint8_t> input) override {
    if (failBinding && !writeThenFail) return false;
    binding.assign(input.begin(), input.end());
    return !failBinding;
  }
  IdentityRead readMarker(Identity& value) override {
    if (!hasMarker) return IdentityRead::Missing;
    value = marker;
    return IdentityRead::Present;
  }
  bool createMarker(const Identity& value) override {
    if (failMarker && !writeThenFail) return false;
    marker = value;
    hasMarker = true;
    return !failMarker;
  }
  bool randomIdentity(Identity& value) override {
    value.fill(0);
    if (!emptyEntropy) value[0] = nonce++;
    return true;
  }
};
}  // namespace

TEST(CompanionIdentity, StableHardwareAndGenerationWithFreshDurableEpochs) {
  FakeIdentityStorage storage;
  IdentityState first, next;
  ASSERT_EQ(provisionIdentity(storage, first), IdentityResult::Ok);
  ASSERT_EQ(provisionIdentity(storage, next), IdentityResult::Ok);
  EXPECT_EQ(first.device, next.device);
  EXPECT_EQ(first.storageGeneration, next.storageGeneration);
  EXPECT_EQ(next.eventEpoch, first.eventEpoch + 1);
}
TEST(CompanionIdentity, ClonedCardRotatesGenerationEvenWithIdenticalFiles) {
  FakeIdentityStorage storage;
  IdentityState first, clone;
  ASSERT_EQ(provisionIdentity(storage, first), IdentityResult::Ok);
  const auto originalMarker = storage.marker;
  storage.card[0] = 3;
  ASSERT_EQ(provisionIdentity(storage, clone), IdentityResult::Ok);
  EXPECT_EQ(storage.marker, originalMarker);
  EXPECT_EQ(first.device, clone.device);
  EXPECT_NE(first.storageGeneration, clone.storageGeneration);
  EXPECT_EQ(clone.eventEpoch, first.eventEpoch + 1);
}
TEST(CompanionIdentity, ReformattedCardRotatesGeneration) {
  FakeIdentityStorage storage;
  IdentityState first, reformatted;
  ASSERT_EQ(provisionIdentity(storage, first), IdentityResult::Ok);
  storage.hasMarker = false;
  ASSERT_EQ(provisionIdentity(storage, reformatted), IdentityResult::Ok);
  EXPECT_NE(first.storageGeneration, reformatted.storageGeneration);
  EXPECT_EQ(first.device, reformatted.device);
}
TEST(CompanionIdentity, FailedWritesNeverExposeAnUnreservedEpoch) {
  for (bool after : {false, true}) {
    FakeIdentityStorage storage;
    IdentityState first, output;
    ASSERT_EQ(provisionIdentity(storage, first), IdentityResult::Ok);
    output = first;
    storage.failBinding = true;
    storage.writeThenFail = after;
    EXPECT_EQ(provisionIdentity(storage, output), IdentityResult::IoError);
    EXPECT_EQ(output, first);
    storage.failBinding = false;
    ASSERT_EQ(provisionIdentity(storage, output), IdentityResult::Ok);
    EXPECT_EQ(output.eventEpoch, first.eventEpoch + (after ? 2 : 1));
  }
}
TEST(CompanionIdentity, MarkerCommitPrecedesBindingAndRecoversInterruptedCreation) {
  for (bool after : {false, true}) {
    FakeIdentityStorage storage;
    storage.failMarker = true;
    storage.writeThenFail = after;
    IdentityState output;
    EXPECT_EQ(provisionIdentity(storage, output), IdentityResult::IoError);
    EXPECT_TRUE(storage.binding.empty());
    EXPECT_EQ(output.eventEpoch, 0U);
    storage.failMarker = false;
    EXPECT_EQ(provisionIdentity(storage, output), IdentityResult::Ok);
  }
}
TEST(CompanionIdentity, RejectsEveryCorruptedBindingByteWithoutChangingOutput) {
  FakeIdentityStorage storage;
  IdentityState original, output;
  ASSERT_EQ(provisionIdentity(storage, original), IdentityResult::Ok);
  const auto saved = storage.binding;
  for (size_t i = 0; i < saved.size(); ++i) {
    storage.binding = saved;
    storage.binding[i] ^= 1;
    output = original;
    EXPECT_EQ(provisionIdentity(storage, output), IdentityResult::Corrupt) << i;
    EXPECT_EQ(output, original);
  }
}
TEST(CompanionIdentity, RejectsCorruptMarkerAndForeignHardwareBinding) {
  FakeIdentityStorage storage;
  IdentityState output;
  ASSERT_EQ(provisionIdentity(storage, output), IdentityResult::Ok);
  storage.device[0] = 3;
  EXPECT_EQ(provisionIdentity(storage, output), IdentityResult::WrongHardware);
  storage.device[0] = 1;
  storage.marker.fill(0);
  EXPECT_EQ(provisionIdentity(storage, output), IdentityResult::Corrupt);
}
TEST(CompanionIdentity, FactoryResetUsesFreshGenerationAndRandomizedEpoch) {
  FakeIdentityStorage storage;
  IdentityState first, reset;
  ASSERT_EQ(provisionIdentity(storage, first), IdentityResult::Ok);
  storage.binding.clear();
  ASSERT_EQ(provisionIdentity(storage, reset), IdentityResult::Ok);
  EXPECT_EQ(first.device, reset.device);
  EXPECT_NE(first.storageGeneration, reset.storageGeneration);
  EXPECT_NE(first.eventEpoch, reset.eventEpoch);
}
TEST(CompanionIdentity, RejectsMissingPhysicalIdentityAndEmptyEntropy) {
  FakeIdentityStorage storage;
  IdentityState output;
  storage.card.fill(0);
  EXPECT_EQ(provisionIdentity(storage, output), IdentityResult::Corrupt);
  storage.card[0] = 1;
  storage.emptyEntropy = true;
  EXPECT_EQ(provisionIdentity(storage, output), IdentityResult::EntropyError);
  EXPECT_TRUE(storage.binding.empty());
}

TEST(CompanionIdentity, ExhaustedEpochDoesNotWrapOrModifyOutput) {
  FakeIdentityStorage storage;
  IdentityState original, output;
  ASSERT_EQ(provisionIdentity(storage, original), IdentityResult::Ok);
  for (size_t i = 68; i < 76; ++i) storage.binding[i] = 0xff;
  const uint32_t checksum = credential_integrity::crc32({reinterpret_cast<const char*>(storage.binding.data()), 76});
  for (unsigned i = 0; i < 4; ++i) storage.binding[76 + i] = static_cast<uint8_t>(checksum >> (8 * i));
  const auto saved = storage.binding;
  output = original;
  EXPECT_EQ(provisionIdentity(storage, output), IdentityResult::Exhausted);
  EXPECT_EQ(output, original);
  EXPECT_EQ(storage.binding, saved);
}

TEST(CompanionIdentity, TruncatedBindingsFailClosed) {
  FakeIdentityStorage storage;
  IdentityState original, output;
  ASSERT_EQ(provisionIdentity(storage, original), IdentityResult::Ok);
  const auto saved = storage.binding;
  for (size_t length = 1; length < saved.size(); ++length) {
    storage.binding.assign(saved.begin(), saved.begin() + length);
    output = original;
    EXPECT_EQ(provisionIdentity(storage, output), IdentityResult::Corrupt) << length;
    EXPECT_EQ(output, original);
  }
}
