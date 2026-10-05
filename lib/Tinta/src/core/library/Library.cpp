#include "core/library/Library.h"

#include <cstring>

#include "core/session/Exercise.h"

namespace tinta::core::library {

namespace pk = pack;

uint16_t readings(const pk::Pack& pack, uint16_t* out, const uint16_t cap) {
  uint16_t n = 0;
  const uint32_t count = pack.count(pk::Section::Stor);
  for (uint32_t i = 0; i < count && n < cap; ++i) {
    pk::Story story;
    if (pack.story(static_cast<uint16_t>(i), story) && story.kind == pk::StoryKind::Reading) {
      out[n++] = static_cast<uint16_t>(i);
    }
  }
  return n;
}

uint32_t storyKey(const pk::Pack& pack, const pk::Story& story) {
  const uint32_t h = pack.hashStr(story.title);
  // 0 is "no key" to callers.
  return h ? h : 1u;
}

int32_t recogniseItem(const pk::Pack& pack, const uint16_t lemma) {
  pk::Lemma record;
  if (!pack.lemma(lemma, record) || !(record.flags & pk::kLemmaHasItems)) return -1;
  const uint32_t count = pack.itemCount();
  for (uint32_t i = 0; i < count; ++i) {
    pk::Item item;
    if (pack.item(i, item) && item.kind == ItemKind::VocabRecognise && item.a == lemma) {
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}

uint16_t categoryItems(const pk::Pack& pack, const uint16_t category, const bool showVulgar, uint32_t* out,
                       const uint16_t cap) {
  uint16_t n = 0;
  const uint32_t count = pack.itemCount();
  for (uint32_t i = 0; i < count && n < cap; ++i) {
    pk::Item item;
    if (!pack.item(i, item) || item.kind != ItemKind::Phrase) continue;
    pk::PhraseEntry entry;
    if (!pack.phraseEntry(item.b, entry) || entry.category != category) continue;
    if (!showVulgar && session::vulgarItem(pack, item)) continue;
    out[n++] = i;
  }
  return n;
}

uint16_t tokenLemma(const pk::Pack& pack, const pk::Sentence& sentence, const pk::Token& token) {
  if (token.lemma != pk::kNone16) return token.lemma;
  if (token.flags & pk::kTokenNumber) return pk::kNone16;
  const pk::TextSpan text = pack.tokenText(sentence, token);
  char word[48];
  if (text.length == 0 || text.length >= sizeof word) return pk::kNone16;
  std::memcpy(word, text.text, text.length);
  word[text.length] = '\0';
  pk::KeyRange r = pack.findLemma(word);
  if (r.count > 0) {
    pk::LemmaKey key;
    if (pack.lemmaKeyAt(r.first, key)) return key.lemma;
  }
  r = pack.findForm(word);
  if (r.count > 0) {
    pk::FormKey key;
    if (pack.formAt(r.first, key)) return key.lemma;
  }
  return pk::kNone16;
}

}  // namespace tinta::core::library
