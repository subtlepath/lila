#include <gtest/gtest.h>

#include <array>

#include "CompanionWifiHandoffCommands.h"
#include "HalCompanionWifiMessages.h"

using namespace companion;
namespace {
struct Fixture {
  WifiHandoffOffer offer;
  std::array<uint8_t, SESSION_WORKSPACE_SIZE> workspace{};
  HalCompanionWifiMessages reader;
  HalCompanionWifiCipher apple;
  uint64_t now = 0;
  unsigned calls = 0;
  unsigned mode = 0;
  Fixture() {
    offer.reader.fill(1);
    offer.storageGeneration.fill(2);
    offer.installation.fill(3);
    offer.transaction.fill(4);
    offer.session.fill(5);
    offer.key.fill(6);
    offer.port = 8080;
    offer.lifetimeSeconds = 30;
    std::fill(workspace.begin() + TRANSFER_OFFSET, workspace.end(), 7);
  }
  static uint64_t clock(void* context) { return static_cast<Fixture*>(context)->now; }
  bool begin() {
    return reader.begin(offer, offer.reader, offer.storageGeneration, offer.installation, offer.transaction, workspace,
                        0, {this, clock}) &&
           apple.begin(offer.key, offer.session, WifiMessageDirection::AppleToReader);
  }
  static WifiDispatchReply dispatch(void* context, const FrameView& request, const Identity& owner,
                                    std::span<uint8_t> payload) {
    auto& self = *static_cast<Fixture*>(context);
    ++self.calls;
    EXPECT_EQ(owner, self.offer.installation);
    if (self.mode == 1) self.now += 30000;
    if (self.mode == 2) return {Command::Discover, 0};
    if (self.mode == 3) return {request.command, MAX_CONTROL_PAYLOAD + 1};
    if (self.mode == 4) {
      const auto busy = self.reader.process(48, {context, dispatch});
      EXPECT_EQ(busy.result, WifiMessageResult::Busy);
      EXPECT_TRUE(self.reader.requestBuffer().empty());
    }
    if (self.mode == 5) {
      self.reader.end();
      return {request.command, 0};
    }
    if (self.mode >= 6) {
      Identity wrong = self.offer.session;
      wrong[0] ^= 1;
      EXPECT_FALSE(self.reader.acquireWorkspace(wrong));
      EXPECT_TRUE(self.reader.acquireWorkspace(self.offer.session));
      EXPECT_TRUE(self.reader.workspaceOwned(self.offer.session));
      EXPECT_FALSE(self.reader.workspaceOwned(wrong));
      EXPECT_FALSE(self.reader.acquireWorkspace(self.offer.session));
      EXPECT_FALSE(self.reader.releaseWorkspace(wrong));
      EXPECT_TRUE(self.reader.requestBuffer().empty());
      EXPECT_EQ(self.reader.process(48, {context, dispatch}).result, WifiMessageResult::Busy);
      std::fill(self.workspace.begin(), self.workspace.end(), 85);
      EXPECT_FALSE(self.reader.begin(self.offer, self.offer.reader, self.offer.storageGeneration,
                                     self.offer.installation, self.offer.transaction, self.workspace, 0,
                                     {context, clock}));
      if (self.mode == 7) self.reader.end();
      if (self.mode == 8) {
        self.now += 30000;
        EXPECT_FALSE(self.reader.workspaceOwned(self.offer.session));
        EXPECT_FALSE(self.reader.pollDeadline());
      }
      if (self.mode == 7 || self.mode == 8) {
        EXPECT_FALSE(self.reader.workspaceOwned(self.offer.session));
        EXPECT_TRUE(std::all_of(self.workspace.begin(), self.workspace.end(), [](uint8_t b) { return b == 85; }));
      }
      if (self.mode == 9) return {request.command, 0};
      EXPECT_TRUE(self.reader.releaseWorkspace(self.offer.session));
      EXPECT_FALSE(self.reader.workspaceOwned(self.offer.session));
      EXPECT_FALSE(self.reader.releaseWorkspace(self.offer.session));
      if (self.mode == 7 || self.mode == 8) {
        EXPECT_TRUE(std::all_of(self.workspace.begin(), self.workspace.begin() + TRANSFER_OFFSET,
                                [](uint8_t b) { return b == 0; }));
        return {request.command, 0};
      }
    }
    payload[0] = 0;
    payload[1] = 8;
    return {request.command, 2};
  }
  size_t request(Command command = Command::TransferStatus, bool foreign = false) {
    std::array<uint8_t, FRAME_HEADER_SIZE + MAX_CONTROL_PAYLOAD> plain{};
    Identity transaction = offer.transaction;
    if (foreign) transaction[0] ^= 1;
    FrameView frame{command, false, 19, transaction};
    const auto count = encodeFrame(frame, plain);
    size_t length = 0;
    EXPECT_EQ(apple.seal(std::span(plain).first(count), reader.requestBuffer(), length), WifiCipherResult::Ok);
    return length;
  }
  WifiMessageReply process(size_t length) { return reader.process(length, {this, dispatch}); }
  void checkCleared() {
    EXPECT_TRUE(reader.requestBuffer().empty());
    EXPECT_TRUE(
        std::all_of(workspace.begin(), workspace.begin() + TRANSFER_OFFSET, [](uint8_t byte) { return byte == 0; }));
    EXPECT_TRUE(
        std::all_of(workspace.begin() + TRANSFER_OFFSET, workspace.end(), [](uint8_t byte) { return byte == 7; }));
  }
};
}  // namespace
TEST(HalCompanionWifiMessagesTest, FinishAcknowledgementDoesNotDispatchOrEraseBeforeHttpDrains) {
  Fixture fixture;
  ASSERT_TRUE(fixture.begin());
  WifiHandoffSessionCommand command{WifiHandoffAction::Cancel, fixture.offer.transaction, fixture.offer.session};
  std::array<uint8_t, WIFI_HANDOFF_SESSION_COMMAND_SIZE> payload{};
  ASSERT_EQ(encodeWifiHandoffSessionCommand(command, payload), payload.size());
  std::array<uint8_t, 128> plain{};
  const auto frameLength = encodeFrame({Command::WifiHandoff, false, 29, payload}, plain);
  size_t length = 0;
  ASSERT_EQ(fixture.apple.seal(std::span(plain).first(frameLength), fixture.reader.requestBuffer(), length),
            WifiCipherResult::Ok);
  const auto result = fixture.process(length);
  ASSERT_EQ(result.result, WifiMessageResult::Ok);
  EXPECT_TRUE(fixture.reader.finishing());
  EXPECT_EQ(fixture.calls, 0u);
  size_t replyLength = 0;
  ASSERT_EQ(fixture.apple.open(result.bytes, plain, replyLength), WifiCipherResult::Ok);
  FrameView reply;
  ASSERT_EQ(decodeFrame(std::span(plain).first(replyLength), true, reply), FrameError::None);
  EXPECT_EQ(reply.command, Command::WifiHandoff);
  EXPECT_EQ(reply.requestId, 29u);
  ASSERT_EQ(reply.payload.size(), 1u);
  EXPECT_EQ(reply.payload[0], 0u);
  EXPECT_TRUE(fixture.reader.pollDeadline());
  fixture.reader.end();
  fixture.checkCleared();
}
TEST(HalCompanionWifiMessagesTest, ForeignOrMalformedFinishCannotDispatchAndClearsSession) {
  for (unsigned mode = 0; mode < 7; ++mode) {
    SCOPED_TRACE(mode);
    Fixture fixture;
    ASSERT_TRUE(fixture.begin());
    WifiHandoffSessionCommand command{WifiHandoffAction::Cancel, fixture.offer.transaction, fixture.offer.session};
    if (mode == 0) command.transaction[0] ^= 1;
    if (mode == 1) command.session[0] ^= 1;
    if (mode == 2) command.action = WifiHandoffAction::Activate;
    std::array<uint8_t, WIFI_HANDOFF_SESSION_COMMAND_SIZE> payload{};
    ASSERT_EQ(encodeWifiHandoffSessionCommand(command, payload), payload.size());
    if (mode == 3) payload[0] = 2;
    if (mode == 4) payload[1] = 4;
    std::array<uint8_t, 128> plain{};
    const auto body =
        mode == 5 ? std::span<const uint8_t>(payload).first(payload.size() - 1) : std::span<const uint8_t>(payload);
    const auto frameLength = encodeFrame({Command::WifiHandoff, mode == 6, 29, body}, plain);
    size_t length = 0;
    ASSERT_EQ(fixture.apple.seal(std::span(plain).first(frameLength), fixture.reader.requestBuffer(), length),
              WifiCipherResult::Ok);
    EXPECT_EQ(fixture.process(length).result, WifiMessageResult::InvalidRequest);
    EXPECT_EQ(fixture.calls, 0u);
    EXPECT_FALSE(fixture.reader.finishing());
    fixture.checkCleared();
  }
}
TEST(HalCompanionWifiMessagesTest, AuthenticatedDispatchAndIdleRenewalPreserveTransferScratch) {
  Fixture fixture;
  ASSERT_TRUE(fixture.begin());
  for (const uint64_t time : {29000u, 58000u}) {
    fixture.now = time;
    const auto reply = fixture.process(fixture.request());
    ASSERT_EQ(reply.result, WifiMessageResult::Ok);
    std::array<uint8_t, HalCompanionWifiCipher::MAX_PAYLOAD> plain{};
    size_t length = 0;
    ASSERT_EQ(fixture.apple.open(reply.bytes, plain, length), WifiCipherResult::Ok);
    FrameView response;
    ASSERT_EQ(decodeFrame(std::span(plain).first(length), true, response), FrameError::None);
    EXPECT_TRUE(response.response);
    EXPECT_EQ(response.command, Command::TransferStatus);
    EXPECT_EQ(response.requestId, 19u);
    ASSERT_EQ(response.payload.size(), 2u);
    EXPECT_EQ(response.payload[1], 8);
  }
  EXPECT_EQ(fixture.calls, 2u);
  const auto length = fixture.request();
  fixture.now = 88000;
  EXPECT_EQ(fixture.process(length).result, WifiMessageResult::Expired);
  EXPECT_EQ(fixture.calls, 2u);
  fixture.checkCleared();
}
TEST(HalCompanionWifiMessagesTest, TamperingForeignTransactionAndUnsupportedCommandNeverDispatch) {
  for (unsigned mode = 0; mode < 3; ++mode) {
    Fixture fixture;
    ASSERT_TRUE(fixture.begin());
    const auto length = fixture.request(mode == 2 ? Command::Inventory : Command::TransferStatus, mode == 1);
    if (mode == 0) fixture.reader.requestBuffer()[length - 1] ^= 1;
    EXPECT_EQ(fixture.process(length).result,
              mode == 0 ? WifiMessageResult::InvalidMessage : WifiMessageResult::InvalidRequest);
    EXPECT_EQ(fixture.calls, 0u);
    fixture.checkCleared();
    EXPECT_EQ(fixture.process(length).result, WifiMessageResult::Inactive);
  }
}
TEST(HalCompanionWifiMessagesTest, InitialExpiryAndDispatchFailuresEndWithoutReply) {
  for (unsigned mode = 0; mode <= 5; ++mode) {
    Fixture fixture;
    ASSERT_TRUE(fixture.begin());
    const auto length = fixture.request();
    fixture.mode = mode;
    if (mode == 0) fixture.now = 30000;
    const auto reply = fixture.process(length);
    if (mode == 4) {
      EXPECT_EQ(reply.result, WifiMessageResult::Ok);
      fixture.reader.end();
    } else {
      EXPECT_EQ(reply.result, mode <= 1   ? WifiMessageResult::Expired
                              : mode == 5 ? WifiMessageResult::Inactive
                                          : WifiMessageResult::DispatchError);
      EXPECT_TRUE(reply.bytes.empty());
    }
    EXPECT_EQ(fixture.calls, mode == 0 ? 0u : 1u);
    fixture.checkCleared();
  }
}
TEST(HalCompanionWifiMessagesTest, BindingWorkspaceClockAndMessageBoundsAreChecked) {
  Fixture fixture;
  Identity foreign{};
  foreign.fill(9);
  for (unsigned at = 0; at < 4; ++at) {
    EXPECT_FALSE(fixture.reader.begin(
        fixture.offer, at == 0 ? foreign : fixture.offer.reader, at == 1 ? foreign : fixture.offer.storageGeneration,
        at == 2 ? foreign : fixture.offer.installation, at == 3 ? foreign : fixture.offer.transaction,
        fixture.workspace, 0, {&fixture, Fixture::clock}));
  }
  EXPECT_FALSE(fixture.reader.begin(fixture.offer, fixture.offer.reader, fixture.offer.storageGeneration,
                                    fixture.offer.installation, fixture.offer.transaction,
                                    std::span(fixture.workspace).first(SESSION_WORKSPACE_SIZE - 1), 0,
                                    {&fixture, Fixture::clock}));
  EXPECT_FALSE(fixture.reader.begin(fixture.offer, fixture.offer.reader, fixture.offer.storageGeneration,
                                    fixture.offer.installation, fixture.offer.transaction, fixture.workspace, 0, {}));
  fixture.now = 30000;
  EXPECT_FALSE(fixture.begin());
  fixture.now = 0;
  ASSERT_TRUE(fixture.begin());
  EXPECT_EQ(fixture.process(HalCompanionWifiMessages::MAX_MESSAGE_SIZE + 1).result, WifiMessageResult::InvalidMessage);
  EXPECT_EQ(fixture.calls, 0u);
  fixture.checkCleared();
}

TEST(HalCompanionWifiMessagesTest, ReplayCannotDispatchAgainAndMaximumChunkFitsWorkspace) {
  Fixture fixture;
  ASSERT_TRUE(fixture.begin());
  std::array<uint8_t, MAX_CONTROL_PAYLOAD> payload{};
  std::copy(fixture.offer.transaction.begin(), fixture.offer.transaction.end(), payload.begin());
  std::array<uint8_t, FRAME_HEADER_SIZE + MAX_CONTROL_PAYLOAD> plain{};
  FrameView frame{Command::TransferChunk, false, 9, payload};
  const auto count = encodeFrame(frame, plain);
  size_t length = 0;
  ASSERT_EQ(fixture.apple.seal(std::span(plain).first(count), fixture.reader.requestBuffer(), length),
            WifiCipherResult::Ok);
  ASSERT_EQ(length, HalCompanionWifiMessages::MAX_MESSAGE_SIZE);
  std::array<uint8_t, HalCompanionWifiMessages::MAX_MESSAGE_SIZE> repeated{};
  std::copy_n(fixture.reader.requestBuffer().begin(), length, repeated.begin());
  ASSERT_EQ(fixture.process(length).result, WifiMessageResult::Ok);
  EXPECT_EQ(fixture.calls, 1u);
  std::copy(repeated.begin(), repeated.end(), fixture.reader.requestBuffer().begin());
  EXPECT_EQ(fixture.process(length).result, WifiMessageResult::InvalidMessage);
  EXPECT_EQ(fixture.calls, 1u);
  fixture.checkCleared();
}

TEST(HalCompanionWifiMessagesTest, FullWorkspaceLeaseIsDispatchBoundAndRepliesSurviveReusingRequestBytes) {
  Fixture fixture;
  ASSERT_TRUE(fixture.begin());
  EXPECT_FALSE(fixture.reader.acquireWorkspace(fixture.offer.session));
  EXPECT_FALSE(fixture.reader.workspaceOwned(fixture.offer.session));
  EXPECT_FALSE(fixture.reader.releaseWorkspace(fixture.offer.session));
  fixture.mode = 6;
  const auto reply = fixture.process(fixture.request());
  ASSERT_EQ(reply.result, WifiMessageResult::Ok);
  EXPECT_EQ(fixture.calls, 1u);
  EXPECT_FALSE(fixture.reader.workspaceOwned(fixture.offer.session));
  EXPECT_FALSE(fixture.reader.acquireWorkspace(fixture.offer.session));
  std::array<uint8_t, HalCompanionWifiCipher::MAX_PAYLOAD> plain{};
  size_t length = 0;
  ASSERT_EQ(fixture.apple.open(reply.bytes, plain, length), WifiCipherResult::Ok);
  FrameView response;
  ASSERT_EQ(decodeFrame(std::span(plain).first(length), true, response), FrameError::None);
  EXPECT_EQ(response.command, Command::TransferStatus);
  EXPECT_EQ(response.requestId, 19u);
  ASSERT_EQ(response.payload.size(), 2u);
  EXPECT_EQ(response.payload[1], 8u);
  EXPECT_TRUE(std::all_of(fixture.workspace.begin() + TRANSFER_OFFSET, fixture.workspace.end(),
                          [](uint8_t b) { return b == 85; }));
}

TEST(HalCompanionWifiMessagesTest, TeardownAndExpiryRevokePermissionWithoutClearingLoanedBytes) {
  for (const unsigned mode : {7u, 8u}) {
    SCOPED_TRACE(mode);
    Fixture fixture;
    ASSERT_TRUE(fixture.begin());
    fixture.mode = mode;
    EXPECT_EQ(fixture.process(fixture.request()).result, WifiMessageResult::Inactive);
    EXPECT_FALSE(fixture.reader.workspaceOwned(fixture.offer.session));
    EXPECT_FALSE(fixture.reader.releaseWorkspace(fixture.offer.session));
    EXPECT_TRUE(fixture.reader.requestBuffer().empty());
    EXPECT_TRUE(std::all_of(fixture.workspace.begin(), fixture.workspace.begin() + TRANSFER_OFFSET,
                            [](uint8_t b) { return b == 0; }));
    EXPECT_TRUE(std::all_of(fixture.workspace.begin() + TRANSFER_OFFSET, fixture.workspace.end(),
                            [](uint8_t b) { return b == 85; }));
  }
}

TEST(HalCompanionWifiMessagesTest, AnUnreleasedDispatchLoanEndsTheSessionBeforeEncoding) {
  Fixture fixture;
  ASSERT_TRUE(fixture.begin());
  fixture.mode = 9;
  const auto reply = fixture.process(fixture.request());
  EXPECT_EQ(reply.result, WifiMessageResult::DispatchError);
  EXPECT_TRUE(reply.bytes.empty());
  EXPECT_FALSE(fixture.reader.workspaceOwned(fixture.offer.session));
  EXPECT_FALSE(fixture.reader.releaseWorkspace(fixture.offer.session));
  EXPECT_TRUE(fixture.reader.requestBuffer().empty());
  EXPECT_TRUE(std::all_of(fixture.workspace.begin(), fixture.workspace.begin() + TRANSFER_OFFSET,
                          [](uint8_t b) { return b == 0; }));
}
