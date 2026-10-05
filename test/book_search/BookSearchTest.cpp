// Host tests for in-book search: the fold, the phrase matcher, excerpts, the chapter scanner, and the
// agreement that matters most -- a match's offset must land on the page the layout parser puts it on.

#include <BookSearchResults.h>
#include <BookSearchText.h>
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <SpineTextScanner.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
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

using booksearch::AnchorTag;
using booksearch::Folded;
using booksearch::Hit;
using booksearch::SpineTextScanner;

struct Found {
  uint32_t start;
  uint32_t end;
  int16_t tag;
  std::string excerpt;
};

class Collector final : public booksearch::HitSink {
 public:
  std::vector<Found> hits;
  size_t limit = SIZE_MAX;
  bool onHit(const Hit& hit) override {
    hits.push_back({hit.start, hit.end, hit.tag, hit.excerpt ? hit.excerpt : ""});
    return hits.size() < limit;
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
std::vector<Found> search(const std::string& html, const char* query, const size_t chunk = 7,
                          const AnchorTag* anchors = nullptr, const size_t anchorCount = 0, bool* parsed = nullptr,
                          const size_t limit = SIZE_MAX) {
  Collector collector;
  collector.limit = limit;
  SpineTextScanner scanner(collector);
  EXPECT_TRUE(scanner.setQuery(query)) << query;
  EXPECT_TRUE(scanner.begin(anchors, anchorCount, -1));
  for (size_t at = 0; at < html.size(); at += chunk) {
    const size_t n = std::min(chunk, html.size() - at);
    if (scanner.write(reinterpret_cast<const uint8_t*>(html.data() + at), n) != n) break;
  }
  const bool ok = scanner.finish();
  if (parsed) *parsed = ok;
  return collector.hits;
}

size_t countHits(const std::string& body, const char* query) { return search(doc(body), query).size(); }

// --- fold -----------------------------------------------------------------------------------------------

uint32_t foldOne(const uint32_t cp) {
  const Folded folded = booksearch::foldCodepoint(cp);
  return folded.kind == Folded::Letters && folded.count == 1 ? folded.units[0] : 0;
}

TEST(BookSearchFold, DropsCaseAndAccents) {
  EXPECT_EQ(foldOne('Q'), static_cast<uint32_t>('q'));
  EXPECT_EQ(foldOne(0x00C9), static_cast<uint32_t>('e'));  // É
  EXPECT_EQ(foldOne(0x0101), static_cast<uint32_t>('a'));  // ā
  EXPECT_EQ(foldOne(0x0100), static_cast<uint32_t>('a'));  // Ā
  EXPECT_EQ(foldOne(0x1E47), static_cast<uint32_t>('n'));  // ṇ
  EXPECT_EQ(foldOne(0x1E43), static_cast<uint32_t>('m'));  // ṃ
  EXPECT_EQ(foldOne(0x1E5D), static_cast<uint32_t>('r'));  // ṝ: two marks on r
  EXPECT_EQ(foldOne(0x015B), static_cast<uint32_t>('s'));  // ś
  EXPECT_EQ(foldOne(0x1EC7), static_cast<uint32_t>('e'));  // ệ
  EXPECT_EQ(foldOne(0x00F8), static_cast<uint32_t>('o'));  // ø has no decomposition
  EXPECT_EQ(foldOne(0x0142), static_cast<uint32_t>('l'));  // ł
  EXPECT_EQ(foldOne(0x0386), 0x03B1u);                     // Ά
  EXPECT_EQ(foldOne(0x03A3), 0x03C3u);                     // Σ
  EXPECT_EQ(foldOne(0x03C2), 0x03C3u);                     // final ς
  EXPECT_EQ(foldOne(0x0401), 0x0435u);                     // Ё
  EXPECT_EQ(foldOne(0x0416), 0x0436u);                     // Ж
  EXPECT_EQ(foldOne(0xFF21), static_cast<uint32_t>('a'));  // fullwidth Ａ
  EXPECT_EQ(foldOne(0x4E2D), 0x4E2Du);                     // 中 unchanged
}

TEST(BookSearchFold, ExpandsLettersAndLigatures) {
  const Folded sharpS = booksearch::foldCodepoint(0x00DF);
  ASSERT_EQ(sharpS.count, 2);
  EXPECT_EQ(sharpS.units[0], static_cast<uint32_t>('s'));
  const Folded ffi = booksearch::foldCodepoint(0xFB03);
  ASSERT_EQ(ffi.count, 3);
  EXPECT_EQ(ffi.units[2], static_cast<uint32_t>('i'));
}

TEST(BookSearchFold, ClassifiesInvisiblesAndPunctuation) {
  EXPECT_EQ(booksearch::foldCodepoint(0x00AD).kind, Folded::Ignore);  // soft hyphen
  EXPECT_EQ(booksearch::foldCodepoint(0x0301).kind, Folded::Ignore);  // combining acute
  EXPECT_EQ(booksearch::foldCodepoint(0x200B).kind, Folded::Ignore);  // zero-width space
  EXPECT_EQ(booksearch::foldCodepoint(0x2019).kind, Folded::Apostrophe);
  EXPECT_EQ(booksearch::foldCodepoint(0x201C).kind, Folded::Separator);
  EXPECT_EQ(booksearch::foldCodepoint(0x2014).kind, Folded::Separator);
  EXPECT_EQ(booksearch::foldCodepoint(0x00A0).kind, Folded::Separator);
  EXPECT_EQ(booksearch::foldCodepoint(0x3002).kind, Folded::Separator);  // 。
}

// --- matching -------------------------------------------------------------------------------------------

TEST(BookSearchMatch, MatchesBeginAWord) {
  EXPECT_EQ(countHits("<p>Vajrasattva, ovajra and the vajra-holder.</p>", "vajra"), 2u);
  EXPECT_EQ(countHits("<p>catch the cat; locate it</p>", "cat"), 2u);
}

TEST(BookSearchMatch, IgnoresCaseAndAccents) {
  const std::string body =
      "<p>Nirvāṇa, NIRVANA and nirvana.</p><p>Nirva\xCC\x84n\xCC\xA3"
      "a decomposed.</p>";
  EXPECT_EQ(countHits(body, "nirvana"), 4u);
  EXPECT_EQ(countHits(body, "NIRVĀṆA"), 4u);
  EXPECT_EQ(countHits("<p>Saṃsāra and samsara</p>", "samsara"), 2u);
  EXPECT_EQ(countHits("<p>El niño soñó</p>", "nino sono"), 1u);
}

TEST(BookSearchMatch, PunctuationBetweenWordsDoesNotMatter) {
  EXPECT_EQ(countHits("<p>To be, or not to be: that is the question.</p>", "to be or not"), 1u);
  EXPECT_EQ(countHits("<p>the self-arising awareness</p>", "self arising"), 1u);
  EXPECT_EQ(countHits("<p>the self-arising awareness</p>", "self-arising"), 1u);
  EXPECT_EQ(countHits("<p>the self arising awareness</p>",
                      "self\xE2\x80\x94"
                      "arising"),
            1u);
  EXPECT_EQ(countHits("<p>selfarising</p>", "self arising"), 0u);
}

TEST(BookSearchMatch, ApostrophesInsideWordsAndQuotesAround) {
  EXPECT_EQ(countHits("<p>I don\xE2\x80\x99t know.</p>", "don't"), 1u);
  EXPECT_EQ(countHits("<p>I don't know.</p>", "don\xE2\x80\x99t"), 1u);
  EXPECT_EQ(countHits("<p>He said \xE2\x80\x98hello\xE2\x80\x99 twice.</p>", "hello"), 1u);
  EXPECT_EQ(countHits("<p>He said \xE2\x80\x98hello\xE2\x80\x99 twice.</p>", "said hello twice"), 1u);
  EXPECT_EQ(countHits("<p>Ahab\xE2\x80\x99s leg</p>", "ahab"), 1u);
}

TEST(BookSearchMatch, AcrossInlineFormatting) {
  EXPECT_EQ(countHits("<p><span class=\"run-in\">Having thus</span> understood</p>", "thus understood"), 1u);
  EXPECT_EQ(countHits("<p>hel<b>lo</b> there</p>", "hello there"), 1u);
  EXPECT_EQ(countHits("<p><i>Moby</i>-<i>Dick</i></p>", "moby dick"), 1u);
  EXPECT_EQ(countHits("<p><em>Kye ho! O secret vajra-holder,<br/>my vehicles</em></p>", "holder my vehicles"), 1u);
  EXPECT_EQ(countHits("<p>a ﬁne day</p>", "fine"), 1u);
  EXPECT_EQ(countHits("<p>won\xC2\xAD"
                      "der\xC2\xAD"
                      "ful</p>",
                      "wonderful"),
            1u);
}

TEST(BookSearchMatch, BlockEdgesBreakWords) {
  EXPECT_EQ(countHits("<p>end</p><p>start</p>", "endstart"), 0u);
  EXPECT_EQ(countHits("<p>end</p><p>start</p>", "end start"), 1u);
  EXPECT_EQ(countHits("<h2>Title</h2>Body", "titlebody"), 0u);
  EXPECT_EQ(countHits("<p>a<br/>b</p>", "ab"), 0u);
}

TEST(BookSearchMatch, EntitiesMatchTheirText) {
  EXPECT_EQ(countHits("<p>Tom &amp; Jerry</p>", "tom jerry"), 1u);
  EXPECT_EQ(countHits("<p>a&nbsp;b</p>", "a b"), 1u);
  EXPECT_EQ(countHits("<p>don&#x2019;t</p>", "don't"), 1u);
  EXPECT_EQ(countHits("<p>caf&eacute;</p>", "cafe"), 1u);
}

TEST(BookSearchMatch, HiddenTextIsSkippedButCounted) {
  const auto hits = search(doc("<p>one <span style=\"display: none\">two</span> three</p>"), "one three");
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(countHits("<p>one <span style=\"display: none\">two</span> three</p>", "two"), 0u);
  EXPECT_EQ(countHits("<p>one <span hidden=\"\">two</span> three</p>", "two"), 0u);
  // "three" sits after "one ", "two" and " ": the hidden word still advances the offset.
  const auto three = search(doc("<p>one <span style=\"display:none\">two</span> three</p>"), "three");
  ASSERT_EQ(three.size(), 1u);
  EXPECT_EQ(three[0].start, 8u);
}

TEST(BookSearchMatch, NoteMarkersAndPageBreaksDoNotSplitPhrases) {
  EXPECT_EQ(countHits("<p>the end<a epub:type=\"noteref\" href=\"#n1\">12</a> of it</p>", "end of it"), 1u);
  EXPECT_EQ(countHits("<p>the end <span epub:type=\"pagebreak\" title=\"12\">12</span>of it</p>", "end of it"), 1u);
  EXPECT_EQ(countHits("<p><ruby>漢<rt>kan</rt>字<rt>ji</rt></ruby></p>", "漢字"), 1u);
  EXPECT_EQ(countHits("<p>the end<sup><a href=\"notes.xhtml#n1\">12</a></sup> of it</p>", "end of it"), 1u);
  EXPECT_EQ(countHits("<p>the end<a href=\"#n1\"><sup>12</sup></a> of it</p>", "end of it"), 1u);
  EXPECT_EQ(countHits("<p>10<sup>59</sup> kalpas</p>", "1059"), 1u);
}

TEST(BookSearchMatch, CountsFromTheBodyLikeTheParser) {
  const auto hits = search(doc("<p>abc def</p>"), "def");
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].start, 4u);
  EXPECT_EQ(hits[0].end, 7u);
}

TEST(BookSearchMatch, UnspacedScriptsMatchAnywhere) {
  EXPECT_EQ(countHits("<p>我们的仏教の経典</p>", "仏教"), 1u);
  EXPECT_EQ(countHits("<p>私は猫が好き</p>", "猫"), 1u);
}

TEST(BookSearchMatch, RejectsEmptyQueries) {
  Collector collector;
  SpineTextScanner scanner(collector);
  EXPECT_FALSE(scanner.setQuery(""));
  EXPECT_FALSE(scanner.setQuery("  ...  "));
  EXPECT_FALSE(scanner.setQuery(std::string(200, 'a')));
  EXPECT_TRUE(scanner.setQuery(" a "));
}

TEST(BookSearchMatch, NothingFound) { EXPECT_EQ(countHits("<p>Nothing to see here.</p>", "elephant"), 0u); }

TEST(BookSearchMatch, AnchorsTagTheMatchesAfterThem) {
  const AnchorTag anchors[] = {{booksearch::hashAnchorId("one"), 3}, {booksearch::hashAnchorId("two"), 4}};
  const std::string html = doc("<p>word</p><h3 id=\"one\">One</h3><p>word</p><h3 id=\"two\">Two</h3><p>word</p>");
  const auto hits = search(html, "word", 5, anchors, 2);
  ASSERT_EQ(hits.size(), 3u);
  EXPECT_EQ(hits[0].tag, -1);
  EXPECT_EQ(hits[1].tag, 3);
  EXPECT_EQ(hits[2].tag, 4);
}

TEST(BookSearchMatch, StopsWhenTheSinkIsFull) {
  bool parsed = false;
  const auto hits = search(doc("<p>a a a a a a</p>"), "a", 3, nullptr, 0, &parsed, 4);
  EXPECT_EQ(hits.size(), 4u);
  EXPECT_TRUE(parsed);
}

TEST(BookSearchMatch, KeepsMatchesBeforeAParseError) {
  bool parsed = true;
  const auto hits =
      search("<html><body><p>first match</p><p>second <b>match</p></body></html>", "match", 7, nullptr, 0, &parsed);
  EXPECT_FALSE(parsed);
  ASSERT_GE(hits.size(), 1u);
  EXPECT_EQ(hits[0].start, 6u);
}

// --- excerpts -------------------------------------------------------------------------------------------

TEST(BookSearchExcerpt, ShowsTheMatchInItsParagraph) {
  const auto hits = search(doc("<h2>Heading</h2><p>The <em>essence</em>   is a category.</p>"), "essence");
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].excerpt, "The essence is a category.");
}

TEST(BookSearchExcerpt, CutsLongContextOnWordsWithEllipses) {
  std::string paragraph = "<p>";
  for (int i = 0; i < 30; i++) paragraph += "alpha beta gamma ";
  paragraph += "TARGET";
  for (int i = 0; i < 30; i++) paragraph += " delta epsilon";
  paragraph += "</p>";
  const auto hits = search(doc(paragraph), "target");
  ASSERT_EQ(hits.size(), 1u);
  const std::string& excerpt = hits[0].excerpt;
  EXPECT_EQ(excerpt.rfind("\xE2\x80\xA6", 0), 0u) << excerpt;  // starts with …
  EXPECT_NE(excerpt.find(" TARGET delta"), std::string::npos) << excerpt;
  EXPECT_EQ(excerpt.substr(excerpt.size() - 3), "\xE2\x80\xA6") << excerpt;
  EXPECT_LT(excerpt.size(), booksearch::ExcerptBuilder::MAX_BYTES);
  // Whole words only on both sides.
  EXPECT_TRUE(excerpt.find("\xE2\x80\xA6"
                           "alpha") == 0 ||
              excerpt.find("\xE2\x80\xA6"
                           "beta") == 0 ||
              excerpt.find("\xE2\x80\xA6"
                           "gamma") == 0)
      << excerpt;
}

TEST(BookSearchExcerpt, NeighbouringMatchesEachGetAnExcerpt) {
  const auto hits = search(doc("<p>one fish two fish red fish</p>"), "fish");
  ASSERT_EQ(hits.size(), 3u);
  EXPECT_NE(hits[0].excerpt.find("one fish two"), std::string::npos) << hits[0].excerpt;
  EXPECT_NE(hits[2].excerpt.find("red fish"), std::string::npos) << hits[2].excerpt;
}

TEST(BookSearchExcerpt, ComposesDecomposedAccents) {
  const auto hits = search(doc("<p>Nirva\xCC\x84n\xCC\xA3"
                               "a</p>"),
                           "nirvana");
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].excerpt,
            "Nirv\xC4\x81\xE1\xB9\x87"
            "a");
}

// --- results file -------------------------------------------------------------------------------------

TEST(BookSearchResultsFile, RoundTripsAndReadsAWindow) {
  booksearch::ResultsHeader header;
  header.state = booksearch::SearchState::Running;
  header.spineCount = 33;
  header.nextSpine = 12;
  header.spineFirstResult = 3;
  header.count = 5;
  header.tagAtNextSpine = 7;
  strcpy(header.query,
         "nirv\xC4\x81\xE1\xB9\x87"
         "a");
  uint8_t headerBytes[booksearch::ResultsHeader::SIZE];
  header.serialize(headerBytes);

  const auto path = (std::filesystem::temp_directory_path() / "crosspoint-search.bin").string();
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(headerBytes), sizeof(headerBytes));
    for (uint16_t i = 0; i < header.count; i++) {
      booksearch::ResultRecord record;
      record.spineIndex = i;
      record.tocIndex = static_cast<int16_t>(i - 1);
      record.start = 1000u * i;
      record.end = 1000u * i + 7;
      record.bookPercent = static_cast<uint8_t>(10 * i);
      snprintf(record.excerpt, sizeof(record.excerpt), "excerpt %u", i);
      uint8_t bytes[booksearch::ResultRecord::SIZE];
      record.serialize(bytes);
      out.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }
  }

  booksearch::ResultsHeader read;
  ASSERT_TRUE(booksearch::readHeader(path, read));
  EXPECT_EQ(read.state, booksearch::SearchState::Running);
  EXPECT_EQ(read.spineCount, 33);
  EXPECT_EQ(read.nextSpine, 12);
  EXPECT_EQ(read.spineFirstResult, 3);
  EXPECT_EQ(read.count, 5);
  EXPECT_EQ(read.tagAtNextSpine, 7);
  EXPECT_STREQ(read.query, header.query);

  booksearch::ResultRecord window[4];
  ASSERT_EQ(booksearch::readResults(path, 2, 4, window), 3u);  // only 2, 3 and 4 exist
  EXPECT_EQ(window[0].spineIndex, 2);
  EXPECT_EQ(window[0].tocIndex, 1);
  EXPECT_EQ(window[1].start, 3000u);
  EXPECT_EQ(window[1].end, 3007u);
  EXPECT_EQ(window[2].bookPercent, 40);
  EXPECT_STREQ(window[2].excerpt, "excerpt 4");
  std::filesystem::remove(path);
}

TEST(BookSearchResultsFile, RejectsForeignOrInconsistentHeaders) {
  booksearch::ResultsHeader header;
  header.count = 4;
  header.spineFirstResult = 2;
  uint8_t bytes[booksearch::ResultsHeader::SIZE];
  header.serialize(bytes);
  booksearch::ResultsHeader read;
  EXPECT_TRUE(read.deserialize(bytes));

  uint8_t foreign[booksearch::ResultsHeader::SIZE];
  memcpy(foreign, bytes, sizeof(bytes));
  foreign[0] ^= 0xFF;
  EXPECT_FALSE(read.deserialize(foreign));

  header.spineFirstResult = 5;  // past the count
  header.serialize(bytes);
  EXPECT_FALSE(read.deserialize(bytes));

  header.spineFirstResult = 0;
  header.count = booksearch::MAX_RESULTS + 1;
  header.serialize(bytes);
  EXPECT_FALSE(read.deserialize(bytes));
}

// --- page placement -------------------------------------------------------------------------------------

TEST(BookSearchPage, LocatorFindsWordsSplitByLayout) {
  booksearch::PageMatchLocator locator;
  ASSERT_TRUE(locator.setQuery("self-arising wisdom"));
  const char* words[] = {"the", "self-", "arising", "wis", "dom", "is"};
  for (uint16_t i = 0; i < 6; i++) locator.addWord(words[i], i);
  uint16_t first = 0;
  uint16_t last = 0;
  ASSERT_TRUE(locator.nearest(0.5f, first, last));
  EXPECT_EQ(first, 1);
  EXPECT_EQ(last, 4);
}

TEST(BookSearchPage, LocatorPrefersTheOccurrenceNearThePosition) {
  booksearch::PageMatchLocator locator;
  ASSERT_TRUE(locator.setQuery("cat"));
  const char* words[] = {"cat", "x", "x", "x", "x", "x", "x", "x", "x", "cat"};
  for (uint16_t i = 0; i < 10; i++) locator.addWord(words[i], i);
  uint16_t first = 0;
  uint16_t last = 0;
  ASSERT_TRUE(locator.nearest(0.9f, first, last));
  EXPECT_EQ(first, 9);
  ASSERT_TRUE(locator.nearest(0.1f, first, last));
  EXPECT_EQ(first, 0);
}

struct LaidOutPage {
  uint32_t start;
  std::vector<std::string> words;
};

// Lays the chapter out with the firmware's parser on a small page, so it spans many pages.
std::vector<LaidOutPage> layOut(const std::string& html, uint32_t& visibleTotal) {
  const auto path = std::filesystem::temp_directory_path() / "crosspoint-book-search.xhtml";
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
                                   for (uint16_t w = 0; w < block.wordCount(); w++) {
                                     laidOut.words.emplace_back(block.wordText(w));
                                   }
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
  // Formatting, entities, comments, hidden text, verse breaks and CRLF line ends, repeated with variations
  // so matches fall on many pages and at page edges.
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
        body +=
            "Nirva\xCC\x84n\xCC\xA3"
            "a and nirvana again";
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

TEST(BookSearchPage, EveryMatchLandsOnThePageThatShowsIt) {
  const std::string html = longChapter();
  uint32_t visibleTotal = 0;
  const auto pages = layOut(html, visibleTotal);
  ASSERT_GT(pages.size(), 30u);

  for (const char* query :
       {"nirvana", "samsara", "beyond", "self arising awareness", "tom jerry", "don't argue", "holder my vehicles",
        "before the samsara", "phenomena", "now positions", "caf\xC3\xA9 au lait"}) {
    SCOPED_TRACE(query);
    Collector collector;
    SpineTextScanner scanner(collector);
    ASSERT_TRUE(scanner.setQuery(query));
    ASSERT_TRUE(scanner.begin(nullptr, 0, -1));
    scanner.write(reinterpret_cast<const uint8_t*>(html.data()), html.size());
    ASSERT_TRUE(scanner.finish());
    // The scanner counts the same text the parser counts.
    EXPECT_EQ(scanner.visibleTextLength(), visibleTotal);
    ASSERT_FALSE(collector.hits.empty());

    for (const auto& hit : collector.hits) {
      size_t page = 0;
      while (page + 1 < pages.size() && pages[page + 1].start <= hit.start) page++;
      // The match begins on `page` and may run onto the next one.
      booksearch::PageMatchLocator locator;
      ASSERT_TRUE(locator.setQuery(query));
      uint16_t index = 0;
      for (size_t p = page; p < std::min(page + 2, pages.size()); p++) {
        for (const auto& word : pages[p].words) locator.addWord(word, index++);
      }
      uint16_t first = 0;
      uint16_t last = 0;
      ASSERT_TRUE(locator.nearest(0.0f, first, last)) << "offset " << hit.start << " page " << page;
      EXPECT_LT(first, pages[page].words.size()) << "offset " << hit.start << " page " << page;
    }
  }
}

}  // namespace
