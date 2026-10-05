#include "ui/views/Exercises.h"

#include <stdio.h>
#include <string.h>

#include <new>

#include "core/lang/Charset.h"
#include "core/lang/Utf8.h"
#include "icons/Icons.h"
#include "platform/Log.h"
#include "ui/Strings.h"
#include "ui/TypesetView.h"
#include "ui/views/Cards.h"
#include "ui/views/Chrome.h"

namespace tinta::ui {
namespace {

namespace pk = core::pack;
namespace ss = core::session;
using core::Grade;
using core::ItemKind;
using freeink::ui::Color;
using freeink::ui::Paint;
using freeink::ui::Rect;
using freeink::ui::TextAlign;
using freeink::ui::TextStyle;
using ss::Format;

constexpr int16_t kGap = 10;
constexpr int16_t kTight = 4;

int16_t i16(int32_t v) { return static_cast<int16_t>(v); }

Column columnFor(const Rect area) {
  Column col;
  col.x = area.x;
  col.y = area.y;
  col.width = area.width;
  col.bottom = area.bottom();
  return col;
}

TextStyle scratchStyle(Color color = Color::Black, TextAlign align = TextAlign::Left, uint8_t lines = 1) {
  TextStyle s;
  s.font = kSlotScratch;
  s.color = color;
  s.align = align;
  s.maxLines = lines;
  return s;
}

// "Fill the gap · NOUN · f"
void promptAndLabel(const char* prompt, const pk::Lemma* lemma, char* out, size_t cap) {
  char label[64] = {};
  if (lemma) formatLemmaLabel(*lemma, label, sizeof label);
  if (prompt && label[0]) {
    snprintf(out, cap, "%s · %s", prompt, label);
  } else {
    snprintf(out, cap, "%s", prompt ? prompt : label);
  }
}

// "nosotros · present" for a conjugation item.
void personAndTense(const pk::Item& item, char* out, size_t cap) {
  snprintf(out, cap, "%s · %s", personLabel(pk::tagPerson(item.b)), tenseName(pk::tagTense(item.b)));
}

// "Press any key to go on" at the bottom of the area, if there is room.
void drawGoOn(freeink::ui::DisplayTarget& t, Column& col, bool touch) {
  const BitmapFont& f = font(FontRole::ChromeSmall);
  if (col.room() < f.yAdvance + kTight) return;
  t.setFont(kSlotScratch, f);
  t.text(Rect{col.x, i16(col.bottom - f.yAdvance), col.width, f.yAdvance}, tr(touch ? Str::GoOnTouch : Str::GoOnKeys),
         scratchStyle(Color::Black, TextAlign::Center));
}

// A check (the answer) or a cross (a wrong choice) at the right end of `r`.
void drawTick(freeink::ui::DisplayTarget& t, const Rect r, bool right) {
  drawIcon(t, Rect{i16(r.right() - 26), r.y, 24, r.height}, right ? icons::kCheck24 : icons::kCross24);
}

}  // namespace

// ── Choices ──────────────────────────────────────────────────────────────────

bool ChoiceView::accepts(const CardInput& card) const { return ss::isChoice(card.format); }

ExerciseFormat ChoiceView::journalFormat() const {
  switch (card_.format) {
    case Format::ChooseMeaning:
      return ExerciseFormat::ChooseMeaning;
    case Format::ChooseWord:
      return ExerciseFormat::ChooseWord;
    case Format::ChooseGap:
      return ExerciseFormat::ChooseGap;
    case Format::ChooseArticle:
      return ExerciseFormat::ChooseArticle;
    case Format::ChooseForm:
      return ExerciseFormat::ChooseForm;
    default:
      return ExerciseFormat::Unknown;
  }
}

bool ChoiceView::load(const CardInput& card) {
  card_ = card;
  chosen_ = -1;
  const pk::Pack& pack = *card.pack;
  const pk::Item& item = card.item;
  if (!ss::pickOptions(pack, item, card.format, item.uid, card.reps, card.showVulgar, options_)) return false;

  hasLemma_ = false;
  hasSentence_ = false;
  lemma_ = pk::Lemma{};
  sentence_ = pk::Sentence{};
  if (item.kind == ItemKind::Cloze) {
    hasSentence_ = pack.sentence(item.a, sentence_);
    pk::Token token;
    if (hasSentence_ && pack.token(sentence_, static_cast<uint8_t>(item.b), token) && token.lemma != pk::kNone16) {
      hasLemma_ = pack.lemma(token.lemma, lemma_);
    }
  } else {
    hasLemma_ = pack.lemma(item.a, lemma_);
    if (hasLemma_) hasSentence_ = pack.sentence(pack.lemmaExample(lemma_, 0), sentence_);
  }

  switch (card.format) {
    case Format::ChooseMeaning:
      promptAndLabel(tr(Str::PromptMeaning), hasLemma_ ? &lemma_ : nullptr, label_, sizeof label_);
      break;
    case Format::ChooseWord:
      promptAndLabel(tr(Str::PromptProduce), hasLemma_ ? &lemma_ : nullptr, label_, sizeof label_);
      break;
    case Format::ChooseGap:
      promptAndLabel(tr(Str::PromptCloze), nullptr, label_, sizeof label_);
      break;
    case Format::ChooseArticle:
      promptAndLabel(tr(Str::PromptGender), nullptr, label_, sizeof label_);
      break;
    case Format::ChooseForm:
      promptAndLabel(tr(Str::PromptConjugate), nullptr, label_, sizeof label_);
      break;
    default:
      label_[0] = '\0';
      break;
  }

  bar_ = ChoiceBar{};
  for (uint8_t i = 0; i < options_.count; ++i) {
    ss::optionText(pack, item, card.format, options_.options[i], texts_[i], sizeof texts_[i]);
    // The footer says what the key under it answers: the option's number, or
    // for el / la the article itself.
    if (card.format == Format::ChooseArticle) {
      snprintf(cells_[i], sizeof cells_[i], "%s", texts_[i]);
    } else {
      snprintf(cells_[i], sizeof cells_[i], "%u", i + 1);
    }
    bar_.cells[i].label = cells_[i];
  }
  platform::log("choice answer %u of %u", options_.answer + 1, options_.count);
  return true;
}

void ChoiceView::drawPrompt(freeink::ui::DisplayTarget& t, Column& col) {
  const pk::Pack& pack = *card_.pack;
  const TextSize size = card_.size;
  char text[96];
  switch (card_.format) {
    case Format::ChooseMeaning:
      formatHeadword(pack, lemma_, true, text, sizeof text);
      drawHeadword(t, col, text, 0);
      drawText(t, col, pack.str(lemma_.pron), FontRole::Respelling, size, 1, kGap);
      break;
    case Format::ChooseWord:
      drawText(t, col, pack.str(lemma_.en), FontRole::EnglishGloss, size, 3, kGap);
      break;
    case Format::ChooseGap:
      if (!hasSentence_) break;
      drawSentence(t, col, pack, sentence_, pk::kNone16, static_cast<int16_t>(card_.item.b), true, size, kTight);
      drawText(t, col, pack.str(sentence_.en), FontRole::EnglishTranslation, size, 3, kGap);
      break;
    case Format::ChooseForm:
      drawText(t, col, pack.str(lemma_.es), FontRole::SpanishEmphasis, size, 1, kTight);
      drawText(t, col, pack.str(lemma_.en), FontRole::EnglishTranslation, size, 2, kTight);
      personAndTense(card_.item, text, sizeof text);
      drawText(t, col, text, FontRole::SpanishAside, size, 1, kGap);
      break;
    case Format::ChooseArticle:
      drawHeadword(t, col, pack.str(lemma_.es), 0);
      drawText(t, col, pack.str(lemma_.en), FontRole::EnglishGloss, size, 2, kGap);
      break;
    default:
      break;
  }
}

void ChoiceView::drawExplanation(freeink::ui::DisplayTarget& t, Column& col) {
  const pk::Pack& pack = *card_.pack;
  const TextSize size = card_.size;
  char text[128];
  const bool right = chosen_ == options_.answer;
  if (right) {
    snprintf(text, sizeof text, "%s", tr(Str::Right));
  } else {
    snprintf(text, sizeof text, tr(Str::TheAnswerFmt), texts_[options_.answer]);
  }
  drawTextIn(t, col, text, font(FontRole::ChromeBodyBold), 2, kTight);
  switch (card_.format) {
    case Format::ChooseMeaning:
      if (hasSentence_ && drawSentence(t, col, pack, sentence_, card_.item.a, -1, false, size, kTight)) {
        drawText(t, col, pack.str(sentence_.en), FontRole::EnglishTranslation, size, 2, kTight);
      }
      break;
    case Format::ChooseWord:
      formatHeadword(pack, lemma_, true, text, sizeof text);
      drawText(t, col, text, FontRole::SpanishEmphasis, size, 1, 0);
      drawText(t, col, pack.str(lemma_.pron), FontRole::Respelling, size, 1, kTight);
      if (hasSentence_) drawSentence(t, col, pack, sentence_, card_.item.a, -1, false, size, kTight);
      break;
    case Format::ChooseGap:
      if (hasSentence_) {
        drawSentence(t, col, pack, sentence_, pk::kNone16, static_cast<int16_t>(card_.item.b), false, size, kTight);
        drawText(t, col, pack.str(sentence_.note), FontRole::EnglishTranslation, size, 0, kTight);
      }
      if (hasLemma_) {
        snprintf(text, sizeof text, "%s " TINTA_EM_DASH " %s", pack.str(lemma_.es), pack.str(lemma_.en));
        drawText(t, col, text, FontRole::EnglishTranslation, size, 2, kTight);
      }
      return;
    case Format::ChooseForm: {
      const char* form = texts_[options_.answer];
      snprintf(text, sizeof text, "%s %s%s", personLabel(pk::tagPerson(card_.item.b)),
               pk::tagTense(card_.item.b) == pk::Tense::NegativeImperative ? "no " : "", form);
      drawText(t, col, text, FontRole::SpanishEmphasis, size, 2, kTight);
      break;
    }
    case Format::ChooseArticle:
      formatHeadword(pack, lemma_, true, text, sizeof text);
      drawText(t, col, text, FontRole::SpanishEmphasis, size, 1, kTight);
      break;
    default:
      break;
  }
  if (hasLemma_) drawText(t, col, pack.str(lemma_.note), FontRole::EnglishTranslation, size, 0, kTight);
}

void ChoiceView::draw(app::UiScreen& screen, const Rect area, const bool back) {
  freeink::ui::DisplayTarget& t = displayOf(screen);
  Column col = columnFor(area);
  drawText(t, col, label_, FontRole::Label, card_.size, 1, kGap);
  drawPrompt(t, col);

  // The options, numbered as the footer cells over the keys are.
  const bool spanish = card_.format != Format::ChooseMeaning;
  const BitmapFont& optionFont = font(spanish ? FontRole::SpanishText : FontRole::EnglishGloss, card_.size);
  const int16_t minRow = i16(card_.touch ? 52 : 40);
  constexpr int16_t kNumberW = 30;
  constexpr int16_t kMarkW = 30;
  const int16_t textX = i16(area.x + kNumberW + kGap);
  const int16_t textW = i16(area.width - kNumberW - kGap - kMarkW);
  for (uint8_t i = 0; i < options_.count; ++i) {
    t.setFont(kSlotScratch, optionFont);
    const freeink::ui::Size size =
        freeink::ui::measureWrappedText(t, texts_[i], scratchStyle(Color::Black, TextAlign::Left, 2), textW);
    int16_t rowH = i16(size.height + 14);
    if (rowH < minRow) rowH = minRow;
    if (col.room() < rowH) break;
    const Rect row{area.x, col.y, area.width, rowH};
    // The number in a box, as on the footer.
    const Rect box{area.x, i16(row.y + (rowH - 28) / 2), kNumberW, 28};
    t.stroke(box, Paint::solid(Color::Black), 1);
    TextStyle number;
    number.font = kSlotBodyBold;
    number.align = TextAlign::Center;
    char digit[4];
    snprintf(digit, sizeof digit, "%u", i + 1);
    t.text(box, digit, number);
    t.setFont(kSlotScratch, optionFont);
    t.text(Rect{textX, row.y, textW, rowH}, texts_[i], scratchStyle(Color::Black, TextAlign::Left, 2));
    if (back && i == options_.answer) drawTick(t, row, true);
    if (back && i == chosen_ && chosen_ != options_.answer) drawTick(t, row, false);
    t.fill(Rect{area.x, i16(row.bottom() - 2), area.width, 2}, Paint::dither(Color::LightGray));
    if (!back && card_.touch) {
      screen.frame().hit(row, static_cast<freeink::ui::ActionId>(kExerciseAction + i), 0, freeink::ui::InputTouch);
      if (platform::kSimulator) {
        platform::log("target option/%u %d %d", i + 1, row.x + row.width / 2, row.y + row.height / 2);
      }
    }
    col.skip(rowH);
  }
  if (!back) return;
  col.skip(kGap);
  drawExplanation(t, col);
  if (goOnHint()) drawGoOn(t, col, card_.touch);
}

ExerciseView::Reply ChoiceView::choose(const uint8_t option, Grade& grade) {
  if (option >= options_.count || chosen_ >= 0) return Reply::Ignored;
  chosen_ = static_cast<int8_t>(option);
  const bool right = option == options_.answer;
  grade = ss::gradeChoice(right);
  platform::log("choice %u %s", option + 1, right ? "right" : "wrong");
  return Reply::Answered;
}

ExerciseView::Reply ChoiceView::onChoice(const uint8_t cell, Grade& grade) { return choose(cell, grade); }

uint8_t ChoiceView::optionTexts(const char** out, const uint8_t cap, uint8_t& answer) const {
  const uint8_t count = options_.count < cap ? options_.count : cap;
  for (uint8_t i = 0; i < count; ++i) out[i] = texts_[i];
  answer = options_.answer;
  return count;
}

AnswerDetail ChoiceView::answerDetail() const {
  AnswerDetail d;
  if (chosen_ >= 0) d.chosen = static_cast<uint8_t>(chosen_);
  return d;
}

ExerciseView::Reply ChoiceView::onAction(const app::ActionEvent& event, Grade& grade) {
  if (event.action < kExerciseAction || event.action >= kExerciseAction + ss::OptionSet::kMax) return Reply::Ignored;
  return choose(static_cast<uint8_t>(event.action - kExerciseAction), grade);
}

// ── Word order ───────────────────────────────────────────────────────────────

bool WordOrderView::accepts(const CardInput& card) const { return card.format == Format::BuildSentence; }

bool WordOrderView::load(const CardInput& card) {
  card_ = card;
  lastWrong_ = -1;
  if (!card.pack->sentence(card.item.a, sentence_)) return false;
  if (!order_.begin(*card.pack, sentence_, ss::showingSeed(card.item.uid, card.reps))) return false;
  snprintf(label_, sizeof label_, "%s", tr(Str::PromptOrder));
  cursor_ = 0;
  // For the simulator flows: the tray slot of each word in turn.
  char line[96];
  size_t used = 0;
  uint16_t taken = 0;
  for (uint8_t w = 0; w < sentence_.tokenCount && used + 4 < sizeof line; ++w) {
    pk::Token token;
    card.pack->token(sentence_, w, token);
    const pk::TextSpan word = card.pack->tokenText(sentence_, token);
    for (uint8_t k = 0; k < order_.tileCount(); ++k) {
      const pk::TextSpan tile = order_.tile(k);
      if ((taken >> k) & 1u || tile.length != word.length || memcmp(tile.text, word.text, word.length) != 0) continue;
      taken = static_cast<uint16_t>(taken | (1u << k));
      used += static_cast<size_t>(snprintf(line + used, sizeof line - used, " %u", k));
      break;
    }
  }
  platform::log("tiles%s", line);
  logCursor();
  return true;
}

void WordOrderView::logCursor() const { platform::log("tile cursor %u", cursor_); }

// The words placed so far, in order.
void WordOrderView::drawBuilt(freeink::ui::DisplayTarget& t, Column& band) const {
  char built[160];
  size_t used = 0;
  built[0] = '\0';
  for (uint8_t k = 0; k < order_.placedCount() && used + 2 < sizeof built; ++k) {
    const pk::TextSpan word = order_.tile(order_.placedAt(k));
    const int n = snprintf(built + used, sizeof built - used, "%s%.*s", used ? " " : "", static_cast<int>(word.length),
                           word.text);
    if (n < 0) break;
    used += static_cast<size_t>(n) < sizeof built - used ? static_cast<size_t>(n) : sizeof built - used - 1;
  }
  drawText(t, band, built, FontRole::SpanishText, card_.size, 2, 0);
}

// The tray: the words as tiles, flowing left to right and wrapping. A placed
// word leaves its outline, so the others do not move.
void WordOrderView::drawTray(app::UiScreen& screen, freeink::ui::DisplayTarget& t, Column& col) {
  const BitmapFont& spanish = font(FontRole::SpanishText, card_.size);
  // Touch tiles at least as tall as a finger needs.
  const int16_t tileH = i16(spanish.yAdvance + 14 < (card_.touch ? 52 : 0) ? 52 : spanish.yAdvance + 14);
  const int16_t pad = 12;
  int16_t x = col.x;
  int16_t y = col.y;
  t.setFont(kSlotScratch, spanish);
  for (uint8_t i = 0; i < order_.tileCount(); ++i) {
    const pk::TextSpan word = order_.tile(i);
    char text[48];
    snprintf(text, sizeof text, "%.*s", static_cast<int>(word.length), word.text);
    const int16_t w = i16(core::text::Typesetter::measure(spanish, text, strlen(text)) + 2 * pad);
    if (x > col.x && x + w > col.x + col.width) {
      x = col.x;
      y = i16(y + tileH + kGap);
    }
    if (y + tileH > col.bottom) break;
    const Rect tile{x, y, w, tileH};
    if (order_.placed(i)) {
      t.stroke(tile, Paint::dither(Color::LightGray), 1);
    } else {
      // The key cursor: a heavier frame and a bar under the tile. Moving it
      // takes away little ink, so a fast refresh leaves little ghost (an
      // inverted tile would leave a grey block).
      const bool focused = !card_.touch && i == cursor_;
      t.stroke(tile, Paint::solid(Color::Black), focused ? 4 : 2);
      if (focused) t.fill(Rect{tile.x, i16(tile.bottom() + 3), tile.width, 4}, Paint::solid(Color::Black));
      t.setFont(kSlotScratch, spanish);
      t.text(tile, text, scratchStyle(Color::Black, TextAlign::Center));
      if (i == lastWrong_) {
        // A cross in the corner: tried out of turn.
        t.line(freeink::ui::Point{i16(tile.right() - 12), i16(tile.y + 4)},
               freeink::ui::Point{i16(tile.right() - 4), i16(tile.y + 12)}, 2, Paint::solid(Color::Black));
        t.line(freeink::ui::Point{i16(tile.right() - 4), i16(tile.y + 4)},
               freeink::ui::Point{i16(tile.right() - 12), i16(tile.y + 12)}, 2, Paint::solid(Color::Black));
      }
      if (card_.touch) {
        screen.frame().hit(tile, static_cast<freeink::ui::ActionId>(kExerciseAction + i), 0, freeink::ui::InputTouch);
        if (platform::kSimulator)
          platform::log("target tile/%u %d %d", i, tile.x + tile.width / 2, tile.y + tile.height / 2);
      }
    }
    x = i16(x + w + kGap);
  }
  col.y = i16(y + tileH + kGap);
}

void WordOrderView::draw(app::UiScreen& screen, const Rect area, const bool back) {
  freeink::ui::DisplayTarget& t = displayOf(screen);
  const pk::Pack& pack = *card_.pack;
  const TextSize size = card_.size;
  Column col = columnFor(area);
  drawText(t, col, label_, FontRole::Label, size, 1, kGap);
  drawText(t, col, pack.str(sentence_.en), FontRole::EnglishGloss, size, 3, kGap);

  // The sentence so far, in a band of two lines that does not move.
  const BitmapFont& spanish = font(FontRole::SpanishText, size);
  const int16_t bandH = i16(2 * spanish.yAdvance + kTight);
  Column band = col;
  band.bottom = i16(col.y + bandH < col.bottom ? col.y + bandH : col.bottom);
  if (back) {
    drawSentence(t, band, pack, sentence_, pk::kNone16, -1, false, size, 0);
  } else {
    drawBuilt(t, band);
  }
  col.skip(bandH);
  drawRule(t, col, kGap);

  // The tray, until the sentence is complete; the result needs none.
  if (!back) drawTray(screen, t, col);

  char line[64];
  if (back) {
    if (order_.mistakes() == 0) {
      snprintf(line, sizeof line, "%s", tr(Str::Right));
    } else if (order_.mistakes() == 1) {
      snprintf(line, sizeof line, "%s", tr(Str::OneMistake));
    } else {
      snprintf(line, sizeof line, tr(Str::MistakesFmt), order_.mistakes());
    }
    drawTextIn(t, col, line, font(FontRole::ChromeBodyBold), 1, kTight);
    drawText(t, col, pack.str(sentence_.note), FontRole::EnglishTranslation, size, 0, kTight);
    if (goOnHint()) drawGoOn(t, col, card_.touch);
  } else if (order_.mistakes() > 0) {
    if (order_.mistakes() == 1) {
      snprintf(line, sizeof line, "%s", tr(Str::OneMistake));
    } else {
      snprintf(line, sizeof line, tr(Str::MistakesFmt), order_.mistakes());
    }
    drawTextIn(t, col, line, font(FontRole::ChromeSmall), 1, 0);
  }
}

AnswerDetail WordOrderView::answerDetail() const {
  AnswerDetail d;
  d.attempts = order_.mistakes();
  return d;
}

bool WordOrderView::keyHints(const KeyMap& keys, ChoiceBar& out) const {
  out = ChoiceBar{};
  for (uint8_t i = 0; i < kFooterCellCount; ++i) {
    switch (keys.footerCell(i).key) {
      case Key::Confirm:
        out.cells[i].label = tr(Str::Place);
        break;
      case Key::Left:
        out.cells[i].icon = &icons::kChevronLeft24;
        break;
      case Key::Right:
        out.cells[i].icon = &icons::kChevronRight24;
        break;
      default:
        break;
    }
  }
  return true;
}

void WordOrderView::moveCursor(const int8_t dir) {
  const uint8_t n = order_.tileCount();
  for (uint8_t step = 0; step < n; ++step) {
    cursor_ = static_cast<uint8_t>((cursor_ + dir + n) % n);
    if (!order_.placed(cursor_)) break;
  }
  logCursor();
}

ExerciseView::Reply WordOrderView::pick(const uint8_t tile, Grade& grade) {
  if (tile >= order_.tileCount() || order_.placed(tile)) return Reply::Ignored;
  if (order_.pick(tile)) {
    lastWrong_ = -1;
    platform::log("tile %u placed", tile);
    if (order_.done()) {
      grade = ss::gradeTiles(order_.mistakes());
      platform::log("tiles done, %u mistakes", order_.mistakes());
      return Reply::Answered;
    }
    if (cursor_ == tile) moveCursor(1);
  } else {
    lastWrong_ = static_cast<int8_t>(tile);
    platform::log("tile %u wrong", tile);
  }
  return Reply::Handled;
}

ExerciseView::Reply WordOrderView::onKey(const InputEvent& event, Grade& grade) {
  if (event.kind != InputEvent::Kind::Key || event.hold) return Reply::Ignored;
  switch (event.key) {
    case Key::Left:
      moveCursor(-1);
      return Reply::Handled;
    case Key::Right:
      moveCursor(1);
      return Reply::Handled;
    case Key::Confirm:
    case Key::Down:
      return pick(cursor_, grade);
    default:
      return Reply::Ignored;
  }
}

ExerciseView::Reply WordOrderView::onAction(const app::ActionEvent& event, Grade& grade) {
  if (event.action < kExerciseAction || event.action >= kExerciseAction + ss::WordOrder::kMaxTiles) {
    return Reply::Ignored;
  }
  return pick(static_cast<uint8_t>(event.action - kExerciseAction), grade);
}

// ── Typed answers ────────────────────────────────────────────────────────────

bool TypedView::accepts(const CardInput& card) const { return ss::isTyped(card.format) && card.touch; }

ExerciseFormat TypedView::journalFormat() const {
  switch (card_.format) {
    case Format::TypeWord:
      return ExerciseFormat::TypeWord;
    case Format::TypeGap:
      return ExerciseFormat::TypeGap;
    case Format::TypeForm:
      return ExerciseFormat::TypeForm;
    default:
      return ExerciseFormat::Unknown;
  }
}

bool TypedView::load(const CardInput& card) {
  card_ = card;
  answered_ = false;
  verdict_ = core::lang::Verdict::Wrong;
  typed_[0] = '\0';
  keyboard_.traceNext();
  const pk::Pack& pack = *card.pack;
  const pk::Item& item = card.item;
  hasLemma_ = false;
  hasSentence_ = false;
  if (item.kind == ItemKind::Cloze) {
    hasSentence_ = pack.sentence(item.a, sentence_);
    pk::Token token;
    if (hasSentence_ && pack.token(sentence_, static_cast<uint8_t>(item.b), token) && token.lemma != pk::kNone16) {
      hasLemma_ = pack.lemma(token.lemma, lemma_);
    }
  } else {
    hasLemma_ = pack.lemma(item.a, lemma_);
    if (hasLemma_) hasSentence_ = pack.sentence(pack.lemmaExample(lemma_, 0), sentence_);
  }
  ss::typedAnswer(pack, item, card.format, expected_, sizeof expected_);
  if (!expected_[0]) return false;
  switch (card.format) {
    case Format::TypeWord:
      promptAndLabel(tr(Str::PromptProduce), hasLemma_ ? &lemma_ : nullptr, label_, sizeof label_);
      break;
    case Format::TypeGap:
      promptAndLabel(tr(Str::PromptCloze), nullptr, label_, sizeof label_);
      break;
    case Format::TypeForm:
      promptAndLabel(tr(Str::PromptConjugate), nullptr, label_, sizeof label_);
      break;
    default:
      break;
  }
  if (platform::kSimulator) platform::log("expect %s", expected_);
  return true;
}

void TypedView::drawPrompt(freeink::ui::DisplayTarget& t, Column& col) {
  const pk::Pack& pack = *card_.pack;
  const TextSize size = card_.size;
  char text[96];
  switch (card_.format) {
    case Format::TypeWord:
      drawText(t, col, pack.str(lemma_.en), FontRole::EnglishGloss, size, 3, kGap);
      break;
    case Format::TypeGap:
      if (!hasSentence_) break;
      drawSentence(t, col, pack, sentence_, pk::kNone16, static_cast<int16_t>(card_.item.b), true, size, kTight);
      drawText(t, col, pack.str(sentence_.en), FontRole::EnglishTranslation, size, 3, kGap);
      break;
    case Format::TypeForm:
      drawText(t, col, pack.str(lemma_.es), FontRole::SpanishEmphasis, size, 1, kTight);
      drawText(t, col, pack.str(lemma_.en), FontRole::EnglishTranslation, size, 2, kTight);
      personAndTense(card_.item, text, sizeof text);
      drawText(t, col, text, FontRole::SpanishAside, size, 1, kGap);
      break;
    default:
      break;
  }
}

void TypedView::draw(app::UiScreen& screen, const Rect area, const bool back) {
  freeink::ui::DisplayTarget& t = displayOf(screen);
  const pk::Pack& pack = *card_.pack;
  const TextSize size = card_.size;
  Column col = columnFor(area);
  drawText(t, col, label_, FontRole::Label, size, 1, kGap);
  drawPrompt(t, col);

  const BitmapFont& spanish = font(FontRole::SpanishEmphasis, size);
  if (!back) {
    // The keyboard across the whole width at the bottom, the same place on
    // every card; the answer field just above it.
    const Rect full = screen.frame().screen();
    const int16_t keyboardH = SpanishKeyboard::height();
    const int16_t keysY = i16(area.bottom() - keyboardH);
    const int16_t fieldH = i16(spanish.yAdvance + 16);
    const int16_t fieldY = i16(keysY - kGap - fieldH);
    // Whatever of the prompt does not fit above the field is cut.
    if (fieldY < col.y) return;
    const Rect field{col.x, fieldY, col.width, fieldH};
    t.stroke(field, Paint::solid(Color::Black), 2);
    char shown[72];
    snprintf(shown, sizeof shown, "%s_", typed_);
    t.setFont(kSlotScratch, typed_[0] ? spanish : font(FontRole::ChromeSmall));
    t.text(Rect{i16(field.x + 10), field.y, i16(field.width - 20), field.height}, typed_[0] ? shown : tr(Str::TypeHere),
           scratchStyle(Color::Black, TextAlign::Left));

    keyboard_.draw(screen, Rect{full.x, keysY, full.width, keyboardH}, tr(Str::Check));
    return;
  }

  // The result: the verdict, the answer and what was typed, the note.
  char line[128];
  switch (verdict_) {
    case core::lang::Verdict::Exact:
      snprintf(line, sizeof line, "%s", tr(Str::Right));
      break;
    case core::lang::Verdict::Accents:
      snprintf(line, sizeof line, "%s", tr(Str::RightAccents));
      break;
    case core::lang::Verdict::Typo:
      snprintf(line, sizeof line, "%s", tr(Str::RightTypo));
      break;
    case core::lang::Verdict::Alternative:
      snprintf(line, sizeof line, tr(Str::RightAlternativeFmt), expected_);
      break;
    case core::lang::Verdict::Wrong:
      snprintf(line, sizeof line, tr(Str::TheAnswerFmt), expected_);
      break;
  }
  drawTextIn(t, col, line, font(FontRole::ChromeBodyBold), 2, kTight);
  switch (card_.format) {
    case Format::TypeGap:
      if (hasSentence_) {
        drawSentence(t, col, pack, sentence_, pk::kNone16, static_cast<int16_t>(card_.item.b), false, size, kTight);
      }
      break;
    case Format::TypeForm:
      snprintf(line, sizeof line, "%s %s%s", personLabel(pk::tagPerson(card_.item.b)),
               pk::tagTense(card_.item.b) == pk::Tense::NegativeImperative ? "no " : "", expected_);
      drawText(t, col, line, FontRole::SpanishEmphasis, size, 2, kTight);
      break;
    default:
      if (hasLemma_) {
        formatHeadword(pack, lemma_, true, line, sizeof line);
        drawText(t, col, line, FontRole::SpanishEmphasis, size, 1, 0);
        drawText(t, col, pack.str(lemma_.pron), FontRole::Respelling, size, 1, kTight);
      }
      break;
  }
  snprintf(line, sizeof line, tr(Str::YouTypedFmt), typed_[0] ? typed_ : TINTA_EM_DASH);
  drawText(t, col, line, FontRole::EnglishTranslation, size, 2, kGap);
  if (hasLemma_) drawText(t, col, pack.str(lemma_.note), FontRole::EnglishTranslation, size, 0, kTight);
  if (goOnHint()) drawGoOn(t, col, true);
}

AnswerDetail TypedView::answerDetail() const {
  AnswerDetail d;
  if (answered_) d.typed = typed_;
  return d;
}

ExerciseView::Reply TypedView::onAction(const app::ActionEvent& event, Grade& grade) {
  if (answered_) return Reply::Ignored;
  const SpanishKeyboard::Edit edit = keyboard_.apply(event, typed_, sizeof typed_);
  if (edit == SpanishKeyboard::Edit::Changed) return Reply::Handled;
  if (edit == SpanishKeyboard::Edit::Ok) {
    const char* alternatives = card_.format == Format::TypeWord && hasLemma_ ? card_.pack->str(lemma_.alt) : nullptr;
    verdict_ = core::lang::checkAnswer(typed_, expected_, alternatives);
    answered_ = true;
    grade = ss::gradeTyped(verdict_);
    static const char* const kNames[] = {"wrong", "exact", "accents", "typo", "alternative"};
    platform::log("typed %s", kNames[static_cast<uint8_t>(verdict_)]);
    return Reply::Answered;
  }
  return Reply::Ignored;
}

// ── Registry ─────────────────────────────────────────────────────────────────

namespace {

struct Views {
  ChoiceView choice;
  WordOrderView wordOrder;
  TypedView typed;
  FlashcardView flashcard;
  RevealCardView reveal;
  ExerciseView* const all[5] = {&choice, &wordOrder, &typed, &flashcard, &reveal};
};

Views* gViews = nullptr;

}  // namespace

bool openExerciseViews() {
  if (!gViews) gViews = new (std::nothrow) Views;
  return gViews != nullptr;
}

void closeExerciseViews() {
  delete gViews;
  gViews = nullptr;
}

ExerciseView* const* exerciseViews(uint8_t& count) {
  count = sizeof gViews->all / sizeof gViews->all[0];
  return gViews->all;
}

}  // namespace tinta::ui
