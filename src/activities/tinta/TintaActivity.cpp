#if LILA_TINTA

#include "TintaActivity.h"

// Before lila's I18n.h, whose tr() macro would rewrite Tinta's own ui::tr().
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "activities/util/FrontlightPanelActivity.h"
#include "app/App.h"
#include "components/UITheme.h"

namespace {

uint32_t freeInternal() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
uint32_t largestInternal() { return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

}  // namespace

TintaActivity::TintaActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Tinta", renderer, mappedInput) {}

TintaActivity::~TintaActivity() = default;

void TintaActivity::onEnter() {
  Activity::onEnter();
  LOG_DBG("TNT", "Before open: free %lu, largest %lu", static_cast<unsigned long>(freeInternal()),
          static_cast<unsigned long>(largestInternal()));
  // Opened before render() can see it: a render may still be on its way.
  auto opened = makeUniqueNoThrow<tinta::app::App>(renderer);
  if (!opened || !opened->open()) {
    LOG_ERR("TNT", "OOM: tinta app (free %lu, largest %lu)", static_cast<unsigned long>(freeInternal()),
            static_cast<unsigned long>(largestInternal()));
    opened.reset();
  } else {
    LOG_DBG("TNT", "Opened: free %lu, largest %lu", static_cast<unsigned long>(freeInternal()),
            static_cast<unsigned long>(largestInternal()));
  }
  {
    RenderLock lock;
    app = std::move(opened);
    failed = !app;
  }
  requestFrame();
}

void TintaActivity::onExit() {
  // lila goes to sleep or elsewhere through here, with the render lock held:
  // Tinta writes what it holds.
  if (app) {
    app->close();
    app.reset();
  }
  LOG_DBG("TNT", "Closed: free %lu, largest %lu, render stack low water %u", static_cast<unsigned long>(freeInternal()),
          static_cast<unsigned long>(largestInternal()), renderStackLow);
  Activity::onExit();
}

void TintaActivity::requestFrame() {
  frameRequested.store(true);
  requestUpdate();
}

void TintaActivity::loop() {
  if (failed) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasBackGesture()) {
      onGoHome();
    }
    return;
  }
  app->board().pollInput();

  RenderLock lock(RenderLock::Mode::Try);
  // The panel is still refreshing: input stays queued for the next pass and
  // is routed against the frame that is going up.
  if (!lock.ownsLock()) return;
  app->update();
  const bool exit = app->exitRequested();
  const tinta::app::App::HostRequest request = app->takeHostRequest();
  const bool frame = app->frameWanted();
  lock.unlock();

  if (exit) {
    onGoHome();
    return;
  }
  if (request == tinta::app::App::HostRequest::Light) {
    auto panel = makeUniqueNoThrow<FrontlightPanelActivity>(renderer, mappedInput);
    if (panel) {
      startActivityForResult(std::move(panel), [this](const ActivityResult&) { requestUpdate(); });
      return;
    }
    LOG_ERR("TNT", "OOM: light panel");
  }
  if (frame) requestFrame();
}

void TintaActivity::render(RenderLock&&) {
  if (!app) {
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_DICT_LOW_MEMORY));
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    return;
  }
  app->renderFrame(!frameRequested.exchange(false));
  const unsigned low = uxTaskGetStackHighWaterMark(nullptr);
  if (low < renderStackLow) renderStackLow = low;
}

bool TintaActivity::drawSleepFrame() { return app && app->drawSleepCard(); }

bool TintaActivity::handleHomeGesture() {
  if (!app || !gpio.hasHomeKey()) return false;
  tinta::platform::RawInput event{};
  event.kind = tinta::platform::RawInput::Kind::HomeTap;
  app->board().send(event);
  return true;
}

#endif  // LILA_TINTA
