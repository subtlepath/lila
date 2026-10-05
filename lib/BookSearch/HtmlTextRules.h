#pragma once

// What a reader sees of a chapter's XHTML, element by element: which elements flow inside a line and which
// content the page does not show. Shared by every scanner that walks chapter text the way the layout parser
// lays it out (search, speed reading), so they agree on words and visible-text offsets.

#include <expat.h>

#include <cstdint>

namespace booksearch {

// Elements that flow inside a line. Every other element starts or ends a block, which breaks words.
bool isInlineElement(const char* localName);

// Content a reader does not see on the page: the hidden attribute and inline display:none (which the
// layout skips), page-break markers (skipped too), note-reference markers ("1" between two words would
// break a phrase) and ruby annotations (set above the line, not in it).
bool hidesText(const char* localName, const XML_Char** atts);

// Note markers that books set as a superscript link without epub:type="noteref":
// <sup><a href="notes.xhtml#n14">14</a></sup> or <a href="#n14"><sup>14</sup></a>. The page draws them small
// and raised; read in line they would join the word before ("wisdom14"). Fed the elements that open and close
// outside hidden content, it picks out the inner half of such a pair, which is then hidden like hidesText().
// A superscript without a link (10<sup>59</sup>, 1<sup>st</sup>) stays.
class NoteMarkerTracker {
 public:
  void reset();
  // True when the element is a note marker's inner half; it does not then close through closes().
  bool opens(const char* localName, const XML_Char** atts);
  void closes(const char* localName);

 private:
  uint8_t supDepth = 0;
  bool insideLink = false;
};

// The value of attribute `name`, or nullptr.
const char* findAttribute(const XML_Char** atts, const char* name);

}  // namespace booksearch
