#include <Epub/ParsedText.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <random>
#include <string>
#include <vector>

// Fixture metrics (stub renderer): every glyph is 8 px wide and a space is 4 px, so a justified space may
// stretch by 2 px and shrink by 4/3 px.
namespace {

struct Line {
  std::vector<std::string> words;
  std::vector<int16_t> xpos;
};

int widthOf(const std::string& word) {
  GfxRenderer renderer;
  return renderer.getTextAdvanceX(0, word.c_str(), EpdFontFamily::REGULAR);
}

int rightEdge(const Line& line) { return line.xpos.back() + widthOf(line.words.back()); }

std::vector<int> gapsOf(const Line& line) {
  std::vector<int> gaps;
  for (size_t i = 1; i < line.words.size(); ++i) {
    gaps.push_back(line.xpos[i] - (line.xpos[i - 1] + widthOf(line.words[i - 1])));
  }
  return gaps;
}

BlockStyle styleFor(const CssTextAlign alignment) {
  BlockStyle style;
  style.alignment = alignment;
  style.textIndentDefined = true;
  return style;
}

void collectLines(ParsedText& text, const uint16_t width, std::vector<Line>& lines, const bool includeLastLine = true) {
  GfxRenderer renderer;
  text.layoutAndExtractLines(
      renderer, 0, width,
      [&](std::unique_ptr<TextBlock> block, auto) {
        auto& line = lines.emplace_back();
        for (uint16_t i = 0; i < block->wordCount(); ++i) {
          line.words.emplace_back(block->wordText(i));
          line.xpos.push_back(block->wordXpos(i));
        }
      },
      includeLastLine);
}

std::vector<Line> layout(const std::vector<std::string>& words, const bool hyphenation, const CssTextAlign alignment,
                         const uint16_t width) {
  ParsedText text(false, hyphenation, false, styleFor(alignment));
  for (const auto& word : words) text.addWord(word, EpdFontFamily::REGULAR);
  std::vector<Line> lines;
  collectLines(text, width, lines);
  return lines;
}

std::vector<std::vector<std::string>> wordsOf(const std::vector<Line>& lines) {
  std::vector<std::vector<std::string>> result;
  for (const auto& line : lines) result.push_back(line.words);
  return result;
}

std::vector<std::string> splitWords(const std::string& text) {
  std::vector<std::string> words;
  size_t start = 0;
  while (start < text.size()) {
    const size_t end = text.find(' ', start);
    words.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return words;
}

const char* const kProse =
    "However little known the feelings or views of such a man may be on his first entering a neighbourhood, this "
    "truth is so well fixed in the minds of the surrounding families, that he is considered the rightful property "
    "of some one or other of their daughters. Whenever I find myself growing grim about the mouth; whenever it is a "
    "damp, drizzly November in my soul; whenever I find myself involuntarily pausing before coffin warehouses, and "
    "bringing up the rear of every funeral I meet, then I account it high time to get to sea as soon as I can.";

// Replays the laid-out tokens against the source words. Every token must be the rest of the current word, or --
// as the last token of a line -- a piece of it followed by an inserted '-'. Returns the number of hyphenated breaks.
int expectTextPreserved(const std::vector<std::string>& source, const std::vector<Line>& lines) {
  size_t word = 0;
  size_t offset = 0;
  int insertedHyphens = 0;
  for (size_t li = 0; li < lines.size(); ++li) {
    for (size_t i = 0; i < lines[li].words.size(); ++i) {
      const std::string& token = lines[li].words[i];
      EXPECT_LT(word, source.size()) << "extra token " << token;
      if (word >= source.size()) return insertedHyphens;
      const std::string rest = source[word].substr(offset);
      if (token == rest) {
        ++word;
        offset = 0;
        continue;
      }
      const bool lineEnd = i + 1 == lines[li].words.size();
      if (lineEnd && rest.compare(0, token.size(), token) == 0) {
        offset += token.size();  // split after a visible hyphen or dash
        continue;
      }
      const bool inserted = lineEnd && token.size() > 1 && token.back() == '-' &&
                            rest.compare(0, token.size() - 1, token, 0, token.size() - 1) == 0;
      EXPECT_TRUE(inserted) << "token \"" << token << "\" does not continue \"" << rest << "\"";
      if (!inserted) return insertedHyphens;
      offset += token.size() - 1;
      ++insertedHyphens;
    }
  }
  EXPECT_EQ(word, source.size());
  EXPECT_EQ(offset, 0u);
  return insertedHyphens;
}

}  // namespace

TEST(LineBreaking, TotalFitMovesAWordDownToSpareALooseLine) {
  // First-fit fills line 1 exactly and leaves line 2 ("g hhhhhh iiiiiii" minus "g") with one 16 px gap. Total fit
  // carries "g" down: line 1 gets five 6-7 px gaps and line 2 fills the measure exactly.
  const auto lines = layout({"aaa", "b", "c", "d", "eee", "ff", "g", "hhhhhh", "iiiiiii", "jjjjjj"}, false,
                            CssTextAlign::Justify, 120);
  const std::vector<std::vector<std::string>> expected{
      {"aaa", "b", "c", "d", "eee", "ff"}, {"g", "hhhhhh", "iiiiiii"}, {"jjjjjj"}};
  ASSERT_EQ(wordsOf(lines), expected);
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 30, 44, 59, 73, 104}));  // gaps 6, 6, 7, 6, 7 px
  EXPECT_EQ(lines[1].xpos, (std::vector<int16_t>{0, 12, 64}));
}

TEST(LineBreaking, SpacesShrinkRatherThanLeaveALooseLine) {
  // Six words need 116 px; first-fit stops at five with four 16 px gaps. Each 4 px space may give up 1 px, so the
  // sixth word fits with gaps of 4, 3, 3, 3, 3 px.
  const auto lines = layout({"a", "b", "c", "d", "ee", "ffffff", "g"}, false, CssTextAlign::Justify, 112);
  const std::vector<std::vector<std::string>> expected{{"a", "b", "c", "d", "ee", "ffffff"}, {"g"}};
  ASSERT_EQ(wordsOf(lines), expected);
  EXPECT_EQ(gapsOf(lines[0]), (std::vector<int>{4, 3, 3, 3, 3}));
  EXPECT_EQ(rightEdge(lines[0]), 112);
}

TEST(LineBreaking, JustifiedLinesEndOnTheMarginWithEvenGaps) {
  Hyphenator::setPreferredLanguage("en");
  const auto source = splitWords(kProse);
  for (const bool hyphenation : {false, true}) {
    for (const uint16_t width : {152, 200, 264, 344}) {
      SCOPED_TRACE(testing::Message() << "hyphenation=" << hyphenation << " width=" << width);
      const auto lines = layout(source, hyphenation, CssTextAlign::Justify, width);
      ASSERT_GT(lines.size(), 2u);
      for (size_t i = 0; i + 1 < lines.size(); ++i) {
        if (lines[i].words.size() < 2) continue;
        EXPECT_EQ(rightEdge(lines[i]), width) << "line " << i;
        const auto gaps = gapsOf(lines[i]);
        const auto [narrowest, widest] = std::minmax_element(gaps.begin(), gaps.end());
        EXPECT_LE(*widest - *narrowest, 1) << "line " << i;
        // Spaces never shrink below two thirds of their width.
        EXPECT_GE(*narrowest, 3) << "line " << i;
      }
      for (const int gap : gapsOf(lines.back())) EXPECT_EQ(gap, 4);
      expectTextPreserved(source, lines);
    }
  }
}

TEST(LineBreaking, RaggedTextKeepsNaturalSpacesAndFitsTheMeasure) {
  Hyphenator::setPreferredLanguage("en");
  const auto source = splitWords(kProse);
  for (const bool hyphenation : {false, true}) {
    for (const uint16_t width : {152, 264}) {
      SCOPED_TRACE(testing::Message() << "hyphenation=" << hyphenation << " width=" << width);
      const auto lines = layout(source, hyphenation, CssTextAlign::Left, width);
      for (const auto& line : lines) {
        EXPECT_EQ(line.xpos.front(), 0);
        EXPECT_LE(rightEdge(line), width);
        for (const int gap : gapsOf(line)) EXPECT_EQ(gap, 4);
      }
      expectTextPreserved(source, lines);
    }
  }
}

TEST(LineBreaking, HyphenatesWhereItSavesALooseLine) {
  Hyphenator::setPreferredLanguage("en");
  const auto breaks = Hyphenator::breakOffsets("information", false);
  ASSERT_FALSE(breaks.empty());
  const size_t split = breaks.front().byteOffset;
  ASSERT_TRUE(breaks.front().requiresInsertedHyphen);
  // "aaaa bbbb " plus the hyphenated prefix fills the line exactly; without the split line 1 would carry
  // ~40 px of slack in a single gap.
  const auto width = static_cast<uint16_t>(32 + 4 + 32 + 4 + 8 * (split + 1));
  const auto lines = layout({"aaaa", "bbbb", "information", "cc"}, true, CssTextAlign::Justify, width);
  const std::string prefix = std::string("information").substr(0, split) + "-";
  const std::string suffix = std::string("information").substr(split);
  const std::vector<std::vector<std::string>> expected{{"aaaa", "bbbb", prefix}, {suffix, "cc"}};
  ASSERT_EQ(wordsOf(lines), expected);
  EXPECT_EQ(rightEdge(lines[0]), width);
}

TEST(LineBreaking, WordsStayWholeWithHyphenationOff) {
  const auto lines = layout({"aaaa", "bbbb", "information", "cc"}, false, CssTextAlign::Justify, 120);
  for (const auto& line : lines) {
    for (const auto& word : line.words) EXPECT_NE(word.back(), '-');
  }
}

TEST(LineBreaking, VisibleHyphenIsABreakOpportunityWithHyphenationOff) {
  // "aaaa well-known" needs 116 px. Breaking after the existing hyphen beats leaving "aaaa" alone on a line, and
  // no hyphen is inserted.
  const auto lines = layout({"aaaa", "well-known", "bb"}, false, CssTextAlign::Justify, 96);
  const std::vector<std::vector<std::string>> expected{{"aaaa", "well-"}, {"known", "bb"}};
  ASSERT_EQ(wordsOf(lines), expected);
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 56}));
}

TEST(LineBreaking, NonBreakingHyphenNeverBreaks) {
  for (const bool hyphenation : {false, true}) {
    const auto lines = layout({"aaaa", "well‑known", "bb"}, hyphenation, CssTextAlign::Justify, 96);
    const std::vector<std::vector<std::string>> expected{{"aaaa"}, {"well‑known"}, {"bb"}};
    EXPECT_EQ(wordsOf(lines), expected) << "hyphenation=" << hyphenation;
  }
  for (const auto& info : Hyphenator::breakOffsets("anti‑war", false)) {
    EXPECT_NE(info.byteOffset, 7u);  // right after U+2011
  }
  EXPECT_TRUE(Hyphenator::visibleHyphenBreakOffsets("anti‑war").empty());
}

TEST(LineBreaking, VisibleHyphenBreaksExcludeSoftHyphens) {
  const auto hard = Hyphenator::visibleHyphenBreakOffsets("well-known");
  ASSERT_EQ(hard.size(), 1u);
  EXPECT_EQ(hard[0].byteOffset, 5u);
  EXPECT_FALSE(hard[0].requiresInsertedHyphen);
  EXPECT_TRUE(Hyphenator::visibleHyphenBreakOffsets("hy­phen").empty());
}

TEST(LineBreaking, CjkLinesStayFull) {
  std::string text;
  for (int i = 0; i < 50; ++i) text += "一";
  const auto lines = layout({text}, false, CssTextAlign::Justify, 100);
  // 12 ideographs (96 px) fit a 100 px line; the 4 px of slack is spread over the 11 break opportunities.
  std::vector<size_t> perLine;
  for (const auto& line : lines) perLine.push_back(line.words.size());
  EXPECT_EQ(perLine, (std::vector<size_t>{12, 12, 12, 12, 2}));
  for (size_t i = 0; i + 1 < lines.size(); ++i) EXPECT_EQ(rightEdge(lines[i]), 100);
}

TEST(LineBreaking, SoftFlushContinuationIsNotIndented) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;  // textIndent undefined: a three-space first-line indent
  ParsedText text(false, false, false, style);
  for (int i = 0; i < 12; ++i) text.addWord("aaaa", EpdFontFamily::REGULAR);
  std::vector<Line> lines;
  collectLines(text, 100, lines, /*includeLastLine=*/false);
  ASSERT_FALSE(lines.empty());
  EXPECT_EQ(lines.front().xpos.front(), 12);
  const size_t firstPass = lines.size();
  for (int i = 0; i < 12; ++i) text.addWord("bbbb", EpdFontFamily::REGULAR);
  collectLines(text, 100, lines);
  ASSERT_GT(lines.size(), firstPass);
  EXPECT_EQ(lines[firstPass].xpos.front(), 0);  // same paragraph, so no second indent
}

TEST(LineBreaking, TextAfterBrIsNotIndented) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;  // textIndent undefined: a paragraph would get a three-space indent
  style.fromBrElement = true;
  ParsedText text(false, false, false, style);
  for (int i = 0; i < 6; ++i) text.addWord("aaaa", EpdFontFamily::REGULAR);
  std::vector<Line> lines;
  collectLines(text, 100, lines);
  ASSERT_FALSE(lines.empty());
  EXPECT_EQ(lines.front().xpos.front(), 0);
}

TEST(LineBreaking, AtMostTwoHyphenatedLinesInARow) {
  Hyphenator::setPreferredLanguage("en");
  const auto vocabulary = splitWords(kProse);
  std::mt19937 rng(99);
  int hyphenated = 0;
  for (int trial = 0; trial < 200; ++trial) {
    std::vector<std::string> source(60 + rng() % 60);
    for (auto& word : source) word = vocabulary[rng() % vocabulary.size()];
    // The measures the reader sets each alignment at (ReaderTypography): ragged from 30 characters, justified
    // from 45. Narrower justified lines can need a third hyphen to avoid a far looser line.
    const bool ragged = trial % 2 == 0;
    const auto alignment = ragged ? CssTextAlign::Left : CssTextAlign::Justify;
    const auto width = static_cast<uint16_t>(8 * ((ragged ? 30 : 45) + rng() % 26));
    SCOPED_TRACE(testing::Message() << "trial=" << trial << " width=" << width);
    const auto lines = layout(source, true, alignment, width);
    int run = 0;
    for (const auto& line : lines) {
      const std::string& last = line.words.back();
      run = last.size() > 1 && last.back() == '-' ? run + 1 : 0;
      hyphenated += run > 0;
      EXPECT_LE(run, 2);
    }
  }
  EXPECT_GT(hyphenated, 0);
}

TEST(LineBreaking, TextSurvivesRandomParagraphs) {
  Hyphenator::setPreferredLanguage("en");
  const auto vocabulary = splitWords(kProse);
  std::mt19937 rng(1234);
  int hyphenated = 0;
  for (int trial = 0; trial < 200; ++trial) {
    std::vector<std::string> source(5 + rng() % 60);
    for (auto& word : source) word = vocabulary[rng() % vocabulary.size()];
    const bool hyphenation = trial % 2 == 0;
    const auto alignment = trial % 3 == 0 ? CssTextAlign::Left : CssTextAlign::Justify;
    const auto width = static_cast<uint16_t>(80 + rng() % 300);
    SCOPED_TRACE(testing::Message() << "trial=" << trial << " width=" << width);
    const auto lines = layout(source, hyphenation, alignment, width);
    hyphenated += expectTextPreserved(source, lines);
    for (const auto& line : lines) {
      if (line.words.size() > 1) EXPECT_LE(rightEdge(line), width);
    }
  }
  EXPECT_GT(hyphenated, 0);
}

TEST(LineBreaking, HugeTokenStreamsFallBackToFirstFit) {
  // Past the optimal breaker's 16-bit candidate indices, layout falls back to first-fit at word boundaries.
  ParsedText text(false, false, false, styleFor(CssTextAlign::Justify));
  constexpr size_t TOKENS = 70000;
  for (size_t i = 0; i < TOKENS; ++i) text.addWord("a", EpdFontFamily::REGULAR);
  std::vector<Line> lines;
  collectLines(text, 100, lines);
  size_t tokens = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    tokens += lines[i].words.size();
    if (i + 1 < lines.size()) {
      EXPECT_EQ(lines[i].words.size(), 8u);  // 8 x 8 px + 7 x 4 px = 92 px
      EXPECT_EQ(rightEdge(lines[i]), 100);
    }
  }
  EXPECT_EQ(tokens, TOKENS);
}
