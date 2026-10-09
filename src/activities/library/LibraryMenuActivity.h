#pragma once

#include <atomic>
#include <cstdint>

#include "activities/UiListActivity.h"

// The bookshelf's secondary menu: everything Home offers besides reading.
// Pushed over the Library; Back returns to the shelf as it was, and every row
// replaces the stack with its screen.
class LibraryMenuActivity final : public UiListActivity {
 public:
  LibraryMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onBackgroundSaveFailed() override;

  enum class Entry : uint8_t { Folders, AddBooks, Catalogs, Games, Tinta, Companion, Settings };
  static constexpr int MAX_ROWS = 5
#if LILA_TINTA
                                  + 1
#endif
#if LILA_COMPANION
                                  + 1
#endif
      ;

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
  std::atomic<bool> syncStartFailed{false};
  // Entered from a held Confirm or Back: their releases belong to the shelf.
  bool lockNextConfirmRelease = false;
  bool lockNextBackRelease = false;
};
