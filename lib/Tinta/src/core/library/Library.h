#pragma once

#include <cstdint>

#include "core/pack/Pack.h"

namespace tinta::core::library {

// What the reader, the phrasebook and "add to my deck" look up in the pack
// (PLAN.md M6). Linear scans of flash-mapped tables: tens of microseconds per
// thousand records, run when a screen opens, never per frame.

// The readings, in pack order: stories of kind Reading (lesson dialogues are
// stories too, and are left out). Returns how many were written.
uint16_t readings(const pack::Pack& pack, uint16_t* out, uint16_t cap);

// A key for a story that survives a rebuild of the pack (story ids do not):
// FNV-1a of its Spanish title.
uint32_t storyKey(const pack::Pack& pack, const pack::Story& story);

// The VocabRecognise item of a lemma, or -1 (dictionary-only words have none).
int32_t recogniseItem(const pack::Pack& pack, uint16_t lemma);

// The phrase items of a phrasebook category, in phrasebook order, leaving out
// vulgar ones unless `showVulgar`. Returns how many were written.
uint16_t categoryItems(const pack::Pack& pack, uint16_t category, bool showVulgar, uint32_t* out, uint16_t cap);

// The lemma a word of a sentence stands for: its token's link, or for an
// unlinked token (a name, a number, a form the compiler did not link) the
// lemma whose headword or inflected form is spelled like it. kNone16 when
// there is none: digits, and words the dictionary does not have.
uint16_t tokenLemma(const pack::Pack& pack, const pack::Sentence& sentence, const pack::Token& token);

}  // namespace tinta::core::library
