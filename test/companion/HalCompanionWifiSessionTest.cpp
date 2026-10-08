#include <gtest/gtest.h>
#include <lwip/sockets.h>
#include <mdns.h>

#include <string>

#include "HalCompanionWifiSession.h"

using namespace companion;
unsigned wifiDiscoveryErrors = 0;
namespace {
unsigned starts = 0, stops = 0, failure = 0;
uint64_t* advanceClock = nullptr;
uint16_t freePort() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  socklen_t length = sizeof(address);
  EXPECT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length), 0);
  ::close(fd);
  return ntohs(address.sin_port);
}
struct Fixture {
  WifiHandoffOffer offer;
  std::array<uint8_t, SESSION_WORKSPACE_SIZE> workspace;
  uint64_t now = 0;
  bool cancelOnClock = false, reenterOnClock = false;
  HalCompanionWifiSession session;
  Fixture() {
    starts = stops = failure = wifiDiscoveryErrors = 0;
    advanceClock = nullptr;
    offer.reader.fill(1);
    offer.storageGeneration.fill(2);
    offer.installation.fill(3);
    offer.transaction.fill(4);
    offer.session.fill(5);
    offer.key.fill(6);
    offer.port = freePort();
    offer.lifetimeSeconds = 30;
    workspace.fill(0x7a);
  }
  bool begin() {
    return session.begin(offer, offer.reader, offer.storageGeneration, offer.installation, offer.transaction, workspace,
                         0,
                         {this,
                          [](void* context) {
                            auto& fixture = *static_cast<Fixture*>(context);
                            if (fixture.cancelOnClock) fixture.session.end();
                            if (fixture.reenterOnClock) {
                              fixture.reenterOnClock = false;
                              EXPECT_FALSE(fixture.begin());
                            }
                            return fixture.now;
                          }},
                         {nullptr, [](void*, const FrameView&, const Identity&, std::span<uint8_t>) {
                            return WifiDispatchReply{Command::Error, 0};
                          }});
  }
  void expectReleased() {
    EXPECT_FALSE(session.isActive());
    EXPECT_FALSE(session.poll());
    EXPECT_TRUE(
        std::all_of(workspace.begin(), workspace.begin() + TRANSFER_OFFSET, [](uint8_t byte) { return byte == 0; }));
    EXPECT_TRUE(
        std::all_of(workspace.begin() + TRANSFER_OFFSET, workspace.end(), [](uint8_t byte) { return byte == 0x7a; }));
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(offer.port);
    EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ::close(fd);
  }
};
}  // namespace
esp_err_t mdns_init() {
  ++starts;
  return failure == 1 ? -1 : ESP_OK;
}
void mdns_free() { ++stops; }
esp_err_t mdns_hostname_set(const char*) { return failure == 2 ? -1 : ESP_OK; }
esp_err_t mdns_service_add(const char*, const char*, const char*, uint16_t, mdns_txt_item_t*, size_t) {
  if (advanceClock) *advanceClock = 30000;
  return failure == 3 ? -1 : ESP_OK;
}
TEST(HalCompanionWifiSessionTest, StartsOnceAndExplicitEndReleasesWorkspaceListenerAndDiscovery) {
  Fixture fixture;
  ASSERT_TRUE(fixture.begin());
  EXPECT_TRUE(fixture.session.poll());
  EXPECT_FALSE(fixture.begin());
  EXPECT_TRUE(fixture.session.isActive());
  EXPECT_EQ(starts, 1u);
  fixture.session.end();
  fixture.expectReleased();
  EXPECT_EQ(stops, 1u);
  fixture.session.end();
  EXPECT_EQ(stops, 1u);
}
TEST(HalCompanionWifiSessionTest, EveryDiscoveryFailureClosesListenerAndClearsCipherWorkspace) {
  for (unsigned fail = 1; fail <= 3; ++fail) {
    Fixture fixture;
    failure = fail;
    EXPECT_FALSE(fixture.begin());
    fixture.expectReleased();
    EXPECT_EQ(starts, 1u);
    EXPECT_EQ(stops, fail == 1 ? 0u : 1u);
    failure = 0;
    fixture.offer.session[0] = 9;
    fixture.offer.key[0] = 10;
    ASSERT_TRUE(fixture.begin());
    fixture.session.end();
  }
}
TEST(HalCompanionWifiSessionTest, ListenerFailureNeverPublishesDiscovery) {
  Fixture fixture;
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(fixture.offer.port);
  ASSERT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  EXPECT_FALSE(fixture.begin());
  EXPECT_EQ(starts, 0u);
  EXPECT_FALSE(fixture.session.isActive());
  ::close(fd);
  fixture.expectReleased();
}
TEST(HalCompanionWifiSessionTest, ExpiryDuringPublicationOrPollingReleasesAllResources) {
  for (bool duringStartup : {false, true}) {
    Fixture fixture;
    if (duringStartup) {
      advanceClock = &fixture.now;
      EXPECT_FALSE(fixture.begin());
      advanceClock = nullptr;
    } else {
      ASSERT_TRUE(fixture.begin());
      fixture.now = 30000;
      EXPECT_FALSE(fixture.session.poll());
    }
    fixture.expectReleased();
    EXPECT_EQ(stops, 1u);
  }
}

TEST(HalCompanionWifiSessionTest, CancellationAndReentryDuringStartupDoNotRestartSession) {
  for (bool cancel : {false, true}) {
    Fixture fixture;
    fixture.cancelOnClock = cancel;
    fixture.reenterOnClock = !cancel;
    EXPECT_EQ(fixture.begin(), !cancel);
    EXPECT_EQ(starts, cancel ? 0u : 1u);
    fixture.session.end();
    fixture.expectReleased();
  }
}
TEST(HalCompanionWifiSessionTest, DestructionClosesListenerAndDiscovery) {
  uint16_t port;
  {
    Fixture fixture;
    ASSERT_TRUE(fixture.begin());
    port = fixture.offer.port;
  }
  EXPECT_EQ(stops, 1u);
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  ::close(fd);
}

TEST(HalCompanionWifiSessionTest, UnauthenticatedHttpMessageEndsWholeSession) {
  Fixture fixture;
  ASSERT_TRUE(fixture.begin());
  int client = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(fixture.offer.port);
  ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  const std::string request = std::string(
                                  "POST /companion/v1/messages HTTP/1.1\r\nHost: reader\r\nContent-Type: "
                                  "application/octet-stream\r\nContent-Length: 48\r\n\r\n") +
                              std::string(48, 'x');
  ASSERT_EQ(::send(client, request.data(), request.size(), 0), static_cast<ssize_t>(request.size()));
  for (unsigned attempt = 0; attempt < 10 && fixture.session.isActive(); ++attempt) fixture.session.poll();
  EXPECT_FALSE(fixture.session.isActive());
  EXPECT_EQ(stops, 1u);
  EXPECT_TRUE(std::all_of(fixture.workspace.begin(), fixture.workspace.begin() + TRANSFER_OFFSET,
                          [](uint8_t byte) { return byte == 0; }));
  ::close(client);
}
