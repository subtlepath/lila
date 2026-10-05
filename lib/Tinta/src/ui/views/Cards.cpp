#include "ui/views/Cards.h"

#include <stdio.h>
#include <string.h>

#include "core/lang/Charset.h"
#include "ui/Strings.h"

namespace tinta::ui {
namespace {

namespace pk = core::pack;
using core::ItemKind;
using freeink::ui::Rect;

constexpr int16_t kGap = 10;
constexpr int16_t kTight = 4;

Column columnFor(const Rect area) {
  Column col;
  col.x = area.x;
  col.y = area.y;
  col.width = area.width;
  col.bottom = area.bottom();
  return col;
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

// A small deterministic shuffle (xorshift seeded by the uid): the same item
// always shows the same order, so a resumed card looks the same.
uint32_t nextRandom(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

}  // namespace

// ── Flashcard ────────────────────────────────────────────────────────────────

ExerciseFormat FlashcardView::journalFormat() const {
  return card_.item.kind == ItemKind::VocabProduce ? ExerciseFormat::FlashcardProduce
                                                   : ExerciseFormat::FlashcardRecognise;
}

bool FlashcardView::accepts(const CardInput& card) const {
  return card.format == core::session::Format::Flashcard &&
         (card.item.kind == ItemKind::VocabRecognise || card.item.kind == ItemKind::VocabProduce);
}

bool FlashcardView::load(const CardInput& card) {
  card_ = card;
  const pk::Pack& pack = *card.pack;
  pack.lemma(card.item.a, lemma_);
  hasExample_ = pack.sentence(pack.lemmaExample(lemma_, 0), example_);
  formatHeadword(pack, lemma_, true, headword_, sizeof headword_);
  if (card.item.kind == ItemKind::VocabProduce) {
    promptAndLabel(tr(Str::PromptProduce), &lemma_, label_, sizeof label_);
  } else {
    formatLemmaLabel(lemma_, label_, sizeof label_);
  }
  return true;
}

void FlashcardView::draw(app::UiScreen& screen, const Rect area, const bool back) {
  freeink::ui::DisplayTarget& t = displayOf(screen);
  const pk::Pack& pack = *card_.pack;
  const TextSize size = card_.size;
  Column col = columnFor(area);
  drawText(t, col, label_, FontRole::Label, size, 1, kGap);

  if (card_.item.kind == ItemKind::VocabProduce) {
    // Meaning first; the Spanish appears below it on the back.
    drawText(t, col, pack.str(lemma_.en), FontRole::EnglishGloss, size, 3, kTight);
    if (hasExample_) drawText(t, col, pack.str(example_.en), FontRole::EnglishTranslation, size, 2, kGap);
    if (!back) return;
    drawRule(t, col, kGap);
    drawHeadword(t, col, headword_, 0);
    drawText(t, col, pack.str(lemma_.pron), FontRole::Respelling, size, 1, kGap);
    if (hasExample_) drawSentence(t, col, pack, example_, card_.item.a, -1, false, size, kGap);
    drawText(t, col, pack.str(lemma_.note), FontRole::EnglishTranslation, size, 0, 0);
    return;
  }

  drawHeadword(t, col, headword_, 0);
  drawText(t, col, pack.str(lemma_.pron), FontRole::Respelling, size, 1, kGap);
  if (!back) return;
  drawRule(t, col, kGap);
  drawText(t, col, pack.str(lemma_.en), FontRole::EnglishGloss, size, 3, kGap);
  if (hasExample_ && drawSentence(t, col, pack, example_, card_.item.a, -1, false, size, kTight)) {
    drawText(t, col, pack.str(example_.en), FontRole::EnglishTranslation, size, 3, kGap);
  }
  drawText(t, col, pack.str(lemma_.note), FontRole::EnglishTranslation, size, 0, 0);
}

// ── Reveal card ──────────────────────────────────────────────────────────────

bool RevealCardView::accepts(const CardInput& card) const { return card.format == core::session::Format::Flashcard; }

bool RevealCardView::load(const CardInput& card) {
  card_ = card;
  const pk::Pack& pack = *card.pack;
  const pk::Item& item = card.item;
  hasLemma_ = false;
  hasSentence_ = false;
  text_[0] = '\0';
  label_[0] = '\0';
  lemma_ = pk::Lemma{};
  sentence_ = pk::Sentence{};

  switch (item.kind) {
    case ItemKind::Cloze: {
      hasSentence_ = pack.sentence(item.a, sentence_);
      pk::Token token;
      if (hasSentence_ && pack.token(sentence_, static_cast<uint8_t>(item.b), token) && token.lemma != pk::kNone16) {
        hasLemma_ = pack.lemma(token.lemma, lemma_);
      }
      promptAndLabel(tr(Str::PromptCloze), nullptr, label_, sizeof label_);
      if (hasLemma_) snprintf(text_, sizeof text_, "%s " TINTA_EM_DASH " %s", pack.str(lemma_.es), pack.str(lemma_.en));
      break;
    }
    case ItemKind::Gender:
      hasLemma_ = pack.lemma(item.a, lemma_);
      promptAndLabel(tr(Str::PromptGender), nullptr, label_, sizeof label_);
      if (hasLemma_) formatHeadword(pack, lemma_, true, text_, sizeof text_);
      break;
    case ItemKind::Conjugation: {
      hasLemma_ = pack.lemma(item.a, lemma_);
      const pk::Tense tense = pk::tagTense(item.b);
      const pk::Person person = pk::tagPerson(item.b);
      snprintf(label_, sizeof label_, "%s · %s", tr(Str::PromptConjugate), tenseName(tense));
      if (hasLemma_) {
        const char* form = pack.verbForm(lemma_.verbTable, item.b);
        snprintf(text_, sizeof text_, "%s %s%s", personLabel(person),
                 tense == pk::Tense::NegativeImperative ? "no " : "", form);
      }
      break;
    }
    case ItemKind::WordOrder: {
      hasSentence_ = pack.sentence(item.a, sentence_);
      promptAndLabel(tr(Str::PromptOrder), nullptr, label_, sizeof label_);
      // The words, shuffled, lower-cased only as written; " / " between them.
      uint8_t order[32];
      const uint8_t n = sentence_.tokenCount < sizeof order ? sentence_.tokenCount : sizeof order;
      for (uint8_t i = 0; i < n; ++i) order[i] = i;
      uint32_t state = item.uid * 2654435761u + 1;
      for (uint8_t i = n; i > 1; --i) {
        const uint8_t j = static_cast<uint8_t>(nextRandom(state) % i);
        const uint8_t swap = order[i - 1];
        order[i - 1] = order[j];
        order[j] = swap;
      }
      size_t used = 0;
      for (uint8_t i = 0; i < n && used + 4 < sizeof text_; ++i) {
        pk::Token token;
        if (!pack.token(sentence_, order[i], token)) continue;
        const pk::TextSpan word = pack.tokenText(sentence_, token);
        const int w = snprintf(text_ + used, sizeof text_ - used, "%s%.*s", used ? " / " : "",
                               static_cast<int>(word.length), word.text);
        if (w < 0) break;
        used += static_cast<size_t>(w) < sizeof text_ - used ? static_cast<size_t>(w) : sizeof text_ - used - 1;
      }
      break;
    }
    case ItemKind::Phrase: {
      hasSentence_ = pack.sentence(item.a, sentence_);
      promptAndLabel(tr(Str::PromptPhrase), nullptr, label_, sizeof label_);
      pk::PhraseEntry entry;
      if (pack.phraseEntry(item.b, entry)) snprintf(text_, sizeof text_, "%s", pack.str(entry.pron));
      break;
    }
    default:
      // A vocabulary item no other view took: show it as a plain card.
      hasLemma_ = pack.lemma(item.a, lemma_);
      if (hasLemma_) {
        formatLemmaLabel(lemma_, label_, sizeof label_);
        formatHeadword(pack, lemma_, true, text_, sizeof text_);
      }
      break;
  }
  return true;
}

void RevealCardView::draw(app::UiScreen& screen, const Rect area, const bool back) {
  freeink::ui::DisplayTarget& t = displayOf(screen);
  Column col = columnFor(area);
  drawText(t, col, label_, FontRole::Label, card_.size, 1, kGap);
  switch (card_.item.kind) {
    case ItemKind::Cloze:
      drawCloze(t, col, back);
      break;
    case ItemKind::Gender:
      drawGender(t, col, back);
      break;
    case ItemKind::Conjugation:
      drawConjugation(t, col, back);
      break;
    case ItemKind::WordOrder:
      drawWordOrder(t, col, back);
      break;
    case ItemKind::Phrase:
      drawPhrase(t, col, back);
      break;
    default:
      drawHeadword(t, col, text_, 0);
      if (back && hasLemma_) {
        drawRule(t, col, kGap);
        drawText(t, col, card_.pack->str(lemma_.en), FontRole::EnglishGloss, card_.size, 3, 0);
      }
      break;
  }
}

void RevealCardView::drawCloze(freeink::ui::DisplayTarget& t, Column& col, const bool back) {
  if (!hasSentence_) return;
  const pk::Pack& pack = *card_.pack;
  // The gap is laid out with the missing word's own width, so the back
  // fills it in without moving anything.
  drawSentence(t, col, pack, sentence_, pk::kNone16, static_cast<int16_t>(card_.item.b), !back, card_.size, kTight);
  drawText(t, col, pack.str(sentence_.en), FontRole::EnglishTranslation, card_.size, 3, kGap);
  if (!back || !hasLemma_) return;
  drawRule(t, col, kGap);
  drawText(t, col, text_, FontRole::EnglishGloss, card_.size, 2, kTight);
  drawText(t, col, pack.str(lemma_.pron), FontRole::Respelling, card_.size, 1, 0);
}

void RevealCardView::drawGender(freeink::ui::DisplayTarget& t, Column& col, const bool back) {
  if (!hasLemma_) return;
  const pk::Pack& pack = *card_.pack;
  drawHeadword(t, col, pack.str(lemma_.es), 0);
  drawText(t, col, pack.str(lemma_.en), FontRole::EnglishGloss, card_.size, 2, kGap);
  if (!back) return;
  drawRule(t, col, kGap);
  drawText(t, col, text_, FontRole::SpanishEmphasis, card_.size, 2, kTight);
  drawText(t, col, pack.str(lemma_.note), FontRole::EnglishTranslation, card_.size, 0, 0);
}

void RevealCardView::drawConjugation(freeink::ui::DisplayTarget& t, Column& col, const bool back) {
  if (!hasLemma_) return;
  const pk::Pack& pack = *card_.pack;
  drawHeadword(t, col, pack.str(lemma_.es), 0);
  drawText(t, col, pack.str(lemma_.en), FontRole::EnglishGloss, card_.size, 2, kGap);
  drawText(t, col, personLabel(pk::tagPerson(card_.item.b)), FontRole::SpanishAside, card_.size, 1, kGap);
  if (!back) return;
  drawRule(t, col, kGap);
  drawText(t, col, text_, FontRole::SpanishEmphasis, card_.size, 2, 0);
}

void RevealCardView::drawWordOrder(freeink::ui::DisplayTarget& t, Column& col, const bool back) {
  if (!hasSentence_) return;
  const pk::Pack& pack = *card_.pack;
  drawText(t, col, pack.str(sentence_.en), FontRole::EnglishGloss, card_.size, 3, kGap);
  drawText(t, col, text_, FontRole::SpanishText, card_.size, 4, kGap);
  if (!back) return;
  drawRule(t, col, kGap);
  drawSentence(t, col, pack, sentence_, pk::kNone16, -1, false, card_.size, 0);
}

void RevealCardView::drawPhrase(freeink::ui::DisplayTarget& t, Column& col, const bool back) {
  if (!hasSentence_) return;
  const pk::Pack& pack = *card_.pack;
  drawText(t, col, pack.str(sentence_.en), FontRole::EnglishGloss, card_.size, 3, kGap);
  if (!back) return;
  drawRule(t, col, kGap);
  drawSentence(t, col, pack, sentence_, pk::kNone16, -1, false, card_.size, kTight);
  drawText(t, col, text_, FontRole::Respelling, card_.size, 2, 0);
}

}  // namespace tinta::ui
