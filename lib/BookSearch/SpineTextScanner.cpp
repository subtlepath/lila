#include "SpineTextScanner.h"

#include <Epub/VisibleTextUtils.h>
#include <Epub/htmlEntities.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <strings.h>

#include <cstring>

namespace booksearch {

namespace {

// Elements that flow inside a line. Every other element starts or ends a block, which breaks words.
constexpr const char* INLINE_TAGS[] = {
    "a",     "abbr", "acronym", "b",      "bdi",   "bdo",  "big",  "cite", "code", "data", "del",  "dfn", "em",
    "font",  "i",    "ins",     "kbd",    "label", "mark", "nobr", "q",    "rb",   "rtc",  "ruby", "s",   "samp",
    "small", "span", "strike",  "strong", "sub",   "sup",  "time", "tt",   "u",    "var",  "wbr"};

bool isInline(const char* localName) {
  for (const char* tag : INLINE_TAGS) {
    if (strcasecmp(localName, tag) == 0) return true;
  }
  return false;
}

const char* attribute(const XML_Char** atts, const char* name) {
  for (int i = 0; atts && atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

// A whitespace-separated token list (epub:type) contains `token`.
bool hasToken(const char* list, const char* token) {
  if (!list) return false;
  const size_t tokenLength = strlen(token);
  const char* cursor = list;
  while (*cursor) {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n') cursor++;
    const char* start = cursor;
    while (*cursor && *cursor != ' ' && *cursor != '\t' && *cursor != '\n') cursor++;
    const size_t length = static_cast<size_t>(cursor - start);
    // "z3998:pagebreak"-style prefixed tokens count too.
    if (length >= tokenLength && strncmp(cursor - tokenLength, token, tokenLength) == 0 &&
        (length == tokenLength || cursor[-static_cast<ptrdiff_t>(tokenLength) - 1] == ':')) {
      return true;
    }
  }
  return false;
}

// An inline style that hides the element ("display: none").
bool styleHides(const char* style) {
  if (!style) return false;
  for (const char* cursor = style; *cursor; cursor++) {
    if (strncasecmp(cursor, "display", 7) != 0) continue;
    const char* value = cursor + 7;
    while (*value == ' ' || *value == '\t') value++;
    if (*value != ':') continue;
    value++;
    while (*value == ' ' || *value == '\t') value++;
    if (strncasecmp(value, "none", 4) == 0) return true;
  }
  return false;
}

// Content a reader does not see on the page: the hidden attribute and inline display:none (which the
// layout skips), page-break markers (skipped too), note-reference markers ("1" between two words would
// break a phrase) and ruby annotations (set above the line, not in it).
bool hidesText(const char* localName, const XML_Char** atts) {
  if (strcasecmp(localName, "rt") == 0) return true;
  if (attribute(atts, "hidden") != nullptr || styleHides(attribute(atts, "style"))) return true;
  const char* epubType = attribute(atts, "epub:type");
  if (hasToken(epubType, "pagebreak") || hasToken(epubType, "noteref")) return true;
  const char* role = attribute(atts, "role");
  return role && (strcmp(role, "doc-pagebreak") == 0 || strcmp(role, "doc-noteref") == 0);
}

}  // namespace

uint32_t hashAnchorId(const std::string_view id) {
  uint32_t hash = 2166136261u;
  for (const char c : id) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 16777619u;
  }
  return hash;
}

SpineTextScanner::~SpineTextScanner() { end(); }

void SpineTextScanner::end() { destroyXmlParser(parser); }

bool SpineTextScanner::begin(const AnchorTag* anchorTags, const size_t count, const int16_t initialTag) {
  end();
  parser = XML_ParserCreate(nullptr);
  if (!parser) return false;
  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  // Same entity handling as the layout parser, so &nbsp; and friends count the same.
  XML_SetDefaultHandlerExpand(parser, defaultHandler);

  parseFailed = false;
  halted = false;
  htmlEnded = false;
  insideBody = false;
  nonVisibleDepth = 0;
  visibleOffset = 0;
  hiddenDepth = 0;
  anchors = anchorTags;
  anchorCount = count;
  tag = initialTag;
  hitPending = false;
  matcher.reset();
  folder.reset();
  excerpt.reset();
  return true;
}

size_t SpineTextScanner::write(const uint8_t* buffer, const size_t size) {
  if (!parser || halted) return 0;
  if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
    // A stop requested from a callback (the result list is full) also lands here, as XML_ERROR_ABORTED,
    // with `halted` already set by deliverPending().
    // cppcheck-suppress knownConditionTrueFalse
    if (!halted && !htmlEnded) parseFailed = true;
    halted = true;
    return 0;
  }
  return size;
}

bool SpineTextScanner::finish() {
  if (parser && !halted && XML_Parse(parser, "", 0, XML_TRUE) != XML_STATUS_OK && !htmlEnded) {
    parseFailed = true;
  }
  // Also after a parse error: everything matched before it was counted the way the reader counts it.
  deliverPending(false);
  end();
  return !parseFailed;
}

void XMLCALL SpineTextScanner::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  static_cast<SpineTextScanner*>(userData)->onStart(name, atts);
}

void XMLCALL SpineTextScanner::endElement(void* userData, const XML_Char* name) {
  static_cast<SpineTextScanner*>(userData)->onEnd(name);
}

void XMLCALL SpineTextScanner::characterData(void* userData, const XML_Char* s, const int len) {
  static_cast<SpineTextScanner*>(userData)->onText(s, len);
}

void XMLCALL SpineTextScanner::defaultHandler(void* userData, const XML_Char* s, const int len) {
  // ChapterHtmlSlimParser::defaultHandlerExpand: a known HTML entity becomes its text, an unknown one stays
  // as written, anything else is not text.
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (value) {
      characterData(userData, value, static_cast<int>(strlen(value)));
    } else {
      characterData(userData, s, len);
    }
  }
}

void SpineTextScanner::onStart(const XML_Char* name, const XML_Char** atts) {
  // Visible-offset rules, as ChapterHtmlSlimParser::startElement applies them.
  if (strcasecmp(name, "body") == 0) insideBody = true;
  if (insideBody && (nonVisibleDepth > 0 || VisibleTextUtils::isNonVisibleElement(name))) nonVisibleDepth++;
  if (!insideBody || nonVisibleDepth > 0 || halted) return;

  if (hiddenDepth > 0) {
    hiddenDepth++;
    return;
  }
  const char* localName = xmlLocalName(name);
  if (anchorCount > 0) {
    if (const char* id = attribute(atts, "id")) {
      const uint32_t hash = hashAnchorId(id);
      for (size_t i = 0; i < anchorCount; i++) {
        if (anchors[i].idHash == hash) {
          tag = anchors[i].tag;
          break;
        }
      }
    }
  }
  if (hidesText(localName, atts)) {
    hiddenDepth = 1;
    return;
  }
  if (!isInline(localName)) breakBlock();
}

void SpineTextScanner::onEnd(const XML_Char* name) {
  const bool wasNonVisible = nonVisibleDepth > 0;
  if (nonVisibleDepth > 0) nonVisibleDepth--;
  if (insideBody && !wasNonVisible && !halted) {
    if (hiddenDepth > 0) {
      hiddenDepth--;
    } else if (!isInline(xmlLocalName(name))) {
      breakBlock();
    }
  }
  if (strcmp(name, "body") == 0) insideBody = false;
  if (strcmp(name, "html") == 0) htmlEnded = true;
}

void SpineTextScanner::onText(const XML_Char* s, const int len) {
  // Counted exactly as ChapterHtmlSlimParser::characterData counts.
  if (!insideBody || nonVisibleDepth > 0) return;
  const auto* cursor = reinterpret_cast<const unsigned char*>(s);
  const auto* limit = cursor + len;
  const bool searchable = hiddenDepth == 0 && !halted;
  while (cursor < limit) {
    const uint32_t cp = utf8NextCodepoint(&cursor);
    const uint32_t offset = visibleOffset++;
    if (!searchable || halted) continue;
    excerpt.push(cp, offset);
    folder.feed(cp, offset);
    if (hitPending && excerpt.complete()) deliverPending(true);
  }
}

void SpineTextScanner::breakBlock() {
  deliverPending(false);
  excerpt.breakBlock();
  folder.breakWord();
}

void SpineTextScanner::deliverPending(const bool more) {
  if (!hitPending) return;
  hitPending = false;
  pendingHit.excerpt = excerpt.finish(more);
  if (!sink.onHit(pendingHit)) {
    halted = true;
    if (parser) XML_StopParser(parser, XML_FALSE);
  }
}

void SpineTextScanner::onMatch(const uint32_t start, const uint32_t matchEnd) {
  if (halted) return;
  deliverPending(true);
  if (halted) return;
  pendingHit.start = start;
  pendingHit.end = matchEnd;
  pendingHit.byteIndex = parser ? static_cast<uint32_t>(XML_GetCurrentByteIndex(parser)) : 0;
  pendingHit.tag = tag;
  pendingHit.excerpt = nullptr;
  hitPending = true;
  excerpt.beginMatch(start, matchEnd);
}

}  // namespace booksearch
