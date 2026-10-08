#include "HalCompanionWifiSession.h"

#include <Logging.h>

namespace companion {
HalCompanionWifiSession::~HalCompanionWifiSession() { end(); }
void HalCompanionWifiSession::end() {
  active = false;
  if (starting) startupCancelled = true;
  discovery.end();
  http.end();
}
bool HalCompanionWifiSession::begin(const WifiHandoffOffer& offer, const Identity& reader, const Identity& generation,
                                    const Identity& installation, const Identity& transaction,
                                    std::span<uint8_t> workspace, uint64_t receivedAtMilliseconds,
                                    WifiMessageClock clock, WifiMessageDispatch dispatch) {
  if (active || polling || starting) {
    LOG_ERR("CWIFI", "Session already active");
    return false;
  }
  startupCancelled = false;
  struct Guard {
    bool& value;
    explicit Guard(bool& value) : value(value) { value = true; }
    ~Guard() { value = false; }
  } guard(starting);
  if (!messages.begin(offer, reader, generation, installation, transaction, workspace, receivedAtMilliseconds, clock) ||
      startupCancelled || !http.begin(offer.port, clock, dispatch) || startupCancelled || !discovery.begin(offer) ||
      startupCancelled || !messages.pollDeadline() || startupCancelled) {
    end();
    return false;
  }
  active = true;
  return true;
}
bool HalCompanionWifiSession::poll() {
  if (!active) return false;
  if (polling || starting) {
    LOG_ERR("CWIFI", "Session polling reentered");
    return true;
  }
  struct Guard {
    bool& value;
    explicit Guard(bool& value) : value(value) { value = true; }
    ~Guard() { value = false; }
  } guard(polling);
  if (!http.poll() || !active) {
    end();
    return false;
  }
  return true;
}
}  // namespace companion
