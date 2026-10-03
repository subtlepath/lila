#pragma once
#include <Epub.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// The reader menu. The first page carries only what readers reach for while
// reading (contents, search, position, text, look-up, light, bookmarks); everything
// else sits one level down under More Options, and repair tools under More
// Options > Troubleshooting. Back closes the menu from any page, straight to
// the page being read.
class EpubReaderMenuActivity final : public UiListActivity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction {
    SELECT_CHAPTER,
    FOOTNOTES,
    TEXT_SETTINGS,
    NIGHT_MODE,
    FRONTLIGHT,
    GO_TO_PERCENT,
    AUTO_PAGE_TURN,
    ROTATE_SCREEN,
    BOOKMARKS,
    TOGGLE_BOOKMARK,
    SCREENSHOT,
    DISPLAY_QR,
    GO_HOME,
    SYNC,
    DELETE_CACHE,
    DICTIONARY,
    MORE_OPTIONS,
    TROUBLESHOOTING,
    REFRESH_SCREEN,
    SEARCH
  };

  enum class MenuPage : uint8_t { Main, More, Troubleshooting };

  struct MenuItem {
    MenuAction action;
    StrId labelId;
    // Whitespace above the row separates it from the group before.
    bool startsGroup = false;
  };

  // Reader state that decides which rows exist and what they say.
  struct MenuContext {
    bool hasFootnotes = false;
    int footnoteCount = 0;
    int bookmarkCount = 0;
    bool pageBookmarked = false;
    bool canSync = false;
  };

  static void buildMenuItems(std::vector<MenuItem>& items, MenuPage page, const MenuContext& context);
  // The toggle row's label follows the page's bookmark state.
  static StrId labelFor(const MenuItem& item, const MenuContext& context);
  static freeink::ui::BitmapRef iconFor(MenuAction action, const MenuContext& context);
  // One-line explanation under a Troubleshooting row; nullptr elsewhere.
  static const char* descriptionFor(MenuAction action);
  static StrId pageTitle(MenuPage page);

  struct Position {
    std::string chapterTitle;
    int currentPage = 0;
    int totalPages = 0;
    int bookPercent = 0;
  };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  Position position, std::string textValue, uint8_t currentOrientation,
                                  uint8_t pageTurnOption, const MenuContext& context);

  void render(RenderLock&&) override;
  bool handleHomeGesture() override;

 private:
  // Fixed-capacity row storage, so rendering never allocates. Labels and icons
  // are set when a page loads; buildScreen() refreshes only the values.
  static constexpr size_t MAX_MENU_ITEMS = 12;
  static constexpr size_t VALUE_LEN = 24;
  freeink::ui::ListItem menuRowItems[MAX_MENU_ITEMS]{};
  char rowValues[MAX_MENU_ITEMS][VALUE_LEN]{};
  void loadPage(MenuPage page);
  void showPage(MenuPage page);

  int listCount() const override { return static_cast<int>(menuItems.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Popup input runs before any button or touch handling.
  bool handleCustomInput() override;
  // Back closes on RELEASE and Confirm activates on RELEASE; everything else
  // (row navigation, page jumps) falls through to the base handler.
  bool handleButtons() override;
  // Header via GUI.drawHeader inside the safe area for the battery indicator.
  void drawChrome() override;
  void drawFooter() override;

  void closeCancelled();
  void refreshRowValue(size_t row);

  MenuPage page = MenuPage::Main;
  std::vector<MenuItem> menuItems;
  MenuContext context;

  OptionPopup optionPopup;
  std::string title;
  Position position;
  std::string textValue;
  uint8_t pendingOrientation = 0;
  uint8_t selectedPageTurnOption = 0;
};
