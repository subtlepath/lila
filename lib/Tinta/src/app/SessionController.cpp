#include "app/SessionController.h"

#include <Arduino.h>

#include "app/App.h"
#include "core/library/Library.h"
#include "core/session/Exercise.h"
#include "core/session/Lessons.h"
#include "core/srs/Bytes.h"
#include "platform/Log.h"

namespace tinta::app {
namespace {

using core::Grade;
using platform::log;
using Status = core::ProgressStore::Status;
namespace usage = core::usage;

// An answer counts for at most a minute of study: a learner who put the
// device down mid-card was not studying. Simulator builds count every answer
// as two seconds, because their virtual clock races ahead while the firmware
// idles between scripted presses, and screens that show the time studied
// must match their goldens.
constexpr uint32_t kMaxResponseMs = 60000;
constexpr uint32_t kSimulatedResponseMs = 2000;

// The session's part of session.bin (App writes it after the screen stack):
//   0 journal count u32 (the store's, when written)  4 flags u8 (bit 0: back
//   side showing)  5 zero u8  6 flushed reviews u16  8 correct u16  10 new
//   u16  12 ms u32  16 queue length u16  18 zero u16  20 DayQueue blob
constexpr uint32_t kHeader = 20;
constexpr uint8_t kFlagBack = 1;

const char* formatName(const core::session::Format format) {
  static const char* const kNames[] = {"flashcard", "meaning", "word",      "gap",      "article",
                                       "form",      "order",   "type-word", "type-gap", "type-form"};
  const uint8_t i = static_cast<uint8_t>(format);
  return i < sizeof kNames / sizeof kNames[0] ? kNames[i] : "?";
}

// SessionLimits::excluded with "show vulgar words" off.
bool excludeVulgar(void* context, const uint32_t index) {
  const core::pack::Pack& pack = *static_cast<const core::pack::Pack*>(context);
  core::pack::Item item;
  return pack.item(index, item) && core::session::vulgarItem(pack, item);
}

// A lesson practice holds its items, every step return included, within the
// queue's capacity.
constexpr uint16_t kLessonItems = 32;
// The largest phrasebook category has 30 phrases.
constexpr uint16_t kCategoryItems = 48;
constexpr uint16_t kCategoryTag = 0x8000;

const char* kindName(const core::ItemKind kind) {
  static const char* const kNames[] = {"recognise", "produce", "cloze", "conjugation", "gender", "phrase", "order"};
  const uint8_t i = static_cast<uint8_t>(kind);
  return i < sizeof kNames / sizeof kNames[0] ? kNames[i] : "?";
}

uint16_t addSaturating(uint16_t a, uint16_t b) {
  const uint32_t sum = static_cast<uint32_t>(a) + b;
  return sum > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(sum);
}

}  // namespace

SessionController::SessionController(App& app)
    : app_(app), queue_(app.progress(), app.pack(), app.fsrs(), app.clock(), entries_, kCapacity) {}

core::SessionLimits SessionController::limits() const {
  const core::Profile& p = app_.profile();
  core::SessionLimits l;
  l.newPerDay = p.newPerDay;
  l.reviewCap = p.reviewCap;
  l.sessionSize = p.sessionSize;
  // New items come from the lessons unlocked so far (PLAN.md 8.2: the
  // current lesson included), then from the frequency deck.
  l.unlockedThrough = p.unlockedThrough;
  l.first = app_.starredItems(l.firstCount);
  if (!p.showVulgar) {
    l.excluded = excludeVulgar;
    l.excludedContext = const_cast<core::pack::Pack*>(&app_.pack());
  }
  return l;
}

const TodayCounts& SessionController::todayCounts() {
  const core::DayNumber today = app_.clock().today();
  if (countsValid_ && countsDay_ == today) return counts_;
  counts_ = TodayCounts{};
  countsValid_ = true;
  countsDay_ = today;
  if (!app_.packReady()) return counts_;
  if (active_) {
    const core::QueueCounts c = queue_.counts();
    counts_.active = true;
    counts_.left = c.remaining;
    counts_.due = static_cast<uint16_t>(c.dueLeft + c.stepsLeft);
    counts_.fresh = c.newLeft;
    return counts_;
  }
  // A dry run of build() over the whole day: the queue is free while no
  // session is active, and Start builds it again with the session size.
  core::SessionLimits l = limits();
  l.sessionSize = 0;
  if (!queue_.build(l)) {
    log("today day %u: progress unreadable", today);
    return counts_;
  }
  const core::QueueCounts c = queue_.counts();
  counts_.due = static_cast<uint16_t>(c.dueLeft + c.stepsLeft);
  counts_.fresh = c.newLeft;
  log("today day %u due %u new %u", today, counts_.due, counts_.fresh);
  return counts_;
}

bool SessionController::startToday() {
  if (!app_.packReady()) return false;
  app_.pruneStarred();
  flushed_ = core::StudyTotals{};
  leechPending_ = false;
  showingResult_ = false;
  const uint32_t started = millis();
  if (!queue_.build(limits()) || queue_.empty()) {
    active_ = false;
    countsChanged();
    return false;
  }
  log("session built in %lu ms", static_cast<unsigned long>(millis() - started));
  return begin();
}

bool SessionController::startLesson(const uint16_t lesson) {
  if (!app_.packReady()) return false;
  flushed_ = core::StudyTotals{};
  leechPending_ = false;
  showingResult_ = false;
  uint32_t items[kLessonItems];
  const uint16_t count =
      core::session::lessonPractice(app_.pack(), lesson, app_.profile().showVulgar, items, kLessonItems);
  // The tag is the lesson plus one: 0 stays "no lesson".
  if (count == 0 || !queue_.buildPractice(items, count, static_cast<uint16_t>(lesson + 1)) || queue_.empty()) {
    active_ = false;
    return false;
  }
  log("lesson %u practice, %u items", lesson, count);
  return begin();
}

bool SessionController::startCategory(const uint16_t category) {
  if (!app_.packReady()) return false;
  flushed_ = core::StudyTotals{};
  leechPending_ = false;
  showingResult_ = false;
  uint32_t items[kCategoryItems];
  const uint16_t count =
      core::library::categoryItems(app_.pack(), category, app_.profile().showVulgar, items, kCategoryItems);
  if (count == 0 || !queue_.buildPractice(items, count, static_cast<uint16_t>(kCategoryTag | category)) ||
      queue_.empty()) {
    active_ = false;
    return false;
  }
  log("phrasebook %u practice, %u items", category, count);
  return begin();
}

// Practice tags: a lesson's is lesson + 1, a phrasebook category's has the
// top bit set; 0 is no tag.
int32_t SessionController::sessionLesson() const {
  if (queue_.kind() != core::SessionKind::Practice) return -1;
  const uint16_t tag = queue_.tag();
  return tag > 0 && !(tag & kCategoryTag) ? tag - 1 : -1;
}

int32_t SessionController::sessionCategory() const {
  if (queue_.kind() != core::SessionKind::Practice) return -1;
  const uint16_t tag = queue_.tag();
  return (tag & kCategoryTag) ? tag & ~kCategoryTag : -1;
}

bool SessionController::begin() {
  active_ = true;
  completedLesson_ = -1;
  // A session rebuilt after a wake is the same session: no second hint.
  hintPending_ = !resuming_;
  const core::QueueCounts c = queue_.counts();
  log("session start day %u items %u due %u new %u", queue_.day(), c.remaining,
      static_cast<unsigned>(c.dueLeft + c.stepsLeft), c.newLeft);
  recordStart(resuming_);
  showCurrent(false);
  countsChanged();
  app_.saveSession();
  return true;
}

uint16_t SessionController::position() const { return static_cast<uint16_t>(queue_.counts().done + 1); }

uint16_t SessionController::total() const {
  const core::QueueCounts c = queue_.counts();
  return static_cast<uint16_t>(c.done + c.remaining);
}

void SessionController::showCurrent(const bool turned) {
  revealed_ = false;
  view_ = nullptr;
  const int32_t index = queue_.current();
  if (index < 0) return;
  ui::CardInput& card = card_;
  card = ui::CardInput{};
  card.pack = &app_.pack();
  app_.pack().item(static_cast<uint32_t>(index), card.item);
  card.index = static_cast<uint32_t>(index);
  card.size = static_cast<ui::TextSize>(app_.profile().textSize);
  card.touch = !app_.keyDevice();
  card.showVulgar = app_.profile().showVulgar;
  core::ItemState state;
  app_.progress().load(static_cast<uint32_t>(index), state);
  card.reps = state.reps;
  card.format = core::session::resolveFormat(app_.pack(), card.item, core::session::historyOf(state),
                                             card.touch && app_.profile().typedAnswers, card.showVulgar);
  uint8_t count = 0;
  ui::ExerciseView* const* views = ui::exerciseViews(count);
  for (int attempt = 0; attempt < 2 && !view_; ++attempt) {
    for (uint8_t i = 0; i < count; ++i) {
      if (!views[i]->accepts(card)) continue;
      if (views[i]->load(card)) view_ = views[i];
      break;
    }
    // Too few options, or no view for the format: a flashcard asks it.
    if (!view_) card.format = core::session::Format::Flashcard;
  }
  // An auto-graded card is answered on its front, never revealed.
  revealed_ = turned && !(view_ && view_->autoGraded());
  hintCard_ = hintPending_ && !revealed_ && view_ && !view_->autoGraded();
  if (hintCard_) hintPending_ = false;
  if (revealed_) queue_.preview(previews_);
  shownMs_ = millis();
  logCard();
  recordShown();
}

// ── Usage log ────────────────────────────────────────────────────────────────

void SessionController::recordStart(const bool resumed) const {
  const core::QueueCounts c = queue_.counts();
  usage::SessionKind kind = usage::SessionKind::Today;
  uint16_t tag = 0;
  if (sessionLesson() >= 0) {
    kind = usage::SessionKind::Lesson;
    tag = static_cast<uint16_t>(sessionLesson());
  } else if (sessionCategory() >= 0) {
    kind = usage::SessionKind::Phrases;
    tag = static_cast<uint16_t>(sessionCategory());
  }
  app_.usage().sessionStart(kind, tag, c.remaining, static_cast<uint16_t>(c.dueLeft + c.stepsLeft), c.newLeft, resumed);
}

// The card as asked: its format and, for a choice, the options offered.
void SessionController::recordShown() const {
  const int32_t index = queue_.current();
  if (index < 0 || !view_) return;
  const char* options[usage::UsageLog::kMaxOptions] = {};
  uint8_t answer = 0xFF;
  const uint8_t count = view_->optionTexts(options, usage::UsageLog::kMaxOptions, answer);
  core::ItemState state;
  app_.progress().load(static_cast<uint32_t>(index), state);
  app_.usage().itemShown(card_.item.uid, static_cast<uint8_t>(view_->journalFormat()),
                         static_cast<uint8_t>(card_.item.kind), card_.item.lesson, state.reps, options, count, answer);
}

// An auto-graded answer is right, near (a slip, a mistake, the accents:
// graded Hard) or wrong by its grade; a flashcard's grade is the learner's.
void SessionController::recordAnswer(const uint32_t uid, const uint8_t format, const Grade grade,
                                     const uint32_t responseMs) const {
  ui::AnswerDetail detail;
  usage::Correct correct = usage::Correct::SelfGraded;
  if (view_ && view_->autoGraded()) {
    detail = view_->answerDetail();
    correct = grade == Grade::Again  ? usage::Correct::Wrong
              : grade == Grade::Hard ? usage::Correct::Near
                                     : usage::Correct::Right;
  }
  app_.usage().answer(uid, format, detail.chosen, correct, static_cast<uint8_t>(grade), responseMs, detail.attempts,
                      detail.typed);
}

void SessionController::restyle() {
  const auto size = static_cast<ui::TextSize>(app_.profile().textSize);
  if (!view_ || size == card_.size) return;
  card_.size = size;
  view_->load(card_);
}

void SessionController::logCard() const {
  const int32_t index = queue_.current();
  if (index < 0) return;
  log("card %u/%u uid %lu %s %s", position(), total(), static_cast<unsigned long>(app_.pack().uidAt(index)),
      kindName(app_.pack().kindAt(index)), revealed_ ? "back" : "front");
  log("ask %s", formatName(card_.format));
}

void SessionController::reveal() {
  if (!active_ || revealed_ || queue_.empty()) return;
  revealed_ = true;
  queue_.preview(previews_);
  log("reveal");
  app_.usage().reveal(card_.item.uid);
}

// Journals the grade for the card on screen and moves the queue on; false if
// it could not be stored even as a guest.
bool SessionController::store(const Grade grade) {
  const uint32_t started = millis();
  const uint32_t index = static_cast<uint32_t>(queue_.current());
  const uint32_t uid = app_.pack().uidAt(index);
  const uint8_t format = view_ ? static_cast<uint8_t>(view_->journalFormat()) : 0;
  uint32_t responseMs = started - shownMs_;
  if (responseMs > kMaxResponseMs) responseMs = kMaxResponseMs;
  if (platform::kSimulator) responseMs = kSimulatedResponseMs;
  core::AnswerResult r = queue_.answer(grade, format, responseMs);
  if (r.status == Status::Failed && !app_.progress().isGuest()) {
    // The card stopped answering. What reached it stays as it was; this and
    // later grades are kept in RAM only (the screen shows the banner).
    log("grade %lu not stored: the card failed, continuing as a guest", static_cast<unsigned long>(uid));
    app_.reopenProgressAsGuest();
    r = queue_.answer(grade, format, responseMs);
  }
  if (r.status == Status::Failed || r.status == Status::Invalid) {
    log("grade %lu failed", static_cast<unsigned long>(uid));
    return false;
  }
  recordAnswer(uid, format, grade, responseMs);
  if (r.inSession) {
    log("grade %lu %u next session", static_cast<unsigned long>(uid), static_cast<unsigned>(grade));
  } else {
    log("grade %lu %u next %u d", static_cast<unsigned long>(uid), static_cast<unsigned>(grade), r.intervalDays);
  }
  countsChanged();
  if (r.becameLeech) {
    leechPending_ = true;
    leechIndex_ = index;
    log("leech %lu", static_cast<unsigned long>(uid));
  }
  return true;
}

void SessionController::grade(const Grade grade) {
  if (!active_ || queue_.empty() || leechPending_ || showingResult_) return;
  const uint32_t started = millis();
  if (!store(grade)) return;
  if (!leechPending_) {
    if (queue_.empty()) {
      end(usage::SessionEndHow::Finished);
    } else {
      showCurrent(false);
    }
  }
  app_.saveSession();
  app_.usage().flush();
  log("review stored in %lu ms", static_cast<unsigned long>(millis() - started));
}

void SessionController::answer(const Grade grade) {
  if (!active_ || queue_.empty() || leechPending_ || showingResult_ || revealed_) return;
  const uint32_t started = millis();
  if (!store(grade)) return;
  // The view still holds the card just answered: its back is the result.
  showingResult_ = true;
  revealed_ = true;
  app_.saveSession();
  app_.usage().flush();
  log("review stored in %lu ms", static_cast<unsigned long>(millis() - started));
}

void SessionController::next() {
  if (!showingResult_ || leechPending_) return;
  showingResult_ = false;
  if (queue_.empty()) {
    end(usage::SessionEndHow::Finished);
  } else {
    showCurrent(false);
  }
}

bool SessionController::undo() {
  if (!canUndo() || !queue_.undo()) return false;
  log("undo");
  if (queue_.current() >= 0) app_.usage().undo(app_.pack().uidAt(static_cast<uint32_t>(queue_.current())));
  countsChanged();
  showingResult_ = false;
  // Back to the card just graded: self-graded with the answer showing, to
  // grade it again; auto-graded on its front, to answer it again (the same
  // options: they follow the repetition count, which the undo restored).
  showCurrent(true);
  app_.saveSession();
  app_.usage().flush();
  return true;
}

void SessionController::resolveLeech(const bool suspend) {
  if (!leechPending_) return;
  leechPending_ = false;
  showingResult_ = false;
  if (suspend) {
    const Status s = queue_.suspend(leechIndex_);
    log("leech %lu %s", static_cast<unsigned long>(app_.pack().uidAt(leechIndex_)),
        s == Status::Stored ? "suspended" : "not suspended");
  }
  countsChanged();
  if (queue_.empty()) {
    end(usage::SessionEndHow::Finished);
  } else {
    showCurrent(false);
  }
  app_.saveSession();
  if (suspend) app_.usage().flush();
}

void SessionController::addTotals(const core::StudyTotals& t) {
  flushed_.reviews = addSaturating(flushed_.reviews, t.reviews);
  flushed_.correct = addSaturating(flushed_.correct, t.correct);
  flushed_.newItems = addSaturating(flushed_.newItems, t.newItems);
  flushed_.milliseconds += t.milliseconds;
}

bool SessionController::flushToLog() {
  if (app_.dayLog().hasUncertainWrite()) return false;
  const core::StudyTotals t = queue_.takeTotals();
  addTotals(t);
  if (t.reviews == 0) return true;
  core::DayTotals day;
  day.reviews = t.reviews;
  day.correct = t.correct;
  day.newItems = t.newItems;
  day.seconds = (t.milliseconds + 500) / 1000;
  if (!app_.dayLog().addChecked(queue_.day(), day)) {
    log("days.bin not written: authoritative recovery required");
    return false;
  }
  return true;
}

void SessionController::end(const usage::SessionEndHow how) {
  if (!active_) return;
  // Starred words this session brought in are done.
  app_.pruneStarred();
  const int32_t lesson = sessionLesson();
  completedLesson_ = lesson >= 0 && queue_.empty() ? lesson : -1;
  flushToLog();
  summary_ = flushed_;
  flushed_ = core::StudyTotals{};
  active_ = false;
  leechPending_ = false;
  showingResult_ = false;
  view_ = nullptr;
  countsChanged();
  log("session end reviewed %u correct %u new %u seconds %lu", summary_.reviews, summary_.correct, summary_.newItems,
      static_cast<unsigned long>((summary_.milliseconds + 500) / 1000));
  app_.usage().sessionEnd(how, summary_.reviews, summary_.correct, (summary_.milliseconds + 500) / 1000);
  if (completedLesson_ >= 0 && !app_.lessonCompleted(static_cast<uint16_t>(completedLesson_))) completedLesson_ = -1;
  if (!platform::kSimulator) {
    log("heap free %lu, lowest %lu", static_cast<unsigned long>(app_.board().freeHeap()),
        static_cast<unsigned long>(app_.board().minFreeHeap()));
  }
}

bool SessionController::setAside() {
  if (app_.dayLog().hasUncertainWrite()) return false;
  if (!active_) return true;
  if (!flushToLog()) return false;
  // It may resume after the wake; the usage log sees a new start then.
  app_.usage().sessionEnd(usage::SessionEndHow::Slept, flushed_.reviews, flushed_.correct,
                          (flushed_.milliseconds + 500) / 1000);
  return true;
}

uint32_t SessionController::serialize(uint8_t* out, const uint32_t cap) const {
  if (!active_ || cap < kHeader) return 0;
  const uint32_t blob = queue_.serialize(out + kHeader, cap - kHeader);
  if (blob == 0) return 0;
  core::putU32(out, app_.progress().journalCount());
  // An answer's result belongs to the card just graded; the queue is already
  // on the next one, which comes back on its front.
  out[4] = revealed_ && !showingResult_ ? kFlagBack : 0;
  out[5] = 0;
  core::putU16(out + 6, flushed_.reviews);
  core::putU16(out + 8, flushed_.correct);
  core::putU16(out + 10, flushed_.newItems);
  core::putU32(out + 12, flushed_.milliseconds);
  core::putU16(out + 16, static_cast<uint16_t>(blob));
  core::putU16(out + 18, 0);
  return kHeader + blob;
}

SessionController::Resume SessionController::restore(const uint8_t* in, const uint32_t length) {
  active_ = false;
  leechPending_ = false;
  showingResult_ = false;
  countsChanged();
  if (!app_.packReady() || length < kHeader) return Resume::Gone;
  const uint32_t blob = core::getU16(in + 16);
  if (kHeader + blob > length) return Resume::Gone;
  if (!queue_.restore(in + kHeader, blob) || (queue_.empty() && !app_.pendingSessionRequiresRebuild())) {
    log("session not resumed: another day, or nothing left");
    return Resume::Gone;
  }
  // The course pack lives on the card and can be replaced while Tinta is
  // closed: a practice of a lesson or phrase category it no longer has does
  // not come back.
  const int32_t lesson = sessionLesson();
  const int32_t category = sessionCategory();
  if ((lesson >= 0 && lesson >= app_.lessonCount()) ||
      (category >= 0 && static_cast<uint32_t>(category) >= app_.pack().count(core::pack::Section::Phrs))) {
    log("session not resumed: its lesson or category is not in this course");
    queue_.restore(in, 0);
    return Resume::Gone;
  }
  flushed_.reviews = core::getU16(in + 6);
  flushed_.correct = core::getU16(in + 8);
  flushed_.newItems = core::getU16(in + 10);
  flushed_.milliseconds = core::getU32(in + 12);
  if (app_.pendingSessionRequiresRebuild() || core::getU32(in) != app_.progress().journalCount()) {
    // The saved queue belongs to earlier authority. Build from current schedules.
    const core::StudyTotals kept = flushed_;
    log("session authority changed; rebuilding today's session");
    resuming_ = true;
    const bool rebuilt = startToday();
    resuming_ = false;
    if (!rebuilt) return Resume::Gone;
    flushed_ = kept;
    return Resume::Rebuilt;
  }
  active_ = true;
  recordStart(true);
  showCurrent((in[4] & kFlagBack) != 0);
  log("session resumed");
  return Resume::Restored;
}

}  // namespace tinta::app
