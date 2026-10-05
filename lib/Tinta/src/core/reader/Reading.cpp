#include "core/reader/Reading.h"

#include <cstring>

namespace tinta::core::reader {

namespace pk = pack;
using text::Span;

namespace {

bool startsParagraph(const pk::StoryLine& line, uint16_t index) {
  return index == 0 || (line.flags & pk::kLineNewParagraph) || line.speaker != 0;
}

uint8_t tokenCount(const pk::Pack& pack, const pk::StoryLine& line) {
  pk::Sentence sentence;
  return pack.sentence(line.sentence, sentence) ? sentence.tokenCount : 0;
}

}  // namespace

uint16_t paragraphs(const pk::Pack& pack, const pk::Story& story, Paragraph* out, const uint16_t cap) {
  uint16_t n = 0;
  uint16_t token = 0;
  for (uint16_t i = 0; i < story.lineCount; ++i) {
    pk::StoryLine line;
    if (!pack.storyLine(story, i, line)) break;
    if (startsParagraph(line, i) || n == 0) {
      if (n >= cap) break;
      out[n] = Paragraph{i, 0, token};
      ++n;
    }
    ++out[n - 1].lineCount;
    token = static_cast<uint16_t>(token + tokenCount(pack, line));
  }
  return n;
}

uint16_t paragraphSpans(const pk::Pack& pack, const pk::Story& story, const Paragraph& paragraph, const Fonts& fonts,
                        Span* spans, const uint16_t cap) {
  uint16_t n = 0;
  const auto add = [&](const char* text, uint16_t length, const text::BitmapFont* font, uint16_t token) {
    if (length == 0 || n >= cap) return;
    Span& s = spans[n++];
    s = Span{};
    s.text = text;
    s.length = length;
    s.font = font;
    s.token = token;
  };
  uint16_t token = paragraph.firstToken;
  for (uint16_t l = 0; l < paragraph.lineCount; ++l) {
    pk::StoryLine line;
    pk::Sentence sentence;
    if (!pack.storyLine(story, static_cast<uint16_t>(paragraph.firstLine + l), line)) break;
    if (!pack.sentence(line.sentence, sentence)) continue;
    if (l > 0) add(" ", 1, fonts.text, text::kNoToken);
    if (line.speaker) {
      const char* speaker = pack.str(line.speaker);
      add(speaker, static_cast<uint16_t>(std::strlen(speaker)), fonts.speaker, text::kNoToken);
      add(": ", 2, fonts.speaker, text::kNoToken);
    }
    const char* es = pack.str(sentence.es);
    const uint16_t length = static_cast<uint16_t>(std::strlen(es));
    uint16_t at = 0;
    for (uint8_t t = 0; t < sentence.tokenCount; ++t) {
      pk::Token tok;
      if (!pack.token(sentence, t, tok)) break;
      if (tok.start < at || tok.start + tok.length > length) continue;
      add(es + at, static_cast<uint16_t>(tok.start - at), fonts.text, text::kNoToken);
      add(es + tok.start, tok.length, fonts.text, static_cast<uint16_t>(token + t));
      at = static_cast<uint16_t>(tok.start + tok.length);
    }
    add(es + at, static_cast<uint16_t>(length - at), fonts.text, text::kNoToken);
    token = static_cast<uint16_t>(token + sentence.tokenCount);
  }
  return n;
}

bool findWord(const pk::Pack& pack, const pk::Story& story, const uint16_t token, WordRef& out) {
  uint16_t first = 0;
  for (uint16_t i = 0; i < story.lineCount; ++i) {
    pk::StoryLine line;
    pk::Sentence sentence;
    if (!pack.storyLine(story, i, line) || !pack.sentence(line.sentence, sentence)) return false;
    if (token < first + sentence.tokenCount) {
      out.line = i;
      out.sentence = line.sentence;
      out.token = static_cast<uint8_t>(token - first);
      return true;
    }
    first = static_cast<uint16_t>(first + sentence.tokenCount);
  }
  return false;
}

}  // namespace tinta::core::reader
