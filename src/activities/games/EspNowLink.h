#pragma once

#include <NearbyTransfer.h>
#include <TableSession.h>

#include <cstddef>
#include <cstdint>

// Binds a TableSession to ESP-NOW through the SDK's EspNowTransport. Every
// table uses one fixed channel so hosts and guests meet without scanning.
//
// Heap cost while a table is open: the transport's 4-event receive ring
// (~4.4 KB) plus one event of scratch (~1.1 KB) here, on top of the WiFi
// driver's own buffers. Allocated by the Games activity only while it runs.
class EspNowLink final : public table::Link {
 public:
  static constexpr uint8_t CHANNEL = 1;

  bool begin();
  void end();
  bool send(const uint8_t* mac, const uint8_t* data, size_t len) override;
  // Hands queued packets to the session. Returns how many were delivered.
  int pump(table::TableSession& session, uint32_t nowMs);
  bool localMac(uint8_t* out) const { return transport.localMac(out); }
  bool started() const { return transport.started(); }

 private:
  // ESP-NOW caps unencrypted peers (~20) and the SDK transport registers one
  // per new destination without ever removing it. Recent unicast peers are
  // tracked here and the least recently used one is dropped before the cap.
  static constexpr int PEER_BUDGET = 12;
  void makeRoomForPeer(const uint8_t* mac);
  uint8_t peers[PEER_BUDGET][6] = {};
  uint32_t peerUse[PEER_BUDGET] = {};
  uint8_t peerCount = 0;
  uint32_t useClock = 0;

  freeink::nearby::EspNowTransport transport;
  freeink::nearby::EspNowTransport::Event event;
  bool overflowLogged = false;
  uint32_t sendFailures = 0;
};
