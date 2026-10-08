#include "app/App.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <new>

#include "core/library/Library.h"
#include "core/library/SleepWord.h"
#include "core/session/SessionFile.h"
#include "core/srs/Bytes.h"
#include "icons/Icons.h"
#include "platform/Log.h"
#include "ui/Strings.h"
#include "ui/TextBuffers.h"
#include "ui/screens/SleepScreen.h"
#include "ui/views/CardText.h"
#include "ui/views/Chrome.h"
#include "ui/views/ExerciseView.h"

namespace tinta::app {
namespace {

using freeink::ui::Color;
using freeink::ui::RefreshHint;
using platform::log;
namespace usage = core::usage;

constexpr uint32_t kBatteryPollMs = 30000;
constexpr uint32_t kSaveDelayMs = 1000;

// session.bin: what to return to after a wake or a power cut.
//   0 magic "TSES"  4 version u16  6 depth u8  7 ids[depth] u8
//   version 2 then: session length u16, the review session (SessionController)
//   version 3 appends the verified learner snapshot SHA-256
//   then crc32 of everything before it
// Version 1 (M2) had no session part. The file is rewritten in place after
// every grade, so it may be longer than its content; the CRC sits where the
// lengths say.
constexpr const char* kSessionFile = "session.bin";
constexpr uint8_t kSessionMagic[4] = {'T', 'S', 'E', 'S'};
constexpr uint16_t kSessionVersion = 3;

bool isTimeStep(const ScreenId id) { return id == ScreenId::DatePrompt || id == ScreenId::DatePicker; }

// The Auto interface language turns Spanish once Unit 4 is behind the
// learner (PLAN.md 4.1).
constexpr uint16_t kAutoSpanishAfterUnit = 4;

// usage::Input for an input event.
usage::Input usageInput(const ui::InputEvent& event) {
  using ui::InputEvent;
  if (event.kind == InputEvent::Kind::Tap) return usage::Input::Tap;
  if (event.kind == InputEvent::Kind::Swipe) return usage::Input::Swipe;
  switch (event.key) {
    case ui::Key::Back:
      return event.hold ? usage::Input::BackHold : usage::Input::Back;
    case ui::Key::Confirm:
      return usage::Input::Confirm;
    case ui::Key::Left:
      return usage::Input::Left;
    case ui::Key::Right:
      return usage::Input::Right;
    case ui::Key::Up:
      return usage::Input::Up;
    case ui::Key::Down:
      return usage::Input::Down;
    case ui::Key::Power:
      return usage::Input::Power;
    case ui::Key::Home:
      return event.hold ? usage::Input::HomeHold : usage::Input::Home;
  }
  return usage::Input::Back;
}

}  // namespace

// ── Opening and closing ──────────────────────────────────────────────────────

Screens* createScreens(App& app);
void destroyScreens(Screens* screens);

App::App(GfxRenderer& renderer) : board_(renderer) {}

App::~App() {
  close();
  destroyScreens(screens_);
  ui::closeExerciseViews();
  ui::closeCardText();
  ui::closeTextBuffers();
}

bool App::setLearnerPreparation(LearnerPreparation preparation) {
  if (opened_) {
    log("learner preparation: app already open");
    return false;
  }
  learnerPreparation_ = preparation;
  return true;
}

bool App::setVerifiedLearnerSnapshot(std::span<const uint8_t, 32> digest) {
  if (opened_ || std::all_of(digest.begin(), digest.end(), [](uint8_t byte) { return byte == 0; })) {
    log("session snapshot: invalid binding or already open");
    return false;
  }
  std::copy(digest.begin(), digest.end(), learnerSnapshot_);
  learnerSnapshotBound_ = true;
  return true;
}

bool App::open(const uint8_t* courseIdentity) {
  power_.begin();
  // The screens and the shared text buffers first: the biggest blocks, while
  // the heap is least broken up. Without them there is nothing to show.
  screens_ = createScreens(*this);
  if (!screens_ || !ui::openTextBuffers() || !ui::openCardText() || !ui::openExerciseViews()) {
    log("open: no memory for the screens");
    return false;
  }
#if LILA_TINTA
  if (courseIdentity) {
    if (!storage_.beginCourse(std::span<const uint8_t, 16>(courseIdentity, 16))) {
      log("open: course state unavailable");
      return false;
    }
  } else
#endif
    storage_.begin();

  if (!lessonCompletion_.recover(storage_.available())) {
    log("open: authoritative lesson recovery failed");
    return false;
  }
  const core::Profile::LoadResult loaded = profile_.load(storage_);
  static const char* const kLoadNames[] = {"loaded", "upgraded", "defaults", "corrupt"};
  log("profile %s", kLoadNames[static_cast<uint8_t>(loaded)]);
  firstRun_ = loaded == core::Profile::LoadResult::Defaults;
  // A corrupt file is left alone until the learner changes a setting.
  profileDirty_ = loaded == core::Profile::LoadResult::Upgraded;
  // Settings > Study > Record usage arrives with its profile field; on until then.
  usage_.open(true);

  if (!openCourse()) {
    closeCourse();
    storage_.release();
    log("open: authoritative learner recovery failed");
    return false;
  }

  clock_.begin(board_.hasRtc());
  // An RTC reading before the last day the device saw (or studied) is not
  // trusted: the app asks for the date before scheduling anything.
  core::DayNumber lastSeen = profile_.lastConfirmedDay;
  const core::DayNumber lastReview = lastJournalDay();
  if (lastReview > lastSeen) lastSeen = lastReview;
  clock_.configure(profile_.rolloverHour, lastSeen);
  applyLanguage();

  // Every panel is landscape-native and Tinta is always held tall.
  target_ = new (targetStorage_) freeink::ui::DisplayTarget(
      board_.frameBuffer(), static_cast<int16_t>(board_.panelWidth()), static_cast<int16_t>(board_.panelHeight()),
      static_cast<int16_t>(board_.panelWidthBytes()), freeink::ui::Orientation::Portrait);
  freeink::ui::DisplayTarget& target = *target_;
  device_ = target.deviceContext();
  device_.hasTouch = board_.hasTouch();
  device_.hasButtons = board_.hasFrontKeys();
  const platform::Insets bezel = board_.bezelInsets();
  device_.safeArea = freeink::ui::Insets{bezel.top, bezel.right, bezel.bottom, bezel.left};
  device_.minTouchSize = device_.hasTouch ? 48 : 44;
  ui::buildTheme(theme_, target, device_);

  ui_ = new (uiStorage_) UiApp(target, device_);
  themeRef_.store(&theme_.tokens);
  ui_->setThemeRef(&themeRef_);
  ui_->setClearColor(Color::White);
  ui_->setTransitionFullEvery(profile_.fullRefreshEvery);

  keys_.begin(board_, device_);
  char order[48];
  keys_.describeOrder(order, sizeof order);
  log("keymap %s", order);

  battery_ = board_.readBattery();
  lastBatteryPoll_ = battery_;
  lastBatteryPollMs_ = millis();
  startUsageLog();
  opened_ = true;

  resumeDepth_ = loadResumeStack(resume_, kMaxDepth);
  if (firstRun_ && storage_.available() && packReady()) {
    // A new card: the first run (ui/screens/WelcomeScreens) asks for the
    // language first and runs the time step itself. Without a card nothing
    // would be kept, so a guest goes straight to the time step and Home.
    resumeDepth_ = 0;
    pendingSessionLength_ = 0;
    // The day is noted before the first frame, as finishTimeStep() does, so
    // the first poll neither redraws the page nor saves the profile: only
    // choosing the language may end the first run.
    if (clock_.trusted() && clock_.hasTimeOfDay()) profile_.lastConfirmedDay = clock_.today();
    clock_.pollDayChange();
    const ScreenId first = ScreenId::Welcome;
    resetTo(&first, 1, RefreshHint::Full);
  } else if (!clock_.trusted()) {
    // The X4 asks for the date once per power-on (platform/Clock keeps it
    // while lila runs); an RTC that cannot be trusted asks for the clock.
    beginTimeStep();
  } else {
    finishTimeStep();
  }
  return true;
}

void App::close() {
  if (!opened_) return;
  opened_ = false;
  if (depth_ > 0) view(topId())->leave();
  flush();
  storage_.release();
  log("closed: arena peak %lu, %lu strings did not fit", static_cast<unsigned long>(pack_.arenaPeak()),
      static_cast<unsigned long>(pack_.arenaOverflows()));
  closeCourse();
}

// The word is chosen by the day and how many times the device has slept, so
// it changes from one sleep to the next.
bool App::drawSleepCard() {
  if (!opened_ || !packReady()) return false;
  pack_.beginPass();
  const core::DayNumber today = clock_.today();
  const uint32_t seed = static_cast<uint32_t>(today) * 7u + profile_.sleepCount;
  const core::library::SleepWord word =
      core::library::pickSleepWord(pack_, progress_, fsrs_, today, seed, profile_.currentLesson);
  core::pack::Item item;
  core::pack::Lemma lemma;
  if (word.item < 0 || !pack_.item(static_cast<uint32_t>(word.item), item) || !pack_.lemma(item.a, lemma)) {
    log("sleep: no word");
    return false;
  }
  ui::SleepInfo info;
  info.dayKnown = clock_.trusted();
  info.day = today;
  info.pack = &pack_;
  info.lemma = item.a;
  info.learnt = word.learnt;
  if (info.dayKnown) {
    core::StreakInfo streak;
    info.streak = core::currentStreak(dayLog_, today, streak) ? streak.days : 0;
    info.dueTomorrow = core::library::dueBy(progress_, today, static_cast<core::DayNumber>(today + 1));
    info.countsKnown = true;
  }
  ui::drawSleepScreen(*target_, theme_, info);
  log("sleep word %s%s", pack_.str(lemma.es), word.learnt ? "" : " (new)");
  // Saved by close().
  ++profile_.sleepCount;
  profileDirty_ = true;
  return true;
}

bool App::inTimeStep() const { return depth_ > 0 && isTimeStep(stack_[0]); }

void App::beginTimeStep(const ScreenId* then, const uint8_t count) {
  timeStepThen_ = count > 0;
  if (timeStepThen_) {
    resumeDepth_ = count < kMaxDepth ? count : kMaxDepth;
    for (uint8_t i = 0; i < resumeDepth_; ++i) resume_[i] = then[i];
  }
  // Without a trusted clock Tinta asks for the date once per power-on; the
  // very first time there is no previous date to offer, so it opens the picker.
  const ScreenId first = profile_.lastConfirmedDay == 0 ? ScreenId::DatePicker : ScreenId::DatePrompt;
  resetTo(&first, 1, frameCount_ == 0 ? RefreshHint::Full : RefreshHint::Fast);
}

void App::finishTimeStep() {
  if (clock_.hasTimeOfDay() && clock_.today() != profile_.lastConfirmedDay) {
    profile_.lastConfirmedDay = clock_.today();
    profileDirty_ = true;
  }
  clock_.pollDayChange();
  // The review session can only come back once the day is known.
  resumeSession();
  if (resumeDepth_ > 0 && packReady()) {
    restoring_ = !timeStepThen_;
    resetTo(resume_, resumeDepth_, RefreshHint::Full);
    restoring_ = false;
    resumeDepth_ = 0;
    if (session_.active()) saveSession();
  } else {
    resumeDepth_ = 0;
    const ScreenId root = rootId();
    resetTo(&root, 1, RefreshHint::Full);
  }
}

void App::resumeSession() {
  uint8_t at = 0;
  while (at < resumeDepth_ && resume_[at] != ScreenId::Session) ++at;
  if (at == resumeDepth_) return;
  using Resume = SessionController::Resume;
  const Resume r = pendingSessionLength_ > 0 ? session_.restore(sessionFile_ + pendingSessionAt_, pendingSessionLength_)
                                             : Resume::Gone;
  // Without a session there is nothing to show on its screen: wake one step
  // below it.
  if (r == Resume::Gone) {
    if (pendingSessionLength_ > 0) usage_.error(usage::ErrorCode::SessionLost, 0);
    resumeDepth_ = at;
  }
  pendingSessionLength_ = 0;
  pendingSessionRebuild_ = false;
}

// ── Course and progress ──────────────────────────────────────────────────────

const char* App::packStatusName() const {
  if (!packFound_) return "no course.pack";
  static const char* const kNames[] = {"ok",          "too small",       "bad magic",       "wrong format version",
                                       "bad size",    "bad directory",   "missing section", "bad section",
                                       "bad strings", "unsupported host"};
  const uint8_t i = static_cast<uint8_t>(packStatus_);
  return i < sizeof kNames / sizeof kNames[0] ? kNames[i] : "?";
}

bool App::openCourse() {
  const auto preparation = learnerPreparation_;
  learnerPreparation_ = {};
  fsrs_.configure(profile_.desiredRetention(), profile_.maxInterval);
  const uint32_t started = millis();
  packBlocks_ = new (std::nothrow) uint8_t[kPackBlocks * core::pack::CachedSource::kBlockSize];
  arena_ = new (std::nothrow) char[kArenaBytes];
  if (!packBlocks_ || !arena_) {
    log("pack: no memory for its cache");
    packStatus_ = core::pack::PackStatus::TooSmall;
    return true;
  }
  pack_.setArena(arena_, kArenaBytes);
  packFound_ = packFile_.open(platform::kPackPath, packBlocks_, kPackBlocks);
  packStatus_ = packFound_ ? pack_.open(packFile_) : core::pack::PackStatus::TooSmall;
  if (!pack_.isOpen()) {
    log("pack %s: %s", platform::kPackPath, packStatusName());
    return true;
  }
  log("pack open in %lu ms: %s, %lu bytes, edition %lu, %lu items, %lu lemmas",
      static_cast<unsigned long>(millis() - started), platform::kPackPath, static_cast<unsigned long>(packFile_.size()),
      static_cast<unsigned long>(pack_.contentVersion()), static_cast<unsigned long>(pack_.itemCount()),
      static_cast<unsigned long>(pack_.count(core::pack::Section::Lemm)));

  if (storage_.available() && preparation.run && !preparation.run(preparation.context, *this)) {
    log("learner preparation: authoritative recovery failed");
    return false;
  }
  fsrs_.configure(profile_.desiredRetention(), profile_.maxInterval);
  slotCount_ = pack_.itemCount();
  slots_ = new (std::nothrow) uint16_t[slotCount_];
  if (!slots_) {
    // Too little memory for the slot table: no progress at all this time.
    log("progress: no memory for %lu slots", static_cast<unsigned long>(slotCount_));
    slotCount_ = 0;
    return true;
  }
  const uint32_t opening = millis();
  const core::ProgressStore::OpenResult opened = progress_.open(slots_, slotCount_, guest_, kGuestCapacity);
  progressOpened_ = static_cast<uint8_t>(opened);
  static const char* const kOpenNames[] = {"opened", "created", "replayed", "rebuilt", "salvaged", "guest", "failed"};
  log("progress %s in %lu ms: %lu records, %lu journal", kOpenNames[static_cast<uint8_t>(opened)],
      static_cast<unsigned long>(millis() - opening), static_cast<unsigned long>(progress_.recordCount()),
      static_cast<unsigned long>(progress_.journalCount()));
  if (progress_.authoritativeRecoveryFailed()) return false;
  starred_.open();
  if (starred_.journalFailed()) return false;
  readLog_.open();
  if (readLog_.journalFailed()) return false;
  log("starred %u, read %u", starred_.count(), readLog_.count());
  return true;
}

void App::closeCourse() {
  pack_.close();
  packFile_.close();
  delete[] slots_;
  slots_ = nullptr;
  slotCount_ = 0;
  delete[] packBlocks_;
  packBlocks_ = nullptr;
  delete[] arena_;
  arena_ = nullptr;
}

App::DeckState App::deckState(const uint16_t lemma) {
  const int32_t item = core::library::recogniseItem(pack_, lemma);
  if (item < 0) return DeckState::None;
  core::ItemState state;
  if (progress_.load(static_cast<uint32_t>(item), state) && !state.isNew()) return DeckState::Learnt;
  return starred_.contains(pack_.uidAt(static_cast<uint32_t>(item))) ? DeckState::Starred : DeckState::Open;
}

void App::pruneStarred() {
  for (uint16_t i = starred_.count(); i-- > 0;) {
    const uint32_t uid = starred_.at(i);
    const int32_t index = pack_.indexOfUid(uid);
    core::ItemState state;
    if (index < 0 || !progress_.load(static_cast<uint32_t>(index), state) || !state.isNew()) {
      starred_.remove(uid);
      log("starred %lu done", static_cast<unsigned long>(uid));
    }
  }
}

const uint32_t* App::starredItems(uint16_t& count) {
  count = 0;
  for (uint16_t i = 0; i < starred_.count(); ++i) {
    const int32_t index = pack_.indexOfUid(starred_.at(i));
    if (index >= 0) starredItems_[count++] = static_cast<uint32_t>(index);
  }
  return starredItems_;
}

bool App::toggleStar(const uint16_t lemma) {
  const DeckState state = deckState(lemma);
  if (state != DeckState::Starred && state != DeckState::Open) return false;
  const uint32_t uid = pack_.uidAt(static_cast<uint32_t>(core::library::recogniseItem(pack_, lemma)));
  if (state == DeckState::Starred) {
    starred_.remove(uid);
    log("unstarred %lu", static_cast<unsigned long>(uid));
    core::pack::Lemma record;
    usage_.star(uid, false, pack_.lemma(lemma, record) ? pack_.str(record.es) : "");
    return true;
  }
  if (starred_.count() >= core::library::MarkLog::kCapacity) return false;
  starred_.add(uid);
  log("starred %lu", static_cast<unsigned long>(uid));
  core::pack::Lemma record;
  usage_.star(uid, true, pack_.lemma(lemma, record) ? pack_.str(record.es) : "");
  return true;
}

App::LessonState App::lessonState(const uint16_t lesson) const {
  if (lesson < profile_.currentLesson) return LessonState::Done;
  if (lesson == profile_.currentLesson) return LessonState::Current;
  return lesson <= profile_.unlockedThrough ? LessonState::Open : LessonState::Locked;
}

bool App::lessonCompleted(const uint16_t lesson) {
  if (!lessonCompletion_.apply(profile_, lesson, lessonCount(), storage_.available())) {
    log("lesson completion persistence failed: %u", lesson);
    return false;
  }
  log("lesson %u complete; current %u, unlocked through %u", lesson, profile_.currentLesson, profile_.unlockedThrough);
  profileChanged();
  // Not left to the idle save: the unlock is the learner's reward.
  saveIfDirty();
  return true;
}

void App::unlockAllLessons() {
  const uint16_t count = lessonCount();
  if (count == 0) return;
  profile_.unlockedThrough = static_cast<uint16_t>(count - 1);
  log("lessons unlocked through %u", profile_.unlockedThrough);
  profileChanged();
  saveIfDirty();
}

void App::reopenProgressAsGuest() {
  if (!slots_) return;
  // Recorded, and then dropped with everything else the card no longer takes.
  usage_.error(usage::ErrorCode::CardFailed, 0);
  const core::ProgressStore::OpenResult opened = progress_.open(slots_, slotCount_, guest_, kGuestCapacity);
  log("progress reopened %s", opened == core::ProgressStore::OpenResult::Guest ? "as a guest" : "on the card");
  session_.countsChanged();
}

core::DayNumber App::lastJournalDay() {
  const core::DayNumber snapshotDay = progress_.lastStudyDay();
  const int32_t size = storage_.size(core::ProgressStore::kJournalFile);
  if (size < static_cast<int32_t>(core::JournalEntry::kSize)) return snapshotDay;
  uint8_t record[core::JournalEntry::kSize];
  const uint32_t at = (static_cast<uint32_t>(size) / core::JournalEntry::kSize - 1) * core::JournalEntry::kSize;
  if (storage_.read(core::ProgressStore::kJournalFile, at, record, sizeof record) != sizeof record) return snapshotDay;
  const auto journalDay = core::JournalEntry::decode(record).day;
  return journalDay > snapshotDay ? journalDay : snapshotDay;
}

// ── Navigation ───────────────────────────────────────────────────────────────

void App::transition(const RefreshHint hint, const uint8_t how) {
  ui_->clearTapFlash();
  // setScreen() also drops the focus left over from the previous screen.
  ui_->setScreen(buildFrame, this, RefreshHint::None);
  if (hint == RefreshHint::Full) {
    ui_->invalidate(RefreshHint::Full);
  } else {
    ui_->invalidateTransition();
  }
  focusPending_ = true;
  screenChanged_ = true;
  ++changes_;
  log("screen %s", view(topId())->name());
  usage_.screen(static_cast<uint8_t>(topId()), depth_, restoring_ ? 3 : how);
}

void App::resetTo(const ScreenId* ids, const uint8_t count, const RefreshHint hint) {
  if (depth_ > 0) view(topId())->leave();
  depth_ = 0;
  for (uint8_t i = 0; i < count && i < kMaxDepth; ++i) {
    stack_[depth_++] = ids[i];
    view(ids[i])->enter(false);
  }
  transition(hint, 2);
}

void App::push(const ScreenId id) {
  if (depth_ >= kMaxDepth) {
    log("screen stack full, %u not opened", static_cast<unsigned>(id));
    return;
  }
  if (depth_ > 0) view(topId())->leave();
  stack_[depth_++] = id;
  view(id)->enter(false);
  transition(RefreshHint::Fast, 0);
}

void App::pop() {
  // Back from the bottom screen leaves Tinta.
  if (depth_ <= 1) {
    requestExit();
    return;
  }
  view(topId())->leave();
  --depth_;
  view(topId())->enter(true);
  transition(RefreshHint::Fast, 1);
}

void App::goHome() {
  const ScreenId root = rootId();
  resetTo(&root, 1, RefreshHint::Full);
}

void App::openPause() {
  if (topId() == ScreenId::Pause) {
    pop();
    return;
  }
  if (!view(topId())->allowsGlobalGestures()) return;
  if (view(topId())->kind() == View::Kind::Overlay) pop();
  push(ScreenId::Pause);
}

void App::openLight() {
  if (!board_.hasFrontlight() || !view(topId())->allowsGlobalGestures()) return;
  if (view(topId())->kind() == View::Kind::Overlay) pop();
  hostRequest_ = HostRequest::Light;
  ++changes_;
}

bool App::setProfileMutationJournal(ProfileMutationJournal journal) {
  if (profileAuthorityFailed_ && journal.persist) {
    log("profile journal: authority recovery requires fresh app");
    return false;
  }
  profileJournal_ = journal;
  return true;
}

void App::profileChanged() {
  applyLanguage();
  fsrs_.configure(profile_.desiredRetention(), profile_.maxInterval);
  session_.countsChanged();
  ui_->setTransitionFullEvery(profile_.fullRefreshEvery);
  clock_.setRolloverHour(profile_.rolloverHour);
  profileDirty_ = true;
  invalidate();
}

void App::applyLanguage() {
  core::UiLanguage language = profile_.uiLanguage;
  if (language == core::UiLanguage::Auto) {
    bool past = false;
    const uint16_t current = profile_.currentLesson;
    if (packReady() && lessonCount() > 0) {
      core::pack::Lesson lesson;
      core::pack::Unit unit;
      past = current >= lessonCount() ||
             (pack_.lesson(current, lesson) && pack_.unit(lesson.unit, unit) && unit.number > kAutoSpanishAfterUnit);
    }
    language = past ? core::UiLanguage::Spanish : core::UiLanguage::English;
  }
  ui::setLanguage(language);
}

// ── Drawing ──────────────────────────────────────────────────────────────────

void App::buildFrame(UiScreen& screen, void* self) { static_cast<App*>(self)->compose(screen); }

void App::compose(UiScreen& screen) {
  View& top = *view(topId());
  switch (top.kind()) {
    case View::Kind::FullScreen:
      top.build(screen);
      return;
    case View::Kind::Normal:
      composeNormal(screen, top);
      return;
    case View::Kind::Overlay:
      break;
  }
  for (int8_t i = static_cast<int8_t>(depth_ - 2); i >= 0; --i) {
    View& below = *view(stack_[i]);
    if (below.kind() == View::Kind::Overlay) continue;
    freeink::ui::Frame<kMaxInteractions> frame(*target_, device_, freeink::ui::InputSnapshot{}, scratch_);
    UiScreen background(frame, theme_.tokens);
    if (below.kind() == View::Kind::FullScreen) {
      below.build(background);
    } else {
      composeNormal(background, below);
    }
    break;
  }
  drawFooter(screen, top, true);
  top.build(screen);
}

void App::composeNormal(UiScreen& screen, View& view) {
  ui::StatusInfo status;
  status.title = view.title();
  status.battery = battery_;
  status.back = !keyDevice() && depth_ > 1 && stack_[0] != topId();
  char time[8];
  platform::LocalTime now;
  if (clock_.hasTimeOfDay() && clock_.trusted() && clock_.localTime(now)) {
    snprintf(time, sizeof time, "%02u:%02u", now.hour, now.minute);
    status.time = time;
    shownMinute_ = now.minute;
  }
  ui::drawStatusBar(screen, screen.takeTop(theme_.statusHeight), theme_, status);
  drawFooter(screen, view, true);
  screen.spacer(theme_.gap);
  view.build(screen);
}

void App::drawFooter(UiScreen& screen, View& view, const bool registerTaps) {
  if (const ui::ChoiceBar* bar = view.choiceBar()) {
    const freeink::ui::Rect rect = screen.takeBottom(theme_.choiceHeight);
    ui::drawFooter(screen, rect, theme_, keys_, *bar, ui::FooterStyle::Choice, registerTaps && !keyDevice());
    return;
  }
  if (!keyDevice()) return;
  ui::ChoiceBar hints;
  if (!view.keyHints(hints)) return;
  ui::drawFooter(screen, screen.takeBottom(theme_.hintsHeight), theme_, keys_, hints, ui::FooterStyle::Hints, false);
}

bool App::frameWanted() const { return opened_ && (ui_->invalidated() || framePending_); }

void App::renderFrame(const bool repaint) {
  if (!opened_ || depth_ == 0) return;
  pack_.beginPass();
  if (repaint) {
    window_ = freeink::ui::Rect{};
    ui_->invalidateTransition();
    screenChanged_ = true;
  }
  if (ui_->invalidated()) {
    ui_->render();
    const RefreshHint hint = ui_->lastRenderRefreshHint();
    if (focusPending_) {
      focusPending_ = false;
      // Key devices start each screen with something focused: route as many
      // focus steps as the view asks for, then draw again to show it.
      const int8_t ordinal = view(topId())->focusOrdinal();
      if (keyDevice() && ordinal >= 0) {
        freeink::ui::InputSnapshot next;
        next.focusNext = true;
        for (int8_t i = 0; i <= ordinal; ++i) ui_->route(next);
        ui_->render();
      }
    }
    if (static_cast<uint8_t>(hint) > static_cast<uint8_t>(pendingHint_)) pendingHint_ = hint;
    framePending_ = true;
  }
  if (!framePending_) return;

  // The first frame is full: lila's screen was on the panel before. A new
  // screen gets the board's screen refresh: a half refresh on the X3 (one
  // pass, no ghost of the old screen), fast on the X4 family, where a half
  // refresh flashes. Changes within a screen (focus, a value) stay fast.
  using Refresh = platform::Board::Refresh;
  const bool full = frameCount_ == 0 || pendingHint_ == RefreshHint::Full || pendingHint_ == RefreshHint::Clean;
  const Refresh refresh = full ? Refresh::Full : screenChanged_ ? board_.screenRefresh() : Refresh::Fast;
  if (frameCount_ == 0) log("first frame drawn %lu ms", static_cast<unsigned long>(millis() - power_.bootMs()));
  // A cursor's worth of change: a window of the panel where the board can.
  // Not when the screen changed since (a cursor move, then Back in the same
  // pass): the X4 family's screen refresh is fast too, and needs the whole frame.
  const bool window = refresh == Refresh::Fast && !screenChanged_ && !window_.empty() &&
                      board_.presentWindow(window_.x, window_.y, window_.width, window_.height);
  if (!window) board_.present(refresh);
  window_ = freeink::ui::Rect{};
  framePending_ = false;
  screenChanged_ = false;
  pendingHint_ = RefreshHint::None;
  ++frameCount_;
  static const char* const kRefreshNames[] = {"full", "half", "fast"};
  log("frame %u %s", frameCount_, window ? "window" : kRefreshNames[static_cast<uint8_t>(refresh)]);
  if (frameCount_ == 1) {
    log("ready %lu ms, heap free %lu, lowest %lu, arena peak %lu",
        static_cast<unsigned long>(millis() - power_.bootMs()), static_cast<unsigned long>(board_.freeHeap()),
        static_cast<unsigned long>(board_.minFreeHeap()), static_cast<unsigned long>(pack_.arenaPeak()));
  }
}

// ── Input ────────────────────────────────────────────────────────────────────

void App::logInput(const ui::InputEvent& event) {
  switch (event.kind) {
    case ui::InputEvent::Kind::Key:
      log("key %s%s", ui::keyName(event.key), event.hold ? "-hold" : "");
      break;
    case ui::InputEvent::Kind::Tap:
      log("tap %d %d", event.x, event.y);
      break;
    case ui::InputEvent::Kind::Swipe: {
      static const char* const kDirs[] = {"none", "left", "right", "up", "down"};
      log("swipe %s %d %d %d %d%s", kDirs[static_cast<uint8_t>(event.swipe)], event.x, event.y, event.x2, event.y2,
          event.fromTopEdge ? " top" : "");
      break;
    }
  }
}

void App::handle(const ui::InputEvent& event) {
  power_.noteActivity();
  logInput(event);
  const uint32_t changesBefore = changes_;
  dispatch(event);
  recordInput(event, changesBefore, false);
}

// An input event for the usage log, once handled: a press that changed
// nothing on screen was a dead one; one that came while the panel was still
// showing the last frame waited for it.
void App::recordInput(const ui::InputEvent& event, const uint32_t changesBefore, const bool duringRefresh) {
  const bool changed = changes_ != changesBefore;
  const usage::Outcome outcome = !changed        ? usage::Outcome::Ignored
                                 : duringRefresh ? usage::Outcome::Queued
                                                 : usage::Outcome::Handled;
  const bool point = event.kind != ui::InputEvent::Kind::Key;
  usage_.input(usageInput(event), outcome, static_cast<uint8_t>(topId()),
               point ? static_cast<uint16_t>(event.x) : 0xFFFF, point ? static_cast<uint16_t>(event.y) : 0xFFFF);
}

void App::dispatch(const ui::InputEvent& event) {
  View* top = view(topId());
  using ui::Key;

  if (event.kind == ui::InputEvent::Kind::Key) {
    // lila owns the Power key; it never reaches Tinta.
    if (event.key == Key::Power) return;
    if ((event.key == Key::Back && event.hold) || (event.key == Key::Home && !event.hold)) {
      openPause();
      return;
    }
    if (event.key == Key::Home && event.hold) {
      if (top->allowsGlobalGestures()) goHome();
      return;
    }
  }
  if (event.kind == ui::InputEvent::Kind::Swipe && event.fromTopEdge && event.swipe == freeink::ui::SwipeDir::Down) {
    openLight();
    return;
  }
  if (top->onInput(event)) return;

  if (const ui::ChoiceBar* bar = top->choiceBar()) {
    if (event.kind == ui::InputEvent::Kind::Key) {
      const int8_t choice = keys_.choiceFor(event, *bar, 32767);
      if (choice >= 0) {
        ActionEvent answer;
        answer.action = kActionChoice;
        answer.value = choice;
        top->onAction(answer);
        invalidate();
        return;
      }
    }
  }

  const freeink::ui::InputSnapshot snapshot = keys_.snapshotFor(event);
  const ActionEvent action = ui_->route(snapshot);
  if (action) {
    if (action.action == kActionBack) {
      top->onBack();
    } else {
      top->onAction(action);
    }
    return;
  }
  if (event.kind == ui::InputEvent::Kind::Key && event.key == Key::Back && !event.hold) {
    top->onBack();
    return;
  }
  if (snapshot.focusNext || snapshot.focusPrev) invalidate();
}

// ── Loop ─────────────────────────────────────────────────────────────────────

void App::update() {
  if (!opened_ || exitRequested_) return;
  // Input after a screen change waits until the new screen has been drawn:
  // it has to be routed against that screen's controls, not the old one's.
  ui::InputEvent event;
  while (!focusPending_ && !exitRequested_ && keys_.next(event)) {
    pack_.beginPass();
    handle(event);
  }
  pack_.beginPass();
  pollPeriodic();
  // A second after the last input, or when the screen changes: one write for
  // a burst of changes.
  if (focusPending_ || millis() - power_.lastActivityMs() >= kSaveDelayMs) saveIfDirty();
  // The usage log's buffer is filling between answers (a long read, a
  // search): written between frames.
  if (usage_.needsFlush() && !framePending_) usage_.flush();
  if (depth_ > 0) view(topId())->tick();
}

void App::pollPeriodic() {
  const uint32_t now = millis();
  if (now - lastBatteryPollMs_ >= kBatteryPollMs) {
    lastBatteryPollMs_ = now;
    const platform::BatteryReading reading = board_.readBattery();
    // A gauge read can glitch (a missed I2C transfer reads as 0 %): a new
    // value counts once two polls in a row agree. Only what the status bar
    // shows matters; a change in the charging flag alone is not worth a refresh.
    const bool confirmed =
        reading.percentKnown == lastBatteryPoll_.percentKnown && reading.percent == lastBatteryPoll_.percent;
    lastBatteryPoll_ = reading;
    if (confirmed && (reading.percentKnown != battery_.percentKnown || reading.percent != battery_.percent)) {
      battery_ = reading;
      log("battery %u%%%s", reading.percent, reading.chargingKnown && reading.charging ? " charging" : "");
      invalidate();
    }
  }
  // Before the date or the clock is known (the time step, or a first run
  // that has not reached it) there is no day to follow.
  if (depth_ == 0 || isTimeStep(stack_[0]) || !clock_.trusted()) return;
  if (clock_.pollDayChange()) {
    const uint32_t now = clock_.nowSeconds();
    usage_.clockChange(usage::ClockKind::DayRollover, now, now);
    session_.countsChanged();
    if (clock_.hasTimeOfDay()) {
      profile_.lastConfirmedDay = clock_.today();
      profileDirty_ = true;
    }
    invalidate();
  }
  // The status bar's clock, once a minute.
  if (clock_.hasTimeOfDay() && view(topId())->kind() != View::Kind::FullScreen) {
    platform::LocalTime local;
    if (clock_.localTime(local) && local.minute != shownMinute_) {
      shownMinute_ = local.minute;
      invalidate();
    }
  }
}

void App::saveIfDirty() {
  if (!profileDirty_) return;
  profileDirty_ = false;
  if (!storage_.available()) return;
  if (profileAuthorityFailed_) {
    log("profile save blocked: authority recovery required");
    return;
  }
  if (profileJournal_.persist && !profileJournal_.persist(profileJournal_.context, profile_)) {
    profileAuthorityFailed_ = true;
    log("profile save blocked: authoritative journal failed");
    return;
  }
  if (profile_.save(storage_)) {
    log("profile saved");
  } else {
    log("profile save failed");
  }
}

// ── Sleep ────────────────────────────────────────────────────────────────────

void App::saveSession() {
  if (!storage_.available()) return;
  uint8_t* b = sessionFile_;
  uint8_t depth = 0;
  uint32_t sessionLength = 0;
  if (inTimeStep()) {
    // Asleep again before the date was given: keep the screens and the
    // session it was going to return to. The session bytes are still where
    // loadResumeStack() found them, behind the same number of screen ids.
    for (uint8_t i = 0; i < resumeDepth_; ++i) b[7 + depth++] = static_cast<uint8_t>(resume_[i]);
    if (pendingSessionLength_ > 0) {
      memmove(b + 7 + depth + 2, b + pendingSessionAt_, pendingSessionLength_);
      sessionLength = pendingSessionLength_;
      pendingSessionAt_ = 7u + depth + 2;
    }
  } else {
    for (uint8_t i = 0; i < depth_; ++i) {
      if (!view(stack_[i])->restorable()) break;
      b[7 + depth++] = static_cast<uint8_t>(stack_[i]);
    }
    sessionLength = session_.serialize(b + 7 + depth + 2, kSessionFileCap - (7u + depth + 2) - 32 - 4);
  }
  for (uint8_t i = 0; i < 4; ++i) b[i] = kSessionMagic[i];
  const bool bindSnapshot = learnerSnapshotBound_ && !(inTimeStep() && pendingSessionRebuild_);
  core::putU16(b + 4, bindSnapshot ? kSessionVersion : 2);
  b[6] = depth;
  core::putU16(b + 7 + depth, static_cast<uint16_t>(sessionLength));
  uint32_t covered = 7u + depth + 2 + sessionLength;
  if (bindSnapshot) {
    std::copy_n(learnerSnapshot_, 32, b + covered);
    covered += 32;
  }
  core::putU32(b + covered, core::crc32(b, covered));
  // In place, not through a temporary file: this runs after every grade, and
  // a torn write only fails the CRC, which loses no more than the session's
  // place (the grades are in the journal).
  if (!storage_.write(kSessionFile, 0, b, covered + 4)) log("session.bin not written");
}

uint8_t App::loadResumeStack(ScreenId* out, const uint8_t cap) {
  uint8_t* b = sessionFile_;
  pendingSessionLength_ = 0;
  pendingSessionRebuild_ = false;
  const int32_t size = storage_.size(kSessionFile);
  if (size < 11) return 0;
  const uint32_t length = static_cast<uint32_t>(size) < kSessionFileCap ? static_cast<uint32_t>(size) : kSessionFileCap;
  if (storage_.read(kSessionFile, 0, b, length) != static_cast<int32_t>(length)) return 0;
  core::SessionFileView saved;
  if (!core::decodeSessionFile({b, length}, cap, static_cast<uint8_t>(ScreenId::Count), saved) ||
      (saved.version >= 3 && !learnerSnapshotBound_))
    return 0;
  pendingSessionRebuild_ = learnerSnapshotBound_ && core::sessionSnapshotChanged(saved, learnerSnapshot_);
  const auto depth = static_cast<uint8_t>(saved.screens.size());
  for (uint8_t i = 0; i < depth; ++i) out[i] = static_cast<ScreenId>(saved.screens[i]);
  const auto sessionLength = static_cast<uint32_t>(saved.session.size());
  pendingSessionAt_ = sessionLength ? static_cast<uint32_t>(saved.session.data() - b) : 0;
  pendingSessionLength_ = sessionLength;
  if (depth > 0) log("resume %u screens%s", depth, sessionLength ? " and a session" : "");
  return depth;
}

void App::flush() {
  session_.setAside();
  saveSession();
  saveIfDirty();
  usage_.flush();
  log("flushed");
}

void App::invalidateWindow(const freeink::ui::Rect rect) {
  const bool had = !window_.empty();
  // Widen the window to cover both rectangles.
  if (had) {
    const int16_t x = rect.x < window_.x ? rect.x : window_.x;
    const int16_t y = rect.y < window_.y ? rect.y : window_.y;
    const int16_t right = rect.right() > window_.right() ? rect.right() : window_.right();
    const int16_t bottom = rect.bottom() > window_.bottom() ? rect.bottom() : window_.bottom();
    window_ = freeink::ui::Rect{x, y, static_cast<int16_t>(right - x), static_cast<int16_t>(bottom - y)};
  } else {
    window_ = rect;
  }
  // Something else is pending already: the frame goes whole.
  if (!had && (framePending_ || ui_->invalidated() || screenChanged_)) window_ = freeink::ui::Rect{};
  ui_->invalidate(freeink::ui::RefreshHint::Fast);
  ++changes_;
}

// Once the pack is open: what this opening is, for the
// usage log (docs/usage-log.md), and anything that already went wrong.
void App::startUsageLog() {
  usage::BootInfo info;
  // Each time Tinta is opened counts as a boot.
  info.reason = usage::BootReason::PowerOn;
  info.device = board_.id();
  info.build = "lila";
#ifdef CROSSPOINT_VERSION
  info.version = CROSSPOINT_VERSION;
#endif
  if (pack_.isOpen()) {
    info.packEdition = pack_.contentVersion();
    info.packMajor = static_cast<uint8_t>(pack_.formatMajor());
    info.packMinor = static_cast<uint8_t>(pack_.formatMinor());
    info.packCrc = pack_.headerCrc();
    info.packBuildTime = pack_.buildTime();
  }
  usage_.boot(info);
  using Opened = core::ProgressStore::OpenResult;
  if (!pack_.isOpen()) {
    usage_.error(usage::ErrorCode::PackError, static_cast<uint32_t>(packStatus_));
  } else if (progressOpened_ == static_cast<uint8_t>(Opened::Guest) ||
             progressOpened_ == static_cast<uint8_t>(Opened::Failed)) {
    usage_.error(usage::ErrorCode::ProgressGuest, progressOpened_);
  }
}

uint32_t App::uptimeMs() { return millis(); }

// ── View defaults that need the app ──────────────────────────────────────────

void View::onBack() { app_.pop(); }

bool View::keyHints(ui::ChoiceBar& out) const {
  for (uint8_t i = 0; i < ui::kFooterCellCount; ++i) {
    ui::CellSpec& cell = out.cells[i];
    cell = ui::CellSpec{};
    switch (app_.keys().footerCell(i).key) {
      case ui::Key::Back:
        cell.label = ui::tr(ui::Str::Back);
        break;
      case ui::Key::Confirm:
        cell.label = ui::tr(ui::Str::Select);
        break;
      case ui::Key::Left:
        cell.icon = &icons::kChevronUp24;
        break;
      case ui::Key::Right:
        cell.icon = &icons::kChevronDown24;
        break;
      default:
        break;
    }
  }
  return true;
}

}  // namespace tinta::app
