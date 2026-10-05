#include "core/text/Typesetter.h"

#include "core/lang/Utf8.h"

namespace tinta::core::text {

struct Typesetter::Line {
  Position start;       // first character; spaces at a line start are skipped
  Position contentEnd;  // just past the last non-space character
  Position next;        // where the following line starts
  int32_t width = 0;    // pen advance from start to contentEnd
};

namespace {

int16_t missingAdvance(const BitmapFont& font) {
  const int16_t adv = static_cast<int16_t>(font.yAdvance / 2);
  return adv > 6 ? adv : 6;
}

// Bytes a codepoint takes once TypesetView re-encodes it for DisplayTarget,
// which cannot take NUL or four-byte sequences.
uint16_t drawnBytes(uint32_t cp) { return cp == 0 || cp > 0xFFFF ? 3 : static_cast<uint16_t>(utf8::encodedLength(cp)); }

uint8_t descentOf(const BitmapFont& font) {
  return font.yAdvance > font.ascent ? static_cast<uint8_t>(font.yAdvance - font.ascent) : 0;
}

int16_t clamp16(int32_t v) { return static_cast<int16_t>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); }

}  // namespace

int16_t Typesetter::advance(const BitmapFont& font, uint32_t cp) {
  if (cp == 0x2026) {
    if ('.' < font.first || '.' > font.last) return 0;
    return static_cast<int16_t>(font.glyphs['.' - font.first].xAdvance * 3);
  }
  if (cp >= font.first && cp <= font.last && cp <= 0xFFFF) return font.glyphs[cp - font.first].xAdvance;
  return missingAdvance(font);
}

int32_t Typesetter::measure(const BitmapFont& font, const char* text, size_t length) {
  int32_t width = 0;
  for (size_t i = 0; i < length;) {
    uint32_t cp = 0;
    i += utf8::decode(text + i, length - i, cp);
    width += advance(font, cp);
  }
  return width;
}

uint16_t Typesetter::spanLength(uint16_t span) const {
  const Span& s = spans_[span];
  return s.text != nullptr && s.font != nullptr ? s.length : 0;
}

Position Typesetter::normalize(Position p) const {
  while (p.span < count_ && p.offset >= spanLength(p.span)) {
    ++p.span;
    p.offset = 0;
  }
  if (p.span >= count_) p = Position{count_, 0};
  return p;
}

uint32_t Typesetter::peek(Position p, uint16_t& bytes) const {
  const Span& s = spans_[p.span];
  uint32_t cp = 0;
  bytes = static_cast<uint16_t>(utf8::decode(s.text + p.offset, spanLength(p.span) - p.offset, cp));
  return cp;
}

void Typesetter::fillLine(Position p, Line& line) const {
  line.start = p;
  line.contentEnd = p;
  line.width = 0;
  int32_t pen = 0;
  bool canBreak = false;
  Position breakEnd;
  Position breakNext;
  int32_t breakWidth = 0;
  while (p.span < count_) {
    uint16_t n = 0;
    const uint32_t cp = peek(p, n);
    const Span& span = spans_[p.span];
    const Position after = normalize(Position{p.span, static_cast<uint16_t>(p.offset + n)});
    if (cp == '\n') {
      line.next = after;
      return;
    }
    const int16_t adv = advance(*span.font, cp);
    if (cp == ' ') {
      if ((span.flags & kNoBreak) == 0) {
        canBreak = true;
        breakEnd = line.contentEnd;
        breakWidth = line.width;
        breakNext = p;
      }
      pen += adv;
      p = after;
      continue;
    }
    // At least one character per line, so an over-wide word breaks between
    // characters instead of looping.
    if (pen + adv > frame_.width && line.contentEnd != line.start) {
      if (canBreak) {
        line.contentEnd = breakEnd;
        line.width = breakWidth;
        line.next = breakNext;
      } else {
        line.next = p;
      }
      return;
    }
    pen += adv;
    p = after;
    line.contentEnd = p;
    line.width = pen;
  }
  line.next = p;
}

bool Typesetter::placeRuns(const Line& line, uint16_t& placed, uint8_t& ascent, uint8_t& descent) {
  placed = 0;
  ascent = 0;
  descent = 0;
  int32_t pen = 0;
  for (uint16_t s = line.start.span; s < count_ && s <= line.contentEnd.span; ++s) {
    const Span& span = spans_[s];
    const uint16_t length = spanLength(s);
    const uint16_t end = s == line.contentEnd.span ? line.contentEnd.offset : length;
    uint16_t i = s == line.start.span ? line.start.offset : 0;
    while (i < end) {
      uint32_t cp = 0;
      uint16_t n = static_cast<uint16_t>(utf8::decode(span.text + i, length - i, cp));
      if (cp == ' ') {  // spaces before a run only move the pen
        pen += advance(*span.font, cp);
        i = static_cast<uint16_t>(i + n);
        continue;
      }
      const uint16_t begin = i;
      const int32_t runPen = pen;
      uint16_t last = i;
      int32_t lastPen = pen;
      uint16_t bytes = 0;
      while (i < end) {
        n = static_cast<uint16_t>(utf8::decode(span.text + i, length - i, cp));
        const uint16_t drawn = drawnBytes(cp);
        if (bytes + drawn > kMaxRunBytes) break;
        bytes = static_cast<uint16_t>(bytes + drawn);
        pen += advance(*span.font, cp);
        i = static_cast<uint16_t>(i + n);
        if (cp != ' ') {
          last = i;
          lastPen = pen;
        }
      }
      if (runs_ != nullptr) {
        if (layout_.runCount + placed >= capacity_) return false;
        Run& run = runs_[layout_.runCount + placed];
        run.x = clamp16(runPen);
        run.width = clamp16(lastPen - runPen);
        run.span = s;
        run.begin = begin;
        run.end = last;
      }
      ++placed;
      if (span.font->ascent > ascent) ascent = span.font->ascent;
      if (descentOf(*span.font) > descent) descent = descentOf(*span.font);
    }
  }
  return true;
}

const Layout& Typesetter::layout(const Span* spans, uint16_t count, const Frame& frame, Position from) {
  spans_ = spans;
  count_ = spans != nullptr ? count : 0;
  frame_ = frame;
  layout_ = Layout{};
  Position p = normalize(from);
  int32_t y = frame.y;
  while (true) {
    uint16_t n = 0;
    while (p.span < count_ && peek(p, n) == ' ') p = normalize(Position{p.span, static_cast<uint16_t>(p.offset + n)});
    if (p.span >= count_) {
      layout_.stop = Stop::End;
      break;
    }
    if ((frame.maxLines > 0 && layout_.lineCount >= frame.maxLines) || layout_.lineCount == 0xFF) {
      layout_.stop = Stop::MaxLines;
      break;
    }
    Line line;
    fillLine(p, line);
    uint16_t placed = 0;
    uint8_t ascent = 0;
    uint8_t descent = 0;
    if (!placeRuns(line, placed, ascent, descent)) {
      layout_.stop = Stop::Runs;
      break;
    }
    if (placed == 0) {  // empty line between two '\n'
      const BitmapFont& font = *spans_[line.start.span].font;
      ascent = font.ascent;
      descent = descentOf(font);
    }
    const int32_t baseline = y + ascent;
    const int32_t bottom = baseline + descent;
    if (frame.height > 0 && layout_.lineCount > 0 && bottom - frame.y > frame.height) {
      layout_.stop = Stop::Height;
      break;
    }
    int32_t shift = frame.x;
    if (frame.align == Align::Center && line.width < frame.width) shift += (frame.width - line.width) / 2;
    if (runs_ != nullptr) {
      for (uint16_t k = layout_.runCount; k < layout_.runCount + placed; ++k) {
        Run& run = runs_[k];
        run.x = clamp16(run.x + shift);
        run.baseline = clamp16(baseline);
        run.lineTop = clamp16(y);
        run.lineBottom = clamp16(bottom);
        run.line = layout_.lineCount;
      }
      layout_.runCount = static_cast<uint16_t>(layout_.runCount + placed);
    }
    ++layout_.lineCount;
    if (line.width > layout_.width) layout_.width = clamp16(line.width);
    layout_.height = clamp16(bottom - frame.y);
    y = bottom + frame.lineGap;
    p = line.next;
  }
  layout_.next = p;
  return layout_;
}

int32_t Typesetter::runAt(int16_t x, int16_t y, int16_t slop) const {
  int32_t best = -1;
  int32_t bestDistance = 0;
  for (uint16_t k = 0; k < layout_.runCount; ++k) {
    const Run& run = runs_[k];
    if (spans_[run.span].token == kNoToken) continue;
    const int32_t right = run.x + run.width;
    const int32_t dx = x < run.x ? run.x - x : (x >= right ? x - right + 1 : 0);
    const int32_t dy = y < run.lineTop ? run.lineTop - y : (y >= run.lineBottom ? y - run.lineBottom + 1 : 0);
    if (dx > slop || dy > slop) continue;
    if (best < 0 || dx + dy < bestDistance) {
      best = k;
      bestDistance = dx + dy;
    }
  }
  return best;
}

uint16_t Typesetter::hitTest(int16_t x, int16_t y, int16_t slop) const {
  const int32_t k = runAt(x, y, slop);
  return k < 0 ? kNoToken : spans_[runs_[k].span].token;
}

}  // namespace tinta::core::text
