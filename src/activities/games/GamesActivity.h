#pragma once

#include "activities/UiListActivity.h"

// Home > Games: a launcher for hosting or joining a table over ESP-NOW, or a
// solo game against the CPU. Launching replaces this screen with the table,
// which returns here when the reader leaves.
class GamesActivity final : public UiListActivity {
 public:
  GamesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;

  static constexpr int ROW_COUNT = 7;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void onBackButton() override;
  const char* headerTitle() const override;

  void editPlayerName();
  void swallowHeldReleases();

  // Rows are a fixed menu; storage lives here for the render pass.
  freeink::ui::ListItem rowItems[ROW_COUNT]{};
  char playerNameBuf[16] = {};
  bool lockNextConfirmRelease = false;
  bool lockNextBackRelease = false;
};
