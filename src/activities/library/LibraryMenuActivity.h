#pragma once

#include <cstdint>

#include "activities/UiListActivity.h"

// The bookshelf's secondary menu: everything Home offers besides reading.
// Pushed over the Library; Back returns to the shelf as it was, and every row
// replaces the stack with its screen.
class LibraryMenuActivity final : public UiListActivity {
 public:
  LibraryMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;

  enum class Entry : uint8_t { Folders, AddBooks, Catalogs, Games, Tinta, Settings };
#if LILA_TINTA
  static constexpr int MAX_ROWS = 6;
#else
  static constexpr int MAX_ROWS = 5;
#endif

 private:
  int listCount() const override { return rowCount; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;
  void drawFooter() override;

  Entry entries[MAX_ROWS]{};
  freeink::ui::ListItem rowItems[MAX_ROWS]{};
  int rowCount = 0;
  // Entered from a held Confirm or Back: their releases belong to the shelf.
  bool lockNextConfirmRelease = false;
  bool lockNextBackRelease = false;
};
