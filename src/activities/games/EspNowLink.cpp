#include "EspNowLink.h"

#include <Logging.h>
#include <esp_now.h>

#include <cstring>

bool EspNowLink::begin() {
  if (!transport.begin(CHANNEL)) {
    LOG_ERR("GAME", "ESP-NOW start failed on channel %u", static_cast<unsigned>(CHANNEL));
    return false;
  }
  overflowLogged = false;
  sendFailures = 0;
  peerCount = 0;
  LOG_INF("GAME", "ESP-NOW up on channel %u", static_cast<unsigned>(CHANNEL));
  return true;
}

void EspNowLink::end() {
  if (!transport.started()) return;
  transport.end();
  LOG_INF("GAME", "ESP-NOW down (%lu send failures)", static_cast<unsigned long>(sendFailures));
}

void EspNowLink::makeRoomForPeer(const uint8_t* mac) {
  static constexpr uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  if (memcmp(mac, BROADCAST, sizeof(BROADCAST)) == 0) return;
  useClock++;
  int oldest = 0;
  for (int i = 0; i < peerCount; i++) {
    if (memcmp(peers[i], mac, 6) == 0) {
      peerUse[i] = useClock;
      return;
    }
    if (peerUse[i] < peerUse[oldest]) oldest = i;
  }
  int slot = peerCount;
  if (peerCount < PEER_BUDGET) {
    peerCount++;
  } else {
    // Forget the least recently used peer; the transport re-adds it on demand.
    esp_now_del_peer(peers[oldest]);
    slot = oldest;
  }
  memcpy(peers[slot], mac, 6);
  peerUse[slot] = useClock;
}

bool EspNowLink::send(const uint8_t* mac, const uint8_t* data, const size_t len) {
  if (!mac) return false;
  makeRoomForPeer(mac);
  if (transport.send(mac, data, len)) return true;
  // Losses are healed by the session's heartbeats; only count them.
  sendFailures++;
  return false;
}

int EspNowLink::pump(table::TableSession& session, const uint32_t nowMs) {
  // Bounded per loop pass so a chatty neighbour cannot starve input handling;
  // the ring holds only four events, so this still drains it every pass.
  constexpr int MAX_PER_PASS = 8;
  int delivered = 0;
  while (delivered < MAX_PER_PASS && transport.poll(event)) {
    session.onPacket(event.sourceMac.data(), event.data.data(), event.length, nowMs);
    delivered++;
  }
  if (transport.overflowed() && !overflowLogged) {
    overflowLogged = true;
    LOG_ERR("GAME", "ESP-NOW receive ring overflowed; heartbeats will resync");
  }
  return delivered;
}
