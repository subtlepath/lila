#include "HtmlTextRules.h"

#include <strings.h>

#include <cstddef>
#include <cstring>

namespace booksearch {

namespace {

constexpr const char* INLINE_TAGS[] = {
    "a",     "abbr", "acronym", "b",      "bdi",   "bdo",  "big",  "cite", "code", "data", "del",  "dfn", "em",
    "font",  "i",    "ins",     "kbd",    "label", "mark", "nobr", "q",    "rb",   "rtc",  "ruby", "s",   "samp",
    "small", "span", "strike",  "strong", "sub",   "sup",  "time", "tt",   "u",    "var",  "wbr"};

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

// A link to another place in the book: no URI scheme before the path, query or fragment.
bool isInternalLink(const char* href) {
  if (!href || href[0] == '\0') return false;
  for (const char* cursor = href; *cursor && *cursor != '/' && *cursor != '?' && *cursor != '#'; cursor++) {
    if (*cursor == ':') return false;
  }
  return true;
}

}  // namespace

bool isInlineElement(const char* localName) {
  for (const char* tag : INLINE_TAGS) {
    if (strcasecmp(localName, tag) == 0) return true;
  }
  return false;
}

const char* findAttribute(const XML_Char** atts, const char* name) {
  for (int i = 0; atts && atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

bool hidesText(const char* localName, const XML_Char** atts) {
  if (strcasecmp(localName, "rt") == 0) return true;
  if (findAttribute(atts, "hidden") != nullptr || styleHides(findAttribute(atts, "style"))) return true;
  const char* epubType = findAttribute(atts, "epub:type");
  if (hasToken(epubType, "pagebreak") || hasToken(epubType, "noteref")) return true;
  const char* role = findAttribute(atts, "role");
  return role && (strcmp(role, "doc-pagebreak") == 0 || strcmp(role, "doc-noteref") == 0);
}

void NoteMarkerTracker::reset() {
  supDepth = 0;
  insideLink = false;
}

bool NoteMarkerTracker::opens(const char* localName, const XML_Char** atts) {
  if (strcasecmp(localName, "sup") == 0) {
    if (insideLink) return true;
    if (supDepth < UINT8_MAX) supDepth++;
    return false;
  }
  if (strcasecmp(localName, "a") == 0 && isInternalLink(findAttribute(atts, "href"))) {
    if (supDepth > 0) return true;
    // Links do not nest, so the next </a> closes this one.
    insideLink = true;
  }
  return false;
}

void NoteMarkerTracker::closes(const char* localName) {
  if (strcasecmp(localName, "sup") == 0) {
    if (supDepth > 0) supDepth--;
  } else if (strcasecmp(localName, "a") == 0) {
    insideLink = false;
  }
}

}  // namespace booksearch
