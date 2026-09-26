#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "TableWire.h"

namespace table {

// Game seats index the players of one match (0..seatCount-1). The session maps
// them to table slots, so a game never knows about radios, names or MACs.
constexpr uint8_t SPECTATOR = 0xFF;

// Small deterministic PRNG (xorshift32). The host owns every roll; guests only
// ever see results, so no seed or dice leak through the view serialization.
struct Rng {
  uint32_t state = 0x9E3779B9u;
  explicit Rng(const uint32_t seed = 0x9E3779B9u) : state(seed ? seed : 0x9E3779B9u) {}
  uint32_t next() {
    uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
  }
  // Uniform in [0, bound).
  uint32_t below(const uint32_t bound) { return bound ? next() % bound : 0; }
};

// Rules and state of one game. The host keeps the authoritative instance and
// applies moves; every device (host included) renders from an instance loaded
// with serialize(viewerSeat), so hidden information stays on the host.
class Game {
 public:
  virtual ~Game() = default;

  virtual GameId id() const = 0;
  virtual uint8_t minSeats() const = 0;
  virtual uint8_t maxSeats() const = 0;

  // Host: begin a fresh match.
  virtual void reset(uint8_t seatCount, uint32_t seed) = 0;
  // Host: validate and apply a move. Returns false (state unchanged) when the
  // move is illegal or not this seat's to make.
  virtual bool apply(uint8_t seat, const uint8_t* action, size_t len) = 0;
  // Host: the state as `viewerSeat` may see it (SPECTATOR for onlookers).
  virtual size_t serialize(uint8_t viewerSeat, uint8_t* out, size_t capacity) const = 0;
  // Any device: load a view produced by serialize().
  virtual bool deserialize(const uint8_t* data, size_t len) = 0;
  // Host: the seat stood up mid-match. The game forfeits or skips it.
  virtual void seatLeft(uint8_t seat) = 0;
  // Host: a move for a CPU-controlled seat, or 0 when the seat has nothing to do.
  virtual size_t botAction(uint8_t seat, Rng& rng, uint8_t* out, size_t capacity) const = 0;

  virtual uint8_t seatCount() const = 0;
  // Seat whose move the game is waiting for, or -1 (match over).
  virtual int currentSeat() const = 0;
  virtual bool over() const = 0;
  // Bit per winning seat once over(); 0 for a draw.
  virtual uint8_t winnerMask() const = 0;
  // True while the table should linger on a result (CPU players wait longer).
  virtual bool showingResult() const { return false; }
};

std::unique_ptr<Game> createGame(GameId id);
bool validGameId(uint8_t raw);

}  // namespace table
