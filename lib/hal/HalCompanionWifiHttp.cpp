#include "HalCompanionWifiHttp.h"

#include <Logging.h>
#include <lwip/sockets.h>

#include <cerrno>
#include <cstdio>

namespace companion {
static_assert(WifiHttpRequest::MAX_BODY_SIZE == HalCompanionWifiMessages::MAX_MESSAGE_SIZE);
namespace {
bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
}  // namespace
HalCompanionWifiHttp::~HalCompanionWifiHttp() { end(); }
void HalCompanionWifiHttp::closeConnection() {
  if (connection >= 0) lwip_close(connection);
  connection = -1;
  parser.reset({});
  reply = {};
  header.fill(0);
  headerLength = headerSent = bodySent = 0;
  sending = false;
}
void HalCompanionWifiHttp::closeSockets() {
  closeConnection();
  if (listener >= 0) lwip_close(listener);
  listener = -1;
}
void HalCompanionWifiHttp::end() {
  closeSockets();
  messages.end();
  clock = {};
  dispatch = {};
}
bool HalCompanionWifiHttp::socketFailure(const char* operation) {
  LOG_ERR("CWIFI", "HTTP %s failed: %d", operation, errno);
  end();
  return false;
}
bool HalCompanionWifiHttp::begin(uint16_t port, WifiMessageClock clock, WifiMessageDispatch dispatch) {
  closeSockets();
  if (!port || !clock.milliseconds || !dispatch.execute || !messages.pollDeadline()) {
    LOG_ERR("CWIFI", "Invalid HTTP session");
    end();
    return false;
  }
  this->clock = clock;
  this->dispatch = dispatch;
  listener = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) return socketFailure("socket");
  if (lwip_fcntl(listener, F_SETFL, O_NONBLOCK) < 0) return socketFailure("nonblocking listener");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (lwip_bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0)
    return socketFailure("bind");
  if (lwip_listen(listener, 1) < 0) return socketFailure("listen");
  return true;
}
bool HalCompanionWifiHttp::poll() {
  if (listener < 0) return false;
  if (connection < 0 && messages.finishing()) {
    end();
    return false;
  }
  if (!messages.pollDeadline()) {
    end();
    return false;
  }
  if (connection < 0) {
    connection = lwip_accept(listener, nullptr, nullptr);
    if (connection < 0) return wouldBlock() ? true : socketFailure("accept");
    if (lwip_fcntl(connection, F_SETFL, O_NONBLOCK) < 0) return socketFailure("nonblocking connection");
    phaseStarted = clock.milliseconds(clock.context);
    parser.reset(messages.requestBuffer());
  }
  const auto now = clock.milliseconds(clock.context);
  if (now < phaseStarted || now - phaseStarted >= CONNECTION_TIMEOUT_MS) {
    LOG_ERR("CWIFI", "HTTP connection timed out");
    if (sending) {
      end();
      return false;
    }
    closeConnection();
    return true;
  }
  return sending ? sendReply() : receive();
}
bool HalCompanionWifiHttp::receive() {
  uint8_t input[128];
  size_t budget = IO_BUDGET;
  while (budget) {
    const auto count = lwip_recv(connection, input, std::min(budget, sizeof(input)), 0);
    if (count < 0) {
      if (wouldBlock()) return true;
      LOG_ERR("CWIFI", "HTTP read failed: %d", errno);
      closeConnection();
      return true;
    }
    if (!count) {
      LOG_ERR("CWIFI", "HTTP request interrupted");
      closeConnection();
      return true;
    }
    budget -= static_cast<size_t>(count);
    if (!parser.feed(std::span(input).first(static_cast<size_t>(count)))) {
      LOG_ERR("CWIFI", "Invalid HTTP request");
      closeConnection();
      return true;
    }
    if (parser.state() != WifiHttpRequestState::Complete) continue;
    const auto result = messages.process(parser.length(), dispatch);
    if (result.result != WifiMessageResult::Ok) {
      end();
      return false;
    }
    reply = result.bytes;
    static constexpr char FORMAT[] =
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %u\r\nConnection: close\r\n\r\n";
    const int written = snprintf(header.data(), header.size(), FORMAT, static_cast<unsigned>(reply.size()));
    if (written < 0 || static_cast<size_t>(written) >= header.size()) {
      LOG_ERR("CWIFI", "HTTP reply header failed");
      end();
      return false;
    }
    headerLength = static_cast<size_t>(written);
    sending = true;
    phaseStarted = clock.milliseconds(clock.context);
    return true;
  }
  return true;
}
bool HalCompanionWifiHttp::sendReply() {
  size_t budget = IO_BUDGET;
  while (budget) {
    const bool writingHeader = headerSent < headerLength;
    const auto* data =
        writingHeader ? reinterpret_cast<const uint8_t*>(header.data()) + headerSent : reply.data() + bodySent;
    const size_t remaining = writingHeader ? headerLength - headerSent : reply.size() - bodySent;
    if (!remaining) {
      closeConnection();
      return true;
    }
    const auto count = lwip_send(connection, data, std::min(budget, remaining), 0);
    if (count < 0) {
      if (wouldBlock()) return true;
      LOG_ERR("CWIFI", "HTTP write failed: %d", errno);
      end();
      return false;
    }
    if (!count) {
      LOG_ERR("CWIFI", "HTTP reply interrupted");
      end();
      return false;
    }
    budget -= static_cast<size_t>(count);
    if (writingHeader)
      headerSent += static_cast<size_t>(count);
    else
      bodySent += static_cast<size_t>(count);
  }
  return true;
}
}  // namespace companion
