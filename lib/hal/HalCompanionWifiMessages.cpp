#include "HalCompanionWifiMessages.h"

#include <Logging.h>

#include "CompanionWifiHandoffCommands.h"

namespace companion {
HalCompanionWifiMessages::~HalCompanionWifiMessages() { end(); }
void HalCompanionWifiMessages::end() {
  active = false;
  activated = false;
  finishRequested = false;
  cipher.end();
  if (!workspace.empty()) std::fill(workspace.begin(), workspace.begin() + TRANSFER_OFFSET, 0);
  workspace = {};
  owner = {};
  transaction = {};
  generation = {};
  session = {};
  clock = {};
  lastActivity = lifetimeMilliseconds = 0;
}
bool HalCompanionWifiMessages::begin(const WifiHandoffOffer& offer, const Identity& reader, const Identity& generation,
                                     const Identity& installation, const Identity& transaction,
                                     std::span<uint8_t> workspace, uint64_t receivedAtMilliseconds,
                                     WifiMessageClock clock) {
  if (processing) {
    LOG_ERR("CWIFI", "Message processing active");
    return false;
  }
  end();
  if (!wifiHandoffMatches(offer, reader, generation, installation, transaction) ||
      workspace.size() < SESSION_WORKSPACE_SIZE || !clock.milliseconds) {
    LOG_ERR("CWIFI", "Invalid message session");
    return false;
  }
  const auto now = clock.milliseconds(clock.context);
  const uint64_t lifetime = static_cast<uint64_t>(offer.lifetimeSeconds) * 1000;
  if (now < receivedAtMilliseconds || now - receivedAtMilliseconds >= lifetime) {
    LOG_ERR("CWIFI", "Expired message offer");
    return false;
  }
  if (!cipher.begin(offer.key, offer.session, WifiMessageDirection::ReaderToApple)) return false;
  this->workspace = workspace.first(SESSION_WORKSPACE_SIZE);
  this->owner = installation;
  this->generation = generation;
  this->transaction = transaction;
  session = offer.session;
  this->clock = clock;
  lastActivity = receivedAtMilliseconds;
  lifetimeMilliseconds = lifetime;
  active = true;
  return true;
}
bool HalCompanionWifiMessages::pollDeadline() {
  if (!active) return false;
  if (validAt(clock.milliseconds(clock.context))) return true;
  fail(WifiMessageResult::Expired);
  return false;
}
std::span<uint8_t> HalCompanionWifiMessages::requestBuffer() {
  return active ? workspace.first(MAX_MESSAGE_SIZE) : std::span<uint8_t>{};
}
bool HalCompanionWifiMessages::validAt(uint64_t now) const {
  const uint64_t budget = activated ? 30000 : lifetimeMilliseconds;
  return active && now >= lastActivity && now - lastActivity < budget;
}
WifiMessageReply HalCompanionWifiMessages::fail(WifiMessageResult result) {
  LOG_ERR("CWIFI", "Message session ended: %u", static_cast<unsigned>(result));
  end();
  return {result, {}};
}
WifiMessageReply HalCompanionWifiMessages::process(size_t length, WifiMessageDispatch dispatch) {
  if (!active) return fail(WifiMessageResult::Inactive);
  if (finishRequested) return fail(WifiMessageResult::InvalidRequest);
  if (processing) {
    LOG_ERR("CWIFI", "Concurrent message rejected");
    return {WifiMessageResult::Busy, {}};
  }
  processing = true;
  struct Guard {
    bool& value;
    ~Guard() { value = false; }
  } guard{processing};
  if (!validAt(clock.milliseconds(clock.context))) return fail(WifiMessageResult::Expired);
  if (!dispatch.execute || length > MAX_MESSAGE_SIZE) return fail(WifiMessageResult::InvalidMessage);
  auto plain = workspace.subspan(PLAIN_OFFSET, HalCompanionWifiCipher::MAX_PAYLOAD);
  size_t plainLength = 0;
  if (cipher.open(workspace.first(length), plain, plainLength) != WifiCipherResult::Ok)
    return fail(WifiMessageResult::InvalidMessage);
  FrameView request;
  if (decodeFrame(plain.first(plainLength), true, request) != FrameError::None)
    return fail(WifiMessageResult::InvalidRequest);
  WifiHandoffSessionCommand finish;
  const bool ending = request.command == Command::WifiHandoff && !request.response &&
                      decodeWifiHandoffSessionCommand(request.payload, finish) &&
                      finish.action == WifiHandoffAction::Cancel && finish.transaction == transaction &&
                      finish.session == session;
  if (!ending && !validWifiTransferRequest(request, transaction, owner, generation))
    return fail(WifiMessageResult::InvalidRequest);
  if (!validAt(clock.milliseconds(clock.context))) return fail(WifiMessageResult::Expired);
  auto payload = workspace.subspan(PAYLOAD_OFFSET, MAX_CONTROL_PAYLOAD);
  if (ending) payload[0] = 0;
  const auto reply =
      ending ? WifiDispatchReply{Command::WifiHandoff, 1} : dispatch.execute(dispatch.context, request, owner, payload);
  if (!active) return fail(WifiMessageResult::Inactive);
  if (!validAt(clock.milliseconds(clock.context))) return fail(WifiMessageResult::Expired);
  if (reply.length > payload.size() || (reply.command != request.command && reply.command != Command::Error))
    return fail(WifiMessageResult::DispatchError);
  FrameView response{reply.command, true, request.requestId, payload.first(reply.length)};
  auto frame = workspace.subspan(REPLY_OFFSET, HalCompanionWifiCipher::MAX_PAYLOAD);
  const size_t frameLength = encodeFrame(response, frame);
  if (!frameLength) return fail(WifiMessageResult::DispatchError);
  auto wire = workspace.subspan(WIRE_OFFSET, MAX_MESSAGE_SIZE);
  size_t wireLength = 0;
  if (cipher.seal(frame.first(frameLength), wire, wireLength) != WifiCipherResult::Ok)
    return fail(WifiMessageResult::CryptoError);
  const auto now = clock.milliseconds(clock.context);
  if (!validAt(now)) return fail(WifiMessageResult::Expired);
  activated = true;
  lastActivity = now;
  finishRequested = ending;
  return {WifiMessageResult::Ok, wire.first(wireLength)};
}
}  // namespace companion
