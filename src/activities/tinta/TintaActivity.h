#pragma once

#if LILA_TINTA

#include <atomic>
#include <memory>

#include "activities/Activity.h"

namespace tinta::app {
class App;
}
struct TintaCompanionSession;

// Tinta, the Spanish course (lib/Tinta), as a lila app. Tinta draws its own
// FreeInkUI screens straight into the framebuffer and keeps its progress and
// course in /tinta/ on the SD card (docs/tinta.md). Everything it allocates
// lives from onEnter() to onExit(), so a closed Tinta costs no RAM.
class TintaActivity final : public Activity {
 public:
  TintaActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~TintaActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // The X4 Pro's Home pad: Tinta's pause sheet, as on its own firmware.
  bool handleHomeGesture() override;
  // The word card (PLAN.md 4.7) instead of the screen Tinta was on.
  bool drawSleepFrame() override;

 private:
  void requestFrame();

  std::unique_ptr<tinta::app::App> app;
  std::unique_ptr<TintaCompanionSession> companionSession;
  bool failed = false;
  // Set when this activity asked for the frame; a render nobody here asked for
  // follows an activity drawn on top (lila's light panel), so Tinta repaints.
  std::atomic<bool> frameRequested{false};
  // The render task's stack low-water mark while Tinta draws, for the log.
  unsigned renderStackLow = ~0u;
};

#endif  // LILA_TINTA
