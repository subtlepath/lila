#include "ui/views/CardText.h"

#include <stdio.h>
#include <string.h>

#include <new>

#include "core/text/Typesetter.h"
#include "ui/Strings.h"
#include "ui/TypesetView.h"

namespace tinta::ui {
namespace {

namespace pk = core::pack;
namespace tx = core::text;
using freeink::ui::Color;
using freeink::ui::Paint;
using freeink::ui::Rect;

int16_t i16(int32_t v) { return static_cast<int16_t>(v); }

// One block at a time, so one buffer serves every card. A sentence of 40
// words on 6 lines takes about 12 runs; a block that needs more is cut short
// rather than overrunning.
constexpr uint16_t kRunCap = 64;
constexpr uint16_t kSpanCap = 11;  // up to five emphasised words in a sentence
struct CardBuffers {
  tx::Run runs[kRunCap];
  tx::Span spans[kSpanCap];
  char prefix[48];  // drawLabelled()'s "Label: "
};
CardBuffers* gCard = nullptr;

uint16_t u16len(const char* s) {
  const size_t n = strlen(s);
  return static_cast<uint16_t>(n > 0xFFFF ? 0xFFFF : n);
}

// Lays out spans in the column and draws them if at least the first line
// fits; the lines that fit are drawn.
bool place(freeink::ui::DisplayTarget& t, Column& col, const tx::Span* spans, uint16_t count, uint8_t maxLines,
           int16_t gapAfter) {
  if (count == 0 || col.room() <= 0) return false;
  tx::Typesetter typesetter(gCard->runs, kRunCap);
  tx::Frame frame;
  frame.x = col.x;
  frame.y = col.y;
  frame.width = col.width;
  frame.height = col.room();
  frame.maxLines = maxLines;
  const tx::Layout& layout = typesetter.layout(spans, count, frame);
  if (layout.lineCount == 0 || layout.height > col.room()) return false;
  if (col.whole && !layout.complete()) return false;
  if (!col.dryRun) drawTypeset(t, typesetter);
  col.skip(i16(layout.height + gapAfter));
  return true;
}

}  // namespace

bool openCardText() {
  if (!gCard) gCard = new (std::nothrow) CardBuffers;
  return gCard != nullptr;
}

void closeCardText() {
  delete gCard;
  gCard = nullptr;
}

void formatLemmaLabel(const pk::Lemma& lemma, char* out, const size_t cap) {
  size_t used = 0;
  out[0] = '\0';
  const auto add = [&](const char* part) {
    if (!part || !part[0] || used >= cap) return;
    const int n = snprintf(out + used, cap - used, "%s%s", used ? " · " : "", part);
    if (n > 0) used += static_cast<size_t>(n);
  };
  const uint8_t pos = static_cast<uint8_t>(lemma.pos);
  if (pos >= 1 && pos <= 12) add(tr(static_cast<Str>(static_cast<uint16_t>(Str::PosNoun) + pos - 1)));
  switch (lemma.gender) {
    case pk::Gender::Masculine:
      add("m");
      break;
    case pk::Gender::Feminine:
      add("f");
      break;
    case pk::Gender::Both:
      add("m/f");
      break;
    default:
      break;
  }
  if (lemma.flags & pk::kLemmaPluralOnly) add("pl");
  switch (lemma.reg) {
    case pk::Register::Formal:
      add("formal");
      break;
    case pk::Register::Informal:
      add("informal");
      break;
    case pk::Register::Vulgar:
      add("vulgar");
      break;
    default:
      break;
  }
  if (lemma.flags & pk::kLemmaMexico) add("MX");
}

void formatHeadword(const pk::Pack& pack, const pk::Lemma& lemma, const bool withArticle, char* out, const size_t cap) {
  const char* word = pack.str(lemma.es);
  const char* article = "";
  if (withArticle && lemma.pos == pk::PartOfSpeech::Noun) {
    const bool plural = (lemma.flags & pk::kLemmaPluralOnly) != 0;
    switch (lemma.gender) {
      case pk::Gender::Masculine:
        article = plural ? "los " : "el ";
        break;
      case pk::Gender::Feminine:
        article = plural ? "las " : ((lemma.flags & pk::kLemmaElFeminine) ? "el " : "la ");
        break;
      case pk::Gender::Both:
        article = plural ? "los/las " : "el/la ";
        break;
      default:
        break;
    }
  }
  snprintf(out, cap, "%s%s", article, word);
}

bool drawTextIn(freeink::ui::DisplayTarget& t, Column& col, const char* text, const BitmapFont& font,
                const uint8_t maxLines, const int16_t gapAfter) {
  if (!text || !text[0]) return false;
  tx::Span& span = gCard->spans[0];
  span = tx::Span{};
  span.text = text;
  span.length = u16len(text);
  span.font = &font;
  return place(t, col, &span, 1, maxLines, gapAfter);
}

bool drawText(freeink::ui::DisplayTarget& t, Column& col, const char* text, const FontRole role, const TextSize size,
              const uint8_t maxLines, const int16_t gapAfter) {
  return drawTextIn(t, col, text, font(role, size), maxLines, gapAfter);
}

bool drawHeadword(freeink::ui::DisplayTarget& t, Column& col, const char* text, const int16_t gapAfter) {
  if (!text || !text[0]) return false;
  const BitmapFont& strike = fitHeadword(text, strlen(text), col.width);
  return drawTextIn(t, col, text, strike, 2, gapAfter);
}

bool drawSentence(freeink::ui::DisplayTarget& t, Column& col, const pk::Pack& pack, const pk::Sentence& sentence,
                  const uint16_t lemma, const int16_t tokenIndex, const bool blank, const TextSize size,
                  const int16_t gapAfter) {
  const char* es = pack.str(sentence.es);
  const uint16_t length = u16len(es);
  if (length == 0) return false;
  const BitmapFont& plain = font(FontRole::SpanishText, size);
  const BitmapFont& strong = font(FontRole::SpanishEmphasis, size);

  uint16_t count = 0;
  uint16_t at = 0;
  for (uint8_t i = 0; i < sentence.tokenCount && count + 2 < kSpanCap; ++i) {
    pk::Token token;
    if (!pack.token(sentence, i, token)) break;
    const bool marked = lemma != pk::kNone16 ? token.lemma == lemma : i == tokenIndex;
    if (!marked || token.start < at || token.start + token.length > length) continue;
    if (token.start > at) {
      gCard->spans[count] = tx::Span{};
      gCard->spans[count].text = es + at;
      gCard->spans[count].length = static_cast<uint16_t>(token.start - at);
      gCard->spans[count].font = &plain;
      ++count;
    }
    gCard->spans[count] = tx::Span{};
    gCard->spans[count].text = es + token.start;
    gCard->spans[count].length = token.length;
    gCard->spans[count].font = &strong;
    gCard->spans[count].flags = blank ? tx::kBlank : 0;
    ++count;
    at = static_cast<uint16_t>(token.start + token.length);
  }
  if (at < length) {
    gCard->spans[count] = tx::Span{};
    gCard->spans[count].text = es + at;
    gCard->spans[count].length = static_cast<uint16_t>(length - at);
    gCard->spans[count].font = &plain;
    ++count;
  }
  return place(t, col, gCard->spans, count, 0, gapAfter);
}

bool drawLabelled(freeink::ui::DisplayTarget& t, Column& col, const char* label, const char* value, const TextSize size,
                  const int16_t gapAfter) {
  if (!value || !value[0]) return false;
  char* prefix = gCard->prefix;
  snprintf(prefix, sizeof gCard->prefix, "%s: ", label);
  gCard->spans[0] = tx::Span{};
  gCard->spans[0].text = prefix;
  gCard->spans[0].length = u16len(prefix);
  gCard->spans[0].font = &font(FontRole::EnglishTranslation, size);
  gCard->spans[1] = tx::Span{};
  gCard->spans[1].text = value;
  gCard->spans[1].length = u16len(value);
  gCard->spans[1].font = &font(FontRole::SpanishText, size);
  return place(t, col, gCard->spans, 2, 0, gapAfter);
}

void drawRule(freeink::ui::DisplayTarget& t, Column& col, const int16_t gapAfter) {
  if (col.room() < 2) return;
  if (!col.dryRun) t.fill(Rect{col.x, col.y, col.width, 1}, Paint::solid(Color::Black));
  col.skip(i16(1 + gapAfter));
}

namespace {

const char* const kTenseNames[2][core::pack::kTenseCount] = {
    {"present", "preterite", "imperfect", "future", "conditional", "present subjunctive", "imperative",
     "negative imperative"},
    {"presente", "pretérito", "imperfecto", "futuro", "condicional", "presente de subjuntivo", "imperativo",
     "imperativo negativo"},
};

}  // namespace

const char* tenseName(const core::pack::Tense tense) {
  const uint8_t i = static_cast<uint8_t>(tense);
  if (i >= core::pack::kTenseCount) return "";
  return kTenseNames[language() == core::UiLanguage::Spanish ? 1 : 0][i];
}

const char* tenseNameSpanish(const core::pack::Tense tense) {
  const uint8_t i = static_cast<uint8_t>(tense);
  return i < core::pack::kTenseCount ? kTenseNames[1][i] : "";
}

void lessonCode(const core::pack::Pack& pack, const uint16_t lesson, char* out, const size_t cap) {
  core::pack::Lesson record;
  core::pack::Unit unit;
  if (!pack.lesson(lesson, record) || !pack.unit(record.unit, unit)) {
    snprintf(out, cap, "%u", lesson + 1);
    return;
  }
  snprintf(out, cap, "%u.%u", unit.number, record.number);
}

const char* personLabel(const core::pack::Person person) {
  static const char* const kPersons[core::pack::kPersonCount] = {"yo", "tú", "él / ella / usted", "nosotros",
                                                                 "ellos / ellas / ustedes"};
  const uint8_t i = static_cast<uint8_t>(person);
  return i < core::pack::kPersonCount ? kPersons[i] : "";
}

}  // namespace tinta::ui
