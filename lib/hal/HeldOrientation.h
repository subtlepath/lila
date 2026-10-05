#pragma once

#include <cmath>
#include <cstdint>

// TODO: Move enums into new header and share with CrossPointSettings.h
namespace CrossPointOrientation {
enum Value : uint8_t { PORTRAIT = 0, LANDSCAPE_CW = 1, INVERTED = 2, LANDSCAPE_CCW = 3 };
}

// Which way the device is held, from the accelerometer in the SDK's board frame
// (X along the glass's long side, Y along its short side). The frame is the
// X3's, as its glass was seen to turn in xtwiki: gravity toward -X is portrait,
// toward -Y landscape CCW. Other boards reach it through their profile's IMU
// mount correction.
//
// A hold counts once it has stayed put for SETTLE_MS. Lying near flat, or held
// near a diagonal, says nothing, so the page keeps the orientation it has.
class HeldOrientation {
 public:
  static constexpr int8_t NONE = -1;
  // Share of gravity in the glass's plane below which the device is lying flat (~24° from flat).
  static constexpr float MIN_UPRIGHT = 0.4f;
  // An edge is down when in-plane gravity falls within ~30° of its axis; between, it is a diagonal.
  static constexpr float MIN_AXIS_SHARE = 0.87f;
  static constexpr unsigned long SETTLE_MS = 600;

  // One sample (g) as an orientation value, or NONE.
  static int8_t classify(const float ax, const float ay, const float az) {
    const float inPlane = std::sqrt(ax * ax + ay * ay);
    const float total = std::sqrt(inPlane * inPlane + az * az);
    if (total < 0.5f || inPlane < total * MIN_UPRIGHT) return NONE;  // falling, or lying flat
    if (std::fabs(ax) >= inPlane * MIN_AXIS_SHARE) {
      return ax < 0 ? CrossPointOrientation::PORTRAIT : CrossPointOrientation::INVERTED;
    }
    if (std::fabs(ay) >= inPlane * MIN_AXIS_SHARE) {
      return ay < 0 ? CrossPointOrientation::LANDSCAPE_CCW : CrossPointOrientation::LANDSCAPE_CW;
    }
    return NONE;
  }

  // Forgets the hold, so the next one to settle is reported even if unchanged.
  void reset() {
    leaning = NONE;
    held = NONE;
    turned = false;
  }

  // Feeds one sample; true when a new hold has just settled.
  bool update(const float ax, const float ay, const float az, const unsigned long nowMs) {
    const int8_t lean = classify(ax, ay, az);
    if (lean != leaning) {
      leaning = lean;
      leanSinceMs = nowMs;
      return false;
    }
    if (lean == NONE || lean == held || nowMs - leanSinceMs < SETTLE_MS) return false;
    held = lean;
    turned = true;
    return true;
  }

  // The settled hold as an orientation value, or NONE.
  int8_t current() const { return held; }

  // Once per settled change: the orientation the device is now held in.
  bool takeTurn(uint8_t& orientation) {
    if (!turned) return false;
    turned = false;
    orientation = static_cast<uint8_t>(held);
    return true;
  }

 private:
  int8_t leaning = NONE;
  int8_t held = NONE;
  bool turned = false;
  unsigned long leanSinceMs = 0;
};
