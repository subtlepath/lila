#include "ui/screens/SessionScreen.h"

#include <stdio.h>

#include "app/App.h"
#include "core/stats/Streak.h"
#include "platform/Log.h"
#include "ui/Fonts.h"
#include "ui/Strings.h"
#include "ui/screens/HomeScreen.h"
#include "ui/screens/LessonScreens.h"
#include "ui/views/CardText.h"
#include "ui/views/Chrome.h"

namespace tinta::ui {
namespace {

using app::App;
using app::ScreenId;
namespace pk = core::pack;
using core::Grade;
using freeink::ui::Color;
using freeink::ui::Paint;
using freeink::ui::Rect;
using freeink::ui::TextStyle;

int16_t i16(int32_t v) { return static_cast<int16_t>(v); }

enum : app::ActionId { kActionReveal = app::kFirstViewAction };

constexpr int16_t kBarHeight = 6;
constexpr int16_t kHintGap = 10;

// "Press any key to show the answer" on a self-graded front's last line, in
// the style of a result's "Press any key to go on"; the area left above it.
Rect drawShowHint(App& app, const Rect area) {
  const BitmapFont& f = font(FontRole::ChromeSmall);
  freeink::ui::DisplayTarget& t = app.target();
  t.setFont(kSlotScratch, f);
  TextStyle s;
  s.font = kSlotScratch;
  s.align = freeink::ui::TextAlign::Center;
  t.text(Rect{area.x, i16(area.bottom() - f.yAdvance), area.width, f.yAdvance},
         tr(app.keyDevice() ? Str::ShowAnswerKeys : Str::ShowAnswerTouch), s);
  return Rect{area.x, area.y, area.width, i16(area.height - f.yAdvance - kHintGap)};
}

void formatInterval(const core::IntervalPreview& p, char* out, size_t cap) {
  if (p.inSession) {
    snprintf(out, cap, "%s", tr(Str::ThisSession));
  } else if (p.days < 30) {
    snprintf(out, cap, tr(Str::DaysShortFmt), p.days);
  } else if (p.days < 365) {
    snprintf(out, cap, tr(Str::MonthsShortFmt), (p.days + 15) / 30);
  } else {
    const unsigned tenths = (p.days * 10u + 182u) / 365u;
    snprintf(out, cap, tr(Str::YearsShortFmt), tenths / 10, tenths % 10);
  }
}

// The leech notice keeps the card's own footer cells: Keep where Confirm is,
// Suspend to its right.
constexpr uint8_t kKeepCell = 1;
constexpr uint8_t kSuspendCell = 2;

}  // namespace

// ── Session ──────────────────────────────────────────────────────────────────

const char* SessionScreen::title() const {
  app::SessionController& s = app_.session();
  if (!s.active()) return tr(Str::Today);
  const int32_t lesson = s.sessionLesson();
  if (lesson >= 0) {
    char code[16];
    lessonCode(app_.pack(), static_cast<uint16_t>(lesson), code, sizeof code);
    snprintf(title_, sizeof title_, tr(Str::LessonProgressFmt), code, s.position(), s.total());
  } else if (s.sessionCategory() >= 0) {
    snprintf(title_, sizeof title_, tr(Str::PhrasesProgressFmt), s.position(), s.total());
  } else {
    snprintf(title_, sizeof title_, tr(Str::ReviewFmt), s.position(), s.total());
  }
  return title_;
}

const Str kGrades[4] = {Str::Again, Str::Hard, Str::Good, Str::Easy};

void SessionScreen::enter(bool) {
  traceNext_ = true;
  // The text size may have changed in Settings since the card was loaded.
  app_.session().restyle();
  if (app_.session().revealed()) fillGrades();
}

void SessionScreen::fillGrades() {
  const core::IntervalPreview* previews = app_.session().previews();
  bar_ = ChoiceBar{};
  for (uint8_t i = 0; i < 4; ++i) {
    formatInterval(previews[i], details_[i], sizeof details_[i]);
    bar_.cells[i].label = tr(kGrades[i]);
    bar_.cells[i].detail = details_[i];
  }
}

// Which side of which kind of card is showing.
bool SessionScreen::showingResult() const {
  const app::SessionController& s = app_.session();
  return s.active() && s.showingResult() && !s.leechPending();
}

bool SessionScreen::autoFront() const {
  const app::SessionController& s = app_.session();
  return s.active() && !s.revealed() && !s.leechPending() && !s.showingResult() && s.view() && s.view()->autoGraded();
}

bool SessionScreen::selfGradedFront() const {
  const app::SessionController& s = app_.session();
  return s.active() && !s.revealed() && !s.leechPending() && !s.showingResult() &&
         !(s.view() && s.view()->autoGraded());
}

const ChoiceBar* SessionScreen::choiceBar() const {
  app::SessionController& s = app_.session();
  if (!s.active()) return nullptr;
  if (s.leechPending()) return &bar_;
  // An answer's result keeps the front's footer (it only adds ink); any key
  // goes on.
  if (showingResult() || autoFront()) return s.view() ? s.view()->answers() : nullptr;
  if (s.revealed()) return &bar_;
  // The grades to come, greyed out: revealing them only adds ink.
  ChoiceBar& front = const_cast<ChoiceBar&>(frontBar_);
  for (uint8_t i = 0; i < 4; ++i) {
    front.cells[i] = CellSpec{};
    front.cells[i].label = tr(kGrades[i]);
    // An empty detail line keeps the label where the back will draw it.
    front.cells[i].detail = "";
    front.cells[i].enabled = false;
  }
  return &frontBar_;
}

// Key devices, when there is no choice bar: a view's own hints (word order),
// or Continue once a result shows.
bool SessionScreen::keyHints(ChoiceBar& out) const {
  out = ChoiceBar{};
  const app::SessionController& s = app_.session();
  if (autoFront() && s.view()) return s.view()->keyHints(app_.keys(), out);
  for (uint8_t i = 0; i < kFooterCellCount; ++i) {
    if (app_.keys().footerCell(i).key == Key::Confirm) out.cells[i].label = tr(Str::Continue);
  }
  return true;
}

void SessionScreen::build(app::UiScreen& screen) {
  app::SessionController& s = app_.session();
  const Theme& theme = app_.theme();
  freeink::ui::DrawTarget& t = screen.target();

  // Progress through the session: done out of done + left.
  const Rect bar = screen.takeTop(kBarHeight, theme.gap);
  const Rect track{i16(bar.x + theme.margin), bar.y, i16(bar.width - 2 * theme.margin), bar.height};
  t.stroke(track, Paint::solid(Color::Black), 1);
  const uint16_t total = s.total();
  if (total > 0) {
    const int16_t filled = i16(static_cast<int32_t>(track.width) * (s.position() - 1) / total);
    if (filled > 0) t.fill(Rect{track.x, track.y, filled, track.height}, Paint::solid(Color::Black));
  }
  if (const char* banner = storageBanner(app_)) drawBanner(screen, theme, banner);
  if (!s.active() || !s.view()) return;

  Rect area = screen.body();
  area.x = i16(area.x + theme.margin);
  area.width = i16(area.width - 2 * theme.margin);
  if (s.leechPending()) {
    buildLeech(screen, area);
    return;
  }
  if (!app_.keyDevice() && (selfGradedFront() || showingResult())) {
    // Touch: the whole card reveals (a self-graded front) or goes on (a
    // result); a result's footer cells go on too.
    const Rect full = screen.frame().screen();
    const Rect card{full.x, screen.body().y, full.width, i16(full.bottom() - screen.body().y)};
    screen.frame().hit(card, kActionReveal, 0, freeink::ui::InputTouch);
    if (platform::kSimulator && traceNext_) {
      const Rect body = screen.body();
      platform::log("target session/card %d %d", card.x + card.width / 2, body.y + body.height / 2);
    }
  }
  traceNext_ = false;
  // The back draws over the hint's line (see reveal()).
  if (selfGradedFront() && s.showHint()) area = drawShowHint(app_, area);
  s.view()->draw(screen, area, s.revealed() || showingResult());
}

void SessionScreen::buildLeech(app::UiScreen& screen, const Rect area) {
  app::SessionController& s = app_.session();
  core::ItemState state;
  app_.progress().load(s.leechIndex(), state);
  Column col;
  col.x = area.x;
  col.y = area.y;
  col.width = area.width;
  col.bottom = area.bottom();
  char text[192];
  snprintf(text, sizeof text, tr(Str::LeechFmt), state.lapses);
  freeink::ui::DisplayTarget& t = app_.target();
  drawTextIn(t, col, tr(Str::LeechTitle), font(FontRole::ChromeTitle), 1, 4);
  drawTextIn(t, col, text, font(FontRole::ChromeBody), 4, 12);
  drawRule(t, col, 12);
  const Rect rest{area.x, col.y, area.width, i16(area.bottom() - col.y)};
  s.view()->showGoOn(false);
  s.view()->draw(screen, rest, true);
  s.view()->showGoOn(true);
}

void SessionScreen::reveal() {
  app::SessionController& s = app_.session();
  if (!selfGradedFront()) return;
  const bool hinted = s.showHint();
  s.reveal();
  fillGrades();
  // A reveal only adds ink: fast. The first flashcard of a session loses its
  // hint, which a fast refresh would leave as a ghost under the answer: that
  // one is a half refresh.
  if (hinted) {
    app_.invalidateCard();
  } else {
    app_.invalidate();
  }
}

void SessionScreen::grade(const uint8_t cell) {
  if (cell > 3) return;
  app_.session().grade(static_cast<Grade>(cell + 1));
  advance(true);
}

void SessionScreen::answered(const Grade grade) {
  app::SessionController& s = app_.session();
  const bool newScreen = s.view() && s.view()->resultIsNewScreen();
  s.answer(grade);
  // The result adds marks and the answer to the card: a fast refresh, unless
  // the card loses ink (a keyboard) or a leech notice takes over.
  advance(newScreen || s.leechPending());
}

void SessionScreen::goOn() {
  app_.session().next();
  advance(true);
}

void SessionScreen::reply(const ExerciseView::Reply r, const Grade grade) {
  if (r == ExerciseView::Reply::Answered) {
    answered(grade);
  } else if (r == ExerciseView::Reply::Handled) {
    app_.invalidate();
  }
}

void SessionScreen::undo() {
  if (!app_.session().undo()) return;
  advance(true);
}

void SessionScreen::resolveLeech(const bool suspend) {
  app_.session().resolveLeech(suspend);
  advance(true);
}

void SessionScreen::advance(const bool newCard) {
  app::SessionController& s = app_.session();
  if (!s.active()) {
    app_.clearTapFlash();
    const ScreenId ids[] = {app_.rootId(), ScreenId::Summary};
    app_.resetTo(ids, 2, freeink::ui::RefreshHint::Full);
    return;
  }
  if (s.leechPending()) {
    bar_ = ChoiceBar{};
    bar_.cells[kKeepCell].label = tr(Str::Keep);
    bar_.cells[kSuspendCell].label = tr(Str::Suspend);
  } else if (s.revealed()) {
    fillGrades();
  }
  traceNext_ = true;
  app_.clearTapFlash();
  if (newCard) {
    app_.invalidateCard();
  } else {
    app_.invalidate();
  }
}

void SessionScreen::onAction(const app::ActionEvent& event) {
  app::SessionController& s = app_.session();
  if (event.action == kActionReveal) {
    if (showingResult()) {
      goOn();
    } else {
      reveal();
    }
    return;
  }
  Grade answer = Grade::Good;
  if (event.action >= kExerciseAction) {
    if (autoFront()) reply(s.view()->onAction(event, answer), answer);
    return;
  }
  if (event.action != app::kActionChoice || event.value < 0) return;
  const uint8_t cell = static_cast<uint8_t>(event.value);
  if (s.leechPending()) {
    if (cell == kKeepCell) resolveLeech(false);
    if (cell == kSuspendCell) resolveLeech(true);
  } else if (showingResult()) {
    goOn();
  } else if (s.revealed()) {
    grade(cell);
  } else if (autoFront()) {
    reply(s.view()->onChoice(cell, answer), answer);
  }
}

bool SessionScreen::onInput(const InputEvent& event) {
  app::SessionController& s = app_.session();
  if (!s.active()) return false;
  if (event.kind == InputEvent::Kind::Swipe && event.swipe == freeink::ui::SwipeDir::Right) {
    undo();
    return true;
  }
  if (event.kind != InputEvent::Kind::Key || event.hold) return false;
  if (event.key == Key::Up) {
    undo();
    return true;
  }
  if (s.leechPending()) {
    if (event.key != Key::Down) return false;
    resolveLeech(false);
    return true;
  }
  // After an answer every key but Power goes on.
  if (showingResult() && event.key != Key::Power) {
    goOn();
    return true;
  }
  if (autoFront()) {
    // The view's own keys (word order); a choice's front keys are answers
    // through the choice bar.
    Grade answer = Grade::Good;
    const ExerciseView::Reply r = s.view()->onKey(event, answer);
    if (r == ExerciseView::Reply::Ignored) return event.key == Key::Down;
    reply(r, answer);
    return true;
  }
  switch (event.key) {
    case Key::Down:
      if (s.revealed()) {
        grade(static_cast<uint8_t>(Grade::Good) - 1);
      } else {
        reveal();
      }
      return true;
    case Key::Back:
    case Key::Confirm:
    case Key::Left:
    case Key::Right:
      // On the front every front key reveals; on the back they are grades.
      if (!selfGradedFront()) return false;
      reveal();
      return true;
    default:
      return false;
  }
}

void SessionScreen::onBack() { app_.openPause(); }

// ── Summary ──────────────────────────────────────────────────────────────────

const char* SummaryScreen::title() const { return tr(Str::SessionDone); }

void SummaryScreen::enter(const bool returning) {
  FormView::enter(returning);
  core::StreakInfo streak;
  streak_ = core::currentStreak(app_.dayLog(), app_.clock().today(), streak) ? streak.days : 0;
}

void SummaryScreen::onBack() { app_.goHome(); }

namespace {

// After a lesson's practice: the lesson now current, if any is left.
int32_t nextLesson(app::App& app) {
  if (app.session().completedLesson() < 0) return -1;
  const uint16_t next = app.profile().currentLesson;
  return next < app.lessonCount() ? next : -1;
}

}  // namespace

uint8_t SummaryScreen::rowCount() const { return nextLesson(app_) >= 0 ? 2 : 1; }

void SummaryScreen::row(const uint8_t index, RowSpec& out, char*, size_t) const {
  if (index == 0 && rowCount() == 2) {
    out.kind = RowSpec::Kind::Link;
    out.label = tr(Str::NextLessonRow);
    out.slug = "next-lesson";
    return;
  }
  out.kind = RowSpec::Kind::Action;
  out.label = tr(Str::Done);
  out.slug = "done";
}

void SummaryScreen::activate(const uint8_t index) {
  app_.clearTapFlash();
  const int32_t next = nextLesson(app_);
  if (index == 0 && next >= 0) {
    const app::ScreenId root = app_.rootId();
    app_.resetTo(&root, 1);
    showLesson(app_, static_cast<uint16_t>(next));
    return;
  }
  app_.goHome();
}

void SummaryScreen::buildHeader(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  freeink::ui::DrawTarget& t = screen.target();
  const core::StudyTotals& totals = app_.session().summaryTotals();
  if (const char* banner = storageBanner(app_)) drawBanner(screen, theme, banner);
  screen.spacer(theme.gap);

  char values[5][32];
  snprintf(values[0], sizeof values[0], "%u", totals.reviews);
  const unsigned percent = totals.reviews ? (totals.correct * 100u + totals.reviews / 2) / totals.reviews : 0;
  snprintf(values[1], sizeof values[1], "%u (%u%%)", totals.correct, percent);
  snprintf(values[2], sizeof values[2], "%u", totals.newItems);
  const unsigned long seconds = (totals.milliseconds + 500) / 1000;
  snprintf(values[3], sizeof values[3], "%lu:%02lu", seconds / 60, seconds % 60);
  if (streak_ == 1) {
    snprintf(values[4], sizeof values[4], "%s", tr(Str::OneDay));
  } else {
    snprintf(values[4], sizeof values[4], tr(Str::DaysFmt), streak_);
  }
  const Str labels[5] = {Str::Reviewed, Str::Correct, Str::NewItems, Str::TimeSpent, Str::Streak};

  TextStyle label = theme.tokens.bodyText;
  TextStyle value;
  value.font = kSlotTitle;
  value.align = freeink::ui::TextAlign::Right;
  const int16_t lineH = i16(t.lineHeight(kSlotTitle) + 2 * theme.gap);
  for (uint8_t i = 0; i < 5; ++i) {
    const Rect r = screen.takeTop(lineH);
    const Rect inner{i16(r.x + theme.margin), r.y, i16(r.width - 2 * theme.margin), r.height};
    t.text(inner, tr(labels[i]), label);
    t.text(inner, values[i], value);
    t.fill(Rect{inner.x, i16(r.bottom() - 1), inner.width, 1}, Paint::dither(Color::LightGray));
  }

  drawLessonDone(screen);
  screen.spacer(2 * theme.gap);
}

// A lesson's practice done: the lesson is complete, the next one open.
void SummaryScreen::drawLessonDone(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  freeink::ui::DrawTarget& t = screen.target();
  const int32_t done = app_.session().completedLesson();
  if (done >= 0) {
    const pk::Pack& pack = app_.pack();
    char code[16];
    char text[96];
    screen.spacer(theme.gap);
    lessonCode(pack, static_cast<uint16_t>(done), code, sizeof code);
    snprintf(text, sizeof text, tr(Str::LessonDoneFmt), code);
    TextStyle bold;
    bold.font = kSlotBodyBold;
    const int16_t width = i16(screen.body().width - 2 * theme.margin);
    Rect r = screen.takeTop(t.lineHeight(kSlotBodyBold), theme.gap);
    t.text(Rect{i16(r.x + theme.margin), r.y, width, r.height}, text, bold);
    const int32_t next = nextLesson(app_);
    if (next >= 0) {
      pk::Lesson record;
      pack.lesson(static_cast<uint16_t>(next), record);
      lessonCode(pack, static_cast<uint16_t>(next), code, sizeof code);
      snprintf(text, sizeof text, tr(Str::NextLessonFmt), code, pack.str(record.title));
    } else {
      snprintf(text, sizeof text, "%s", tr(Str::CourseFinished));
    }
    TextStyle body = theme.tokens.bodyText;
    body.maxLines = 2;
    const freeink::ui::Size size = freeink::ui::measureWrappedText(t, text, body, width);
    r = screen.takeTop(size.height, theme.gap);
    t.text(Rect{i16(r.x + theme.margin), r.y, width, r.height}, text, body);
  }
}

}  // namespace tinta::ui
