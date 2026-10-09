#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

#include "lib/Companion/CompanionIdentity.h"
#include "lib/Serialization/CredentialIntegrity.h"
#include "src/CompanionBookmarkReaderBinding.h"

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
  unsigned bindingWrites = 0, markerWrites = 0;
  bool bindingReadError = false, markerReadError = false;
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
    if (bindingReadError) return IdentityRead::Error;
    if (binding.empty()) return IdentityRead::Missing;
    if (binding.size() != output.size()) return IdentityRead::Corrupt;
    std::copy(binding.begin(), binding.end(), output.begin());
    return IdentityRead::Present;
  }
  bool writeBinding(std::span<const uint8_t> input) override {
    ++bindingWrites;
    if (failBinding && !writeThenFail) return false;
    binding.assign(input.begin(), input.end());
    return !failBinding;
  }
  IdentityRead readMarker(Identity& value) override {
    if (markerReadError) return IdentityRead::Error;
    if (!hasMarker) return IdentityRead::Missing;
    value = marker;
    return IdentityRead::Present;
  }
  bool createMarker(const Identity& value) override {
    ++markerWrites;
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

TEST(CompanionIdentity, InspectionNeverReservesEpochOrCreatesMissingIdentity) {
  FakeIdentityStorage storage;
  IdentityState output, expected;
  EXPECT_EQ(inspectIdentity(storage, output), IdentityInspectionResult::Unavailable);
  EXPECT_EQ(storage.bindingWrites, 0u);
  EXPECT_EQ(storage.markerWrites, 0u);
  EXPECT_EQ(storage.nonce, 10);
  ASSERT_EQ(provisionIdentity(storage, expected), IdentityResult::Ok);
  const auto saved = storage.binding;
  const auto nonce = storage.nonce;
  for (int i = 0; i < 3; ++i) {
    ASSERT_EQ(inspectIdentity(storage, output), IdentityInspectionResult::Ok);
    EXPECT_EQ(output, expected);
  }
  EXPECT_EQ(storage.binding, saved);
  EXPECT_EQ(storage.nonce, nonce);
  EXPECT_EQ(storage.bindingWrites, 1u);
  EXPECT_EQ(storage.markerWrites, 1u);
  storage.hasMarker = false;
  EXPECT_EQ(inspectIdentity(storage, output), IdentityInspectionResult::Unavailable);
  EXPECT_EQ(output, expected);
  EXPECT_FALSE(storage.hasMarker);
  EXPECT_EQ(storage.markerWrites, 1u);
}
TEST(CompanionIdentity, InspectionRejectsChangedCardMarkerHardwareAndReadFailuresWithoutMutation) {
  FakeIdentityStorage storage;
  IdentityState expected, output;
  ASSERT_EQ(provisionIdentity(storage, expected), IdentityResult::Ok);
  output = expected;
  const auto saved = storage.binding;
  storage.card[0] ^= 1;
  EXPECT_EQ(inspectIdentity(storage, output), IdentityInspectionResult::WrongStorage);
  EXPECT_EQ(output, expected);
  storage.card[0] ^= 1;
  storage.marker[0] ^= 1;
  EXPECT_EQ(inspectIdentity(storage, output), IdentityInspectionResult::WrongStorage);
  EXPECT_EQ(output, expected);
  storage.marker[0] ^= 1;
  storage.device[0] ^= 3;
  EXPECT_EQ(inspectIdentity(storage, output), IdentityInspectionResult::WrongHardware);
  EXPECT_EQ(output, expected);
  storage.device[0] ^= 3;
  storage.bindingReadError = true;
  EXPECT_EQ(inspectIdentity(storage, output), IdentityInspectionResult::IoError);
  EXPECT_EQ(output, expected);
  storage.bindingReadError = false;
  storage.markerReadError = true;
  EXPECT_EQ(inspectIdentity(storage, output), IdentityInspectionResult::IoError);
  EXPECT_EQ(output, expected);
  EXPECT_EQ(storage.binding, saved);
  EXPECT_EQ(storage.bindingWrites, 1u);
  EXPECT_EQ(storage.markerWrites, 1u);
}
TEST(CompanionIdentity, InspectionRejectsCorruptionAndAcceptsExhaustedReadOnlyEpoch) {
  FakeIdentityStorage storage;
  IdentityState expected, output;
  ASSERT_EQ(provisionIdentity(storage, expected), IdentityResult::Ok);
  const auto saved = storage.binding;
  for (size_t i = 0; i < saved.size(); ++i) {
    storage.binding = saved;
    storage.binding[i] ^= 1;
    output = expected;
    EXPECT_EQ(inspectIdentity(storage, output), IdentityInspectionResult::Corrupt) << i;
    EXPECT_EQ(output, expected);
  }
  storage.binding = saved;
  for (size_t i = 68; i < 76; ++i) storage.binding[i] = 0xff;
  const uint32_t checksum = credential_integrity::crc32({reinterpret_cast<const char*>(storage.binding.data()), 76});
  for (unsigned i = 0; i < 4; ++i) storage.binding[76 + i] = static_cast<uint8_t>(checksum >> (8 * i));
  ASSERT_EQ(inspectIdentity(storage, output), IdentityInspectionResult::Ok);
  EXPECT_EQ(output.eventEpoch, UINT64_MAX);
  EXPECT_EQ(storage.bindingWrites, 1u);
  EXPECT_EQ(storage.markerWrites, 1u);
}

TEST(CompanionIdentity, BookmarkRestoreBootstrapsOnceAndSubsequentProofsAreReadOnly) {
  FakeIdentityStorage storage;
  Digest edition{};
  edition.fill(7);
  IdentityState first, repeated;
  ASSERT_EQ(prepareBookmarkReaderIdentity(storage, edition, first), TintaJournalResult::Ok);
  const auto record = storage.binding;
  const auto nonce = storage.nonce;
  ASSERT_EQ(prepareBookmarkReaderIdentity(storage, edition, repeated), TintaJournalResult::Ok);
  EXPECT_EQ(first, repeated);
  EXPECT_EQ(storage.binding, record);
  EXPECT_EQ(storage.nonce, nonce);
  EXPECT_EQ(storage.bindingWrites, 1u);
  EXPECT_EQ(storage.markerWrites, 1u);
}
TEST(CompanionIdentity, BookmarkEditsRequireRestoredBindingAndPermitWriterEpochAdvancement) {
  FakeIdentityStorage storage;
  Digest edition{};
  edition.fill(7);
  IdentityState state;
  ASSERT_EQ(prepareBookmarkReaderIdentity(storage, edition, state), TintaJournalResult::Ok);
  ReaderBookmarkBinding binding{edition, state.device, state.storageGeneration, false};
  EXPECT_EQ(validateBookmarkReaderBinding(storage, edition, binding, state), TintaJournalResult::Invalid);
  binding.ready = true;
  IdentityState advanced;
  ASSERT_EQ(provisionIdentity(storage, advanced), IdentityResult::Ok);
  const auto record = storage.binding;
  const auto nonce = storage.nonce;
  EXPECT_EQ(validateBookmarkReaderBinding(storage, edition, binding, state), TintaJournalResult::Ok);
  EXPECT_EQ(state.eventEpoch, advanced.eventEpoch);
  EXPECT_TRUE(binding.ready);
  EXPECT_EQ(storage.binding, record);
  EXPECT_EQ(storage.nonce, nonce);
  auto different = edition;
  different[0] ^= 1;
  EXPECT_EQ(validateBookmarkReaderBinding(storage, different, binding, state), TintaJournalResult::Invalid);
  EXPECT_FALSE(binding.ready);
  EXPECT_EQ(storage.binding, record);
}
TEST(CompanionIdentity, ChangedCardInvalidatesBookmarkEditsWithoutProvisioning) {
  FakeIdentityStorage storage;
  Digest edition{};
  edition.fill(7);
  IdentityState state;
  ASSERT_EQ(prepareBookmarkReaderIdentity(storage, edition, state), TintaJournalResult::Ok);
  ReaderBookmarkBinding binding{edition, state.device, state.storageGeneration, true};
  const auto record = storage.binding;
  const auto nonce = storage.nonce;
  storage.card[0] ^= 1;
  EXPECT_EQ(validateBookmarkReaderBinding(storage, edition, binding, state), TintaJournalResult::Invalid);
  EXPECT_FALSE(binding.ready);
  EXPECT_EQ(storage.binding, record);
  EXPECT_EQ(storage.nonce, nonce);
  EXPECT_EQ(storage.bindingWrites, 1u);
  EXPECT_EQ(storage.markerWrites, 1u);
  ASSERT_EQ(prepareBookmarkReaderIdentity(storage, edition, state), TintaJournalResult::Ok);
  EXPECT_NE(state.storageGeneration, binding.storageGeneration);
  EXPECT_EQ(storage.bindingWrites, 2u);
}
TEST(CompanionIdentity, BookmarkChoiceBindingRequiresAConflictAndNeverEnablesOrdinaryEdits) {
  FakeIdentityStorage storage;
  Digest edition{};
  edition.fill(7);
  IdentityState state;
  ASSERT_EQ(prepareBookmarkReaderIdentity(storage, edition, state), TintaJournalResult::Ok);
  ReaderBookmarkBinding binding{edition, state.device, state.storageGeneration, false};
  const auto record = storage.binding;
  const auto nonce = storage.nonce;
  EXPECT_EQ(validateBookmarkChoiceBinding(storage, edition, binding, state), TintaJournalResult::Invalid);
  binding.conflict[0] = 3;
  binding.legacyDecision = LegacyBookmarkDecision::Associate;
  EXPECT_EQ(validateBookmarkReaderBinding(storage, edition, binding, state), TintaJournalResult::Invalid);
  EXPECT_EQ(binding.conflict[0], 3u);
  ASSERT_EQ(validateBookmarkChoiceBinding(storage, edition, binding, state), TintaJournalResult::Ok);
  EXPECT_FALSE(binding.ready);
  EXPECT_EQ(binding.legacyDecision, LegacyBookmarkDecision::Associate);
  EXPECT_EQ(binding.conflict[0], 3u);
  EXPECT_EQ(storage.binding, record);
  EXPECT_EQ(storage.nonce, nonce);
  EXPECT_EQ(storage.bindingWrites, 1u);
  EXPECT_EQ(storage.markerWrites, 1u);
}

TEST(CompanionIdentity, LegacyAssociationRequiresPendingProofAndDoesNotEnableEditsOrReserveIdentity) {
  FakeIdentityStorage storage;
  Digest edition{};
  edition.fill(7);
  IdentityState state;
  ASSERT_EQ(prepareBookmarkReaderIdentity(storage, edition, state), TintaJournalResult::Ok);
  ReaderBookmarkBinding binding{edition, state.device, state.storageGeneration, false};
  binding.legacyAssociationRequired = true;
  const auto record = storage.binding;
  const auto nonce = storage.nonce;
  ASSERT_EQ(validateBookmarkAssociationBinding(storage, edition, binding, state), TintaJournalResult::Ok);
  EXPECT_FALSE(binding.ready);
  EXPECT_TRUE(binding.legacyAssociationRequired);
  EXPECT_EQ(validateBookmarkReaderBinding(storage, edition, binding, state), TintaJournalResult::Invalid);
  EXPECT_TRUE(binding.legacyAssociationRequired);
  EXPECT_EQ(storage.binding, record);
  EXPECT_EQ(storage.nonce, nonce);
  EXPECT_EQ(storage.bindingWrites, 1u);
  EXPECT_EQ(storage.markerWrites, 1u);
}

TEST(CompanionIdentity, LegacyAssociationRejectsStaleOrMixedProofWithoutProvisioning) {
  for (unsigned mode = 0; mode < 8; ++mode) {
    FakeIdentityStorage storage;
    Digest edition{};
    edition.fill(7);
    IdentityState state;
    ASSERT_EQ(prepareBookmarkReaderIdentity(storage, edition, state), TintaJournalResult::Ok);
    ReaderBookmarkBinding binding{edition, state.device, state.storageGeneration, false};
    binding.legacyAssociationRequired = true;
    if (mode == 0) binding.legacyAssociationRequired = false;
    if (mode == 1) binding.ready = true;
    if (mode == 2) binding.conflict[0] = 3;
    if (mode == 3) edition[0] ^= 1;
    if (mode == 4) storage.card[0] ^= 1;
    if (mode == 5) storage.device[0] ^= 1;
    if (mode == 6) storage.bindingReadError = true;
    if (mode == 7) storage.binding.clear();
    const auto record = storage.binding;
    const auto nonce = storage.nonce;
    EXPECT_EQ(validateBookmarkAssociationBinding(storage, edition, binding, state),
              mode == 6 ? TintaJournalResult::IoError : TintaJournalResult::Invalid);
    EXPECT_FALSE(binding.ready);
    EXPECT_FALSE(binding.legacyAssociationRequired);
    EXPECT_FALSE(tinta_body_detail::nonzero(binding.conflict));
    EXPECT_EQ(storage.binding, record);
    EXPECT_EQ(storage.nonce, nonce);
    EXPECT_EQ(storage.bindingWrites, 1u);
    EXPECT_EQ(storage.markerWrites, 1u);
  }
}

TEST(CompanionIdentity, BookmarkChoiceBindingRejectsChangedEditionCardAndUnreadableProofWithoutProvisioning) {
  for (unsigned mode = 0; mode < 3; ++mode) {
    FakeIdentityStorage storage;
    Digest edition{};
    edition.fill(7);
    IdentityState state;
    ASSERT_EQ(prepareBookmarkReaderIdentity(storage, edition, state), TintaJournalResult::Ok);
    ReaderBookmarkBinding binding{edition, state.device, state.storageGeneration, false};
    binding.conflict[0] = 3;
    const auto record = storage.binding;
    const auto nonce = storage.nonce;
    if (mode == 0) edition[0] ^= 1;
    if (mode == 1) storage.card[0] ^= 1;
    if (mode == 2) storage.bindingReadError = true;
    EXPECT_EQ(validateBookmarkChoiceBinding(storage, edition, binding, state),
              mode == 2 ? TintaJournalResult::IoError : TintaJournalResult::Invalid);
    EXPECT_FALSE(binding.ready);
    EXPECT_FALSE(std::any_of(binding.conflict.begin(), binding.conflict.end(), [](uint8_t byte) { return byte; }));
    EXPECT_EQ(storage.binding, record);
    EXPECT_EQ(storage.nonce, nonce);
    EXPECT_EQ(storage.bindingWrites, 1u);
    EXPECT_EQ(storage.markerWrites, 1u);
  }
}

TEST(CompanionIdentity, BookmarkRestoreProtectsCorruptForeignAndUnreadableIdentityRecords) {
  FakeIdentityStorage storage;
  Digest edition{};
  edition.fill(7);
  IdentityState state;
  ASSERT_EQ(provisionIdentity(storage, state), IdentityResult::Ok);
  const auto record = storage.binding;
  const auto nonce = storage.nonce;
  storage.binding[10] ^= 1;
  EXPECT_EQ(prepareBookmarkReaderIdentity(storage, edition, state), TintaJournalResult::Corrupt);
  storage.binding = record;
  storage.device[0] ^= 1;
  EXPECT_EQ(prepareBookmarkReaderIdentity(storage, edition, state), TintaJournalResult::Corrupt);
  storage.device[0] ^= 1;
  storage.bindingReadError = true;
  EXPECT_EQ(prepareBookmarkReaderIdentity(storage, edition, state), TintaJournalResult::IoError);
  storage.bindingReadError = false;
  edition.fill(0);
  EXPECT_EQ(prepareBookmarkReaderIdentity(storage, edition, state), TintaJournalResult::Invalid);
  EXPECT_EQ(storage.binding, record);
  EXPECT_EQ(storage.nonce, nonce);
  EXPECT_EQ(storage.bindingWrites, 1u);
  EXPECT_EQ(storage.markerWrites, 1u);
}
