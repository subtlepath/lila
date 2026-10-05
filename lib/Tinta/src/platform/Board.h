#pragma once

// Tinta's view of the hardware, inside lila: the display, input and battery
// belong to lila (GfxRenderer, HalGPIO, HalPowerManager) and Board only
// answers device questions from BoardConfig::ACTIVE and presents tinta's
// frames. Implemented in src/activities/tinta/platform/Board.cpp.

#include <stdint.h>

class GfxRenderer;

namespace tinta::platform {

// lila builds the X3 and X4 into one ESP32-C3 image and tells them apart at
// boot; the X4 Classic and X4 Pro each have an ESP32-S3 image.
enum class Device : uint8_t { X3, X4, X4Classic, X4Pro };
constexpr uint8_t kDeviceCount = 4;

// The physical keys, in InputManager's numbering. Tinta draws its footer over
// the front keys by position, so it takes them unmapped.
enum class Button : uint8_t { Back, Confirm, Left, Right, Up, Down, Power };

// Holding Back this long opens the pause sheet instead of going back
// (PLAN.md §4.4). A short press reports Back on release.
constexpr uint32_t kBackHoldMs = 600;

// One input event. Positions are normalized (0..1) in the panel's native
// frame; ui/KeyMap maps them to the logical screen.
struct RawInput {
  enum class Kind : uint8_t { Button, BackHold, Tap, Swipe, HomeTap, HomeHold };
  Kind kind;
  Button button;  // kind == Button
  float nx;       // Tap position, Swipe start
  float ny;
  float nx2;  // Swipe end
  float ny2;
};

// Panel rows and columns the bezel covers, in the portrait frame.
struct Insets {
  int16_t top;
  int16_t right;
  int16_t bottom;
  int16_t left;
};

struct BatteryReading {
  bool percentKnown = false;
  uint8_t percent = 0;
  bool chargingKnown = false;
  bool charging = false;

  bool operator==(const BatteryReading& o) const {
    return percentKnown == o.percentKnown && percent == o.percent && chargingKnown == o.chargingKnown &&
           charging == o.charging;
  }
  bool operator!=(const BatteryReading& o) const { return !(*this == o); }
};

class Board {
 public:
  explicit Board(GfxRenderer& renderer) : renderer_(renderer) {}

  // Reads lila's input for this pass into the queue: the activity calls it
  // once per loop, before the app takes events with nextInput().
  void pollInput();
  bool nextInput(RawInput& out);
  // Queues an event the activity received another way (lila's Home gesture).
  void send(const RawInput& event);
  // Drops queued events and the Back hold in progress.
  void clearInput();

  // Framebuffer in the panel's native orientation (1 bpp, set bit = white).
  uint8_t* frameBuffer() const;
  uint16_t panelWidth() const;
  uint16_t panelHeight() const;
  uint16_t panelWidthBytes() const;

  // Pushes the framebuffer and waits for the waveform (lila's render task).
  // Full: the cleanest waveform, slow. Half: one self-contained pass. Fast:
  // differential.
  enum class Refresh : uint8_t { Full, Half, Fast };
  void present(Refresh refresh);
  // The refresh for a new screen or card: a half refresh on the X3 (one quiet
  // pass, no ghost), fast on the X4 family, where a half refresh flashes.
  Refresh screenRefresh() const;
  // A fast refresh of one rectangle in the DisplayTarget's logical portrait
  // coordinates (the reader's word cursor); false when the caller presents
  // the whole frame instead. Only where windowRefresh().
  bool presentWindow(int16_t x, int16_t y, int16_t w, int16_t h);
  // The X4's SSD1677: the other panels' drivers have no window.
  bool windowRefresh() const;

  Device device() const;
  const char* id() const;           // X3, X4, X4CLASSIC, X4PRO
  const char* name() const;         // for people
  const char* profileName() const;  // BoardConfig profile, e.g. xteink_x3_uc8279
  const char* panelController() const;

  // Portrait, the way Tinta is always held.
  int16_t logicalWidth() const;
  int16_t logicalHeight() const;
  // The board profile's viewable insets: content stays out of them.
  Insets bezelInsets() const;

  bool hasFrontKeys() const;
  bool hasTouch() const;
  bool hasHomePad() const;
  bool hasFrontlight() const;
  bool hasRtc() const;

  bool touchFlipX() const { return false; }
  bool touchFlipY() const { return false; }

  BatteryReading readBattery() const;

  // Internal RAM: free now and the lowest it has been.
  uint32_t freeHeap() const;
  uint32_t minFreeHeap() const;

 private:
  static constexpr uint8_t kQueueSize = 16;

  GfxRenderer& renderer_;
  RawInput queue_[kQueueSize] = {};
  uint8_t head_ = 0;
  uint8_t count_ = 0;
  uint32_t backDownMs_ = 0;
  bool backDown_ = false;
  bool backHoldSent_ = false;
  bool presented_ = false;
};

}  // namespace tinta::platform
