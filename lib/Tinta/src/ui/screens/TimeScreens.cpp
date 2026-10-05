#include "ui/screens/TimeScreens.h"

#include <stdio.h>

#include "app/App.h"
#include "platform/Log.h"
#include "ui/Fonts.h"
#include "ui/Strings.h"

namespace tinta::ui {
namespace {

using app::App;
using freeink::ui::Rect;
using freeink::ui::TextAlign;
using freeink::ui::TextStyle;
namespace date = core::date;
using RowKind = RowSpec::Kind;

int16_t i16(int32_t v) { return static_cast<int16_t>(v); }

// The X4's date for this power-on, from the prompt or a picker.
void confirmDay(App& app, const core::DayNumber day) {
  const uint32_t before = app.clock().nowSeconds();
  app.profile().lastConfirmedDay = day;
  app.clock().confirmDay(day);
  app.profileChanged();
  app.usage().clockChange(core::usage::ClockKind::DayConfirmed, before, app.clock().nowSeconds());
  app.clearTapFlash();
  if (app.inTimeStep()) {
    app.finishTimeStep();
  } else {
    app.pop();
  }
}

// A title and a wrapped paragraph above a form or a prompt.
void titleAndText(App& app, app::UiScreen& screen, const char* title, const char* text) {
  const Theme& theme = app.theme();
  freeink::ui::DrawTarget& t = screen.target();
  const int16_t width = i16(screen.body().width - 2 * theme.margin);
  if (title) {
    const Rect r = screen.takeTop(t.lineHeight(kSlotTitle), theme.gap);
    t.text(Rect{i16(r.x + theme.margin), r.y, width, r.height}, title, theme.tokens.titleText);
  }
  if (text) {
    TextStyle s = theme.tokens.bodyText;
    s.maxLines = 4;
    const freeink::ui::Size size = freeink::ui::measureWrappedText(t, text, s, width);
    const Rect r = screen.takeTop(size.height, 2 * theme.gap);
    t.text(Rect{i16(r.x + theme.margin), r.y, width, r.height}, text, s);
  }
}

void clampDay(date::Civil& c) {
  const uint8_t last = date::daysInMonth(c.year, c.month);
  if (c.day > last) c.day = last;
}

void stepDate(date::Civil& c, uint8_t field, int8_t dir) {
  switch (field) {
    case 0: {  // day, wrapping within the month
      const int last = date::daysInMonth(c.year, c.month);
      c.day = static_cast<uint8_t>((c.day - 1 + dir + last) % last + 1);
      break;
    }
    case 1:  // month, wrapping
      c.month = static_cast<uint8_t>((c.month - 1 + dir + 12) % 12 + 1);
      clampDay(c);
      break;
    case 2: {
      const int year = c.year + dir;
      if (year >= date::kFirstYear && year <= date::kLastYear) c.year = static_cast<uint16_t>(year);
      clampDay(c);
      break;
    }
    default:
      break;
  }
}

void formatDateField(const date::Civil& c, uint8_t field, char* out, size_t cap) {
  switch (field) {
    case 0:
      snprintf(out, cap, "%u", c.day);
      break;
    case 1:
      snprintf(out, cap, "%s", monthName(c.month));
      break;
    case 2:
      snprintf(out, cap, "%u", c.year);
      break;
    default:
      out[0] = '\0';
      break;
  }
}

const Str kDateLabels[] = {Str::Day, Str::Month, Str::Year};
const char* const kDateSlugs[] = {"day", "month", "year"};

}  // namespace

// ── What day is it? ──────────────────────────────────────────────────────────

const char* DatePromptScreen::title() const { return tr(Str::AppName); }

void DatePromptScreen::enter(bool) {
  last_ = app_.profile().lastConfirmedDay;
  formatDate(last_, DateStyle::Short, sameText_, sizeof sameText_);
  formatDate(static_cast<core::DayNumber>(last_ + 1), DateStyle::Short, nextText_, sizeof nextText_);
  bar_ = ChoiceBar{};
  bar_.cells[0].label = tr(Str::SameDay);
  bar_.cells[0].detail = sameText_;
  bar_.cells[1].label = tr(Str::NextDay);
  bar_.cells[1].detail = nextText_;
  bar_.cells[2].label = tr(Str::OtherDate);
}

void DatePromptScreen::build(app::UiScreen& screen) {
  char last[64];
  char line[96];
  formatDate(last_, DateStyle::Long, last, sizeof last);
  snprintf(line, sizeof line, tr(Str::LastTimeFmt), last);
  screen.spacer(app_.theme().rowHeight);
  titleAndText(app_, screen, tr(Str::WhatDayIsIt), line);
}

void DatePromptScreen::onAction(const app::ActionEvent& event) {
  if (event.action != app::kActionChoice) return;
  switch (event.value) {
    case 0:
      platform::log("date same");
      confirmDay(app_, last_);
      break;
    case 1:
      platform::log("date next");
      confirmDay(app_, static_cast<core::DayNumber>(last_ + 1));
      break;
    case 2:
      app_.clearTapFlash();
      app_.push(app::ScreenId::DatePicker);
      break;
    default:
      break;
  }
}

// ── Date picker ──────────────────────────────────────────────────────────────

const char* DatePickerScreen::title() const { return tr(Str::SetDateTitle); }

bool DatePickerScreen::allowsGlobalGestures() const { return !app_.inTimeStep(); }

void DatePickerScreen::enter(const bool returning) {
  FormView::enter(returning);
  if (returning) return;
  core::DayNumber day = app_.profile().lastConfirmedDay;
  if (day == 0) day = platform::kFirmwareDay;
  date_ = date::civil(day);
}

// Cancel only when there is somewhere to go back to.
uint8_t DatePickerScreen::rowCount() const { return app_.depth() > 1 ? 5 : 4; }

void DatePickerScreen::row(const uint8_t index, RowSpec& out, char* value, const size_t cap) const {
  if (index < 3) {
    out.kind = RowKind::Stepper;
    out.label = tr(kDateLabels[index]);
    out.slug = kDateSlugs[index];
    formatDateField(date_, index, value, cap);
    out.value = value;
  } else {
    out.kind = RowKind::Action;
    out.label = tr(index == 3 ? Str::Save : Str::Cancel);
    out.slug = index == 3 ? "save" : "cancel";
  }
}

void DatePickerScreen::step(const uint8_t index, const int8_t dir) { stepDate(date_, index, dir); }

void DatePickerScreen::activate(const uint8_t index) {
  if (index == 4) {
    app_.clearTapFlash();
    app_.pop();
    return;
  }
  if (index != 3 || !date::valid(date_)) return;
  platform::log("date picked %04u-%02u-%02u", date_.year, date_.month, date_.day);
  confirmDay(app_, date::dayNumber(date_));
}

void DatePickerScreen::buildHeader(app::UiScreen& screen) {
  if (app_.inTimeStep() && app_.profile().lastConfirmedDay == 0) {
    titleAndText(app_, screen, tr(Str::Welcome), tr(Str::FirstRunDate));
  }
}

// ── Set clock ────────────────────────────────────────────────────────────────

namespace {

const Str kClockLabels[] = {Str::Year, Str::Month, Str::Day, Str::Hour, Str::Minute};
const char* const kClockSlugs[] = {"year", "month", "day", "hour", "minute"};

}  // namespace

const char* SetClockScreen::title() const { return tr(Str::SetClockTitle); }

bool SetClockScreen::allowsGlobalGestures() const { return !app_.inTimeStep(); }

void SetClockScreen::enter(const bool returning) {
  FormView::enter(returning);
  if (returning) return;
  hadReading_ = app_.clock().localTime(reading_);
  if (hadReading_ && app_.clock().trusted()) {
    time_ = reading_;
  } else {
    time_ = platform::LocalTime{};
    time_.date = date::civil(platform::kFirmwareDay);
    time_.hour = 12;
  }
  time_.second = 0;
}

uint8_t SetClockScreen::rowCount() const { return app_.depth() > 1 ? 7 : 6; }

void SetClockScreen::row(const uint8_t index, RowSpec& out, char* value, const size_t cap) const {
  if (index < 5) {
    out.kind = RowKind::Stepper;
    out.label = tr(kClockLabels[index]);
    out.slug = kClockSlugs[index];
    switch (index) {
      case 0:
        formatDateField(time_.date, 2, value, cap);
        break;
      case 1:
        formatDateField(time_.date, 1, value, cap);
        break;
      case 2:
        formatDateField(time_.date, 0, value, cap);
        break;
      case 3:
        snprintf(value, cap, "%02u", time_.hour);
        break;
      default:
        snprintf(value, cap, "%02u", time_.minute);
        break;
    }
    out.value = value;
  } else {
    out.kind = RowKind::Action;
    out.label = tr(index == 5 ? Str::Save : Str::Cancel);
    out.slug = index == 5 ? "save" : "cancel";
  }
}

void SetClockScreen::step(const uint8_t index, const int8_t dir) {
  switch (index) {
    case 0:
      stepDate(time_.date, 2, dir);
      break;
    case 1:
      stepDate(time_.date, 1, dir);
      break;
    case 2:
      stepDate(time_.date, 0, dir);
      break;
    case 3:
      time_.hour = static_cast<uint8_t>((time_.hour + dir + 24) % 24);
      break;
    case 4:
      time_.minute = static_cast<uint8_t>((time_.minute + dir + 60) % 60);
      break;
    default:
      break;
  }
}

void SetClockScreen::activate(const uint8_t index) {
  if (index == 6) {
    app_.clearTapFlash();
    app_.pop();
    return;
  }
  if (index != 5) return;
  const uint32_t before = app_.clock().nowSeconds();
  if (!app_.clock().setLocalTime(time_)) {
    platform::log("clock set failed");
    return;
  }
  app_.usage().clockChange(core::usage::ClockKind::Set, before, app_.clock().nowSeconds());
  app_.profile().lastConfirmedDay = app_.clock().today();
  app_.profileChanged();
  app_.clearTapFlash();
  if (app_.inTimeStep()) {
    app_.finishTimeStep();
  } else {
    app_.pop();
  }
}

void SetClockScreen::buildHeader(app::UiScreen& screen) {
  if (!app_.inTimeStep()) return;
  if (app_.firstRun() || !hadReading_) {
    titleAndText(app_, screen, tr(Str::Welcome), tr(Str::FirstRunClock));
    return;
  }
  char reading[48];
  char text[160];
  snprintf(reading, sizeof reading, "%04u-%02u-%02u %02u:%02u", reading_.date.year, reading_.date.month,
           reading_.date.day, reading_.hour, reading_.minute);
  snprintf(text, sizeof text, tr(Str::ClockUnsetFmt), reading);
  titleAndText(app_, screen, nullptr, text);
}

}  // namespace tinta::ui
