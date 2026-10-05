#include "ui/screens/SearchScreen.h"

#include <stdio.h>
#include <string.h>

#include "app/App.h"
#include "platform/Log.h"
#include "ui/Fonts.h"
#include "ui/Strings.h"
#include "ui/screens/DictionaryScreens.h"
#include "ui/views/CardText.h"

namespace tinta::ui {
namespace {

namespace pk = core::pack;
namespace sr = core::search;
using freeink::ui::Color;
using freeink::ui::Paint;
using freeink::ui::Rect;
using freeink::ui::TextAlign;
using freeink::ui::TextStyle;

int16_t i16(int32_t v) { return static_cast<int16_t>(v); }

TextStyle styled(freeink::ui::FontId font, Color color = Color::Black, TextAlign align = TextAlign::Left) {
  TextStyle s;
  s.font = font;
  s.color = color;
  s.align = align;
  return s;
}

}  // namespace

const char* SearchScreen::title() const { return tr(Str::Search); }

void SearchScreen::enter(const bool returning) {
  traceNext_ = true;
  keyboard_.traceNext();
  if (returning) return;
  query_[0] = '\0';
  run();
}

// What was searched, for the usage log: the query as it stood when the
// learner opened a result or left, not every letter on the way.
void SearchScreen::leave() {
  if (!unrecorded_ || query_[0] == '\0') return;
  unrecorded_ = false;
  app_.usage().search(core::usage::SearchMode::Spanish, query_, search_.count());
}

void SearchScreen::run() {
  unrecorded_ = true;
  search_.run(app_.pack(), query_);
  platform::log("search '%s' %u%s", query_, search_.count(), search_.more() ? "+" : "");
}

void SearchScreen::build(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  const pk::Pack& pack = app_.pack();
  freeink::ui::DisplayTarget& t = app_.target();
  const Rect body = screen.body();
  const Rect full = screen.frame().screen();

  // The query field across the top.
  const BitmapFont& spanish = font(FontRole::SpanishEmphasis);
  const int16_t fieldH = i16(spanish.yAdvance + 16);
  const Rect field{i16(body.x + theme.margin), i16(body.y + theme.gap), i16(body.width - 2 * theme.margin), fieldH};
  t.stroke(field, Paint::solid(Color::Black), 2);
  char shown[56];
  snprintf(shown, sizeof shown, "%s_", query_);
  t.setFont(kSlotScratch, query_[0] ? spanish : font(FontRole::ChromeSmall));
  TextStyle fieldText = styled(kSlotScratch);
  fieldText.maxLines = 1;
  t.text(Rect{i16(field.x + 10), field.y, i16(field.width - 20), field.height}, query_[0] ? shown : tr(Str::SearchHint),
         fieldText);

  // The keyboard at the bottom; the results between.
  const int16_t keyboardH = SpanishKeyboard::height();
  const int16_t keysY = i16(body.bottom() - keyboardH);
  const Rect list{body.x, i16(field.bottom() + theme.gap), body.width, i16(keysY - field.bottom() - 2 * theme.gap)};
  const int16_t rowH =
      i16(font(FontRole::SpanishText).yAdvance + 10 < 48 ? 48 : font(FontRole::SpanishText).yAdvance + 10);
  rowCount_ = 0;
  int16_t y = list.y;
  const BitmapFont& headFont = font(FontRole::SpanishText);
  for (uint8_t i = 0; i < search_.count() && rowCount_ < kMaxRows && y + rowH <= list.bottom(); ++i) {
    const sr::Result& result = search_.at(i);
    pk::Lemma lemma;
    if (!pack.lemma(result.lemma, lemma)) continue;
    const Rect row{list.x, y, list.width, rowH};
    const Rect inner{i16(row.x + theme.margin), row.y, i16(row.width - 2 * theme.margin), row.height};
    // The headword, the form found (fui) after it, then the gloss.
    char head[72];
    if (result.how == sr::Match::Form) {
      snprintf(head, sizeof head, "%s (%s)", pack.str(lemma.es), pack.str(result.form));
    } else {
      snprintf(head, sizeof head, "%s", pack.str(lemma.es));
    }
    t.setFont(kSlotScratch, headFont);
    TextStyle headStyle = styled(kSlotScratch);
    headStyle.maxLines = 1;
    int16_t headW = i16(t.measureText(kSlotScratch, head, headStyle).width + 2);
    if (headW > inner.width / 2) headW = i16(inner.width / 2);
    t.text(Rect{inner.x, inner.y, headW, inner.height}, head, headStyle);
    const bool hidden = lemma.reg == pk::Register::Vulgar && !app_.profile().showVulgar;
    const int16_t glossX = i16(inner.x + headW + 2 * theme.gap);
    if (glossX < inner.right()) {
      TextStyle gloss = styled(kSlotSmall);
      gloss.maxLines = 1;
      t.text(Rect{glossX, inner.y, i16(inner.right() - glossX), inner.height}, hidden ? "vulgar" : pack.str(lemma.en),
             gloss);
    }
    t.fill(Rect{inner.x, i16(row.bottom() - 1), inner.width, 1}, Paint::dither(Color::LightGray));
    rows_[rowCount_++] = row;
    if (platform::kSimulator && traceNext_) {
      platform::log("target search/row%u %d %d", i, row.x + row.width / 2, row.y + row.height / 2);
    }
    y = i16(y + rowH);
  }
  if (query_[0] && search_.count() == 0 && y + rowH <= list.bottom()) {
    t.text(Rect{i16(list.x + theme.margin), y, i16(list.width - 2 * theme.margin), rowH}, tr(Str::SearchNothing),
           styled(kSlotBody));
  } else if (search_.more() && y + rowH <= list.bottom()) {
    t.text(Rect{i16(list.x + theme.margin), y, i16(list.width - 2 * theme.margin), rowH}, tr(Str::SearchMore),
           styled(kSlotSmall));
  }
  keyboard_.draw(screen, Rect{full.x, keysY, full.width, keyboardH}, tr(Str::Open));
  traceNext_ = false;
}

void SearchScreen::open(const uint8_t row) {
  if (row >= search_.count()) return;
  showEntry(app_, search_.at(row).lemma, core::usage::Source::Search);
}

void SearchScreen::onAction(const app::ActionEvent& event) {
  const SpanishKeyboard::Edit edit = keyboard_.apply(event, query_, sizeof query_);
  if (edit == SpanishKeyboard::Edit::Changed) {
    run();
    traceNext_ = true;
    // The list under the query changes: fast, it is a small part of the
    // screen and typing should not flash.
    app_.invalidate();
  } else if (edit == SpanishKeyboard::Edit::Ok) {
    open(0);
  }
}

bool SearchScreen::onInput(const InputEvent& event) {
  if (event.kind != InputEvent::Kind::Tap) return false;
  for (uint8_t i = 0; i < rowCount_; ++i) {
    if (rows_[i].contains(event.x, event.y)) {
      open(i);
      return true;
    }
  }
  return false;
}

}  // namespace tinta::ui
