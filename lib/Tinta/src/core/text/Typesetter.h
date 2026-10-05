#pragma once

// Mixed-font line layout for FreeInkUI's DisplayTarget (PLAN.md section 5.5).
//
// Input is an array of spans, each with its own BitmapFont; output is a
// caller-provided array of single-line runs that ui/TypesetView draws one by
// one. Spans on a line share one baseline and the line is as tall as its
// tallest font. Lines break greedily at spaces (and at '\n'); a word wider
// than the frame breaks between characters. Widths come straight from the
// BitmapFont advances, exactly as DisplayTarget measures them, so a run drawn
// with text() never wraps or ellipsises.
//
// No heap, no exceptions: everything lives in the spans and the run array.

#include <FreeInkUIFont.h>

#include <cstddef>
#include <cstdint>

namespace tinta::core::text {

using freeink::ui::BitmapFont;

inline constexpr uint16_t kNoToken = 0xFFFF;

// Longest run in bytes, as drawn. Longer stretches of one span on one line are
// split into several runs, so a run always fits DisplayTarget's line buffer
// and TypesetView's copy of it.
inline constexpr uint16_t kMaxRunBytes = 160;

enum SpanFlags : uint8_t {
  kUnderline = 1 << 0,
  kBlank = 1 << 1,      // cloze gap: laid out like the text, drawn as a rule only
  kHighlight = 1 << 2,  // inverted box, e.g. the reader's word cursor
  kNoBreak = 1 << 3,    // spaces inside this span never break the line
};

struct Span {
  const char* text = nullptr;  // UTF-8 in font charset form, not NUL-terminated
  uint16_t length = 0;
  const BitmapFont* font = nullptr;
  uint8_t flags = 0;
  uint16_t token = kNoToken;  // returned by hitTest()
};

enum class Align : uint8_t { Left, Center };

struct Frame {
  int16_t x = 0;
  int16_t y = 0;  // top of the first line
  int16_t width = 0;
  int16_t height = 0;    // 0 = unlimited
  uint8_t maxLines = 0;  // 0 = unlimited
  int8_t lineGap = 0;    // extra pixels between lines
  Align align = Align::Left;
};

// A place in the span array: byte `offset` of span `span`.
struct Position {
  uint16_t span = 0;
  uint16_t offset = 0;
};

inline bool operator==(Position a, Position b) { return a.span == b.span && a.offset == b.offset; }
inline bool operator!=(Position a, Position b) { return !(a == b); }

// One single-line piece of one span. A run never starts or ends with a space;
// the spaces around it only move the pen.
struct Run {
  int16_t x;  // pen position of the first glyph
  int16_t baseline;
  int16_t width;    // pen advance across the run
  int16_t lineTop;  // the line box, shared by every run on the line
  int16_t lineBottom;
  uint16_t span;
  uint16_t begin;  // byte range in the span's text
  uint16_t end;
  uint8_t line;
};

enum class Stop : uint8_t {
  End,       // all text laid out
  MaxLines,  // frame.maxLines reached
  Height,    // the next line would pass frame.y + frame.height
  Runs,      // the run array is full
};

struct Layout {
  uint16_t runCount = 0;
  uint8_t lineCount = 0;
  int16_t width = 0;   // widest line
  int16_t height = 0;  // frame.y to the bottom of the last line (no trailing gap)
  Position next;       // where to continue; {spanCount, 0} once complete
  Stop stop = Stop::End;

  bool complete() const { return stop == Stop::End; }
};

class Typesetter {
 public:
  // `runs` may be null with capacity 0 to measure without storing runs.
  Typesetter(Run* runs, uint16_t capacity) : runs_(runs), capacity_(runs ? capacity : 0) {}

  // Lays out spans[0..count) from `from` into the frame, replacing any
  // previous layout. The first line is always placed, even if it is taller
  // than the frame, so paginating by calling again from result().next always
  // makes progress. The run array must hold at least one line's runs.
  const Layout& layout(const Span* spans, uint16_t count, const Frame& frame, Position from = {});

  const Layout& result() const { return layout_; }
  const Run* runs() const { return runs_; }
  uint16_t runCount() const { return layout_.runCount; }
  const Span* spans() const { return spans_; }

  // The run nearest (x, y) among runs whose span has a token, if it lies
  // within `slop` pixels of the run's box (x extent by line box); -1 if none.
  int32_t runAt(int16_t x, int16_t y, int16_t slop = 0) const;
  // The token of runAt(), or kNoToken.
  uint16_t hitTest(int16_t x, int16_t y, int16_t slop = 0) const;

  // Pen advance of one codepoint, as DisplayTarget::runAdvance computes it:
  // U+2026 is three full stops, a codepoint outside the font a missing-glyph
  // box. Codepoints above U+FFFF count as missing (TypesetView draws them as
  // U+FFFD, the box).
  static int16_t advance(const BitmapFont& font, uint32_t cp);
  // Pen advance of a UTF-8 string.
  static int32_t measure(const BitmapFont& font, const char* text, size_t length);

 private:
  struct Line;

  void fillLine(Position start, Line& line) const;
  bool placeRuns(const Line& line, uint16_t& count, uint8_t& ascent, uint8_t& descent);
  uint16_t spanLength(uint16_t span) const;
  Position normalize(Position p) const;
  uint32_t peek(Position p, uint16_t& bytes) const;

  Run* runs_;
  uint16_t capacity_;
  const Span* spans_ = nullptr;
  uint16_t count_ = 0;
  Frame frame_;
  Layout layout_;
};

}  // namespace tinta::core::text
