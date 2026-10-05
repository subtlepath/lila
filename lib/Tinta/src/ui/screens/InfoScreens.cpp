#include "ui/screens/InfoScreens.h"

#include <Arduino.h>
#include <stdio.h>

#include "app/App.h"
#include "core/lang/Charset.h"
#include "icons/Icons.h"
#include "platform/Log.h"
#include "ui/Fonts.h"
#include "ui/Strings.h"
#include "ui/screens/HomeScreen.h"
#include "ui/views/Chrome.h"

namespace tinta::ui {
namespace {

using app::App;
using freeink::ui::Rect;
using freeink::ui::TextAlign;
using freeink::ui::TextStyle;
using RowKind = RowSpec::Kind;

int16_t i16(int32_t v) { return static_cast<int16_t>(v); }

// One "label  value" line of the About screen.
void infoLine(app::UiScreen& screen, const Theme& theme, const char* label, const char* value) {
  freeink::ui::DrawTarget& t = screen.target();
  const Rect r = screen.takeTop(t.lineHeight(kSlotBody), theme.gap);
  const int16_t labelW = i16(r.width * 2 / 5);
  t.text(Rect{i16(r.x + theme.margin), r.y, labelW, r.height}, label, theme.tokens.bodyText);
  TextStyle bold;
  bold.font = kSlotBodyBold;
  t.text(Rect{i16(r.x + theme.margin + labelW), r.y, i16(r.width - labelW - 2 * theme.margin), r.height}, value, bold);
}

}  // namespace

// ── About ────────────────────────────────────────────────────────────────────

const char* AboutScreen::title() const { return tr(Str::About); }

void AboutScreen::buildHeader(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  freeink::ui::DrawTarget& t = screen.target();
  platform::Board& board = app_.board();

  // The name in Times, the Spanish face.
  const BitmapFont& display = displayStrike(1);
  app_.target().setFont(kSlotScratch, display);
  TextStyle name;
  name.font = kSlotScratch;
  name.align = TextAlign::Center;
  t.text(screen.takeTop(display.yAdvance, theme.gap), tr(Str::AppName), name);
  TextStyle tagline;
  tagline.font = kSlotScratch;
  tagline.align = TextAlign::Center;
  const BitmapFont& aside = font(FontRole::SpanishAside);
  app_.target().setFont(kSlotScratch, aside);
  t.text(screen.takeTop(aside.yAdvance, 3 * theme.gap), "español de México", tagline);

  char version[48];
  snprintf(version, sizeof version, "lila %s", CROSSPOINT_VERSION);
  infoLine(screen, theme, tr(Str::Version), version);
  infoLine(screen, theme, tr(Str::Device), board.name());
  char panel[48];
  snprintf(panel, sizeof panel, "%s %dx%d", board.panelController(), board.logicalWidth(), board.logicalHeight());
  infoLine(screen, theme, tr(Str::Panel), panel);
  char course[48];
  const core::pack::Pack& pack = app_.pack();
  if (app_.packReady()) {
    snprintf(course, sizeof course, tr(Str::CourseEditionFmt), static_cast<unsigned long>(pack.contentVersion()));
  } else {
    snprintf(course, sizeof course, "%s", app_.packStatusName());
  }
  infoLine(screen, theme, tr(Str::Course), course);
  screen.spacer(3 * theme.gap);
}

void AboutScreen::row(const uint8_t index, RowSpec& out, char*, size_t) const {
  out.kind = RowKind::Link;
  if (index == 0) {
    out.label = tr(app_.keyDevice() ? Str::KeysTitle : Str::TouchTitle);
    out.slug = "keys";
  } else {
    out.label = tr(Str::Licences);
    out.slug = "licences";
  }
}

void AboutScreen::activate(const uint8_t index) {
  app_.clearTapFlash();
  app_.push(index == 0 ? app::ScreenId::KeyGuide : app::ScreenId::Licences);
}

// ── Licences ─────────────────────────────────────────────────────────────────

namespace {

// NOTICE, as the device shows it. Paragraphs of one page, null-terminated.
const char* const kAdobeNotice[] = {
    "Bitmap fonts from the X.org font-adobe-100dpi strikes, with the ISO 10646-1 extension by Markus Kuhn.",
    "Copyright 1984" TINTA_EN_DASH
    "1989, 1994 Adobe Systems Incorporated. Copyright 1988, 1994 Digital Equipment "
    "Corporation.",
    "Adobe is a trademark of Adobe Systems Incorporated which may be registered in certain jurisdictions. "
    "Permission to use these trademarks is hereby granted only in association with the images described in this "
    "file.",
    "Permission to use, copy, modify, distribute and sell this software and its documentation for any purpose and "
    "without fee is hereby granted, provided that the above copyright notices appear in all copies and that both "
    "those copyright notices and this permission notice appear in supporting documentation, and that the names of "
    "Adobe Systems and Digital Equipment Corporation not be used in advertising or publicity pertaining to "
    "distribution of the software without specific, written prior permission. Adobe Systems and Digital Equipment "
    "Corporation make no representations about the suitability of this software for any purpose. It is "
    "provided " TINTA_LDQUO "as is" TINTA_RDQUO " without express or implied warranty.",
    "Times and Helvetica are trademarks of Linotype-Hell AG and/or its subsidiaries.",
    nullptr,
};

const char* const kLargerNotice[] = {
    "The 22 and 29 px text sizes: CrossPoint Reader" TINTA_RSQUO
    "s strikes, grid-fitted from URW++ Nimbus Roman "
    "and Nimbus Sans outlines and edited by hand.",
    "Headwords: TeX Gyre Termes Bold 2.004, rasterised with FreeType. Copyright 2006, 2009 for TeX Gyre extensions "
    "by B. Jackowski and J.M. Nowacki (on behalf of TeX users groups). This work is released under the GUST Font "
    "License " TINTA_EM_DASH
    " see tug.org/fonts/licenses/GUST-FONT-LICENSE.txt for details. TeX Gyre Termes is "
    "derived from URW++ Nimbus Roman No9 L.",
    nullptr,
};

const char* const kSoftwareNotice[] = {
    "FreeInk SDK. Copyright (c) 2026 FreeInk. MIT License. Based in part on the OpenX4 E-Paper Community SDK, "
    "Copyright (c) 2025 Open X4 E-Paper Contributors, MIT License.",
    "The scheduler is a C++ port of py-fsrs 6.3.2. Copyright (c) Open Spaced Repetition. MIT License.",
    "SdFat by Bill Greiman. MIT License.",
    "The MIT License is on the next page.",
    "Arduino core for the ESP32. Copyright (c) Espressif Systems and contributors. GNU Lesser General Public "
    "License 2.1 or later.",
    "ESP-IDF. Copyright (c) Espressif Systems (Shanghai) CO LTD. Apache License 2.0. With the components it "
    "bundles: FreeRTOS (MIT License), newlib, mbedTLS (Apache License 2.0) and, on the X4 Classic and X4 Pro, "
    "TinyUSB (MIT License).",
    nullptr,
};

const char* const kMitText[] = {
    "Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated "
    "documentation files (the " TINTA_LDQUO "Software" TINTA_RDQUO
    "), to deal in the Software without restriction, "
    "including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or "
    "sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the "
    "following conditions:",
    "The above copyright notice and this permission notice shall be included in all copies or substantial portions "
    "of the Software.",
    "THE SOFTWARE IS PROVIDED " TINTA_LDQUO "AS IS" TINTA_RDQUO
    ", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, "
    "INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND "
    "NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER "
    "LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE "
    "SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.",
    nullptr,
};

struct LicencePage {
  Str heading;
  const char* const* paragraphs;  // null: the heading's own translated text
  Str text;
};

const LicencePage kLicencePages[] = {
    {Str::LicTimesHelvetica, kAdobeNotice, Str::Count}, {Str::LicLargerType, kLargerNotice, Str::Count},
    {Str::LicSoftware, kSoftwareNotice, Str::Count},    {Str::LicMit, kMitText, Str::Count},
    {Str::Course, nullptr, Str::LicCourseText},
};
constexpr uint8_t kLicencePageCount = sizeof kLicencePages / sizeof kLicencePages[0];

bool swipe(const InputEvent& e, const freeink::ui::SwipeDir dir) {
  return e.kind == InputEvent::Kind::Swipe && e.swipe == dir && !e.fromTopEdge;
}

}  // namespace

const char* LicencesScreen::title() const { return tr(Str::Licences); }

void LicencesScreen::enter(const bool returning) {
  if (!returning) page_ = 0;
  platform::log("licences page %u/%u", page_ + 1, kLicencePageCount);
}

void LicencesScreen::build(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  freeink::ui::DrawTarget& t = screen.target();
  const LicencePage& page = kLicencePages[page_];
  const int16_t width = i16(screen.body().width - 2 * theme.margin);

  char number[16];
  snprintf(number, sizeof number, tr(Str::PageFmt), page_ + 1, kLicencePageCount);
  TextStyle small = theme.tokens.smallText;
  small.align = TextAlign::Right;
  const Rect numberRect = screen.takeBottom(t.lineHeight(kSlotSmall), 2);
  t.text(Rect{i16(numberRect.x + theme.margin), numberRect.y, width, numberRect.height}, number, small);

  Rect r = screen.takeTop(t.lineHeight(kSlotTitle), 2 * theme.gap);
  t.text(Rect{i16(r.x + theme.margin), r.y, width, r.height}, tr(page.heading), theme.tokens.titleText);
  TextStyle body = theme.tokens.smallText;
  body.maxLines = 40;
  const char* const single[] = {page.paragraphs ? nullptr : tr(page.text), nullptr};
  for (const char* const* p = page.paragraphs ? page.paragraphs : single; *p; ++p) {
    const freeink::ui::Size size = freeink::ui::measureWrappedText(t, *p, body, width);
    r = screen.takeTop(size.height, theme.gap);
    if (r.empty()) break;
    t.text(Rect{i16(r.x + theme.margin), r.y, width, r.height}, *p, body);
  }
}

void LicencesScreen::turn(const int8_t dir) {
  const int next = page_ + dir;
  if (next < 0 || next >= kLicencePageCount) return;
  page_ = static_cast<uint8_t>(next);
  platform::log("licences page %u/%u", page_ + 1, kLicencePageCount);
  app_.clearTapFlash();
  app_.invalidateCard();
}

const ChoiceBar* LicencesScreen::choiceBar() const {
  if (app_.keyDevice()) return nullptr;
  ChoiceBar& bar = const_cast<ChoiceBar&>(bar_);
  bar = ChoiceBar{};
  bar.cells[0].icon = &icons::kChevronLeft24;
  bar.cells[0].enabled = page_ > 0;
  bar.cells[3].icon = &icons::kChevronRight24;
  bar.cells[3].enabled = page_ + 1 < kLicencePageCount;
  return &bar_;
}

bool LicencesScreen::keyHints(ChoiceBar& out) const {
  View::keyHints(out);
  for (uint8_t i = 0; i < kFooterCellCount; ++i) {
    CellSpec& cell = out.cells[i];
    switch (app_.keys().footerCell(i).key) {
      case Key::Confirm:
        cell = CellSpec{};
        break;
      case Key::Left:
        cell = CellSpec{};
        if (page_ > 0) cell.icon = &icons::kChevronLeft24;
        break;
      case Key::Right:
        cell = CellSpec{};
        if (page_ + 1 < kLicencePageCount) cell.icon = &icons::kChevronRight24;
        break;
      default:
        break;
    }
  }
  return true;
}

void LicencesScreen::onAction(const app::ActionEvent& event) {
  if (event.action != app::kActionChoice) return;
  if (event.value == 0) turn(-1);
  if (event.value == 3) turn(1);
}

bool LicencesScreen::onInput(const InputEvent& event) {
  if (swipe(event, freeink::ui::SwipeDir::Left)) {
    turn(1);
    return true;
  }
  if (swipe(event, freeink::ui::SwipeDir::Right)) {
    turn(-1);
    return true;
  }
  if (!app_.keyDevice() || event.kind != InputEvent::Kind::Key || event.hold) return false;
  switch (event.key) {
    case Key::Left:
    case Key::Up:
      turn(-1);
      return true;
    case Key::Right:
    case Key::Down:
    case Key::Confirm:
      turn(1);
      return true;
    default:
      return false;
  }
}

// ── Diagnostics ──────────────────────────────────────────────────────────────

namespace {

enum class Diag : uint8_t {
  CheckCourse,
  UnlockLessons,
  Status,
  Card,
  Clock,
  Today,
  Course,
  Lesson,
  Unlocked,
  Profile,
  Count,
};

}  // namespace

const char* DiagnosticsScreen::title() const { return tr(Str::Diagnostics); }

uint8_t DiagnosticsScreen::rowCount() const { return static_cast<uint8_t>(Diag::Count); }

void DiagnosticsScreen::row(const uint8_t index, RowSpec& out, char* value, const size_t cap) const {
  out.kind = RowKind::Info;
  out.value = value;
  value[0] = '\0';
  switch (static_cast<Diag>(index)) {
    case Diag::CheckCourse:
      // verifyCrc() reads the whole pack: only on request (docs/pack-format.md 4).
      out.kind = RowKind::Action;
      out.label = tr(Str::CheckCourse);
      out.slug = "check-course";
      out.enabled = app_.packReady();
      if (packCheck_ != 0) snprintf(value, cap, "%s", tr(packCheck_ == 1 ? Str::CourseIntact : Str::CourseDamaged));
      out.value = packCheck_ != 0 ? value : nullptr;
      break;
    case Diag::UnlockLessons:
      // For testing and for learners who already know the early units.
      out.kind = RowKind::Action;
      out.label = tr(Str::UnlockAll);
      out.slug = "unlock-lessons";
      out.enabled = app_.packReady() && app_.profile().unlockedThrough + 1u < app_.lessonCount();
      out.value = nullptr;
      break;
    case Diag::Status:
      out.kind = RowKind::Heading;
      out.label = tr(Str::Device);
      break;
    case Diag::Course:
      out.label = tr(Str::Course);
      if (app_.packReady()) {
        snprintf(value, cap, tr(Str::CourseItemsFmt), static_cast<unsigned long>(app_.pack().contentVersion()),
                 static_cast<unsigned long>(app_.pack().itemCount()));
      } else {
        snprintf(value, cap, "%s", app_.packStatusName());
      }
      break;
    case Diag::Card:
      out.label = tr(Str::StorageInfo);
      snprintf(value, cap, "%s",
               tr(app_.storage().failed()      ? Str::StorageFailed
                  : app_.storage().available() ? Str::StorageReady
                                               : Str::StorageAbsent));
      break;
    case Diag::Clock:
      out.label = tr(Str::ClockSource);
      snprintf(value, cap, "%s", tr(app_.clock().hasTimeOfDay() ? Str::ClockRtc : Str::ClockAsked));
      break;
    case Diag::Today:
      out.label = tr(Str::Day);
      formatDate(app_.clock().today(), DateStyle::Short, value, cap);
      break;
    case Diag::Lesson:
      out.label = tr(Str::CourseLesson);
      snprintf(value, cap, "%u", app_.profile().currentLesson);
      break;
    case Diag::Unlocked:
      out.label = tr(Str::CourseUnlocked);
      snprintf(value, cap, "%u", app_.profile().unlockedThrough);
      break;
    case Diag::Profile:
      out.label = tr(Str::BoardProfile);
      snprintf(value, cap, "%s", app_.board().profileName());
      break;
    case Diag::Count:
      break;
  }
}

void DiagnosticsScreen::activate(const uint8_t index) {
  if (index == static_cast<uint8_t>(Diag::CheckCourse)) {
    const uint32_t started = millis();
    packCheck_ = app_.pack().verifyCrc() ? 1 : 2;
    platform::log("pack crc %s in %lu ms", packCheck_ == 1 ? "ok" : "bad",
                  static_cast<unsigned long>(millis() - started));
    return;
  }
  if (index == static_cast<uint8_t>(Diag::UnlockLessons)) {
    app_.unlockAllLessons();
    return;
  }
}

// ── Pack error ───────────────────────────────────────────────────────────────

const char* PackErrorScreen::title() const { return tr(Str::AppName); }

void PackErrorScreen::row(const uint8_t index, RowSpec& out, char*, size_t) const {
  out.kind = index == 0 ? RowKind::Link : RowKind::Action;
  out.label = tr(index == 0 ? Str::Settings : Str::Leave);
  out.slug = index == 0 ? "settings" : "leave";
}

void PackErrorScreen::activate(const uint8_t index) {
  app_.clearTapFlash();
  if (index == 0) {
    app_.push(app::ScreenId::Settings);
  } else {
    app_.requestExit();
  }
}

void PackErrorScreen::onBack() { app_.requestExit(); }

void PackErrorScreen::buildHeader(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  freeink::ui::DrawTarget& t = screen.target();
  if (const char* banner = storageBanner(app_)) drawBanner(screen, theme, banner);
  const int16_t width = i16(screen.body().width - 2 * theme.margin);
  screen.spacer(theme.gap);
  drawIcon(t, screen.takeTop(36, theme.gap), icons::kWarning32);
  const Rect title = screen.takeTop(t.lineHeight(kSlotTitle), theme.gap);
  TextStyle titleStyle = theme.tokens.titleText;
  titleStyle.align = TextAlign::Center;
  t.text(title, tr(Str::PackErrorTitle), titleStyle);
  char text[192];
  snprintf(text, sizeof text, tr(Str::PackErrorFmt), app_.packStatusName());
  TextStyle body = theme.tokens.bodyText;
  body.maxLines = 6;
  const freeink::ui::Size size = freeink::ui::measureWrappedText(t, text, body, width);
  const Rect r = screen.takeTop(size.height, 3 * theme.gap);
  t.text(Rect{i16(r.x + theme.margin), r.y, width, r.height}, text, body);
}

}  // namespace tinta::ui
