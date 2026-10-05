#include "SpineWordScanner.h"

#include <Epub/VisibleTextUtils.h>
#include <Epub/htmlEntities.h>
#include <HtmlTextRules.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <strings.h>

#include <cstring>

namespace speedread {

namespace {

bool isSeparator(const uint32_t cp) {
  return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200B) ||
         cp == 0x2028 || cp == 0x2029 || cp == 0x205F || cp == 0x3000;
}

size_t encodeUtf8(const uint32_t cp, char* out) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

}  // namespace

SpineWordScanner::~SpineWordScanner() { end(); }

void SpineWordScanner::end() { destroyXmlParser(parser); }

bool SpineWordScanner::begin(const uint16_t spineIndex, const uint32_t bytes) {
  end();
  parser = XML_ParserCreate(nullptr);
  if (!parser) return false;
  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  // Same entity handling as the layout parser, so &nbsp; and friends count the same.
  XML_SetDefaultHandlerExpand(parser, defaultHandler);

  spine = spineIndex;
  totalBytes = bytes;
  parseFailed = false;
  htmlEnded = false;
  insideBody = false;
  nonVisibleDepth = 0;
  visibleOffset = 0;
  hiddenDepth = 0;
  noteMarkers.reset();
  isBuilding = false;
  holding = false;
  return true;
}

size_t SpineWordScanner::write(const uint8_t* buffer, const size_t size) {
  if (!parser || parseFailed) return 0;
  if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
    // Trailing junk after </html> is not an error worth reporting; the words before it are all delivered.
    if (!htmlEnded) parseFailed = true;
    end();
    return 0;
  }
  return size;
}

bool SpineWordScanner::finish() {
  if (parser && XML_Parse(parser, "", 0, XML_TRUE) != XML_STATUS_OK && !htmlEnded) parseFailed = true;
  endWord(true);
  if (holding) {
    held.flags |= PARAGRAPH_END | CHAPTER_END;
    sink.onWord(held, heldEnd);
    holding = false;
  }
  end();
  return !parseFailed;
}

void XMLCALL SpineWordScanner::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  static_cast<SpineWordScanner*>(userData)->onStart(name, atts);
}

void XMLCALL SpineWordScanner::endElement(void* userData, const XML_Char* name) {
  static_cast<SpineWordScanner*>(userData)->onEnd(name);
}

void XMLCALL SpineWordScanner::characterData(void* userData, const XML_Char* s, const int len) {
  static_cast<SpineWordScanner*>(userData)->onText(s, len);
}

void XMLCALL SpineWordScanner::defaultHandler(void* userData, const XML_Char* s, const int len) {
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

void SpineWordScanner::onStart(const XML_Char* name, const XML_Char** atts) {
  // Visible-offset rules, as ChapterHtmlSlimParser::startElement applies them.
  if (strcasecmp(name, "body") == 0) insideBody = true;
  if (insideBody && (nonVisibleDepth > 0 || VisibleTextUtils::isNonVisibleElement(name))) nonVisibleDepth++;
  if (!insideBody || nonVisibleDepth > 0) return;

  if (hiddenDepth > 0) {
    hiddenDepth++;
    return;
  }
  const char* localName = xmlLocalName(name);
  if (booksearch::hidesText(localName, atts) || noteMarkers.opens(localName, atts)) {
    hiddenDepth = 1;
    return;
  }
  if (!booksearch::isInlineElement(localName)) breakBlock();
}

void SpineWordScanner::onEnd(const XML_Char* name) {
  const bool wasNonVisible = nonVisibleDepth > 0;
  if (nonVisibleDepth > 0) nonVisibleDepth--;
  if (insideBody && !wasNonVisible) {
    const char* localName = xmlLocalName(name);
    if (hiddenDepth > 0) {
      hiddenDepth--;
    } else {
      noteMarkers.closes(localName);
      if (!booksearch::isInlineElement(localName)) breakBlock();
    }
  }
  if (strcmp(name, "body") == 0) insideBody = false;
  if (strcmp(name, "html") == 0) htmlEnded = true;
}

void SpineWordScanner::onText(const XML_Char* s, const int len) {
  // Counted exactly as ChapterHtmlSlimParser::characterData counts.
  if (!insideBody || nonVisibleDepth > 0) return;
  const auto* cursor = reinterpret_cast<const unsigned char*>(s);
  const auto* limit = cursor + len;
  while (cursor < limit) {
    const uint32_t cp = utf8NextCodepoint(&cursor);
    const uint32_t offset = visibleOffset++;
    if (hiddenDepth == 0) addCodepoint(cp, offset);
  }
}

void SpineWordScanner::addCodepoint(const uint32_t cp, const uint32_t offset) {
  if (isSeparator(cp)) {
    endWord(false);
    return;
  }
  // Soft hyphens and byte-order marks are never drawn.
  if (cp == 0x00AD || cp == 0xFEFF) return;
  if (cp == 0x00A0 || cp == 0x202F) {
    // A no-break space keeps its words together ("10 km"), drawn as a plain space.
    if (isBuilding && building.length + 1 < Word::TEXT_BYTES) {
      appendBytes(" ", 1);
      buildingEnd = offset + 1;
    }
    return;
  }

  char bytes[4];
  const size_t length = encodeUtf8(cp, bytes);
  if (!isBuilding) {
    startWord(offset);
  } else if (building.length + length >= Word::TEXT_BYTES) {
    building.flags |= CONTINUES;
    pushWord();
    startWord(offset);
  }
  appendBytes(bytes, length);
  buildingEnd = offset + 1;

  // An em dash between words ("this—that") ends the word before it.
  if ((cp == 0x2014 || cp == 0x2015) && building.length > length &&
      hasContent(building.text, building.length - length)) {
    endWord(false);
  }
}

void SpineWordScanner::startWord(const uint32_t offset) {
  building = Word{};
  building.offset = offset;
  building.spine = spine;
  if (parser && totalBytes > 0) {
    const XML_Index at = XML_GetCurrentByteIndex(parser);
    const uint64_t permille = at > 0 ? static_cast<uint64_t>(at) * 1000 / totalBytes : 0;
    building.permille = static_cast<uint16_t>(permille > 1000 ? 1000 : permille);
  }
  isBuilding = true;
}

void SpineWordScanner::appendBytes(const char* bytes, const size_t length) {
  memcpy(building.text + building.length, bytes, length);
  building.length = static_cast<uint8_t>(building.length + length);
  building.text[building.length] = '\0';
}

void SpineWordScanner::endWord(const bool blockEnds) {
  if (!isBuilding) return;
  while (building.length > 0 && building.text[building.length - 1] == ' ') building.text[--building.length] = '\0';
  if (building.length == 0) {
    isBuilding = false;
    return;
  }
  if (!hasContent(building.text, building.length)) {
    // Punctuation on its own ("—", "…") joins the word before it in the same paragraph...
    if (holding && !(held.flags & (PARAGRAPH_END | CONTINUES)) &&
        held.length + 1 + building.length < Word::TEXT_BYTES) {
      held.text[held.length++] = ' ';
      memcpy(held.text + held.length, building.text, building.length);
      held.length = static_cast<uint8_t>(held.length + building.length);
      held.text[held.length] = '\0';
      heldEnd = buildingEnd;
      isBuilding = false;
      return;
    }
    // ...or, opening one, leads the word after it. A block of nothing else ("* * *") is a word of its own.
    if (!blockEnds && building.length + 1 < Word::TEXT_BYTES) {
      appendBytes(" ", 1);
      return;
    }
  }
  pushWord();
}

void SpineWordScanner::pushWord() {
  if (holding) sink.onWord(held, heldEnd);
  held = building;
  heldEnd = buildingEnd;
  holding = true;
  isBuilding = false;
}

void SpineWordScanner::breakBlock() {
  endWord(true);
  if (holding) held.flags |= PARAGRAPH_END;
}

}  // namespace speedread
