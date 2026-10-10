#include <gtest/gtest.h>
#include <lwip/sockets.h>

#include <array>
#include <string>

#include "CompanionWifiHandoffCommands.h"
#include "HalCompanionWifiHttp.h"

using namespace companion;
namespace {
uint16_t freePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  socklen_t length = sizeof(address);
  EXPECT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length), 0);
  const uint16_t port = ntohs(address.sin_port);
  ::close(fd);
  return port;
}
struct Fixture {
  WifiHandoffOffer offer;
  std::array<uint8_t, SESSION_WORKSPACE_SIZE> workspace{};
  HalCompanionWifiMessages messages;
  HalCompanionWifiHttp server{messages};
  HalCompanionWifiCipher apple;
  uint64_t now = 0;
  unsigned calls = 0;
  bool reuseWorkspace = false;
  Command expectedCommand = Command::TransferStatus;
  int client = -1;
  Fixture() {
    offer.reader.fill(1);
    offer.storageGeneration.fill(2);
    offer.installation.fill(3);
    offer.transaction.fill(4);
    offer.session.fill(5);
    offer.key.fill(6);
    offer.port = freePort();
    offer.lifetimeSeconds = 30;
  }
  ~Fixture() {
    if (client >= 0) ::close(client);
  }
  static uint64_t clock(void* context) { return static_cast<Fixture*>(context)->now; }
  static WifiDispatchReply dispatch(void* context, const FrameView& request, const Identity& owner,
                                    std::span<uint8_t> output) {
    auto& self = *static_cast<Fixture*>(context);
    ++self.calls;
    EXPECT_EQ(owner, self.offer.installation);
    EXPECT_EQ(request.command, self.expectedCommand);
    if (self.reuseWorkspace) {
      EXPECT_TRUE(self.messages.acquireWorkspace(self.offer.session));
      EXPECT_TRUE(self.messages.workspaceOwned(self.offer.session));
      std::fill(self.workspace.begin(), self.workspace.end(), 85);
      EXPECT_TRUE(self.messages.releaseWorkspace(self.offer.session));
    }
    output[0] = 0;
    return {request.command, 1};
  }
  bool begin() {
    return messages.begin(offer, offer.reader, offer.storageGeneration, offer.installation, offer.transaction,
                          workspace, 0, {this, clock}) &&
           apple.begin(offer.key, offer.session, WifiMessageDirection::AppleToReader) &&
           server.begin(offer.port, {this, clock}, {this, dispatch});
  }
  void connect() {
    if (client >= 0) ::close(client);
    client = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(offer.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(::fcntl(client, F_SETFL, O_NONBLOCK), 0);
    ASSERT_TRUE(server.poll());
  }
  std::string request(bool tamper = false) {
    std::array<uint8_t, FRAME_HEADER_SIZE + MAX_CONTROL_PAYLOAD> plain{};
    std::array<uint8_t, MAX_CONTROL_PAYLOAD> chunk{};
    std::copy(offer.transaction.begin(), offer.transaction.end(), chunk.begin());
    if (expectedCommand == Command::WifiHandoff)
      encodeWifiHandoffSessionCommand({WifiHandoffAction::Cancel, offer.transaction, offer.session}, chunk);
    const auto payload = expectedCommand == Command::WifiHandoff     ? std::span<const uint8_t>(chunk).first(34)
                         : expectedCommand == Command::TransferChunk ? std::span<const uint8_t>(chunk)
                                                                     : std::span<const uint8_t>(offer.transaction);
    const auto size = encodeFrame({expectedCommand, false, 17, payload}, plain);
    std::array<uint8_t, HalCompanionWifiMessages::MAX_MESSAGE_SIZE> wire{};
    size_t length = 0;
    EXPECT_EQ(apple.seal(std::span(plain).first(size), wire, length), WifiCipherResult::Ok);
    if (tamper) wire[length - 1] ^= 1;
    auto text = std::string(
                    "POST /companion/v1/messages HTTP/1.1\r\nHost: reader\r\nContent-Type: "
                    "application/octet-stream\r\nContent-Length: ") +
                std::to_string(length) + "\r\n\r\n";
    text.append(reinterpret_cast<const char*>(wire.data()), length);
    return text;
  }
  void write(const std::string& text) {
    ASSERT_EQ(::send(client, text.data(), text.size(), MSG_NOSIGNAL), static_cast<ssize_t>(text.size()));
  }
  std::string reply() {
    std::string result;
    result.reserve(2048);
    std::array<char, 128> bytes{};
    for (unsigned at = 0; at < 1000; ++at) {
      const bool running = server.poll();
      if (expectedCommand != Command::WifiHandoff) {
        EXPECT_TRUE(running);
      }
      const auto count = ::recv(client, bytes.data(), bytes.size(), 0);
      if (!count) return result;
      if (count > 0) result.append(bytes.data(), static_cast<size_t>(count));
    }
    ADD_FAILURE() << "connection not closed";
    return result;
  }
};
}  // namespace
TEST(HalCompanionWifiHttpTest, FinishDrainsEncryptedAcknowledgementBeforeStoppingListener) {
  Fixture fixture;
  ASSERT_TRUE(fixture.begin());
  fixture.expectedCommand = Command::WifiHandoff;
  fixture.connect();
  fixture.write(fixture.request());
  const auto reply = fixture.reply();
  ASSERT_TRUE(reply.starts_with("HTTP/1.1 200 OK\r\n"));
  const auto split = reply.find("\r\n\r\n");
  ASSERT_NE(split, std::string::npos);
  const auto body = std::span(reinterpret_cast<const uint8_t*>(reply.data() + split + 4), reply.size() - split - 4);
  std::array<uint8_t, 128> plain{};
  size_t length = 0;
  ASSERT_EQ(fixture.apple.open(body, plain, length), WifiCipherResult::Ok);
  FrameView response;
  ASSERT_EQ(decodeFrame(std::span(plain).first(length), true, response), FrameError::None);
  EXPECT_EQ(response.command, Command::WifiHandoff);
  ASSERT_EQ(response.payload.size(), 1u);
  EXPECT_EQ(response.payload[0], 0u);
  EXPECT_EQ(fixture.calls, 0u);
  EXPECT_FALSE(fixture.server.poll());
  EXPECT_TRUE(fixture.messages.requestBuffer().empty());
}
TEST(HalCompanionWifiHttpTest, RealTcpEncryptedReplyAndRepeatedConnections) {
  Fixture fixture;
  ASSERT_TRUE(fixture.begin());
  for (unsigned at = 0; at < 2; ++at) {
    fixture.connect();
    fixture.write(fixture.request());
    const auto reply = fixture.reply();
    EXPECT_TRUE(reply.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_NE(reply.find("Connection: close\r\n"), std::string::npos);
    const auto split = reply.find("\r\n\r\n");
    ASSERT_NE(split, std::string::npos);
    const auto body = std::span(reinterpret_cast<const uint8_t*>(reply.data() + split + 4), reply.size() - split - 4);
    std::array<uint8_t, HalCompanionWifiCipher::MAX_PAYLOAD> plain{};
    size_t length = 0;
    ASSERT_EQ(fixture.apple.open(body, plain, length), WifiCipherResult::Ok);
    FrameView response;
    ASSERT_EQ(decodeFrame(std::span(plain).first(length), true, response), FrameError::None);
    EXPECT_TRUE(response.response);
    EXPECT_EQ(response.requestId, 17u);
    EXPECT_EQ(response.command, Command::TransferStatus);
    ASSERT_EQ(response.payload.size(), 1u);
    EXPECT_EQ(response.payload[0], 0);
  }
  EXPECT_EQ(fixture.calls, 2u);
  fixture.server.end();
  EXPECT_TRUE(fixture.messages.requestBuffer().empty());
  EXPECT_FALSE(fixture.server.poll());
}
TEST(HalCompanionWifiHttpTest, MalformedHttpAndSlowConnectionDoNotConsumeCipherSession) {
  Fixture fixture;
  ASSERT_TRUE(fixture.begin());
  fixture.connect();
  fixture.write("GET / HTTP/1.1\r\n\r\n");
  EXPECT_TRUE(fixture.reply().empty());
  EXPECT_EQ(fixture.calls, 0u);
  fixture.connect();
  fixture.write("POST /companion");
  fixture.now = 5000;
  EXPECT_TRUE(fixture.server.poll());
  EXPECT_EQ(fixture.calls, 0u);
  EXPECT_TRUE(fixture.messages.pollDeadline());
  fixture.connect();
  fixture.write(fixture.request());
  EXPECT_FALSE(fixture.reply().empty());
  EXPECT_EQ(fixture.calls, 1u);
}
TEST(HalCompanionWifiHttpTest, TamperingAndIdleExpiryCloseListenerAndClearMessageSession) {
  {
    Fixture fixture;
    ASSERT_TRUE(fixture.begin());
    fixture.connect();
    fixture.write(fixture.request(true));
    EXPECT_FALSE(fixture.server.poll());
    EXPECT_EQ(fixture.calls, 0u);
    EXPECT_TRUE(fixture.messages.requestBuffer().empty());
    EXPECT_FALSE(fixture.server.poll());
  }
  {
    Fixture fixture;
    ASSERT_TRUE(fixture.begin());
    fixture.now = 30000;
    EXPECT_FALSE(fixture.server.poll());
    EXPECT_TRUE(fixture.messages.requestBuffer().empty());
  }
}
TEST(HalCompanionWifiHttpTest, FailedBindReleasesItsSocketAndKey) {
  Fixture first;
  ASSERT_TRUE(first.begin());
  Fixture second;
  second.offer.port = first.offer.port;
  EXPECT_FALSE(second.begin());
  EXPECT_TRUE(second.messages.requestBuffer().empty());
  EXPECT_FALSE(second.server.poll());
  EXPECT_TRUE(first.server.poll());
}

TEST(HalCompanionWifiHttpTest, ReplyTimeoutEndsSessionAfterAuthenticatedDispatch) {
  Fixture fixture;
  ASSERT_TRUE(fixture.begin());
  fixture.connect();
  fixture.write(fixture.request());
  ASSERT_TRUE(fixture.server.poll());
  ASSERT_EQ(fixture.calls, 1u);
  fixture.now = 5000;
  EXPECT_FALSE(fixture.server.poll());
  EXPECT_TRUE(fixture.messages.requestBuffer().empty());
  EXPECT_EQ(fixture.calls, 1u);
}

TEST(HalCompanionWifiHttpTest, MaximumEncryptedChunkCrossesParserAndSocketReadBoundaries) {
  Fixture fixture;
  fixture.expectedCommand = Command::TransferChunk;
  ASSERT_TRUE(fixture.begin());
  fixture.connect();
  fixture.write(fixture.request());
  const auto reply = fixture.reply();
  const auto split = reply.find("\r\n\r\n");
  ASSERT_NE(split, std::string::npos);
  const auto body = std::span(reinterpret_cast<const uint8_t*>(reply.data() + split + 4), reply.size() - split - 4);
  EXPECT_NE(reply.find("Content-Length: " + std::to_string(body.size()) + "\r\n"), std::string::npos);
  std::array<uint8_t, HalCompanionWifiCipher::MAX_PAYLOAD> plain{};
  size_t length = 0;
  ASSERT_EQ(fixture.apple.open(body, plain, length), WifiCipherResult::Ok);
  FrameView response;
  ASSERT_EQ(decodeFrame(std::span(plain).first(length), true, response), FrameError::None);
  EXPECT_EQ(response.command, Command::TransferChunk);
  EXPECT_EQ(fixture.calls, 1u);
}

TEST(HalCompanionWifiHttpTest, FullWorkspaceCommitLoanSurvivesHttpFramingAndRepeatedEncryptedConnections) {
  Fixture fixture;
  ASSERT_TRUE(fixture.begin());
  fixture.reuseWorkspace = true;
  fixture.expectedCommand = Command::Commit;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    fixture.connect();
    fixture.write(fixture.request());
    const auto reply = fixture.reply();
    ASSERT_TRUE(reply.starts_with("HTTP/1.1 200 OK\r\n"));
    const auto split = reply.find("\r\n\r\n");
    ASSERT_NE(split, std::string::npos);
    const auto body = std::span(reinterpret_cast<const uint8_t*>(reply.data() + split + 4), reply.size() - split - 4);
    std::array<uint8_t, 128> plain{};
    size_t length = 0;
    ASSERT_EQ(fixture.apple.open(body, plain, length), WifiCipherResult::Ok);
    FrameView response;
    ASSERT_EQ(decodeFrame(std::span(plain).first(length), true, response), FrameError::None);
    EXPECT_EQ(response.command, Command::Commit);
    EXPECT_EQ(response.requestId, 17u);
    ASSERT_EQ(response.payload.size(), 1u);
    EXPECT_EQ(response.payload[0], 0u);
    EXPECT_FALSE(fixture.messages.workspaceOwned(fixture.offer.session));
  }
  EXPECT_EQ(fixture.calls, 2u);
}
