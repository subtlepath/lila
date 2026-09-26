#pragma once

#include "TableGame.h"

namespace table {

// Liar's Dice for 2-6 seats, the game that needs one screen per player: each
// device shows only its own cup. Ones are wild and cannot be bid. On your turn
// raise the bid (more dice, or the same count of a higher face) or call
// "liar". The loser of a challenge loses a die and opens the next round; the
// last player holding dice wins.
class LiarsDice final : public Game {
 public:
  static constexpr uint8_t START_DICE = 5;
  static constexpr uint8_t NO_SEAT = 0xFF;

  enum class Kind : uint8_t { Bid = 1, Liar = 2, NextRound = 3 };
  enum class Phase : uint8_t { Bidding = 0, Reveal = 1, Over = 2 };

  struct Result {
    bool valid = false;
    uint8_t challenger = NO_SEAT;
    uint8_t bidder = NO_SEAT;
    uint8_t quantity = 0;
    uint8_t face = 0;
    uint8_t actual = 0;
    uint8_t loser = NO_SEAT;
  };

  GameId id() const override { return GameId::LiarsDice; }
  uint8_t minSeats() const override { return 2; }
  uint8_t maxSeats() const override { return MAX_SLOTS; }

  void reset(uint8_t seatCount, uint32_t seed) override;
  bool apply(uint8_t seat, const uint8_t* action, size_t len) override;
  size_t serialize(uint8_t viewerSeat, uint8_t* out, size_t capacity) const override;
  bool deserialize(const uint8_t* data, size_t len) override;
  void seatLeft(uint8_t seat) override;
  size_t botAction(uint8_t seat, Rng& rng, uint8_t* out, size_t capacity) const override;

  uint8_t seatCount() const override { return seats; }
  int currentSeat() const override;
  bool over() const override { return phase_ == Phase::Over; }
  uint8_t winnerMask() const override;
  bool showingResult() const override { return phase_ == Phase::Reveal; }

  static size_t encodeBid(uint8_t* out, uint8_t quantity, uint8_t face);
  static size_t encodeLiar(uint8_t* out);
  static size_t encodeNextRound(uint8_t* out);

  Phase phase() const { return phase_; }
  uint8_t turnSeat() const { return turn; }
  uint8_t starterSeat() const { return starter; }
  uint8_t bidQuantity() const { return bidQty; }
  uint8_t bidFace() const { return bidFace_; }
  uint8_t bidderSeat() const { return bidder; }
  bool hasBid() const { return bidFace_ != 0; }
  uint8_t diceLeft(const uint8_t seat) const { return seat < seats ? diceCount[seat] : 0; }
  // Dice rolled this round (the loser's die is still shown during the reveal).
  uint8_t diceRolled(const uint8_t seat) const { return seat < seats ? rolledCount[seat] : 0; }
  // 0 when hidden from this view.
  uint8_t die(const uint8_t seat, const uint8_t index) const {
    return seat < seats && index < START_DICE ? dice[seat][index] : 0;
  }
  uint8_t totalDice() const;
  uint8_t round() const { return roundNo; }
  const Result& lastResult() const { return result; }
  uint8_t viewer() const { return viewerSeat_; }
  // Smallest legal bid for `face` given the standing bid (0 if none fits).
  uint8_t minQuantityFor(uint8_t face) const;
  bool bidIsLegal(uint8_t quantity, uint8_t face) const;

 private:
  void rollAll();
  uint8_t nextActive(uint8_t from) const;
  uint8_t activeCount() const;
  uint8_t countFace(uint8_t face) const;

  Rng rng;
  uint8_t dice[MAX_SLOTS][START_DICE] = {};
  uint8_t diceCount[MAX_SLOTS] = {};
  uint8_t rolledCount[MAX_SLOTS] = {};
  uint8_t seats = 2;
  Phase phase_ = Phase::Bidding;
  uint8_t turn = 0;
  uint8_t starter = 0;
  uint8_t bidQty = 0;
  uint8_t bidFace_ = 0;
  uint8_t bidder = NO_SEAT;
  uint8_t roundNo = 0;
  uint8_t viewerSeat_ = SPECTATOR;
  Result result;
};

}  // namespace table
