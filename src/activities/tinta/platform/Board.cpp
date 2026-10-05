#if LILA_TINTA

#include "platform/Board.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <esp_heap_caps.h>

#include "platform/Log.h"

namespace tinta::platform {

void Board::send(const RawInput& event) {
  if (count_ >= kQueueSize) {
    log("input queue full, event dropped");
    return;
  }
  queue_[(head_ + count_) % kQueueSize] = event;
  ++count_;
}

bool Board::nextInput(RawInput& out) {
  if (count_ == 0) return false;
  out = queue_[head_];
  head_ = static_cast<uint8_t>((head_ + 1) % kQueueSize);
  --count_;
  return true;
}

void Board::clearInput() {
  head_ = 0;
  count_ = 0;
  backDown_ = false;
  backHoldSent_ = false;
}

// lila's main loop has already updated HalGPIO for this pass; the edges read
// here are the same ones lila's own screens see. The keys are taken by
// position (HalGPIO's numbering is InputManager's), unmapped: Tinta labels
// its footer by where each key sits.
void Board::pollInput() {
  const uint32_t now = millis();
  RawInput event{};
  event.kind = RawInput::Kind::Button;
  for (uint8_t button = HalGPIO::BTN_CONFIRM; button <= HalGPIO::BTN_DOWN; ++button) {
    if (!gpio.wasPressed(button)) continue;
    event.button = static_cast<Button>(button);
    send(event);
  }

  // Back is reported on release, unless it was held long enough to mean the
  // pause sheet, which is reported as soon as the hold time is reached.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    backDown_ = true;
    backHoldSent_ = false;
    backDownMs_ = now;
  }
  if (backDown_) {
    if (!gpio.isPressed(HalGPIO::BTN_BACK)) {
      backDown_ = false;
      if (!backHoldSent_) {
        event = RawInput{};
        event.kind = RawInput::Kind::Button;
        event.button = Button::Back;
        send(event);
      }
    } else if (!backHoldSent_ && now - backDownMs_ >= kBackHoldMs) {
      backHoldSent_ = true;
      event = RawInput{};
      event.kind = RawInput::Kind::BackHold;
      send(event);
    }
  }

  if (!hasTouch()) return;
  event = RawInput{};
  if (gpio.wasSwipe(event.nx, event.ny, event.nx2, event.ny2)) {
    event.kind = RawInput::Kind::Swipe;
    send(event);
  } else if (gpio.wasTouchTap(event.nx, event.ny)) {
    event.kind = RawInput::Kind::Tap;
    send(event);
  }
  // A Home pad tap reaches TintaActivity::handleHomeGesture() as lila's Home
  // gesture; only the hold comes this way.
  if (gpio.hasHomeKey() && gpio.wasHomeKeyLongPressed()) {
    event = RawInput{};
    event.kind = RawInput::Kind::HomeHold;
    send(event);
  }
}

uint8_t* Board::frameBuffer() const { return renderer_.getFrameBuffer(); }
uint16_t Board::panelWidth() const { return display.getDisplayWidth(); }
uint16_t Board::panelHeight() const { return display.getDisplayHeight(); }
uint16_t Board::panelWidthBytes() const { return display.getDisplayWidthBytes(); }

void Board::present(const Refresh refresh) {
  const auto mode = refresh == Refresh::Full   ? HalDisplay::FULL_REFRESH
                    : refresh == Refresh::Half ? HalDisplay::HALF_REFRESH
                                               : HalDisplay::FAST_REFRESH;
  renderer_.displayBuffer(mode);
  presented_ = true;
}

Board::Refresh Board::screenRefresh() const { return device() == Device::X3 ? Refresh::Half : Refresh::Fast; }

bool Board::windowRefresh() const {
#if FREEINK_MCU_C3
  return BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::SSD1677;
#else
  return false;
#endif
}

bool Board::presentWindow(int16_t x, int16_t y, int16_t w, int16_t h) {
  if (!windowRefresh() || !presented_) return false;
  // Clip to the screen.
  if (x < 0) {
    w = static_cast<int16_t>(w + x);
    x = 0;
  }
  if (y < 0) {
    h = static_cast<int16_t>(h + y);
    y = 0;
  }
  if (x + w > logicalWidth()) w = static_cast<int16_t>(logicalWidth() - x);
  if (y + h > logicalHeight()) h = static_cast<int16_t>(logicalHeight() - y);
  if (w <= 0 || h <= 0) return false;
  // FreeInkUI's Portrait is the panel turned 90° clockwise: logical (x, y) is
  // panel column y, row panelHeight - 1 - x. Columns are packed eight to a
  // byte, and the window must start and end on a byte.
  const int32_t column0 = y & ~7;
  int32_t column1 = (y + h + 7) & ~7;
  if (column1 > panelWidth()) column1 = panelWidth();
  const int32_t row0 = panelHeight() - (x + w);
  display.displayWindow(static_cast<uint16_t>(column0), static_cast<uint16_t>(row0),
                        static_cast<uint16_t>(column1 - column0), static_cast<uint16_t>(w));
  return true;
}

Device Board::device() const {
  switch (BoardConfig::ACTIVE.board) {
    case BoardConfig::Board::XteinkX3:
    case BoardConfig::Board::XteinkX3Uc8279:
      return Device::X3;
    case BoardConfig::Board::XteinkX4Classic:
      return Device::X4Classic;
    case BoardConfig::Board::XteinkX4Pro:
      return Device::X4Pro;
    case BoardConfig::Board::XteinkX4:
    default:
      return Device::X4;
  }
}

const char* Board::id() const {
  static const char* const kIds[kDeviceCount] = {"X3", "X4", "X4CLASSIC", "X4PRO"};
  return kIds[static_cast<uint8_t>(device())];
}

const char* Board::name() const {
  static const char* const kNames[kDeviceCount] = {"Xteink X3", "Xteink X4", "Xteink X4 Classic", "Xteink X4 Pro"};
  return kNames[static_cast<uint8_t>(device())];
}

const char* Board::profileName() const { return BoardConfig::ACTIVE.name; }

const char* Board::panelController() const {
  switch (BoardConfig::ACTIVE.displayController) {
    case BoardConfig::DisplayController::SSD1677:
      return "SSD1677";
    case BoardConfig::DisplayController::UC8253:
      return "UC8253";
    case BoardConfig::DisplayController::UC8279:
      return "UC8279";
    case BoardConfig::DisplayController::UC8179:
      return "UC8179";
    default:
      return "other";
  }
}

int16_t Board::logicalWidth() const {
  const auto& p = BoardConfig::ACTIVE;
  return static_cast<int16_t>(p.displayWidth < p.displayHeight ? p.displayWidth : p.displayHeight);
}

int16_t Board::logicalHeight() const {
  const auto& p = BoardConfig::ACTIVE;
  return static_cast<int16_t>(p.displayWidth < p.displayHeight ? p.displayHeight : p.displayWidth);
}

Insets Board::bezelInsets() const {
  const auto& v = BoardConfig::ACTIVE.viewableInsets;
  return Insets{v.top, v.right, v.bottom, v.left};
}

bool Board::hasFrontKeys() const {
  // The ladder carries all six navigation keys; GPIO boards list each key.
  return BoardConfig::ACTIVE.inputStyle == BoardConfig::InputStyle::XteinkAdcLadder ||
         BoardConfig::ACTIVE.input.back >= 0;
}

bool Board::hasTouch() const { return BoardConfig::hasTouch(); }
bool Board::hasHomePad() const { return BoardConfig::hasHomeKey(); }
bool Board::hasFrontlight() const { return Frontlight.present(); }
bool Board::hasRtc() const { return halClock.isAvailable(); }

BatteryReading Board::readBattery() const {
  BatteryReading reading;
  const uint16_t percent = powerManager.getBatteryPercentage();
  reading.percentKnown = true;
  reading.percent = static_cast<uint8_t>(percent > 100 ? 100 : percent);
  reading.chargingKnown = true;
  reading.charging = gpio.isUsbConnected();
  return reading;
}

uint32_t Board::freeHeap() const {
  return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

uint32_t Board::minFreeHeap() const {
  return static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

}  // namespace tinta::platform

#endif  // LILA_TINTA
