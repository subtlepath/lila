#pragma once

#include <cstdint>

#include "core/pack/Pack.h"
#include "core/text/Typesetter.h"

namespace tinta::core::reader {

// A story as typesetter input (PLAN.md 4.5 reader, M6). The text is laid out a
// paragraph at a time, so one small span buffer serves any story: a paragraph
// is a run of lines that starts at a line flagged NEW_PARAGRAPH, at the first
// line, or at any line with a speaker. Lines in a paragraph flow on, one
// sentence after another; a speaker's line starts with the name in bold.
//
// Every word is a span of its own whose `token` is the word's number in the
// story (line by line, Token order), so a page knows where each word it drew
// sits and the gloss knows which Token it is. Punctuation and spaces between
// words are spans without a token. Span text points into the pack: nothing
// is copied.

struct Paragraph {
  uint16_t firstLine = 0;
  uint16_t lineCount = 0;
  uint16_t firstToken = 0;  // story-wide number of its first word
};

struct Fonts {
  const text::BitmapFont* text = nullptr;
  const text::BitmapFont* speaker = nullptr;
};

// The paragraphs of a story, in order. Returns how many (at most cap).
uint16_t paragraphs(const pack::Pack& pack, const pack::Story& story, Paragraph* out, uint16_t cap);

// The spans of one paragraph. Returns how many were written; a paragraph that
// needs more than `cap` is cut at a word (the rest is not shown).
uint16_t paragraphSpans(const pack::Pack& pack, const pack::Story& story, const Paragraph& paragraph,
                        const Fonts& fonts, text::Span* spans, uint16_t cap);

// Where word number `token` of the story is: its line and its index in that
// line's sentence. False past the last word.
struct WordRef {
  uint16_t line = 0;
  uint16_t sentence = pack::kNone16;
  uint8_t token = 0;
};
bool findWord(const pack::Pack& pack, const pack::Story& story, uint16_t token, WordRef& out);

}  // namespace tinta::core::reader
