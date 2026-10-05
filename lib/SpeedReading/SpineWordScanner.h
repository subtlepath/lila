#pragma once

// Streams one spine item's XHTML (fed through Print) and delivers its words in reading order.
//
// Each word carries the visible-text offset of its first codepoint, counted exactly as ChapterHtmlSlimParser
// counts page starts (the rules SpineTextScanner follows), so a word opens on the page that shows it under
// any layout. Words follow what a reader sees: inline formatting joins text, block edges break words and end
// paragraphs, hidden content (display:none, page-break markers, note references, ruby) is skipped while
// still counted. Punctuation standing alone joins the word next to it, and a word breaks after an em dash.

#include <HtmlTextRules.h>
#include <Print.h>
#include <expat.h>

#include <cstddef>
#include <cstdint>

#include "SpeedReadingText.h"

namespace speedread {

class WordSink {
 public:
  // `end` is the visible-text offset one past the word's last codepoint.
  virtual void onWord(const Word& word, uint32_t end) = 0;

 protected:
  ~WordSink() = default;
};

class SpineWordScanner final : public Print {
 public:
  explicit SpineWordScanner(WordSink& sink) : sink(sink) {}
  ~SpineWordScanner() override;
  SpineWordScanner(const SpineWordScanner&) = delete;
  SpineWordScanner& operator=(const SpineWordScanner&) = delete;

  // Prepares a new document of `totalBytes` bytes (for each word's position). False when expat cannot be
  // allocated.
  bool begin(uint16_t spine, uint32_t totalBytes);
  // Delivers the last word, marked as ending the chapter. False when the document did not parse to its end;
  // the words already delivered stay valid.
  bool finish();
  // Frees the parser early (finish() and the destructor also do).
  void end();

  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* buffer, size_t size) override;

  uint32_t visibleTextLength() const { return visibleOffset; }

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL endElement(void* userData, const XML_Char* name);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandler(void* userData, const XML_Char* s, int len);

  void onStart(const XML_Char* name, const XML_Char** atts);
  void onEnd(const XML_Char* name);
  void onText(const XML_Char* s, int len);

  void addCodepoint(uint32_t cp, uint32_t offset);
  void startWord(uint32_t offset);
  void appendBytes(const char* bytes, size_t length);
  // The word being built is complete. Inside a block, punctuation with no word before it waits to lead the
  // next word.
  void endWord(bool blockEnds);
  // A finished word: the one held back goes out, this one is held until it is known whether a paragraph or
  // the chapter ends after it.
  void pushWord();
  void breakBlock();

  WordSink& sink;
  XML_Parser parser = nullptr;
  bool parseFailed = false;
  bool htmlEnded = false;
  uint16_t spine = 0;
  uint32_t totalBytes = 0;

  // Mirrors ChapterHtmlSlimParser's visible-offset bookkeeping.
  bool insideBody = false;
  uint16_t nonVisibleDepth = 0;
  uint32_t visibleOffset = 0;
  // Depth inside content the page does not show; its text is counted but not read.
  uint16_t hiddenDepth = 0;
  booksearch::NoteMarkerTracker noteMarkers;

  Word building{};
  uint32_t buildingEnd = 0;
  bool isBuilding = false;
  Word held{};
  uint32_t heldEnd = 0;
  bool holding = false;
};

}  // namespace speedread
