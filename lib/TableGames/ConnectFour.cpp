#include "ConnectFour.h"

#include <algorithm>

namespace table {
namespace {

constexpr int COLS = ConnectFour::COLS;
constexpr int ROWS = ConnectFour::ROWS;
constexpr int CELLS = COLS * ROWS;
constexpr int BOT_DEPTH = 5;
constexpr int WIN_SCORE = 100000;
// Center-first move order makes alpha-beta prune far more.
constexpr int MOVE_ORDER[COLS] = {3, 2, 4, 1, 5, 0, 6};

// Scratch board for the bot's search: cells plus per-column fill heights, so
// make/unmake is O(1) and the search never copies the board.
struct SearchBoard {
  uint8_t cells[CELLS];
  uint8_t heights[COLS];

  bool canPlay(const int col) const { return heights[col] < ROWS; }
  int play(const int col, const uint8_t piece) {
    const int row = heights[col]++;
    cells[row * COLS + col] = piece;
    return row;
  }
  void undo(const int col) { cells[--heights[col] * COLS + col] = 0; }

  bool winsAt(const int col, const int row, const uint8_t piece) const {
    static constexpr int DIRS[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
    for (const auto& d : DIRS) {
      int run = 1;
      for (int sign = -1; sign <= 1; sign += 2) {
        int c = col + d[0] * sign;
        int r = row + d[1] * sign;
        while (c >= 0 && c < COLS && r >= 0 && r < ROWS && cells[r * COLS + c] == piece) {
          run++;
          c += d[0] * sign;
          r += d[1] * sign;
        }
      }
      if (run >= 4) return true;
    }
    return false;
  }
};

int scoreWindow(const int mine, const int theirs) {
  if (mine > 0 && theirs > 0) return 0;
  if (mine == 3) return 50;
  if (mine == 2) return 10;
  if (theirs == 3) return -80;
  if (theirs == 2) return -10;
  return 0;
}

// Static evaluation from `piece`'s point of view: open windows of four plus a
// small center-column bias.
int evaluate(const SearchBoard& b, const uint8_t piece) {
  const uint8_t other = piece == 1 ? 2 : 1;
  int score = 0;
  for (int r = 0; r < ROWS; r++) {
    if (b.cells[r * COLS + 3] == piece) score += 6;
  }
  static constexpr int DIRS[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
  for (int r = 0; r < ROWS; r++) {
    for (int c = 0; c < COLS; c++) {
      for (const auto& d : DIRS) {
        const int endC = c + d[0] * 3;
        const int endR = r + d[1] * 3;
        if (endC < 0 || endC >= COLS || endR < 0 || endR >= ROWS) continue;
        int mine = 0;
        int theirs = 0;
        for (int i = 0; i < 4; i++) {
          const uint8_t v = b.cells[(r + d[1] * i) * COLS + (c + d[0] * i)];
          if (v == piece)
            mine++;
          else if (v == other)
            theirs++;
        }
        score += scoreWindow(mine, theirs);
      }
    }
  }
  return score;
}

int negamax(SearchBoard& b, const int depth, int alpha, const int beta, const uint8_t piece) {
  if (depth == 0) return evaluate(b, piece);
  const uint8_t other = piece == 1 ? 2 : 1;
  bool anyMove = false;
  int best = -WIN_SCORE * 2;
  for (const int col : MOVE_ORDER) {
    if (!b.canPlay(col)) continue;
    anyMove = true;
    const int row = b.play(col, piece);
    int score;
    if (b.winsAt(col, row, piece)) {
      // Prefer quicker wins: remaining depth breaks ties.
      score = WIN_SCORE + depth;
    } else {
      score = -negamax(b, depth - 1, -beta, -alpha, other);
    }
    b.undo(col);
    best = std::max(best, score);
    alpha = std::max(alpha, score);
    if (alpha >= beta) break;
  }
  return anyMove ? best : 0;  // full board: draw
}

}  // namespace

void ConnectFour::reset(uint8_t, const uint32_t seed) {
  std::fill(std::begin(cells), std::end(cells), EMPTY);
  // The seed picks who opens, so "play again" does not always favour one seat.
  turn = static_cast<uint8_t>(seed & 1u);
  status = Status::Playing;
  winner = 0;
  forfeit = false;
  lastMoveCol = -1;
  lastMoveRow = -1;
  moves = 0;
  std::fill(std::begin(winLine), std::end(winLine), 0xFF);
}

bool ConnectFour::findWin(const int col, const int row, uint8_t winCells[4]) const {
  static constexpr int DIRS[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
  const uint8_t piece = cell(col, row);
  for (const auto& d : DIRS) {
    // Walk back to the start of the run through (col,row), then forward.
    int c = col;
    int r = row;
    while (c - d[0] >= 0 && c - d[0] < COLS && r - d[1] >= 0 && r - d[1] < ROWS && cell(c - d[0], r - d[1]) == piece) {
      c -= d[0];
      r -= d[1];
    }
    int run = 0;
    uint8_t line[4];
    while (c >= 0 && c < COLS && r >= 0 && r < ROWS && cell(c, r) == piece) {
      if (run < 4) line[run] = static_cast<uint8_t>(r * COLS + c);
      run++;
      c += d[0];
      r += d[1];
    }
    if (run >= 4) {
      std::copy(line, line + 4, winCells);
      return true;
    }
  }
  return false;
}

bool ConnectFour::apply(const uint8_t seat, const uint8_t* action, const size_t len) {
  if (status != Status::Playing || seat != turn || len < 1 || !action) return false;
  const int col = action[0];
  if (!canDrop(col)) return false;
  int row = 0;
  while (cells[row * COLS + col] != EMPTY) row++;
  cells[row * COLS + col] = static_cast<uint8_t>(seat + 1);
  lastMoveCol = static_cast<int8_t>(col);
  lastMoveRow = static_cast<int8_t>(row);
  moves++;
  if (findWin(col, row, winLine)) {
    status = Status::Won;
    winner = seat;
  } else if (moves >= CELLS) {
    status = Status::Draw;
  } else {
    turn ^= 1u;
  }
  return true;
}

void ConnectFour::seatLeft(const uint8_t seat) {
  if (status != Status::Playing || seat > 1) return;
  status = Status::Won;
  winner = seat ^ 1u;
  forfeit = true;
}

bool ConnectFour::isWinningCell(const int col, const int row) const {
  if (status != Status::Won) return false;
  const uint8_t idx = static_cast<uint8_t>(row * COLS + col);
  return std::find(std::begin(winLine), std::end(winLine), idx) != std::end(winLine);
}

size_t ConnectFour::serialize(uint8_t, uint8_t* out, const size_t capacity) const {
  Writer w(out, capacity);
  w.u8(static_cast<uint8_t>(status));
  w.u8(turn);
  w.u8(winner);
  w.u8(forfeit ? 1 : 0);
  w.u8(static_cast<uint8_t>(lastMoveCol));
  w.u8(static_cast<uint8_t>(lastMoveRow));
  w.u8(moves);
  w.bytes(winLine, sizeof(winLine));
  w.bytes(cells, sizeof(cells));
  return w.ok() ? w.size() : 0;
}

bool ConnectFour::deserialize(const uint8_t* data, const size_t len) {
  Reader r(data, len);
  const uint8_t rawStatus = r.u8();
  const uint8_t rawTurn = r.u8();
  const uint8_t rawWinner = r.u8();
  const bool rawForfeit = r.u8() != 0;
  const auto rawLastCol = static_cast<int8_t>(r.u8());
  const auto rawLastRow = static_cast<int8_t>(r.u8());
  const uint8_t rawMoves = r.u8();
  uint8_t rawLine[4];
  uint8_t rawCells[CELLS];
  r.bytes(rawLine, sizeof(rawLine));
  r.bytes(rawCells, sizeof(rawCells));
  if (!r.ok() || rawStatus > static_cast<uint8_t>(Status::Draw) || rawTurn > 1 || rawWinner > 1) return false;
  for (const uint8_t v : rawCells) {
    if (v > 2) return false;
  }
  status = static_cast<Status>(rawStatus);
  turn = rawTurn;
  winner = rawWinner;
  forfeit = rawForfeit;
  lastMoveCol = rawLastCol;
  lastMoveRow = rawLastRow;
  moves = rawMoves;
  std::copy(rawLine, rawLine + 4, winLine);
  std::copy(rawCells, rawCells + CELLS, cells);
  return true;
}

size_t ConnectFour::botAction(const uint8_t seat, Rng& rng, uint8_t* out, const size_t capacity) const {
  if (status != Status::Playing || seat != turn || capacity < 1) return 0;
  SearchBoard b{};
  std::copy(std::begin(cells), std::end(cells), b.cells);
  for (int c = 0; c < COLS; c++) {
    int h = 0;
    while (h < ROWS && cells[h * COLS + c] != EMPTY) h++;
    b.heights[c] = static_cast<uint8_t>(h);
  }
  const uint8_t piece = static_cast<uint8_t>(seat + 1);
  const uint8_t other = piece == 1 ? 2 : 1;

  int bestScore = -WIN_SCORE * 4;
  int bestCols[COLS];
  int bestCount = 0;
  for (const int col : MOVE_ORDER) {
    if (!b.canPlay(col)) continue;
    const int row = b.play(col, piece);
    const int score =
        b.winsAt(col, row, piece) ? WIN_SCORE * 2 : -negamax(b, BOT_DEPTH - 1, -WIN_SCORE * 4, WIN_SCORE * 4, other);
    b.undo(col);
    if (score > bestScore) {
      bestScore = score;
      bestCount = 0;
    }
    if (score == bestScore) bestCols[bestCount++] = col;
  }
  if (bestCount == 0) return 0;
  // Equal moves are picked at random so the CPU does not replay one opening.
  out[0] = static_cast<uint8_t>(bestCols[rng.below(static_cast<uint32_t>(bestCount))]);
  return 1;
}

}  // namespace table
