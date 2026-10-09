#pragma once
#include <Epub.h>
#include <I18n.h>
#if LILA_COMPANION
#include "CompanionBookmarkChoicePage.h"
#include "CompanionReaderBookmarks.h"
#endif

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "../../BookmarkEntry.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class EpubReaderBookmarksActivity final : public UiListActivity {
  // The list rides the UiListActivity scaffold (themed rows, touch routing);
  // the title keeps its legacy draw, and OptionPopup keeps its legacy overlay
  // rendering for the delete confirmation.
  std::shared_ptr<Epub> epub;
  std::string epubPath;
  std::vector<BookmarkEntry> bookmarks;
  StrId pendingBookmarkError = StrId::_COUNT;
#if LILA_COMPANION
  companion::ReaderBookmarkBinding bookmarkBinding;
  bool associationUi = false;
  void restoreBookmarkList(const RenderLock& lock);
  void rebuildAssociationRows();
  void selectAssociation(int index);
  struct ConflictUi {
    static constexpr size_t SUBTITLE_BYTES = 640;
    companion::NativeBookmarkChoicePage page;
    std::array<std::array<char, BookmarkEntry::MAX_NAME_LENGTH + 1>, companion::NativeBookmarkChoicePage::CAPACITY>
        labels{};
    std::array<std::array<char, SUBTITLE_BYTES>, companion::NativeBookmarkChoicePage::CAPACITY> subtitles{};
    uint32_t offset = 0;
  };
  static_assert(sizeof(ConflictUi) <= 6400);
  std::unique_ptr<ConflictUi> conflictUi;
  bool loadConflictChoices(uint32_t offset);
  void rebuildConflictRows();
  void appendConflictRow(uint32_t index);
  void formatConflictSubtitle(uint32_t index, const companion::BookmarkBodyView& body);
  void selectConflictChoice(int index);
  void closeConflictChoices();
#endif
  // Row buffers derived from `bookmarks`, rebuilt only when it changes
  // (onEnter() load, post-delete) instead of on every repaint — buildScreen()
  // used to re-compose a percentage/chapter/TOC-title subtitle string per
  // bookmark on every render (cursor move, tap flash, ...).
  std::vector<std::string> bookmarkSubtitles;
  std::vector<freeink::ui::ListItem> bookmarkRowItems;
  void rebuildBookmarkRowItems();
  std::string bookmarkSubtitle(const BookmarkEntry& bookmark) const;
  bool confirmingDelete = false;
  OptionPopup confirmPopup;

 public:
  explicit EpubReaderBookmarksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::shared_ptr<Epub>& epub, const std::string& epubPath);
  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  int listCount() const override {
#if LILA_COMPANION
    if (!bookmarkBinding.ready || conflictUi || associationUi) return static_cast<int>(bookmarkRowItems.size());
#endif
    return static_cast<int>(bookmarks.size());
  }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  // Popup handling runs before everything else each pass.
  bool handleCustomInput() override;
  // Back cancels with a result; Confirm opens on release and a hold shows actions.
  bool handleButtons() override;

  // Open the selected bookmark: finishes with a ProgressChangeResult for the reader.
  void openSelectedBookmark();

  void startRename();
  void showBookmarkActions();

  // Opens the Cancel/Delete confirmation for the selected bookmark.
  void showDeleteConfirmation();

  // Delete the currently selected bookmark and persist the list
  void deleteSelectedBookmark();
};
