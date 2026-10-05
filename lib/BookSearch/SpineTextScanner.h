#pragma once

// Streams one spine item's XHTML (fed through Print, straight from the zip or the unzipped HTML cache) and
// reports every match of a query with its excerpt.
//
// Match positions are visible-text offsets: zero-based codepoints of <body> character data outside
// head/style/script/title/rp, counted exactly as ChapterHtmlSlimParser counts them while it records each
// page's start (Section::getPageForVisibleTextOffset). They do not depend on font, size or margins, so a
// result found once opens on the right page under any layout.
//
// Matching sees the text a reader sees: inline formatting (<em>, <span>, ...) joins text without a break,
// block edges (paragraphs, headings, <br/>) break words, and hidden content (hidden attribute, inline
// display:none, page-break markers, note references, ruby annotations) is skipped while still counted.

#include <Print.h>
#include <expat.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "BookSearchText.h"
#include "HtmlTextRules.h"

namespace booksearch {

// An element id that names a place in the book, typically a table-of-contents entry, as a 32-bit FNV-1a
// hash, and the tag a match after it carries.
struct AnchorTag {
  uint32_t idHash;
  int16_t tag;
};

uint32_t hashAnchorId(std::string_view id);

struct Hit {
  uint32_t start;      // visible-text offset of the match's first codepoint
  uint32_t end;        // one past its last codepoint
  uint32_t byteIndex;  // byte position in the XHTML where the match was found
  int16_t tag;         // the AnchorTag in force where the match begins
  const char* excerpt;
};

class HitSink {
 public:
  // Return false to stop the scan (the result list is full).
  virtual bool onHit(const Hit& hit) = 0;

 protected:
  ~HitSink() = default;
};

class SpineTextScanner final : public Print, private MatchSink {
 public:
  explicit SpineTextScanner(HitSink& sink) : sink(sink), matcher(*this) {}
  ~SpineTextScanner() override;
  SpineTextScanner(const SpineTextScanner&) = delete;
  SpineTextScanner& operator=(const SpineTextScanner&) = delete;

  // False when the query has nothing to search for.
  bool setQuery(std::string_view utf8Query) { return matcher.setQuery(utf8Query); }

  // Prepares a new document. `anchors` must outlive the scan. False when expat cannot be allocated.
  bool begin(const AnchorTag* anchors, size_t anchorCount, int16_t initialTag);
  // Delivers the last pending match. False when the document did not parse to its end; matches already
  // delivered stay valid, since every offset before the error was counted the way the reader counts it.
  bool finish();
  // Frees the parser early (finish() and the destructor also do).
  void end();

  size_t write(uint8_t byte) override { return write(&byte, 1); }
  // Returns less than `size` once the scan has stopped (a parse error, or the sink asked to stop), which
  // lets an early-stopping source stop reading.
  size_t write(const uint8_t* buffer, size_t size) override;

  bool stopped() const { return halted; }
  int16_t currentTag() const { return tag; }
  uint32_t visibleTextLength() const { return visibleOffset; }

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL endElement(void* userData, const XML_Char* name);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandler(void* userData, const XML_Char* s, int len);

  void onStart(const XML_Char* name, const XML_Char** atts);
  void onEnd(const XML_Char* name);
  void onText(const XML_Char* s, int len);
  void breakBlock();
  void deliverPending(bool more);
  void onMatch(uint32_t start, uint32_t end) override;

  // Where match text goes: folded for matching, verbatim for excerpts.
  class FolderSink final : public UnitSink {
   public:
    explicit FolderSink(PhraseMatcher& matcher) : matcher(matcher) {}
    void onUnit(uint32_t unit, uint32_t offset, bool wordStart) override { matcher.onUnit(unit, offset, wordStart); }

   private:
    PhraseMatcher& matcher;
  };

  HitSink& sink;
  PhraseMatcher matcher;
  FolderSink folderSink{matcher};
  Folder folder{folderSink};
  ExcerptBuilder excerpt;

  XML_Parser parser = nullptr;
  bool parseFailed = false;
  bool halted = false;
  bool htmlEnded = false;

  // Mirrors ChapterHtmlSlimParser's visible-offset bookkeeping.
  bool insideBody = false;
  uint16_t nonVisibleDepth = 0;
  uint32_t visibleOffset = 0;

  // Depth inside content the page does not show; its text is counted but not searched.
  uint16_t hiddenDepth = 0;
  NoteMarkerTracker noteMarkers;

  const AnchorTag* anchors = nullptr;
  size_t anchorCount = 0;
  int16_t tag = -1;

  // The match whose excerpt is still collecting the text after it.
  bool hitPending = false;
  Hit pendingHit{};
};

}  // namespace booksearch
