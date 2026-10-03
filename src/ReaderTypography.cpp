#include "ReaderTypography.h"

#include <GfxRenderer.h>

#include <iterator>

#include "ReaderFontSizes.h"

namespace ReaderTypography {

namespace {

// [face][BUILTIN_READER_POINT_SIZES index][Tight, Normal, Wide, Extra wide]. The designed Wide pitch runs from
// about 1.35 em at 8pt, whose line is long, to 1.18 em at 16pt, whose line is short; Helvetica's taller x-height
// takes about 0.04 em more. The other steps sit 0.07 em apart below it and 0.14 em above, and Tight never closes
// below the strike's own ascent plus descent.
constexpr uint8_t LINE_PITCH[2][std::size(BUILTIN_READER_POINT_SIZES)][4] = {
    {{21, 22, 23, 25}, {23, 25, 26, 29}, {27, 29, 31, 34}, {31, 33, 35, 39}, {35, 38, 40, 45}},  // Times
    {{21, 22, 24, 26}, {24, 25, 27, 30}, {28, 30, 32, 35}, {32, 34, 36, 40}, {37, 39, 41, 46}},  // Helvetica
};

// English letter frequencies a..z, per mille.
constexpr uint8_t LETTER_PERMILLE[26] = {82, 15, 28, 43, 127, 22, 20, 61, 70, 2,  8, 40, 24,
                                         67, 75, 19, 1,  60,  63, 91, 28, 10, 24, 2, 20, 1};
// Running text averages 4.7 letters per word: 47 letters to every 10 spaces.
constexpr int LETTERS_PER_TEN_SPACES = 47;

}  // namespace

int builtinLinePitch(const bool sans, const uint8_t pointSize, const uint8_t lineSpacing) {
  for (size_t i = 0; i < std::size(BUILTIN_READER_POINT_SIZES); ++i) {
    if (BUILTIN_READER_POINT_SIZES[i] == pointSize) return LINE_PITCH[sans ? 1 : 0][i][lineSpacing & 3];
  }
  return 0;
}

int averageCharAdvanceFP(const GfxRenderer& renderer, const int fontId) {
  int permilleTotal = 0;
  int weightedLetters = 0;  // px * per mille
  char letter[2] = {'a', '\0'};
  for (int i = 0; i < 26; ++i) {
    letter[0] = static_cast<char>('a' + i);
    weightedLetters += LETTER_PERMILLE[i] * renderer.getTextAdvanceX(fontId, letter, EpdFontFamily::REGULAR);
    permilleTotal += LETTER_PERMILLE[i];
  }
  const int space = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
  // (47 average letters + 10 spaces) / 57 characters, in 1/16 px.
  const int64_t numerator =
      16LL * (static_cast<int64_t>(LETTERS_PER_TEN_SPACES) * weightedLetters + 10LL * space * permilleTotal);
  const int64_t denominator = static_cast<int64_t>(LETTERS_PER_TEN_SPACES + 10) * permilleTotal;
  return static_cast<int>(numerator / denominator);
}

int measureInset(const GfxRenderer& renderer, const int fontId, const int availableWidth) {
  const int advanceFP = averageCharAdvanceFP(renderer, fontId);
  if (advanceFP <= 0) return 0;
  const int maxMeasure = MAX_MEASURE_CHARS * advanceFP / 16;
  return availableWidth > maxMeasure ? (availableWidth - maxMeasure) / 2 : 0;
}

bool measureJustifies(const GfxRenderer& renderer, const int fontId, const int width) {
  const int advanceFP = averageCharAdvanceFP(renderer, fontId);
  return advanceFP <= 0 || width * 16 >= MIN_JUSTIFIED_CHARS * advanceFP;
}

}  // namespace ReaderTypography
