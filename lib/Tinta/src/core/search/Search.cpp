#include "core/search/Search.h"

#include <cstring>

namespace tinta::core::search {

namespace pk = pack;

// The longest run of keys nextLetters() reads looking for a space.
constexpr uint32_t kSpaceScan = 1024;

bool Search::add(const uint16_t lemma, const Match how, const uint32_t form) {
  for (uint8_t i = 0; i < count_; ++i) {
    if (results_[i].lemma == lemma) return true;
  }
  if (count_ >= kMaxResults) {
    more_ = true;
    return false;
  }
  results_[count_++] = Result{lemma, how, form};
  return true;
}

uint8_t Search::run(const pk::Pack& pack, const char* query) {
  count_ = 0;
  more_ = false;
  bool any = false;
  for (const char* p = query; p && *p; ++p) {
    if (*p != ' ' && *p != '\t') any = true;
  }
  if (!any) return 0;

  pk::KeyRange r = pack.findLemma(query);
  for (uint32_t i = 0; i < r.count; ++i) {
    pk::LemmaKey key;
    if (pack.lemmaKeyAt(r.first + i, key) && !add(key.lemma, Match::Headword)) return count_;
  }
  r = pack.findForm(query);
  for (uint32_t i = 0; i < r.count; ++i) {
    pk::FormKey key;
    if (pack.formAt(r.first + i, key) && !add(key.lemma, Match::Form, key.form)) return count_;
  }
  r = pack.findLemmaPrefix(query);
  for (uint32_t i = 0; i < r.count; ++i) {
    pk::LemmaKey key;
    if (pack.lemmaKeyAt(r.first + i, key) && !add(key.lemma, Match::Prefix)) return count_;
  }
  r = pack.findEnglishPrefix(query);
  for (uint32_t i = 0; i < r.count; ++i) {
    pk::EnglishKey key;
    if (pack.englishAt(r.first + i, key) && !add(key.lemma, Match::English)) return count_;
  }
  return count_;
}

uint32_t nextLetters(const pk::Pack& pack, const char* prefix, const bool english) {
  char probe[24];
  const size_t length = std::strlen(prefix);
  if (length + 2 > sizeof probe) return 0;
  std::memcpy(probe, prefix, length);
  probe[length + 1] = '\0';
  uint32_t bits = 0;
  // Every longer prefix lies inside the prefix's own range, which keeps the
  // 26 searches to a few cached blocks when the pack is on the card.
  const pk::KeyRange r = english ? pack.findEnglishPrefix(prefix) : pack.findLemmaPrefix(prefix);
  if (r.count == 0) return 0;
  for (uint8_t i = 0; i < 26; ++i) {
    probe[length] = static_cast<char>('a' + i);
    const pk::KeyRange sub = english ? pack.findEnglishPrefix(probe, r) : pack.findLemmaPrefix(probe, r);
    if (sub.count > 0) bits |= 1u << i;
  }
  // A space: folding drops a trailing one, so a prefix search cannot ask for
  // it. Look at the keys that start with the prefix instead (a few hundred at
  // most: the longest one-letter range).
  if (length == 0 || prefix[length - 1] == ' ') return bits;
  for (uint32_t i = 0; i < r.count && i < kSpaceScan; ++i) {
    uint32_t key = 0;
    if (english) {
      pk::EnglishKey k;
      if (!pack.englishAt(r.first + i, k)) break;
      key = k.key;
    } else {
      pk::LemmaKey k;
      if (!pack.lemmaKeyAt(r.first + i, k)) break;
      key = k.key;
    }
    char text[sizeof probe + 1];
    if (pack.copyStr(key, text, static_cast<uint32_t>(length + 2)) > length && text[length] == ' ') {
      bits |= 1u << kSpaceBit;
      break;
    }
  }
  return bits;
}

}  // namespace tinta::core::search
