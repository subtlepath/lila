#include "ui/Fonts.h"

#include "core/text/Typesetter.h"
#include "fonts/Strikes.h"

namespace tinta::ui {

namespace {

using namespace tinta::fonts;

// [role][text size], rows in FontRole order.
const BitmapFont* const kRoleFonts[kFontRoleCount][kTextSizeCount] = {
    {&kTimesBold34, &kTimesBold34, &kTimesBold34},                       // SpanishDisplay: see font()
    {&kTimesRoman22, &kTimesRoman25, &kTimesRoman29},                    // SpanishText
    {&kTimesBold22, &kTimesBold25, &kTimesBold29},                       // SpanishEmphasis
    {&kTimesItalic22, &kTimesItalic25, &kTimesItalic29},                 // SpanishAside
    {&kHelvetica22, &kHelvetica25, &kHelvetica29},                       // EnglishGloss
    {&kHelvetica17, &kHelvetica20, &kHelvetica22},                       // EnglishTranslation
    {&kHelveticaOblique17, &kHelveticaOblique20, &kHelveticaOblique22},  // Respelling
    {&kHelveticaBold14, &kHelveticaBold14, &kHelveticaBold17},           // Label
    {&kHelvetica17, &kHelvetica17, &kHelvetica17},                       // ChromeSmall
    {&kHelvetica20, &kHelvetica20, &kHelvetica20},                       // ChromeBody
    {&kHelveticaBold25, &kHelveticaBold25, &kHelveticaBold25},           // ChromeTitle
    {&kHelveticaBold20, &kHelveticaBold20, &kHelveticaBold20},           // ChromeBodyBold
    {&kHelvetica34, &kHelvetica34, &kHelvetica34},                       // KeyboardKey
    {&kHelveticaBold17, &kHelveticaBold17, &kHelveticaBold17},           // KeyboardLabel
};

const char* const kRoleNames[kFontRoleCount] = {
    "Spanish display", "Spanish text",   "Spanish emphasis", "Spanish aside", "English gloss", "English translation",
    "Respelling",      "Label",          "Chrome small",     "Chrome body",   "Chrome title",  "Chrome body bold",
    "Keyboard key",    "Keyboard label",
};

}  // namespace

const BitmapFont& displayStrike(uint8_t index) {
  static const BitmapFont* const kStrikes[kDisplayStrikeCount] = {&kTermesBold87, &kTermesBold75, &kTermesBold68,
                                                                  &kTermesBold58, &kTermesBold50, &kTimesBold34};
  return *kStrikes[index < kDisplayStrikeCount ? index : kDisplayStrikeCount - 1];
}

const BitmapFont& font(FontRole role, TextSize size) {
  if (role == FontRole::SpanishDisplay) return displayStrike(0);
  const uint8_t r = static_cast<uint8_t>(role) < kFontRoleCount ? static_cast<uint8_t>(role) : 0;
  const uint8_t s = static_cast<uint8_t>(size) < kTextSizeCount ? static_cast<uint8_t>(size) : 1;
  return *kRoleFonts[r][s];
}

const char* roleName(FontRole role) {
  const uint8_t r = static_cast<uint8_t>(role);
  return r < kFontRoleCount ? kRoleNames[r] : "";
}

void applyFontSlots(freeink::ui::DisplayTarget& target) {
  target.setFont(kSlotSmall, font(FontRole::ChromeSmall));
  target.setFont(kSlotBody, font(FontRole::ChromeBody));
  target.setFont(kSlotTitle, font(FontRole::ChromeTitle));
  target.setFont(kSlotBodyBold, font(FontRole::ChromeBodyBold));
  target.setFont(kSlotKey, font(FontRole::KeyboardKey));
  target.setFont(kSlotKeyLabel, font(FontRole::KeyboardLabel));
}

const BitmapFont& fitHeadword(const char* text, size_t length, int16_t maxWidth) {
  for (uint8_t i = 0; i + 1 < kDisplayStrikeCount; ++i) {
    const BitmapFont& candidate = displayStrike(i);
    if (core::text::Typesetter::measure(candidate, text, length) <= maxWidth) return candidate;
  }
  return displayStrike(kDisplayStrikeCount - 1);
}

}  // namespace tinta::ui
