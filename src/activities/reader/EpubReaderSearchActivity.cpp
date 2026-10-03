#include "EpubReaderSearchActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/search24.h"

namespace fui = freeink::ui;
using booksearch::BookSearchWorker;

namespace {

std::string trimmed(const std::string& text) {
  const size_t first = text.find_first_not_of(" \t\n");
  if (first == std::string::npos) return {};
  const size_t last = text.find_last_not_of(" \t\n");
  return text.substr(first, last - first + 1);
}

}  // namespace

EpubReaderSearchActivity::EpubReaderSearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   std::shared_ptr<Epub> epub, std::string query,
                                                   const bool showResults, const int selectedResult)
    : UiListActivity("EpubReaderSearch", renderer, mappedInput),
      epub(std::move(epub)),
      query(std::move(query)),
      reopenResults(showResults),
      initialSelection(selectedResult) {}

void EpubReaderSearchActivity::onEnter() {
  UiListActivity::onEnter();
  // The reader beneath keeps its page glyphs; let the font cache give them back if the heap is short, as
  // the contents list does, so excerpts in fallback scripts stay resident.
  if (auto* fcm = renderer.getFontCacheManager()) fcm->clearCache();

  if (!epub) {
    leave();
    return;
  }
  resultsFilePath = booksearch::resultsPath(epub->getCachePath());
  if (query.empty()) {
    // The last search in this book, kept with its results: searching it again is instant.
    booksearch::ResultsHeader saved;
    if (booksearch::readHeader(resultsFilePath, saved)) query = saved.query;
  }
  worker = makeUniqueNoThrow<BookSearchWorker>();
  if (!worker) LOG_ERR("ERSR", "OOM: search worker");

  if (reopenResults && worker) {
    startSearch(query, initialSelection + 1);
  } else {
    // Pushed before the first paint, so the screen opens on the keyboard.
    openKeyboard();
  }
}

void EpubReaderSearchActivity::onExit() {
  // The task must be gone before this activity is: it reads the worker's state and the shared Epub.
  if (worker) worker->stop();
  UiListActivity::onExit();
}

void EpubReaderSearchActivity::openKeyboard() {
  app.clearTapFlash();
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH_BOOK), query,
                                                           booksearch::QUERY_MAX_BYTES, InputType::Text);
  if (!keyboard) {
    LOG_ERR("ERSR", "OOM: search keyboard");
    if (!searched) leave();
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    const std::string text = result.isCancelled ? std::string() : trimmed(std::get<KeyboardResult>(result.data).text);
    if (text.empty()) {
      // Nothing to search: before any search that means the reader changed their mind.
      if (!searched) leave();
      return;
    }
    startSearch(text);
  });
}

void EpubReaderSearchActivity::startSearch(const std::string& text, const int selectRow) {
  const bool sameSearch = searched && text == query && worker && worker->status() != BookSearchWorker::Status::Idle &&
                          worker->status() != BookSearchWorker::Status::Failed;
  searched = true;
  if (sameSearch) {
    requestUpdate();
    return;
  }
  if (worker) worker->stop();
  {
    RenderLock lock;
    query = text;
    snprintf(queryLabel, sizeof(queryLabel), "\xE2\x80\x9C%s\xE2\x80\x9D", query.c_str());
    searchGeneration++;
    shownCount = 0;
    // Out of range, so the first poll always formats the status.
    shownPercent = 0xFF;
    nav.reset();
    // Valid once the worker reports the saved results (pollWorker clamps it otherwise).
    nav.selected = std::max(selectRow, 0);
    firstResultSelected = selectRow > 0;
  }
  if (worker) {
    worker->start(epub, query);
    pollWorker();
  } else {
    RenderLock lock;
    formatStatus();
  }
  requestUpdate();
}

bool EpubReaderSearchActivity::pollWorker() {
  if (!worker) return false;
  const auto status = worker->status();
  const int count = worker->resultCount();
  const uint8_t percent = worker->progressPercent();
  if (status == shownStatus && count == shownCount && percent == shownPercent) return false;

  // Settled states, the first result and new rows show at once; plain progress waits its turn.
  const bool changedState = status != shownStatus || (shownCount == 0 && count > 0);
  if (!changedState && millis() - lastProgressRefreshMs < PROGRESS_REFRESH_MS) return false;
  lastProgressRefreshMs = millis();
  {
    RenderLock lock;
    shownStatus = status;
    shownCount = count;
    shownPercent = percent;
    formatStatus();
    if (nav.selected >= listCount()) nav.selected = listCount() - 1;
  }
  if (!firstResultSelected && count > 0 && nav.selected == 0) {
    // The first result is what Confirm should open; the query row stays one press up.
    moveSelectionTo(1);
    firstResultSelected = true;
  }
  return true;
}

void EpubReaderSearchActivity::formatStatus() {
  char count[32];
  if (shownCount == 1) {
    snprintf(count, sizeof(count), "%s", tr(STR_SEARCH_ONE_MATCH));
  } else {
    snprintf(count, sizeof(count), tr(STR_SEARCH_MATCHES), shownCount);
  }
  switch (shownStatus) {
    case BookSearchWorker::Status::Running:
    case BookSearchWorker::Status::Stopped: {
      const int len = snprintf(statusText, sizeof(statusText), tr(STR_SEARCH_SEARCHING), shownPercent);
      if (shownCount > 0 && len > 0 && static_cast<size_t>(len) < sizeof(statusText)) {
        snprintf(statusText + len, sizeof(statusText) - len, "  \xC2\xB7  %s", count);
      }
      break;
    }
    case BookSearchWorker::Status::Complete:
      snprintf(statusText, sizeof(statusText), "%s", shownCount == 0 ? tr(STR_SEARCH_NO_MATCHES) : count);
      break;
    case BookSearchWorker::Status::Full:
      snprintf(statusText, sizeof(statusText), tr(STR_SEARCH_FIRST_MATCHES), shownCount);
      break;
    case BookSearchWorker::Status::Idle:
      snprintf(statusText, sizeof(statusText), "%s", tr(STR_SEARCH_NOTHING_TO_FIND));
      break;
    case BookSearchWorker::Status::Failed:
      snprintf(statusText, sizeof(statusText), "%s", tr(STR_SEARCH_FAILED));
      break;
  }
}

void EpubReaderSearchActivity::loop() {
  if (pollWorker()) requestUpdate();
  UiListActivity::loop();
}

void EpubReaderSearchActivity::openResult(const int resultIndex) {
  booksearch::ResultRecord record;
  if (booksearch::readResults(resultsFilePath, static_cast<uint16_t>(resultIndex), 1, &record) != 1) {
    LOG_ERR("ERSR", "Cannot read result %d", resultIndex);
    return;
  }
  // The search pauses here; Back from the result resumes it.
  if (worker) worker->stop();
  SearchResult result;
  result.spineIndex = record.spineIndex;
  result.start = record.start;
  result.end = record.end;
  result.resultIndex = resultIndex;
  result.query = query;
  setResult(std::move(result));
  finish();
}

void EpubReaderSearchActivity::leave() {
  if (worker) worker->stop();
  SearchResult closed;
  closed.query = query;
  closed.fromResults = searched;
  ActivityResult result(std::move(closed));
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void EpubReaderSearchActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  if (index == 0) {
    openKeyboard();
  } else {
    openResult(index - 1);
  }
}

bool EpubReaderSearchActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    leave();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateIndex(nav.selected);
    return true;
  }
  return false;
}

void EpubReaderSearchActivity::refreshWindow(const int firstRow) {
  const int total = listCount();
  const int start = std::clamp(firstRow, 0, std::max(0, total - ROW_WINDOW));
  if (start == windowStart && total == windowTotal && windowGeneration == searchGeneration) return;
  windowStart = start;
  windowTotal = total;
  windowGeneration = searchGeneration;
  windowCount = std::min(ROW_WINDOW, total - start);

  // Row 0 is the query; the rest are results first..first+n-1.
  const int firstResult = std::max(0, start - 1);
  const int resultRows = windowCount - (start == 0 ? 1 : 0);
  const size_t read = resultRows > 0 ? booksearch::readResults(resultsFilePath, static_cast<uint16_t>(firstResult),
                                                               static_cast<size_t>(resultRows), windowRecords)
                                     : 0;
  if (static_cast<int>(read) < resultRows) windowCount = static_cast<int>(read) + (start == 0 ? 1 : 0);

  {
    // Section names come from the book's table of contents, which the search task also reads.
    BookSearchWorker::BookLock lock(*worker);
    for (size_t i = 0; i < read; i++) {
      const auto& record = windowRecords[i];
      const std::string title = record.tocIndex >= 0 ? epub->getTocItem(record.tocIndex).title : std::string();
      if (title.empty()) {
        snprintf(windowSubtitles[i], sizeof(windowSubtitles[i]), "%u%%", record.bookPercent);
      } else {
        snprintf(windowSubtitles[i], sizeof(windowSubtitles[i]), "%s  \xC2\xB7  %u%%", title.c_str(),
                 record.bookPercent);
      }
    }
  }

  for (int row = 0; row < windowCount; row++) {
    const int absolute = start + row;
    fui::ListItem item;
    item.actionValue = static_cast<int16_t>(absolute);
    if (absolute == 0) {
      item.label = queryLabel;
      item.subtitle = statusText;
      item.icon = fui::bitmapFromIcon(Search24Icon);
    } else {
      const int slot = absolute - 1 - firstResult;
      item.label = windowRecords[slot].excerpt;
      item.subtitle = windowSubtitles[slot];
    }
    windowItems[row] = item;
  }

  // One SD pass for the window's fallback glyphs (CJK excerpts), so repaints inside it stay in RAM.
  struct PrewarmCtx {
    const fui::ListItem* items;
    int count;
  } prewarmCtx{windowItems, windowCount};
  renderer.prewarmFallbackText(
      uiScaleSpec().bodyFontId,
      [](const void* ctx, uint32_t i) -> const char* {
        const auto* c = static_cast<const PrewarmCtx*>(ctx);
        return i < static_cast<uint32_t>(c->count) ? c->items[i].label : nullptr;
      },
      &prewarmCtx, static_cast<uint32_t>(windowCount));
}

void EpubReaderSearchActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band drawChrome paints.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (!worker) return;

  const auto& theme = screen.theme();
  fui::ListProps props;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  applyListControlStyle(props, theme);
  // An excerpt gets two lines: enough to show the match with the words around it.
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  refreshWindow(nav.top);
  props.items = windowItems;
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(windowCount);
  screen.list(props);
}

void EpubReaderSearchActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight}, tr(STR_SEARCH));
}

// Back leaves for the page; Confirm searches again from the query row and opens a result anywhere else.
void EpubReaderSearchActivity::drawFooter() {
  const char* confirm = nav.selected == 0 ? tr(STR_SEARCH) : tr(STR_OPEN);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK_TO_BOOK), confirm, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
