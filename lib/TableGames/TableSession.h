#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "TableGame.h"
#include "TableWire.h"

namespace table {

// Radio seam: the device binds ESP-NOW, the host tests bind an in-memory bus.
class Link {
 public:
  virtual ~Link() = default;
  virtual bool send(const uint8_t* mac, const uint8_t* data, size_t len) = 0;
};

constexpr uint8_t BROADCAST_MAC[MAC_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

constexpr uint32_t BEACON_MS = 1000;
constexpr uint32_t PING_MS = 1000;
// Host marks a silent guest away; a guest that stops hearing the host starts
// rejoining. Several missed heartbeats, so one lost packet never trips it.
constexpr uint32_t PEER_TIMEOUT_MS = 4500;
// Away seats outside the running match are freed after this long.
constexpr uint32_t AWAY_DROP_MS = 30000;
// A guest gives up rejoining after this long without hearing the host.
constexpr uint32_t HOST_LOST_MS = 30000;
constexpr uint32_t JOIN_RETRY_MS = 700;
constexpr uint32_t JOIN_TIMEOUT_MS = 6000;
constexpr uint32_t ACTION_RETRY_MS = 400;
// Tables drop out of the browse list when their beacon goes quiet.
constexpr uint32_t TABLE_EXPIRE_MS = 4000;
constexpr uint32_t BOT_DELAY_MS = 900;
// Long enough to read a Liar's Dice reveal before a CPU rolls on.
constexpr uint32_t BOT_REVEAL_DELAY_MS = 5000;
constexpr uint8_t MAX_TABLES = 6;

struct TableInfo {
  uint8_t mac[MAC_LEN] = {};
  uint32_t tableId = 0;
  char hostName[NAME_LEN + 1] = {};
  uint8_t players = 0;
  uint8_t maxPlayers = 0;
  GameId game = GameId::None;
  Phase phase = Phase::Lobby;
  uint32_t lastSeenMs = 0;
};

struct SeatInfo {
  SeatStatus status = SeatStatus::Empty;
  char name[NAME_LEN + 1] = {};
  // Host-side bookkeeping; guests only see status and name.
  uint8_t mac[MAC_LEN] = {};
  uint32_t lastHeardMs = 0;
  uint16_t ackRoster = 0;
  uint16_t ackState = 0;
  uint16_t lastActionSeq = 0;
};

// One table: a host (slot 0) plus up to five guests or CPU players. The host is
// authoritative: guests send moves, the host validates them and sends every
// guest the state as that guest may see it. Guests report the roster and state
// versions they hold in each heartbeat and the host resends whatever is stale,
// so a lost packet heals within a second without per-message acks.
//
// The table outlives the games played on it: the host picks a game, plays it,
// and picks another (or the same) without anyone rejoining.
//
// Not thread-safe; the owner calls every method from one task.
class TableSession {
 public:
  enum class Role : uint8_t { None, Host, Guest, Solo };
  enum class Status : uint8_t { Idle, Browsing, Joining, Seated, Reconnecting, Closed, Denied, Lost };

  // link may be null for solo play. localMac is copied.
  void begin(Link* link, const uint8_t* localMac, const char* localName, uint32_t seed);
  void end();

  // --- host / solo ----------------------------------------------------------
  void host(uint32_t nowMs);
  void hostSolo();
  bool addBot();
  bool removeBot();
  bool startGame(GameId id, uint32_t nowMs);
  void returnToLobby();
  // CPU makes the moves of seats whose players have dropped out.
  void setBotTakeover(bool on) { botTakeover = on; }
  bool botTakeoverEnabled() const { return botTakeover; }

  // --- guest ----------------------------------------------------------------
  void browse();
  bool join(uint8_t tableIndex, uint32_t nowMs);

  // --- both -----------------------------------------------------------------
  // Guest: stand up. Host: close the table. Returns to Idle either way.
  void leave();
  void onPacket(const uint8_t* mac, const uint8_t* data, size_t len, uint32_t nowMs);
  void tick(uint32_t nowMs);
  // The local player's move. Host/solo apply it at once; a guest sends it and
  // retries until the host's state acknowledges it. False while a previous
  // guest move is still in flight or when the move is illegal on the host.
  bool submitAction(const uint8_t* action, size_t len, uint32_t nowMs);

  // --- views ----------------------------------------------------------------
  Role role() const { return role_; }
  Status status() const { return status_; }
  bool isHostLike() const { return role_ == Role::Host || role_ == Role::Solo; }
  Phase phase() const { return phase_; }
  GameId gameId() const { return gameId_; }
  uint8_t localSlot() const { return localSlot_; }
  // Game seat of the local player, or -1 when spectating / in the lobby.
  int localSeat() const { return seatOfSlot(localSlot_); }
  const SeatInfo& seat(const uint8_t slot) const { return seats[slot < MAX_SLOTS ? slot : 0]; }
  uint8_t playerCount() const;
  uint8_t gameSeatCount() const { return gameSeats; }
  uint8_t slotOfSeat(const uint8_t gameSeat) const { return gameSeat < gameSeats ? gameSlots[gameSeat] : NO_SLOT; }
  int seatOfSlot(uint8_t slot) const;
  // The game as the local player may see it (null before the first state).
  const Game* view() const { return viewGame.get(); }
  // False while a guest still holds the previous match's state (the new
  // roster arrived first). Moves are only accepted against a current view.
  bool viewIsCurrent() const;
  // The serialized view, for handing to another task.
  size_t viewBytes(uint8_t* out, size_t capacity) const;
  uint8_t tableCount() const { return tables; }
  const TableInfo& tableAt(const uint8_t i) const { return tableList[i < MAX_TABLES ? i : 0]; }
  bool actionPending() const { return pendingLen > 0; }
  DenyReason denyReason() const { return deny; }
  // Bumped on every change a screen might show.
  uint32_t revision() const { return revision_; }

 private:
  void reset();
  void touch() { revision_++; }

  // host
  void hostHandle(const Header& h, const uint8_t* mac, Reader& r, uint32_t nowMs);
  void hostOnJoin(const uint8_t* mac, Reader& r, uint32_t nowMs);
  uint8_t slotForMac(const uint8_t* mac) const;
  bool slotInGame(uint8_t slot) const;
  // Seats a CPU player without sending anything; NO_SLOT when full.
  uint8_t seatBot();
  void releaseLeftSeats();
  // Slots startGame would seat; false (nothing changed) when the table cannot
  // field the game's minimum even with CPU players.
  bool planSeats(const Game& game, uint8_t* slots, uint8_t& count, uint8_t& botsNeeded) const;
  void rosterChanged();
  void stateChanged(uint32_t nowMs);
  void refreshHostView();
  void runBots(uint32_t nowMs);
  void flush();
  void sendBeacon();
  void sendRoster(uint8_t slot);
  void sendState(uint8_t slot);
  void sendWelcome(uint8_t slot);
  void sendDeny(const uint8_t* mac, DenyReason reason);
  void sendClose(const uint8_t* mac);

  // guest
  void guestHandle(const Header& h, const uint8_t* mac, Reader& r, uint32_t nowMs);
  void guestOnBeacon(const Header& h, const uint8_t* mac, Reader& r, uint32_t nowMs);
  void guestOnRoster(Reader& r);
  void guestOnState(Reader& r);
  void sendJoin();
  void sendPing();
  void sendAction();
  void sendLeave();

  bool send(const uint8_t* mac, const uint8_t* data, size_t len);
  bool fromHost(const Header& h, const uint8_t* mac) const;

  Link* link = nullptr;
  Rng rng;
  uint8_t localMac[MAC_LEN] = {};
  char localName[NAME_LEN + 1] = {};

  Role role_ = Role::None;
  Status status_ = Status::Idle;
  uint32_t tableId = 0;
  uint8_t localSlot_ = NO_SLOT;
  SeatInfo seats[MAX_SLOTS];
  Phase phase_ = Phase::Lobby;
  GameId gameId_ = GameId::None;
  uint8_t gameSlots[MAX_SLOTS] = {};
  uint8_t gameSeats = 0;
  uint16_t rosterVersion = 0;
  uint16_t stateVersion = 0;
  // Bumped by every startGame. Moves carry it, so a move retried across a
  // restart or game switch is acknowledged but never applied to the new match.
  uint8_t matchId = 0;
  // Guest: match the held view belongs to.
  uint8_t stateMatch = 0;
  std::unique_ptr<Game> viewGame;
  uint8_t viewBuf[MAX_GAME_STATE] = {};
  size_t viewLen = 0;
  DenyReason deny = DenyReason::Full;
  uint32_t revision_ = 0;

  // host only
  std::unique_ptr<Game> authGame;
  bool rosterDirty = false;
  bool stateDirty = false;
  bool botTakeover = false;
  uint32_t lastBeaconMs = 0;
  uint32_t lastMoveMs = 0;

  // guest only
  uint8_t hostMac[MAC_LEN] = {};
  bool haveRoster = false;
  bool haveState = false;
  uint32_t lastHostHeardMs = 0;
  // Last time the host showed it hears us: a unicast to us, or a beacon whose
  // present-seat mask includes our slot. Beacons alone only prove the host is
  // alive, so a guest the host cannot hear still notices and rejoins.
  uint32_t lastAckedMs = 0;
  uint32_t lastPingMs = 0;
  uint32_t lastJoinMs = 0;
  uint32_t joinStartMs = 0;
  uint16_t actionSeq = 0;
  uint8_t pendingAction[MAX_ACTION] = {};
  uint8_t pendingLen = 0;
  uint8_t pendingMatch = 0;
  uint32_t lastActionMs = 0;
  TableInfo tableList[MAX_TABLES];
  uint8_t tables = 0;
};

}  // namespace table
