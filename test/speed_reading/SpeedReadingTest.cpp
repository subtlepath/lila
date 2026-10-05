// Host tests for speed reading: punctuation pauses, the recognition point, word timing, how words join into
// one flash, the chapter word scanner, and the agreement that matters most -- a word's offset must land on the
// page the layout parser puts it on, since that is where Back returns the reader.

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <SpeedReadingText.h>
#include <SpineWordScanner.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#define class struct
#define private public
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class

namespace {

using speedread::Pause;
using speedread::Word;

Word makeWord(const char* text, const uint8_t flags = 0) {
  Word word{};
  word.length = static_cast<uint8_t>(strlen(text));
  memcpy(word.text, text, word.length);
  word.flags = flags;
  return word;
}

Pause pauseOf(const char* text) { return speedread::trailingPause(text, strlen(text)); }

std::string pivotLetter(const char* text) {
  const auto pivot = speedread::pivotOf(text, strlen(text));
  return std::string(text + pivot.start, pivot.length);
}

TEST(SpeedReadingText, PunctuationPauses) {
  EXPECT_EQ(pauseOf("word"), Pause::None);
  EXPECT_EQ(pauseOf("end."), Pause::Sentence);
  EXPECT_EQ(pauseOf("what?!"), Pause::Sentence);
  EXPECT_EQ(pauseOf("so\xE2\x80\xA6"), Pause::Sentence);
  EXPECT_EQ(pauseOf("said.\xE2\x80\x9D"), Pause::Sentence);
  EXPECT_EQ(pauseOf("(aside.)"), Pause::Sentence);
  EXPECT_EQ(pauseOf("wait,"), Pause::Clause);
  EXPECT_EQ(pauseOf("first;"), Pause::Clause);
  EXPECT_EQ(pauseOf("this\xE2\x80\x94"), Pause::Clause);
  EXPECT_EQ(pauseOf("(aside)"), Pause::None);
}

TEST(SpeedReadingText, AbbreviationsAndInitialsDoNotEndSentences) {
  EXPECT_EQ(pauseOf("Mr."), Pause::None);
  EXPECT_EQ(pauseOf("e.g."), Pause::None);
  EXPECT_EQ(pauseOf("J."), Pause::None);
  EXPECT_EQ(pauseOf("Mr.\xE2\x80\x9D"), Pause::Sentence);
  EXPECT_EQ(pauseOf("Mrs"), Pause::None);
}

TEST(SpeedReadingText, ContentIsMoreThanPunctuation) {
  EXPECT_TRUE(speedread::hasContent("a", 1));
  EXPECT_TRUE(speedread::hasContent("&", 1));
  EXPECT_FALSE(speedread::hasContent("\xE2\x80\x94", 3));
  EXPECT_FALSE(speedread::hasContent("* * *", 5));
}

TEST(SpeedReadingText, RecognitionPointMovesRightWithLength) {
  EXPECT_EQ(pivotLetter("a"), "a");
  EXPECT_EQ(pivotLetter("word"), "o");
  EXPECT_EQ(pivotLetter("reading"), "a");
  EXPECT_EQ(pivotLetter("comprehension"), "p");
  EXPECT_EQ(pivotLetter("incomprehensibility"), "m");
}

TEST(SpeedReadingText, RecognitionPointSkipsPunctuationAndKeepsMarks) {
  EXPECT_EQ(pivotLetter("\xE2\x80\x9CHello,"), "e");
  EXPECT_EQ(pivotLetter("na\xC3\xAFve"), "a");
  EXPECT_EQ(pivotLetter("\xC3\xAF"
                        "a"),
            "a");
  // A decomposed accent travels with its letter.
  EXPECT_EQ(pivotLetter("e\xCC\x81"), "e\xCC\x81");
  EXPECT_EQ(pivotLetter("e\xCC\x81tude"), "t");
  EXPECT_EQ(pivotLetter("\xE2\x80\x94"), "\xE2\x80\x94");
}

TEST(SpeedReadingText, DwellFollowsPaceLengthAndPunctuation) {
  EXPECT_EQ(speedread::wordDwellMs(makeWord("the"), 300), 200u);
  EXPECT_EQ(speedread::wordDwellMs(makeWord("the"), 600), 100u);
  // Ten letters: 4 past six, 5% each.
  EXPECT_EQ(speedread::wordDwellMs(makeWord("categories"), 300), 240u);
  EXPECT_EQ(speedread::wordDwellMs(makeWord("then,"), 300), 300u);
  EXPECT_EQ(speedread::wordDwellMs(makeWord("then."), 300), 400u);
  EXPECT_EQ(speedread::wordDwellMs(makeWord("then", speedread::PARAGRAPH_END), 300), 500u);
  EXPECT_EQ(speedread::wordDwellMs(makeWord("then.", speedread::PARAGRAPH_END | speedread::CHAPTER_END), 300), 600u);
}

struct Phrase {
  std::vector<Word> words;
  std::vector<const Word*> pointers;
  explicit Phrase(std::initializer_list<Word> list) : words(list) {
    for (const auto& word : words) pointers.push_back(&word);
  }
};

size_t plan(const Phrase& phrase, const uint16_t wpm, const uint32_t minFlashMs, uint32_t& dwell,
            bool (*fits)(void*, size_t) = nullptr, void* context = nullptr) {
  const speedread::ChunkRules rules{wpm, minFlashMs, 4};
  return speedread::planChunk(phrase.pointers.data(), phrase.pointers.size(), rules, fits, context, dwell);
}

TEST(SpeedReadingChunk, WordsJoinUntilTheyCoverThePanelsRefresh) {
  const Phrase phrase{makeWord("the"), makeWord("quick"), makeWord("brown"), makeWord("fox")};
  uint32_t dwell = 0;
  EXPECT_EQ(plan(phrase, 300, 450, dwell), 3u);
  EXPECT_EQ(dwell, 600u);
  EXPECT_EQ(plan(phrase, 300, 0, dwell), 1u);
  EXPECT_EQ(dwell, 200u);
  EXPECT_EQ(plan(phrase, 120, 450, dwell), 1u);
  EXPECT_EQ(plan(phrase, 800, 5000, dwell), 4u);
}

TEST(SpeedReadingChunk, PhrasesEndAtPunctuationAndParagraphs) {
  uint32_t dwell = 0;
  const Phrase clause{makeWord("Hello,"), makeWord("world")};
  EXPECT_EQ(plan(clause, 300, 1000, dwell), 1u);
  EXPECT_EQ(dwell, 300u);
  const Phrase sentence{makeWord("Stop."), makeWord("Go")};
  EXPECT_EQ(plan(sentence, 300, 1000, dwell), 1u);
  const Phrase paragraph{makeWord("end", speedread::PARAGRAPH_END), makeWord("Next")};
  EXPECT_EQ(plan(paragraph, 300, 1000, dwell), 1u);
  const Phrase piece{makeWord("Donaudampfschifffahrtsgesellsc", speedread::CONTINUES), makeWord("haft")};
  EXPECT_EQ(plan(piece, 300, 1000, dwell), 1u);
}

TEST(SpeedReadingChunk, PhrasesFitTheLine) {
  const Phrase phrase{makeWord("one"), makeWord("two"), makeWord("three"), makeWord("four")};
  uint32_t dwell = 0;
  auto twoFit = [](void*, const size_t words) { return words <= 2; };
  EXPECT_EQ(plan(phrase, 300, 5000, dwell, twoFit), 2u);
  auto noneFit = [](void*, size_t) { return false; };
  EXPECT_EQ(plan(phrase, 300, 5000, dwell, noneFit), 1u);
}

// --- Scanner -------------------------------------------------------------------------------------------------

struct Scanned {
  std::string text;
  uint32_t offset;
  uint32_t end;
  uint8_t flags;
};

class Collector final : public speedread::WordSink {
 public:
  std::vector<Scanned> words;
  void onWord(const Word& word, const uint32_t end) override {
    EXPECT_EQ(strlen(word.text), word.length);
    words.push_back({word.text, word.offset, end, word.flags});
  }
};

std::string doc(const std::string& body) {
  // XHTML's external DTD is what lets expat hand named HTML entities (&nbsp;) to the entity table.
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" "
         "\"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">\n<html xmlns=\"http://www.w3.org/1999/xhtml\" "
         "xmlns:epub=\"http://www.idpf.org/2007/ops\"><head><title>Ignored title</title>"
         "<style>p { margin: 0 }</style></head><body>" +
         body + "</body></html>";
}

// Feeds in small, odd-sized chunks so tokens and UTF-8 sequences straddle buffer edges.
std::vector<Scanned> scan(const std::string& html, const size_t chunk = 7, uint32_t* visibleLength = nullptr) {
  Collector collector;
  speedread::SpineWordScanner scanner(collector);
  EXPECT_TRUE(scanner.begin(3, static_cast<uint32_t>(html.size())));
  for (size_t at = 0; at < html.size(); at += chunk) {
    const size_t n = std::min(chunk, html.size() - at);
    EXPECT_EQ(scanner.write(reinterpret_cast<const uint8_t*>(html.data() + at), n), n);
  }
  EXPECT_TRUE(scanner.finish());
  if (visibleLength) *visibleLength = scanner.visibleTextLength();
  return collector.words;
}

std::vector<std::string> texts(const std::vector<Scanned>& words) {
  std::vector<std::string> out;
  for (const auto& word : words) out.push_back(word.text);
  return out;
}

using Texts = std::vector<std::string>;

TEST(SpeedReadingScanner, WordsCarryTheirOffsetsAndParagraphs) {
  const auto words = scan(doc("<p>Hello world.</p><p>Second para</p>"));
  ASSERT_EQ(texts(words), (Texts{"Hello", "world.", "Second", "para"}));
  EXPECT_EQ(words[0].offset, 0u);
  EXPECT_EQ(words[0].end, 5u);
  EXPECT_EQ(words[1].offset, 6u);
  EXPECT_EQ(words[2].offset, 12u);
  EXPECT_EQ(words[0].flags, 0);
  EXPECT_EQ(words[1].flags, speedread::PARAGRAPH_END);
  EXPECT_EQ(words[3].flags, speedread::PARAGRAPH_END | speedread::CHAPTER_END);
}

TEST(SpeedReadingScanner, ParagraphEndsSurviveTrailingWhitespace) {
  const auto words = scan(doc("<p>One two.  \r\n </p>\r\n<p> Three</p>"));
  ASSERT_EQ(texts(words), (Texts{"One", "two.", "Three"}));
  EXPECT_EQ(words[1].flags, speedread::PARAGRAPH_END);
}

TEST(SpeedReadingScanner, InlineFormattingJoinsAndBlocksBreak) {
  EXPECT_EQ(texts(scan(doc("<p>un<em>believ</em>able</p>"))), (Texts{"unbelievable"}));
  const auto words = scan(doc("<div>end<br/>start</div>"));
  ASSERT_EQ(texts(words), (Texts{"end", "start"}));
  EXPECT_EQ(words[0].flags, speedread::PARAGRAPH_END);
}

TEST(SpeedReadingScanner, HiddenTextIsSkippedButCounted) {
  const auto words =
      scan(doc("<p>word<a epub:type=\"noteref\" href=\"#n\">1</a>, next "
               "<span style=\"display:none\">secret</span>shown</p>"));
  ASSERT_EQ(texts(words), (Texts{"word,", "next", "shown"}));
  EXPECT_EQ(words[1].offset, 7u);
  EXPECT_EQ(words[2].offset, 18u);
}

TEST(SpeedReadingScanner, SuperscriptLinksAreNoteMarkers) {
  const auto words = scan(doc("<p>primordial wisdom<sup><a href=\"end.xhtml#n14\" id=\"n14a\">14</a></sup> and "
                              "beautiful.<a href=\"#n16\"><sup>16</sup></a> It is</p>"));
  ASSERT_EQ(texts(words), (Texts{"primordial", "wisdom", "and", "beautiful.", "It", "is"}));
  EXPECT_EQ(speedread::trailingPause(words[3].text.c_str(), words[3].text.size()), speedread::Pause::Sentence);
  EXPECT_EQ(words[2].offset, 20u);
  // A superscript alone, a link alone and a link out of the book are words.
  EXPECT_EQ(texts(scan(doc("<p>10<sup>59</sup> kalpas, the 1<sup>st</sup></p>"))),
            (Texts{"1059", "kalpas,", "the", "1st"}));
  EXPECT_EQ(texts(scan(doc("<p>see <a href=\"#c2\">chapter 2</a> now</p>"))), (Texts{"see", "chapter", "2", "now"}));
  EXPECT_EQ(texts(scan(doc("<p>x<sup><a href=\"https://example.org\">web</a></sup></p>"))), (Texts{"xweb"}));
}

TEST(SpeedReadingScanner, EntitiesAndNoBreakSpaces) {
  EXPECT_EQ(texts(scan(doc("<p>Tom &amp; Jerry</p>"))), (Texts{"Tom", "&", "Jerry"}));
  EXPECT_EQ(texts(scan(doc("<p>10&nbsp;km away</p>"))), (Texts{"10 km", "away"}));
  EXPECT_EQ(texts(scan(doc("<p>&nbsp;</p><p>&nbsp;&nbsp;Text</p>"))), (Texts{"Text"}));
  // A soft hyphen is not drawn but is counted.
  const auto words = scan(doc("<p>hy&#173;phen next</p>"));
  ASSERT_EQ(texts(words), (Texts{"hyphen", "next"}));
  EXPECT_EQ(words[1].offset, 8u);
}

TEST(SpeedReadingScanner, DashesAndLonePunctuation) {
  EXPECT_EQ(texts(scan(doc("<p>this\xE2\x80\x94that</p>"))), (Texts{"this\xE2\x80\x94", "that"}));
  EXPECT_EQ(texts(scan(doc("<p>word \xE2\x80\x94 next</p>"))), (Texts{"word \xE2\x80\x94", "next"}));
  EXPECT_EQ(texts(scan(doc("<p>\xE2\x80\x94Hello, she said</p>"))), (Texts{"\xE2\x80\x94Hello,", "she", "said"}));
  // Punctuation opening a paragraph leads its first word; a scene break stays a word of its own.
  EXPECT_EQ(texts(scan(doc("<p>End.</p><p>\xE2\x80\x9C Yes</p>"))), (Texts{"End.", "\xE2\x80\x9C Yes"}));
  const auto words = scan(doc("<p>End.</p><p>* * *</p><p>Begin</p>"));
  ASSERT_EQ(texts(words), (Texts{"End.", "* * *", "Begin"}));
  EXPECT_EQ(words[1].flags, speedread::PARAGRAPH_END);
}

TEST(SpeedReadingScanner, LongWordsArriveInPieces) {
  const std::string longWord(45, 'x');
  const auto words = scan(doc("<p>" + longWord + " end</p>"));
  ASSERT_EQ(words.size(), 3u);
  EXPECT_EQ(words[0].text.size(), Word::TEXT_BYTES - 1);
  EXPECT_EQ(words[0].flags, speedread::CONTINUES);
  EXPECT_EQ(words[1].text.size(), 45 - (Word::TEXT_BYTES - 1));
  EXPECT_EQ(words[1].offset, Word::TEXT_BYTES - 1);
  EXPECT_EQ(words[2].text, "end");
}

TEST(SpeedReadingScanner, ChunkSizeDoesNotChangeTheWords) {
  const std::string html =
      doc("<h1>Title</h1><p>Caf\xC3\xA9 &amp; <i>cr\xC3\xA8me</i>\r\nbr\xC3\xBBl\xC3\xA9"
          "e\xE2\x80\x94"
          "fine.</p>"
          "<p>Nirva\xCC\x84n\xCC\xA3"
          "a, again.</p>");
  const auto whole = scan(html, html.size());
  const auto pieces = scan(html, 3);
  ASSERT_EQ(texts(whole), texts(pieces));
  for (size_t i = 0; i < whole.size(); i++) {
    EXPECT_EQ(whole[i].offset, pieces[i].offset) << whole[i].text;
    EXPECT_EQ(whole[i].flags, pieces[i].flags) << whole[i].text;
  }
}

TEST(SpeedReadingScanner, KeepsWordsBeforeAParseError) {
  Collector collector;
  speedread::SpineWordScanner scanner(collector);
  const std::string html = "<html><body><p>Good words</p><p>then <b>broken</i></p></body></html>";
  ASSERT_TRUE(scanner.begin(0, static_cast<uint32_t>(html.size())));
  scanner.write(reinterpret_cast<const uint8_t*>(html.data()), html.size());
  EXPECT_FALSE(scanner.finish());
  ASSERT_GE(collector.words.size(), 2u);
  EXPECT_EQ(collector.words[0].text, "Good");
  EXPECT_EQ(collector.words[1].text, "words");
}

// --- Agreement with the layout parser ------------------------------------------------------------------------

struct LaidOutPage {
  uint32_t start;
  std::string text;  // the page's words run together
};

// Lays the chapter out with the firmware's parser on a small page, so it spans many pages.
std::vector<LaidOutPage> layOut(const std::string& html, uint32_t& visibleTotal) {
  const auto path = std::filesystem::temp_directory_path() / "crosspoint-speed-reading.xhtml";
  {
    std::ofstream out(path, std::ios::binary);
    out << html;
  }
  std::vector<LaidOutPage> pages;
  GfxRenderer renderer;
  CssParser cssParser{"/tmp"};
  const std::string filepath = path.string();
  ChapterHtmlSlimParser parser{nullptr,
                               filepath,
                               renderer,
                               0,
                               1.0f,
                               false,
                               0,
                               200,
                               64,
                               false,
                               false,
                               [&](std::unique_ptr<Page> page, uint16_t, uint16_t, const uint32_t offset) {
                                 LaidOutPage laidOut{offset, {}};
                                 for (const auto& element : page->elements) {
                                   if (element->getTag() != TAG_PageLine) continue;
                                   const auto& block = *static_cast<const PageLine&>(*element).getBlock();
                                   for (uint16_t w = 0; w < block.wordCount(); w++) laidOut.text += block.wordText(w);
                                 }
                                 pages.push_back(std::move(laidOut));
                               },
                               true,
                               "",
                               "",
                               0,
                               {},
                               nullptr,
                               &cssParser};
  EXPECT_TRUE(parser.parseAndBuildPages());
  visibleTotal = parser.visibleTextOffset;
  std::filesystem::remove(path);
  return pages;
}

std::string longChapter() {
  // Formatting, entities, comments, hidden text, verse breaks, dashes and CRLF line ends, repeated with
  // variations so words fall on many pages and at page edges.
  std::string body;
  const char* filler[] = {"the",     "categories",  "of",          "phenomena", "that",
                          "are",     "deluded",     "appearances", "we",        "now",
                          "present", "established", "positions",   "criteria",  "assessed"};
  for (int p = 0; p < 60; p++) {
    body += "<p class=\"x\">\r\n";
    for (int w = 0; w < 9 + p % 7; w++) {
      body += filler[(p * 7 + w * 3) % 15];
      body += (w % 4 == 3) ? ",\r\n  " : " ";
    }
    switch (p % 6) {
      case 0:
        body += "<span class=\"run-in\">Nirv&#x101;&#x1E47;a</span> is <em>beyond</em> samsara";
        break;
      case 1:
        body += "the self-<i>arising</i> awareness<a epub:type=\"noteref\" href=\"#n\">3</a> shines";
        break;
      case 2:
        body += "Tom &amp; Jerry<!-- a comment --> don&#x2019;t <span style=\"display:none\">hidden</span>argue";
        break;
      case 3:
        body += "<em>Kye ho! O secret vajra-holder,<br/>my vehicles are beyond imagining</em>";
        break;
      case 4:
        body += "this&#8212;that and &#8212; the other";
        break;
      default:
        body += "plain words before&nbsp;the samsara end, caf&eacute;\r\nau lait";
        break;
    }
    body += ".</p>\r\n";
    if (p % 10 == 9) body += "<h3 id=\"s" + std::to_string(p) + "\">Section &#8212; " + std::to_string(p) + "</h3>";
  }
  return doc(body);
}

// The first run of letters in a word, the part the layout keeps in one piece.
std::string letters(const std::string& word) {
  size_t start = 0;
  while (start < word.size() && strchr("\"'(*-.,;:!? ", word[start])) start++;
  size_t end = start;
  while (end < word.size() && !strchr("\"'()*-.,;:!? ", word[end]) && static_cast<uint8_t>(word[end]) != 0xE2) end++;
  return word.substr(start, end - start);
}

TEST(SpeedReadingLayout, EveryWordOpensOnThePageThatShowsIt) {
  const std::string html = longChapter();
  uint32_t visibleTotal = 0;
  const auto pages = layOut(html, visibleTotal);
  ASSERT_GT(pages.size(), 30u);

  uint32_t scannedLength = 0;
  const auto words = scan(html, 11, &scannedLength);
  // The scanner counts the same text the parser counts.
  EXPECT_EQ(scannedLength, visibleTotal);
  ASSERT_GT(words.size(), 500u);

  for (size_t i = 0; i < words.size(); i++) {
    const auto& word = words[i];
    if (i > 0) EXPECT_GT(word.offset, words[i - 1].offset);
    size_t page = 0;
    while (page + 1 < pages.size() && pages[page + 1].start <= word.offset) page++;
    const std::string key = letters(word.text);
    if (key.empty()) continue;
    EXPECT_NE(pages[page].text.find(key), std::string::npos)
        << "\"" << word.text << "\" at " << word.offset << " not on page " << page;
  }
  // And every page starts on a word the scanner delivers.
  for (size_t page = 1; page < pages.size(); page++) {
    bool found = false;
    for (const auto& word : words) {
      if (word.offset <= pages[page].start && pages[page].start < word.end) {
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found) << "page " << page << " starts at " << pages[page].start;
  }
}

}  // namespace
