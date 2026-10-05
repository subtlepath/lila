#pragma once

// Font roles, the text-size setting and the DisplayTarget slot plan
// (PLAN.md sections 5.2 and 5.5).

#include <FreeInkUIDisplayTarget.h>

#include <cstddef>
#include <cstdint>

namespace tinta::ui {

using freeink::ui::BitmapFont;
using freeink::ui::FontId;

enum class TextSize : uint8_t { Small, Medium, Large };
inline constexpr uint8_t kTextSizeCount = 3;

enum class FontRole : uint8_t {
  SpanishDisplay,      // flashcard headword; see fitHeadword()
  SpanishText,         // sentences, dialogues, the reader
  SpanishEmphasis,     // the target word in a sentence
  SpanishAside,        // speaker names, grammar examples
  EnglishGloss,        // meaning on a card
  EnglishTranslation,  // sentence translations, notes
  Respelling,          // pronunciation
  Label,               // SUSTANTIVO · f, tags
  ChromeSmall,         // status bar
  ChromeBody,          // lists
  ChromeTitle,         // headers
  ChromeBodyBold,
  KeyboardKey,
  KeyboardLabel,
  Count,
};
inline constexpr uint8_t kFontRoleCount = static_cast<uint8_t>(FontRole::Count);

// The Spanish roles follow the text size; English roles and labels step with
// it around PLAN.md's sizes, which are Medium; chrome and keyboard stay put.
const BitmapFont& font(FontRole role, TextSize size = TextSize::Medium);
const char* roleName(FontRole role);

// DisplayTarget slots. 0-2 are FreeInkUI's small/body/title theme fonts; 7
// belongs to the typesetter, which re-points it before every run.
inline constexpr FontId kSlotSmall = 0;
inline constexpr FontId kSlotBody = 1;
inline constexpr FontId kSlotTitle = 2;
inline constexpr FontId kSlotBodyBold = 3;
inline constexpr FontId kSlotKey = 4;
inline constexpr FontId kSlotKeyLabel = 5;
inline constexpr FontId kSlotScratch = 7;

// Points slots 0-5 at the chrome and keyboard fonts. Call once after
// constructing the target; slot 7 is set per run by TypesetView.
void applyFontSlots(freeink::ui::DisplayTarget& target);

// Headword strikes, largest first: TeX Gyre Termes Bold at 87, 75, 68, 58
// and 50 px (rasterised from the outline; line heights 101, 87, 79, 67 and
// 58 px), then Adobe Times Bold 34 px as drawn.
inline constexpr uint8_t kDisplayStrikeCount = 6;
const BitmapFont& displayStrike(uint8_t index);

// The largest headword strike in which `text` fits `maxWidth` on one line,
// or the smallest if none does (the caller then wraps it).
const BitmapFont& fitHeadword(const char* text, size_t length, int16_t maxWidth);

}  // namespace tinta::ui
