#include "ui/TypesetView.h"

#include "core/lang/Utf8.h"
#include "ui/Fonts.h"

namespace tinta::ui {

namespace {

using freeink::ui::Color;
using freeink::ui::Paint;
using freeink::ui::Rect;

constexpr int16_t kHighlightPad = 3;

int16_t i16(int32_t v) { return static_cast<int16_t>(v); }

// Copies a run as NUL-terminated UTF-8 that DisplayTarget decodes exactly as
// the typesetter measured it: malformed bytes, NUL and four-byte sequences
// become U+FFFD, which both treat as a missing glyph.
void copyRun(const core::text::Span& span, const core::text::Run& run, char (&out)[core::text::kMaxRunBytes + 1]) {
  size_t o = 0;
  for (size_t i = run.begin; i < run.end;) {
    uint32_t cp = 0;
    i += core::utf8::decode(span.text + i, span.length - i, cp);
    if (cp == 0 || cp > 0xFFFF) cp = core::utf8::kReplacement;
    if (o + core::utf8::encodedLength(cp) > core::text::kMaxRunBytes) break;
    o += core::utf8::encode(cp, out + o);
  }
  out[o] = '\0';
}

}  // namespace

void drawRun(freeink::ui::DisplayTarget& target, const core::text::Span& span, const core::text::Run& run, int16_t dx,
             int16_t dy) {
  const BitmapFont& font = *span.font;
  const int16_t x = i16(run.x + dx);
  const int16_t baseline = i16(run.baseline + dy);
  const bool highlight = (span.flags & core::text::kHighlight) != 0;

  if (highlight) {
    target.fill(Rect{i16(x - kHighlightPad), i16(run.lineTop + dy), i16(run.width + 2 * kHighlightPad),
                     i16(run.lineBottom - run.lineTop)},
                Paint::solid(Color::Black));
  }
  const Color ink = highlight ? Color::White : Color::Black;
  // Display strikes get a heavier rule.
  const int16_t rule = font.yAdvance >= 48 ? 3 : (font.yAdvance >= 30 ? 2 : 1);

  if (span.flags & core::text::kBlank) {
    target.fill(Rect{x, i16(baseline + 2), run.width, i16(rule + 1)}, Paint::solid(ink));
    return;
  }

  char buf[core::text::kMaxRunBytes + 1];
  copyRun(span, run, buf);
  freeink::ui::TextStyle style;
  style.font = kSlotScratch;
  style.color = ink;
  style.maxLines = 1;
  target.setFont(kSlotScratch, font);
  // A little slack on the width: DisplayTarget wraps only when a run is wider
  // than the rect, and the run is exactly as wide as it measures.
  target.text(Rect{x, i16(baseline - font.ascent), i16(run.width + 4), font.yAdvance}, buf, style);

  if (span.flags & core::text::kUnderline) {
    target.fill(Rect{x, i16(baseline + 2), run.width, rule}, Paint::solid(ink));
  }
}

void drawTypeset(freeink::ui::DisplayTarget& target, const core::text::Typesetter& typesetter, int16_t dx, int16_t dy) {
  const core::text::Run* runs = typesetter.runs();
  for (uint16_t k = 0; k < typesetter.runCount(); ++k) {
    drawRun(target, typesetter.spans()[runs[k].span], runs[k], dx, dy);
  }
}

}  // namespace tinta::ui
