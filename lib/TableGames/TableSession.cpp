#include "TableSession.h"

#include <algorithm>
#include <cstdio>

namespace table {
namespace {

bool sameMac(const uint8_t* a, const uint8_t* b) { return memcmp(a, b, MAC_LEN) == 0; }

bool isSeated(const SeatStatus s) { return s == SeatStatus::Present || s == SeatStatus::Away; }

}  // namespace

// --- lifecycle ---------------------------------------------------------------

void TableSession::begin(Link* newLink, const uint8_t* mac, const char* name, const uint32_t seed) {
  reset();
  link = newLink;
  rng = Rng(seed);
  if (mac) memcpy(localMac, mac, MAC_LEN);
  snprintf(localName, sizeof(localName), "%s", name ? name : "");
}

void TableSession::end() {
  leave();
  link = nullptr;
}

void TableSession::reset() {
  role_ = Role::None;
  status_ = Status::Idle;
  tableId = 0;
  localSlot_ = NO_SLOT;
  for (auto& s : seats) s = SeatInfo{};
  phase_ = Phase::Lobby;
  gameId_ = GameId::None;
  gameSeats = 0;
  rosterVersion = 0;
  stateVersion = 0;
  matchId = 0;
  stateMatch = 0;
  viewGame.reset();
  authGame.reset();
  viewLen = 0;
  rosterDirty = false;
  stateDirty = false;
  botTakeover = false;
  haveRoster = false;
  haveState = false;
  pendingLen = 0;
  actionSeq = 0;
  tables = 0;
  touch();
}

void TableSession::leave() {
  if (role_ == Role::Host) {
    // Unicast first (MAC-level retries), then a broadcast for anyone the
    // roster lost track of.
    for (uint8_t slot = 1; slot < MAX_SLOTS; slot++) {
      if (isSeated(seats[slot].status)) sendClose(seats[slot].mac);
    }
    sendClose(BROADCAST_MAC);
  } else if (role_ == Role::Guest &&
             (status_ == Status::Seated || status_ == Status::Reconnecting || status_ == Status::Joining)) {
    // Also while joining: the host may already have seated us.
    sendLeave();
    sendLeave();
  }
  reset();
}

// --- host / solo ---------------------------------------------------------------

void TableSession::host(const uint32_t nowMs) {
  reset();
  role_ = Role::Host;
  status_ = Status::Seated;
  tableId = rng.next() | 1u;  // never 0
  localSlot_ = HOST_SLOT;
  seats[HOST_SLOT].status = SeatStatus::Present;
  memcpy(seats[HOST_SLOT].mac, localMac, MAC_LEN);
  memcpy(seats[HOST_SLOT].name, localName, sizeof(localName));
  rosterVersion = 1;
  lastBeaconMs = nowMs - BEACON_MS;  // advertise on the first tick
  touch();
}

void TableSession::hostSolo() {
  reset();
  role_ = Role::Solo;
  status_ = Status::Seated;
  localSlot_ = HOST_SLOT;
  seats[HOST_SLOT].status = SeatStatus::Present;
  memcpy(seats[HOST_SLOT].name, localName, sizeof(localName));
  rosterVersion = 1;
  touch();
}

uint8_t TableSession::playerCount() const {
  uint8_t n = 0;
  for (const auto& s : seats) n += s.status != SeatStatus::Empty ? 1 : 0;
  return n;
}

int TableSession::seatOfSlot(const uint8_t slot) const {
  if (phase_ != Phase::Playing || slot == NO_SLOT) return -1;
  for (uint8_t i = 0; i < gameSeats; i++) {
    if (gameSlots[i] == slot) return i;
  }
  return -1;
}

bool TableSession::slotInGame(const uint8_t slot) const {
  for (uint8_t i = 0; i < gameSeats; i++) {
    if (gameSlots[i] == slot) return true;
  }
  return false;
}

uint8_t TableSession::seatBot() {
  for (uint8_t slot = 1; slot < MAX_SLOTS; slot++) {
    // A seat still tied to the running match stays reserved for its player.
    if (seats[slot].status != SeatStatus::Empty || (phase_ == Phase::Playing && slotInGame(slot))) continue;
    // Number bots by the lowest free label so removals do not leave gaps.
    for (unsigned n = 1; n <= MAX_SLOTS; n++) {
      char label[NAME_LEN + 1];
      snprintf(label, sizeof(label), "CPU %u", n);
      bool used = false;
      for (const auto& s : seats) used = used || (s.status == SeatStatus::Bot && strcmp(s.name, label) == 0);
      if (used) continue;
      seats[slot] = SeatInfo{};
      seats[slot].status = SeatStatus::Bot;
      memcpy(seats[slot].name, label, sizeof(label));
      break;
    }
    rosterChanged();
    return slot;
  }
  return NO_SLOT;
}

bool TableSession::addBot() {
  if (!isHostLike() || seatBot() == NO_SLOT) return false;
  flush();
  return true;
}

void TableSession::releaseLeftSeats() {
  for (uint8_t i = 0; i < gameSeats; i++) {
    const uint8_t slot = gameSlots[i];
    if (seats[slot].status == SeatStatus::Empty) seats[slot] = SeatInfo{};
  }
  gameSeats = 0;
}

bool TableSession::removeBot() {
  if (!isHostLike()) return false;
  for (int slot = MAX_SLOTS - 1; slot > 0; slot--) {
    if (seats[slot].status != SeatStatus::Bot) continue;
    if (phase_ == Phase::Playing && slotInGame(static_cast<uint8_t>(slot))) continue;
    seats[slot] = SeatInfo{};
    rosterChanged();
    flush();
    return true;
  }
  return false;
}

bool TableSession::planSeats(const Game& game, uint8_t* slots, uint8_t& count, uint8_t& botsNeeded) const {
  // The people present in slot order (host first), then CPU players, so a
  // two-seat game at a busy table goes to humans.
  count = 0;
  for (const SeatStatus wanted : {SeatStatus::Present, SeatStatus::Bot}) {
    for (uint8_t slot = 0; slot < MAX_SLOTS && count < game.maxSeats(); slot++) {
      if (seats[slot].status == wanted) slots[count++] = slot;
    }
  }
  // CPU players top up to the game's minimum, so a lone host can always start.
  // Any empty slot will do: the new match releases the old one's reservations.
  uint8_t freeSlots = 0;
  for (uint8_t slot = 1; slot < MAX_SLOTS; slot++) freeSlots += seats[slot].status == SeatStatus::Empty ? 1 : 0;
  botsNeeded = count < game.minSeats() ? static_cast<uint8_t>(game.minSeats() - count) : 0;
  return botsNeeded <= freeSlots;
}

bool TableSession::startGame(const GameId id, const uint32_t nowMs) {
  if (!isHostLike()) return false;
  auto game = createGame(id);
  auto freshView = createGame(id);
  if (!game || !freshView) return false;
  uint8_t slots[MAX_SLOTS];
  uint8_t count = 0;
  uint8_t botsNeeded = 0;
  if (!planSeats(*game, slots, count, botsNeeded)) return false;

  // Nothing below can fail. Players who stood up during the previous match
  // are forgotten, which frees their seats for any CPU players needed.
  releaseLeftSeats();
  for (uint8_t i = 0; i < botsNeeded; i++) slots[count++] = seatBot();

  authGame = std::move(game);
  viewGame = std::move(freshView);
  authGame->reset(count, rng.next());
  std::copy(slots, slots + count, gameSlots);
  gameSeats = count;
  gameId_ = id;
  phase_ = Phase::Playing;
  matchId++;
  rosterChanged();
  stateChanged(nowMs);
  flush();
  return true;
}

bool TableSession::viewIsCurrent() const {
  if (!viewGame || phase_ != Phase::Playing) return false;
  if (isHostLike()) return true;
  return haveState && stateMatch == matchId && viewGame->id() == gameId_;
}

void TableSession::returnToLobby() {
  if (!isHostLike() || phase_ == Phase::Lobby) return;
  phase_ = Phase::Lobby;
  // Seats freed mid-match were kept for their player; release them now.
  releaseLeftSeats();
  rosterChanged();
  flush();
}

void TableSession::rosterChanged() {
  rosterVersion++;
  rosterDirty = true;
  touch();
}

void TableSession::stateChanged(const uint32_t nowMs) {
  stateVersion++;
  stateDirty = true;
  lastMoveMs = nowMs;
  refreshHostView();
  touch();
}

void TableSession::refreshHostView() {
  if (!authGame || !viewGame) return;
  const int gameSeat = localSeat();
  viewLen = authGame->serialize(gameSeat >= 0 ? static_cast<uint8_t>(gameSeat) : SPECTATOR, viewBuf, sizeof(viewBuf));
  if (viewLen == 0 || !viewGame->deserialize(viewBuf, viewLen)) viewLen = 0;
}

size_t TableSession::viewBytes(uint8_t* out, const size_t capacity) const {
  if (!out || viewLen == 0 || viewLen > capacity) return 0;
  memcpy(out, viewBuf, viewLen);
  return viewLen;
}

void TableSession::flush() {
  if (role_ != Role::Host) {
    rosterDirty = false;
    stateDirty = false;
    return;
  }
  for (uint8_t slot = 1; slot < MAX_SLOTS; slot++) {
    if (seats[slot].status != SeatStatus::Present) continue;
    if (rosterDirty) sendRoster(slot);
    if (stateDirty && phase_ == Phase::Playing) sendState(slot);
  }
  rosterDirty = false;
  stateDirty = false;
}

void TableSession::runBots(const uint32_t nowMs) {
  if (phase_ != Phase::Playing || !authGame || authGame->over()) return;
  const int gameSeat = authGame->currentSeat();
  if (gameSeat < 0 || gameSeat >= gameSeats) return;
  const SeatInfo& s = seats[gameSlots[gameSeat]];
  const bool cpuMoves =
      s.status == SeatStatus::Bot || (botTakeover && (s.status == SeatStatus::Away || s.status == SeatStatus::Empty));
  if (!cpuMoves) return;
  const uint32_t delay = authGame->showingResult() ? BOT_REVEAL_DELAY_MS : BOT_DELAY_MS;
  if (nowMs - lastMoveMs < delay) return;
  uint8_t action[MAX_ACTION];
  const size_t len = authGame->botAction(static_cast<uint8_t>(gameSeat), rng, action, sizeof(action));
  if (len > 0 && authGame->apply(static_cast<uint8_t>(gameSeat), action, len)) {
    stateChanged(nowMs);
  } else {
    // Never spin on a bot that cannot move; try again after another delay.
    lastMoveMs = nowMs;
  }
}

// --- guest ---------------------------------------------------------------------

void TableSession::browse() {
  reset();
  role_ = Role::Guest;
  status_ = Status::Browsing;
  touch();
}

bool TableSession::join(const uint8_t tableIndex, const uint32_t nowMs) {
  if (role_ != Role::Guest || status_ != Status::Browsing || tableIndex >= tables) return false;
  const TableInfo& t = tableList[tableIndex];
  memcpy(hostMac, t.mac, MAC_LEN);
  tableId = t.tableId;
  status_ = Status::Joining;
  joinStartMs = nowMs;
  lastJoinMs = nowMs;
  lastHostHeardMs = nowMs;
  lastAckedMs = nowMs;
  haveRoster = false;
  haveState = false;
  sendJoin();
  touch();
  return true;
}

bool TableSession::submitAction(const uint8_t* action, const size_t len, const uint32_t nowMs) {
  if (!action || len == 0 || len > MAX_ACTION || phase_ != Phase::Playing) return false;
  if (isHostLike()) {
    const int gameSeat = localSeat();
    if (gameSeat < 0 || !authGame || !authGame->apply(static_cast<uint8_t>(gameSeat), action, len)) return false;
    stateChanged(nowMs);
    flush();
    return true;
  }
  if (role_ != Role::Guest || status_ != Status::Seated || pendingLen > 0 || localSeat() < 0 || !viewIsCurrent()) {
    return false;
  }
  actionSeq++;
  memcpy(pendingAction, action, len);
  pendingLen = static_cast<uint8_t>(len);
  pendingMatch = matchId;
  lastActionMs = nowMs;
  sendAction();
  touch();
  return true;
}

// --- packet handling -----------------------------------------------------------

void TableSession::onPacket(const uint8_t* mac, const uint8_t* data, const size_t len, const uint32_t nowMs) {
  if (!mac || !data || len > MAX_PACKET) return;
  Reader r(data, len);
  Header h;
  if (!readHeader(r, h)) return;
  if (role_ == Role::Host) {
    hostHandle(h, mac, r, nowMs);
    flush();
  } else if (role_ == Role::Guest) {
    guestHandle(h, mac, r, nowMs);
  }
}

uint8_t TableSession::slotForMac(const uint8_t* mac) const {
  for (uint8_t slot = 1; slot < MAX_SLOTS; slot++) {
    if (isSeated(seats[slot].status) && sameMac(seats[slot].mac, mac)) return slot;
  }
  return NO_SLOT;
}

void TableSession::hostHandle(const Header& h, const uint8_t* mac, Reader& r, const uint32_t nowMs) {
  if (h.tableId != tableId) return;
  if (h.type == MsgType::Join) {
    hostOnJoin(mac, r, nowMs);
    return;
  }

  const uint8_t slot = slotForMac(mac);
  if (slot == NO_SLOT || slot != h.srcSlot) {
    // A guest the host no longer seats (dropped while away): tell it to
    // rejoin rather than letting it talk into the void.
    if (h.type == MsgType::Ping || h.type == MsgType::Action) sendDeny(mac, DenyReason::Full);
    return;
  }
  SeatInfo& s = seats[slot];
  s.lastHeardMs = nowMs;
  if (s.status == SeatStatus::Away) {
    s.status = SeatStatus::Present;
    rosterChanged();
  }

  switch (h.type) {
    case MsgType::Ping: {
      s.ackRoster = r.u16();
      s.ackState = r.u16();
      if (!r.ok()) return;
      if (s.ackRoster != rosterVersion) sendRoster(slot);
      if (phase_ == Phase::Playing && s.ackState != stateVersion) sendState(slot);
      break;
    }
    case MsgType::Action: {
      const uint8_t match = r.u8();
      const uint16_t seq = r.u16();
      const uint8_t len = r.u8();
      uint8_t action[MAX_ACTION];
      if (!r.ok() || len == 0 || len > MAX_ACTION || !r.bytes(action, len)) return;
      if (seqNewer(seq, s.lastActionSeq)) {
        s.lastActionSeq = seq;
        // A move meant for an earlier match is consumed (and acknowledged
        // below) but never applied to this one.
        if (match != matchId || phase_ != Phase::Playing) {
          if (phase_ == Phase::Playing) sendState(slot);
          return;
        }
        const int gameSeat = seatOfSlot(slot);
        if (gameSeat >= 0 && authGame && authGame->apply(static_cast<uint8_t>(gameSeat), action, len)) {
          stateChanged(nowMs);
          return;  // flush() carries the new state (and the ack) to everyone
        }
      }
      // Duplicate or rejected: the state still acknowledges the sequence, so
      // the guest stops retrying.
      if (phase_ == Phase::Playing) sendState(slot);
      break;
    }
    case MsgType::Leave: {
      const int gameSeat = seatOfSlot(slot);
      if (gameSeat >= 0 && authGame && !authGame->over()) {
        authGame->seatLeft(static_cast<uint8_t>(gameSeat));
        stateChanged(nowMs);
      }
      // A seat still referenced by the match keeps its name for the scoreboard.
      if (gameSeat >= 0) {
        s.status = SeatStatus::Empty;
        memset(s.mac, 0, MAC_LEN);
      } else {
        s = SeatInfo{};
      }
      rosterChanged();
      break;
    }
    default:
      break;
  }
}

void TableSession::hostOnJoin(const uint8_t* mac, Reader& r, const uint32_t nowMs) {
  char name[NAME_LEN + 1];
  r.name(name);
  if (!r.ok()) return;

  uint8_t slot = slotForMac(mac);
  if (slot == NO_SLOT) {
    for (uint8_t candidate = 1; candidate < MAX_SLOTS; candidate++) {
      // Seats tied to the running match stay reserved for their players.
      if (seats[candidate].status == SeatStatus::Empty && !(phase_ == Phase::Playing && slotInGame(candidate))) {
        slot = candidate;
        break;
      }
    }
    if (slot == NO_SLOT) {
      sendDeny(mac, DenyReason::Full);
      return;
    }
    seats[slot] = SeatInfo{};
    memcpy(seats[slot].mac, mac, MAC_LEN);
  }
  SeatInfo& s = seats[slot];
  s.status = SeatStatus::Present;
  s.lastHeardMs = nowMs;
  memcpy(s.name, name, sizeof(name));
  rosterChanged();
  sendWelcome(slot);
  // The joiner gets the table right away instead of waiting for its first ping.
  sendRoster(slot);
  if (phase_ == Phase::Playing) sendState(slot);
}

void TableSession::guestHandle(const Header& h, const uint8_t* mac, Reader& r, const uint32_t nowMs) {
  if (h.type == MsgType::Beacon) {
    guestOnBeacon(h, mac, r, nowMs);
    return;
  }
  if (h.type == MsgType::Close && status_ == Status::Browsing) {
    // A closed table leaves the list at once rather than when its beacon expires.
    uint8_t kept = 0;
    for (uint8_t i = 0; i < tables; i++) {
      if (tableList[i].tableId != h.tableId || !sameMac(tableList[i].mac, mac)) tableList[kept++] = tableList[i];
    }
    if (kept != tables) {
      tables = kept;
      touch();
    }
    return;
  }
  if (!fromHost(h, mac)) return;
  lastHostHeardMs = nowMs;
  lastAckedMs = nowMs;

  switch (h.type) {
    case MsgType::Welcome: {
      const uint8_t slot = r.u8();
      const uint16_t lastSeq = r.u16();
      if (!r.ok() || slot == HOST_SLOT || slot >= MAX_SLOTS) return;
      if (status_ != Status::Joining && status_ != Status::Reconnecting) return;
      localSlot_ = slot;
      // Continue numbering after whatever the host already processed, so a
      // move sent before a dropout is neither lost nor applied twice.
      if (pendingLen > 0 && !seqNewer(actionSeq, lastSeq)) pendingLen = 0;
      if (seqNewer(lastSeq, actionSeq)) actionSeq = lastSeq;
      status_ = Status::Seated;
      lastPingMs = nowMs;
      touch();
      break;
    }
    case MsgType::Deny: {
      const auto reason = static_cast<DenyReason>(r.u8());
      if (status_ == Status::Joining || status_ == Status::Reconnecting) {
        // Reconnecting too: our seat was freed and the table has since filled.
        deny = reason;
        status_ = Status::Denied;
        pendingLen = 0;
        touch();
      } else if (status_ == Status::Seated) {
        // The host dropped our seat; sit back down.
        status_ = Status::Reconnecting;
        lastJoinMs = nowMs - JOIN_RETRY_MS;
        touch();
      }
      break;
    }
    case MsgType::Roster:
      guestOnRoster(r);
      break;
    case MsgType::State:
      guestOnState(r);
      break;
    case MsgType::Close:
      if (status_ != Status::Browsing) {
        status_ = Status::Closed;
        pendingLen = 0;
        touch();
      }
      break;
    default:
      break;
  }
}

bool TableSession::fromHost(const Header& h, const uint8_t* mac) const {
  return status_ != Status::Browsing && h.tableId == tableId && sameMac(mac, hostMac);
}

void TableSession::guestOnBeacon(const Header& h, const uint8_t* mac, Reader& r, const uint32_t nowMs) {
  TableInfo info;
  memcpy(info.mac, mac, MAC_LEN);
  info.tableId = h.tableId;
  r.name(info.hostName);
  info.players = r.u8();
  info.maxPlayers = r.u8();
  const uint8_t game = r.u8();
  const uint8_t rawPhase = r.u8();
  const uint8_t present = r.u8();
  if (!r.ok()) return;
  info.game = validGameId(game) ? static_cast<GameId>(game) : GameId::None;
  info.phase = rawPhase == static_cast<uint8_t>(Phase::Playing) ? Phase::Playing : Phase::Lobby;
  info.lastSeenMs = nowMs;

  if (fromHost(h, mac)) {
    lastHostHeardMs = nowMs;
    if (status_ == Status::Seated && localSlot_ < MAX_SLOTS && (present >> localSlot_) & 1u) lastAckedMs = nowMs;
  }
  if (status_ != Status::Browsing) return;

  for (uint8_t i = 0; i < tables; i++) {
    TableInfo& t = tableList[i];
    if (t.tableId != info.tableId || !sameMac(t.mac, mac)) continue;
    const bool changed = t.players != info.players || t.game != info.game || t.phase != info.phase ||
                         strcmp(t.hostName, info.hostName) != 0;
    t = info;
    if (changed) touch();
    return;
  }
  if (tables < MAX_TABLES) {
    tableList[tables++] = info;
    touch();
  }
}

void TableSession::guestOnRoster(Reader& r) {
  const uint16_t version = r.u16();
  const uint8_t game = r.u8();
  const uint8_t rawPhase = r.u8();
  const uint8_t match = r.u8();
  const uint8_t count = r.u8();
  uint8_t slots[MAX_SLOTS];
  r.bytes(slots, sizeof(slots));
  SeatInfo incoming[MAX_SLOTS];
  for (auto& s : incoming) {
    const uint8_t rawStatus = r.u8();
    r.name(s.name);
    s.status =
        rawStatus <= static_cast<uint8_t>(SeatStatus::Bot) ? static_cast<SeatStatus>(rawStatus) : SeatStatus::Empty;
  }
  if (!r.ok() || count > MAX_SLOTS) return;
  if (haveRoster && !seqNewer(version, rosterVersion)) return;
  for (uint8_t i = 0; i < count; i++) {
    if (slots[i] >= MAX_SLOTS) return;
  }

  haveRoster = true;
  rosterVersion = version;
  for (uint8_t slot = 0; slot < MAX_SLOTS; slot++) {
    seats[slot].status = incoming[slot].status;
    memcpy(seats[slot].name, incoming[slot].name, sizeof(seats[slot].name));
  }
  gameId_ = validGameId(game) ? static_cast<GameId>(game) : GameId::None;
  phase_ = rawPhase == static_cast<uint8_t>(Phase::Playing) ? Phase::Playing : Phase::Lobby;
  // A new match (or the lobby) makes any move still in flight moot.
  if (match != matchId || phase_ == Phase::Lobby) pendingLen = 0;
  matchId = match;
  std::copy(slots, slots + count, gameSlots);
  gameSeats = count;
  touch();
}

void TableSession::guestOnState(Reader& r) {
  const uint16_t version = r.u16();
  const uint8_t game = r.u8();
  const uint8_t match = r.u8();
  const uint16_t ackSeq = r.u16();
  const uint8_t len = r.u8();
  if (!r.ok() || len > MAX_GAME_STATE || r.remaining() < len || !validGameId(game)) return;

  bool changed = false;
  if (pendingLen > 0 && !seqNewer(actionSeq, ackSeq)) {
    pendingLen = 0;
    changed = true;
  }
  if (!haveState || seqNewer(version, stateVersion)) {
    const auto id = static_cast<GameId>(game);
    // Load into a fresh instance when the game changes, and swap it in only
    // once it parsed, so view() and viewBytes() always describe one state.
    std::unique_ptr<Game> fresh;
    Game* target = viewGame.get();
    if (!target || target->id() != id) {
      fresh = createGame(id);
      target = fresh.get();
    }
    if (target && target->deserialize(r.cursor(), len)) {
      if (fresh) viewGame = std::move(fresh);
      memcpy(viewBuf, r.cursor(), len);
      viewLen = len;
      stateVersion = version;
      stateMatch = match;
      haveState = true;
      changed = true;
    }
  }
  if (changed) touch();
}

// --- timers --------------------------------------------------------------------

void TableSession::tick(const uint32_t nowMs) {
  if (role_ == Role::Host) {
    if (nowMs - lastBeaconMs >= BEACON_MS) {
      lastBeaconMs = nowMs;
      sendBeacon();
      // Second healing path on the host's own clock, so recovery never hinges
      // on the one reply that follows a guest's ping.
      for (uint8_t slot = 1; slot < MAX_SLOTS; slot++) {
        const SeatInfo& s = seats[slot];
        if (s.status != SeatStatus::Present) continue;
        if (s.ackRoster != rosterVersion) sendRoster(slot);
        if (phase_ == Phase::Playing && s.ackState != stateVersion) sendState(slot);
      }
    }
    for (uint8_t slot = 1; slot < MAX_SLOTS; slot++) {
      SeatInfo& s = seats[slot];
      if (s.status == SeatStatus::Present && nowMs - s.lastHeardMs > PEER_TIMEOUT_MS) {
        s.status = SeatStatus::Away;
        rosterChanged();
      } else if (s.status == SeatStatus::Away && nowMs - s.lastHeardMs > AWAY_DROP_MS &&
                 !(phase_ == Phase::Playing && slotInGame(slot))) {
        s = SeatInfo{};
        rosterChanged();
      }
    }
    runBots(nowMs);
    flush();
  } else if (role_ == Role::Solo) {
    runBots(nowMs);
  } else if (role_ == Role::Guest) {
    switch (status_) {
      case Status::Browsing: {
        uint8_t kept = 0;
        for (uint8_t i = 0; i < tables; i++) {
          if (nowMs - tableList[i].lastSeenMs <= TABLE_EXPIRE_MS) tableList[kept++] = tableList[i];
        }
        if (kept != tables) {
          tables = kept;
          touch();
        }
        break;
      }
      case Status::Joining:
        if (nowMs - joinStartMs > JOIN_TIMEOUT_MS) {
          status_ = Status::Lost;
          touch();
        } else if (nowMs - lastJoinMs >= JOIN_RETRY_MS) {
          lastJoinMs = nowMs;
          sendJoin();
        }
        break;
      case Status::Seated:
        if (nowMs - lastAckedMs > PEER_TIMEOUT_MS) {
          status_ = Status::Reconnecting;
          lastJoinMs = nowMs - JOIN_RETRY_MS;
          touch();
          break;
        }
        if (nowMs - lastPingMs >= PING_MS) {
          lastPingMs = nowMs;
          sendPing();
        }
        if (pendingLen > 0 && nowMs - lastActionMs >= ACTION_RETRY_MS) {
          lastActionMs = nowMs;
          sendAction();
        }
        break;
      case Status::Reconnecting:
        if (nowMs - lastHostHeardMs > HOST_LOST_MS) {
          status_ = Status::Lost;
          pendingLen = 0;
          touch();
        } else if (nowMs - lastJoinMs >= JOIN_RETRY_MS) {
          lastJoinMs = nowMs;
          sendJoin();
        }
        break;
      default:
        break;
    }
  }
}

// --- senders -------------------------------------------------------------------

bool TableSession::send(const uint8_t* mac, const uint8_t* data, const size_t len) {
  return link && len > 0 && link->send(mac, data, len);
}

void TableSession::sendBeacon() {
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  writeHeader(w, MsgType::Beacon, tableId, HOST_SLOT);
  w.name(localName);
  w.u8(playerCount());
  w.u8(MAX_SLOTS);
  w.u8(static_cast<uint8_t>(gameId_));
  w.u8(static_cast<uint8_t>(phase_));
  uint8_t present = 0;
  for (uint8_t slot = 1; slot < MAX_SLOTS; slot++) {
    if (seats[slot].status == SeatStatus::Present) present |= static_cast<uint8_t>(1u << slot);
  }
  w.u8(present);
  if (w.ok()) send(BROADCAST_MAC, buf, w.size());
}

void TableSession::sendRoster(const uint8_t slot) {
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  writeHeader(w, MsgType::Roster, tableId, HOST_SLOT);
  w.u16(rosterVersion);
  w.u8(static_cast<uint8_t>(gameId_));
  w.u8(static_cast<uint8_t>(phase_));
  w.u8(matchId);
  w.u8(phase_ == Phase::Playing ? gameSeats : 0);
  w.bytes(gameSlots, sizeof(gameSlots));
  for (const auto& s : seats) {
    w.u8(static_cast<uint8_t>(s.status));
    w.name(s.name);
  }
  if (w.ok()) send(seats[slot].mac, buf, w.size());
}

void TableSession::sendState(const uint8_t slot) {
  if (!authGame) return;
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  writeHeader(w, MsgType::State, tableId, HOST_SLOT);
  w.u16(stateVersion);
  w.u8(static_cast<uint8_t>(gameId_));
  w.u8(matchId);
  w.u16(seats[slot].lastActionSeq);
  if (!w.ok()) return;
  // The view is serialized straight into the packet after its length byte.
  const size_t lenAt = w.size();
  const int gameSeat = seatOfSlot(slot);
  const size_t capacity = std::min(sizeof(buf) - lenAt - 1, MAX_GAME_STATE);
  const size_t len =
      authGame->serialize(gameSeat >= 0 ? static_cast<uint8_t>(gameSeat) : SPECTATOR, buf + lenAt + 1, capacity);
  if (len == 0) return;
  buf[lenAt] = static_cast<uint8_t>(len);
  send(seats[slot].mac, buf, lenAt + 1 + len);
}

void TableSession::sendWelcome(const uint8_t slot) {
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  writeHeader(w, MsgType::Welcome, tableId, HOST_SLOT);
  w.u8(slot);
  w.u16(seats[slot].lastActionSeq);
  if (w.ok()) send(seats[slot].mac, buf, w.size());
}

void TableSession::sendDeny(const uint8_t* mac, const DenyReason reason) {
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  writeHeader(w, MsgType::Deny, tableId, HOST_SLOT);
  w.u8(static_cast<uint8_t>(reason));
  if (w.ok()) send(mac, buf, w.size());
}

void TableSession::sendClose(const uint8_t* mac) {
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  writeHeader(w, MsgType::Close, tableId, HOST_SLOT);
  if (w.ok()) send(mac, buf, w.size());
}

void TableSession::sendJoin() {
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  writeHeader(w, MsgType::Join, tableId, NO_SLOT);
  w.name(localName);
  if (w.ok()) send(hostMac, buf, w.size());
}

void TableSession::sendPing() {
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  writeHeader(w, MsgType::Ping, tableId, localSlot_);
  w.u16(haveRoster ? rosterVersion : 0);
  w.u16(haveState ? stateVersion : 0);
  if (w.ok()) send(hostMac, buf, w.size());
}

void TableSession::sendAction() {
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  writeHeader(w, MsgType::Action, tableId, localSlot_);
  w.u8(pendingMatch);
  w.u16(actionSeq);
  w.u8(pendingLen);
  w.bytes(pendingAction, pendingLen);
  if (w.ok()) send(hostMac, buf, w.size());
}

void TableSession::sendLeave() {
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  writeHeader(w, MsgType::Leave, tableId, localSlot_);
  if (w.ok()) send(hostMac, buf, w.size());
}

}  // namespace table
