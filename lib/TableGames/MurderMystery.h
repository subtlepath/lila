#pragma once

#include "TableGame.h"

namespace table {

// A whodunit for 3-6 seats in the style of Clue. The host seals one suspect,
// one weapon and one room in the case file and deals the other 15 cards
// around the table. On your turn, suggest a suspect, weapon and room: the
// first player clockwise holding any of them privately shows you one. Accuse
// when you are sure. A right accusation wins; a wrong one takes you out of
// the running, though you keep showing cards.
class MurderMystery final : public Game {
 public:
  static constexpr uint8_t CATEGORIES = 3;  // suspect, weapon, room
  static constexpr uint8_t PER_CATEGORY = 6;
  static constexpr uint8_t CARD_COUNT = CATEGORIES * PER_CATEGORY;
  static constexpr uint8_t NO_CARD = 0xFF;
  static constexpr uint8_t NO_SEAT = 0xFF;
  static constexpr uint8_t LOG_LEN = 6;

  enum class Kind : uint8_t { Suggest = 1, Accuse = 2, Show = 3, Continue = 4 };
  // Refute: waiting for a player to pick which card to show. Result: the
  // outcome stays up until the player whose turn it was continues.
  enum class Phase : uint8_t { Turn = 0, Refute = 1, Result = 2, Over = 3 };
  enum class EventKind : uint8_t { Suggestion = 1, WrongAccusation = 2, RightAccusation = 3 };

  // Card ids: suspects 0-5, weapons 6-11, rooms 12-17.
  static constexpr uint8_t cardOf(const uint8_t category, const uint8_t index) {
    return static_cast<uint8_t>(category * PER_CATEGORY + index);
  }
  static constexpr uint8_t categoryOf(const uint8_t card) { return card / PER_CATEGORY; }
  static constexpr uint8_t indexOf(const uint8_t card) { return card % PER_CATEGORY; }
  static constexpr uint32_t cardBit(const uint8_t card) { return 1u << card; }
  static constexpr uint32_t categoryMask(const uint8_t category) {
    return ((1u << PER_CATEGORY) - 1u) << (category * PER_CATEGORY);
  }

  // A suggestion or accusation as the whole table saw it.
  struct Event {
    EventKind kind = EventKind::Suggestion;
    uint8_t seat = NO_SEAT;
    uint8_t cards[CATEGORIES] = {};
    // Suggestions: the seat that showed a card, or NO_SEAT when none could.
    uint8_t refuter = NO_SEAT;
  };

  GameId id() const override { return GameId::MurderMystery; }
  uint8_t minSeats() const override { return 3; }
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
  uint8_t winnerMask() const override { return winner < seats ? static_cast<uint8_t>(1u << winner) : 0; }
  bool showingResult() const override { return phase_ == Phase::Result; }

  // Suggest and Accuse take an index (0-5) per category.
  static size_t encodeSuggest(uint8_t* out, uint8_t suspect, uint8_t weapon, uint8_t room);
  static size_t encodeAccuse(uint8_t* out, uint8_t suspect, uint8_t weapon, uint8_t room);
  static size_t encodeShow(uint8_t* out, uint8_t card);
  static size_t encodeContinue(uint8_t* out);

  Phase phase() const { return phase_; }
  uint8_t turnSeat() const { return turn; }
  // The seat answering the current suggestion, or NO_SEAT.
  uint8_t refuterSeat() const { return refuter; }
  // The cards of the suggestion being answered (valid in Refute and Result).
  uint8_t pendingCard(const uint8_t category) const { return category < CATEGORIES ? pending[category] : NO_CARD; }
  // The card shown for the current suggestion; NO_CARD for views outside the
  // exchange or when nobody could show one.
  uint8_t shownCard() const { return shown; }
  // Views hold only the viewer's own hand and the cards shown to it.
  uint32_t handOf(const uint8_t seat) const { return seat < MAX_SLOTS ? hand[seat] : 0; }
  uint32_t seenBy(const uint8_t seat) const { return seat < MAX_SLOTS ? seen[seat] : 0; }
  // Hands of players who left, laid face up for everyone.
  uint32_t revealedCards() const { return revealed; }
  uint8_t handSize(const uint8_t seat) const { return seat < MAX_SLOTS ? handCount[seat] : 0; }
  bool eliminated(const uint8_t seat) const { return seat < MAX_SLOTS && (eliminatedMask >> seat) & 1u; }
  bool departed(const uint8_t seat) const { return seat < MAX_SLOTS && (departedMask >> seat) & 1u; }
  // The case file card for a category; NO_CARD while hidden from this view.
  uint8_t solutionCard(const uint8_t category) const { return category < CATEGORIES ? solution[category] : NO_CARD; }
  // Newest first.
  uint8_t eventCount() const { return events; }
  const Event& event(const uint8_t index) const { return history[index < LOG_LEN ? index : 0]; }
  uint8_t viewer() const { return viewerSeat_; }
  // Cards in `seat`'s hand that answer the pending suggestion; returns the count.
  uint8_t refuteOptions(uint8_t seat, uint8_t* out) const;

 private:
  bool active(uint8_t seat) const;
  uint8_t activeCount() const;
  uint8_t nextActive(uint8_t from) const;
  uint32_t pendingMask() const;
  // Finds who answers the pending suggestion and settles it when no choice
  // is needed.
  void startRefute();
  void showCard(uint8_t card);
  void pushEvent(EventKind kind, uint8_t seat, const uint8_t* cards, uint8_t refuterSeat);

  uint8_t seats = 3;
  Phase phase_ = Phase::Turn;
  uint8_t turn = 0;
  uint8_t refuter = NO_SEAT;
  uint8_t winner = NO_SEAT;
  uint8_t eliminatedMask = 0;
  uint8_t departedMask = 0;
  uint8_t solution[CATEGORIES] = {NO_CARD, NO_CARD, NO_CARD};
  uint8_t pending[CATEGORIES] = {NO_CARD, NO_CARD, NO_CARD};
  uint8_t shown = NO_CARD;
  uint32_t hand[MAX_SLOTS] = {};
  uint32_t seen[MAX_SLOTS] = {};
  // Host only: what each seat has deduced from suggestions nobody answered,
  // for CPU players. Never serialized.
  uint32_t inferred[MAX_SLOTS] = {};
  uint32_t revealed = 0;
  uint8_t handCount[MAX_SLOTS] = {};
  Event history[LOG_LEN];
  uint8_t events = 0;
  uint8_t viewerSeat_ = SPECTATOR;
};

}  // namespace table
