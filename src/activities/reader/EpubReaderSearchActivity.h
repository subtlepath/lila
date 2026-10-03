#pragma once
#include <BookSearchResults.h>
#include <BookSearchWorker.h>
#include <Epub.h>

#include <memory>
#include <string>

#include "activities/UiListActivity.h"

// Search inside the open book. The query sits in the first row (Confirm there reopens the keyboard); the
// results follow, each an excerpt over the section it is in and its place in the book. The book is
// searched on a background task, so results appear while it runs and Back leaves at once. Confirm on a
// result hands it to the reader; the reader's Back reopens this list where it was left.
class EpubReaderSearchActivity final : public UiListActivity {
 public:
  // With `showResults` the last search's results open with `selectedResult` selected (Back from a result);
  // otherwise the keyboard opens first, holding `query`.
  explicit EpubReaderSearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Epub> epub,
                                    std::string query, bool showResults, int selectedResult);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override { return worker && worker->running(); }

 private:
  int listCount() const override { return 1 + shownCount; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleButtons() override;
  void drawChrome() override;
  void drawFooter() override;

  void openKeyboard();
  // `selectRow` > 0 selects that row instead of moving to the first result when it arrives.
  void startSearch(const std::string& text, int selectRow = 0);
  void openResult(int resultIndex);
  void leave();
  // Picks up the worker's progress; returns true when the screen should be redrawn.
  bool pollWorker();
  void formatStatus();
  // Rows [first, first + ROW_WINDOW) from the results file, read only when the viewport or count moves.
  void refreshWindow(int firstRow);

  // Enough rows for a screen of one-line excerpts plus the partial preview row.
  static constexpr int ROW_WINDOW = 16;
  // Progress repaints while searching, at most this often (each one is a panel refresh).
  static constexpr unsigned long PROGRESS_REFRESH_MS = 1200;

  std::shared_ptr<Epub> epub;
  std::string query;
  std::string resultsFilePath;
  const bool reopenResults;
  const int initialSelection;
  std::unique_ptr<booksearch::BookSearchWorker> worker;
  // A search has run on this screen, so cancelling the keyboard returns to the list instead of the page.
  bool searched = false;
  // The selection moved to the first result once; after that it is the reader's.
  bool firstResultSelected = false;

  // Snapshot of the worker that the screen shows; written by loop() under the render lock.
  int shownCount = 0;
  booksearch::BookSearchWorker::Status shownStatus = booksearch::BookSearchWorker::Status::Idle;
  uint8_t shownPercent = 0;
  unsigned long lastProgressRefreshMs = 0;
  // Bumped by every new search, so the row window reloads.
  uint32_t searchGeneration = 0;

  char queryLabel[booksearch::QUERY_MAX_BYTES + 8] = {};
  char statusText[96] = {};

  // Row window (render task). Row 0 is the query.
  booksearch::ResultRecord windowRecords[ROW_WINDOW];
  char windowSubtitles[ROW_WINDOW][96] = {};
  freeink::ui::ListItem windowItems[ROW_WINDOW];
  int windowStart = -1;
  int windowCount = 0;
  int windowTotal = 0;
  uint32_t windowGeneration = 0;
};
