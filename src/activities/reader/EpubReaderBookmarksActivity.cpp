#include "EpubReaderBookmarksActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../../util/BookmarkFile.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr int ENTER_ACTIONS_MODE_MS = 700;
}  // namespace

EpubReaderBookmarksActivity::EpubReaderBookmarksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                         const std::shared_ptr<Epub>& epub, const std::string& epubPath)
    : UiListActivity("EpubReaderBookmarks", renderer, mappedInput, /*wantsTouchLongPress=*/true),
      epub(epub),
      epubPath(epubPath) {}

void EpubReaderBookmarksActivity::onEnter() {
  UiListActivity::onEnter();
  RenderLock lock;

  if (!epub) {
    return;
  }

#if LILA_COMPANION
  restoreBookmarkList(lock);
#else
  if (!BookmarkFile::load(epubPath, bookmarks)) {
    bookmarks.shrink_to_fit();
  }
  rebuildBookmarkRowItems();
#endif
  LOG_DBG("EPB", "Loaded %d bookmarks for book: %s", static_cast<int>(bookmarks.size()), epubPath.c_str());
}

#if LILA_COMPANION
void EpubReaderBookmarksActivity::restoreBookmarkList(const RenderLock&) {
  bookmarkRowItems.clear();
  bookmarkSubtitles.clear();
  conflictUi.reset();
  associationUi = false;
  pendingBookmarkError = StrId::_COUNT;
  const auto result = companion::restoreReaderBookmarks(*epub, bookmarks, bookmarkBinding);
  if (bookmarkBinding.legacyAssociationRequired) {
    associationUi = true;
    rebuildAssociationRows();
    return;
  }
  if (companion::tinta_body_detail::nonzero(bookmarkBinding.conflict)) {
    if (!loadConflictChoices(0)) rebuildBookmarkRowItems();
    return;
  }
  if (result != companion::TintaJournalResult::Ok)
    pendingBookmarkError = result == companion::TintaJournalResult::Conflict ? StrId::STR_BOOKMARK_CONFLICT
                                                                             : StrId::STR_SAVE_BOOKMARK_FAILED;
  rebuildBookmarkRowItems();
}

void EpubReaderBookmarksActivity::rebuildAssociationRows() {
  bookmarkRowItems.clear();
  bookmarkSubtitles.clear();
  bookmarkRowItems.reserve(2);
  fui::ListItem item;
  item.label = tr(STR_IMPORT_LEGACY_BOOKMARKS);
  item.subtitle = tr(STR_IMPORT_LEGACY_BOOKMARKS_EXPLANATION);
  item.actionValue = 0;
  bookmarkRowItems.push_back(item);
  item.label = tr(STR_LEAVE_BOOKMARKS_UNASSOCIATED);
  item.subtitle = tr(STR_KEEP_LEGACY_BOOKMARK_FILE);
  item.actionValue = 1;
  bookmarkRowItems.push_back(item);
}

void EpubReaderBookmarksActivity::selectAssociation(int index) {
  RenderLock lock;
  if (!associationUi || index < 0 || index > 1) return;
  app.clearTapFlash();
  const auto decision =
      index == 0 ? companion::LegacyBookmarkDecision::Associate : companion::LegacyBookmarkDecision::LeaveUnassociated;
  const auto result = companion::decideReaderLegacyBookmarks(*epub, bookmarks, bookmarkBinding, decision);
  associationUi = bookmarkBinding.legacyAssociationRequired;
  nav.selected = 0;
  if (associationUi) {
    rebuildAssociationRows();
  } else if (companion::tinta_body_detail::nonzero(bookmarkBinding.conflict)) {
    loadConflictChoices(0);
  } else {
    rebuildBookmarkRowItems();
  }
  nav.follow(listCount());
  const bool presented =
      result == companion::TintaJournalResult::Conflict &&
      (bookmarkBinding.legacyAssociationRequired || companion::tinta_body_detail::nonzero(bookmarkBinding.conflict));
  if (result != companion::TintaJournalResult::Ok && !presented)
    pendingBookmarkError = result == companion::TintaJournalResult::Conflict ? StrId::STR_BOOKMARK_CONFLICT
                                                                             : StrId::STR_SAVE_BOOKMARK_FAILED;
  requestUpdate();
}

void EpubReaderBookmarksActivity::closeConflictChoices() {
  bookmarkRowItems.clear();
  bookmarkSubtitles.clear();
  conflictUi.reset();
  rebuildBookmarkRowItems();
}

bool EpubReaderBookmarksActivity::loadConflictChoices(uint32_t offset) {
  // Caller holds RenderLock; page and label buffers must outlive row pointers.
  if (!conflictUi) {
    conflictUi = makeUniqueNoThrow<ConflictUi>();
    if (!conflictUi) {
      LOG_ERR("EPB", "OOM: bookmark conflict UI");
      pendingBookmarkError = StrId::STR_SAVE_BOOKMARK_FAILED;
      return false;
    }
  }
  const auto result = companion::loadReaderBookmarkChoices(*epub, conflictUi->page, bookmarkBinding, offset);
  if (result != companion::TintaJournalResult::Ok) {
    closeConflictChoices();
    pendingBookmarkError = result == companion::TintaJournalResult::Conflict ? StrId::STR_BOOKMARK_CONFLICT
                                                                             : StrId::STR_SAVE_BOOKMARK_FAILED;
    return false;
  }
  conflictUi->offset = offset;
  pendingBookmarkError = StrId::_COUNT;
  rebuildConflictRows();
  nav.selected = 0;
  nav.follow(listCount());
  return true;
}

void EpubReaderBookmarksActivity::formatConflictSubtitle(uint32_t index, const companion::BookmarkBodyView& body) {
  auto& output = conflictUi->subtitles[index];
  int written = 0;
  if (body.deleted)
    written = snprintf(output.data(), output.size(), tr(STR_BOOKMARK_DELETED_VERSION_FORMAT),
                       static_cast<unsigned>(conflictUi->offset + index + 1));
  else if (!body.summary.empty())
    written =
        snprintf(output.data(), output.size(), tr(STR_BOOKMARK_VERSION_SUMMARY_FORMAT),
                 static_cast<unsigned>(conflictUi->offset + index + 1), static_cast<unsigned>(body.anchor.spine) + 1,
                 static_cast<int>(body.summary.size()), reinterpret_cast<const char*>(body.summary.data()));
  else
    written =
        snprintf(output.data(), output.size(), tr(STR_BOOKMARK_VERSION_FORMAT),
                 static_cast<unsigned>(conflictUi->offset + index + 1), static_cast<unsigned>(body.anchor.spine) + 1);
  if (written < 0) output[0] = 0;
  if (written >= static_cast<int>(output.size())) {
    size_t end = output.size() - 1;
    while (end && (static_cast<unsigned char>(output[end - 1]) & 0xc0) == 0x80) --end;
    if (end && (static_cast<unsigned char>(output[end - 1]) & 0x80)) --end;
    output[end] = 0;
  }
}

void EpubReaderBookmarksActivity::appendConflictRow(uint32_t index) {
  companion::BookmarkBodyView body;
  if (!conflictUi->page.choice(index, body)) return;
  auto& label = conflictUi->labels[index];
  const auto text = body.name.empty() ? body.summary : body.name;
  size_t size = std::min(text.size(), label.size() - 1);
  while (size && size < text.size() && (text[size] & 0xc0) == 0x80) --size;
  if (size) memcpy(label.data(), text.data(), size);
  label[size] = 0;
  formatConflictSubtitle(index, body);
  fui::ListItem item;
  item.label = body.deleted ? tr(STR_BOOKMARK_DELETED_VERSION) : size ? label.data() : tr(STR_UNNAMED);
  item.subtitle = conflictUi->subtitles[index].data();
  item.icon = listIconFor(UIIcon::Bookmark, 32);
  item.actionValue = static_cast<int16_t>(bookmarkRowItems.size());
  bookmarkRowItems.push_back(item);
}

void EpubReaderBookmarksActivity::rebuildConflictRows() {
  bookmarkRowItems.clear();
  bookmarkSubtitles.clear();
  bookmarkRowItems.reserve(companion::NativeBookmarkChoicePage::CAPACITY + 2);
  for (uint32_t at = 0; at < conflictUi->page.count(); ++at) appendConflictRow(at);
  if (conflictUi->page.hasNext()) {
    fui::ListItem item;
    item.label = tr(STR_BOOKMARK_NEXT_VERSIONS);
    item.actionValue = static_cast<int16_t>(bookmarkRowItems.size());
    bookmarkRowItems.push_back(item);
  }
  if (conflictUi->offset) {
    fui::ListItem item;
    item.label = tr(STR_BOOKMARK_PREVIOUS_VERSIONS);
    item.actionValue = static_cast<int16_t>(bookmarkRowItems.size());
    bookmarkRowItems.push_back(item);
  }
}

void EpubReaderBookmarksActivity::selectConflictChoice(int index) {
  RenderLock lock;
  if (!conflictUi || index < 0 || index >= listCount()) return;
  const auto count = conflictUi->page.count();
  if (static_cast<uint32_t>(index) >= count) {
    const bool next = conflictUi->page.hasNext() && static_cast<uint32_t>(index) == count;
    loadConflictChoices(next ? conflictUi->offset + count
                             : conflictUi->offset - companion::NativeBookmarkChoicePage::CAPACITY);
    requestUpdate();
    return;
  }
  const auto result =
      companion::resolveReaderBookmarkChoice(*epub, conflictUi->page, index, bookmarks, bookmarkBinding);
  if (bookmarkBinding.legacyAssociationRequired) {
    closeConflictChoices();
    associationUi = true;
    rebuildAssociationRows();
    nav.selected = 0;
    nav.follow(listCount());
    requestUpdate();
    return;
  }
  if (result == companion::TintaJournalResult::Conflict &&
      companion::tinta_body_detail::nonzero(bookmarkBinding.conflict) && loadConflictChoices(0)) {
    requestUpdate();
    return;
  }
  closeConflictChoices();
  nav.selected = 0;
  nav.follow(listCount());
  if (result != companion::TintaJournalResult::Ok)
    pendingBookmarkError = result == companion::TintaJournalResult::Conflict ? StrId::STR_BOOKMARK_CONFLICT
                                                                             : StrId::STR_SAVE_BOOKMARK_FAILED;
  requestUpdate();
}
#endif

// Derives bookmarkSubtitles/bookmarkRowItems from `bookmarks`. Called
// whenever `bookmarks` changes (onEnter() load, post-delete) so buildScreen()
// reuses the cached rows on every repaint instead of re-composing a
// percentage/chapter/TOC-title subtitle string per bookmark each time.
void EpubReaderBookmarksActivity::rebuildBookmarkRowItems() {
  bookmarkSubtitles.clear();
  bookmarkRowItems.clear();
  if (!epub) {
    return;
  }
#if LILA_COMPANION
  if (!bookmarkBinding.ready && !associationUi && !conflictUi) {
    bookmarkRowItems.reserve(1);
    fui::ListItem item;
    item.label = tr(STR_RETRY);
    item.actionValue = 0;
    bookmarkRowItems.push_back(item);
    return;
  }
#endif
  bookmarkSubtitles.reserve(bookmarks.size());
  bookmarkRowItems.reserve(bookmarks.size());
  for (const auto& bookmark : bookmarks) {
    bookmarkSubtitles.push_back(bookmarkSubtitle(bookmark));

    fui::ListItem item;
    item.label = bookmark.name.empty() ? bookmark.summary.c_str() : bookmark.name.c_str();
    item.subtitle = bookmarkSubtitles.back().c_str();
    item.icon = listIconFor(UIIcon::Bookmark, 32);  // subtitle rows carry the larger icon
    item.actionValue = static_cast<int16_t>(bookmarkRowItems.size());
    bookmarkRowItems.push_back(item);
  }
}

std::string EpubReaderBookmarksActivity::bookmarkSubtitle(const BookmarkEntry& bookmark) const {
  const auto tocIndex = epub->getTocIndexForSpineIndex(bookmark.computedSpineIndex);
  const auto tocTitle = (tocIndex >= 0) ? epub->getTocItem(tocIndex).title : tr(STR_UNNAMED);
  char prefix[40];
  const int percent = static_cast<int>(std::clamp(bookmark.percentage, 0.0f, 1.0f) * 100.0f + 0.5f);
  if (bookmark.computedChapterPageCount > 0) {
    snprintf(prefix, sizeof(prefix), tr(STR_BOOKMARK_PAGE_FORMAT), percent,
             static_cast<unsigned>(bookmark.computedChapterProgress) + 1,
             static_cast<unsigned>(bookmark.computedChapterPageCount));
  } else {
    snprintf(prefix, sizeof(prefix), tr(STR_BOOKMARK_PERCENT_FORMAT), percent);
  }
  std::string subtitle;
  subtitle.reserve(strlen(prefix) + tocTitle.size());
  subtitle = prefix;
  subtitle += tocTitle;
  return subtitle;
}

void EpubReaderBookmarksActivity::openSelectedBookmark() {
#if LILA_COMPANION
  if (associationUi) {
    selectAssociation(nav.selected);
    return;
  }
  if (conflictUi) {
    selectConflictChoice(nav.selected);
    return;
  }
  if (!bookmarkBinding.ready) {
    RenderLock lock;
    restoreBookmarkList(lock);
    nav.selected = 0;
    nav.follow(listCount());
    requestUpdate();
    return;
  }
#endif
  if (bookmarks.empty()) {
    return;
  }
  const auto& bookmark = bookmarks.at(nav.selected);
  ProgressChangeResult result{};
  result.xpath = bookmark.xpath;
  result.percentage = bookmark.percentage;
  result.hasSavedProgress = true;
  result.hasVisibleTextOffset = bookmark.hasVisibleTextOffset;
  result.visibleTextOffset = bookmark.visibleTextOffset;
  // The offset is spine-relative, so carry its spine even when the legacy page hints below
  // are stale. The reader validates the index.
  result.spineIndex = bookmark.computedSpineIndex;
  if (bookmark.computedChapterPageCount > 0 && bookmark.computedChapterProgress < bookmark.computedChapterPageCount &&
      bookmark.computedSpineIndex < epub->getSpineItemsCount()) {
    result.page = bookmark.computedChapterProgress;
    result.totalPages = bookmark.computedChapterPageCount;
  }
  setResult(std::move(result));
  finish();
}

void EpubReaderBookmarksActivity::activateIndex(const int index) {
  if (confirmPopup.isActive()) return;
  // The interaction table can deliver a row index captured before a delete
  // shrank the list; the next render re-registers the rows.
  if (index < 0 || index >= listCount()) return;
  // The tapped row leaves this screen; a lingering flash would gray an
  // unrelated row on the next render.
  app.clearTapFlash();
  nav.selected = index;
  openSelectedBookmark();
}

void EpubReaderBookmarksActivity::onRowLongPress(const int index) {
#if LILA_COMPANION
  if (!bookmarkBinding.ready || conflictUi || associationUi) return;
#endif
  if (confirmPopup.isActive()) return;
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  showBookmarkActions();
}

bool EpubReaderBookmarksActivity::handleCustomInput() {
  // Bookmark action or delete confirmation popup.
  if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  if (confirmingDelete) {
    // Popup dismissed without a selection (Back button or tap outside): cancel delete
    confirmingDelete = false;
    requestUpdate();
    return true;
  }
  return false;
}

bool EpubReaderBookmarksActivity::handleButtons() {
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, ENTER_ACTIONS_MODE_MS)) {
#if LILA_COMPANION
    if (!bookmarkBinding.ready || conflictUi || associationUi) return true;
#endif
    showBookmarkActions();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSelectedBookmark();
    return true;
  }

  return false;
}

void EpubReaderBookmarksActivity::startRename() {
  if (bookmarks.empty() || nav.selected < 0 || nav.selected >= listCount()) {
    return;
  }

  app.clearTapFlash();
  const int renameIndex = nav.selected;
  auto keyboard =
      makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_RENAME), bookmarks[renameIndex].name,
                                               BookmarkEntry::MAX_NAME_LENGTH, InputType::Text);
  if (!keyboard) {
    LOG_ERR("EPB", "OOM: bookmark rename keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this, renameIndex](const ActivityResult& result) {
    RenderLock lock;
    if (result.isCancelled || renameIndex < 0 || renameIndex >= listCount()) {
      return;
    }
#if LILA_COMPANION
    const auto renamed = companion::renameReaderBookmark(
        *epub, bookmarks[renameIndex], std::get<KeyboardResult>(result.data).text, bookmarks, bookmarkBinding);
    if (renamed != companion::TintaJournalResult::Ok)
      pendingBookmarkError = renamed == companion::TintaJournalResult::Conflict ? StrId::STR_BOOKMARK_CONFLICT
                                                                                : StrId::STR_SAVE_BOOKMARK_FAILED;
    rebuildBookmarkRowItems();
    nav.selected = std::min(renameIndex, std::max(0, listCount() - 1));
    nav.follow(listCount());
    if (companion::tinta_body_detail::nonzero(bookmarkBinding.conflict)) loadConflictChoices(0);
#else
    std::string previousName = std::move(bookmarks[renameIndex].name);
    bookmarks[renameIndex].name = std::get<KeyboardResult>(result.data).text;
    rebuildBookmarkRowItems();
    if (!BookmarkFile::save(epubPath, bookmarks)) {
      LOG_ERR("EPB", "Failed to save bookmarks after rename");
      bookmarks[renameIndex].name = std::move(previousName);
      rebuildBookmarkRowItems();
    }
#endif
    requestUpdate();
  });
}

void EpubReaderBookmarksActivity::showBookmarkActions() {
  if (bookmarks.empty() || confirmPopup.isActive()) {
    return;
  }
#if LILA_COMPANION
  if (!bookmarkBinding.ready) {
    pendingBookmarkError = StrId::STR_SAVE_BOOKMARK_FAILED;
    requestUpdate();
    return;
  }
#endif
  const StrId options[] = {StrId::STR_OPEN, StrId::STR_RENAME, StrId::STR_DELETE};
  confirmPopup.show(StrId::STR_BOOKMARKS, options, 3, 0, [this](const int idx) {
    if (idx == 0) {
      openSelectedBookmark();
    } else if (idx == 1) {
      startRename();
    } else if (idx == 2) {
      showDeleteConfirmation();
    }
  });
  requestUpdate();
}

void EpubReaderBookmarksActivity::showDeleteConfirmation() {
  if (bookmarks.empty() || confirmPopup.isActive()) {
    return;
  }
  confirmingDelete = true;
  const char* options[] = {tr(STR_CANCEL), tr(STR_DELETE)};
  confirmPopup.show(tr(STR_CONFIRM_DELETE_BOOKMARK), options, 2, 0, [this](int idx) {
    confirmingDelete = false;
    if (idx == 1) {
      deleteSelectedBookmark();
    }
    requestUpdate();
  });
  requestUpdate();
}

void EpubReaderBookmarksActivity::deleteSelectedBookmark() {
  RenderLock lock;
  if (nav.selected < 0 || nav.selected >= listCount() || !epub) return;
#if LILA_COMPANION
  const auto result = companion::deleteReaderBookmark(*epub, bookmarks[nav.selected], bookmarks, bookmarkBinding);
  rebuildBookmarkRowItems();
  nav.selected = std::min(nav.selected.load(), std::max(0, listCount() - 1));
  if (result != companion::TintaJournalResult::Ok) {
    pendingBookmarkError = result == companion::TintaJournalResult::Conflict ? StrId::STR_BOOKMARK_CONFLICT
                                                                             : StrId::STR_SAVE_BOOKMARK_FAILED;
    nav.follow(listCount());
    if (companion::tinta_body_detail::nonzero(bookmarkBinding.conflict)) loadConflictChoices(0);
    requestUpdate(true);
    return;
  }
#else
  bookmarks.erase(bookmarks.begin() + nav.selected);
  // Deleting shifts every later bookmark's index, so the cached subtitles and
  // actionValues must be re-derived, not just trimmed — and before the SD
  // save, so the render task never sees rows aliasing the erased storage.
  rebuildBookmarkRowItems();
  if (!BookmarkFile::save(epubPath, bookmarks)) {
    LOG_ERR("EPB", "Failed to save bookmarks after delete");
  }
#endif

  // Move selector up if we deleted the last item
  if (nav.selected >= static_cast<int>(bookmarks.size()) && nav.selected > 0) {
    nav.selected--;
  }

  if (!listCount()) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  nav.follow(listCount());
  requestUpdate(true);
}

void EpubReaderBookmarksActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the title band render() paints.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (!listCount()) {
    screen.centeredText(tr(STR_NO_BOOKMARKS), screen.theme().bodyText);
    return;
  }

  // This hint names a physical button, so omit it on touch boards.
  if (!mappedInput.hasTouch()
#if LILA_COMPANION
      && bookmarkBinding.ready && !conflictUi && !associationUi
#endif
  ) {
    const int helpLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const fui::Rect band = screen.takeBottom(static_cast<int16_t>(helpLineHeight + metrics.verticalSpacing));
    GUI.drawHelpText(renderer, Rect{band.x, band.y + metrics.verticalSpacing, band.width, helpLineHeight},
                     tr(STR_HOLD_OPEN_FOR_ACTIONS));
  }

  // bookmarkSubtitles/bookmarkRowItems are built once whenever `bookmarks`
  // changes (see rebuildBookmarkRowItems()) and reused here on every repaint.
  fui::ListProps props;
  props.items = bookmarkRowItems.data();
  props.count = static_cast<uint16_t>(bookmarkRowItems.size());
  props.action = ACTION_ROW;
  // Tap opens; long-press shows bookmark actions (physical buttons stay in loop()).
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  syncListViewport(screen, props);
  screen.list(props);
}

void EpubReaderBookmarksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  // Landscape orientation: reserve a horizontal gutter for button hints.
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  // Inverted portrait: reserve vertical space for hints at the top.
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 40 : 0;
  // Landscape CW places hints on the left edge; CCW keeps them on the right.
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int contentY = isPortraitInverted ? 50 : 0;

  // Manual centering to honor content gutters.
  const char* title = tr(STR_BOOKMARKS);
#if LILA_COMPANION
  if (conflictUi) title = tr(STR_CHOOSE_BOOKMARK_VERSION);
  if (associationUi) title = tr(STR_ASSOCIATE_LEGACY_BOOKMARKS);
#endif
  const int titleX = contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, title, true, EpdFontFamily::BOLD);

  renderUi();

  if (confirmPopup.processRender(renderer, mappedInput)) return;

  if (pendingBookmarkError != StrId::_COUNT) {
    GUI.drawPopup(renderer, (pendingBookmarkError == StrId::STR_BOOKMARK_CONFLICT ? tr(STR_BOOKMARK_CONFLICT)
                                                                                  : tr(STR_SAVE_BOOKMARK_FAILED)));
    pendingBookmarkError = StrId::_COUNT;
  }

  const auto confirmLabel = listCount() > 0 ? tr(STR_SELECT) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK_TO_BOOK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
