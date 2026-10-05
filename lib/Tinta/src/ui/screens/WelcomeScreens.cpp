#include "ui/screens/WelcomeScreens.h"

#include <stdio.h>

#include "app/App.h"
#include "platform/Log.h"
#include "ui/Fonts.h"
#include "ui/Strings.h"

namespace tinta::ui {
namespace {

using app::App;
using app::ScreenId;
using core::UiLanguage;
using freeink::ui::Rect;
using freeink::ui::TextStyle;
using RowKind = RowSpec::Kind;

int16_t i16(int32_t v) { return static_cast<int16_t>(v); }

// A wrapped paragraph across the body, inside the margins.
void paragraph(App& app, app::UiScreen& screen, const char* text, TextStyle style, const int16_t gapAfter) {
  const Theme& theme = app.theme();
  freeink::ui::DrawTarget& t = screen.target();
  const int16_t width = i16(screen.body().width - 2 * theme.margin);
  style.maxLines = 8;
  const freeink::ui::Size size = freeink::ui::measureWrappedText(t, text, style, width);
  const Rect r = screen.takeTop(size.height, gapAfter);
  t.text(Rect{i16(r.x + theme.margin), r.y, width, r.height}, text, style);
}

const UiLanguage kLanguages[] = {UiLanguage::English, UiLanguage::Spanish, UiLanguage::Auto};
const char* const kLanguageSlugs[] = {"english", "spanish", "auto"};

}  // namespace

// ── Language ─────────────────────────────────────────────────────────────────

const char* WelcomeScreen::title() const { return tr(Str::AppName); }

// Nothing is chosen yet: the welcome is in both languages.
void WelcomeScreen::buildHeader(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  screen.spacer(theme.gap);
  for (const UiLanguage language : {UiLanguage::English, UiLanguage::Spanish}) {
    paragraph(app_, screen, trIn(Str::Welcome, language), theme.tokens.titleText, theme.gap);
    paragraph(app_, screen, trIn(Str::WelcomeChoose, language), theme.tokens.bodyText, 3 * theme.gap);
  }
}

void WelcomeScreen::row(const uint8_t index, RowSpec& out, char*, size_t) const {
  out.kind = RowKind::Action;
  out.slug = kLanguageSlugs[index];
  switch (index) {
    case 0:
      out.label = trIn(Str::LangEnglish, UiLanguage::English);
      break;
    case 1:
      out.label = trIn(Str::LangSpanish, UiLanguage::English);
      break;
    default:
      out.label = "Auto / Automático";
      break;
  }
}

uint8_t WelcomeScreen::initialRow() const {
  const auto i = static_cast<uint8_t>(app_.profile().uiLanguage);
  return i < 3 ? i : 0;
}

void WelcomeScreen::activate(const uint8_t index) {
  if (index >= 3) return;
  app_.profile().uiLanguage = kLanguages[index];
  app_.profileChanged();
  app_.usage().setting("uiLanguage", index);
  platform::log("set uiLanguage %s", kLanguageSlugs[index]);
  app_.clearTapFlash();
  if (app_.clock().trusted()) {
    app_.push(ScreenId::KeyGuide);
    return;
  }
  // Without a trusted clock, the date first, then the guide.
  const ScreenId next[] = {ScreenId::Welcome, ScreenId::KeyGuide};
  app_.beginTimeStep(next, 2);
}

// ── How the keys work ────────────────────────────────────────────────────────

const char* KeyGuideScreen::title() const { return tr(app_.keyDevice() ? Str::KeysTitle : Str::TouchTitle); }

bool KeyGuideScreen::firstRun() const { return app_.idAt(0) == ScreenId::Welcome; }

bool KeyGuideScreen::allowsGlobalGestures() const { return !firstRun(); }

void KeyGuideScreen::build(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  const TextStyle body = theme.tokens.bodyText;
  const int16_t gap = i16(2 * theme.gap);
  screen.spacer(theme.gap);
  if (app_.keyDevice()) {
    char hold[128];
    snprintf(hold, sizeof hold, tr(Str::KeysHoldBackFmt), tr(Str::KeyBack));
    paragraph(app_, screen, tr(Str::KeysFront), body, gap);
    paragraph(app_, screen, tr(Str::KeysSide), body, gap);
    paragraph(app_, screen, hold, body, gap);
  } else {
    paragraph(app_, screen, tr(Str::TouchTap), body, gap);
    paragraph(app_, screen, tr(Str::TouchCard), body, gap);
    if (app_.board().hasHomePad()) paragraph(app_, screen, tr(Str::TouchHome), body, gap);
    if (app_.board().hasFrontlight()) paragraph(app_, screen, tr(Str::TouchLight), body, gap);
  }
  paragraph(app_, screen, tr(Str::KeysPower), body, gap);
}

void KeyGuideScreen::next() {
  if (!firstRun()) return;
  app_.clearTapFlash();
  app_.push(ScreenId::VulgarChoice);
}

// Touch: Next in the footer during the first run; otherwise the status bar's
// arrow goes back.
const ChoiceBar* KeyGuideScreen::choiceBar() const {
  if (app_.keyDevice() || !firstRun()) return nullptr;
  ChoiceBar& bar = const_cast<ChoiceBar&>(bar_);
  bar = ChoiceBar{};
  bar.cells[1].label = tr(Str::Next);
  return &bar_;
}

bool KeyGuideScreen::keyHints(ChoiceBar& out) const {
  View::keyHints(out);
  for (uint8_t i = 0; i < kFooterCellCount; ++i) {
    CellSpec& cell = out.cells[i];
    if (app_.keys().footerCell(i).key == Key::Back) continue;
    cell = CellSpec{};
    if (app_.keys().footerCell(i).key == Key::Confirm && firstRun()) cell.label = tr(Str::Next);
  }
  return true;
}

void KeyGuideScreen::onAction(const app::ActionEvent& event) {
  if (event.action == app::kActionChoice && event.value == 1) next();
}

bool KeyGuideScreen::onInput(const InputEvent& event) {
  if (event.kind != InputEvent::Kind::Key || event.hold || event.key != Key::Confirm) return false;
  next();
  return true;
}

// ── Vulgar words ─────────────────────────────────────────────────────────────

const char* VulgarChoiceScreen::title() const { return tr(Str::VulgarTitle); }

void VulgarChoiceScreen::buildHeader(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  screen.spacer(theme.gap);
  paragraph(app_, screen, tr(Str::VulgarText), theme.tokens.bodyText, 3 * theme.gap);
}

void VulgarChoiceScreen::row(const uint8_t index, RowSpec& out, char*, size_t) const {
  if (index == 0) {
    out.kind = RowKind::Toggle;
    out.label = tr(Str::ShowVulgar);
    out.slug = "show-vulgar";
    out.checked = app_.profile().showVulgar;
    return;
  }
  out.kind = RowKind::Action;
  out.label = tr(Str::Start);
  out.slug = "start";
}

void VulgarChoiceScreen::step(const uint8_t index, int8_t) {
  if (index != 0) return;
  core::Profile& p = app_.profile();
  p.showVulgar = !p.showVulgar;
  app_.profileChanged();
  app_.usage().setting("showVulgar", p.showVulgar);
  platform::log("set showVulgar %s", p.showVulgar ? "on" : "off");
}

void VulgarChoiceScreen::activate(const uint8_t index) {
  if (index != 1) return;
  app_.goHome();
}

}  // namespace tinta::ui
