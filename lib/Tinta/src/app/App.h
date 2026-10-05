#pragma once

// The app shell (PLAN.md 6.4, 6.5, 4.6): opening, the screen stack, input
// dispatch, the refresh policy and settings persistence. Inside lila an
// activity (src/activities/tinta/TintaActivity) owns one App for as long as
// Tinta is open: update() on lila's loop task and renderFrame() on its render
// task, never at once (lila's render lock). Sleep and power are lila's.
//
// Refresh policy:
// - Render only when something changed (FreeInkApp::invalidated()).
// - Screen changes use invalidateTransition(): fast, with a full refresh every
//   Nth one (the profile's fullRefreshEvery); going Home and the first frame
//   after boot are full.
// - FreeInkApp keeps the strongest hint asked for until the next render;
//   input that arrives while the panel is busy is routed against the frame on
//   screen and lands in one later repaint.

#include <FreeInkUIDisplayTarget.h>
#include <stdint.h>

#include <atomic>

class GfxRenderer;

#include "app/SessionController.h"
#include "app/View.h"
#include "core/library/MarkLog.h"
#include "core/pack/Pack.h"
#include "core/profile/Profile.h"
#include "core/srs/Fsrs.h"
#include "core/srs/ProgressStore.h"
#include "core/stats/DayLog.h"
#include "core/usage/UsageLog.h"
#include "platform/Board.h"
#include "platform/Clock.h"
#include "platform/PackFile.h"
#include "platform/Power.h"
#include "platform/StateFiles.h"
#include "ui/KeyMap.h"
#include "ui/Theme.h"

namespace tinta::app {

struct Screens;

class App {
 public:
  explicit App(GfxRenderer& renderer);
  ~App();
  App(const App&) = delete;
  App& operator=(const App&) = delete;

  // Allocates the screens and buffers, opens the course and the learner's
  // files and picks the first screen. False when memory ran out; the App can
  // then only be destroyed.
  bool open();
  // Writes everything held in RAM (as going to sleep does) and closes the
  // files. Before lila sleeps or goes elsewhere.
  void close();
  // lila is going to sleep and keeps the current frame on the glass: draws
  // the sleep card (one of the weakest words, PLAN.md 4.7) into the frame
  // buffer without presenting it, and moves the next sleep on to another
  // word. False, with the frame untouched, when there is no word to show.
  // Before close().
  bool drawSleepCard();
  // Loop task: input, the clock, saves.
  void update();
  // True when a frame waits to be drawn.
  bool frameWanted() const;
  // Render task: draws the frame and presents it, waiting for the panel.
  // `repaint`: something else drew on the panel since the last frame (an
  // activity on top), so the frame is drawn whole with a screen refresh.
  void renderFrame(bool repaint);

  // Back on Home, or Home's Leave row: the activity hands over to lila.
  void requestExit() {
    exitRequested_ = true;
    ++changes_;
  }
  bool exitRequested() const { return exitRequested_; }
  // Something only lila can show: the activity opens it over Tinta.
  enum class HostRequest : uint8_t { None, Light };
  HostRequest takeHostRequest() {
    const HostRequest r = hostRequest_;
    hostRequest_ = HostRequest::None;
    return r;
  }

  platform::Board& board() { return board_; }
  platform::Clock& clock() { return clock_; }
  platform::StateFiles& storage() { return storage_; }
  const ui::Theme& theme() const { return theme_; }
  const ui::KeyMap& keys() const { return keys_; }
  freeink::ui::DisplayTarget& target() { return *target_; }
  bool keyDevice() const { return board_.hasFrontKeys(); }

  // Settings. Call profileChanged() after editing: it applies the change
  // (language, refresh cadence, clock, light) and saves the profile once the
  // current burst of input has been handled.
  core::Profile& profile() { return profile_; }
  void profileChanged();
  // True when profile.bin did not exist at boot.
  bool firstRun() const { return firstRun_; }

  // The usage log (PLAN.md 8.6, docs/usage-log.md). Screens record what the
  // learner does through it; App flushes it after journal writes, at sleep
  // and when its buffer fills, never while drawing.
  core::usage::UsageLog& usage() { return usage_; }

  // The course and the learner's progress (PLAN.md 6.6, 8.3). The pack is
  // opened at boot with the cheap structural checks only; without it there
  // is no Home, only the pack error screen.
  const core::pack::Pack& pack() const { return pack_; }
  bool packReady() const { return pack_.isOpen(); }
  core::pack::PackStatus packStatus() const { return packStatus_; }
  const char* packStatusName() const;
  core::ProgressStore& progress() { return progress_; }
  core::DayLog& dayLog() { return dayLog_; }
  const core::Fsrs& fsrs() const { return fsrs_; }
  SessionController& session() { return session_; }
  // The store stopped answering: reopens progress as a guest (RAM only), so
  // a session can go on without overwriting anything on the card.
  void reopenProgressAsGuest();
  // Writes session.bin now: the screens to return to and the review session.
  void saveSession();
  // Writes everything held in RAM, as going to sleep does: the session's day
  // totals so far, session.bin (with the stack up to the first screen that is
  // not restorable), the profile if it changed and the usage log.
  void flush();
  const platform::BatteryReading& battery() const { return battery_; }

  // "Add to my deck" (PLAN.md M6): the uids of recognise items starred from
  // the reader or the dictionary, introduced ahead of everything by the next
  // Today session, and the readings finished (core/library/Library storyKey).
  core::library::MarkLog& starred() { return starred_; }
  core::library::MarkLog& readLog() { return readLog_; }
  enum class DeckState : uint8_t {
    None,     // nothing to learn: a dictionary-only word
    Learnt,   // its recognise item has been graded
    Starred,  // in the deck for the next session
    Open,     // can be starred
  };
  DeckState deckState(uint16_t lemma);
  // Stars or unstars a word; false when it cannot be (None, Learnt, a full deck).
  bool toggleStar(uint16_t lemma);
  // Drops starred words that are no longer new (introduced, or learnt some
  // other way) and those the pack no longer has. Before each Today session.
  void pruneStarred();
  // The starred words' item indices, in the order they were starred.
  const uint32_t* starredItems(uint16_t& count);

  // Lessons (PLAN.md 4.1, M5). profile.currentLesson is the first lesson not
  // yet completed; profile.unlockedThrough the last one that may be studied
  // (and whose items the daily session may introduce).
  enum class LessonState : uint8_t { Done, Current, Open, Locked };
  LessonState lessonState(uint16_t lesson) const;
  uint16_t lessonCount() const { return static_cast<uint16_t>(pack_.count(core::pack::Section::Less)); }
  // A lesson's practice ran to the end: the next one is unlocked.
  void lessonCompleted(uint16_t lesson);
  void unlockAllLessons();

  // Navigation. Each change is a counted transition (see setTransitionFullEvery).
  void push(ScreenId id);
  void pop();
  // Replaces the whole stack, bottom first.
  void resetTo(const ScreenId* ids, uint8_t count, freeink::ui::RefreshHint hint = freeink::ui::RefreshHint::Fast);
  void goHome();
  // Home, or the pack error screen when there is no course.
  ScreenId rootId() const { return packReady() ? ScreenId::Home : ScreenId::PackError; }
  void openPause();
  void openLight();
  ScreenId topId() const { return depth_ ? stack_[depth_ - 1] : ScreenId::None; }
  ScreenId idAt(uint8_t index) const { return index < depth_ ? stack_[index] : ScreenId::None; }
  uint8_t depth() const { return depth_; }
  View* view(ScreenId id);

  // True while the boot-time date or clock step is showing (the date prompt,
  // or a picker opened from it at boot).
  bool inTimeStep() const;
  // The boot-time date step is done: go on to the saved screen or Home.
  void finishTimeStep();
  // Asks for the date (no trusted clock: platform/Clock), then goes on to `then` (bottom first), or at boot to the
  // screens saved at sleep. The first run asks for the language first, so it starts the step itself.
  void beginTimeStep(const ScreenId* then = nullptr, uint8_t count = 0);

  void invalidate() {
    window_ = freeink::ui::Rect{};
    ui_->invalidate(freeink::ui::RefreshHint::Fast);
    ++changes_;
  }
  // A change confined to `rect` (the reader's word cursor): presented as a
  // window of the panel where the board can (Board::presentWindow, the X4),
  // otherwise a fast refresh like invalidate(). Several calls before the
  // next frame widen the window; any other invalidation drops it.
  void invalidateWindow(freeink::ui::Rect rect);
  // The content changed as much as a new screen would (the next card, a
  // page): a new screen's refresh (Board::screenRefresh), counted toward the
  // full refresh every Nth transition.
  // Within a card, invalidate() is fast: a reveal only adds ink.
  void invalidateCard() {
    window_ = freeink::ui::Rect{};
    ui_->invalidateTransition();
    screenChanged_ = true;
    ++changes_;
  }
  // Call from an action handler that navigates: the tapped element's gray
  // flash must not land on the next screen.
  void clearTapFlash() { ui_->clearTapFlash(); }

 private:
  static constexpr uint8_t kMaxDepth = 8;

  static void buildFrame(UiScreen& screen, void* self);
  void compose(UiScreen& screen);
  void composeNormal(UiScreen& screen, View& view);
  void drawFooter(UiScreen& screen, View& view, bool registerTaps);

  void handle(const ui::InputEvent& event);
  void dispatch(const ui::InputEvent& event);
  void logInput(const ui::InputEvent& event);
  void recordInput(const ui::InputEvent& event, uint32_t changesBefore, bool duringRefresh);
  void transition(freeink::ui::RefreshHint hint, uint8_t how);
  void applyLanguage();
  void startUsageLog();
  static uint32_t uptimeMs();

  void pollPeriodic();
  void saveIfDirty();
  uint8_t loadResumeStack(ScreenId* out, uint8_t cap);
  void openCourse();
  void closeCourse();
  core::DayNumber lastJournalDay();
  void resumeSession();

  platform::Board board_;
  platform::Power power_;
  platform::StateFiles storage_;
  platform::Clock clock_;
  freeink::ui::DisplayTarget* target_ = nullptr;
  freeink::ui::DeviceContext device_{};
  // Built in place by open(), once the framebuffer is known.
  alignas(freeink::ui::DisplayTarget) unsigned char targetStorage_[sizeof(freeink::ui::DisplayTarget)];
  alignas(UiApp) unsigned char uiStorage_[sizeof(UiApp)];
  UiApp* ui_ = nullptr;
  // An overlay draws the screen beneath it through a throwaway interaction
  // table, so only the overlay's own controls take input.
  freeink::ui::InteractionBuffer<kMaxInteractions> scratch_;
  // One instance of each screen, made by open() (app/Screens.cpp).
  Screens* screens_ = nullptr;
  std::atomic<const freeink::ui::ThemeTokens*> themeRef_{nullptr};
  ui::Theme theme_{};
  ui::KeyMap keys_;
  core::Profile profile_{};
  bool profileDirty_ = false;
  bool firstRun_ = false;
  core::usage::UsageLog usage_{storage_, clock_, uptimeMs};

  core::pack::Pack pack_;
  core::pack::PackStatus packStatus_ = core::pack::PackStatus::TooSmall;
  // The pack file's block cache and the string arena: heap, while open.
  static constexpr uint8_t kPackBlocks = 8;
  static constexpr uint32_t kArenaBytes = 6 * 1024;
  platform::PackFile packFile_;
  bool packFound_ = true;  // false: no /tinta/course.pack
  uint8_t* packBlocks_ = nullptr;
  char* arena_ = nullptr;
  uint8_t progressOpened_ = 0;  // ProgressStore::OpenResult
  core::Fsrs fsrs_;
  core::ProgressStore progress_{storage_, pack_, fsrs_};
  core::DayLog dayLog_{storage_};
  // One slot per pack item (2 bytes each), on the heap while open: the only
  // allocation that grows with the course. Guests keep their records here
  // instead.
  uint16_t* slots_ = nullptr;
  uint32_t slotCount_ = 0;
  static constexpr uint16_t kGuestCapacity = 128;
  core::ItemState guest_[kGuestCapacity] = {};
  SessionController session_{*this};
  core::library::MarkLog starred_{storage_, "starred.bin"};
  core::library::MarkLog readLog_{storage_, "read.bin"};
  uint32_t starredItems_[core::library::MarkLog::kCapacity] = {};

  // session.bin: magic, version, the screen stack, the review session, CRC.
  static constexpr uint32_t kSessionFileCap = 4 + 2 + 1 + kMaxDepth + 2 + SessionController::kBlobCap + 4;
  uint8_t sessionFile_[kSessionFileCap] = {};
  // The review session read at boot, inside sessionFile_, until the screens
  // are restored.
  uint32_t pendingSessionAt_ = 0;
  uint32_t pendingSessionLength_ = 0;

  ScreenId stack_[kMaxDepth] = {};
  uint8_t depth_ = 0;
  // The screens to return to once the boot-time date step is done.
  ScreenId resume_[kMaxDepth] = {};
  uint8_t resumeDepth_ = 0;
  // finishTimeStep() is putting back the screens saved at sleep, or (the
  // first run) going on to the screen beginTimeStep() was given.
  bool restoring_ = false;
  bool timeStepThen_ = false;

  // Set by a screen change until the new screen has been drawn.
  bool focusPending_ = false;
  bool framePending_ = false;
  freeink::ui::RefreshHint pendingHint_ = freeink::ui::RefreshHint::None;
  freeink::ui::Rect window_{};  // invalidateWindow()'s, empty when the frame is whole
  bool screenChanged_ = false;
  uint16_t frameCount_ = 0;
  bool opened_ = false;
  bool exitRequested_ = false;
  HostRequest hostRequest_ = HostRequest::None;

  // For the usage log: bumped by everything that changes what is on screen,
  // so an input that left it unchanged was a dead press.
  uint32_t changes_ = 0;

  platform::BatteryReading battery_{};
  platform::BatteryReading lastBatteryPoll_{};
  uint32_t lastBatteryPollMs_ = 0;
  int16_t shownMinute_ = -1;
};

}  // namespace tinta::app
