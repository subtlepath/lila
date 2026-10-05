#include "ui/screens/ProgressScreen.h"

#include <stdio.h>

#include "app/App.h"
#include "core/stats/Streak.h"
#include "platform/Log.h"
#include "ui/Fonts.h"
#include "ui/Strings.h"
#include "ui/screens/HomeScreen.h"
#include "ui/views/Chrome.h"

namespace tinta::ui {
namespace {

using freeink::ui::Color;
using freeink::ui::Paint;
using freeink::ui::Rect;
using freeink::ui::TextAlign;
using freeink::ui::TextStyle;

int16_t i16(int32_t v) { return static_cast<int16_t>(v); }

TextStyle styled(freeink::ui::FontId font, TextAlign align = TextAlign::Left) {
  TextStyle s;
  s.font = font;
  s.align = align;
  return s;
}

// A caption, then `count` bars scaled to the largest value, which is printed
// at the right of the caption. `marked` gets an outline under it: today.
void drawBars(app::UiScreen& screen, const Theme& theme, const char* caption, const uint16_t* values, uint8_t count,
              uint8_t marked, int16_t chartHeight) {
  freeink::ui::DrawTarget& t = screen.target();
  uint16_t most = 0;
  for (uint8_t i = 0; i < count; ++i) most = values[i] > most ? values[i] : most;
  const Rect head = screen.takeTop(t.lineHeight(kSlotSmall), 4);
  const Rect text{i16(head.x + theme.margin), head.y, i16(head.width - 2 * theme.margin), head.height};
  t.text(text, caption, styled(kSlotKeyLabel));
  char top[12];
  snprintf(top, sizeof top, "%u", most);
  t.text(text, top, styled(kSlotSmall, TextAlign::Right));

  const Rect chart = screen.takeTop(chartHeight, 4);
  const int16_t left = i16(chart.x + theme.margin);
  const int16_t width = i16(chart.width - 2 * theme.margin);
  const int16_t slot = i16(width / count);
  const int16_t baseline = i16(chart.bottom() - 6);
  t.fill(Rect{left, baseline, width, 1}, Paint::solid(Color::Black));
  for (uint8_t i = 0; i < count; ++i) {
    const int16_t x = i16(left + i * slot + 2);
    const int16_t w = i16(slot - 4);
    const int16_t h = most ? i16(static_cast<int32_t>(baseline - chart.y - 2) * values[i] / most) : 0;
    if (h > 0) t.fill(Rect{x, i16(baseline - h), w, h}, Paint::solid(Color::Black));
    if (i == marked) t.fill(Rect{x, i16(baseline + 3), w, 3}, Paint::solid(Color::Black));
  }
}

}  // namespace

const char* ProgressScreen::title() const { return tr(Str::Progress); }

void ProgressScreen::enter(bool) {
  app::App& app = app_;
  const core::DayNumber today = app.clock().today();
  core::StreakInfo streak;
  streak_ = core::currentStreak(app.dayLog(), today, streak) ? streak.days : 0;

  core::ProgressStore::Totals totals;
  app.progress().totals(totals);
  const uint32_t items = app.pack().itemCount();
  notStarted_ = items > totals.seen ? items - totals.seen : 0;
  mature_ = totals.mature;
  learning_ = totals.seen > totals.mature ? totals.seen - totals.mature : 0;
  suspended_ = totals.suspended;

  core::DayTotals days[kDays];
  const core::DayNumber first = today >= kDays - 1 ? static_cast<core::DayNumber>(today - (kDays - 1)) : 0;
  if (!app.dayLog().range(first, kDays, days)) {
    for (core::DayTotals& d : days) d = core::DayTotals{};
  }
  for (uint8_t i = 0; i < kDays; ++i)
    past_[i] = static_cast<uint16_t>(days[i].reviews > 0xFFFF ? 0xFFFF : days[i].reviews);
  if (!app.progress().forecast(today, ahead_, kDays)) {
    for (uint16_t& v : ahead_) v = 0;
  }
  core::DayTotals all;
  if (!app.dayLog().allTime(all, &studyDays_)) {
    all = core::DayTotals{};
    studyDays_ = 0;
  }
  seconds_ = all.seconds;
  platform::log("progress streak %u not-started %lu learning %lu mature %lu suspended %lu today %u due %u", streak_,
                static_cast<unsigned long>(notStarted_), static_cast<unsigned long>(learning_),
                static_cast<unsigned long>(mature_), static_cast<unsigned long>(suspended_), past_[kDays - 1],
                ahead_[0]);
}

void ProgressScreen::build(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  freeink::ui::DrawTarget& t = screen.target();
  if (const char* banner = storageBanner(app_)) drawBanner(screen, theme, banner);
  const int16_t textW = i16(screen.body().width - 2 * theme.margin);

  char line[64];
  if (streak_ == 1) {
    snprintf(line, sizeof line, "%s", tr(Str::StreakOne));
  } else {
    snprintf(line, sizeof line, tr(Str::StreakFmt), streak_);
  }
  Rect r = screen.takeTop(t.lineHeight(kSlotTitle), theme.gap);
  t.text(Rect{i16(r.x + theme.margin), r.y, textW, r.height}, line, styled(kSlotTitle));

  // Four totals, two to a line.
  const Str labels[4] = {Str::NotStarted, Str::Learning, Str::Mature, Str::Suspended};
  const uint32_t values[4] = {notStarted_, learning_, mature_, suspended_};
  const int16_t half = i16(textW / 2);
  for (uint8_t row = 0; row < 2; ++row) {
    r = screen.takeTop(t.lineHeight(kSlotBody), row == 1 ? 2 * theme.gap : 4);
    for (uint8_t col = 0; col < 2; ++col) {
      const uint8_t i = static_cast<uint8_t>(row * 2 + col);
      const Rect cell{i16(r.x + theme.margin + col * half), r.y, i16(half - theme.gap), r.height};
      char value[16];
      snprintf(value, sizeof value, "%lu", static_cast<unsigned long>(values[i]));
      t.text(cell, tr(labels[i]), theme.tokens.bodyText);
      t.text(cell, value, styled(kSlotBodyBold, TextAlign::Right));
    }
  }

  const int16_t chartH = i16(app_.keyDevice() ? 96 : 88);
  drawBars(screen, theme, tr(Str::ReviewsLast14), past_, kDays, kDays - 1, chartH);
  screen.spacer(theme.gap);
  drawBars(screen, theme, tr(Str::DueNext14), ahead_, kDays, 0, chartH);
  screen.spacer(theme.gap);

  const unsigned minutes = static_cast<unsigned>((seconds_ + 30) / 60);
  snprintf(line, sizeof line, tr(Str::TimeStudiedFmt), minutes / 60, minutes % 60);
  r = screen.takeTop(t.lineHeight(kSlotBody), 4);
  t.text(Rect{i16(r.x + theme.margin), r.y, textW, r.height}, line, theme.tokens.bodyText);
  snprintf(line, sizeof line, tr(Str::StudyDaysFmt), static_cast<unsigned long>(studyDays_));
  r = screen.takeTop(t.lineHeight(kSlotSmall));
  t.text(Rect{i16(r.x + theme.margin), r.y, textW, r.height}, line, theme.tokens.smallText);
}

// Nothing to select: only Back.
bool ProgressScreen::keyHints(ChoiceBar& out) const {
  View::keyHints(out);
  for (CellSpec& cell : out.cells) {
    if (cell.label != tr(Str::Back)) cell = CellSpec{};
  }
  return true;
}

}  // namespace tinta::ui
