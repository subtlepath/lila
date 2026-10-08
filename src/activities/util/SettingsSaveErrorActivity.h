#pragma once

#include <I18n.h>

#include "activities/Activity.h"
#include "components/UITheme.h"

class SettingsSaveErrorActivity final : public Activity {
 public:
  SettingsSaveErrorActivity(GfxRenderer& renderer, MappedInputManager& input)
      : Activity("SaveError", renderer, input) {}
  void onEnter() override {
    Activity::onEnter();
    requestUpdate();
  }
  void loop() override {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm))
      finish();
  }
  void render(RenderLock&&) override {
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_SETTINGS_SAVE_FAILED));
  }
};
