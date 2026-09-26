#include "MurderMystery.h"

#include <algorithm>

namespace table {

namespace {

constexpr uint32_t ALL_CARDS = (1u << MurderMystery::CARD_COUNT) - 1u;

bool validCard(const uint8_t card) { return card < MurderMystery::CARD_COUNT || card == MurderMystery::NO_CARD; }

size_t encodeTheory(uint8_t* out, const MurderMystery::Kind kind, const uint8_t suspect, const uint8_t weapon,
                    const uint8_t room) {
  out[0] = static_cast<uint8_t>(kind);
  out[1] = suspect;
  out[2] = weapon;
  out[3] = room;
  return 4;
}

}  // namespace

size_t MurderMystery::encodeSuggest(uint8_t* out, const uint8_t suspect, const uint8_t weapon, const uint8_t room) {
  return encodeTheory(out, Kind::Suggest, suspect, weapon, room);
}

size_t MurderMystery::encodeAccuse(uint8_t* out, const uint8_t suspect, const uint8_t weapon, const uint8_t room) {
  return encodeTheory(out, Kind::Accuse, suspect, weapon, room);
}

size_t MurderMystery::encodeShow(uint8_t* out, const uint8_t card) {
  out[0] = static_cast<uint8_t>(Kind::Show);
  out[1] = card;
  return 2;
}

size_t MurderMystery::encodeContinue(uint8_t* out) {
  out[0] = static_cast<uint8_t>(Kind::Continue);
  return 1;
}

void MurderMystery::reset(const uint8_t seatCount, const uint32_t seed) {
  seats = std::clamp<uint8_t>(seatCount, minSeats(), maxSeats());
  phase_ = Phase::Turn;
  refuter = NO_SEAT;
  winner = NO_SEAT;
  eliminatedMask = 0;
  departedMask = 0;
  shown = NO_CARD;
  revealed = 0;
  events = 0;
  viewerSeat_ = SPECTATOR;
  std::fill(std::begin(pending), std::end(pending), NO_CARD);
  std::fill(std::begin(hand), std::end(hand), 0u);
  std::fill(std::begin(seen), std::end(seen), 0u);
  std::fill(std::begin(inferred), std::end(inferred), 0u);
  std::fill(std::begin(handCount), std::end(handCount), 0);
  std::fill(std::begin(history), std::end(history), Event{});

  Rng rng(seed);
  for (uint8_t c = 0; c < CATEGORIES; c++) solution[c] = cardOf(c, static_cast<uint8_t>(rng.below(PER_CATEGORY)));

  uint8_t deck[CARD_COUNT];
  uint8_t n = 0;
  for (uint8_t card = 0; card < CARD_COUNT; card++) {
    if (card != solution[categoryOf(card)]) deck[n++] = card;
  }
  for (uint8_t i = static_cast<uint8_t>(n - 1); i > 0; i--) std::swap(deck[i], deck[rng.below(i + 1u)]);

  turn = static_cast<uint8_t>(seed % seats);
  for (uint8_t i = 0; i < n; i++) {
    const uint8_t s = static_cast<uint8_t>((turn + i) % seats);
    hand[s] |= cardBit(deck[i]);
    handCount[s]++;
  }
}

bool MurderMystery::active(const uint8_t seat) const {
  return seat < seats && (((eliminatedMask | departedMask) >> seat) & 1u) == 0;
}

uint8_t MurderMystery::activeCount() const {
  uint8_t n = 0;
  for (uint8_t s = 0; s < seats; s++) n += active(s) ? 1 : 0;
  return n;
}

uint8_t MurderMystery::nextActive(const uint8_t from) const {
  for (uint8_t i = 1; i <= seats; i++) {
    const uint8_t s = static_cast<uint8_t>((from + i) % seats);
    if (active(s)) return s;
  }
  return from;
}

uint32_t MurderMystery::pendingMask() const {
  uint32_t mask = 0;
  for (const uint8_t card : pending) {
    if (card < CARD_COUNT) mask |= cardBit(card);
  }
  return mask;
}

int MurderMystery::currentSeat() const {
  switch (phase_) {
    case Phase::Turn:
    case Phase::Result:
      return turn;
    case Phase::Refute:
      return refuter;
    case Phase::Over:
      break;
  }
  return -1;
}

uint8_t MurderMystery::refuteOptions(const uint8_t seat, uint8_t* out) const {
  uint8_t n = 0;
  for (const uint8_t card : pending) {
    if (card < CARD_COUNT && (handOf(seat) & cardBit(card))) out[n++] = card;
  }
  return n;
}

void MurderMystery::pushEvent(const EventKind kind, const uint8_t seat, const uint8_t* cards,
                              const uint8_t refuterSeat) {
  std::copy_backward(history, history + LOG_LEN - 1, history + LOG_LEN);
  Event& e = history[0];
  e.kind = kind;
  e.seat = seat;
  std::copy(cards, cards + CATEGORIES, e.cards);
  e.refuter = refuterSeat;
  events = static_cast<uint8_t>(std::min<int>(events + 1, LOG_LEN));
}

void MurderMystery::showCard(const uint8_t card) {
  shown = card;
  seen[turn] |= cardBit(card);
  phase_ = Phase::Result;
}

void MurderMystery::startRefute() {
  // Eliminated players still answer; players who left have their hands face
  // up instead.
  const uint32_t mask = pendingMask();
  refuter = NO_SEAT;
  for (uint8_t i = 1; i < seats; i++) {
    const uint8_t s = static_cast<uint8_t>((turn + i) % seats);
    if (!departed(s) && (hand[s] & mask)) {
      refuter = s;
      break;
    }
  }
  // The pending suggestion is always the newest event.
  history[0].refuter = refuter;
  if (refuter == NO_SEAT) {
    // Nobody else holds these cards, so each one the suggester lacks is in
    // the case file and rules out the rest of its category.
    for (const uint8_t card : pending) {
      if (!((hand[turn] | revealed) & cardBit(card))) inferred[turn] |= categoryMask(categoryOf(card)) & ~cardBit(card);
    }
    phase_ = Phase::Result;
    return;
  }
  // The refuter always picks, even with one match: showing a lone card at
  // once would tell the table how many of the three it holds.
  phase_ = Phase::Refute;
}

bool MurderMystery::apply(const uint8_t seat, const uint8_t* action, const size_t len) {
  if (!action || len < 1 || seat >= seats) return false;
  switch (static_cast<Kind>(action[0])) {
    case Kind::Suggest:
    case Kind::Accuse: {
      if (phase_ != Phase::Turn || seat != turn || len < 1 + CATEGORIES) return false;
      uint8_t cards[CATEGORIES];
      for (uint8_t c = 0; c < CATEGORIES; c++) {
        if (action[1 + c] >= PER_CATEGORY) return false;
        cards[c] = cardOf(c, action[1 + c]);
      }
      std::copy(cards, cards + CATEGORIES, pending);
      shown = NO_CARD;
      refuter = NO_SEAT;
      if (static_cast<Kind>(action[0]) == Kind::Suggest) {
        pushEvent(EventKind::Suggestion, seat, cards, NO_SEAT);
        startRefute();
        return true;
      }
      const bool right = std::equal(cards, cards + CATEGORIES, solution);
      pushEvent(right ? EventKind::RightAccusation : EventKind::WrongAccusation, seat, cards, NO_SEAT);
      if (right) {
        winner = seat;
        phase_ = Phase::Over;
        return true;
      }
      eliminatedMask |= static_cast<uint8_t>(1u << seat);
      phase_ = activeCount() == 0 ? Phase::Over : Phase::Result;
      return true;
    }
    case Kind::Show: {
      if (phase_ != Phase::Refute || seat != refuter || len < 2 || action[1] >= CARD_COUNT) return false;
      if (!(hand[seat] & pendingMask() & cardBit(action[1]))) return false;
      showCard(action[1]);
      return true;
    }
    case Kind::Continue: {
      if (phase_ != Phase::Result || seat != turn) return false;
      turn = nextActive(turn);
      phase_ = Phase::Turn;
      refuter = NO_SEAT;
      shown = NO_CARD;
      std::fill(std::begin(pending), std::end(pending), NO_CARD);
      return true;
    }
  }
  return false;
}

void MurderMystery::seatLeft(const uint8_t seat) {
  if (seat >= seats || phase_ == Phase::Over || departed(seat)) return;
  departedMask |= static_cast<uint8_t>(1u << seat);
  // As at a real table, a player who leaves lays their cards face up.
  revealed |= hand[seat];
  if (activeCount() == 0) {
    phase_ = Phase::Over;
    return;
  }
  if (seat == turn) {
    turn = nextActive(seat);
    phase_ = Phase::Turn;
    refuter = NO_SEAT;
    shown = NO_CARD;
    std::fill(std::begin(pending), std::end(pending), NO_CARD);
    return;
  }
  // The next player clockwise answers in their place.
  if (phase_ == Phase::Refute && seat == refuter) startRefute();
}

size_t MurderMystery::botAction(const uint8_t seat, Rng& rng, uint8_t* out, const size_t capacity) const {
  if (capacity < 1 + CATEGORIES || seat >= seats) return 0;
  switch (phase_) {
    case Phase::Refute: {
      if (seat != refuter) return 0;
      uint8_t options[CATEGORIES];
      const uint8_t n = refuteOptions(seat, options);
      if (n == 0) return 0;
      // A card the suggester has already seen gives nothing away.
      uint8_t known[CATEGORIES];
      uint8_t k = 0;
      for (uint8_t i = 0; i < n; i++) {
        if (seen[turn] & cardBit(options[i])) known[k++] = options[i];
      }
      return encodeShow(out, k > 0 ? known[rng.below(k)] : options[rng.below(n)]);
    }
    case Phase::Result:
      return seat == turn ? encodeContinue(out) : 0;
    case Phase::Turn: {
      if (seat != turn) return 0;
      const uint32_t known = hand[seat] | seen[seat] | inferred[seat] | revealed;
      uint8_t picks[CATEGORIES];
      bool sure = true;
      for (uint8_t c = 0; c < CATEGORIES; c++) {
        uint32_t open = categoryMask(c) & ~known;
        // Cannot happen with consistent knowledge; never stall on it.
        if (open == 0) open = categoryMask(c);
        const int count = __builtin_popcount(open);
        sure = sure && count == 1;
        uint32_t skip = rng.below(static_cast<uint32_t>(count));
        for (uint8_t i = 0; i < PER_CATEGORY; i++) {
          if (!(open & cardBit(cardOf(c, i)))) continue;
          if (skip-- == 0) {
            picks[c] = i;
            break;
          }
        }
      }
      return sure ? encodeAccuse(out, picks[0], picks[1], picks[2]) : encodeSuggest(out, picks[0], picks[1], picks[2]);
    }
    case Phase::Over:
      break;
  }
  return 0;
}

size_t MurderMystery::serialize(const uint8_t viewerSeat, uint8_t* out, const size_t capacity) const {
  const bool seated = viewerSeat < seats;
  Writer w(out, capacity);
  w.u8(seats);
  w.u8(static_cast<uint8_t>(phase_));
  w.u8(turn);
  w.u8(refuter);
  w.u8(winner);
  w.u8(eliminatedMask);
  w.u8(departedMask);
  w.u8(viewerSeat);
  w.bytes(handCount, sizeof(handCount));
  w.u32(seated ? hand[viewerSeat] : 0);
  w.u32(seated ? seen[viewerSeat] : 0);
  w.u32(revealed);
  w.bytes(pending, sizeof(pending));
  // Only the two players in the exchange learn which card was shown.
  const bool inExchange = seated && (viewerSeat == turn || viewerSeat == refuter);
  w.u8(inExchange ? shown : NO_CARD);
  // A wrong accuser gets to see the case file, as in the board game.
  const bool caseOpen = phase_ == Phase::Over || (seated && eliminated(viewerSeat));
  for (const uint8_t card : solution) w.u8(caseOpen ? card : NO_CARD);
  w.u8(events);
  for (const Event& e : history) {
    w.u8(static_cast<uint8_t>(e.kind));
    w.u8(e.seat);
    w.bytes(e.cards, sizeof(e.cards));
    w.u8(e.refuter);
  }
  return w.ok() ? w.size() : 0;
}

bool MurderMystery::deserialize(const uint8_t* data, const size_t len) {
  Reader r(data, len);
  const uint8_t rawSeats = r.u8();
  const uint8_t rawPhase = r.u8();
  const uint8_t rawTurn = r.u8();
  const uint8_t rawRefuter = r.u8();
  const uint8_t rawWinner = r.u8();
  const uint8_t rawEliminated = r.u8();
  const uint8_t rawDeparted = r.u8();
  const uint8_t rawViewer = r.u8();
  uint8_t rawCounts[MAX_SLOTS];
  r.bytes(rawCounts, sizeof(rawCounts));
  const uint32_t rawHand = r.u32();
  const uint32_t rawSeen = r.u32();
  const uint32_t rawRevealed = r.u32();
  uint8_t rawPending[CATEGORIES];
  r.bytes(rawPending, sizeof(rawPending));
  const uint8_t rawShown = r.u8();
  uint8_t rawSolution[CATEGORIES];
  r.bytes(rawSolution, sizeof(rawSolution));
  const uint8_t rawEvents = r.u8();
  Event rawHistory[LOG_LEN];
  for (Event& e : rawHistory) {
    e.kind = static_cast<EventKind>(r.u8());
    e.seat = r.u8();
    r.bytes(e.cards, sizeof(e.cards));
    e.refuter = r.u8();
  }
  const auto seatOrNone = [rawSeats](const uint8_t s) { return s < rawSeats || s == NO_SEAT; };
  if (!r.ok() || rawSeats < minSeats() || rawSeats > maxSeats() || rawPhase > static_cast<uint8_t>(Phase::Over) ||
      rawTurn >= rawSeats || !seatOrNone(rawRefuter) || !seatOrNone(rawWinner) || rawEvents > LOG_LEN ||
      ((rawHand | rawSeen | rawRevealed) & ~ALL_CARDS) || !validCard(rawShown)) {
    return false;
  }
  for (uint8_t c = 0; c < CATEGORIES; c++) {
    if (!validCard(rawPending[c]) || !validCard(rawSolution[c])) return false;
  }
  for (uint8_t i = 0; i < rawEvents; i++) {
    const Event& e = rawHistory[i];
    const auto kind = static_cast<uint8_t>(e.kind);
    if (kind < static_cast<uint8_t>(EventKind::Suggestion) || kind > static_cast<uint8_t>(EventKind::RightAccusation) ||
        e.seat >= rawSeats || !seatOrNone(e.refuter)) {
      return false;
    }
    for (const uint8_t card : e.cards) {
      if (card >= CARD_COUNT) return false;
    }
  }
  seats = rawSeats;
  phase_ = static_cast<Phase>(rawPhase);
  turn = rawTurn;
  refuter = rawRefuter;
  winner = rawWinner;
  eliminatedMask = rawEliminated;
  departedMask = rawDeparted;
  viewerSeat_ = rawViewer;
  std::copy(rawCounts, rawCounts + MAX_SLOTS, handCount);
  std::fill(std::begin(hand), std::end(hand), 0u);
  std::fill(std::begin(seen), std::end(seen), 0u);
  std::fill(std::begin(inferred), std::end(inferred), 0u);
  if (rawViewer < rawSeats) {
    hand[rawViewer] = rawHand;
    seen[rawViewer] = rawSeen;
  }
  revealed = rawRevealed;
  std::copy(rawPending, rawPending + CATEGORIES, pending);
  shown = rawShown;
  std::copy(rawSolution, rawSolution + CATEGORIES, solution);
  events = rawEvents;
  std::copy(rawHistory, rawHistory + LOG_LEN, history);
  return true;
}

}  // namespace table
