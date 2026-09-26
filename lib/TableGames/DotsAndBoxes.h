#pragma once

#include "TableGame.h"

namespace table {

// Dots and Boxes on a 5x5-box grid for 2-6 seats. Completing a box scores it
// and earns another move. Lines are numbered horizontals first:
//   horizontal (row r in 0..BOXES_H, col c in 0..BOXES_W-1): r * BOXES_W + c
//   vertical   (row r in 0..BOXES_H-1, col c in 0..BOXES_W): H_LINES + r * (BOXES_W + 1) + c
class DotsAndBoxes final : public Game {
 public:
  static constexpr int BOXES_W = 5;
  static constexpr int BOXES_H = 5;
  static constexpr int H_LINES = BOXES_W * (BOXES_H + 1);
  static constexpr int V_LINES = (BOXES_W + 1) * BOXES_H;
  static constexpr int LINES = H_LINES + V_LINES;
  static constexpr int BOXES = BOXES_W * BOXES_H;
  static constexpr uint8_t NO_OWNER = 0xFF;
  static constexpr uint8_t NO_LINE = 0xFF;

  GameId id() const override { return GameId::DotsAndBoxes; }
  uint8_t minSeats() const override { return 2; }
  uint8_t maxSeats() const override { return MAX_SLOTS; }

  void reset(uint8_t seatCount, uint32_t seed) override;
  bool apply(uint8_t seat, const uint8_t* action, size_t len) override;
  size_t serialize(uint8_t viewerSeat, uint8_t* out, size_t capacity) const override;
  bool deserialize(const uint8_t* data, size_t len) override;
  void seatLeft(uint8_t seat) override;
  size_t botAction(uint8_t seat, Rng& rng, uint8_t* out, size_t capacity) const override;

  uint8_t seatCount() const override { return seats; }
  int currentSeat() const override { return finished ? -1 : turn; }
  bool over() const override { return finished; }
  uint8_t winnerMask() const override;

  static int hLine(const int row, const int col) { return row * BOXES_W + col; }
  static int vLine(const int row, const int col) { return H_LINES + row * (BOXES_W + 1) + col; }
  bool lineDrawn(const int line) const { return (lines[line >> 3] >> (line & 7)) & 1u; }
  uint8_t boxOwner(const int row, const int col) const { return owners[row * BOXES_W + col]; }
  int score(uint8_t seat) const;
  bool seatActive(const uint8_t seat) const { return (activeMask >> seat) & 1u; }
  int lastLine() const { return last == NO_LINE ? -1 : last; }

 private:
  // Sides already drawn around box (row, col), given a line bitset.
  static int sidesOf(const uint8_t* bits, int row, int col);
  static bool drawn(const uint8_t* bits, const int line) { return (bits[line >> 3] >> (line & 7)) & 1u; }
  static void setBit(uint8_t* bits, const int line) { bits[line >> 3] |= static_cast<uint8_t>(1u << (line & 7)); }
  // Boxes the next player could take in a row after `line` is drawn.
  static int boxesGivenAway(const uint8_t* bits, int line);
  void advanceTurn();

  uint8_t lines[(LINES + 7) / 8] = {};
  uint8_t owners[BOXES] = {};
  uint8_t seats = 2;
  uint8_t turn = 0;
  uint8_t activeMask = 0;
  uint8_t last = NO_LINE;
  bool finished = false;
};

}  // namespace table
