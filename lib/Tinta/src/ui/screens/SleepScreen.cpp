#include "ui/screens/SleepScreen.h"

#include <stdio.h>
#include <string.h>

#include "ui/Fonts.h"
#include "ui/Strings.h"
#include "ui/views/CardText.h"

namespace tinta::ui {
namespace {

using freeink::ui::Color;
using freeink::ui::Paint;
using freeink::ui::Rect;
using freeink::ui::TextAlign;
using freeink::ui::TextStyle;

int16_t i16(int32_t v) { return static_cast<int16_t>(v); }

// The word, from `top` down: what it is, the headword, respelling, gloss, an
// example with the word in bold and its English, then the streak and
// tomorrow's count.
void drawWord(freeink::ui::DisplayTarget& target, const Theme& theme, const SleepInfo& info, int16_t top,
              int16_t bottom) {
  const core::pack::Pack& pack = *info.pack;
  core::pack::Lemma lemma;
  if (!pack.lemma(info.lemma, lemma)) return;
  Column col;
  col.x = i16(2 * theme.margin);
  col.y = top;
  col.width = i16(target.logicalWidth() - 4 * theme.margin);
  col.bottom = bottom;
  const TextSize size = TextSize::Medium;
  drawTextIn(target, col, tr(info.learnt ? Str::SleepRemember : Str::SleepNewWord), font(FontRole::ChromeSmall), 1, 8);
  char text[96];
  formatLemmaLabel(lemma, text, sizeof text);
  drawText(target, col, text, FontRole::Label, size, 1, 4);
  formatHeadword(pack, lemma, true, text, sizeof text);
  drawHeadword(target, col, text, 0);
  drawText(target, col, pack.str(lemma.pron), FontRole::Respelling, size, 1, 10);
  drawRule(target, col, 10);
  drawText(target, col, pack.str(lemma.en), FontRole::EnglishGloss, size, 3, 12);
  core::pack::Sentence example;
  if (pack.sentence(pack.lemmaExample(lemma, 0), example) &&
      drawSentence(target, col, pack, example, info.lemma, -1, false, size, 4)) {
    drawText(target, col, pack.str(example.en), FontRole::EnglishTranslation, size, 3, 16);
  }
  if (info.countsKnown) {
    // The streak once there is one, and tomorrow's reviews.
    char streak[48] = {};
    if (info.streak == 1) {
      snprintf(streak, sizeof streak, "%s · ", tr(Str::StreakOne));
    } else if (info.streak > 1) {
      snprintf(streak, sizeof streak, tr(Str::StreakFmt), info.streak);
      strncat(streak, " · ", sizeof streak - strlen(streak) - 1);
    }
    char due[48];
    snprintf(due, sizeof due, tr(Str::SleepTomorrowFmt), static_cast<unsigned long>(info.dueTomorrow));
    snprintf(text, sizeof text, "%s%s", streak, due);
    drawTextIn(target, col, text, font(FontRole::ChromeBody), 2, 0);
  }
}

}  // namespace

void drawSleepScreen(freeink::ui::DisplayTarget& target, const Theme& theme, const SleepInfo& info) {
  const int16_t width = target.logicalWidth();
  const int16_t height = target.logicalHeight();
  target.fill(Rect{0, 0, width, height}, Paint::solid(Color::White));

  // The date (or the name), small, over the word.
  TextStyle head;
  head.font = kSlotBodyBold;
  head.align = TextAlign::Center;
  char line[64];
  if (info.dayKnown) {
    formatDate(info.day, DateStyle::Long, line, sizeof line);
  } else {
    snprintf(line, sizeof line, "%s", tr(Str::AppName));
  }
  const int16_t headY = i16(2 * theme.gap + 8);
  target.text(Rect{0, headY, width, target.lineHeight(kSlotBodyBold)}, line, head);
  const int16_t ruleY = i16(headY + target.lineHeight(kSlotBodyBold) + 10);
  target.fill(Rect{i16(width / 2 - 40), ruleY, 80, 2}, Paint::solid(Color::Black));
  // Clear of lila's moon at the bottom edge.
  drawWord(target, theme, info, i16(ruleY + 24), i16(height - 2 * theme.statusHeight));
}

}  // namespace tinta::ui
