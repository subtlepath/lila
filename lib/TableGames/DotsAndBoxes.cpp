#include "DotsAndBoxes.h"

#include <algorithm>

namespace table {

void DotsAndBoxes::reset(const uint8_t seatCount, const uint32_t seed) {
  seats = std::clamp<uint8_t>(seatCount, minSeats(), maxSeats());
  std::fill(std::begin(lines), std::end(lines), 0);
  std::fill(std::begin(owners), std::end(owners), NO_OWNER);
  activeMask = static_cast<uint8_t>((1u << seats) - 1u);
  turn = static_cast<uint8_t>(seed % seats);
  last = NO_LINE;
  finished = false;
}

int DotsAndBoxes::sidesOf(const uint8_t* bits, const int row, const int col) {
  return static_cast<int>(drawn(bits, hLine(row, col))) + static_cast<int>(drawn(bits, hLine(row + 1, col))) +
         static_cast<int>(drawn(bits, vLine(row, col))) + static_cast<int>(drawn(bits, vLine(row, col + 1)));
}

// The (up to two) boxes a line borders, as box indices; -1 for none.
static void boxesOfLine(const int line, int out[2]) {
  out[0] = out[1] = -1;
  if (line < DotsAndBoxes::H_LINES) {
    const int row = line / DotsAndBoxes::BOXES_W;
    const int col = line % DotsAndBoxes::BOXES_W;
    if (row > 0) out[0] = (row - 1) * DotsAndBoxes::BOXES_W + col;
    if (row < DotsAndBoxes::BOXES_H) out[1] = row * DotsAndBoxes::BOXES_W + col;
  } else {
    const int v = line - DotsAndBoxes::H_LINES;
    const int row = v / (DotsAndBoxes::BOXES_W + 1);
    const int col = v % (DotsAndBoxes::BOXES_W + 1);
    if (col > 0) out[0] = row * DotsAndBoxes::BOXES_W + col - 1;
    if (col < DotsAndBoxes::BOXES_W) out[1] = row * DotsAndBoxes::BOXES_W + col;
  }
}

void DotsAndBoxes::advanceTurn() {
  for (int i = 1; i <= seats; i++) {
    const uint8_t next = static_cast<uint8_t>((turn + i) % seats);
    if (seatActive(next)) {
      turn = next;
      return;
    }
  }
}

bool DotsAndBoxes::apply(const uint8_t seat, const uint8_t* action, const size_t len) {
  if (finished || seat != turn || len < 1 || !action) return false;
  const int line = action[0];
  if (line >= LINES || lineDrawn(line)) return false;
  setBit(lines, line);
  last = static_cast<uint8_t>(line);

  int boxes[2];
  boxesOfLine(line, boxes);
  bool scored = false;
  for (const int box : boxes) {
    if (box < 0) continue;
    if (sidesOf(lines, box / BOXES_W, box % BOXES_W) == 4 && owners[box] == NO_OWNER) {
      owners[box] = seat;
      scored = true;
    }
  }
  if (std::none_of(std::begin(owners), std::end(owners), [](const uint8_t o) { return o == NO_OWNER; })) {
    finished = true;
  } else if (!scored) {
    advanceTurn();
  }
  return true;
}

void DotsAndBoxes::seatLeft(const uint8_t seat) {
  if (finished || seat >= seats || !seatActive(seat)) return;
  activeMask &= static_cast<uint8_t>(~(1u << seat));
  // A lone player left at the table wins by default.
  int active = 0;
  for (uint8_t s = 0; s < seats; s++) active += seatActive(s) ? 1 : 0;
  if (active < 2) {
    finished = true;
    return;
  }
  if (turn == seat) advanceTurn();
}

int DotsAndBoxes::score(const uint8_t seat) const {
  return static_cast<int>(std::count(std::begin(owners), std::end(owners), seat));
}

uint8_t DotsAndBoxes::winnerMask() const {
  if (!finished) return 0;
  int best = -1;
  uint8_t mask = 0;
  for (uint8_t s = 0; s < seats; s++) {
    // Players who stood up forfeit, unless nobody else is left.
    if (!seatActive(s)) continue;
    const int sc = score(s);
    if (sc > best) {
      best = sc;
      mask = 0;
    }
    if (sc == best) mask |= static_cast<uint8_t>(1u << s);
  }
  // Every remaining player tied: a draw rather than a shared win.
  const bool allTied = mask == activeMask && (mask & (mask - 1)) != 0;
  return allTied ? 0 : mask;
}

int DotsAndBoxes::boxesGivenAway(const uint8_t* bits, const int line) {
  uint8_t scratch[(LINES + 7) / 8];
  std::copy(bits, bits + sizeof(scratch), scratch);
  setBit(scratch, line);
  // The opponent keeps completing any three-sided box; count how many fall.
  int taken = 0;
  bool progress = true;
  while (progress) {
    progress = false;
    for (int r = 0; r < BOXES_H; r++) {
      for (int c = 0; c < BOXES_W; c++) {
        if (sidesOf(scratch, r, c) != 3) continue;
        const int sides[4] = {hLine(r, c), hLine(r + 1, c), vLine(r, c), vLine(r, c + 1)};
        for (const int s : sides) {
          if (drawn(scratch, s)) continue;
          setBit(scratch, s);
          // Count every box the closing line completes (it may close two).
          int closed[2];
          boxesOfLine(s, closed);
          for (const int b : closed) {
            if (b >= 0 && sidesOf(scratch, b / BOXES_W, b % BOXES_W) == 4) taken++;
          }
          progress = true;
          break;
        }
      }
    }
  }
  return taken;
}

size_t DotsAndBoxes::botAction(const uint8_t seat, Rng& rng, uint8_t* out, const size_t capacity) const {
  if (finished || seat != turn || capacity < 1) return 0;

  // 1. Close any box that already has three sides.
  for (int r = 0; r < BOXES_H; r++) {
    for (int c = 0; c < BOXES_W; c++) {
      if (sidesOf(lines, r, c) != 3) continue;
      const int sides[4] = {hLine(r, c), hLine(r + 1, c), vLine(r, c), vLine(r, c + 1)};
      for (const int s : sides) {
        if (!lineDrawn(s)) {
          out[0] = static_cast<uint8_t>(s);
          return 1;
        }
      }
    }
  }

  // 2. Otherwise draw a safe line (one that hands no box a third side), and
  // 3. failing that, the line that gives away the shortest chain.
  uint8_t safe[LINES];
  int safeCount = 0;
  uint8_t cheapest[LINES];
  int cheapestCount = 0;
  int cheapestCost = BOXES + 1;
  for (int line = 0; line < LINES; line++) {
    if (lineDrawn(line)) continue;
    int boxes[2];
    boxesOfLine(line, boxes);
    bool isSafe = true;
    for (const int b : boxes) {
      if (b >= 0 && sidesOf(lines, b / BOXES_W, b % BOXES_W) == 2) isSafe = false;
    }
    if (isSafe) {
      safe[safeCount++] = static_cast<uint8_t>(line);
      continue;
    }
    if (safeCount > 0) continue;  // only chain costs matter once no safe line exists
    const int cost = boxesGivenAway(lines, line);
    if (cost < cheapestCost) {
      cheapestCost = cost;
      cheapestCount = 0;
    }
    if (cost == cheapestCost) cheapest[cheapestCount++] = static_cast<uint8_t>(line);
  }
  if (safeCount > 0) {
    out[0] = safe[rng.below(static_cast<uint32_t>(safeCount))];
    return 1;
  }
  if (cheapestCount > 0) {
    out[0] = cheapest[rng.below(static_cast<uint32_t>(cheapestCount))];
    return 1;
  }
  return 0;
}

size_t DotsAndBoxes::serialize(uint8_t, uint8_t* out, const size_t capacity) const {
  Writer w(out, capacity);
  w.u8(seats);
  w.u8(turn);
  w.u8(activeMask);
  w.u8(last);
  w.u8(finished ? 1 : 0);
  w.bytes(lines, sizeof(lines));
  w.bytes(owners, sizeof(owners));
  return w.ok() ? w.size() : 0;
}

bool DotsAndBoxes::deserialize(const uint8_t* data, const size_t len) {
  Reader r(data, len);
  const uint8_t rawSeats = r.u8();
  const uint8_t rawTurn = r.u8();
  const uint8_t rawActive = r.u8();
  const uint8_t rawLast = r.u8();
  const bool rawFinished = r.u8() != 0;
  uint8_t rawLines[sizeof(lines)];
  uint8_t rawOwners[BOXES];
  r.bytes(rawLines, sizeof(rawLines));
  r.bytes(rawOwners, sizeof(rawOwners));
  if (!r.ok() || rawSeats < minSeats() || rawSeats > maxSeats() || rawTurn >= rawSeats) return false;
  if (rawLast != NO_LINE && rawLast >= LINES) return false;
  for (const uint8_t o : rawOwners) {
    if (o != NO_OWNER && o >= rawSeats) return false;
  }
  seats = rawSeats;
  turn = rawTurn;
  activeMask = rawActive;
  last = rawLast;
  finished = rawFinished;
  std::copy(rawLines, rawLines + sizeof(lines), lines);
  std::copy(rawOwners, rawOwners + BOXES, owners);
  return true;
}

}  // namespace table
