// modules: text lang

#include "core/text/Typesetter.h"

#include <FreeInkUIDisplayTarget.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "check.h"

// One real strike, to check measurement against DisplayTarget itself.
#include "fonts/TimesRoman25.cpp"

using namespace tinta::core::text;
using freeink::ui::BitmapFont;
using freeink::ui::FontGlyph;

namespace {

// A font where every glyph advances `advance` pixels and a space half that.
struct TestFont {
  FontGlyph glyphs[0xFF - 0x20 + 1];
  BitmapFont font;

  TestFont(uint8_t advance, uint8_t yAdvance, uint8_t ascent) {
    for (FontGlyph& g : glyphs) g = FontGlyph{0, 0, 0, advance, 0, 0};
    glyphs[0] = FontGlyph{0, 0, 0, static_cast<uint8_t>(advance / 2), 0, 0};
    static const uint8_t kNoBitmap[1] = {0};
    font = BitmapFont{kNoBitmap, glyphs, 0x20, 0xFF, yAdvance, ascent, advance, yAdvance, 1};
  }
};

// Small: advance 10, space 5, yAdvance 25, ascent 20. Big: 20/10, 40, 30.
const TestFont& small() {
  static const TestFont f(10, 25, 20);
  return f;
}
const TestFont& big() {
  static const TestFont f(20, 40, 30);
  return f;
}

Span span(const char* text, const BitmapFont& font, uint8_t flags = 0, uint16_t token = kNoToken) {
  Span s;
  s.text = text;
  s.length = static_cast<uint16_t>(std::strlen(text));
  s.font = &font;
  s.flags = flags;
  s.token = token;
  return s;
}

Frame frame(int16_t width, int16_t x = 0, int16_t y = 0) {
  Frame f;
  f.x = x;
  f.y = y;
  f.width = width;
  return f;
}

// The text of run k, NUL-terminated in a static buffer.
const char* text(const Typesetter& ts, uint16_t k) {
  static char buf[256];
  const Run& run = ts.runs()[k];
  const Span& s = ts.spans()[run.span];
  const size_t n = static_cast<size_t>(run.end - run.begin);
  std::memcpy(buf, s.text + run.begin, n);
  buf[n] = '\0';
  return buf;
}

void testAdvanceMatchesDisplayTarget() {
  const BitmapFont& f = tinta::fonts::kTimesRoman25;
  std::vector<uint8_t> fb(800 * 480 / 8, 0xFF);
  freeink::ui::DisplayTarget target(fb.data(), 800, 480, 100);
  target.setFont(7, f);
  const char* samples[] = {
      "\xC2\xBF\xC3\x89l est\xC3\xA1 en \xC3\x81vila? \xC2\x97S\xC3\xAD.",  // ¿Él está en Ávila? —Sí.
      "Ma\xC3\xB1"
      "ana\xE2\x80\xA6",  // Mañana…
      "\xC2\x93"
      "comillas\xC2\x94 \xC2\x80 \xC2\x95",  // “comillas” € •
      "\xC4\x80 out of range",               // Ā: missing-glyph box
      "\xC2\x81 empty slot",
  };
  for (const char* s : samples) {
    const int32_t ours = Typesetter::measure(f, s, std::strlen(s));
    const int16_t theirs = target.measureText(7, s, freeink::ui::TextStyle{}).width;
    CHECK_EQ(ours, theirs);
  }
  CHECK_EQ(Typesetter::advance(f, 0x2026), 3 * Typesetter::advance(f, '.'));
  CHECK_EQ(Typesetter::advance(f, 0x0100), f.yAdvance / 2 > 6 ? f.yAdvance / 2 : 6);
  CHECK_EQ(Typesetter::advance(f, 0x1F642), Typesetter::advance(f, 0x0100));
  CHECK_EQ(Typesetter::advance(f, 0x97), f.glyphs[0x97 - 0x20].xAdvance);
  // Malformed bytes measure as one missing glyph each.
  CHECK_EQ(Typesetter::measure(f, "\x80\x80", 2), 2 * Typesetter::advance(f, 0xFFFD));
}

void testSingleLine() {
  Run runs[8];
  Typesetter ts(runs, 8);
  const Span spans[] = {span("hola mundo", small().font)};
  const Layout& r = ts.layout(spans, 1, frame(200, 7, 11));
  CHECK(r.complete());
  CHECK_EQ(r.lineCount, 1);
  CHECK_EQ(r.runCount, 1);
  CHECK_STR_EQ(text(ts, 0), "hola mundo");
  CHECK_EQ(runs[0].x, 7);
  CHECK_EQ(runs[0].width, 9 * 10 + 5);
  CHECK_EQ(runs[0].baseline, 11 + 20);
  CHECK_EQ(runs[0].lineTop, 11);
  CHECK_EQ(runs[0].lineBottom, 11 + 25);
  CHECK_EQ(r.height, 25);
  CHECK_EQ(r.width, 95);
  CHECK(r.next == (Position{1, 0}));
}

void testGreedyWrap() {
  Run runs[8];
  Typesetter ts(runs, 8);
  // "uno dos" is 65 px; "uno dos tres" 110 px.
  const Span spans[] = {span("uno dos tres", small().font)};
  const Layout& r = ts.layout(spans, 1, frame(100));
  CHECK_EQ(r.lineCount, 2);
  CHECK_EQ(r.runCount, 2);
  CHECK_STR_EQ(text(ts, 0), "uno dos");
  CHECK_STR_EQ(text(ts, 1), "tres");
  CHECK_EQ(runs[0].width, 65);
  CHECK_EQ(runs[1].x, 0);
  CHECK_EQ(runs[1].lineTop, 25);
  CHECK_EQ(runs[1].baseline, 45);
  CHECK_EQ(runs[1].line, 1);
  CHECK_EQ(r.width, 65);

  // Exactly full lines still fit; trailing spaces hang past the edge.
  const Span exact[] = {span("uno dos   ", small().font)};
  CHECK_EQ(ts.layout(exact, 1, frame(65)).lineCount, 1);
  CHECK_EQ(ts.layout(exact, 1, frame(64)).lineCount, 2);
}

void testMixedFontsShareABaseline() {
  Run runs[8];
  Typesetter ts(runs, 8);
  // ¿Él (big) está en (small) Ávila (big) ? (small)
  const Span spans[] = {
      span("\xC2\xBF\xC3\x89l", big().font),
      span(" est\xC3\xA1 en ", small().font),
      span("\xC3\x81vila", big().font),
      span("?", small().font),
  };
  const Layout& r = ts.layout(spans, 4, frame(400, 0, 100));
  CHECK_EQ(r.lineCount, 1);
  CHECK_EQ(r.runCount, 4);
  for (uint16_t k = 0; k < 4; ++k) {
    CHECK_EQ(runs[k].baseline, 100 + 30);
    CHECK_EQ(runs[k].lineTop, 100);
    CHECK_EQ(runs[k].lineBottom, 100 + 30 + 10);
  }
  CHECK_STR_EQ(text(ts, 1), "est\xC3\xA1 en");
  CHECK_EQ(runs[0].x, 0);
  CHECK_EQ(runs[1].x, 60 + 5);  // leading space only moves the pen
  CHECK_EQ(runs[1].width, 4 * 10 + 5 + 2 * 10);
  CHECK_EQ(runs[2].x, 60 + 5 + 65 + 5);
  CHECK_EQ(runs[3].x, runs[2].x + 5 * 20);
  CHECK_EQ(r.width, runs[3].x + 10);
}

void testLineHeightFollowsEachLine() {
  Run runs[8];
  Typesetter ts(runs, 8);
  const Span spans[] = {span("AAAA ", big().font), span("bbbb cccc", small().font)};
  Frame f = frame(100);
  f.lineGap = 3;
  const Layout& r = ts.layout(spans, 2, f);
  // Line 1: "AAAA" (80) + space (10) + "bbbb" would be 130 > 100, so it breaks.
  CHECK_EQ(r.lineCount, 2);
  CHECK_STR_EQ(text(ts, 0), "AAAA");
  CHECK_STR_EQ(text(ts, 1), "bbbb cccc");
  CHECK_EQ(runs[0].lineBottom, 40);
  CHECK_EQ(runs[1].lineTop, 43);
  CHECK_EQ(runs[1].baseline, 43 + 20);
  CHECK_EQ(r.height, 43 + 25);
}

void testWordsSpanningSpansStayTogether() {
  Run runs[8];
  Typesetter ts(runs, 8);
  const Span spans[] = {span("en ", small().font), span("\xC3\x81vila", big().font), span("?", small().font)};
  // "en " = 25, "Ávila" = 100, "?" = 10: 135 needs a break, and "?" must
  // follow "Ávila" to the next line.
  const Layout& r = ts.layout(spans, 3, frame(130));
  CHECK_EQ(r.lineCount, 2);
  CHECK_EQ(r.runCount, 3);
  CHECK_EQ(runs[0].line, 0);
  CHECK_EQ(runs[1].line, 1);
  CHECK_EQ(runs[2].line, 1);
  CHECK_EQ(runs[1].x, 0);
  CHECK_EQ(runs[2].x, 100);
  CHECK_EQ(runs[0].lineBottom - runs[0].lineTop, 25);  // only the small font on line 1
}

void testHardBreaks() {
  Run runs[8];
  Typesetter ts(runs, 8);
  const Span a[] = {span("a\nb", small().font)};
  CHECK_EQ(ts.layout(a, 1, frame(200)).lineCount, 2);
  CHECK_STR_EQ(text(ts, 1), "b");
  CHECK_EQ(runs[1].lineTop, 25);

  const Span b[] = {span("a\n\nb\n", small().font)};
  const Layout& r = ts.layout(b, 1, frame(200));
  CHECK_EQ(r.lineCount, 3);  // the empty line keeps its height; no line after the last '\n'
  CHECK_EQ(r.runCount, 2);
  CHECK_EQ(runs[1].lineTop, 50);
  CHECK(r.complete());

  const Span c[] = {span("uno   \n  dos", small().font)};
  ts.layout(c, 1, frame(200));
  CHECK_STR_EQ(text(ts, 0), "uno");
  CHECK_STR_EQ(text(ts, 1), "dos");
  CHECK_EQ(runs[1].x, 0);  // spaces at a line start are dropped
}

void testOverWideWordBreaksBetweenCharacters() {
  Run runs[8];
  Typesetter ts(runs, 8);
  const Span spans[] = {span("ab abcdefghij", small().font)};
  const Layout& r = ts.layout(spans, 1, frame(35));
  CHECK_EQ(r.lineCount, 5);
  CHECK_STR_EQ(text(ts, 0), "ab");
  CHECK_STR_EQ(text(ts, 1), "abc");
  CHECK_STR_EQ(text(ts, 2), "def");
  CHECK_STR_EQ(text(ts, 3), "ghi");
  CHECK_STR_EQ(text(ts, 4), "j");

  // A frame narrower than one glyph still makes progress, one per line.
  const Span tiny[] = {
      span("\xC3\xB1"
           "a",
           small().font)};
  CHECK_EQ(ts.layout(tiny, 1, frame(4)).lineCount, 2);
  CHECK_STR_EQ(text(ts, 0), "\xC3\xB1");
}

void testCentre() {
  Run runs[8];
  Typesetter ts(runs, 8);
  const Span spans[] = {span("uno dos tres", small().font)};
  Frame f = frame(100, 20);
  f.align = Align::Center;
  ts.layout(spans, 1, f);
  CHECK_EQ(runs[0].x, 20 + (100 - 65) / 2);
  CHECK_EQ(runs[1].x, 20 + (100 - 40) / 2);
}

void testNoBreakSpan() {
  Run runs[8];
  Typesetter ts(runs, 8);
  const Span spans[] = {span("ver ", small().font), span("la chamba", small().font, kNoBreak)};
  const Layout& r = ts.layout(spans, 2, frame(100));
  CHECK_EQ(r.lineCount, 2);
  CHECK_STR_EQ(text(ts, 0), "ver");
  CHECK_STR_EQ(text(ts, 1), "la chamba");
}

void testPaginationByMaxLines() {
  Run runs[16];
  Typesetter ts(runs, 16);
  const Span spans[] = {span("uno dos ", small().font), span("tres cuatro cinco", big().font),
                        span(" seis siete", small().font)};
  Frame f = frame(130);  // five lines: "uno dos" "tres" "cuatro" "cinco" "seis siete"
  f.maxLines = 2;
  std::vector<char> seen;
  Position at;
  int pages = 0;
  while (true) {
    const Layout& r = ts.layout(spans, 3, f, at);
    CHECK(r.lineCount <= 2);
    for (uint16_t k = 0; k < r.runCount; ++k) {
      const char* t = text(ts, k);
      if (!seen.empty()) seen.push_back(' ');
      seen.insert(seen.end(), t, t + std::strlen(t));
    }
    ++pages;
    if (r.complete() || pages > 10) break;
    CHECK(r.stop == Stop::MaxLines);
    CHECK(r.next != at);
    at = r.next;
  }
  seen.push_back('\0');
  CHECK_STR_EQ(seen.data(), "uno dos tres cuatro cinco seis siete");
  CHECK_EQ(pages, 3);

  // Continuing mid-span.
  const Layout& r = ts.layout(spans, 3, frame(1000), Position{1, 5});
  CHECK_EQ(r.runCount, 2);
  CHECK_STR_EQ(text(ts, 0), "cuatro cinco");
  CHECK(r.next == (Position{3, 0}));
}

void testHeightLimit() {
  Run runs[16];
  Typesetter ts(runs, 16);
  const Span spans[] = {span("a b c d", small().font)};
  Frame f = frame(10);
  f.height = 60;  // two 25 px lines fit, a third would end at 75
  const Layout& r = ts.layout(spans, 1, f);
  CHECK_EQ(r.lineCount, 2);
  CHECK(r.stop == Stop::Height);
  CHECK(r.next == (Position{0, 4}));
  CHECK_EQ(r.height, 50);

  f.height = 10;  // the first line is placed even when it does not fit
  const Layout& first = ts.layout(spans, 1, f);
  CHECK_EQ(first.lineCount, 1);
  CHECK(first.stop == Stop::Height);
}

void testRunCapacity() {
  Run runs[3];
  Typesetter ts(runs, 3);
  const Span spans[] = {span("a ", small().font), span("b", small().font), span(" c", small().font),
                        span(" d", small().font)};
  Frame f = frame(30);  // "a b" on line 1, "c d" on line 2
  const Layout& r = ts.layout(spans, 4, f);
  CHECK_EQ(r.lineCount, 1);
  CHECK_EQ(r.runCount, 2);
  CHECK(r.stop == Stop::Runs);
  CHECK(r.next == (Position{2, 1}));
}

void testMeasureOnly() {
  Run runs[16];
  Typesetter withRuns(runs, 16);
  Typesetter measureOnly(nullptr, 0);
  const Span spans[] = {span("uno dos ", small().font), span("tres cuatro cinco", big().font)};
  const Layout& a = withRuns.layout(spans, 2, frame(120));
  const Layout& b = measureOnly.layout(spans, 2, frame(120));
  CHECK_EQ(b.lineCount, a.lineCount);
  CHECK_EQ(b.height, a.height);
  CHECK_EQ(b.width, a.width);
  CHECK_EQ(b.runCount, 0);
  CHECK(b.complete());
  CHECK_EQ(measureOnly.hitTest(5, 5), kNoToken);
}

void testLongRunsAreSplit() {
  Run runs[8];
  Typesetter ts(runs, 8);
  static char ascii[201];
  std::memset(ascii, 'a', 200);
  ascii[200] = '\0';
  const Span a[] = {span(ascii, small().font)};
  const Layout& r = ts.layout(a, 1, frame(5000, 3));
  CHECK_EQ(r.lineCount, 1);
  CHECK_EQ(r.runCount, 2);
  CHECK_EQ(runs[0].end - runs[0].begin, kMaxRunBytes);
  CHECK_EQ(runs[1].begin, kMaxRunBytes);
  CHECK_EQ(runs[1].x, 3 + kMaxRunBytes * 10);
  CHECK_EQ(runs[0].baseline, runs[1].baseline);

  // A split never leaves a run starting with a space.
  static char words[241];
  for (int i = 0; i < 240; ++i) words[i] = i % 4 == 3 ? ' ' : 'b';
  words[240] = '\0';
  const Span w[] = {span(words, small().font)};
  ts.layout(w, 1, frame(5000));
  for (uint16_t k = 0; k < ts.runCount(); ++k) {
    CHECK(words[runs[k].begin] != ' ');
    CHECK(words[runs[k].end - 1] != ' ');
    CHECK(runs[k].end - runs[k].begin <= kMaxRunBytes);
  }
  CHECK_EQ(runs[1].x, Typesetter::measure(small().font, words, runs[1].begin));

  // Malformed bytes count as three drawn bytes each (re-encoded as U+FFFD).
  static char bad[121];
  std::memset(bad, '\x80', 120);
  bad[120] = '\0';
  const Span m[] = {span(bad, small().font)};
  ts.layout(m, 1, frame(30000));
  CHECK_EQ(ts.runCount(), 3);
  CHECK_EQ(runs[0].end - runs[0].begin, kMaxRunBytes / 3);
}

void testHitTest() {
  Run runs[16];
  Typesetter ts(runs, 16);
  const Span spans[] = {span("uno", small().font, 0, 1), span(" ", small().font), span("dos", small().font, 0, 2),
                        span(" ", small().font), span("tres", small().font, 0, 3)};
  ts.layout(spans, 5, frame(80, 10, 50));
  // Line 1: "uno dos" at x 10..75, line 2: "tres" at 10..50.
  CHECK_EQ(ts.hitTest(15, 60), 1);
  CHECK_EQ(ts.hitTest(39, 60), 1);
  CHECK_EQ(ts.hitTest(42, 60), kNoToken);  // the space between words
  CHECK_EQ(ts.hitTest(43, 60, 4), 2);      // nearest word within the slop
  CHECK_EQ(ts.hitTest(41, 60, 4), 1);
  CHECK_EQ(ts.hitTest(20, 80), 3);
  CHECK_EQ(ts.hitTest(20, 76), 3);
  CHECK_EQ(ts.hitTest(200, 60), kNoToken);
  CHECK_EQ(ts.hitTest(20, 49), kNoToken);
  CHECK_EQ(ts.hitTest(20, 49, 2), 1);
  CHECK(ts.runAt(20, 80) >= 0);
  CHECK_EQ(ts.runs()[ts.runAt(20, 80)].line, 1);
}

void testEdgeCases() {
  Run runs[8];
  Typesetter ts(runs, 8);
  CHECK(ts.layout(nullptr, 0, frame(100)).complete());
  CHECK_EQ(ts.result().lineCount, 0);

  Span spans[] = {span("", small().font), span("   ", small().font), span("x", small().font),
                  span("ignored", small().font)};
  spans[3].font = nullptr;  // a span without a font is skipped
  const Layout& r = ts.layout(spans, 4, frame(100));
  CHECK_EQ(r.lineCount, 1);
  CHECK_EQ(r.runCount, 1);
  CHECK_EQ(runs[0].x, 0);  // spaces at a line start are skipped even in their own span
  CHECK(r.next == (Position{4, 0}));

  const Span spaces[] = {span("    ", small().font)};
  CHECK_EQ(ts.layout(spaces, 1, frame(100)).lineCount, 0);
}

}  // namespace

int main() {
  testAdvanceMatchesDisplayTarget();
  testSingleLine();
  testGreedyWrap();
  testMixedFontsShareABaseline();
  testLineHeightFollowsEachLine();
  testWordsSpanningSpansStayTogether();
  testHardBreaks();
  testOverWideWordBreaksBetweenCharacters();
  testCentre();
  testNoBreakSpan();
  testPaginationByMaxLines();
  testHeightLimit();
  testRunCapacity();
  testMeasureOnly();
  testLongRunsAreSplit();
  testHitTest();
  testEdgeCases();
  return tinta_test::result();
}
