#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <fstream>
#include <iterator>

#include "lib/hal/HalRemovalStageClaimStorage.h"
using namespace companion;
namespace {
class RemovalStageClaimTest : public testing::Test {
 protected:
  RemovalStageClaim claim;
  std::array<uint8_t, REMOVAL_STAGE_CLAIM_SIZE> scratch{};
  HalRemovalStageClaimStorage store{scratch};
  void SetUp() override {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = state.falseExists = true;
    claim.request.transaction.fill(1);
    claim.request.owner.fill(2);
    claim.request.generation.fill(3);
    claim.request.manifest.kind = ContentKind::Epub;
    claim.request.manifest.formatVersion = 1;
    claim.request.manifest.length = 42;
    claim.request.manifest.contentHash.fill(4);
    claim.inventoryRevision = 7;
  }
  std::vector<uint8_t> bytes(const RemovalStageClaim& value) {
    std::vector<uint8_t> result(REMOVAL_STAGE_CLAIM_SIZE);
    EXPECT_EQ(encodeRemovalStageClaim(value, result), result.size());
    return result;
  }
  std::string marker() {
    const auto encoded = bytes(claim);
    Digest digest{};
    EXPECT_EQ(EVP_Digest(encoded.data(), encoded.size(), digest.data(), nullptr, EVP_sha256(), nullptr), 1);
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    std::string result = "/.crosspoint/companion/removal-multi-";
    result.reserve(112);
    for (uint8_t value : digest) {
      result += HEX_DIGITS[value >> 4];
      result += HEX_DIGITS[value & 15];
    }
    return result + ".owner";
  }
};
}  // namespace
TEST_F(RemovalStageClaimTest, CodecExactUnalignedFramingAndOwnership) {
  const auto original = bytes(claim);
  std::array<uint8_t, REMOVAL_STAGE_CLAIM_SIZE + 2> output{};
  output.fill(99);
  EXPECT_EQ(encodeRemovalStageClaim(claim, std::span(output).subspan(1, REMOVAL_STAGE_CLAIM_SIZE)),
            REMOVAL_STAGE_CLAIM_SIZE);
  EXPECT_EQ(output.front(), 99);
  EXPECT_EQ(output.back(), 99);
  RemovalStageClaim decoded;
  ASSERT_TRUE(decodeRemovalStageClaim(std::span(output).subspan(1, REMOVAL_STAGE_CLAIM_SIZE), decoded));
  EXPECT_EQ(decoded, claim);
  decoded = claim;
  for (size_t length = 0; length < original.size(); ++length) {
    EXPECT_FALSE(decodeRemovalStageClaim(std::span(original).first(length), decoded));
    EXPECT_EQ(decoded, claim);
  }
  for (size_t at = 0; at < original.size(); ++at) {
    auto corrupt = original;
    corrupt[at] ^= 1;
    EXPECT_FALSE(decodeRemovalStageClaim(corrupt, decoded));
    EXPECT_EQ(decoded, claim);
  }
}
TEST_F(RemovalStageClaimTest, StagePathRequiresDurableClaimAndMatchingRetryDoesNotRename) {
  EXPECT_EQ(store.planStagePath(), nullptr);
  EXPECT_EQ(store.load(claim), RemovalStageClaimResult::Missing);
  EXPECT_EQ(store.planStagePath(), nullptr);
  ASSERT_EQ(store.persist(claim), RemovalStageClaimResult::Ok);
  const std::string target = store.markerPath();
  EXPECT_EQ(target, marker());
  EXPECT_EQ(inventory_hal_test::state.files.at(target), bytes(claim));
  const std::string stage = store.planStagePath();
  EXPECT_EQ(stage, target.substr(0, target.size() - 6) + ".stage");
  EXPECT_FALSE(inventory_hal_test::state.files.contains(stage));
  const auto renames = inventory_hal_test::state.renames;
  HalRemovalStageClaimStorage recovered(scratch);
  EXPECT_EQ(recovered.persist(claim), RemovalStageClaimResult::Ok);
  EXPECT_EQ(inventory_hal_test::state.renames, renames);
  EXPECT_EQ(recovered.load(claim), RemovalStageClaimResult::Ok);
  EXPECT_STREQ(recovered.planStagePath(), stage.c_str());
}
TEST_F(RemovalStageClaimTest, WriteSyncCloseRenameAndCorruptionFailuresRecoverWithoutPlanMutation) {
  for (unsigned fault = 0; fault < 6; ++fault) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = state.falseExists = true;
    const auto target = marker();
    const auto temporary = target + ".tmp";
    if (fault == 0) state.failWrite = true;
    if (fault == 1) state.failSyncPath = temporary;
    if (fault == 2) state.failClosePath = temporary;
    if (fault == 3) state.failRenameAfter = 1;
    if (fault == 4) state.corruptWritePath = temporary;
    if (fault == 5) state.failTruncate = true;
    {
      HalRemovalStageClaimStorage attempt(scratch);
      EXPECT_NE(attempt.persist(claim), RemovalStageClaimResult::Ok) << fault;
      EXPECT_EQ(attempt.planStagePath(), nullptr);
    }
    state.failWrite = state.failTruncate = false;
    state.failSyncPath.clear();
    state.failClosePath.clear();
    state.corruptWritePath.clear();
    state.failRenameAfter = 0;
    HalRemovalStageClaimStorage recovered(scratch);
    EXPECT_EQ(recovered.persist(claim), RemovalStageClaimResult::Ok) << fault;
    EXPECT_EQ(state.files.at(target), bytes(claim));
    EXPECT_FALSE(state.files.contains(recovered.planStagePath()));
  }
}
TEST_F(RemovalStageClaimTest, ForeignPublishedOrStagedMarkersArePreserved) {
  const auto target = marker();
  auto foreign = claim;
  foreign.request.owner.fill(99);
  for (const auto& path : {target, target + ".tmp"}) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = state.falseExists = true;
    state.files[path] = bytes(foreign);
    const auto before = state.files;
    EXPECT_EQ(store.persist(claim), RemovalStageClaimResult::Conflict);
    EXPECT_EQ(state.files, before);
    EXPECT_EQ(store.planStagePath(), nullptr);
  }
}
TEST_F(RemovalStageClaimTest, CorruptPublishedOversizedAndDirectoryCollisionsRemainUntouched) {
  const auto target = marker();
  auto& state = inventory_hal_test::state;
  state.files[target] = bytes(claim);
  state.files[target][0] ^= 1;
  auto before = state.files;
  EXPECT_EQ(store.persist(claim), RemovalStageClaimResult::Corrupt);
  EXPECT_EQ(state.files, before);
  state.files.erase(target);
  state.files[target + ".tmp"] = std::vector<uint8_t>(REMOVAL_STAGE_CLAIM_SIZE + 1, 99);
  before = state.files;
  EXPECT_EQ(store.persist(claim), RemovalStageClaimResult::IoError);
  EXPECT_EQ(state.files, before);
  state.files.clear();
  state.directories[target + ".tmp"] = {{"keep", false}};
  const auto directoryCount = state.directories.size();
  EXPECT_EQ(store.persist(claim), RemovalStageClaimResult::IoError);
  EXPECT_EQ(state.directories.size(), directoryCount);
  ASSERT_EQ(state.directories.at(target + ".tmp").size(), 1U);
  EXPECT_EQ(state.directories.at(target + ".tmp").front().name, "keep");
  EXPECT_EQ(store.planStagePath(), nullptr);
}
TEST_F(RemovalStageClaimTest, BoundedUnpublishedMarkerCanBeRebuiltButExistingPlanBytesAreRetained) {
  const auto target = marker();
  auto& state = inventory_hal_test::state;
  state.files[target + ".tmp"] = {1, 2, 3};
  const auto stage = target.substr(0, target.size() - 6) + ".stage";
  state.files[stage] = {9, 8, 7};
  EXPECT_EQ(store.persist(claim), RemovalStageClaimResult::Ok);
  EXPECT_EQ(state.files.at(stage), (std::vector<uint8_t>{9, 8, 7}));
}
TEST_F(RemovalStageClaimTest, LoadErrorsAndChangedClaimsDoNotExposeStageAuthority) {
  ASSERT_EQ(store.persist(claim), RemovalStageClaimResult::Ok);
  const auto target = marker();
  auto& state = inventory_hal_test::state;
  state.failSyncPath = target;
  EXPECT_EQ(store.load(claim), RemovalStageClaimResult::IoError);
  EXPECT_EQ(store.planStagePath(), nullptr);
  state.failSyncPath.clear();
  state.readErrorPath = target;
  EXPECT_EQ(store.load(claim), RemovalStageClaimResult::IoError);
  state.readErrorPath.clear();
  for (unsigned field = 0; field < 7; ++field) {
    auto changed = claim;
    if (field == 0) changed.request.transaction[0] ^= 1;
    if (field == 1) changed.request.owner[0] ^= 1;
    if (field == 2) changed.request.generation[0] ^= 1;
    if (field == 3) changed.request.manifest.contentHash[0] ^= 1;
    if (field == 4) ++changed.request.manifest.length;
    if (field == 5) changed.request.manifest.formatVersion = 0;
    if (field == 6) ++changed.inventoryRevision;
    EXPECT_EQ(store.load(changed), RemovalStageClaimResult::Missing);
    EXPECT_EQ(store.planStagePath(), nullptr);
  }
  EXPECT_EQ(state.files.at(target), bytes(claim));
}

TEST_F(RemovalStageClaimTest, SealedTemporaryMarkerPublishesWithoutRewritingBytes) {
  const auto target = marker();
  auto& state = inventory_hal_test::state;
  state.files[target + ".tmp"] = bytes(claim);
  state.failWrite = true;
  EXPECT_EQ(store.persist(claim), RemovalStageClaimResult::Ok);
  EXPECT_EQ(state.files.at(target), bytes(claim));
  EXPECT_FALSE(state.files.contains(target + ".tmp"));
}
TEST_F(RemovalStageClaimTest, InvalidClaimsAndSmallScratchNeverPublishAuthority) {
  auto invalid = claim;
  invalid.inventoryRevision = 0;
  scratch.fill(99);
  EXPECT_EQ(encodeRemovalStageClaim(invalid, scratch), 0U);
  EXPECT_EQ(scratch.front(), 99);
  EXPECT_EQ(store.persist(invalid), RemovalStageClaimResult::Invalid);
  EXPECT_EQ(store.planStagePath(), nullptr);
  HalRemovalStageClaimStorage small{std::span(scratch).first(REMOVAL_STAGE_CLAIM_SIZE - 1)};
  EXPECT_EQ(small.persist(claim), RemovalStageClaimResult::Invalid);
  EXPECT_TRUE(inventory_hal_test::state.files.empty());
  auto trailing = bytes(claim);
  trailing.push_back(0);
  RemovalStageClaim output = claim;
  EXPECT_FALSE(decodeRemovalStageClaim(trailing, output));
  EXPECT_EQ(output, claim);
}

TEST_F(RemovalStageClaimTest, IndependentFixtureMatchesCodecAndDigestAddressedMarker) {
  std::ifstream input(std::string(COMPANION_FIXTURE_DIR) + "/RemovalStageClaim.json");
  const std::string json{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  const auto begin = json.find("\"binaryHex\": \"");
  ASSERT_NE(begin, std::string::npos);
  const auto end = json.find('"', begin + 14);
  ASSERT_NE(end, std::string::npos);
  std::vector<uint8_t> golden;
  golden.reserve((end - begin - 14) / 2);
  for (size_t at = begin + 14; at < end; at += 2)
    golden.push_back(static_cast<uint8_t>(std::stoul(json.substr(at, 2), nullptr, 16)));
  ASSERT_EQ(golden.size(), REMOVAL_STAGE_CLAIM_SIZE);
  claim.request.manifest.length = 1234;
  EXPECT_EQ(bytes(claim), golden);
  const auto hash = json.find("\"sha256\": \"");
  ASSERT_NE(hash, std::string::npos);
  const auto expectedPath = "/.crosspoint/companion/removal-multi-" + json.substr(hash + 11, 64) + ".owner";
  ASSERT_EQ(store.persist(claim), RemovalStageClaimResult::Ok);
  EXPECT_STREQ(store.markerPath(), expectedPath.c_str());
}
