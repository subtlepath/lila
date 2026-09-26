#include <gtest/gtest.h>

#include <cstring>
#include <deque>
#include <vector>

#include "ConnectFour.h"
#include "DotsAndBoxes.h"
#include "LiarsDice.h"
#include "MurderMystery.h"
#include "TableSession.h"
#include "TableWire.h"

using namespace table;

// --- wire --------------------------------------------------------------------

TEST(TableWire, HeaderRoundTrip) {
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  writeHeader(w, MsgType::State, 0xA1B2C3D4u, 3);
  ASSERT_TRUE(w.ok());
  ASSERT_EQ(w.size(), HEADER_LEN);

  Reader r(buf, w.size());
  Header h;
  ASSERT_TRUE(readHeader(r, h));
  EXPECT_EQ(h.type, MsgType::State);
  EXPECT_EQ(h.tableId, 0xA1B2C3D4u);
  EXPECT_EQ(h.srcSlot, 3);
}

TEST(TableWire, RejectsForeignAndTruncatedPackets) {
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  writeHeader(w, MsgType::Ping, 7, 1);

  Header h;
  {
    Reader r(buf, HEADER_LEN - 1);
    EXPECT_FALSE(readHeader(r, h));
  }
  buf[0] = 'X';
  {
    Reader r(buf, HEADER_LEN);
    EXPECT_FALSE(readHeader(r, h));
  }
  buf[0] = MAGIC0;
  buf[3] = 0;  // unknown type
  {
    Reader r(buf, HEADER_LEN);
    EXPECT_FALSE(readHeader(r, h));
  }
}

TEST(TableWire, WriterRefusesOverflow) {
  uint8_t buf[4];
  Writer w(buf, sizeof(buf));
  w.u32(1);
  EXPECT_TRUE(w.ok());
  w.u8(2);
  EXPECT_FALSE(w.ok());
}

TEST(TableWire, NamesArePaddedAndTerminated) {
  uint8_t buf[NAME_LEN];
  Writer w(buf, sizeof(buf));
  w.name("A very long player name");
  Reader r(buf, sizeof(buf));
  char name[NAME_LEN + 1];
  r.name(name);
  EXPECT_EQ(strlen(name), NAME_LEN);
  EXPECT_EQ(strncmp(name, "A very long ", NAME_LEN), 0);
}

TEST(TableWire, SequenceComparisonWraps) {
  EXPECT_TRUE(seqNewer(1, 0));
  EXPECT_FALSE(seqNewer(0, 0));
  EXPECT_TRUE(seqNewer(2, 65535));
  EXPECT_FALSE(seqNewer(65535, 2));
}

// --- Connect Four ----------------------------------------------------------------

namespace {
bool drop(Game& g, const uint8_t seat, const uint8_t col) { return g.apply(seat, &col, 1); }
}  // namespace

TEST(ConnectFour, HorizontalWin) {
  ConnectFour g;
  g.reset(2, 0);  // seat 0 opens
  ASSERT_EQ(g.currentSeat(), 0);
  for (uint8_t c = 0; c < 3; c++) {
    ASSERT_TRUE(drop(g, 0, c));
    ASSERT_TRUE(drop(g, 1, c));
  }
  ASSERT_TRUE(drop(g, 0, 3));
  EXPECT_TRUE(g.over());
  EXPECT_EQ(g.winnerMask(), 1u);
  EXPECT_TRUE(g.isWinningCell(0, 0));
  EXPECT_TRUE(g.isWinningCell(3, 0));
  EXPECT_FALSE(g.isWinningCell(0, 1));
}

TEST(ConnectFour, DiagonalWin) {
  ConnectFour g;
  g.reset(2, 0);
  // Seat 0 builds the / diagonal from (0,0) to (3,3).
  const uint8_t moves[][2] = {{0, 0}, {1, 1}, {0, 1}, {1, 2}, {0, 3}, {1, 2}, {0, 2}, {1, 3}, {0, 6}, {1, 3}, {0, 3}};
  for (const auto& m : moves) ASSERT_TRUE(drop(g, m[0], m[1])) << int(m[0]) << "," << int(m[1]);
  EXPECT_TRUE(g.over());
  EXPECT_EQ(g.winnerMask(), 1u);
}

TEST(ConnectFour, RejectsOutOfTurnAndFullColumns) {
  ConnectFour g;
  g.reset(2, 0);
  EXPECT_FALSE(drop(g, 1, 0));
  for (int i = 0; i < ConnectFour::ROWS; i++) ASSERT_TRUE(drop(g, static_cast<uint8_t>(i % 2), 6));
  EXPECT_FALSE(g.canDrop(6));
  EXPECT_FALSE(drop(g, 0, 6));
  EXPECT_FALSE(drop(g, 0, 7));
}

TEST(ConnectFour, SerializeRoundTrip) {
  ConnectFour g;
  g.reset(2, 1);
  drop(g, 1, 3);
  drop(g, 0, 4);
  uint8_t buf[MAX_GAME_STATE];
  const size_t len = g.serialize(0, buf, sizeof(buf));
  ASSERT_GT(len, 0u);
  ConnectFour copy;
  ASSERT_TRUE(copy.deserialize(buf, len));
  EXPECT_EQ(copy.cell(3, 0), 2);
  EXPECT_EQ(copy.cell(4, 0), 1);
  EXPECT_EQ(copy.currentSeat(), 1);
  EXPECT_EQ(copy.lastCol(), 4);
  EXPECT_FALSE(copy.deserialize(buf, len - 1));
}

TEST(ConnectFour, BotTakesWinAndBlocks) {
  Rng rng(42);
  uint8_t out[MAX_ACTION];
  {
    ConnectFour g;
    g.reset(2, 0);
    // Seat 0 has three in column 2; seat 1 to move must block at column 2.
    drop(g, 0, 2);
    drop(g, 1, 5);
    drop(g, 0, 2);
    drop(g, 1, 5);
    drop(g, 0, 2);
    ASSERT_EQ(g.botAction(1, rng, out, sizeof(out)), 1u);
    EXPECT_EQ(out[0], 2);
  }
  {
    ConnectFour g;
    g.reset(2, 0);
    drop(g, 0, 0);
    drop(g, 1, 1);
    drop(g, 0, 0);
    drop(g, 1, 1);
    drop(g, 0, 0);
    drop(g, 1, 1);
    // Seat 0 wins in column 0 immediately.
    ASSERT_EQ(g.botAction(0, rng, out, sizeof(out)), 1u);
    EXPECT_EQ(out[0], 0);
  }
}

TEST(ConnectFour, LeaverForfeits) {
  ConnectFour g;
  g.reset(2, 0);
  g.seatLeft(0);
  EXPECT_TRUE(g.over());
  EXPECT_EQ(g.winnerMask(), 2u);
  EXPECT_TRUE(g.forfeited());
}

TEST(ConnectFour, BotVersusBotFinishes) {
  ConnectFour g;
  g.reset(2, 7);
  Rng rng(9);
  for (int i = 0; i < 42 && !g.over(); i++) {
    uint8_t a[MAX_ACTION];
    const int seat = g.currentSeat();
    ASSERT_EQ(g.botAction(static_cast<uint8_t>(seat), rng, a, sizeof(a)), 1u);
    ASSERT_TRUE(g.apply(static_cast<uint8_t>(seat), a, 1));
  }
  EXPECT_TRUE(g.over());
}

// --- Dots and Boxes -------------------------------------------------------------

namespace {
bool line(Game& g, const uint8_t seat, const int l) {
  const auto b = static_cast<uint8_t>(l);
  return g.apply(seat, &b, 1);
}
}  // namespace

TEST(DotsAndBoxes, CompletingABoxScoresAndKeepsTheTurn) {
  DotsAndBoxes g;
  g.reset(2, 0);
  ASSERT_EQ(g.currentSeat(), 0);
  ASSERT_TRUE(line(g, 0, DotsAndBoxes::hLine(0, 0)));
  ASSERT_EQ(g.currentSeat(), 1);
  ASSERT_TRUE(line(g, 1, DotsAndBoxes::hLine(1, 0)));
  ASSERT_TRUE(line(g, 0, DotsAndBoxes::vLine(0, 0)));
  EXPECT_FALSE(line(g, 0, DotsAndBoxes::vLine(0, 0)));  // not seat 0's turn now
  ASSERT_TRUE(line(g, 1, DotsAndBoxes::vLine(0, 1)));
  EXPECT_EQ(g.boxOwner(0, 0), 1);
  EXPECT_EQ(g.score(1), 1);
  EXPECT_EQ(g.currentSeat(), 1);                        // scorer moves again
  EXPECT_FALSE(line(g, 1, DotsAndBoxes::vLine(0, 1)));  // already drawn
}

TEST(DotsAndBoxes, BotClosesBoxesAndAvoidsThirdSides) {
  DotsAndBoxes g;
  g.reset(2, 0);
  Rng rng(3);
  uint8_t out[MAX_ACTION];
  line(g, 0, DotsAndBoxes::hLine(0, 0));
  line(g, 1, DotsAndBoxes::hLine(1, 0));
  line(g, 0, DotsAndBoxes::vLine(0, 0));
  // Seat 1 to move; box (0,0) has three sides.
  ASSERT_EQ(g.botAction(1, rng, out, sizeof(out)), 1u);
  EXPECT_EQ(out[0], DotsAndBoxes::vLine(0, 1));

  DotsAndBoxes h;
  h.reset(2, 0);
  line(h, 0, DotsAndBoxes::hLine(0, 0));
  line(h, 1, DotsAndBoxes::hLine(1, 0));
  // Seat 0 to move: either remaining side of box (0,0) would hand it over.
  for (int i = 0; i < 20; i++) {
    ASSERT_EQ(h.botAction(0, rng, out, sizeof(out)), 1u);
    EXPECT_NE(out[0], DotsAndBoxes::vLine(0, 0));
    EXPECT_NE(out[0], DotsAndBoxes::vLine(0, 1));
  }
}

TEST(DotsAndBoxes, FullGameEndsWithAWinner) {
  DotsAndBoxes g;
  g.reset(3, 1);
  Rng rng(11);
  int moves = 0;
  while (!g.over() && moves < DotsAndBoxes::LINES + 1) {
    uint8_t a[MAX_ACTION];
    const int seat = g.currentSeat();
    ASSERT_EQ(g.botAction(static_cast<uint8_t>(seat), rng, a, sizeof(a)), 1u);
    ASSERT_TRUE(g.apply(static_cast<uint8_t>(seat), a, 1));
    moves++;
  }
  EXPECT_TRUE(g.over());
  EXPECT_EQ(moves, DotsAndBoxes::LINES);
  EXPECT_EQ(g.score(0) + g.score(1) + g.score(2), DotsAndBoxes::BOXES);
  EXPECT_NE(g.winnerMask(), 0u);
}

TEST(DotsAndBoxes, LeaversAreSkipped) {
  DotsAndBoxes g;
  g.reset(3, 0);
  ASSERT_EQ(g.currentSeat(), 0);
  g.seatLeft(1);
  ASSERT_TRUE(line(g, 0, 0));
  EXPECT_EQ(g.currentSeat(), 2);
  g.seatLeft(2);
  EXPECT_TRUE(g.over());
  EXPECT_EQ(g.winnerMask(), 1u);
}

TEST(DotsAndBoxes, SerializeRoundTrip) {
  DotsAndBoxes g;
  g.reset(4, 2);
  line(g, 2, 5);
  uint8_t buf[MAX_GAME_STATE];
  const size_t len = g.serialize(SPECTATOR, buf, sizeof(buf));
  DotsAndBoxes copy;
  ASSERT_TRUE(copy.deserialize(buf, len));
  EXPECT_TRUE(copy.lineDrawn(5));
  EXPECT_EQ(copy.seatCount(), 4);
  EXPECT_EQ(copy.currentSeat(), 3);
  EXPECT_EQ(copy.lastLine(), 5);
}

// --- Liar's Dice -------------------------------------------------------------------

namespace {

// Builds a Liar's Dice state with chosen cups, via the view format.
LiarsDice craftedDice(const std::vector<std::vector<uint8_t>>& cups, const uint8_t turn) {
  uint8_t buf[MAX_GAME_STATE] = {};
  Writer w(buf, sizeof(buf));
  w.u8(static_cast<uint8_t>(cups.size()));
  w.u8(0);  // bidding
  w.u8(turn);
  w.u8(turn);
  w.u8(0);  // bid qty
  w.u8(0);  // bid face
  w.u8(LiarsDice::NO_SEAT);
  w.u8(1);  // round
  w.u8(SPECTATOR);
  for (int i = 0; i < 7; i++) w.u8(i == 0 ? 0 : LiarsDice::NO_SEAT);  // no result
  for (size_t s = 0; s < MAX_SLOTS; s++) w.u8(s < cups.size() ? static_cast<uint8_t>(cups[s].size()) : 0);
  for (size_t s = 0; s < MAX_SLOTS; s++) w.u8(s < cups.size() ? static_cast<uint8_t>(cups[s].size()) : 0);
  for (size_t s = 0; s < MAX_SLOTS; s++) {
    for (size_t i = 0; i < LiarsDice::START_DICE; i++) w.u8(s < cups.size() && i < cups[s].size() ? cups[s][i] : 0);
  }
  LiarsDice g;
  EXPECT_TRUE(g.deserialize(buf, w.size()));
  return g;
}

}  // namespace

TEST(LiarsDice, BidsMustRise) {
  LiarsDice g = craftedDice({{2, 2, 3, 4, 5}, {1, 6, 6, 6, 2}}, 0);
  uint8_t a[MAX_ACTION];
  EXPECT_FALSE(g.apply(0, a, LiarsDice::encodeBid(a, 2, 1)));  // ones cannot be bid
  EXPECT_FALSE(g.apply(0, a, LiarsDice::encodeLiar(a)));       // nothing to doubt yet
  ASSERT_TRUE(g.apply(0, a, LiarsDice::encodeBid(a, 3, 4)));
  EXPECT_EQ(g.currentSeat(), 1);
  EXPECT_FALSE(g.apply(1, a, LiarsDice::encodeBid(a, 3, 3)));   // lower face, same count
  EXPECT_FALSE(g.apply(1, a, LiarsDice::encodeBid(a, 11, 6)));  // more dice than exist
  EXPECT_EQ(g.minQuantityFor(5), 3);
  EXPECT_EQ(g.minQuantityFor(2), 4);
  ASSERT_TRUE(g.apply(1, a, LiarsDice::encodeBid(a, 3, 6)));
  EXPECT_EQ(g.currentSeat(), 0);
}

TEST(LiarsDice, ChallengeCountsWildOnes) {
  // Sixes on the table: seat 1 has 1,6,6,6 (+2) = 4, seat 0 has none -> 4.
  LiarsDice g = craftedDice({{2, 2, 3, 4, 5}, {1, 6, 6, 6, 2}}, 1);
  uint8_t a[MAX_ACTION];
  ASSERT_TRUE(g.apply(1, a, LiarsDice::encodeBid(a, 4, 6)));
  ASSERT_TRUE(g.apply(0, a, LiarsDice::encodeLiar(a)));
  EXPECT_EQ(g.phase(), LiarsDice::Phase::Reveal);
  EXPECT_EQ(g.lastResult().actual, 4);
  EXPECT_EQ(g.lastResult().loser, 0);  // the bid stood, so the challenger pays
  EXPECT_EQ(g.diceLeft(0), 4);
  EXPECT_EQ(g.currentSeat(), 0);  // loser opens next round
  ASSERT_TRUE(g.apply(0, a, LiarsDice::encodeNextRound(a)));
  EXPECT_EQ(g.phase(), LiarsDice::Phase::Bidding);
  EXPECT_EQ(g.diceRolled(0), 4);
}

TEST(LiarsDice, OverbidLosesADie) {
  LiarsDice g = craftedDice({{2, 2, 3, 4, 5}, {1, 6, 6, 6, 2}}, 1);
  uint8_t a[MAX_ACTION];
  ASSERT_TRUE(g.apply(1, a, LiarsDice::encodeBid(a, 5, 6)));
  ASSERT_TRUE(g.apply(0, a, LiarsDice::encodeLiar(a)));
  EXPECT_EQ(g.lastResult().loser, 1);
  EXPECT_EQ(g.diceLeft(1), 4);
}

TEST(LiarsDice, ViewHidesOtherCupsWhileBidding) {
  LiarsDice g;
  g.reset(3, 1234);
  uint8_t buf[MAX_GAME_STATE];
  const size_t len = g.serialize(1, buf, sizeof(buf));
  LiarsDice view;
  ASSERT_TRUE(view.deserialize(buf, len));
  for (uint8_t i = 0; i < LiarsDice::START_DICE; i++) {
    EXPECT_EQ(view.die(0, i), 0);
    EXPECT_EQ(view.die(2, i), 0);
    EXPECT_GE(view.die(1, i), 1);
  }
  EXPECT_EQ(view.viewer(), 1);
  EXPECT_EQ(view.diceLeft(0), LiarsDice::START_DICE);
}

TEST(LiarsDice, LastCupStandingWins) {
  LiarsDice g = craftedDice({{2}, {3, 3}}, 0);
  uint8_t a[MAX_ACTION];
  ASSERT_TRUE(g.apply(0, a, LiarsDice::encodeBid(a, 2, 2)));
  ASSERT_TRUE(g.apply(1, a, LiarsDice::encodeLiar(a)));
  EXPECT_TRUE(g.over());
  EXPECT_EQ(g.winnerMask(), 2u);
  EXPECT_EQ(g.currentSeat(), -1);
}

TEST(LiarsDice, BotsFinishAGame) {
  LiarsDice g;
  g.reset(4, 99);
  Rng rng(5);
  int steps = 0;
  while (!g.over() && steps < 2000) {
    uint8_t a[MAX_ACTION];
    const int seat = g.currentSeat();
    const size_t len = g.botAction(static_cast<uint8_t>(seat), rng, a, sizeof(a));
    ASSERT_GT(len, 0u);
    ASSERT_TRUE(g.apply(static_cast<uint8_t>(seat), a, len)) << "step " << steps;
    steps++;
  }
  EXPECT_TRUE(g.over());
  EXPECT_EQ(__builtin_popcount(g.winnerMask()), 1);
}

// --- Session over an in-memory radio -------------------------------------------

namespace {

struct Bus;

struct Node : Link {
  Bus* bus = nullptr;
  uint8_t mac[MAC_LEN] = {};
  TableSession session;
  bool online = true;
  // Hears the air but cannot be heard (a one-way link).
  bool muted = false;
  bool send(const uint8_t* dest, const uint8_t* data, size_t len) override;
};

struct Packet {
  uint8_t from[MAC_LEN];
  uint8_t to[MAC_LEN];
  std::vector<uint8_t> data;
};

struct Bus {
  std::vector<Node*> nodes;
  std::deque<Packet> queue;
  uint32_t now = 1000;
  // Percentage of packets lost in the air (random, reproducible).
  uint32_t lossPercent = 0;
  Rng lossRng{1234};
  // Drop every Nth packet when > 0: a pathological pattern that can line up
  // with the heartbeat cadence and starve reply-only recovery.
  int dropEvery = 0;
  int dropCounter = 0;

  void deliver() {
    while (!queue.empty()) {
      Packet p = std::move(queue.front());
      queue.pop_front();
      if (lossPercent > 0 && lossRng.below(100) < lossPercent) continue;
      if (dropEvery > 0 && ++dropCounter % dropEvery == 0) continue;
      for (Node* n : nodes) {
        if (!n->online || memcmp(n->mac, p.from, MAC_LEN) == 0) continue;
        if (memcmp(p.to, BROADCAST_MAC, MAC_LEN) != 0 && memcmp(p.to, n->mac, MAC_LEN) != 0) continue;
        n->session.onPacket(p.from, p.data.data(), p.data.size(), now);
      }
    }
  }

  void run(const uint32_t ms, const uint32_t step = 50) {
    for (uint32_t t = 0; t < ms; t += step) {
      now += step;
      for (Node* n : nodes) {
        if (n->online) n->session.tick(now);
      }
      deliver();
    }
  }
};

bool Node::send(const uint8_t* dest, const uint8_t* data, const size_t len) {
  if (!online || muted) return false;
  Packet p;
  memcpy(p.from, mac, MAC_LEN);
  memcpy(p.to, dest, MAC_LEN);
  p.data.assign(data, data + len);
  bus->queue.push_back(std::move(p));
  return true;
}

class TableSessionTest : public ::testing::Test {
 protected:
  Bus bus;
  Node host, alice, bob;

  void SetUp() override {
    Node* all[] = {&host, &alice, &bob};
    const char* names[] = {"Host", "Alice", "Bob"};
    for (int i = 0; i < 3; i++) {
      all[i]->bus = &bus;
      all[i]->mac[5] = static_cast<uint8_t>(i + 1);
      all[i]->session.begin(all[i], all[i]->mac, names[i], 100 + i);
      bus.nodes.push_back(all[i]);
    }
    host.session.host(bus.now);
  }

  void seat(Node& guest) {
    guest.session.browse();
    bus.run(1500);
    ASSERT_GE(guest.session.tableCount(), 1);
    ASSERT_TRUE(guest.session.join(0, bus.now));
    bus.run(300);
    ASSERT_EQ(guest.session.status(), TableSession::Status::Seated);
  }

  // Everyone plays whatever the bot would over a lossy radio until the game
  // ends; every device must converge on the same final board.
  void playOutLossy(const uint32_t lossPercent, const int dropEvery) {
    seat(alice);
    seat(bob);
    bus.lossPercent = lossPercent;
    bus.dropEvery = dropEvery;
    ASSERT_TRUE(host.session.startGame(GameId::DotsAndBoxes, bus.now));
    Rng rng(1);
    for (int guard = 0; guard < 400; guard++) {
      bus.run(100);
      const Game* hv = host.session.view();
      if (!hv || hv->over()) break;
      Node* nodes[] = {&host, &alice, &bob};
      for (Node* n : nodes) {
        const Game* v = n->session.view();
        if (!v || n->session.actionPending() || v->currentSeat() != n->session.localSeat()) continue;
        uint8_t a[MAX_ACTION];
        const size_t len = v->botAction(static_cast<uint8_t>(v->currentSeat()), rng, a, sizeof(a));
        if (len > 0) n->session.submitAction(a, len, bus.now);
      }
    }
    bus.lossPercent = 0;
    bus.dropEvery = 0;
    bus.run(2500);
    ASSERT_TRUE(host.session.view()->over())
        << "turn=" << host.session.view()->currentSeat() << " alice=" << alice.session.localSeat() << "/"
        << int(alice.session.status()) << " pending=" << alice.session.actionPending()
        << " bob=" << bob.session.localSeat() << "/" << int(bob.session.status())
        << " pending=" << bob.session.actionPending();
    EXPECT_TRUE(alice.session.view()->over());
    EXPECT_TRUE(bob.session.view()->over());
    const auto* h = static_cast<const DotsAndBoxes*>(host.session.view());
    const auto* a = static_cast<const DotsAndBoxes*>(alice.session.view());
    for (uint8_t s = 0; s < 3; s++) EXPECT_EQ(h->score(s), a->score(s));
  }
};

}  // namespace

TEST_F(TableSessionTest, GuestsDiscoverAndJoin) {
  seat(alice);
  seat(bob);
  EXPECT_EQ(host.session.playerCount(), 3);
  EXPECT_EQ(alice.session.localSlot(), 1);
  EXPECT_EQ(bob.session.localSlot(), 2);
  bus.run(1200);
  EXPECT_STREQ(alice.session.seat(2).name, "Bob");
  EXPECT_STREQ(bob.session.seat(0).name, "Host");
}

TEST_F(TableSessionTest, MovesFlowThroughTheHost) {
  seat(alice);
  ASSERT_TRUE(host.session.startGame(GameId::ConnectFour, bus.now));
  bus.run(200);
  ASSERT_EQ(alice.session.phase(), Phase::Playing);
  ASSERT_NE(alice.session.view(), nullptr);
  const int aliceSeat = alice.session.localSeat();
  ASSERT_GE(aliceSeat, 0);

  const Game* view = alice.session.view();
  uint8_t col = 3;
  if (view->currentSeat() != aliceSeat) {
    ASSERT_TRUE(host.session.submitAction(&col, 1, bus.now));
    bus.run(200);
  }
  col = 4;
  ASSERT_TRUE(alice.session.submitAction(&col, 1, bus.now));
  EXPECT_TRUE(alice.session.actionPending());
  bus.run(200);
  EXPECT_FALSE(alice.session.actionPending());
  const auto* c4 = static_cast<const ConnectFour*>(alice.session.view());
  EXPECT_EQ(c4->cell(4, 0), aliceSeat + 1);
  const auto* hostView = static_cast<const ConnectFour*>(host.session.view());
  EXPECT_EQ(hostView->cell(4, 0), aliceSeat + 1);
}

TEST_F(TableSessionTest, RandomLossStillConverges) { playOutLossy(30, 0); }

TEST_F(TableSessionTest, PeriodicLossStillConverges) { playOutLossy(0, 3); }

TEST_F(TableSessionTest, TableSurvivesGameSwitches) {
  seat(alice);
  ASSERT_TRUE(host.session.startGame(GameId::ConnectFour, bus.now));
  bus.run(300);
  ASSERT_EQ(alice.session.gameId(), GameId::ConnectFour);
  ASSERT_TRUE(host.session.startGame(GameId::LiarsDice, bus.now));
  bus.run(300);
  EXPECT_EQ(alice.session.gameId(), GameId::LiarsDice);
  ASSERT_NE(alice.session.view(), nullptr);
  EXPECT_EQ(alice.session.view()->id(), GameId::LiarsDice);
  host.session.returnToLobby();
  bus.run(300);
  EXPECT_EQ(alice.session.phase(), Phase::Lobby);
  EXPECT_EQ(alice.session.status(), TableSession::Status::Seated);
  ASSERT_TRUE(host.session.startGame(GameId::DotsAndBoxes, bus.now));
  bus.run(300);
  EXPECT_EQ(alice.session.view()->id(), GameId::DotsAndBoxes);
  EXPECT_EQ(alice.session.status(), TableSession::Status::Seated);
}

TEST_F(TableSessionTest, LiarsDiceCupsStayPrivate) {
  seat(alice);
  ASSERT_TRUE(host.session.startGame(GameId::LiarsDice, bus.now));
  bus.run(300);
  const auto* mine = static_cast<const LiarsDice*>(alice.session.view());
  ASSERT_NE(mine, nullptr);
  const int me = alice.session.localSeat();
  const int other = 1 - me;
  EXPECT_GE(mine->die(static_cast<uint8_t>(me), 0), 1);
  EXPECT_EQ(mine->die(static_cast<uint8_t>(other), 0), 0);
  const auto* hostView = static_cast<const LiarsDice*>(host.session.view());
  EXPECT_EQ(hostView->die(static_cast<uint8_t>(me), 0), 0);
}

TEST_F(TableSessionTest, DroppedGuestRejoinsTheSameSeat) {
  seat(alice);
  ASSERT_TRUE(host.session.startGame(GameId::ConnectFour, bus.now));
  bus.run(300);
  const int seatBefore = alice.session.localSeat();
  alice.online = false;
  bus.run(PEER_TIMEOUT_MS + 1000);
  EXPECT_EQ(host.session.seat(1).status, SeatStatus::Away);
  alice.online = true;
  bus.run(PEER_TIMEOUT_MS + 2000);
  EXPECT_EQ(alice.session.status(), TableSession::Status::Seated);
  EXPECT_EQ(alice.session.localSeat(), seatBefore);
  EXPECT_EQ(host.session.seat(1).status, SeatStatus::Present);
}

TEST_F(TableSessionTest, AwayPlayerCanBeCoveredByTheCpu) {
  seat(alice);
  ASSERT_TRUE(host.session.startGame(GameId::DotsAndBoxes, bus.now));
  bus.run(300);
  alice.online = false;
  bus.run(PEER_TIMEOUT_MS + 500);
  host.session.setBotTakeover(true);
  // Host plays its own turns; the CPU plays Alice's.
  Rng rng(4);
  for (int guard = 0; guard < 300 && !host.session.view()->over(); guard++) {
    const Game* v = host.session.view();
    if (v->currentSeat() == host.session.localSeat()) {
      uint8_t a[MAX_ACTION];
      const size_t len = v->botAction(static_cast<uint8_t>(v->currentSeat()), rng, a, sizeof(a));
      ASSERT_TRUE(host.session.submitAction(a, len, bus.now));
    }
    bus.run(BOT_DELAY_MS + 100);
  }
  EXPECT_TRUE(host.session.view()->over());
}

TEST_F(TableSessionTest, LeavingMidGameForfeits) {
  seat(alice);
  ASSERT_TRUE(host.session.startGame(GameId::ConnectFour, bus.now));
  bus.run(300);
  const int aliceSeat = alice.session.localSeat();
  alice.session.leave();
  bus.run(300);
  const Game* v = host.session.view();
  ASSERT_TRUE(v->over());
  EXPECT_EQ(v->winnerMask(), 1u << (1 - aliceSeat));
  EXPECT_EQ(alice.session.status(), TableSession::Status::Idle);
  // Alice can sit back down as an onlooker while the finished match stands.
  seat(alice);
  EXPECT_EQ(alice.session.localSeat(), -1);
  EXPECT_NE(alice.session.localSlot(), 1);
}

TEST_F(TableSessionTest, ClosingTheTableNotifiesGuests) {
  seat(alice);
  seat(bob);
  host.session.leave();
  bus.run(100);
  EXPECT_EQ(alice.session.status(), TableSession::Status::Closed);
  EXPECT_EQ(bob.session.status(), TableSession::Status::Closed);
}

TEST_F(TableSessionTest, GuestNoticesAVanishedHost) {
  seat(alice);
  host.online = false;
  bus.run(PEER_TIMEOUT_MS + 500);
  EXPECT_EQ(alice.session.status(), TableSession::Status::Reconnecting);
  bus.run(HOST_LOST_MS);
  EXPECT_EQ(alice.session.status(), TableSession::Status::Lost);
}

TEST_F(TableSessionTest, FullTableDeniesJoin) {
  for (int i = 0; i < MAX_SLOTS - 1; i++) ASSERT_TRUE(host.session.addBot());
  alice.session.browse();
  bus.run(1500);
  ASSERT_TRUE(alice.session.join(0, bus.now));
  bus.run(300);
  EXPECT_EQ(alice.session.status(), TableSession::Status::Denied);
  EXPECT_EQ(alice.session.denyReason(), DenyReason::Full);
}

TEST(TableSessionSolo, CpuOpponentsPlayOut) {
  TableSession solo;
  const uint8_t mac[MAC_LEN] = {};
  solo.begin(nullptr, mac, "Me", 77);
  solo.hostSolo();
  ASSERT_TRUE(solo.addBot());
  ASSERT_TRUE(solo.addBot());
  ASSERT_TRUE(solo.startGame(GameId::LiarsDice, 0));
  EXPECT_EQ(solo.gameSeatCount(), 3);
  Rng rng(8);
  uint32_t now = 0;
  for (int guard = 0; guard < 5000 && !solo.view()->over(); guard++) {
    now += 250;
    const Game* v = solo.view();
    if (v->currentSeat() == solo.localSeat()) {
      uint8_t a[MAX_ACTION];
      const size_t len = v->botAction(static_cast<uint8_t>(solo.localSeat()), rng, a, sizeof(a));
      ASSERT_GT(len, 0u);
      ASSERT_TRUE(solo.submitAction(a, len, now));
    }
    solo.tick(now);
  }
  EXPECT_TRUE(solo.view()->over());
}

TEST(TableSessionSolo, StartingAloneAddsAnOpponent) {
  TableSession solo;
  const uint8_t mac[MAC_LEN] = {};
  solo.begin(nullptr, mac, "Me", 1);
  solo.hostSolo();
  ASSERT_TRUE(solo.startGame(GameId::ConnectFour, 0));
  EXPECT_EQ(solo.gameSeatCount(), 2);
  EXPECT_EQ(solo.seat(1).status, SeatStatus::Bot);
  EXPECT_STREQ(solo.seat(1).name, "CPU 1");
}

TEST_F(TableSessionTest, HumansAreSeatedBeforeCpuPlayers) {
  ASSERT_TRUE(host.session.addBot());
  seat(alice);
  ASSERT_TRUE(host.session.startGame(GameId::ConnectFour, bus.now));
  bus.run(300);
  EXPECT_GE(alice.session.localSeat(), 0);
  EXPECT_EQ(host.session.seatOfSlot(1), -1);  // the CPU player watches
}

TEST(TableWire, NamesNeverSplitACharacter) {
  // Eleven ASCII bytes then a two-byte 'ë': the 12-byte cut lands inside it.
  const char* name = "abcdefghijkë";
  EXPECT_EQ(utf8Prefix(name, NAME_LEN), 11u);
  EXPECT_EQ(utf8Prefix("ZoëZoëZoëZoë", NAME_LEN), 12u);  // cut falls on a boundary
  EXPECT_EQ(utf8Prefix("short", NAME_LEN), 5u);
  EXPECT_EQ(utf8Prefix("exactly12byt", NAME_LEN), 12u);
}

TEST_F(TableSessionTest, FreedGuestRejoiningAFullTableIsDenied) {
  seat(alice);
  // Alice still hears the host's beacons, so she keeps trying rather than
  // giving up, while the host frees her silent seat and the table fills.
  alice.muted = true;
  bus.run(PEER_TIMEOUT_MS + AWAY_DROP_MS + 1000);
  ASSERT_EQ(host.session.seat(1).status, SeatStatus::Empty);
  ASSERT_EQ(alice.session.status(), TableSession::Status::Reconnecting);
  for (int i = 0; i < MAX_SLOTS - 1; i++) ASSERT_TRUE(host.session.addBot());
  alice.muted = false;
  bus.run(5000);
  EXPECT_EQ(alice.session.status(), TableSession::Status::Denied);
}

TEST_F(TableSessionTest, StaleMoveNeverLandsInTheNextMatch) {
  seat(alice);
  ASSERT_TRUE(host.session.startGame(GameId::ConnectFour, bus.now));
  bus.run(300);
  // Make it Alice's turn.
  if (alice.session.view()->currentSeat() != alice.session.localSeat()) {
    uint8_t col = 0;
    ASSERT_TRUE(host.session.submitAction(&col, 1, bus.now));
    bus.run(300);
  }
  // Alice's move is lost in the air; the host restarts before the retry lands.
  uint8_t col = 6;
  bus.lossPercent = 100;
  ASSERT_TRUE(alice.session.submitAction(&col, 1, bus.now));
  bus.deliver();
  bus.lossPercent = 0;
  ASSERT_TRUE(host.session.startGame(GameId::ConnectFour, bus.now));
  bus.run(2000);
  const auto* board = static_cast<const ConnectFour*>(host.session.view());
  for (int c = 0; c < ConnectFour::COLS; c++) EXPECT_EQ(board->cell(c, 0), 0) << "column " << c;
  EXPECT_FALSE(alice.session.actionPending());
}

TEST(LiarsDice, DepartedBiddersBidIsWithdrawn) {
  LiarsDice g = craftedDice({{2, 2, 3, 4, 5}, {1, 6, 6, 6, 2}, {3, 3}}, 0);
  uint8_t a[MAX_ACTION];
  ASSERT_TRUE(g.apply(0, a, LiarsDice::encodeBid(a, 3, 4)));
  g.seatLeft(0);
  EXPECT_FALSE(g.hasBid());
  EXPECT_EQ(g.currentSeat(), 1);
  EXPECT_FALSE(g.apply(1, a, LiarsDice::encodeLiar(a)));
}

TEST_F(TableSessionTest, ClosedTableLeavesTheBrowseList) {
  alice.session.browse();
  bus.run(1500);
  ASSERT_EQ(alice.session.tableCount(), 1);
  host.session.leave();
  bus.run(100);
  EXPECT_EQ(alice.session.tableCount(), 0);
}

TEST_F(TableSessionTest, StartFailsCleanlyWhenNoSeatIsFree) {
  seat(alice);
  for (int i = 0; i < MAX_SLOTS - 2; i++) ASSERT_TRUE(host.session.addBot());
  ASSERT_TRUE(host.session.startGame(GameId::ConnectFour, bus.now));
  const int seats = host.session.gameSeatCount();
  // Every seat is taken; switching games still works and keeps the table.
  ASSERT_TRUE(host.session.startGame(GameId::LiarsDice, bus.now));
  EXPECT_EQ(host.session.gameSeatCount(), MAX_SLOTS);
  EXPECT_EQ(seats, 2);
}

// --- Murder Mystery -------------------------------------------------------------

namespace {

using Mystery = MurderMystery;

uint8_t solutionIndex(const Mystery& g, const uint8_t category) { return Mystery::indexOf(g.solutionCard(category)); }

// Index per category of the case file, with `card` swapped into its category.
void theoryWith(const Mystery& g, const uint8_t card, uint8_t* idx) {
  for (uint8_t c = 0; c < Mystery::CATEGORIES; c++) idx[c] = solutionIndex(g, c);
  idx[Mystery::categoryOf(card)] = Mystery::indexOf(card);
}

bool suggest(Mystery& g, const uint8_t seat, const uint8_t* idx) {
  uint8_t a[MAX_ACTION];
  const size_t len = Mystery::encodeSuggest(a, idx[0], idx[1], idx[2]);
  return g.apply(seat, a, len);
}

bool accuse(Mystery& g, const uint8_t seat, const uint8_t* idx) {
  uint8_t a[MAX_ACTION];
  const size_t len = Mystery::encodeAccuse(a, idx[0], idx[1], idx[2]);
  return g.apply(seat, a, len);
}

bool proceed(Mystery& g, const uint8_t seat) {
  uint8_t a[MAX_ACTION];
  return g.apply(seat, a, Mystery::encodeContinue(a));
}

Mystery viewAs(const Mystery& g, const uint8_t seat) {
  uint8_t buf[MAX_GAME_STATE];
  const size_t len = g.serialize(seat, buf, sizeof(buf));
  Mystery v;
  EXPECT_GT(len, 0u);
  EXPECT_TRUE(v.deserialize(buf, len));
  return v;
}

}  // namespace

TEST(MurderMystery, DealsEverythingButTheCaseFile) {
  Mystery g;
  g.reset(4, 1234);
  uint32_t dealt = 0;
  int total = 0;
  for (uint8_t s = 0; s < 4; s++) {
    EXPECT_EQ(dealt & g.handOf(s), 0u);
    dealt |= g.handOf(s);
    EXPECT_EQ(__builtin_popcount(g.handOf(s)), g.handSize(s));
    total += g.handSize(s);
  }
  uint32_t caseFile = 0;
  for (uint8_t c = 0; c < Mystery::CATEGORIES; c++) {
    const uint8_t card = g.solutionCard(c);
    ASSERT_LT(card, Mystery::CARD_COUNT);
    EXPECT_EQ(Mystery::categoryOf(card), c);
    caseFile |= Mystery::cardBit(card);
  }
  EXPECT_EQ(total, Mystery::CARD_COUNT - 3);
  EXPECT_EQ(dealt | caseFile, (1u << Mystery::CARD_COUNT) - 1u);
  EXPECT_EQ(dealt & caseFile, 0u);
}

TEST(MurderMystery, FirstHolderClockwiseShowsTheOnlyMatch) {
  Mystery g;
  g.reset(4, 42);
  const uint8_t turn = g.turnSeat();
  const auto holder = static_cast<uint8_t>((turn + 2) % 4);
  const uint8_t card = static_cast<uint8_t>(__builtin_ctz(g.handOf(holder)));
  uint8_t idx[3];
  theoryWith(g, card, idx);
  ASSERT_TRUE(suggest(g, turn, idx));
  // A lone match still waits for its holder, so the table cannot tell one
  // matching card from several.
  EXPECT_EQ(g.phase(), Mystery::Phase::Refute);
  EXPECT_EQ(g.refuterSeat(), holder);
  EXPECT_EQ(viewAs(g, static_cast<uint8_t>((turn + 1) % 4)).phase(), Mystery::Phase::Refute);
  uint8_t act[MAX_ACTION];
  ASSERT_TRUE(g.apply(holder, act, Mystery::encodeShow(act, card)));
  EXPECT_EQ(g.phase(), Mystery::Phase::Result);
  EXPECT_EQ(g.shownCard(), card);
  EXPECT_TRUE(g.seenBy(turn) & Mystery::cardBit(card));
  EXPECT_EQ(g.event(0).refuter, holder);
  EXPECT_EQ(viewAs(g, turn).shownCard(), card);
  EXPECT_EQ(viewAs(g, holder).shownCard(), card);
  EXPECT_EQ(viewAs(g, static_cast<uint8_t>((turn + 1) % 4)).shownCard(), Mystery::NO_CARD);
  EXPECT_FALSE(proceed(g, holder));
  ASSERT_TRUE(proceed(g, turn));
  EXPECT_EQ(g.phase(), Mystery::Phase::Turn);
  EXPECT_EQ(g.turnSeat(), (turn + 1) % 4);
}

TEST(MurderMystery, RefuterChoosesBetweenSeveralMatches) {
  // Find a deal where the next seat holds cards in two categories.
  for (uint32_t seed = 1; seed < 50; seed++) {
    Mystery g;
    g.reset(3, seed);
    const uint8_t turn = g.turnSeat();
    const auto next = static_cast<uint8_t>((turn + 1) % 3);
    const uint32_t suspects = g.handOf(next) & Mystery::categoryMask(0);
    const uint32_t weapons = g.handOf(next) & Mystery::categoryMask(1);
    if (!suspects || !weapons) continue;
    const auto a = static_cast<uint8_t>(__builtin_ctz(suspects));
    const auto b = static_cast<uint8_t>(__builtin_ctz(weapons));
    const uint8_t idx[3] = {Mystery::indexOf(a), Mystery::indexOf(b), solutionIndex(g, 2)};
    ASSERT_TRUE(suggest(g, turn, idx));
    ASSERT_EQ(g.phase(), Mystery::Phase::Refute);
    EXPECT_EQ(g.currentSeat(), next);
    uint8_t act[MAX_ACTION];
    EXPECT_FALSE(g.apply(next, act, Mystery::encodeShow(act, g.solutionCard(2))));
    EXPECT_FALSE(g.apply(turn, act, Mystery::encodeShow(act, b)));
    ASSERT_TRUE(g.apply(next, act, Mystery::encodeShow(act, b)));
    EXPECT_EQ(g.phase(), Mystery::Phase::Result);
    EXPECT_EQ(viewAs(g, turn).shownCard(), b);
    return;
  }
  FAIL() << "no suitable deal";
}

TEST(MurderMystery, UnansweredSuggestionIsPublic) {
  Mystery g;
  g.reset(3, 7);
  const uint8_t turn = g.turnSeat();
  const uint8_t idx[3] = {solutionIndex(g, 0), solutionIndex(g, 1), solutionIndex(g, 2)};
  ASSERT_TRUE(suggest(g, turn, idx));
  EXPECT_EQ(g.phase(), Mystery::Phase::Result);
  EXPECT_EQ(g.refuterSeat(), Mystery::NO_SEAT);
  const Mystery other = viewAs(g, static_cast<uint8_t>((turn + 1) % 3));
  ASSERT_EQ(other.eventCount(), 1);
  EXPECT_EQ(other.event(0).kind, Mystery::EventKind::Suggestion);
  EXPECT_EQ(other.event(0).seat, turn);
  EXPECT_EQ(other.event(0).refuter, Mystery::NO_SEAT);
  EXPECT_EQ(other.event(0).cards[1], g.solutionCard(1));
}

TEST(MurderMystery, ViewHidesOtherHandsAndTheCaseFile) {
  Mystery g;
  g.reset(4, 99);
  const Mystery v = viewAs(g, 1);
  EXPECT_EQ(v.handOf(1), g.handOf(1));
  EXPECT_EQ(v.handOf(0), 0u);
  EXPECT_EQ(v.handSize(0), g.handSize(0));
  for (uint8_t c = 0; c < Mystery::CATEGORIES; c++) EXPECT_EQ(v.solutionCard(c), Mystery::NO_CARD);
  const Mystery spectator = viewAs(g, SPECTATOR);
  for (uint8_t s = 0; s < 4; s++) EXPECT_EQ(spectator.handOf(s), 0u);
}

TEST(MurderMystery, WrongAccuserIsOutButStillShowsCards) {
  Mystery g;
  g.reset(3, 5);
  const uint8_t turn = g.turnSeat();
  uint8_t idx[3] = {solutionIndex(g, 0), solutionIndex(g, 1), solutionIndex(g, 2)};
  idx[2] = static_cast<uint8_t>((idx[2] + 1) % Mystery::PER_CATEGORY);
  ASSERT_TRUE(accuse(g, turn, idx));
  EXPECT_FALSE(g.over());
  EXPECT_TRUE(g.eliminated(turn));
  EXPECT_EQ(g.event(0).kind, Mystery::EventKind::WrongAccusation);
  // The accuser now sees the case file; nobody else does.
  EXPECT_EQ(viewAs(g, turn).solutionCard(0), g.solutionCard(0));
  EXPECT_EQ(viewAs(g, static_cast<uint8_t>((turn + 1) % 3)).solutionCard(0), Mystery::NO_CARD);
  ASSERT_TRUE(proceed(g, turn));
  // Two full rounds never hand the eliminated seat a turn.
  for (int i = 0; i < 4; i++) {
    const uint8_t s = g.turnSeat();
    EXPECT_NE(s, turn);
    const uint8_t sol[3] = {solutionIndex(g, 0), solutionIndex(g, 1), solutionIndex(g, 2)};
    ASSERT_TRUE(suggest(g, s, sol));
    ASSERT_TRUE(proceed(g, s));
  }
  // It still answers suggestions from its hand.
  const auto asker = g.turnSeat();
  const uint8_t card = static_cast<uint8_t>(__builtin_ctz(g.handOf(turn)));
  uint8_t probe[3];
  theoryWith(g, card, probe);
  ASSERT_TRUE(suggest(g, asker, probe));
  EXPECT_EQ(g.refuterSeat(), turn);
}

TEST(MurderMystery, RightAccusationWins) {
  Mystery g;
  g.reset(5, 11);
  const uint8_t turn = g.turnSeat();
  const uint8_t idx[3] = {solutionIndex(g, 0), solutionIndex(g, 1), solutionIndex(g, 2)};
  ASSERT_TRUE(accuse(g, turn, idx));
  EXPECT_TRUE(g.over());
  EXPECT_EQ(g.winnerMask(), 1u << turn);
  EXPECT_EQ(viewAs(g, static_cast<uint8_t>((turn + 1) % 5)).solutionCard(2), g.solutionCard(2));
}

TEST(MurderMystery, EveryoneWrongLeavesTheCaseUnsolved) {
  Mystery g;
  g.reset(3, 21);
  for (int i = 0; i < 3; i++) {
    const uint8_t s = g.turnSeat();
    uint8_t idx[3] = {solutionIndex(g, 0), solutionIndex(g, 1), solutionIndex(g, 2)};
    idx[0] = static_cast<uint8_t>((idx[0] + 1) % Mystery::PER_CATEGORY);
    ASSERT_TRUE(accuse(g, s, idx));
    if (i < 2) ASSERT_TRUE(proceed(g, s));
  }
  EXPECT_TRUE(g.over());
  EXPECT_EQ(g.winnerMask(), 0u);
}

TEST(MurderMystery, LeavingRefuterPassesToTheNextHolder) {
  for (uint32_t seed = 1; seed < 80; seed++) {
    Mystery g;
    g.reset(4, seed);
    const uint8_t turn = g.turnSeat();
    const auto first = static_cast<uint8_t>((turn + 1) % 4);
    const auto second = static_cast<uint8_t>((turn + 2) % 4);
    // first holds two matching cards (so it must choose), second holds a third.
    const uint32_t h1 = g.handOf(first);
    const uint32_t h2 = g.handOf(second);
    if (!(h1 & Mystery::categoryMask(0)) || !(h1 & Mystery::categoryMask(1)) || !(h2 & Mystery::categoryMask(2))) {
      continue;
    }
    const auto s = static_cast<uint8_t>(__builtin_ctz(h1 & Mystery::categoryMask(0)));
    const auto w = static_cast<uint8_t>(__builtin_ctz(h1 & Mystery::categoryMask(1)));
    const auto r = static_cast<uint8_t>(__builtin_ctz(h2 & Mystery::categoryMask(2)));
    const uint8_t idx[3] = {Mystery::indexOf(s), Mystery::indexOf(w), Mystery::indexOf(r)};
    ASSERT_TRUE(suggest(g, turn, idx));
    ASSERT_EQ(g.currentSeat(), first);
    g.seatLeft(first);
    EXPECT_TRUE(g.departed(first));
    EXPECT_EQ(g.revealedCards(), h1);
    EXPECT_EQ(g.refuterSeat(), second);
    EXPECT_EQ(g.currentSeat(), second);
    uint8_t act[MAX_ACTION];
    ASSERT_TRUE(g.apply(second, act, Mystery::encodeShow(act, r)));
    EXPECT_EQ(g.shownCard(), r);
    EXPECT_EQ(g.event(0).refuter, second);
    return;
  }
  FAIL() << "no suitable deal";
}

TEST(MurderMystery, SerializeRoundTripFitsAPacket) {
  Mystery g;
  g.reset(6, 3);
  uint8_t buf[MAX_GAME_STATE];
  const size_t len = g.serialize(2, buf, sizeof(buf));
  ASSERT_GT(len, 0u);
  Mystery v;
  ASSERT_TRUE(v.deserialize(buf, len));
  EXPECT_EQ(v.seatCount(), 6);
  EXPECT_EQ(v.turnSeat(), g.turnSeat());
  EXPECT_EQ(v.handOf(2), g.handOf(2));
  EXPECT_FALSE(v.deserialize(buf, len - 1));
}

TEST(MurderMystery, CpuDetectivesAlwaysSolveIt) {
  for (uint32_t seed = 1; seed <= 40; seed++) {
    Mystery g;
    const auto seats = static_cast<uint8_t>(3 + seed % 4);
    g.reset(seats, seed * 2654435761u);
    Rng rng(seed);
    int steps = 0;
    while (!g.over() && steps < 3000) {
      uint8_t a[MAX_ACTION];
      const int seat = g.currentSeat();
      const size_t len = g.botAction(static_cast<uint8_t>(seat), rng, a, sizeof(a));
      ASSERT_GT(len, 0u);
      ASSERT_TRUE(g.apply(static_cast<uint8_t>(seat), a, len)) << "seed " << seed << " step " << steps;
      steps++;
    }
    ASSERT_TRUE(g.over()) << "seed " << seed;
    // CPU players only accuse once certain, so the case is always solved.
    EXPECT_EQ(__builtin_popcount(g.winnerMask()), 1) << "seed " << seed;
  }
}

TEST_F(TableSessionTest, MurderMysteryHandsStayPrivate) {
  seat(alice);
  ASSERT_TRUE(host.session.startGame(GameId::MurderMystery, bus.now));
  bus.run(300);
  // Two readers plus the CPU player that tops up to the three-seat minimum.
  EXPECT_EQ(host.session.gameSeatCount(), 3);
  const auto* mine = static_cast<const MurderMystery*>(alice.session.view());
  const auto* hostView = static_cast<const MurderMystery*>(host.session.view());
  ASSERT_NE(mine, nullptr);
  const auto me = static_cast<uint8_t>(alice.session.localSeat());
  const auto hostSeat = static_cast<uint8_t>(host.session.localSeat());
  EXPECT_NE(mine->handOf(me), 0u);
  EXPECT_EQ(mine->handOf(hostSeat), 0u);
  EXPECT_EQ(hostView->handOf(me), 0u);
  EXPECT_EQ(mine->solutionCard(0), MurderMystery::NO_CARD);
}
