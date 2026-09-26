#pragma once

#include "TableGame.h"

namespace table {

// Classic 7x6 drop-four for exactly two seats. Row 0 is the bottom row.
class ConnectFour final : public Game {
 public:
  static constexpr int COLS = 7;
  static constexpr int ROWS = 6;
  static constexpr uint8_t EMPTY = 0;

  GameId id() const override { return GameId::ConnectFour; }
  uint8_t minSeats() const override { return 2; }
  uint8_t maxSeats() const override { return 2; }

  void reset(uint8_t seatCount, uint32_t seed) override;
  bool apply(uint8_t seat, const uint8_t* action, size_t len) override;
  size_t serialize(uint8_t viewerSeat, uint8_t* out, size_t capacity) const override;
  bool deserialize(const uint8_t* data, size_t len) override;
  void seatLeft(uint8_t seat) override;
  size_t botAction(uint8_t seat, Rng& rng, uint8_t* out, size_t capacity) const override;

  uint8_t seatCount() const override { return 2; }
  int currentSeat() const override { return status == Status::Playing ? turn : -1; }
  bool over() const override { return status != Status::Playing; }
  uint8_t winnerMask() const override { return status == Status::Won ? static_cast<uint8_t>(1u << winner) : 0; }

  // 0 = empty, otherwise seat + 1.
  uint8_t cell(const int col, const int row) const { return cells[row * COLS + col]; }
  bool canDrop(const int col) const { return col >= 0 && col < COLS && cells[(ROWS - 1) * COLS + col] == EMPTY; }
  int lastCol() const { return lastMoveCol; }
  int lastRow() const { return lastMoveRow; }
  // True when (col,row) is part of the winning four.
  bool isWinningCell(int col, int row) const;
  bool forfeited() const { return status == Status::Won && forfeit; }

 private:
  enum class Status : uint8_t { Playing = 0, Won = 1, Draw = 2 };

  bool findWin(int col, int row, uint8_t winCells[4]) const;

  uint8_t cells[COLS * ROWS] = {};
  uint8_t turn = 0;
  Status status = Status::Playing;
  uint8_t winner = 0;
  bool forfeit = false;
  int8_t lastMoveCol = -1;
  int8_t lastMoveRow = -1;
  uint8_t moves = 0;
  // Cell indices of the winning line, 0xFF when unset.
  uint8_t winLine[4] = {0xFF, 0xFF, 0xFF, 0xFF};
};

}  // namespace table
