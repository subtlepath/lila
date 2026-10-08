#include <gtest/gtest.h>

#include <string>

#include "lib/Companion/CompanionWifiHandoffLease.h"
using namespace companion;
namespace {
struct Fixture {
  WifiNetworkOffer source;
  WifiHandoffPrepare request;
  TransferState transfer;
  WifiHandoffLease lease;
  Fixture() {
    source.offer.reader.fill(1);
    source.offer.storageGeneration.fill(2);
    source.offer.installation.fill(3);
    source.offer.transaction.fill(4);
    source.offer.session.fill(5);
    source.offer.key.fill(6);
    source.offer.port = 8080;
    source.offer.lifetimeSeconds = 30;
    source.network = {WifiNetworkMode::Hotspot, "lila-test", "test-password"};
    request.mode = source.network.mode;
    request.transaction = source.offer.transaction;
    transfer.transaction = source.offer.transaction;
    transfer.owner = source.offer.installation;
    transfer.storageGeneration = source.offer.storageGeneration;
    transfer.length = 2000000;
    transfer.durableOffset = 100;
  }
  WifiHandoffLeaseResult prepare(uint64_t token = 7, uint64_t now = 1000) {
    return lease.prepare(source, request, transfer, source.offer.reader, source.offer.storageGeneration,
                         source.offer.installation, token, now);
  }
  WifiHandoffSessionCommand command(WifiHandoffAction action = WifiHandoffAction::Activate) {
    return {action, source.offer.transaction, source.offer.session};
  }
  WifiHandoffLeaseResult apply(WifiHandoffSessionCommand command, uint64_t token = 7, uint64_t now = 1000) {
    return lease.apply(command, token, source.offer.installation, now);
  }
};
struct Sink {
  WifiHandoffLease* lease = nullptr;
  bool success = true;
  unsigned calls = 0;
  const WifiHandoffOffer* borrowed = nullptr;
  const char* password = nullptr;
  static bool accept(void* context, const WifiActivationView& view) {
    auto& self = *static_cast<Sink*>(context);
    ++self.calls;
    self.borrowed = &view.offer;
    self.password = view.password;
    EXPECT_EQ(self.lease->phase(), WifiHandoffLeasePhase::Consuming);
    EXPECT_EQ(self.lease->consume(1000, {context, accept}), WifiHandoffLeaseResult::Busy);
    EXPECT_STREQ(view.ssid, "lila-test");
    EXPECT_STREQ(view.password, "test-password");
    EXPECT_EQ(view.mode, WifiNetworkMode::Hotspot);
    EXPECT_EQ(view.receivedAtMilliseconds, 1000u);
    EXPECT_EQ(view.offer.key.front(), 6);
    return self.success;
  }
};
}  // namespace
TEST(CompanionWifiHandoffLeaseTest, PrepareOwnsNetworkStringsAndPreservesReceiptTime) {
  Fixture fixture;
  std::string ssid = "lila-test", password = "test-password";
  fixture.source.network.ssid = ssid;
  fixture.source.network.password = password;
  ASSERT_EQ(fixture.prepare(), WifiHandoffLeaseResult::Ok);
  ssid.assign(ssid.size(), 'x');
  password.assign(password.size(), 'x');
  std::array<uint8_t, WIFI_NETWORK_OFFER_MAX_SIZE> encoded{};
  const auto length = fixture.lease.encode(encoded, 1000);
  ASSERT_NE(length, 0u);
  WifiNetworkOffer decoded;
  ASSERT_TRUE(decodeWifiNetworkOffer(std::span(encoded).first(length), decoded));
  EXPECT_EQ(decoded.network.ssid, "lila-test");
  EXPECT_EQ(decoded.network.password, "test-password");
  EXPECT_EQ(fixture.prepare(), WifiHandoffLeaseResult::Busy);
  ASSERT_EQ(fixture.apply(fixture.command()), WifiHandoffLeaseResult::Ok);
  EXPECT_EQ(fixture.apply(fixture.command()), WifiHandoffLeaseResult::Ok);
  EXPECT_TRUE(fixture.lease.reconcile(0, {}, 2000));
  Sink sink{&fixture.lease};
  EXPECT_EQ(fixture.lease.consume(2000, {&sink, Sink::accept}), WifiHandoffLeaseResult::Ok);
  EXPECT_EQ(sink.calls, 1u);
  EXPECT_EQ(fixture.lease.phase(), WifiHandoffLeasePhase::Empty);
  EXPECT_EQ(sink.borrowed->key, Digest{});
  EXPECT_EQ(sink.borrowed->session, Identity{});
  EXPECT_TRUE(std::all_of(sink.password, sink.password + 64, [](char byte) { return byte == 0; }));
  EXPECT_EQ(fixture.lease.consume(2000, {&sink, Sink::accept}), WifiHandoffLeaseResult::NoOffer);
  EXPECT_EQ(sink.calls, 1u);
}
TEST(CompanionWifiHandoffLeaseTest, SetupFailureConsumesSecretsAndCancelPreventsSetup) {
  for (const bool cancel : {false, true}) {
    Fixture fixture;
    ASSERT_EQ(fixture.prepare(), WifiHandoffLeaseResult::Ok);
    ASSERT_EQ(fixture.apply(fixture.command()), WifiHandoffLeaseResult::Ok);
    Sink sink{&fixture.lease, false};
    if (cancel) {
      ASSERT_EQ(fixture.apply(fixture.command(WifiHandoffAction::Cancel)), WifiHandoffLeaseResult::Ok);
    }
    EXPECT_EQ(fixture.lease.consume(1000, {&sink, Sink::accept}),
              cancel ? WifiHandoffLeaseResult::NoOffer : WifiHandoffLeaseResult::SetupFailed);
    EXPECT_EQ(sink.calls, cancel ? 0u : 1u);
    if (!cancel) {
      EXPECT_EQ(sink.borrowed->key, Digest{});
      EXPECT_EQ(sink.password[0], 0);
    }
    EXPECT_EQ(fixture.lease.phase(), WifiHandoffLeasePhase::Empty);
  }
}
TEST(CompanionWifiHandoffLeaseTest, ForeignCommandsDoNotDisturbPreparedOffer) {
  Fixture fixture;
  ASSERT_EQ(fixture.prepare(), WifiHandoffLeaseResult::Ok);
  EXPECT_EQ(fixture.apply(fixture.command(), 8), WifiHandoffLeaseResult::Unauthorized);
  auto wrong = fixture.command();
  wrong.session.fill(9);
  EXPECT_EQ(fixture.apply(wrong), WifiHandoffLeaseResult::Invalid);
  wrong = fixture.command();
  wrong.transaction.fill(9);
  EXPECT_EQ(fixture.apply(wrong), WifiHandoffLeaseResult::Invalid);
  Identity owner{};
  owner.fill(9);
  EXPECT_EQ(fixture.lease.apply(fixture.command(), 7, owner, 1000), WifiHandoffLeaseResult::Unauthorized);
  EXPECT_EQ(fixture.lease.phase(), WifiHandoffLeasePhase::Prepared);
  Sink sink{&fixture.lease};
  EXPECT_EQ(fixture.lease.consume(1000, {&sink, Sink::accept}), WifiHandoffLeaseResult::NoOffer);
  EXPECT_EQ(sink.calls, 0u);
}
TEST(CompanionWifiHandoffLeaseTest, ExpiryDisconnectOwnerChangeAndBackwardClockClearOffer) {
  for (unsigned mode = 0; mode < 5; ++mode) {
    Fixture fixture;
    ASSERT_EQ(fixture.prepare(), WifiHandoffLeaseResult::Ok);
    if (mode >= 3) {
      ASSERT_EQ(fixture.apply(fixture.command()), WifiHandoffLeaseResult::Ok);
    }
    const uint64_t now = mode == 0 || mode == 3 ? 31000 : mode == 4 ? 999 : 1000;
    const uint64_t token = mode == 1 ? 0 : 7;
    Identity owner = fixture.source.offer.installation;
    if (mode == 2) owner.fill(9);
    EXPECT_FALSE(fixture.lease.reconcile(token, owner, now));
    EXPECT_EQ(fixture.lease.phase(), WifiHandoffLeasePhase::Empty);
    EXPECT_EQ(fixture.apply(fixture.command()), WifiHandoffLeaseResult::NoOffer);
  }
}
TEST(CompanionWifiHandoffLeaseTest, PrepareRejectsWrongBindingsAndUnrecoverableTransferPhases) {
  for (unsigned mode = 0; mode < 8; ++mode) {
    Fixture fixture;
    if (mode == 0) fixture.source.offer.reader.fill(0);
    if (mode == 1) fixture.transfer.owner.fill(9);
    if (mode == 2) fixture.transfer.storageGeneration.fill(9);
    if (mode == 3) fixture.request.transaction.fill(9);
    if (mode == 4) fixture.request.mode = WifiNetworkMode::SavedNetwork;
    if (mode == 5) fixture.transfer.phase = TransferPhase::Installing;
    if (mode == 6) fixture.transfer.phase = TransferPhase::Committed;
    if (mode == 7) fixture.transfer.phase = TransferPhase::Verified;
    EXPECT_EQ(fixture.prepare(), WifiHandoffLeaseResult::Invalid);
    EXPECT_EQ(fixture.lease.phase(), WifiHandoffLeasePhase::Empty);
  }
  Fixture fixture;
  EXPECT_EQ(fixture.prepare(0), WifiHandoffLeaseResult::Unauthorized);
  fixture.transfer.phase = TransferPhase::Verified;
  fixture.transfer.durableOffset = fixture.transfer.length;
  EXPECT_EQ(fixture.prepare(), WifiHandoffLeaseResult::Ok);
}

TEST(CompanionWifiHandoffLeaseTest, ValidForeignOfferBindingsAreRejectedAgainstActualReaderContext) {
  for (unsigned at = 0; at < 4; ++at) {
    Fixture fixture;
    const auto reader = fixture.source.offer.reader;
    const auto generation = fixture.source.offer.storageGeneration;
    const auto installation = fixture.source.offer.installation;
    if (at == 0) fixture.source.offer.reader.fill(9);
    if (at == 1) fixture.source.offer.storageGeneration.fill(9);
    if (at == 2) fixture.source.offer.installation.fill(9);
    if (at == 3) fixture.source.offer.transaction.fill(9);
    ASSERT_TRUE(validWifiHandoffOffer(fixture.source.offer));
    EXPECT_EQ(fixture.lease.prepare(fixture.source, fixture.request, fixture.transfer, reader, generation, installation,
                                    7, 1000),
              WifiHandoffLeaseResult::Invalid);
    EXPECT_EQ(fixture.lease.phase(), WifiHandoffLeasePhase::Empty);
  }
}

TEST(CompanionWifiHandoffLeaseTest, JournalLeaseRequiresBoundDeclarationAndIncompleteDurablePrefix) {
  Fixture fixture;
  JournalMergeIntent merge;
  merge.generation = fixture.source.offer.storageGeneration;
  merge.owner = fixture.source.offer.installation;
  merge.transaction = fixture.source.offer.transaction;
  merge.previous = {2, 512, {}};
  merge.previous.frontier.fill(8);
  merge.merged = {8, 1024, {}};
  merge.merged.frontier.fill(9);
  auto prepare = [&](uint32_t count, uint64_t token = 7) {
    return fixture.lease.prepareJournal(fixture.source, fixture.request, merge, count, fixture.source.offer.reader,
                                        fixture.source.offer.storageGeneration, fixture.source.offer.installation,
                                        token, 1000);
  };
  EXPECT_EQ(prepare(2, 0), WifiHandoffLeaseResult::Unauthorized);
  EXPECT_EQ(prepare(1), WifiHandoffLeaseResult::Invalid);
  EXPECT_EQ(prepare(8), WifiHandoffLeaseResult::Invalid);
  merge.owner.front() ^= 1;
  EXPECT_EQ(prepare(2), WifiHandoffLeaseResult::Invalid);
  merge.owner.front() ^= 1;
  merge.generation.front() ^= 1;
  EXPECT_EQ(prepare(2), WifiHandoffLeaseResult::Invalid);
  merge.generation.front() ^= 1;
  fixture.request.transaction.front() ^= 1;
  EXPECT_EQ(prepare(2), WifiHandoffLeaseResult::Invalid);
  fixture.request.transaction.front() ^= 1;
  ASSERT_EQ(prepare(3), WifiHandoffLeaseResult::Ok);
  EXPECT_EQ(prepare(3), WifiHandoffLeaseResult::Busy);
  ASSERT_EQ(fixture.apply(fixture.command()), WifiHandoffLeaseResult::Ok);
  Sink sink{&fixture.lease};
  EXPECT_EQ(fixture.lease.consume(1000, {&sink, Sink::accept}), WifiHandoffLeaseResult::Ok);
  EXPECT_EQ(sink.calls, 1u);
}
