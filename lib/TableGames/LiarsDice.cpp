#include "LiarsDice.h"

#include <algorithm>

namespace table {

size_t LiarsDice::encodeBid(uint8_t* out, const uint8_t quantity, const uint8_t face) {
  out[0] = static_cast<uint8_t>(Kind::Bid);
  out[1] = quantity;
  out[2] = face;
  return 3;
}

size_t LiarsDice::encodeLiar(uint8_t* out) {
  out[0] = static_cast<uint8_t>(Kind::Liar);
  return 1;
}

size_t LiarsDice::encodeNextRound(uint8_t* out) {
  out[0] = static_cast<uint8_t>(Kind::NextRound);
  return 1;
}

void LiarsDice::reset(const uint8_t seatCount, const uint32_t seed) {
  seats = std::clamp<uint8_t>(seatCount, minSeats(), maxSeats());
  rng = Rng(seed);
  for (uint8_t s = 0; s < MAX_SLOTS; s++) diceCount[s] = s < seats ? START_DICE : 0;
  starter = static_cast<uint8_t>(seed % seats);
  result = Result{};
  roundNo = 0;
  viewerSeat_ = SPECTATOR;
  rollAll();
}

void LiarsDice::rollAll() {
  for (uint8_t s = 0; s < MAX_SLOTS; s++) {
    rolledCount[s] = diceCount[s];
    for (uint8_t i = 0; i < START_DICE; i++) {
      dice[s][i] = i < diceCount[s] ? static_cast<uint8_t>(1 + rng.below(6)) : 0;
    }
    // Sorted cups are easier to read at a glance.
    std::sort(dice[s], dice[s] + diceCount[s]);
  }
  bidQty = 0;
  bidFace_ = 0;
  bidder = NO_SEAT;
  turn = starter;
  phase_ = Phase::Bidding;
  roundNo++;
}

uint8_t LiarsDice::nextActive(const uint8_t from) const {
  for (uint8_t i = 1; i <= seats; i++) {
    const uint8_t s = static_cast<uint8_t>((from + i) % seats);
    if (diceCount[s] > 0) return s;
  }
  return from;
}

uint8_t LiarsDice::activeCount() const {
  uint8_t n = 0;
  for (uint8_t s = 0; s < seats; s++) n += diceCount[s] > 0 ? 1 : 0;
  return n;
}

uint8_t LiarsDice::totalDice() const {
  uint8_t n = 0;
  for (uint8_t s = 0; s < seats; s++) n += rolledCount[s];
  return n;
}

uint8_t LiarsDice::countFace(const uint8_t face) const {
  uint8_t n = 0;
  for (uint8_t s = 0; s < seats; s++) {
    for (uint8_t i = 0; i < rolledCount[s]; i++) {
      if (dice[s][i] == face || dice[s][i] == 1) n++;
    }
  }
  return n;
}

int LiarsDice::currentSeat() const {
  switch (phase_) {
    case Phase::Bidding:
      return turn;
    case Phase::Reveal:
      return starter;
    case Phase::Over:
      break;
  }
  return -1;
}

uint8_t LiarsDice::winnerMask() const {
  if (phase_ != Phase::Over) return 0;
  uint8_t mask = 0;
  for (uint8_t s = 0; s < seats; s++) {
    if (diceCount[s] > 0) mask |= static_cast<uint8_t>(1u << s);
  }
  return mask;
}

uint8_t LiarsDice::minQuantityFor(const uint8_t face) const {
  if (face < 2 || face > 6) return 0;
  uint8_t q = 1;
  if (hasBid()) q = face > bidFace_ ? bidQty : static_cast<uint8_t>(bidQty + 1);
  return q <= totalDice() ? q : 0;
}

bool LiarsDice::bidIsLegal(const uint8_t quantity, const uint8_t face) const {
  const uint8_t minQ = minQuantityFor(face);
  return minQ != 0 && quantity >= minQ && quantity <= totalDice();
}

bool LiarsDice::apply(const uint8_t seat, const uint8_t* action, const size_t len) {
  if (!action || len < 1 || seat >= seats) return false;
  switch (static_cast<Kind>(action[0])) {
    case Kind::Bid: {
      if (phase_ != Phase::Bidding || seat != turn || len < 3 || !bidIsLegal(action[1], action[2])) return false;
      bidQty = action[1];
      bidFace_ = action[2];
      bidder = seat;
      turn = nextActive(seat);
      return true;
    }
    case Kind::Liar: {
      if (phase_ != Phase::Bidding || seat != turn || !hasBid()) return false;
      result.valid = true;
      result.challenger = seat;
      result.bidder = bidder;
      result.quantity = bidQty;
      result.face = bidFace_;
      result.actual = countFace(bidFace_);
      result.loser = result.actual >= bidQty ? seat : bidder;
      if (diceCount[result.loser] > 0) diceCount[result.loser]--;
      starter = diceCount[result.loser] > 0 ? result.loser : nextActive(result.loser);
      phase_ = activeCount() <= 1 ? Phase::Over : Phase::Reveal;
      return true;
    }
    case Kind::NextRound: {
      // Whoever still holds dice may roll on; the starter is the default.
      if (phase_ != Phase::Reveal || diceCount[seat] == 0) return false;
      rollAll();
      return true;
    }
  }
  return false;
}

void LiarsDice::seatLeft(const uint8_t seat) {
  if (seat >= seats || phase_ == Phase::Over || diceCount[seat] == 0) return;
  diceCount[seat] = 0;
  rolledCount[seat] = 0;
  if (activeCount() <= 1) {
    phase_ = Phase::Over;
    return;
  }
  if (turn == seat) turn = nextActive(seat);
  if (starter == seat) starter = nextActive(seat);
  // A bid nobody can pay for would hand the next caller a free challenge;
  // withdraw it so the player to move opens afresh.
  if (bidder == seat && phase_ == Phase::Bidding) {
    bidQty = 0;
    bidFace_ = 0;
    bidder = NO_SEAT;
  }
}

size_t LiarsDice::botAction(const uint8_t seat, Rng& botRng, uint8_t* out, const size_t capacity) const {
  if (capacity < 3 || seat >= seats) return 0;
  if (phase_ == Phase::Reveal) return seat == starter ? encodeNextRound(out) : 0;
  if (phase_ != Phase::Bidding || seat != turn) return 0;

  // Expected matches of a face, in thirds of a die: own matches count fully,
  // each unseen die matches with probability 1/3 (the face or a wild one).
  const int unknown = totalDice() - rolledCount[seat];
  int own[7] = {};
  for (uint8_t i = 0; i < rolledCount[seat]; i++) {
    const uint8_t d = dice[seat][i];
    for (uint8_t f = 2; f <= 6; f++) {
      if (d == f || d == 1) own[f]++;
    }
  }
  const auto expected3 = [&](const uint8_t face) { return own[face] * 3 + unknown; };

  // Doubt a bid that exceeds the expectation by more than ~1 die; the margin
  // wobbles so the CPU is not perfectly predictable.
  if (hasBid()) {
    const int margin = 2 + static_cast<int>(botRng.below(3));
    if (bidQty * 3 > expected3(bidFace_) + margin) return encodeLiar(out);
  }

  uint8_t bestFace = 0;
  uint8_t bestQty = 0;
  int bestSlack = -1000;
  for (uint8_t f = 2; f <= 6; f++) {
    uint8_t q = minQuantityFor(f);
    if (q == 0) continue;
    // Opening bids start a little under the expectation rather than at one.
    if (!hasBid()) q = static_cast<uint8_t>(std::max(1, expected3(f) / 3 - 1));
    const int slack = expected3(f) - q * 3;
    if (slack > bestSlack || (slack == bestSlack && own[f] > own[bestFace])) {
      bestSlack = slack;
      bestFace = f;
      bestQty = q;
    }
  }
  if (bestFace == 0) return hasBid() ? encodeLiar(out) : 0;
  if (hasBid() && bestSlack < -2) return encodeLiar(out);
  return encodeBid(out, bestQty, bestFace);
}

size_t LiarsDice::serialize(const uint8_t viewerSeat, uint8_t* out, const size_t capacity) const {
  Writer w(out, capacity);
  w.u8(seats);
  w.u8(static_cast<uint8_t>(phase_));
  w.u8(turn);
  w.u8(starter);
  w.u8(bidQty);
  w.u8(bidFace_);
  w.u8(bidder);
  w.u8(roundNo);
  w.u8(viewerSeat);
  w.u8(result.valid ? 1 : 0);
  w.u8(result.challenger);
  w.u8(result.bidder);
  w.u8(result.quantity);
  w.u8(result.face);
  w.u8(result.actual);
  w.u8(result.loser);
  w.bytes(diceCount, sizeof(diceCount));
  w.bytes(rolledCount, sizeof(rolledCount));
  // Cups stay hidden while bidding; the reveal and the final screen show all.
  const bool showAll = phase_ != Phase::Bidding;
  for (uint8_t s = 0; s < MAX_SLOTS; s++) {
    const bool visible = showAll || s == viewerSeat;
    for (uint8_t i = 0; i < START_DICE; i++) w.u8(visible ? dice[s][i] : 0);
  }
  return w.ok() ? w.size() : 0;
}

bool LiarsDice::deserialize(const uint8_t* data, const size_t len) {
  Reader r(data, len);
  const uint8_t rawSeats = r.u8();
  const uint8_t rawPhase = r.u8();
  const uint8_t rawTurn = r.u8();
  const uint8_t rawStarter = r.u8();
  const uint8_t rawBidQty = r.u8();
  const uint8_t rawBidFace = r.u8();
  const uint8_t rawBidder = r.u8();
  const uint8_t rawRound = r.u8();
  const uint8_t rawViewer = r.u8();
  Result rawResult;
  rawResult.valid = r.u8() != 0;
  rawResult.challenger = r.u8();
  rawResult.bidder = r.u8();
  rawResult.quantity = r.u8();
  rawResult.face = r.u8();
  rawResult.actual = r.u8();
  rawResult.loser = r.u8();
  uint8_t rawCount[MAX_SLOTS];
  uint8_t rawRolled[MAX_SLOTS];
  uint8_t rawDice[MAX_SLOTS][START_DICE];
  r.bytes(rawCount, sizeof(rawCount));
  r.bytes(rawRolled, sizeof(rawRolled));
  r.bytes(rawDice, sizeof(rawDice));
  if (!r.ok() || rawSeats < minSeats() || rawSeats > maxSeats() || rawPhase > static_cast<uint8_t>(Phase::Over) ||
      rawTurn >= rawSeats || rawStarter >= rawSeats || rawBidFace > 6) {
    return false;
  }
  for (uint8_t s = 0; s < MAX_SLOTS; s++) {
    if (rawCount[s] > START_DICE || rawRolled[s] > START_DICE) return false;
    for (uint8_t i = 0; i < START_DICE; i++) {
      if (rawDice[s][i] > 6) return false;
    }
  }
  seats = rawSeats;
  phase_ = static_cast<Phase>(rawPhase);
  turn = rawTurn;
  starter = rawStarter;
  bidQty = rawBidQty;
  bidFace_ = rawBidFace;
  bidder = rawBidder;
  roundNo = rawRound;
  viewerSeat_ = rawViewer;
  result = rawResult;
  std::copy(rawCount, rawCount + MAX_SLOTS, diceCount);
  std::copy(rawRolled, rawRolled + MAX_SLOTS, rolledCount);
  for (uint8_t s = 0; s < MAX_SLOTS; s++) std::copy(rawDice[s], rawDice[s] + START_DICE, dice[s]);
  return true;
}

}  // namespace table
