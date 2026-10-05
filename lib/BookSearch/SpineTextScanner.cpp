#include "SpineTextScanner.h"

#include <Epub/VisibleTextUtils.h>
#include <Epub/htmlEntities.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <strings.h>

#include <cstring>

#include "HtmlTextRules.h"

namespace booksearch {

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
  noteMarkers.reset();
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
    if (const char* id = findAttribute(atts, "id")) {
      const uint32_t hash = hashAnchorId(id);
      for (size_t i = 0; i < anchorCount; i++) {
        if (anchors[i].idHash == hash) {
          tag = anchors[i].tag;
          break;
        }
      }
    }
  }
  if (hidesText(localName, atts) || noteMarkers.opens(localName, atts)) {
    hiddenDepth = 1;
    return;
  }
  if (!isInlineElement(localName)) breakBlock();
}

void SpineTextScanner::onEnd(const XML_Char* name) {
  const bool wasNonVisible = nonVisibleDepth > 0;
  if (nonVisibleDepth > 0) nonVisibleDepth--;
  if (insideBody && !wasNonVisible && !halted) {
    const char* localName = xmlLocalName(name);
    if (hiddenDepth > 0) {
      hiddenDepth--;
    } else {
      noteMarkers.closes(localName);
      if (!isInlineElement(localName)) breakBlock();
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
