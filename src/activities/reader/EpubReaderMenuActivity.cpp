#include "EpubReaderMenuActivity.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <I18n.h>

#include <cstdio>
#include <iterator>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/listIcons.h"
#include "components/icons/menuIcons.h"
#include "components/icons/readerToolbarIcons.h"
#include "components/icons/search24.h"

namespace fui = freeink::ui;

namespace {
constexpr StrId ORIENTATION_LABELS[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                        StrId::STR_LANDSCAPE_CCW};
// Pages per minute; index 0 is Off. Mirrors PAGE_TURN_RATES in EpubReaderActivity.
constexpr const char* PAGE_TURN_RATES[] = {nullptr, "1", "3", "6", "12"};
}  // namespace

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, Position position, std::string textValue,
                                               const uint8_t currentOrientation, const uint8_t pageTurnOption,
                                               const MenuContext& context)
    : UiListActivity("EpubReaderMenu", renderer, mappedInput),
      context(context),
      title(title),
      position(std::move(position)),
      textValue(std::move(textValue)),
      pendingOrientation(currentOrientation),
      selectedPageTurnOption(pageTurnOption < std::size(PAGE_TURN_RATES) ? pageTurnOption : 0) {
  loadPage(MenuPage::Main);
}

void EpubReaderMenuActivity::buildMenuItems(std::vector<MenuItem>& items, const MenuPage page,
                                            const MenuContext& context) {
  items.clear();
  items.reserve(MAX_MENU_ITEMS);
  switch (page) {
    case MenuPage::Main:
      // Finding your way through the book.
      items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_TOOL_CONTENTS});
      items.push_back({MenuAction::SEARCH, StrId::STR_SEARCH});
      items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_POSITION});
      if (context.hasFootnotes) items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
      // Reading the page.
      items.push_back({MenuAction::TEXT_SETTINGS, StrId::STR_TOOL_TEXT, true});
      items.push_back({MenuAction::DICTIONARY, StrId::STR_LOOKUP});
      if (Frontlight.present()) items.push_back({MenuAction::FRONTLIGHT, StrId::STR_LIGHT});
      // Marking it.
      items.push_back({MenuAction::TOGGLE_BOOKMARK, StrId::STR_BOOKMARK_THIS_PAGE, true});
      if (context.bookmarkCount > 0) items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});
      items.push_back({MenuAction::MORE_OPTIONS, StrId::STR_MORE_OPTIONS, true});
      break;
    case MenuPage::More:
      items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
      items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_PAGE_TURN});
      items.push_back({MenuAction::NIGHT_MODE, StrId::STR_NIGHT_MODE});
      if (context.canSync) items.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS, true});
      items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR, !context.canSync});
      items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
      items.push_back({MenuAction::GO_HOME, StrId::STR_CLOSE_BOOK, true});
      items.push_back({MenuAction::TROUBLESHOOTING, StrId::STR_TROUBLESHOOTING});
      break;
    case MenuPage::Troubleshooting:
      items.push_back({MenuAction::REFRESH_SCREEN, StrId::STR_FORCE_REFRESH});
      items.push_back({MenuAction::DELETE_CACHE, StrId::STR_REBUILD_BOOK});
      break;
  }
}

StrId EpubReaderMenuActivity::labelFor(const MenuItem& item, const MenuContext& context) {
  if (item.action == MenuAction::TOGGLE_BOOKMARK && context.pageBookmarked) return StrId::STR_REMOVE_BOOKMARK;
  return item.labelId;
}

fui::BitmapRef EpubReaderMenuActivity::iconFor(const MenuAction action, const MenuContext& context) {
  switch (action) {
    case MenuAction::SELECT_CHAPTER:
      return fui::bitmapFromIcon(icon_reader_contents_24);
    case MenuAction::SEARCH:
      return fui::bitmapFromIcon(Search24Icon);
    case MenuAction::GO_TO_PERCENT:
      return fui::bitmapFromIcon(icon_menu_position_24);
    case MenuAction::FOOTNOTES:
      return fui::bitmapFromIcon(icon_menu_footnotes_24);
    case MenuAction::TEXT_SETTINGS:
      return fui::bitmapFromIcon(icon_reader_text_24);
    case MenuAction::DICTIONARY:
      return fui::bitmapFromIcon(icon_menu_lookup_24);
    case MenuAction::FRONTLIGHT:
      return fui::bitmapFromIcon(icon_sun_24);
    case MenuAction::TOGGLE_BOOKMARK:
      return fui::bitmapFromIcon(context.pageBookmarked ? icon_menu_bookmark_remove_24 : icon_menu_bookmark_add_24);
    case MenuAction::BOOKMARKS:
      return fui::bitmapFromIcon(icon_bookmark_24);
    case MenuAction::MORE_OPTIONS:
      return fui::bitmapFromIcon(icon_reader_more_24);
    case MenuAction::ROTATE_SCREEN:
      return fui::bitmapFromIcon(icon_menu_orientation_24);
    case MenuAction::AUTO_PAGE_TURN:
      return fui::bitmapFromIcon(icon_menu_auto_turn_24);
    case MenuAction::NIGHT_MODE:
      return fui::bitmapFromIcon(icon_moon_24);
    case MenuAction::SYNC:
      return fui::bitmapFromIcon(icon_refresh_cw_24);
    case MenuAction::DISPLAY_QR:
      return fui::bitmapFromIcon(icon_menu_qr_24);
    case MenuAction::SCREENSHOT:
      return fui::bitmapFromIcon(icon_menu_screenshot_24);
    case MenuAction::GO_HOME:
      return fui::bitmapFromIcon(icon_menu_close_book_24);
    case MenuAction::TROUBLESHOOTING:
      return fui::bitmapFromIcon(icon_menu_troubleshoot_24);
    case MenuAction::REFRESH_SCREEN:
      return fui::bitmapFromIcon(icon_menu_refresh_screen_24);
    case MenuAction::DELETE_CACHE:
      return fui::bitmapFromIcon(icon_menu_rebuild_24);
  }
  return {};
}

const char* EpubReaderMenuActivity::descriptionFor(const MenuAction action) {
  switch (action) {
    case MenuAction::REFRESH_SCREEN:
      return tr(STR_REFRESH_SCREEN_DESC);
    case MenuAction::DELETE_CACHE:
      return tr(STR_REBUILD_BOOK_DESC);
    default:
      return nullptr;
  }
}

StrId EpubReaderMenuActivity::pageTitle(const MenuPage page) {
  return page == MenuPage::Troubleshooting ? StrId::STR_TROUBLESHOOTING : StrId::STR_MORE_OPTIONS;
}

// Rebuilds the rows for a page: labels, icons and descriptions are fixed while
// the page is up; buildScreen() refreshes only the values.
void EpubReaderMenuActivity::loadPage(const MenuPage next) {
  page = next;
  buildMenuItems(menuItems, page, context);
  for (size_t i = 0; i < menuItems.size() && i < MAX_MENU_ITEMS; i++) {
    fui::ListItem item;
    item.label = I18N.get(labelFor(menuItems[i], context));
    item.subtitle = descriptionFor(menuItems[i].action);
    item.icon = iconFor(menuItems[i].action, context);
    item.toggle = menuItems[i].action == MenuAction::NIGHT_MODE;
    // A blank heading is the group gap: list() draws no text for it, only the
    // section spacing set in buildScreen().
    item.sectionHeading = menuItems[i].startsGroup ? " " : nullptr;
    item.actionValue = static_cast<int16_t>(i);
    menuRowItems[i] = item;
  }
}

void EpubReaderMenuActivity::showPage(const MenuPage next) {
  loadPage(next);
  nav.reset();
  requestUpdate();
}

void EpubReaderMenuActivity::refreshRowValue(const size_t row) {
  char* value = rowValues[row];
  value[0] = '\0';
  switch (menuItems[row].action) {
    case MenuAction::GO_TO_PERCENT:
      snprintf(value, VALUE_LEN, "%d%%", position.bookPercent);
      break;
    case MenuAction::FOOTNOTES:
      if (context.footnoteCount > 1) snprintf(value, VALUE_LEN, "%d", context.footnoteCount);
      break;
    case MenuAction::TEXT_SETTINGS:
      snprintf(value, VALUE_LEN, "%s", textValue.c_str());
      break;
    case MenuAction::FRONTLIGHT:
      if (Frontlight.isOn()) {
        snprintf(value, VALUE_LEN, "%u%%", static_cast<unsigned>(Frontlight.brightness()));
      } else {
        snprintf(value, VALUE_LEN, "%s", tr(STR_STATE_OFF));
      }
      break;
    case MenuAction::BOOKMARKS:
      snprintf(value, VALUE_LEN, "%d", context.bookmarkCount);
      break;
    case MenuAction::ROTATE_SCREEN:
      snprintf(value, VALUE_LEN, "%s",
               I18N.get(ORIENTATION_LABELS[pendingOrientation % std::size(ORIENTATION_LABELS)]));
      break;
    case MenuAction::AUTO_PAGE_TURN:
      if (selectedPageTurnOption == 0) {
        snprintf(value, VALUE_LEN, "%s", tr(STR_STATE_OFF));
      } else {
        snprintf(value, VALUE_LEN, tr(STR_PAGES_PER_MINUTE_VALUE), PAGE_TURN_RATES[selectedPageTurnOption]);
      }
      break;
    case MenuAction::NIGHT_MODE:
      menuRowItems[row].toggleChecked = SETTINGS.screenInverted != 0;
      break;
    default:
      break;
  }
  menuRowItems[row].value = value[0] != '\0' ? value : nullptr;
}

void EpubReaderMenuActivity::closeCancelled() {
  ActivityResult result;
  result.isCancelled = true;
  result.data = MenuResult{-1, pendingOrientation, selectedPageTurnOption};
  setResult(std::move(result));
  finish();
}

bool EpubReaderMenuActivity::handleHomeGesture() {
  closeCancelled();
  return true;
}

void EpubReaderMenuActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  // The activated row leaves this screen (popup or finish); a lingering flash
  // would gray an unrelated element on the next render.
  app.clearTapFlash();
  nav.selected = index;

  const auto selectedAction = menuItems[index].action;
  switch (selectedAction) {
    case MenuAction::MORE_OPTIONS:
      showPage(MenuPage::More);
      return;
    case MenuAction::TROUBLESHOOTING:
      showPage(MenuPage::Troubleshooting);
      return;
    case MenuAction::ROTATE_SCREEN:
      optionPopup.show(StrId::STR_ORIENTATION, ORIENTATION_LABELS, static_cast<int>(std::size(ORIENTATION_LABELS)),
                       pendingOrientation, [this](int idx) {
                         pendingOrientation = idx;
                         // Rotate the menu immediately. Only the renderer turns;
                         // SETTINGS.orientation stays unchanged so the reader's
                         // result handler still detects the change and reflows.
                         ReaderUtils::applyOrientation(renderer, pendingOrientation);
                         app.setDevice(uiTarget.deviceContext());  // hit rects follow the new frame
                         requestUpdate(true);
                       });
      requestUpdate();
      return;
    case MenuAction::AUTO_PAGE_TURN: {
      const char* labels[std::size(PAGE_TURN_RATES)];
      labels[0] = tr(STR_STATE_OFF);
      for (size_t i = 1; i < std::size(PAGE_TURN_RATES); i++) labels[i] = PAGE_TURN_RATES[i];
      optionPopup.show(I18N.get(StrId::STR_AUTO_TURN_PAGES_PER_MIN), labels, static_cast<int>(std::size(labels)),
                       selectedPageTurnOption, [this](int idx) {
                         selectedPageTurnOption = idx;
                         requestUpdate();
                       });
      requestUpdate();
      return;
    }
    case MenuAction::NIGHT_MODE:
      SETTINGS.screenInverted = SETTINGS.screenInverted == 0 ? 1 : 0;
      SETTINGS.saveToFile();
      requestUpdate();
      return;
    default:
      break;
  }

  setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, selectedPageTurnOption});
  finish();
}

bool EpubReaderMenuActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

bool EpubReaderMenuActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    closeCancelled();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateIndex(nav.selected);
    return true;
  }

  return false;
}

void EpubReaderMenuActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band GUI.drawHeader paints.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});

  const auto& theme = screen.theme();
  const int16_t pad = theme.headerSidePadding;
  screen.spacer(theme.spaceLg);
  if (page == MenuPage::Main) {
    // Where you are: the chapter, then the page and the book position, on the
    // header's text column.
    if (!position.chapterTitle.empty()) {
      fui::TextStyle chapterStyle = theme.smallText;
      chapterStyle.bold = true;
      const fui::Rect line = screen.takeTop(screen.target().lineHeight(chapterStyle.font));
      screen.target().text(line.inset(fui::Insets{0, pad, 0, pad}), position.chapterTitle.c_str(), chapterStyle);
    }
    char where[64];
    int len = 0;
    if (position.totalPages > 0) {
      len = snprintf(where, sizeof(where), tr(STR_MENU_PAGE_OF), position.currentPage, position.totalPages);
      if (len < 0 || len >= static_cast<int>(sizeof(where))) len = 0;
      len += snprintf(where + len, sizeof(where) - len, "  \xC2\xB7  ");
    }
    snprintf(where + len, sizeof(where) - len, tr(STR_MENU_BOOK_PERCENT), position.bookPercent);
    const fui::Rect line = screen.takeTop(screen.target().lineHeight(theme.smallText.font));
    screen.target().text(line.inset(fui::Insets{0, pad, 0, pad}), where, theme.smallText);
    screen.spacer(theme.spaceLg);
  }

  for (size_t i = 0; i < menuItems.size() && i < MAX_MENU_ITEMS; i++) refreshRowValue(i);

  fui::ListProps props;
  props.items = menuRowItems;
  props.count = static_cast<uint16_t>(menuItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  applyListControlStyle(props, theme);
  // Group gaps: blank one-pixel headings plus the section spacing.
  props.headerRowHeight = 1;
  props.sectionGap = static_cast<int16_t>(theme.spaceMd + theme.spaceXs);
  if (page == MenuPage::Troubleshooting) {
    // Two-line rows (name + what it does) get the air a single line has.
    props.rowPaddingY = static_cast<int16_t>(theme.spaceMd + theme.spaceXs);
  }
  syncListViewport(screen, props);
  screen.list(props);
}

void EpubReaderMenuActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 page == MenuPage::Main ? title.c_str() : I18N.get(pageTitle(page)));
}

// Back names where it goes: the page, from every level of the menu.
void EpubReaderMenuActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK_TO_BOOK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();
  drawChrome();

  renderUi();

  drawFooter();
  renderer.displayBuffer();
}
