#include "LibraryMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
struct MenuRow {
  LibraryMenuActivity::Entry entry;
  StrId label;
  UIIcon icon;
};

constexpr MenuRow MENU_ROWS[] = {
    {LibraryMenuActivity::Entry::Folders, StrId::STR_BROWSE_FILES, UIIcon::Folder},
    {LibraryMenuActivity::Entry::AddBooks, StrId::STR_ADD_BOOKS, UIIcon::Transfer},
    {LibraryMenuActivity::Entry::Catalogs, StrId::STR_OPDS_BROWSER, UIIcon::Blocks},
    {LibraryMenuActivity::Entry::Games, StrId::STR_GAMES, UIIcon::Dices},
#if LILA_TINTA
    {LibraryMenuActivity::Entry::Tinta, StrId::STR_TINTA, UIIcon::Text},
#endif
#if LILA_COMPANION
    {LibraryMenuActivity::Entry::Companion, StrId::STR_COMPANION_CONNECT_SYNC, UIIcon::Transfer},
#endif
    {LibraryMenuActivity::Entry::Settings, StrId::STR_SETTINGS_TITLE, UIIcon::Settings},
};
static_assert(sizeof(MENU_ROWS) / sizeof(MENU_ROWS[0]) == LibraryMenuActivity::MAX_ROWS, "menu row storage");
}  // namespace

LibraryMenuActivity::LibraryMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("LibraryMenu", renderer, mappedInput) {}

void LibraryMenuActivity::onEnter() {
  UiListActivity::onEnter();
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  lockNextBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);

  // Catalogs only appear once a server is configured (Settings > OPDS).
  const bool hasCatalogs = OPDS_STORE.hasServers();
  rowCount = 0;
  for (const auto& row : MENU_ROWS) {
    if (row.entry == Entry::Catalogs && !hasCatalogs) continue;
    entries[rowCount] = row.entry;
    fui::ListItem item;
    item.label = I18N.get(row.label);
    item.icon = listIconFor(row.icon);
    item.actionValue = static_cast<int16_t>(rowCount);
    rowItems[rowCount++] = item;
  }
}

bool LibraryMenuActivity::handleCustomInput() {
  if (lockNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    lockNextConfirmRelease = false;
    return true;
  }
  if (lockNextBackRelease && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockNextBackRelease = false;
    return true;
  }
  return false;
}

const char* LibraryMenuActivity::headerTitle() const { return tr(STR_HOME_MENU); }

void LibraryMenuActivity::activateIndex(const int index) {
  if (index < 0 || index >= rowCount) return;
  app.clearTapFlash();
  switch (entries[index]) {
    case Entry::Folders:
      activityManager.goToFileBrowser();
      break;
    case Entry::AddBooks:
      activityManager.goToFileTransfer();
      break;
    case Entry::Catalogs:
      activityManager.goToBrowser();
      break;
    case Entry::Games:
      activityManager.goToGames();
      break;
    case Entry::Tinta:
#if LILA_TINTA
      activityManager.goToTinta();
#endif
      break;
    case Entry::Companion:
#if LILA_COMPANION
      activityManager.goToCompanion();
#endif
      break;
    case Entry::Settings:
      activityManager.goToSettings();
      break;
  }
}

void LibraryMenuActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band GUI.drawHeader paints.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(screen.theme().spaceLg);

  fui::ListProps props;
  props.items = rowItems;
  props.count = static_cast<uint16_t>(rowCount);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  applyListControlStyle(props, screen.theme());
  syncListViewport(screen, props);
  screen.list(props);
}

// Back returns to the shelf; Home is the only place this menu opens from.
void LibraryMenuActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
