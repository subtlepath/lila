#include "ui/screens/SettingsScreens.h"

#include <stdio.h>

#include "app/App.h"
#include "core/lang/Charset.h"
#include "platform/Log.h"
#include "ui/Strings.h"
#include "ui/screens/HomeScreen.h"
#include "ui/views/Chrome.h"

namespace tinta::ui {

using app::App;
using app::ScreenId;
using RowKind = RowSpec::Kind;

struct Field {
  Str label;
  const char* slug;
  RowKind kind;
  void (*format)(App& app, char* out, size_t cap);  // the value; null for none
  void (*step)(App& app, int8_t dir);               // Stepper, Choice and Toggle
  bool (*checked)(App& app);                        // Toggle
  ScreenId link;                                    // Link
  bool (*shown)(App& app);                          // null: always
  int32_t (*number)(App& app);                      // the profile's value, for the usage log
};

namespace {

template <typename T>
void stepClamped(T& value, int8_t dir, int step, int lo, int hi) {
  int v = static_cast<int>(value) + dir * step;
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  value = static_cast<T>(v);
}

// Moves to the next or previous preset from wherever `value` is.
uint16_t stepPreset(uint16_t value, int8_t dir, const uint16_t* presets, uint8_t count) {
  uint8_t i = 0;
  while (i + 1 < count && presets[i] < value) ++i;
  if (dir > 0 && presets[i] <= value && i + 1 < count) ++i;
  if (dir < 0 && i > 0) --i;
  return presets[i];
}

template <typename E>
void cycle(E& value, int8_t dir, uint8_t count) {
  const int v = (static_cast<int>(value) + dir + count) % count;
  value = static_cast<E>(v);
}

void formatNumber(char* out, size_t cap, unsigned value) { snprintf(out, cap, "%u", value); }

void formatOffset(char* out, size_t cap, int16_t minutes) {
  if (minutes == 0) {
    snprintf(out, cap, "UTC");
    return;
  }
  const unsigned m = static_cast<unsigned>(minutes < 0 ? -minutes : minutes);
  snprintf(out, cap, "UTC%s%02u:%02u", minutes < 0 ? TINTA_EN_DASH : "+", m / 60, m % 60);
}

bool hasTime(App& app) { return app.clock().hasTimeOfDay(); }
bool noTime(App& app) { return !app.clock().hasTimeOfDay(); }

constexpr uint16_t kIntervalPresets[] = {30, 60, 90, 180, 365, 730, 1825, 3650, 36500};

const Field kStudy[] = {
    {Str::NewPerDay, "newPerDay", RowKind::Stepper,
     [](App& a, char* o, size_t c) { formatNumber(o, c, a.profile().newPerDay); },
     [](App& a, int8_t d) { stepClamped(a.profile().newPerDay, d, 5, 0, 200); }, nullptr, ScreenId::None, nullptr,
     [](App& a) -> int32_t { return a.profile().newPerDay; }},
    {Str::ReviewCap, "reviewCap", RowKind::Stepper,
     [](App& a, char* o, size_t c) { formatNumber(o, c, a.profile().reviewCap); },
     [](App& a, int8_t d) { stepClamped(a.profile().reviewCap, d, 10, 0, 9999); }, nullptr, ScreenId::None, nullptr,
     [](App& a) -> int32_t { return a.profile().reviewCap; }},
    {Str::Retention, "retention", RowKind::Stepper,
     [](App& a, char* o, size_t c) { snprintf(o, c, tr(Str::PercentFmt), a.profile().retentionPermille / 10u); },
     [](App& a, int8_t d) { stepClamped(a.profile().retentionPermille, d, 10, 700, 970); }, nullptr, ScreenId::None,
     nullptr, [](App& a) -> int32_t { return a.profile().retentionPermille; }},
    {Str::MaxInterval, "maxInterval", RowKind::Stepper,
     [](App& a, char* o, size_t c) { snprintf(o, c, tr(Str::DaysFmt), a.profile().maxInterval); },
     [](App& a, int8_t d) {
       a.profile().maxInterval = stepPreset(a.profile().maxInterval, d, kIntervalPresets,
                                            sizeof kIntervalPresets / sizeof kIntervalPresets[0]);
     },
     nullptr, ScreenId::None, nullptr, [](App& a) -> int32_t { return a.profile().maxInterval; }},
    {Str::SessionSize, "sessionSize", RowKind::Stepper,
     [](App& a, char* o, size_t c) { snprintf(o, c, tr(Str::ItemsFmt), a.profile().sessionSize); },
     [](App& a, int8_t d) { stepClamped(a.profile().sessionSize, d, 5, 5, app::SessionController::kCapacity); },
     nullptr, ScreenId::None, nullptr, [](App& a) -> int32_t { return a.profile().sessionSize; }},
    {Str::ShowVulgar, "showVulgar", RowKind::Toggle, nullptr,
     [](App& a, int8_t) { a.profile().showVulgar = !a.profile().showVulgar; },
     [](App& a) { return a.profile().showVulgar; }, ScreenId::None, nullptr,
     [](App& a) -> int32_t { return a.profile().showVulgar; }},
    // Typing needs the X4 Pro's touch keyboard.
    {Str::TypedAnswers, "typedAnswers", RowKind::Toggle, nullptr,
     [](App& a, int8_t) { a.profile().typedAnswers = !a.profile().typedAnswers; },
     [](App& a) { return a.profile().typedAnswers; }, ScreenId::None, [](App& a) { return !a.keyDevice(); },
     [](App& a) -> int32_t { return a.profile().typedAnswers; }},
};

const Field kDisplay[] = {
    {Str::TextSize, "textSize", RowKind::Choice,
     [](App& a, char* o, size_t c) {
       static const Str kNames[] = {Str::TextSmall, Str::TextMedium, Str::TextLarge};
       snprintf(o, c, "%s", tr(kNames[static_cast<uint8_t>(a.profile().textSize)]));
     },
     [](App& a, int8_t d) { cycle(a.profile().textSize, d, 3); }, nullptr, ScreenId::None, nullptr,
     [](App& a) -> int32_t { return static_cast<int32_t>(a.profile().textSize); }},
    {Str::UiLanguage, "uiLanguage", RowKind::Choice,
     [](App& a, char* o, size_t c) {
       // Auto says which language it has picked for now.
       const bool spanish = language() == core::UiLanguage::Spanish;
       static const Str kNames[] = {Str::LangEnglish, Str::LangSpanish, Str::LangAutoEnglish};
       const auto i = static_cast<uint8_t>(a.profile().uiLanguage);
       snprintf(o, c, "%s", tr(i == 2 && spanish ? Str::LangAutoSpanish : kNames[i]));
     },
     [](App& a, int8_t d) { cycle(a.profile().uiLanguage, d, 3); }, nullptr, ScreenId::None, nullptr,
     [](App& a) -> int32_t { return static_cast<int32_t>(a.profile().uiLanguage); }},
    {Str::FullRefreshEvery, "fullRefreshEvery", RowKind::Stepper,
     [](App& a, char* o, size_t c) { snprintf(o, c, tr(Str::ScreensFmt), a.profile().fullRefreshEvery); },
     [](App& a, int8_t d) { stepClamped(a.profile().fullRefreshEvery, d, 1, 1, 50); }, nullptr, ScreenId::None, nullptr,
     [](App& a) -> int32_t { return a.profile().fullRefreshEvery; }},
};

const Field kTime[] = {
    {Str::SetDateTime, "setClock", RowKind::Link, nullptr, nullptr, nullptr, ScreenId::SetClock, hasTime},
    {Str::TodaysDate, "date", RowKind::Link,
     [](App& a, char* o, size_t c) { formatDate(a.clock().today(), DateStyle::Short, o, c); }, nullptr, nullptr,
     ScreenId::DatePicker, noTime},
    {Str::TimeZone, "utcOffset", RowKind::Stepper,
     [](App& a, char* o, size_t c) { formatOffset(o, c, a.profile().utcOffsetMinutes); },
     [](App& a, int8_t d) { stepClamped(a.profile().utcOffsetMinutes, d, 30, -720, 840); }, nullptr, ScreenId::None,
     hasTime, [](App& a) -> int32_t { return a.profile().utcOffsetMinutes; }},
    {Str::DayStartsAt, "rolloverHour", RowKind::Stepper,
     [](App& a, char* o, size_t c) { snprintf(o, c, "%02u:00", a.profile().rolloverHour); },
     [](App& a, int8_t d) { stepClamped(a.profile().rolloverHour, d, 1, 0, 23); }, nullptr, ScreenId::None, hasTime,
     [](App& a) -> int32_t { return a.profile().rolloverHour; }},
};

struct PageInfo {
  const char* name;
  Str title;
  const Field* fields;
  uint8_t count;
};

const PageInfo kPages[] = {
    {"settings-study", Str::Study, kStudy, sizeof kStudy / sizeof kStudy[0]},
    {"settings-display", Str::Display, kDisplay, sizeof kDisplay / sizeof kDisplay[0]},
    {"settings-time", Str::DateAndTime, kTime, sizeof kTime / sizeof kTime[0]},
    // Sleep belongs to lila; the page stays so the ids keep their places.
    {"settings-sleep", Str::SleepAndPower, nullptr, 0},
};

// The root menu.
struct Group {
  Str label;
  const char* slug;
  ScreenId target;
  bool (*shown)(App& app);
};

const Group kGroups[] = {
    {Str::Study, "study", ScreenId::SettingsStudy, nullptr},
    {Str::Display, "display", ScreenId::SettingsDisplay, nullptr},
    {Str::DateAndTime, "time", ScreenId::SettingsTime, nullptr},
    {Str::Light, "light", ScreenId::Light, [](App& a) { return a.board().hasFrontlight(); }},
    {Str::About, "about", ScreenId::About, nullptr},
    {Str::Diagnostics, "diagnostics", ScreenId::Diagnostics, nullptr},
};
constexpr uint8_t kGroupCount = sizeof kGroups / sizeof kGroups[0];

int8_t groupAt(App& app, uint8_t n) {
  for (uint8_t i = 0; i < kGroupCount; ++i) {
    if (kGroups[i].shown && !kGroups[i].shown(app)) continue;
    if (n-- == 0) return static_cast<int8_t>(i);
  }
  return -1;
}

}  // namespace

// ── Root ─────────────────────────────────────────────────────────────────────

const char* SettingsScreen::title() const { return tr(Str::Settings); }

uint8_t SettingsScreen::rowCount() const {
  uint8_t count = 0;
  while (groupAt(app_, count) >= 0) ++count;
  return count;
}

void SettingsScreen::row(const uint8_t index, RowSpec& out, char*, size_t) const {
  const int8_t g = groupAt(app_, index);
  if (g < 0) return;
  out.kind = RowKind::Link;
  out.label = tr(kGroups[g].label);
  out.slug = kGroups[g].slug;
}

void SettingsScreen::activate(const uint8_t index) {
  const int8_t g = groupAt(app_, index);
  if (g < 0) return;
  app_.clearTapFlash();
  if (kGroups[g].target == ScreenId::Light) {
    app_.openLight();
  } else {
    app_.push(kGroups[g].target);
  }
}

void SettingsScreen::buildHeader(app::UiScreen& screen) {
  if (const char* banner = storageBanner(app_)) drawBanner(screen, app_.theme(), banner);
}

// ── Pages ────────────────────────────────────────────────────────────────────

const char* SettingsPage::name() const { return kPages[static_cast<uint8_t>(page_)].name; }

const char* SettingsPage::title() const { return tr(kPages[static_cast<uint8_t>(page_)].title); }

const Field* SettingsPage::field(uint8_t index) const {
  const PageInfo& page = kPages[static_cast<uint8_t>(page_)];
  for (uint8_t i = 0; i < page.count; ++i) {
    const Field& f = page.fields[i];
    if (f.shown && !f.shown(app_)) continue;
    if (index-- == 0) return &f;
  }
  return nullptr;
}

uint8_t SettingsPage::rowCount() const {
  uint8_t count = 0;
  while (field(count)) ++count;
  return count;
}

void SettingsPage::row(const uint8_t index, RowSpec& out, char* value, const size_t cap) const {
  const Field* f = field(index);
  if (!f) return;
  out.kind = f->kind;
  out.label = tr(f->label);
  out.slug = f->slug;
  if (f->format) {
    f->format(app_, value, cap);
    out.value = value;
  }
  if (f->checked) out.checked = f->checked(app_);
}

void SettingsPage::step(const uint8_t index, const int8_t dir) {
  const Field* f = field(index);
  if (!f || !f->step) return;
  const uint32_t before = app_.clock().nowSeconds();
  f->step(app_, dir);
  app_.profileChanged();
  if (f->number) app_.usage().setting(f->slug, f->number(app_));
  if (f->label == Str::TimeZone) {
    app_.usage().clockChange(core::usage::ClockKind::TimeZone, before, app_.clock().nowSeconds());
  }
  char value[kValueCap] = {};
  if (f->format) {
    f->format(app_, value, sizeof value);
  } else if (f->checked) {
    snprintf(value, sizeof value, "%s", f->checked(app_) ? "on" : "off");
  }
  platform::log("set %s %s", f->slug, value);
}

void SettingsPage::activate(const uint8_t index) {
  const Field* f = field(index);
  if (!f || f->kind != RowKind::Link) return;
  app_.clearTapFlash();
  app_.push(f->link);
}

}  // namespace tinta::ui
