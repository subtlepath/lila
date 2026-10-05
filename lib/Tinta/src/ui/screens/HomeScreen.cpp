#include "ui/screens/HomeScreen.h"

#include <stdio.h>

#include "app/App.h"
#include "core/stats/Streak.h"
#include "ui/Fonts.h"
#include "ui/Strings.h"
#include "ui/screens/LessonScreens.h"
#include "ui/views/CardText.h"
#include "ui/views/Chrome.h"

namespace tinta::ui {
namespace {

using app::App;
using app::ScreenId;
using freeink::ui::Rect;
using freeink::ui::TextStyle;

struct HomeEntry {
  Str label;
  const char* slug;
  void (*open)(App& app);
  bool (*shown)(App& app);                           // null: always
  Str (*labelFor)(App& app);                         // null: `label`
  void (*textFor)(App& app, char* out, size_t cap);  // null: the label above
};

bool hasCourse(App& app) { return app.packReady(); }

// Today's session: Start, or Continue one in progress; hidden when there is
// nothing to study.
bool todayShown(App& app) {
  if (!app.packReady()) return false;
  const app::TodayCounts& c = app.session().todayCounts();
  return c.active || c.due + c.fresh > 0;
}

void openToday(App& app) {
  if (app.session().active() || app.session().startToday()) app.push(ScreenId::Session);
}

// The current lesson: the first not yet completed. Hidden once every lesson
// is done (the course map still opens them).
bool lessonShown(App& app) { return app.packReady() && app.profile().currentLesson < app.lessonCount(); }

void lessonText(App& app, char* out, const size_t cap) {
  const uint16_t lesson = app.profile().currentLesson;
  char code[16];
  lessonCode(app.pack(), lesson, code, sizeof code);
  core::pack::Lesson record;
  app.pack().lesson(lesson, record);
  snprintf(out, cap, tr(Str::LessonRowFmt), code, app.pack().str(record.title));
}

// What Home offers, top to bottom. List only what works.
const HomeEntry kEntries[] = {
    {Str::Start, "today", openToday, todayShown,
     [](App& a) { return a.session().active() ? Str::Continue : Str::Start; }, nullptr},
    {Str::LessonFmt, "lesson", [](App& a) { showLesson(a, a.profile().currentLesson); }, lessonShown, nullptr,
     lessonText},
    {Str::CourseMap, "course", [](App& a) { a.push(ScreenId::Course); }, hasCourse, nullptr, nullptr},
    {Str::Read, "read", [](App& a) { a.push(ScreenId::Readings); }, hasCourse, nullptr, nullptr},
    {Str::Phrases, "phrases", [](App& a) { a.push(ScreenId::Phrasebook); }, hasCourse, nullptr, nullptr},
    {Str::Dictionary, "dictionary", [](App& a) { a.push(ScreenId::Dictionary); }, hasCourse, nullptr, nullptr},
    {Str::Progress, "progress", [](App& a) { a.push(ScreenId::Progress); }, hasCourse, nullptr, nullptr},
    {Str::Settings, "settings", [](App& a) { a.push(ScreenId::Settings); }, nullptr, nullptr, nullptr},
    {Str::Leave, "leave", [](App& a) { a.requestExit(); }, nullptr, nullptr, nullptr},
};
constexpr uint8_t kEntryCount = sizeof kEntries / sizeof kEntries[0];

// The index in kEntries of the n-th entry shown, or -1.
int8_t entryAt(App& app, uint8_t n) {
  for (uint8_t i = 0; i < kEntryCount; ++i) {
    if (kEntries[i].shown && !kEntries[i].shown(app)) continue;
    if (n-- == 0) return static_cast<int8_t>(i);
  }
  return -1;
}

}  // namespace

const char* storageBanner(App& app) {
  if (app.storage().failed()) return tr(Str::CardFailedBanner);
  if (!app.storage().available()) return tr(Str::GuestBanner);
  return nullptr;
}

const char* HomeScreen::title() const { return tr(Str::AppName); }

uint8_t HomeScreen::rowCount() const {
  uint8_t count = 0;
  while (entryAt(app_, count) >= 0) ++count;
  return count;
}

void HomeScreen::row(const uint8_t index, RowSpec& out, char* value, const size_t cap) const {
  const int8_t entry = entryAt(app_, index);
  if (entry < 0) return;
  const HomeEntry& e = kEntries[entry];
  out.kind = e.open == openToday ? RowSpec::Kind::Action : RowSpec::Kind::Link;
  out.label = tr(e.labelFor ? e.labelFor(app_) : e.label);
  if (e.textFor) {
    e.textFor(app_, value, cap);
    out.label = value;
  }
  out.slug = e.slug;
}

void HomeScreen::activate(const uint8_t index) {
  const int8_t entry = entryAt(app_, index);
  if (entry < 0) return;
  app_.clearTapFlash();
  kEntries[entry].open(app_);
}

void HomeScreen::buildHeader(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  freeink::ui::DrawTarget& t = screen.target();
  if (const char* banner = storageBanner(app_)) drawBanner(screen, theme, banner);
  const int16_t textW = static_cast<int16_t>(screen.body().width - 2 * theme.margin);
  const auto line = [&](const char* text, const TextStyle& style, int16_t gap) {
    const Rect r = screen.takeTop(t.lineHeight(style.font), gap);
    t.text(Rect{static_cast<int16_t>(r.x + theme.margin), r.y, textW, r.height}, text, style);
  };

  if (app_.clock().trusted()) {
    char date[64];
    formatDate(app_.clock().today(), DateStyle::Long, date, sizeof date);
    line(date, theme.tokens.titleText, theme.gap);
  }
  if (!app_.packReady()) return;

  // The Today card: what the next session holds, and the streak.
  TextStyle bold;
  bold.font = kSlotBodyBold;
  line(tr(Str::Today), bold, 2);
  const app::TodayCounts& c = app_.session().todayCounts();
  char counts[64];
  if (c.active) {
    snprintf(counts, sizeof counts, tr(Str::TodayLeftFmt), c.left);
  } else if (c.due + c.fresh > 0) {
    snprintf(counts, sizeof counts, tr(Str::TodayCountsFmt), c.due, c.fresh);
  } else {
    snprintf(counts, sizeof counts, "%s", tr(Str::NothingDue));
  }
  line(counts, theme.tokens.bodyText, 2);

  const core::DayNumber today = app_.clock().today();
  const uint32_t journal = app_.progress().journalCount();
  if (!streakValid_ || streakDay_ != today || streakJournal_ != journal) {
    core::StreakInfo info;
    streak_ = core::currentStreak(app_.dayLog(), today, info) ? info.days : 0;
    streakValid_ = true;
    streakDay_ = today;
    streakJournal_ = journal;
  }
  if (streak_ > 0) {
    char streak[48];
    if (streak_ == 1) {
      snprintf(streak, sizeof streak, "%s", tr(Str::StreakOne));
    } else {
      snprintf(streak, sizeof streak, tr(Str::StreakFmt), streak_);
    }
    line(streak, theme.tokens.smallText, 0);
  }
  screen.spacer(2 * theme.gap);
}

}  // namespace tinta::ui
