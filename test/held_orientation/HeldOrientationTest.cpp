#include <gtest/gtest.h>

#include <cmath>

#include "HeldOrientation.h"

namespace {

constexpr float kPi = 3.14159265f;

// Gravity as the accelerometer reads it with the glass upright, reclined back
// by reclineDeg (0 = vertical, 90 = flat), and the edge at angleDeg in the
// glass's plane pointing down (0 = the portrait bottom, -X).
void held(const float angleDeg, const float reclineDeg, float& ax, float& ay, float& az) {
  const float inPlane = std::cos(reclineDeg * kPi / 180.0f);
  ax = -inPlane * std::cos(angleDeg * kPi / 180.0f);
  ay = -inPlane * std::sin(angleDeg * kPi / 180.0f);
  az = -std::sin(reclineDeg * kPi / 180.0f);
}

int8_t classifyHeld(const float angleDeg, const float reclineDeg) {
  float ax, ay, az;
  held(angleDeg, reclineDeg, ax, ay, az);
  return HeldOrientation::classify(ax, ay, az);
}

// Feeds the same hold every 50 ms from startMs to endMs; returns the last update().
bool hold(HeldOrientation& h, const float angleDeg, const unsigned long startMs, const unsigned long endMs) {
  float ax, ay, az;
  held(angleDeg, 30, ax, ay, az);
  bool settled = false;
  for (unsigned long t = startMs; t <= endMs; t += 50) settled = h.update(ax, ay, az, t) || settled;
  return settled;
}

}  // namespace

TEST(HeldOrientation, EachEdgeDown) {
  EXPECT_EQ(classifyHeld(0, 0), CrossPointOrientation::PORTRAIT);
  EXPECT_EQ(classifyHeld(180, 0), CrossPointOrientation::INVERTED);
  EXPECT_EQ(classifyHeld(90, 0), CrossPointOrientation::LANDSCAPE_CCW);
  EXPECT_EQ(classifyHeld(-90, 0), CrossPointOrientation::LANDSCAPE_CW);
}

TEST(HeldOrientation, ReclinedReadingStillCounts) {
  EXPECT_EQ(classifyHeld(0, 60), CrossPointOrientation::PORTRAIT);
  EXPECT_EQ(classifyHeld(90, 60), CrossPointOrientation::LANDSCAPE_CCW);
}

TEST(HeldOrientation, FlatOrFallingSaysNothing) {
  EXPECT_EQ(classifyHeld(0, 90), HeldOrientation::NONE);
  EXPECT_EQ(classifyHeld(0, 70), HeldOrientation::NONE);  // 20° from flat
  EXPECT_EQ(HeldOrientation::classify(0, 0, 0), HeldOrientation::NONE);
  EXPECT_EQ(HeldOrientation::classify(0, 0, -1), HeldOrientation::NONE);  // face down
}

TEST(HeldOrientation, DiagonalSaysNothing) {
  EXPECT_EQ(classifyHeld(45, 0), HeldOrientation::NONE);
  EXPECT_EQ(classifyHeld(35, 0), HeldOrientation::NONE);
  EXPECT_EQ(classifyHeld(25, 0), CrossPointOrientation::PORTRAIT);
  EXPECT_EQ(classifyHeld(65, 0), CrossPointOrientation::LANDSCAPE_CCW);
}

TEST(HeldOrientation, TurnsOnceAfterSettling) {
  HeldOrientation h;
  uint8_t to = 0xFF;
  EXPECT_FALSE(hold(h, 90, 0, 550));  // not yet settled
  EXPECT_FALSE(h.takeTurn(to));
  EXPECT_TRUE(hold(h, 90, 600, 650));
  ASSERT_TRUE(h.takeTurn(to));
  EXPECT_EQ(to, CrossPointOrientation::LANDSCAPE_CCW);
  EXPECT_FALSE(h.takeTurn(to));  // taken once
  EXPECT_FALSE(hold(h, 90, 700, 3000));  // the same hold does not turn again
  EXPECT_FALSE(h.takeTurn(to));
}

TEST(HeldOrientation, MovementRestartsTheSettle) {
  HeldOrientation h;
  uint8_t to;
  hold(h, 0, 0, 400);
  hold(h, 45, 450, 450);  // passes the diagonal
  EXPECT_FALSE(hold(h, 0, 500, 1000));
  EXPECT_FALSE(h.takeTurn(to));
  EXPECT_TRUE(hold(h, 0, 1050, 1100));
  EXPECT_TRUE(h.takeTurn(to));
}

TEST(HeldOrientation, LaidDownAndPickedUpTheSameWayKeepsIt) {
  HeldOrientation h;
  uint8_t to;
  hold(h, 0, 0, 1000);
  ASSERT_TRUE(h.takeTurn(to));
  float ax, ay, az;
  held(0, 90, ax, ay, az);
  for (unsigned long t = 1050; t <= 3000; t += 50) h.update(ax, ay, az, t);
  EXPECT_FALSE(hold(h, 0, 3050, 5000));
  EXPECT_FALSE(h.takeTurn(to));
  EXPECT_TRUE(hold(h, 180, 5050, 6000));
  ASSERT_TRUE(h.takeTurn(to));
  EXPECT_EQ(to, CrossPointOrientation::INVERTED);
}

TEST(HeldOrientation, ResetReportsTheCurrentHoldAgain) {
  HeldOrientation h;
  uint8_t to;
  hold(h, -90, 0, 1000);
  ASSERT_TRUE(h.takeTurn(to));
  h.reset();
  EXPECT_TRUE(hold(h, -90, 1050, 2000));
  ASSERT_TRUE(h.takeTurn(to));
  EXPECT_EQ(to, CrossPointOrientation::LANDSCAPE_CW);
}
