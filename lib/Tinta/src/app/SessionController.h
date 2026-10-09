#pragma once

// The review session (PLAN.md 4.5, 4.6, 8.1, 8.2): one controller for every
// exercise format. It owns the Today queue (core::DayQueue), which card is
// showing and which side, the grades, undo, the leech notice, the day's
// totals and the session's part of session.bin. Drawing a card is the job of
// a ui::ExerciseView picked per item (ui/views/ExerciseView.h); the session
// screen (ui/screens/SessionScreen) draws the chrome around it and maps
// input to the calls below.
//
// How each item is asked is core::session::pickFormat's choice (kind,
// maturity, device, the typed-answers setting); the first exercise view that
// accepts the format draws it, and a choice the item cannot fill (too few
// options) falls back to a flashcard.
//
// Writes, in order, for one grade: the journal and the item's record
// (ProgressStore, through DayQueue::answer), then session.bin, then the
// usage log's chunk, then the screen changes. A power cut loses at most the
// grade being written; on the next boot session.bin returns to the same card,
// or, if the cut fell between the journal and session.bin, the queue is
// rebuilt from the store.
//
// The day's totals go to days.bin (core::DayLog) when the session ends, when
// it is set aside for sleep, and when it is dropped.

#include <stdint.h>

#include "core/srs/DayQueue.h"
#include "core/stats/Streak.h"
#include "core/usage/UsageLog.h"
#include "ui/views/ExerciseView.h"

namespace tinta::app {

class App;

// What Home's Today card shows.
struct TodayCounts {
  bool active = false;  // a session is in progress: `left` items remain
  uint16_t due = 0;     // due reviews and items still in their steps
  uint16_t fresh = 0;   // new items the day's limit still allows
  uint16_t left = 0;
};

class SessionController {
 public:
  // Items in one session at most, whatever the Session length setting says
  // (6 bytes each, and 5 in session.bin).
  static constexpr uint16_t kCapacity = 200;
  // The session's part of session.bin, at most.
  static constexpr uint32_t kBlobCap = 20 + core::DayQueue::blobSize(kCapacity);

  explicit SessionController(App& app);

  // Home. Counted by a dry build of the queue (a scan of items.bin), so it
  // agrees with what Start will build; cached until something changes.
  const TodayCounts& todayCounts();
  void countsChanged() { countsValid_ = false; }

  bool active() const { return active_; }
  // Builds today's session; false when there is nothing to study (or the
  // card failed). Logs "[tinta] session start ...".
  bool startToday();
  // A lesson's practice (core/session/Lessons.h) as a practice session;
  // finishing it completes the lesson (App::lessonCompleted).
  bool startLesson(uint16_t lesson);
  // A phrasebook category's phrases as a practice session (PLAN.md M6).
  bool startCategory(uint16_t category);
  // The lesson, or the phrasebook category, a practice session belongs to;
  // -1 for Today (or the other kind of practice).
  int32_t sessionLesson() const;
  int32_t sessionCategory() const;
  // Set by end() when a lesson's practice ran to its last item; else -1.
  int32_t completedLesson() const { return completedLesson_; }

  // The card on screen.
  int32_t currentIndex() const { return queue_.current(); }
  ui::ExerciseView* view() const { return view_; }
  bool revealed() const { return revealed_; }
  // The session's first self-graded card, while its front shows, says how to
  // turn it ("Press any key to show the answer"); later fronts do not, so
  // their reveal only adds ink.
  bool showHint() const { return hintCard_ && !revealed_; }
  // 1-based number of the current card and the session's size so far.
  uint16_t position() const;
  uint16_t total() const;
  // Where each grade sends the current card: [grade - 1]; on the back only.
  const core::IntervalPreview* previews() const { return previews_; }
  // Draws the card again at the profile's text size, if that changed.
  void restyle();

  void reveal();
  // Self-graded: the grade from the back; the next card follows at once.
  void grade(core::Grade grade);
  // Auto-graded (M5): the grade an answer on the front earned. It is stored
  // at once; the card turns to its back to show the result until next().
  void answer(core::Grade grade);
  bool showingResult() const { return showingResult_; }
  void next();
  bool canUndo() const { return active_ && !leechPending_ && queue_.canUndo(); }
  bool undo();

  // A grade made the item a leech: the screen shows the notice and calls
  // resolveLeech() with the learner's choice before the next card.
  bool leechPending() const { return leechPending_; }
  uint32_t leechIndex() const { return leechIndex_; }
  void resolveLeech(bool suspend);

  // Ends the session: totals to days.bin, the summary filled in. `how` is for
  // the usage log: a session that runs out ends itself as Finished.
  void end(core::usage::SessionEndHow how = core::usage::SessionEndHow::Ended);
  // What end() leaves for the summary screen.
  const core::StudyTotals& summaryTotals() const { return summary_; }

  // Sleep: the session stays, its unsaved totals go to days.bin.
  bool setAside();

  // session.bin: what serialize() wrote, given back after a wake. Restored:
  // the same card; Rebuilt: the store moved on since (a cut between the
  // journal and session.bin), so today's queue was built again; Gone:
  // nothing to resume (another day, a corrupt file, nothing left).
  enum class Resume : uint8_t { Restored, Rebuilt, Gone };
  uint32_t serialize(uint8_t* out, uint32_t cap) const;
  Resume restore(const uint8_t* in, uint32_t length);

 private:
  core::SessionLimits limits() const;
  void showCurrent(bool back);
  bool begin();
  bool store(core::Grade grade);
  void addTotals(const core::StudyTotals& t);
  bool flushToLog();
  void logCard() const;
  void recordStart(bool resumed) const;
  void recordShown() const;
  void recordAnswer(uint32_t uid, uint8_t format, core::Grade grade, uint32_t responseMs) const;

  App& app_;
  core::QueueEntry entries_[kCapacity];
  core::DayQueue queue_;
  bool active_ = false;
  bool revealed_ = false;
  bool showingResult_ = false;
  ui::ExerciseView* view_ = nullptr;
  ui::CardInput card_{};
  core::IntervalPreview previews_[4] = {};
  uint32_t shownMs_ = 0;
  bool leechPending_ = false;
  bool resuming_ = false;     // restore() is building the session again after a wake
  bool hintPending_ = false;  // a new session whose first flashcard is still to come
  bool hintCard_ = false;     // the card showing is that one
  uint32_t leechIndex_ = 0;
  int32_t completedLesson_ = -1;
  // Totals already written to days.bin during this session (sleeps), and
  // the summary of the last session that ended.
  core::StudyTotals flushed_{};
  core::StudyTotals summary_{};

  TodayCounts counts_{};
  bool countsValid_ = false;
  core::DayNumber countsDay_ = 0;
};

}  // namespace tinta::app
