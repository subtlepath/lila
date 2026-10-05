#include "LibraryListActivity.h"

#include <Epub.h>
#include <FreeInkUIIcon.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryText.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/library/LibraryMenuActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/themes/lyra/LyraTheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/BookProgress.h"

namespace fui = freeink::ui;

namespace {
constexpr unsigned long LONG_PRESS_MS = 1000;

constexpr int RECENT_TAB = 0;
constexpr int TITLE_TAB = 1;
constexpr int AUTHOR_TAB = 2;
constexpr int TAB_SLOTS = AUTHOR_TAB + 1;

// The search keyboard's input limit.
constexpr size_t QUERY_MAX = 48;

constexpr bool isDescending(const library::SortOrder order) {
  return order == library::SortOrder::RecentDesc || order == library::SortOrder::TitleDesc ||
         order == library::SortOrder::AuthorDesc;
}

constexpr bool isRecentSort(const library::SortOrder order) {
  return order == library::SortOrder::RecentAsc || order == library::SortOrder::RecentDesc;
}

constexpr bool isAuthorSort(const library::SortOrder order) {
  return order == library::SortOrder::AuthorAsc || order == library::SortOrder::AuthorDesc;
}

constexpr library::SortOrder orderForTab(const int tab, const uint8_t descendingTabs) {
  const bool descending = (descendingTabs & (1u << tab)) != 0;
  if (tab == TITLE_TAB) return descending ? library::SortOrder::TitleDesc : library::SortOrder::TitleAsc;
  if (tab == AUTHOR_TAB) return descending ? library::SortOrder::AuthorDesc : library::SortOrder::AuthorAsc;
  return descending ? library::SortOrder::RecentDesc : library::SortOrder::RecentAsc;
}

const char* tabLabelFor(const int tab) {
  if (tab == TITLE_TAB) return tr(STR_LIBRARY_TAB_TITLE);
  if (tab == AUTHOR_TAB) return tr(STR_LIBRARY_TAB_AUTHOR);
  return tr(STR_LIBRARY_TAB_RECENT);
}

// Home sets its type a step under the theme's: the body slot draws names in
// the 20px strike and the small slot details in the 17px one. The slots are
// rebound only while Home builds its screen (FreeInkUI draws as it builds), so
// the shared theme tokens keep deriving from the theme's own body font.
class HomeTypeScale {
 public:
  explicit HomeTypeScale(fui::GfxRendererTarget& target) : target(target) {
    target.setFont(fui::GfxRendererTarget::FONT_BODY, UI_10_FONT_ID);
    target.setFont(fui::GfxRendererTarget::FONT_SMALL, SMALL_FONT_ID);
  }
  ~HomeTypeScale() {
    const auto spec = uiScaleSpec();
    target.setFont(fui::GfxRendererTarget::FONT_BODY, spec.bodyFontId);
    target.setFont(fui::GfxRendererTarget::FONT_SMALL, spec.smallFontId);
  }
  HomeTypeScale(const HomeTypeScale&) = delete;
  HomeTypeScale& operator=(const HomeTypeScale&) = delete;

 private:
  fui::GfxRendererTarget& target;
};

// Home's chrome follows Tinta's main menu: a status band on the top edge, and
// below the shelf either the key hints in a 44px band of small type or, on
// touch boards, a 64px bar of tappable choices.
constexpr int16_t HOME_BAND_HEIGHT = LibraryListActivity::STATUS_BAND_HEIGHT;
constexpr int16_t HOME_HINTS_HEIGHT = 44;
constexpr int16_t HOME_CHOICE_HEIGHT = 64;
// The footer's content starts below its top rule's 2px, as in Tinta.
constexpr int16_t FOOTER_CONTENT_TOP = 2;
constexpr int16_t STATUS_GAP = 14;  // between the clock, the percent cluster and the title
constexpr int16_t PERCENT_GAP = 6;
// Tinta's battery: a 2px outline, a 3x6 nub and the charge inset 3px.
constexpr int16_t BATTERY_W = 26;
constexpr int16_t BATTERY_H = 13;
constexpr int16_t BATTERY_NUB_W = 3;

fui::TextStyle homeTextStyle(const fui::FontId font, const bool bold,
                             const fui::TextAlign align = fui::TextAlign::Left) {
  fui::TextStyle style;
  style.font = font;
  style.bold = bold;
  style.align = align;
  return style;
}

// Line-box top that centres `style`'s capitals in [top, top + height).
int16_t capsCentredLineTop(const fui::DrawTarget& target, const fui::TextStyle& style, const int16_t top,
                           const int16_t height) {
  const fui::Rect caps = target.inkBounds(style.font, "H", style);
  return static_cast<int16_t>(top + (height - caps.height) / 2 - caps.y);
}

// Tinta's status line: clock, battery percent and battery, ending at the
// band's right edge, the texts on the title's baseline.
class HomeStatus {
 public:
  HomeStatus() {
    const uint16_t level = powerManager.getBatteryPercentage();
    percent = static_cast<uint8_t>(level > 100 ? 100 : level);
    charging = gpio.isUsbConnected();
    if (SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS) {
      snprintf(percentText, sizeof(percentText), "%u%%", static_cast<unsigned>(percent));
    }
    if (UITheme::getInstance().getMetrics().headerShowsClock && SETTINGS.clockShowInHeader && halClock.isAvailable() &&
        !halClock.formatTime(clockText, sizeof(clockText), SETTINGS.clockFormat == 1)) {
      clockText[0] = '\0';
    }
  }

  int16_t width(const fui::DrawTarget& target) const {
    int width = BATTERY_W + BATTERY_NUB_W;
    if (percentText[0]) width += PERCENT_GAP + target.measureText(PERCENT_FONT, percentText, percentStyle()).width;
    if (clockText[0]) width += STATUS_GAP + target.measureText(CLOCK_FONT, clockText, clockStyle()).width;
    return static_cast<int16_t>(width);
  }

  void draw(fui::DrawTarget& target, const GfxRenderer& renderer, const int16_t right, const int16_t bandTop,
            const int16_t bandHeight, const int16_t baseline) const {
    const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
    const auto cy = static_cast<int16_t>(bandTop + bandHeight / 2);
    const fui::Rect body{static_cast<int16_t>(right - BATTERY_NUB_W - BATTERY_W),
                         static_cast<int16_t>(cy - BATTERY_H / 2), BATTERY_W, BATTERY_H};
    target.stroke(body, ink, 2);
    target.fill(fui::Rect{body.right(), static_cast<int16_t>(cy - 3), BATTERY_NUB_W, 6}, ink);
    if (charging) {
      target.fill(fui::Rect{static_cast<int16_t>(body.x + 2), static_cast<int16_t>(body.y + 2),
                            static_cast<int16_t>(BATTERY_W - 4), static_cast<int16_t>(BATTERY_H - 4)},
                  ink);
      BaseTheme::drawBatteryLightningBolt(renderer, body.x + (BATTERY_W - 6) / 2, body.y + 2);
    } else {
      const int inner = BATTERY_W - 6;
      const auto fill = static_cast<int16_t>((inner * percent + 50) / 100);
      if (fill > 0) {
        target.fill(fui::Rect{static_cast<int16_t>(body.x + 3), static_cast<int16_t>(body.y + 3), fill,
                              static_cast<int16_t>(BATTERY_H - 6)},
                    ink);
      }
    }

    int16_t x = body.x;
    if (percentText[0])
      x = drawText(target, static_cast<int16_t>(x - PERCENT_GAP), baseline, PERCENT_FONT, percentText, percentStyle());
    if (clockText[0])
      drawText(target, static_cast<int16_t>(x - STATUS_GAP), baseline, CLOCK_FONT, clockText, clockStyle());
  }

 private:
  // Home binds the body slot to the 20px strike (HomeTypeScale); the label
  // slot is the fixed 17px status font.
  static constexpr fui::FontId PERCENT_FONT = fui::GfxRendererTarget::FONT_LABEL;
  static constexpr fui::FontId CLOCK_FONT = fui::GfxRendererTarget::FONT_BODY;
  static fui::TextStyle percentStyle() { return homeTextStyle(PERCENT_FONT, false); }
  static fui::TextStyle clockStyle() { return homeTextStyle(CLOCK_FONT, true); }

  // Draws `text` ending at `right` on `baseline`; returns its left edge.
  static int16_t drawText(fui::DrawTarget& target, const int16_t right, const int16_t baseline, const fui::FontId font,
                          const char* text, const fui::TextStyle& style) {
    const int16_t w = target.measureText(font, text, style).width;
    const fui::Rect caps = target.inkBounds(font, "H", style);
    const auto left = static_cast<int16_t>(right - w);
    target.text(fui::Rect{left, static_cast<int16_t>(baseline - caps.y - caps.height), w, target.lineHeight(font)},
                text, style);
    return left;
  }

  uint8_t percent = 0;
  bool charging = false;
  char percentText[8] = {};
  char clockText[10] = {};
};

std::string fileNameOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The shelf as the reader left it. RAM only: a cold boot opens on the current
// book, which is what waking should offer anyway.
struct ShelfMemory {
  bool valid = false;
  bool heroFocused = true;
  uint8_t activeTab = RECENT_TAB;
  uint8_t descendingTabs = 1u << RECENT_TAB;
  int selected = 0;
  int top = 0;
  char query[QUERY_MAX + 1] = {};
};
ShelfMemory shelfMemory;

}  // namespace

LibraryListActivity::LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         const bool cleanInitialRefresh)
    : UiTabListActivity("Library", renderer, mappedInput, true),
      heroCover(renderer),
      cleanInitialRefresh(cleanInitialRefresh) {}

void LibraryListActivity::onEnter() {
  // One lock across the base lifecycle AND the data phase: the base onEnter
  // schedules a paint, and the render task must not read the index or the
  // filter before they are in place. The rebuild also needs the lock: the
  // render task's SD-loaded fonts read glyph data at draw time, and the walk
  // needs the card to itself.
  RenderLock lock(*this);
  UiTabListActivity::onEnter();
  app.on(ACTION_SEARCH, &LibraryListActivity::searchActionTrampoline, this);
  app.on(ACTION_MENU, &LibraryListActivity::menuActionTrampoline, this);
  app.on(ACTION_HERO, &LibraryListActivity::heroActionTrampoline, this);
  app.on(ACTION_SORT, &LibraryListActivity::sortActionTrampoline, this);

  // Recent is backed by the resident store. Prune before opening the index so
  // its persistence write never overlaps the long-lived index reader.
  if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();

  // Rebuild when the index is missing, invalid, or was built with the other
  // metadata mode. Otherwise entering the screen stays instant.
  const bool readMetadata = SETTINGS.libraryUseMetadata != 0;
  const bool rebuildNeeded = library::isLibraryIndexDirty() || !index.open(library::libraryIndexPath()) ||
                             index.header().metadataEnabled != readMetadata;
  if (rebuildNeeded) {
    index.close();
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    rebuildIndex();
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
  }
  degraded = index.isOpen() && index.ranksDegraded();
  if (index.isOpen() && index.dedupDegraded()) {
    LOG_ERR("LIB", "index was built without duplicate detection");
  }
  resolvePinned();

  // Boards with PSRAM keep the decoded hero cover between repaints.
  if (HalMemory::getPsramHeap().totalBytes > 0) heroCover.begin();
  loadHero();
  restoreShelf();

  // Entered while a button was still held (leaving the reader, or a sub-screen):
  // ignore its release, or it would act on this screen.
  swallowHeldReleases();
  requestUpdate(true);
}

void LibraryListActivity::onExit() {
  saveShelf();
  index.close();
  Activity::onExit();
}

bool LibraryListActivity::rebuildIndex() {
  library::BuildStats stats;
  const bool ok = library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0);
  if (!ok) {
    LOG_ERR("LIB", "index build failed");
    return false;
  }
  LOG_INF("LIB", "reconciled: %u unchanged, %u added, %u renamed, %u removed, %u enriched (%u dup, %u unreadable)",
          static_cast<unsigned>(stats.unchanged), static_cast<unsigned>(stats.added),
          static_cast<unsigned>(stats.renamed), static_cast<unsigned>(stats.removed),
          static_cast<unsigned>(stats.enriched), static_cast<unsigned>(stats.duplicatesDropped),
          static_cast<unsigned>(stats.unreadableSkipped));
  if (stats.dedupDegraded) LOG_ERR("LIB", "rebuild completed without duplicate detection");
  return true;
}

void LibraryListActivity::swallowHeldReleases() {
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  lockNextBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
}

int LibraryListActivity::selectedEntry() const {
  const int entry = ringPos() - 1;
  return entry < 0 ? 0 : entry;
}

// --- hero --------------------------------------------------------------------

void LibraryListActivity::loadHero() {
  heroPath.clear();
  heroTitle.clear();
  heroAuthor.clear();
  heroCoverPath.clear();
  heroCoverReady = false;
  heroCoverPending = false;
  heroProgress = -1;
  heroProgressText[0] = '\0';
  heroCover.invalidate();
  if (!hasHero()) return;

  const RecentBook& book = RECENT_BOOKS.getBooks().front();
  heroPath = book.path;
  heroTitle = book.title.empty() ? fileNameOf(book.path) : book.title;
  heroAuthor = book.author;
  // Thumbs are generated at the drawn height (a rescaled dither aliases):
  // about a sixth of the screen's long side, 136px on the X4.
  const int longSide = std::max(renderer.getScreenWidth(), renderer.getScreenHeight());
  heroCoverHeight = std::clamp(longSide * 17 / 100, 96, 200);
  // An empty cover path means the book has no usable cover; the hero then
  // gives the text the whole width rather than showing a placeholder.
  if (!book.coverBmpPath.empty()) {
    heroCoverPath = UITheme::getCoverThumbPath(book.coverBmpPath, heroCoverHeight);
    heroCoverReady = Storage.exists(heroCoverPath.c_str());
    heroCoverPending = !heroCoverReady;
  }
  // Read from the saved position rather than trusted from the store: the
  // reader writes it on teardown, after anything it could report on exit.
  // Only the current book moves between visits, so recording it here keeps
  // every row's percent current.
  heroProgress = loadBookProgress(book.path);
  if (heroProgress < 0) {
    heroProgress = book.progress;
  } else if (heroProgress != book.progress) {
    RECENT_BOOKS.setProgress(book.path, heroProgress);
  }
  if (heroProgress >= 100) {
    snprintf(heroProgressText, sizeof(heroProgressText), "%s", tr(STR_BOOK_FINISHED));
  } else if (heroProgress >= 0) {
    snprintf(heroProgressText, sizeof(heroProgressText), tr(STR_LIBRARY_PERCENT_READ), heroProgress);
  }
}

bool LibraryListActivity::generateHeroCover() {
  // One parser at a time; EPUB/XTC objects exceed the stack budget.
  if (FsHelpers::hasEpubExtension(heroPath)) {
    auto epub = makeUniqueNoThrow<Epub>(heroPath, "/.crosspoint");
    if (!epub) {
      LOG_ERR("LIB", "OOM: hero cover EPUB");
      return false;
    }
    if (!epub->generateThumbBmpFromSource(heroCoverHeight)) return false;
  } else if (FsHelpers::hasXtcExtension(heroPath)) {
    auto xtc = makeUniqueNoThrow<Xtc>(heroPath, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("LIB", "OOM: hero cover XTC");
      return false;
    }
    if (!xtc->load() || !xtc->generateThumbBmp(heroCoverHeight)) return false;
  } else {
    return false;
  }
  return Storage.exists(heroCoverPath.c_str());
}

void LibraryListActivity::focusHero() {
  heroFocused = true;
  // Keep the list's viewport: only the focus moves to the top.
  activeNav().selected = 0;
  requestUpdate();
}

void LibraryListActivity::openHero() {
  if (!hasHero()) return;
  focusHeroOnReturn = true;
  openBookByPath(heroPath);
}

void LibraryListActivity::buildHero(UiScreen& screen) {
  const auto& theme = screen.theme();
  const auto& metrics = UITheme::getInstance().getMetrics();
  auto& target = screen.target();
  const auto coverH = static_cast<int16_t>(heroCoverHeight);
  const auto coverW = static_cast<int16_t>(heroCoverReady ? heroCoverHeight * 2 / 3 : 0);
  const auto column = static_cast<int16_t>(metrics.headerSidePadding);
  const auto coverGap = static_cast<int16_t>(theme.spaceLg + theme.spaceSm);

  fui::TextStyle titleStyle = theme.bodyText;
  titleStyle.bold = true;
  titleStyle.maxLines = 2;
  const fui::TextStyle& smallStyle = theme.smallText;
  const int16_t smallH = target.lineHeight(smallStyle.font);

  // Title, author and progress centre on the cover, or stand alone on the
  // text column when the book has none.
  const fui::Rect content = screen.body();
  const auto textX = static_cast<int16_t>(content.x + column + (coverW > 0 ? coverW + coverGap : 0));
  const auto textW = static_cast<int16_t>(content.right() - column - textX);
  const int16_t titleH = fui::measureWrappedText(target, heroTitle.c_str(), titleStyle, textW).height;
  const bool hasAuthor = !heroAuthor.empty();
  const bool hasProgress = heroProgressText[0] != '\0';
  const auto blockH =
      static_cast<int16_t>(titleH + (hasAuthor ? smallH : 0) + (hasProgress ? theme.spaceMd + smallH : 0));

  screen.spacer(theme.spaceLg);
  const fui::Rect band = screen.takeTop(std::max<int16_t>(coverW > 0 ? coverH : 0, blockH), theme.spaceLg);
  if (coverW > 0) {
    heroCover.prepare();
    heroCover.paint(fui::Rect{static_cast<int16_t>(band.x + column), band.y, coverW, coverH}, 0, heroCoverPath);
  }
  auto y = static_cast<int16_t>(band.y + (band.height - blockH) / 2);
  target.text(fui::Rect{textX, y, textW, titleH}, heroTitle.c_str(), titleStyle);
  y = static_cast<int16_t>(y + titleH);
  if (hasAuthor) {
    target.text(fui::Rect{textX, y, textW, smallH}, heroAuthor.c_str(), smallStyle);
    y = static_cast<int16_t>(y + smallH);
  }
  if (hasProgress) {
    target.text(fui::Rect{textX, static_cast<int16_t>(y + theme.spaceMd), textW, smallH}, heroProgressText, smallStyle);
  }
  screen.frame().hit(band, ACTION_HERO, 0, fui::InputTouch | fui::InputLongPress);

  // On touch boards nothing holds focus, so Continue Reading stays the solid
  // primary button.
  buildBarRow(screen, ACTION_HERO, heroFocused || mappedInput.hasTouch(), tr(STR_CONTINUE_READING), nullptr);
}

void LibraryListActivity::buildSortRow(UiScreen& screen) {
  snprintf(sortValueText, sizeof(sortValueText), "%s %s", tabLabelFor(activeTabIndex),
           isDescending(sortOrder) ? "\xE2\x86\x93" : "\xE2\x86\x91");
  buildBarRow(screen, ACTION_SORT, tabsFocused() && !mappedInput.hasTouch(), tr(STR_LIBRARY_SORT_BY), sortValueText);
}

void LibraryListActivity::buildBarRow(UiScreen& screen, const fui::ActionId action, const bool focused,
                                      const char* label, const char* value) {
  const auto& theme = screen.theme();
  const auto& metrics = UITheme::getInstance().getMetrics();
  auto& target = screen.target();
  const auto rowH = static_cast<int16_t>(std::max<int>(metrics.listRowHeight, theme.minTouchSize));
  const fui::Rect row = screen.takeTop(rowH);
  const auto inset = static_cast<int16_t>(metrics.listInset);
  const fui::Rect bar{static_cast<int16_t>(row.x + inset), row.y, static_cast<int16_t>(row.width - 2 * inset),
                      row.height};
  screen.frame().hit(bar, action, 0, fui::InputTouch | fui::InputLongPress);
  // A tap flashes the bar like any list row.
  const fui::State state = screen.frame().stateFor(action, 0, focused ? fui::StateSelected : fui::StateNormal);
  const bool inverted = fui::hasState(state, fui::StateSelected) || fui::hasState(state, fui::StateActive);
  if (inverted) target.fill(bar, fui::Paint::solid(fui::Color::Black));

  const auto column = static_cast<int16_t>(metrics.headerSidePadding);
  const fui::Rect text{static_cast<int16_t>(row.x + column), row.y, static_cast<int16_t>(row.width - 2 * column),
                       row.height};
  const fui::Color ink = inverted ? fui::Color::White : fui::Color::Black;
  fui::TextStyle labelStyle = theme.bodyText;
  labelStyle.color = ink;
  fui::TextStyle valueStyle = theme.smallText;
  valueStyle.color = ink;
  valueStyle.align = fui::TextAlign::Right;
  int16_t labelW = text.width;
  if (value) {
    const int16_t valueW = target.measureText(valueStyle.font, value, valueStyle).width;
    target.text(text, value, valueStyle);
    labelW = static_cast<int16_t>(labelW - valueW - theme.spaceMd);
  }
  target.text(fui::Rect{text.x, text.y, labelW, text.height}, label, labelStyle);
}

// --- shelf memory -------------------------------------------------------------

void LibraryListActivity::saveShelf() {
  shelfMemory.valid = true;
  shelfMemory.heroFocused = heroFocused || focusHeroOnReturn;
  shelfMemory.activeTab = static_cast<uint8_t>(activeTabIndex);
  shelfMemory.descendingTabs = descendingTabs;
  // A book opened from the pinned rows or a search becomes the hero, so the
  // shelf comes back on it: the pinned order has changed under the old row,
  // and the search has done its job.
  const fui::ListNav& nav = groupsCollapsed ? expandedNav : activeNav();
  shelfMemory.selected = focusHeroOnReturn ? 0 : nav.selected.load();
  shelfMemory.top = focusHeroOnReturn ? 0 : nav.top;
  snprintf(shelfMemory.query, sizeof(shelfMemory.query), "%s", focusHeroOnReturn ? "" : query.c_str());
}

void LibraryListActivity::restoreShelf() {
  heroFocused = heroShown();
  if (!shelfMemory.valid) return;
  activeTabIndex = shelfMemory.activeTab < TAB_SLOTS ? shelfMemory.activeTab : RECENT_TAB;
  descendingTabs = shelfMemory.descendingTabs;
  sortOrder = orderForTab(activeTabIndex, descendingTabs);
  query = shelfMemory.query;
  applyFilter();
  refreshOverlap();
  heroFocused = shelfMemory.heroFocused && heroShown();

  auto& nav = activeNav();
  const int count = listCount();
  const int selected = heroFocused ? 0 : std::clamp(shelfMemory.selected, 0, count);
  nav.reset(selected);
  nav.top = std::clamp(shelfMemory.top, 0, std::max(0, count - 1));
  // Focus at the top keeps the remembered scroll; a row keeps it while the row
  // is still in view.
  nav.followOnBuild = selected > 0;
}

// --- pinned overlay ------------------------------------------------------------

// The pinned overlay applies only to the shelf that reads as "what am I up
// to": the unfiltered Recent sort, newest first. A search result is a flat
// list the reader narrowed down on purpose, and the ascending toggle asks for
// oldest-first, which pinned fresh reads would contradict.
bool LibraryListActivity::overlayActive() const {
  return activeTabIndex == RECENT_TAB && query.empty() && isDescending(sortOrder);
}

int LibraryListActivity::pinnedCount() const {
  if (!overlayActive()) return 0;
  return std::max(0, static_cast<int>(pinnedTotal) - (hasHero() ? 1 : 0));
}

void LibraryListActivity::resolvePinned() {
  const auto& books = RECENT_BOOKS.getBooks();
  pinnedTotal = static_cast<uint8_t>(std::min<size_t>(books.size(), RecentBooksStore::MAX_RECENT_BOOKS));
  for (int i = 0; i < pinnedTotal; i++) {
    pinnedAscRows[i] = 0xFFFF;
    pinnedOrdinals[i] = 0xFFFF;
  }
  if (pinnedTotal > 0 && index.isOpen()) {
    library::BookIdentity identities[RecentBooksStore::MAX_RECENT_BOOKS];
    for (int i = 0; i < pinnedTotal; i++) {
      const std::string& path = books[static_cast<size_t>(i)].path;
      identities[i].pathHash = library::clixPathHash(path.data(), path.size());
      // Size is only a lookup prefilter; 0 (stat failed, e.g. the index handle
      // is the card's one open reader) falls back to hash-only matching.
      identities[i].fileSize = 0;
      HalFile file;
      if (Storage.openFileForRead("LIB", path.c_str(), file)) {
        identities[i].fileSize = static_cast<uint32_t>(file.fileSize());
      }
    }
    if (!index.recentRowsFor(identities, pinnedTotal, pinnedAscRows)) {
      // Without the match the overlay would duplicate every pinned book that is
      // also in the index; better to drop the pins than to show doubles.
      LOG_ERR("LIB", "recent-book lookup failed; overlay disabled");
      pinnedTotal = 0;
    }
    // Ordinals let any sort's rows find their recorded progress.
    for (int i = 0; i < pinnedTotal; i++) {
      if (pinnedAscRows[i] != 0xFFFF) {
        pinnedOrdinals[i] = index.ordinalForRow(library::SortOrder::RecentAsc, pinnedAscRows[i]);
      }
    }
  }
  refreshOverlap();
}

void LibraryListActivity::refreshOverlap() {
  overlapCount = 0;
  const int total = static_cast<int>(index.bookCount());
  for (int i = 0; i < pinnedTotal; i++) {
    if (pinnedAscRows[i] == 0xFFFF || pinnedAscRows[i] >= total) continue;
    const uint16_t row =
        isDescending(sortOrder) ? static_cast<uint16_t>(total - 1 - pinnedAscRows[i]) : pinnedAscRows[i];
    overlapRows[overlapCount++] = row;
  }
  std::sort(overlapRows, overlapRows + overlapCount);
}

// --- opening and options ---------------------------------------------------------

void LibraryListActivity::openSelectedBook() {
  std::string path;
  if (selectedEntry() < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    const int store = storeIndexFor(selectedEntry());
    if (store >= static_cast<int>(books.size())) return;
    path = books[static_cast<size_t>(store)].path;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(selectedEntry())));
    if (ordinal == 0xFFFF) return;

    library::ClixRecord record{};
    if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
      LOG_ERR("LIB", "cannot resolve path for row %d", selectedEntry());
      return;
    }
  }
  focusHeroOnReturn = overlayActive() || !query.empty();
  openBookByPath(path);
}

// Shared by row activation and the options menu: the reader screen this opens
// has its own surfaces; a lingering tap flash would gray an unrelated element
// there. The index handle is released first — on hardware only one reader can
// hold a file open at a time, and the reader is about to open files of its own.
void LibraryListActivity::openBookByPath(const std::string& path) {
  app.clearTapFlash();
  index.close();
  onSelectBook(path);
}

void LibraryListActivity::openMenu() {
  app.clearTapFlash();
  auto menu = makeUniqueNoThrow<LibraryMenuActivity>(renderer, mappedInput);
  if (!menu) {
    LOG_ERR("LIB", "OOM: home menu");
    return;
  }
  startActivityForResult(std::move(menu), [this](const ActivityResult&) { swallowHeldReleases(); });
}

void LibraryListActivity::activateIndex(const int index) {
  if (groupsCollapsed) {
    expandGroup(index);
  } else {
    openSelectedBook();
  }
}

void LibraryListActivity::onRowAction(const fui::ActionEvent& event) {
  heroFocused = false;
  UiTabListActivity::onRowAction(event);
}

// Row long-press prompts delete wherever grouping does not own the gesture:
// an active search is already a flat list the reader narrowed down on purpose
// ("find it, hold it, delete it"). Unfiltered Title/Author lists keep
// collapse-to-groups. The Recent shelf always opens the row options menu.
bool LibraryListActivity::deleteEligible() const { return !groupsCollapsed && (!query.empty() || !groupable()); }

void LibraryListActivity::onRowLongPress(const int index) {
  if (isRecentSort(sortOrder)) {
    showRecentBookOptions(index);
  } else if (deleteEligible()) {
    promptDeleteBook(index);
  } else if (!groupsCollapsed && groupable()) {
    collapseGroups(index);
  } else {
    activateIndex(index);
  }
}

// Recent-shelf long-press menu (button hold and touch long-press). The first
// rows may come from RecentBooksStore; the rest are index rows sorted by
// modification time. Only store rows can be removed from recents.
void LibraryListActivity::showRecentBookOptions(const int entry) {
  if (entry < 0 || entry >= listCount()) return;

  std::string path;
  std::string title;
  const bool isStoreRow = entry < pinnedCount();
  if (isStoreRow) {
    const auto& books = RECENT_BOOKS.getBooks();
    const int store = storeIndexFor(entry);
    if (store >= static_cast<int>(books.size())) return;
    path = books[static_cast<size_t>(store)].path;
    title = books[static_cast<size_t>(store)].title;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
    library::ClixRecord record{};
    std::string author;
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readPath(record, path) ||
        !rowTextFor(entry, title, author)) {
      LOG_ERR("LIB", "cannot resolve Recent row %d", entry);
      return;
    }
  }
  showBookOptions(path, title, isStoreRow);
}

void LibraryListActivity::showBookOptions(const std::string& path, const std::string& title, const bool isStoreRow) {
  const char* STORE_OPTIONS[] = {tr(STR_OPEN), tr(STR_REMOVE_FROM_RECENTS), tr(STR_DELETE), tr(STR_LIBRARY_REBUILD)};
  const char* INDEX_OPTIONS[] = {tr(STR_OPEN), tr(STR_DELETE), tr(STR_LIBRARY_REBUILD)};
  app.clearTapFlash();
  optionPopup.show(tr(STR_LIBRARY), title.c_str(), isStoreRow ? STORE_OPTIONS : INDEX_OPTIONS, isStoreRow ? 4 : 3, 0,
                   [this, path, title, isStoreRow](const int choice) {
                     swallowHeldReleases();
                     switch (choice) {
                       case 0:
                         focusHeroOnReturn = path == heroPath || overlayActive() || !query.empty();
                         openBookByPath(path);
                         break;
                       case 1:
                         if (isStoreRow) {
                           promptRemoveRecentBook(path, title);
                         } else {
                           promptDeleteBookByPath(path, title);
                         }
                         break;
                       case 2:
                         if (isStoreRow)
                           promptDeleteBookByPath(path, title);
                         else
                           promptRebuildIndex();
                         break;
                       case 3:
                         if (isStoreRow) promptRebuildIndex();
                         break;
                       default:
                         break;
                     }
                   });
  requestUpdate();
}

// Manual index refresh, same card discipline as the onEnter rebuild: the walk
// wants the card to itself, and the render task must not read the index (or
// the filter) around it.
void LibraryListActivity::promptRebuildIndex() {
  RenderLock lock(*this);
  GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
  index.close();
  rebuildIndex();
  if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
  resetAfterRebuild();
  requestUpdate(true);
}

void LibraryListActivity::resetAfterRebuild() {
  // Sort positions, group starts, pinned rows and the hero all point into the
  // old state.
  applyFilter();
  resolvePinned();
  loadHero();
  heroFocused = heroFocused && heroShown();
  auto& nav = activeNav();
  const int count = listCount();
  if (count == 0) {
    nav.selected = 0;
  } else if (nav.selected > count) {
    nav.selected = count;
  }
  nav.followOnBuild = true;
}

void LibraryListActivity::promptRemoveRecentBook(const std::string& path, const std::string& title) {
  const bool reopenIndex = index.isOpen();
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: recent removal confirmation");
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path, reopenIndex](const ActivityResult& result) {
    swallowHeldReleases();
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    if (!result.isCancelled && RECENT_BOOKS.removeByPath(path)) {
      RenderLock lock(*this);
      resetAfterRebuild();
      closeRouting();
    }
  });
}

void LibraryListActivity::promptDeleteBook(const int entry) {
  if (!index.isOpen() || entry < 0 || entry >= bookRowCount()) return;
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  if (ordinal == 0xFFFF) return;

  std::string path;
  library::ClixRecord record{};
  if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
    LOG_ERR("LIB", "cannot resolve path for row %d", entry);
    return;
  }
  std::string title;
  std::string author;
  rowTextFor(entry, title, author);
  promptDeleteBookByPath(path, title);
}

void LibraryListActivity::promptDeleteBookByPath(const std::string& path, const std::string& title) {
  // The dialog and the delete both want the card; reopen when we resume.
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE) + std::string("? "), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: delete confirmation");
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path](const ActivityResult& result) {
    swallowHeldReleases();
    {
      // Same lock rationale as onEnter: the walk wants the card to itself, and
      // the render task must not read the index (or the filter) around the
      // rebuild.
      RenderLock lock(*this);
      if (!result.isCancelled) {
        LOG_DBG("LIB", "deleting %s", path.c_str());
        clearBookCache(path);
        if (!Storage.remove(path.c_str())) LOG_ERR("LIB", "cannot delete %s", path.c_str());
        if (RECENT_BOOKS.removeByPath(path)) RECENT_BOOKS.saveToFile();
        GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
        rebuildIndex();
      }
      if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
      if (!result.isCancelled) {
        resetAfterRebuild();
      }
    }
    if (!result.isCancelled) {
      closeRouting();
      requestUpdate(true);
    }
  });
}

void LibraryListActivity::openSearch() {
  app.clearTapFlash();
  // No key filtering here on purpose. Greying out the letters that lead nowhere
  // was built, tested on device and removed: a letter you can see but cannot
  // reach reads as a broken keyboard, and the eye keeps returning to it.
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_LIBRARY_SEARCH), query,
                                                           QUERY_MAX, InputType::Text);
  if (!keyboard) {
    LOG_ERR("LIB", "OOM: search keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    swallowHeldReleases();
    if (result.isCancelled) return;
    query = std::get<KeyboardResult>(result.data).text;
    applyFilter();
    auto& nav = activeNav();
    if (query.empty()) {
      // Searching for nothing is the whole shelf again.
      nav.selected = 0;
      heroFocused = heroShown();
    } else if (filteredCount == 0 && !degraded) {
      // Up from Sort By reopens Search even with no results.
      nav.selected = 0;
      heroFocused = false;
    } else {
      // A non-empty result belongs to the list: land on
      // its first surviving row, not on Sort By.
      nav.selected = 1;
      heroFocused = false;
    }
    nav.top = 0;
    requestUpdate();
  });
}

// --- tabs ------------------------------------------------------------------------

void LibraryListActivity::stepTab(const int direction) {
  const int next = (activeTab() + (direction > 0 ? 1 : TAB_SLOTS - 1)) % TAB_SLOTS;
  selectTab(next, false);
}

void LibraryListActivity::onTabAction(const int index) {
  app.clearTapFlash();
  heroFocused = false;
  selectTab(index, true);
}

void LibraryListActivity::selectTab(const int index, const bool toggleIfActive) {
  if (index < 0 || index >= TAB_SLOTS) return;
  if (toggleIfActive && index == activeTab()) descendingTabs ^= static_cast<uint8_t>(1u << index);
  sortOrder = orderForTab(index, descendingTabs);
  // The filter and the overlap rows hold positions in the old order, so they
  // must be rebuilt.
  applyFilter();
  activeTabIndex = index;
  refreshOverlap();
  // Tab changes happen only while the bar owns focus. A tab's remembered row
  // must not pull focus back into the list after the switch.
  auto& nav = activeNav();
  nav.selected = 0;
  nav.top = 0;
  requestUpdate();
}

void LibraryListActivity::toggleSortDirection() { selectTab(activeTab(), true); }

int LibraryListActivity::tabCount() const { return TAB_SLOTS; }

int LibraryListActivity::activeTab() const { return activeTabIndex; }

const char* LibraryListActivity::tabLabel(const int index) const { return tabLabelFor(index); }

// --- rows --------------------------------------------------------------------------

int LibraryListActivity::bookRowCount() const {
  if (!query.empty()) return static_cast<int>(filteredCount);
  if (!overlayActive()) return static_cast<int>(index.bookCount());
  // Every store book the index holds (the hero included) is skipped below the
  // pins, not doubled; pinned books the index missed still show.
  return static_cast<int>(index.bookCount()) - overlapCount + pinnedCount();
}

int LibraryListActivity::listCount() const { return groupsCollapsed ? static_cast<int>(groupCount) : bookRowCount(); }

// Entry position on screen to row position in the sort order. Identity while
// unfiltered and outside the overlay, so the shelf costs nothing when nothing
// is typed. In the overlay, entries below pinnedCount() belong to the store and
// must not reach this; the rest walk past the store books' own sort rows.
int LibraryListActivity::rowFor(const int entry) const {
  if (!query.empty()) {
    if (entry < 0 || entry >= static_cast<int>(filteredCount) || !filtered) return 0;
    return filtered[entry];
  }
  if (!overlayActive()) return entry;
  int row = entry - pinnedCount();
  for (int i = 0; i < overlapCount; i++) {
    if (overlapRows[i] <= row) row++;
  }
  return row;
}

bool LibraryListActivity::groupable() const { return !degraded && !isRecentSort(sortOrder) && bookRowCount() > 0; }

uint32_t LibraryListActivity::titleInitialFor(const int entry) {
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) return 0;
  return library::foldedGroupInitial(std::string_view(record.fold, record.foldLen));
}

bool LibraryListActivity::buildGroupStarts() {
  const int count = bookRowCount();
  if (count <= 0) return false;
  if (groupCapacity < count) {
    auto starts = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(count));
    if (!starts) {
      LOG_ERR("LIB", "cannot allocate %u-byte group map", static_cast<unsigned>(count * sizeof(uint16_t)));
      return false;
    }
    groupStarts = std::move(starts);
    groupCapacity = static_cast<uint16_t>(count);
  }

  groupCount = 0;
  uint32_t previousInitial = 0;
  std::string previousAuthor;
  std::string title;
  std::string author;
  previousAuthor.reserve(128);
  title.reserve(128);
  author.reserve(128);
  for (int entry = 0; entry < count; entry++) {
    bool startsGroup = entry == 0;
    if (isAuthorSort(sortOrder)) {
      rowTextFor(entry, title, author);
      startsGroup = startsGroup || author != previousAuthor;
      previousAuthor = author;
    } else {
      const uint32_t initial = titleInitialFor(entry);
      startsGroup = startsGroup || initial != previousInitial;
      previousInitial = initial;
    }
    if (startsGroup) groupStarts[groupCount++] = static_cast<uint16_t>(entry);
  }
  LOG_DBG("LIB", "group map: %u groups, %u bytes", static_cast<unsigned>(groupCount),
          static_cast<unsigned>(groupCapacity * sizeof(uint16_t)));
  return groupCount > 0;
}

int LibraryListActivity::groupForBook(const int bookEntry) const {
  int group = 0;
  while (group + 1 < groupCount && groupStarts[group + 1] <= bookEntry) group++;
  return group;
}

bool LibraryListActivity::collapseGroups(const int bookEntry) {
  if (!groupable() || !buildGroupStarts()) return false;
  expandedNav = activeNav();
  groupsCollapsed = true;
  auto& nav = activeNav();
  nav.reset(groupForBook(bookEntry) + 1);
  requestUpdate();
  return true;
}

void LibraryListActivity::expandGroup(const int groupEntry) {
  if (!groupsCollapsed || groupEntry < 0 || groupEntry >= groupCount) return;
  const int bookEntry = groupStarts[groupEntry];
  groupsCollapsed = false;
  activeNav() = expandedNav;
  auto& nav = activeNav();
  nav.selected = bookEntry + 1;
  nav.top = bookEntry;
  nav.followOnBuild = true;
  requestUpdate();
}

void LibraryListActivity::restoreExpandedList() {
  if (!groupsCollapsed) return;
  groupsCollapsed = false;
  activeNav() = expandedNav;
  requestUpdate();
}

// One pass over the sort order, keeping what matches. No index, no cache: at the
// 4096-book format cap this is 4096 comparisons of at most 96 bytes. The result
// array is allocated once with the exact upper bound and fails back to an
// explicit message rather than letting vector growth abort the firmware.
void LibraryListActivity::applyFilter() {
  groupsCollapsed = false;
  groupCount = 0;
  filtered.reset();
  filteredCount = 0;
  filterFailed = false;
  // The header shows the active query in place of the screen title, so the
  // reader can see what narrowed the list without reopening the keyboard.
  headerSearchTitle = query.empty() ? std::string() : "“" + query + "”";
  if (query.empty()) return;

  // Folded the same way the stored folds were, articles removed included —
  // otherwise "the hobbit" searches for a word no record contains.
  const std::string needle = library::fold(query, /*stripArticle=*/true);
  const int total = static_cast<int>(index.bookCount());
  if (total <= 0) return;

  auto matches = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(total));
  if (!matches) {
    LOG_ERR("LIB", "cannot allocate %u-byte search result buffer", static_cast<unsigned>(total * sizeof(uint16_t)));
    filterFailed = true;
    return;
  }

  uint16_t matchCount = 0;
  std::string author;
  for (int row = 0; row < total; row++) {
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(row));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    if (library::matchesQuery(std::string_view(record.fold, record.foldLen), needle)) {
      matches[matchCount++] = static_cast<uint16_t>(row);
      continue;
    }
    // The stored fold covers the title only, so the author has to be read and
    // folded here. That is the search most worth having: the reader who knows
    // the author usually also knows where the book is, while "emily" finding
    // Alice Hunter is the case the shelf exists to answer.
    author.clear();
    if (index.readAuthor(record, author) && library::matchesQuery(library::fold(author), needle)) {
      matches[matchCount++] = static_cast<uint16_t>(row);
    }
  }
  filtered = std::move(matches);
  filteredCount = matchCount;
}

// Staged back-out: clear the search, expand collapsed groups, return focus to
// the top of the shelf, then open the menu. Home has nowhere further back to go.
void LibraryListActivity::handleBackAction() {
  auto& nav = activeNav();
  if (!query.empty()) {
    query.clear();
    applyFilter();
    nav.selected = 0;
    nav.top = 0;
    heroFocused = heroShown();
    requestUpdate();
  } else if (groupsCollapsed) {
    restoreExpandedList();
  } else if (!heroFocused && heroShown()) {
    focusHero();
  } else if (rowsFocused() && !degraded) {
    // Keep the current list and viewport while returning focus to Sort By.
    nav.selected = 0;
    requestUpdate();
  } else {
    openMenu();
  }
}

void LibraryListActivity::searchActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->openSearch();
}

void LibraryListActivity::menuActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->openMenu();
}

void LibraryListActivity::sortActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<LibraryListActivity*>(user);
  self.app.clearTapFlash();
  self.heroFocused = false;
  if (event.longPress) {
    self.toggleSortDirection();
  } else {
    self.stepTab(1);
  }
}

void LibraryListActivity::heroActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<LibraryListActivity*>(user);
  self.heroFocused = true;
  self.activeNav().selected = 0;
  if (event.longPress) {
    self.showBookOptions(self.heroPath, self.heroTitle, true);
  } else {
    self.openHero();
  }
}

// Title and author for one entry, read straight from the index. Only ever
// called for rows about to be drawn, so at most a screenful of strings exists
// at once.
bool LibraryListActivity::rowTextFor(const int entry, std::string& title, std::string& author, std::string* fileName,
                                     int* progress) {
  title.clear();
  author.clear();
  if (fileName) fileName->clear();
  if (progress) *progress = -1;
  const auto& books = RECENT_BOOKS.getBooks();
  if (entry < pinnedCount()) {
    const int store = storeIndexFor(entry);
    if (entry < 0 || store >= static_cast<int>(books.size())) return false;
    const auto& book = books[static_cast<size_t>(store)];
    title = book.title.empty() ? fileNameOf(book.path) : book.title;
    author = book.author;
    if (fileName) *fileName = book.path;
    if (progress) *progress = book.progress;
    return true;
  }
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal != 0xFFFF && index.readRecord(ordinal, record)) {
    // The build already decided both fields — from the book's own metadata when
    // it has any, and with one spelling chosen per author across the library.
    // Re-parsing the name here would throw that away, and only works while the
    // name still looks like "Title - Author".
    if (!index.readAuthor(record, author)) author.clear();
    // The stored title when the book gave one, the filename otherwise.
    if (!index.readTitle(record, title) || title.empty()) index.readName(record, title);
    if (fileName) index.readName(record, *fileName);
    if (progress) {
      for (int i = 0; i < pinnedTotal && i < static_cast<int>(books.size()); i++) {
        if (pinnedOrdinals[i] == ordinal) {
          *progress = books[static_cast<size_t>(i)].progress;
          break;
        }
      }
    }
  }
  if (title.empty()) title = tr(STR_LIBRARY_UNKNOWN_TITLE);
  return true;
}

// --- input -----------------------------------------------------------------------

bool LibraryListActivity::handleCustomInput() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;

  // The hero first paints text-only; its cover is written once the shelf is on
  // screen, so a missing thumb never delays Home.
  if (heroCoverPending && firstRenderDone) {
    heroCoverPending = false;
    bool generated;
    {
      RenderLock lock(*this);
      generated = generateHeroCover();
      heroCoverReady = generated;
      heroCover.invalidate();
    }
    if (generated) {
      requestUpdate();
    } else {
      // Remember that this book has no usable cover, so Home stops retrying.
      LOG_INF("LIB", "no cover for %s", heroPath.c_str());
      const auto& books = RECENT_BOOKS.getBooks();
      if (!books.empty() && books.front().path == heroPath) {
        const RecentBook& book = books.front();
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
      }
    }
  }

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

bool LibraryListActivity::handleButtons() {
  const int count = listCount();

  // Every hold action fires at the threshold, mid-hold, including the ones
  // that open a dialog (remove-recent, delete). The release that follows is
  // armed as suppressed by wasLongPressed() and consumed globally by
  // ActivityManager::loop() before any activity runs, so it cannot land in
  // the freshly opened confirmation and select its default.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, LONG_PRESS_MS)) {
    if (heroFocused) {
      showBookOptions(heroPath, heroTitle, true);
    } else if (tabsFocused()) {
      if (!degraded) toggleSortDirection();
    } else if (isRecentSort(sortOrder)) {
      showRecentBookOptions(selectedEntry());
    } else if (deleteEligible()) {
      if (count > 0) promptDeleteBook(selectedEntry());
    } else if (!groupsCollapsed && groupable()) {
      collapseGroups(selectedEntry());
    } else {
      activateIndex(selectedEntry());
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    handleBackAction();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (heroFocused) {
      openHero();
    } else if (tabsFocused()) {
      stepTab(1);
    } else if (count > 0) {
      activateIndex(selectedEntry());
    }
    return true;
  }

  return false;
}

// Continue Reading -> Sort By -> rows. Previous from the top of that walk opens
// Search.
void LibraryListActivity::navigateButtons() {
  const int count = listCount();
  auto& nav = activeNav();
  // Down from the top lands on the first row in view, so a remembered scroll
  // position is not thrown away by the first press.
  const auto firstVisibleRow = [count, &nav] { return std::clamp(nav.top, 0, std::max(0, count - 1)) + 1; };
  buttonNavigator.onNextRelease([this, count, firstVisibleRow] {
    if (heroFocused) {
      heroFocused = false;
      if (degraded && count > 0) {
        moveRingTo(firstVisibleRow());
      } else {
        requestUpdate();
      }
    } else if (count > 0) {
      if (tabsFocused()) {
        moveRingTo(firstVisibleRow());
      } else {
        moveRingTo(ringPos() == count ? 1 : ringPos() + 1);
      }
    }
  });
  buttonNavigator.onPreviousRelease([this, count] {
    if (heroFocused) {
      if (!degraded) openSearch();
    } else if (tabsFocused()) {
      if (heroShown()) {
        focusHero();
      } else if (!degraded) {
        openSearch();
      } else if (count > 0) {
        moveRingTo(count);
      }
    } else if (count > 0) {
      if (ringPos() > 1) {
        moveRingTo(ringPos() - 1);
      } else if (!degraded) {
        moveRingTo(0);
      } else if (heroShown()) {
        focusHero();
      } else {
        moveRingTo(count);
      }
    }
  });
  // A held button steps the order while Sort By has focus (the base behaviour
  // Settings keeps for its tabs) and page-jumps once the selection is down in the rows,
  // where fast travel through a long shelf is what a hold means.
  buttonNavigator.onNextContinuous([this, count, &nav] {
    if (heroFocused) return;
    if (tabsFocused()) {
      stepTab(1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::nextPageIndex(selectedEntry(), count, nav.pageRows()) + 1);
    }
  });
  buttonNavigator.onPreviousContinuous([this, count, &nav] {
    if (heroFocused) return;
    if (tabsFocused()) {
      stepTab(-1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::previousPageIndex(selectedEntry(), count, nav.pageRows()) + 1);
    }
  });
}

// --- screen ------------------------------------------------------------------------

void LibraryListActivity::buildRows(UiScreen& screen) {
  auto& nav = activeNav();
  const int count = listCount();
  const bool authorGrouped = isAuthorSort(sortOrder);
  const bool grouped = !isRecentSort(sortOrder);

  fui::ListProps props;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  // Title at body size, author small beneath it, reading progress as the
  // row's value; no per-row icon, since every row would carry the same one.
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  props.subtitleText = screen.theme().smallText;
  props.valueText = screen.theme().smallText;
  props.rowPaddingY = static_cast<int16_t>(screen.theme().spaceSm + screen.theme().spaceXs);
  // Initials and author groups in body bold: the 17px strike has no bold.
  props.headerText = screen.theme().bodyText;
  props.headerText.bold = true;
  props.headerUnderline = false;
  applyListControlStyle(props, screen.theme());
  syncTabListViewport(screen, props);

  // Keep one extra entry in the reusable window for a clipped trailing row.
  const size_t cap = static_cast<size_t>(nav.visibleRows > 0 ? nav.visibleRows : 1) + 1;
  if (winTitles.size() < cap) winTitles.resize(cap);
  if (winAuthors.size() < cap) winAuthors.resize(cap);
  if (winValues.size() < cap) winValues.resize(cap);
  if (!groupsCollapsed && winHeaders.size() < cap) winHeaders.resize(cap);
  winItems.clear();
  if (winItems.capacity() < cap) winItems.reserve(cap);

  int rows = 0;
  int headers = 0;
  uint32_t previousInitial = 0;
  // Capture this after syncTabListViewport(), which may clamp nav.top.
  const int windowStart = static_cast<int>(props.topIndex);
  for (int entry = windowStart; entry < count && rows < static_cast<int>(cap); entry++) {
    std::string& title = winTitles[static_cast<size_t>(rows)];
    std::string& author = winAuthors[static_cast<size_t>(rows)];
    std::string& value = winValues[static_cast<size_t>(rows)];
    value.clear();
    fui::ListItem item;
    if (groupsCollapsed) {
      const int bookEntry = groupStarts[entry];
      if (authorGrouped) {
        rowTextFor(bookEntry, title, author);
        formatAuthorHeading(author, title);
      } else {
        formatInitialHeading(titleInitialFor(bookEntry), title);
      }
    } else {
      int progress = -1;
      if (!rowTextFor(entry, title, author, nullptr, &progress)) continue;
      uint32_t initial = 0;
      bool startsGroup = false;
      if (authorGrouped) {
        startsGroup = rows == 0 || author != winAuthors[static_cast<size_t>(rows - 1)];
      } else if (grouped) {
        initial = titleInitialFor(entry);
        startsGroup = rows == 0 || initial != previousInitial;
        previousInitial = initial;
      }
      if (startsGroup) {
        std::string& heading = winHeaders[static_cast<size_t>(headers++)];
        if (authorGrouped)
          formatAuthorHeading(author, heading);
        else
          formatInitialHeading(initial, heading);
        item.sectionHeading = heading.c_str();
      }
      if (!authorGrouped && !author.empty()) item.subtitle = author.c_str();
      formatProgress(progress, value);
      if (!value.empty()) item.value = value.c_str();
    }

    item.label = title.c_str();
    item.actionValue = static_cast<int16_t>(entry);
    winItems.push_back(item);
    rows++;
  }

  props.items = winItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(winItems.size());
  screen.list(props);
  const int next = nav.drawnRows;
  const auto body = screen.body();
  LOG_DBG("LIB", "page tab=%d top=%d full=%d loaded=%d body=%d..%d next=%d title=%s", activeTabIndex, windowStart,
          nav.drawnRows, rows, body.y, body.bottom(),
          next < rows ? winItems[static_cast<size_t>(next)].actionValue : -1,
          next < rows ? winItems[static_cast<size_t>(next)].label : "<none>");
}

void LibraryListActivity::formatInitialHeading(uint32_t initial, std::string& out) {
  out.clear();
  if (initial == 0) {
    out.push_back('#');
    return;
  }
  if (initial >= 'a' && initial <= 'z') initial -= 'a' - 'A';
  utf8AppendCodepoint(initial, out);
}

void LibraryListActivity::formatAuthorHeading(const std::string& author, std::string& out) const {
  out = author.empty() ? std::string(tr(STR_LIBRARY_UNKNOWN_AUTHOR)) : author;
  if (author.empty()) return;
  const size_t lastSpace = out.find_last_of(' ');
  if (lastSpace != std::string::npos && lastSpace + 1 < out.size()) {
    out = out.substr(lastSpace + 1) + ", " + out.substr(0, lastSpace);
  }
}

// A book opened and closed on its first pages shows nothing: 0% reads as noise
// next to the books actually under way.
void LibraryListActivity::formatProgress(const int progress, std::string& out) {
  out.clear();
  if (progress >= 100) {
    out = tr(STR_BOOK_FINISHED);
  } else if (progress >= 1) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", progress);
    out = buf;
  }
}

void LibraryListActivity::buildHeader(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  auto& target = screen.target();
  const auto frameRect = screen.frame().screen();
  const fui::Rect rect{frameRect.x, screen.frame().safeRect().y, frameRect.width, HOME_BAND_HEIGHT};
  const auto bandHeight = static_cast<int16_t>(rect.height - metrics.headerUnderlineSize);

  fui::HeaderProps header;
  header.title = headerTitle();
  header.titleText = homeTextStyle(fui::GfxRendererTarget::FONT_BODY, true, theme.headerTitleAlign);
  header.sidePadding = theme.headerSidePadding;
  header.styles = theme.popup;
  if (header.styles.normal.border.kind == fui::PaintKind::None && theme.headerUnderline > 0) {
    header.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    header.styles.normal.borderWidth = theme.headerUnderline;
  }
  header.borderEdges = fui::EdgeBottom;
  // As in Tinta, the title's line box centres on the band above the rule;
  // header() centres it on the whole rect, so lift it by the difference.
  const int16_t titleLineHeight = target.lineHeight(header.titleText.font);
  const auto titleTop = static_cast<int16_t>(rect.y + (bandHeight - titleLineHeight) / 2);
  header.titleOffsetY = static_cast<int16_t>(titleTop - (rect.y + (rect.height - titleLineHeight) / 2));
  const fui::Rect caps = target.inkBounds(header.titleText.font, "H", header.titleText);
  const auto baseline = static_cast<int16_t>(titleTop + caps.y + caps.height);

  const HomeStatus status;
  header.rightReserve = static_cast<int16_t>(status.width(target) + STATUS_GAP);
  // Home is the root: no back arrow, and its doors are on the keys or the
  // touch choice bar, so the band holds no buttons.
  headerWithActions(screen.frame(), rect, header);
  status.draw(target, renderer, static_cast<int16_t>(rect.right() - theme.headerSidePadding), rect.y, bandHeight,
              baseline);
}

// Touch boards get Tinta's choice bar in place of key hints: four ruled cells
// across the bottom edge, the shelf's two doors in the first two.
void LibraryListActivity::buildTouchChoices(UiScreen& screen) {
  auto& target = screen.target();
  const auto frameRect = screen.frame().screen();
  const fui::Rect bar{frameRect.x, static_cast<int16_t>(screen.frame().safeRect().bottom() - HOME_CHOICE_HEIGHT),
                      frameRect.width, HOME_CHOICE_HEIGHT};
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  target.fill(bar, fui::Paint::solid(fui::Color::White));
  target.fill(fui::Rect{bar.x, bar.y, bar.width, 2}, ink);

  struct Choice {
    const char* label;
    fui::ActionId action;
  };
  const Choice choices[TOUCH_CHOICE_CELLS] = {
      {tr(STR_HOME_MENU), ACTION_MENU},
      {degraded ? nullptr : tr(STR_SEARCH), ACTION_SEARCH},
      {nullptr, fui::NO_ACTION},
      {nullptr, fui::NO_ACTION},
  };
  const fui::TextStyle labelStyle = homeTextStyle(fui::GfxRendererTarget::FONT_BODY, true, fui::TextAlign::Center);
  const auto contentHeight = static_cast<int16_t>(bar.height - FOOTER_CONTENT_TOP);
  for (int i = 0; i < TOUCH_CHOICE_CELLS; i++) {
    const auto x0 = static_cast<int16_t>(bar.x + bar.width * i / TOUCH_CHOICE_CELLS);
    const auto x1 = static_cast<int16_t>(bar.x + bar.width * (i + 1) / TOUCH_CHOICE_CELLS);
    if (i > 0)
      target.fill(fui::Rect{x0, static_cast<int16_t>(bar.y + 8), 1, static_cast<int16_t>(bar.height - 16)}, ink);
    if (!choices[i].label) continue;
    screen.frame().hit(fui::Rect{x0, bar.y, static_cast<int16_t>(x1 - x0), bar.height}, choices[i].action, 0,
                       fui::InputTouch);
    target.text(fui::Rect{static_cast<int16_t>(x0 + 2), static_cast<int16_t>(bar.y + FOOTER_CONTENT_TOP),
                          static_cast<int16_t>(x1 - x0 - 4), contentHeight},
                choices[i].label, labelStyle);
  }
}

void LibraryListActivity::buildScreen(UiScreen& screen) {
  const HomeTypeScale typeScale(uiTarget);
  buildHeader(screen);
  int16_t footerHeight = HOME_HINTS_HEIGHT;
  if (mappedInput.hasTouch()) {
    buildTouchChoices(screen);
    footerHeight = HOME_CHOICE_HEIGHT;
  }
  // Both bands sit inside the bezel's viewable insets, as Tinta's do.
  screen.setContentMargin(fui::Insets{HOME_BAND_HEIGHT, 0, footerHeight, 0});

  if (heroShown()) {
    buildHero(screen);
  } else {
    screen.spacer(screen.theme().spaceSm);
  }
  if (!degraded) buildSortRow(screen);
  screen.spacer(screen.theme().spaceSm);
  if (bookRowCount() == 0) {
    const char* message = tr(STR_LIBRARY_NO_RESULTS);
    if (filterFailed) {
      message = tr(STR_LIBRARY_SEARCH_UNAVAILABLE);
    } else if (query.empty()) {
      message = tr(STR_LIBRARY_EMPTY);
    }
    screen.centeredText(message);
    return;
  }
  buildRows(screen);
}

const char* LibraryListActivity::headerTitle() const {
  if (!headerSearchTitle.empty()) return headerSearchTitle.c_str();
  return degraded ? tr(STR_LIBRARY_TITLE_UNSORTED) : tr(STR_CROSSPOINT);
}

// OptionPopup is a self-contained modal: it owns the whole frame (hints
// included) whenever it is up, mirroring the FileBrowser pattern.
void LibraryListActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  // Waking onto Home cleans the retained sleep image with the first paint.
  if (cleanInitialRefresh && !firstRenderDone) renderer.promoteNextRefresh(HalDisplay::HALF_REFRESH);
  UiTabListActivity::render(std::move(lock));
  firstRenderDone = true;
}

// Hints say where each button goes. At the top of the shelf (Continue Reading,
// or Sort By when there is no current book) Back opens the menu and Previous
// opens Search; further down, Back climbs back to the top.
void LibraryListActivity::drawFooter() {
  const bool atTop = heroFocused || (tabsFocused() && !heroShown());
  const bool backOpensMenu = atTop && query.empty() && !groupsCollapsed;
  const char* backLabel = backOpensMenu ? tr(STR_HOME_MENU) : tr(STR_BACK);
  const char* confirmLabel = groupsCollapsed ? tr(STR_SELECT) : tr(STR_OPEN);
  const char* previousLabel = tr(STR_DIR_UP);
  if (heroFocused) {
    confirmLabel = tr(STR_RESUME);
  } else if (tabsFocused()) {
    // On Sort By, Confirm steps to the next order: name it, as Settings does.
    confirmLabel = tabLabelFor((activeTabIndex + 1) % TAB_SLOTS);
  }
  if (atTop && !degraded) previousLabel = tr(STR_SEARCH);
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, previousLabel, tr(STR_DIR_DOWN));
  const char* hints[] = {labels.btn1, labels.btn2, labels.btn3, labels.btn4};
  // Tinta's key-hint band: small labels with their capitals centred below the
  // rule, chevrons likewise.
  const fui::TextStyle labelStyle = homeTextStyle(fui::GfxRendererTarget::FONT_LABEL, false);
  const int labelTop = capsCentredLineTop(uiTarget, labelStyle, FOOTER_CONTENT_TOP,
                                          static_cast<int16_t>(HOME_HINTS_HEIGHT - FOOTER_CONTENT_TOP));
  LyraTheme::drawHintBand(renderer, hints, HOME_HINTS_HEIGHT, SMALL_FONT_ID, labelTop, FOOTER_CONTENT_TOP, true);
}
