#include <gtest/gtest.h>

#include <array>
#include <fstream>

#include "lib/Companion/CompanionFirmwareInstallRequest.h"
TEST(CompanionFirmwareInstallRequest, SharedFixtureAndMalformedRequests) {
  std::array<uint8_t, companion::FIRMWARE_INSTALL_REQUEST_SIZE> bytes{}, encoded{};
  std::ifstream input(FIRMWARE_INSTALL_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()));
  ASSERT_EQ(input.peek(), std::char_traits<char>::eof());
  companion::FirmwareInstallRequest request;
  ASSERT_TRUE(companion::decodeFirmwareInstallRequest(bytes, request));
  ASSERT_TRUE(companion::encodeFirmwareInstallRequest(request, encoded));
  EXPECT_EQ(bytes, encoded);
  for (size_t offset :
       {size_t{3}, size_t{84}, size_t{88}, size_t{90}, size_t{91}, size_t{92}, size_t{94}, size_t{95}}) {
    auto bad = bytes;
    bad[offset] = 0xff;
    auto decoded = request;
    EXPECT_FALSE(companion::decodeFirmwareInstallRequest(bad, decoded));
    EXPECT_EQ(decoded, request);
  }
  auto bad = request;
  bad.minimumBattery = 29;
  EXPECT_FALSE(companion::encodeFirmwareInstallRequest(bad, encoded));
  EXPECT_EQ(bytes, encoded);
}
TEST(CompanionFirmwareInstallRequest, BindsCommittedTransactionAndFreshCompatibility) {
  using namespace companion;
  FirmwareInstallRequest request;
  request.generation.fill(1);
  request.transaction.fill(2);
  request.hash.fill(3);
  request.length = 65536;
  request.partitionBytes = 0x900000;
  request.chip = 5;
  request.minimumSchema = request.maximumSchema = 1;
  request.journalVersions = 3;
  FirmwareReaderInfo info;
  info.generation = request.generation;
  info.runningBuild.fill(4);
  info.battery = 60;
  info.partitionBytes = request.partitionBytes;
  info.chip = 5;
  info.stateSchema = 1;
  info.journalVersions = 3;
  ASSERT_TRUE(firmwareInstallCompatible(request, info));
  for (int field = 0; field < 7; ++field) {
    auto changed = info;
    switch (field) {
      case 0:
        changed.battery = 29;
        break;
      case 1:
        changed.generation[0] ^= 1;
        break;
      case 2:
        changed.partitionBytes -= 4096;
        break;
      case 3:
        changed.chip = 9;
        break;
      case 4:
        changed.stateSchema = 2;
        break;
      case 5:
        changed.journalVersions |= 4;
        break;
      case 6:
        changed.minimumProtocol = changed.maximumProtocol = 2;
        break;
    }
    EXPECT_FALSE(firmwareInstallCompatible(request, changed));
  }
  TransferState state;
  state.transaction = request.transaction;
  state.storageGeneration = request.generation;
  state.contentHash = request.hash;
  state.length = state.durableOffset = request.length;
  state.owner.fill(5);
  state.phase = TransferPhase::Committed;
  ContentManifest manifest;
  manifest.kind = ContentKind::Firmware;
  manifest.formatVersion = 1;
  manifest.contentHash = request.hash;
  manifest.length = request.length;
  ASSERT_TRUE(firmwareInstallMatchesTransfer(request, state.owner, state, manifest, FIRMWARE_STAGE_DESTINATION));
  for (int field = 0; field < 7; ++field) {
    auto changed = state;
    switch (field) {
      case 0:
        changed.phase = TransferPhase::Installing;
        break;
      case 1:
        changed.transaction[0] ^= 1;
        break;
      case 2:
        changed.owner[0] ^= 1;
        break;
      case 3:
        changed.storageGeneration[0] ^= 1;
        break;
      case 4:
        changed.contentHash[0] ^= 1;
        break;
      case 5:
        --changed.length;
        break;
      case 6:
        --changed.durableOffset;
        break;
    }
    EXPECT_FALSE(firmwareInstallMatchesTransfer(request, state.owner, changed, manifest, FIRMWARE_STAGE_DESTINATION));
  }
  EXPECT_FALSE(firmwareInstallMatchesTransfer(request, state.owner, state, manifest, "/other.bin"));
  manifest.kind = ContentKind::Epub;
  EXPECT_FALSE(firmwareInstallMatchesTransfer(request, state.owner, state, manifest, FIRMWARE_STAGE_DESTINATION));
}
