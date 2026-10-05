#pragma once

#include <LibraryIndexFile.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/UiTabListActivity.h"
#include "components/HomeCoverCache.h"
#include "components/OptionPopup.h"

// Home: the bookshelf. The current book sits on top, ready to resume; every
// indexed book on the card follows, shown by recency, title, or author. The
// Recent shelf orders by file modification time (when a book landed on the
// card) and pins the recently OPENED books from RecentBooksStore on top, so
// active reads and fresh arrivals share one list. The current book is the
// hero, so the Recent shelf starts with the book after it.
//
// The two-slot row is the whole point rather than a styling choice: the problem
// being solved is "I cannot find my books because I do not know the authors",
// and that is answered by a column the eye can sweep, not by a tidier filename.
// Only the hero carries a cover: one large enough to recognise at a glance
// earns its space, a column of postage stamps would not.
//
// Buttons walk Continue Reading -> Sort By -> rows. The top of that walk is
// where the shelf's other doors are: Previous opens Search and Back opens the
// menu (folders, adding books, games, settings). Rows render through fui::list
// on the UiTabListActivity ring (0 = the Sort By row, 1..N = the books); the
// hero sits outside the ring and owns focus while heroFocused is set, which
// shows as the Continue Reading bar.
//
// Only the visible window of rows is materialized per render (strings and
// ListItems for at most one page). The ordinary shelf therefore keeps one page
// of strings; an active search additionally uses one fallible uint16_t slot per
// indexed book so an allocation failure remains recoverable on the C3.
class LibraryListActivity final : public UiTabListActivity {
 public:
  LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool cleanInitialRefresh = false);

  void onEnter() override;
  void onExit() override;
  bool isHomeActivity() const override { return true; }

 protected:
  // --- UiListActivity / UiTabListActivity contract ---------------------------
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  void onRowAction(const freeink::ui::ActionEvent& event) override;
  int tabCount() const override;
  int activeTab() const override;
  const char* tabLabel(int index) const override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;
  bool tabBarFocused() const override { return tabsFocused(); }
  bool handleCustomInput() override;
  bool handleButtons() override;
  void navigateButtons() override;
  // The FreeInkUI header owns both the title and its touch targets.
  void drawChrome() override {}
  void drawFooter() override;
  // OptionPopup is a self-contained modal: it owns rendering (and the button
  // hints) whenever it is up.
  void render(RenderLock&& lock) override;

 private:
  // The screen's own actions, after the base's ACTION_ROW / ACTION_TAB.
  static constexpr freeink::ui::ActionId ACTION_SEARCH = ACTION_TAB_USER;
  static constexpr freeink::ui::ActionId ACTION_MENU = ACTION_SEARCH + 1;
  static constexpr freeink::ui::ActionId ACTION_HERO = ACTION_MENU + 1;
  static constexpr freeink::ui::ActionId ACTION_SORT = ACTION_HERO + 1;

  // Walk the card and write a fresh index. Blocking, with a popup: at ~70 books
  // it is well under a second, and it only runs when the index is missing or the
  // user asks.
  bool rebuildIndex();

  // Input
  void openSelectedBook();
  void openSearch();
  void openMenu();
  // Shared tail of row activation and the options menu's Open entry.
  void openBookByPath(const std::string& path);
  void promptRebuildIndex();
  void resetAfterRebuild();
  // Long-press menu for a recently opened book (store entry): open / remove
  // from recents / delete / rebuild. Index rows get open / delete / rebuild.
  void showRecentBookOptions(int entry);
  void showBookOptions(const std::string& path, const std::string& title, bool isStoreRow);
  void promptRemoveRecentBook(const std::string& path, const std::string& title);
  // Long-press delete owns the gesture where grouping does not apply: the
  // Recent sort, degraded lists, and any active search result.
  bool deleteEligible() const;
  // Resolves the row's path and title, then confirms via promptDeleteBookByPath.
  void promptDeleteBook(int entry);
  void promptDeleteBookByPath(const std::string& path, const std::string& title);
  bool collapseGroups(int bookEntry);
  void expandGroup(int groupEntry);
  void restoreExpandedList();
  void selectTab(int index, bool toggleIfActive);
  void toggleSortDirection();
  // Sub-screens act on button press, so a button still held when we resume must
  // not also act here. Records what to swallow on the next release.
  void swallowHeldReleases();
  // Staged back-out shared by Button::Back: clear the search, expand groups,
  // return focus to the top, then open the menu.
  void handleBackAction();
  static void searchActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  static void menuActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  static void heroActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  static void sortActionTrampoline(const freeink::ui::ActionEvent& event, void* user);

  // --- hero (the current book) ------------------------------------------------
  bool hasHero() const { return !RECENT_BOOKS.getBooks().empty(); }
  // A search narrows the shelf on purpose; the hero steps aside for it.
  bool heroShown() const { return hasHero() && query.empty(); }
  void focusHero();
  void loadHero();
  void openHero();
  // Writes the hero thumb at heroCoverHeight; false leaves the hero text-only.
  bool generateHeroCover();
  // The current book as plain text beside its cover, then the Continue
  // Reading bar that carries its focus.
  void buildHero(UiScreen& screen);
  // "Sort By  Recent ↓": one value row where the tabs used to be.
  void buildSortRow(UiScreen& screen);
  // A full-width row bar: solid ink when focused, the label on the text column.
  void buildBarRow(UiScreen& screen, freeink::ui::ActionId action, bool focused, const char* label, const char* value);

  // --- shelf memory -----------------------------------------------------------
  // Tab, sort, search, focus and scroll survive leaving Home (RAM only).
  void saveShelf();
  void restoreShelf();

  // Data
  void applyFilter();
  int bookRowCount() const;
  int rowFor(int entry) const;
  // fileName, when asked for, is the on-card name the row's icon derives from
  // (the display title may come from metadata and carry no extension).
  // progress, when asked for, is the recorded percent, or -1 when unknown.
  bool rowTextFor(int entry, std::string& title, std::string& author, std::string* fileName = nullptr,
                  int* progress = nullptr);
  uint32_t titleInitialFor(int entry);
  bool buildGroupStarts();
  int groupForBook(int bookEntry) const;
  bool groupable() const;

  // Screen building
  void buildHeader(UiScreen& screen);
  // Materializes ListItems and their strings for the visible window only.
  void buildRows(UiScreen& screen);
  static void formatInitialHeading(uint32_t initial, std::string& out);
  void formatAuthorHeading(const std::string& author, std::string& out) const;
  static void formatProgress(int progress, std::string& out);
  const char* headerTitle() const override;

  // Ring 0 is the strip; the selected BOOK is ring - 1, with the strip keeping
  // row 0 as the working selection exactly as the pre-ring code did.
  int selectedEntry() const;
  bool tabsFocused() const { return !heroFocused && ringPos() == 0; }
  bool rowsFocused() const { return !heroFocused && ringPos() > 0; }

  // --- pinned recently-opened overlay ---------------------------------------
  // On the unfiltered Recent shelf, newest first, the RecentBooksStore entries
  // after the hero sit on top, in read order; the modification-time list
  // follows with every store book (the hero included) skipped. Entries below
  // pinnedCount() are store rows; the rest go through rowFor().
  bool overlayActive() const;
  int pinnedCount() const;
  // Store index of pinned entry `entry`: the hero (store row 0) is not listed.
  int storeIndexFor(int entry) const { return entry + (hasHero() ? 1 : 0); }
  // Re-match the store against the index (chunked scan). Call whenever the
  // index or the store changes.
  void resolvePinned();
  // Direction-space translation of the matched rows. Call on sort toggles.
  void refreshOverlap();

  library::LibraryIndexFile index;
  int activeTabIndex = 0;
  library::SortOrder sortOrder = library::SortOrder::RecentDesc;
  // One bit per tab; Recent starts descending (newest first).
  uint8_t descendingTabs = 1u << 0;
  // Set when the walk finished but the sort did not, so the screen can say the
  // order is discovery order rather than silently showing a wrong one.
  bool degraded = false;

  // Rows surviving the current query, as positions in the active sort order.
  // Empty query means no filtering and this owns no allocation, so the ordinary
  // shelf pays nothing proportional to the library for the feature.
  std::string query;
  // The active query, pre-quoted for the header: headerTitle() returns a
  // stable c_str the render task can hold across a build.
  std::string headerSearchTitle;
  std::unique_ptr<uint16_t[]> filtered;
  uint16_t filteredCount = 0;
  bool filterFailed = false;

  // One start row per group. Grouping is only offered for the sorted <=512-book
  // index, so this fallible allocation is at most 1 KiB and is reused after its
  // first successful allocation.
  std::unique_ptr<uint16_t[]> groupStarts;
  uint16_t groupCapacity = 0;
  uint16_t groupCount = 0;
  bool groupsCollapsed = false;
  freeink::ui::ListNav expandedNav;

  // Visible-window row storage, reused across renders (buildRows). Bounded by
  // the densest page, never by the library. Headings get their own storage:
  // the surname-first inversion must not overwrite the author slot, whose raw
  // value the next row's group comparison reads.
  std::vector<freeink::ui::ListItem> winItems;
  std::vector<std::string> winTitles;
  std::vector<std::string> winAuthors;
  std::vector<std::string> winHeaders;
  std::vector<std::string> winValues;

  // Pinned overlay state: per store entry its RecentAsc row (0xFFFF when the
  // book is not in the index) and its index ordinal (row progress lookup), and
  // the current-direction rows to skip, sorted ascending, so unpinned entries
  // map to sort rows with a <=10-step walk.
  uint16_t pinnedAscRows[RecentBooksStore::MAX_RECENT_BOOKS] = {};
  uint16_t pinnedOrdinals[RecentBooksStore::MAX_RECENT_BOOKS] = {};
  uint16_t overlapRows[RecentBooksStore::MAX_RECENT_BOOKS] = {};
  uint8_t pinnedTotal = 0;
  uint8_t overlapCount = 0;

  // Hero state. Strings are copied out of the store so the render task never
  // reads a vector the loop task may be rewriting.
  bool heroFocused = false;
  // Set when a book opened from the pinned Recent rows: it becomes the hero, so
  // Home comes back with the hero focused.
  bool focusHeroOnReturn = false;
  bool heroCoverReady = false;
  bool heroCoverPending = false;
  int heroCoverHeight = 0;
  int heroProgress = -1;
  std::string heroPath;
  std::string heroTitle;
  std::string heroAuthor;
  std::string heroCoverPath;
  char heroProgressText[24] = {};
  HomeCoverCache heroCover;
  // The Sort By row's value, rebuilt each render ("Recent ↓").
  char sortValueText[32] = {};

  const bool cleanInitialRefresh;
  // Written by the render task; the loop waits for it before generating a cover.
  std::atomic<bool> firstRenderDone{false};
  bool lockNextConfirmRelease = false;
  bool lockNextBackRelease = false;

  // Row options modal (Recent long-press menu); owned here so it outlives the
  // touch event that opened it.
  OptionPopup optionPopup;
};
