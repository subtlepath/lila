#include "core/session/Exercise.h"

#include <cstdio>
#include <cstring>

namespace tinta::core::session {
namespace {

namespace pk = core::pack;

// Candidates beyond the best few are weaker distractors; the generator draws
// from this many before falling back to the rest.
constexpr uint8_t kDrawPool = 6;

uint32_t nextRandom(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

// Case-insensitive for ASCII; Latin-1 letters compare as written.
bool sameText(const char* a, const char* b) {
  for (;; ++a, ++b) {
    char x = *a;
    char y = *b;
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
    if (x != y) return false;
    if (x == '\0') return true;
  }
}

bool spanEquals(const pk::TextSpan& a, const pk::TextSpan& b) {
  if (a.length != b.length) return false;
  for (uint32_t i = 0; i < a.length; ++i) {
    char x = a.text[i];
    char y = b.text[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
    if (x != y) return false;
  }
  return true;
}

// The article a noun takes in the singular (or, plural-only, the plural):
// 1 el / los, 2 la / las, 0 when the noun has no single gender.
uint8_t articleOf(const pk::Lemma& lemma) {
  if (lemma.gender == pk::Gender::Masculine) return 1;
  if (lemma.gender == pk::Gender::Feminine) {
    const bool plural = (lemma.flags & pk::kLemmaPluralOnly) != 0;
    return !plural && (lemma.flags & pk::kLemmaElFeminine) ? 1 : 2;
  }
  return 0;
}

}  // namespace

bool isChoice(const Format f) {
  return f == Format::ChooseMeaning || f == Format::ChooseWord || f == Format::ChooseGap ||
         f == Format::ChooseArticle || f == Format::ChooseForm;
}

bool isTyped(const Format f) { return f == Format::TypeWord || f == Format::TypeGap || f == Format::TypeForm; }

uint32_t showingSeed(const uint32_t uid, const uint8_t reps) {
  uint32_t s = uid * 2654435761u ^ (static_cast<uint32_t>(reps) + 1u) * 0x9E3779B9u;
  return s ? s : 0x6D2B79F5u;
}

Format pickFormat(const pk::Item& item, const uint32_t uid, const History& history, const bool typing) {
  const bool mature = !history.isNew && history.stabilityDays >= kMatureDays;
  const bool options = item.candidateCount > 0;
  switch (item.kind) {
    case ItemKind::VocabRecognise:
      if (mature || !options) return Format::Flashcard;
      // Once out of the first steps, one showing in three is a flashcard, so
      // the meaning is also recalled, not only recognised among options.
      if (history.reps >= 3 && showingSeed(uid, history.reps) % 3 == 0) return Format::Flashcard;
      return Format::ChooseMeaning;
    case ItemKind::VocabProduce:
      if (typing) return Format::TypeWord;
      if (mature || !options) return Format::Flashcard;
      return Format::ChooseWord;
    case ItemKind::Cloze:
      if (typing) return Format::TypeGap;
      return options ? Format::ChooseGap : Format::Flashcard;
    case ItemKind::Conjugation:
      if (typing) return Format::TypeForm;
      return options ? Format::ChooseForm : Format::Flashcard;
    case ItemKind::Gender:
      return Format::ChooseArticle;
    case ItemKind::WordOrder:
      return Format::BuildSentence;
    case ItemKind::Phrase:
    default:
      return Format::Flashcard;
  }
}

Format resolveFormat(const pk::Pack& pack, const pk::Item& item, const History& history, const bool typing,
                     const bool showVulgar) {
  const Format format = pickFormat(item, item.uid, history, typing);
  if (isChoice(format)) {
    OptionSet options;
    return pickOptions(pack, item, format, item.uid, history.reps, showVulgar, options) ? format : Format::Flashcard;
  }
  if (format == Format::BuildSentence) {
    pk::Sentence sentence;
    WordOrder order;
    return pack.sentence(item.a, sentence) && order.begin(pack, sentence, showingSeed(item.uid, history.reps))
               ? format
               : Format::Flashcard;
  }
  if (isTyped(format)) {
    char expected[64];
    typedAnswer(pack, item, format, expected, sizeof expected);
    return expected[0] ? format : Format::Flashcard;
  }
  return format;
}

bool vulgarItem(const pk::Pack& pack, const pk::Item& item) {
  pk::Lemma lemma;
  pk::Sentence sentence;
  switch (item.kind) {
    case ItemKind::VocabRecognise:
    case ItemKind::VocabProduce:
    case ItemKind::Gender:
    case ItemKind::Conjugation:
      return pack.lemma(item.a, lemma) && lemma.reg == pk::Register::Vulgar;
    case ItemKind::Cloze:
    case ItemKind::WordOrder:
    case ItemKind::Phrase:
      return pack.sentence(item.a, sentence) && sentence.reg == pk::Register::Vulgar;
  }
  return false;
}

void optionText(const pk::Pack& pack, const pk::Item& item, const Format format, const Option& option, char* out,
                const size_t cap) {
  if (cap == 0) return;
  out[0] = '\0';
  pk::Lemma lemma;
  switch (option.kind) {
    case OptionKind::Lemma:
      if (pack.lemma(static_cast<uint16_t>(option.value), lemma)) {
        snprintf(out, cap, "%s", pack.str(format == Format::ChooseWord ? lemma.es : lemma.en));
      }
      break;
    case OptionKind::String:
      snprintf(out, cap, "%s", pack.str(option.value));
      break;
    case OptionKind::Token: {
      pk::Sentence sentence;
      pk::Token token;
      if (pack.sentence(item.a, sentence) && pack.token(sentence, static_cast<uint8_t>(option.value), token)) {
        const pk::TextSpan t = pack.tokenText(sentence, token);
        snprintf(out, cap, "%.*s", static_cast<int>(t.length), t.text);
      }
      break;
    }
    case OptionKind::Form:
      if (pack.lemma(item.a, lemma)) snprintf(out, cap, "%s", pack.verbForm(lemma.verbTable, option.value));
      break;
    case OptionKind::Article: {
      const bool plural = pack.lemma(item.a, lemma) && (lemma.flags & pk::kLemmaPluralOnly);
      snprintf(out, cap, "%s", option.value == 1 ? (plural ? "los" : "el") : (plural ? "las" : "la"));
      break;
    }
  }
}

void typedAnswer(const pk::Pack& pack, const pk::Item& item, const Format format, char* out, const size_t cap) {
  Option answer;
  switch (format) {
    case Format::TypeWord:
      answer = Option{OptionKind::Lemma, item.a};
      optionText(pack, item, Format::ChooseWord, answer, out, cap);
      return;
    case Format::TypeGap:
      answer = Option{OptionKind::Token, item.b};
      break;
    case Format::TypeForm:
      answer = Option{OptionKind::Form, item.b};
      break;
    default:
      if (cap) out[0] = '\0';
      return;
  }
  optionText(pack, item, format, answer, out, cap);
}

bool pickOptions(const pk::Pack& pack, const pk::Item& item, const Format format, const uint32_t uid,
                 const uint8_t reps, const bool showVulgar, OptionSet& out) {
  out = OptionSet{};
  uint32_t seed = showingSeed(uid, reps);
  char taken[OptionSet::kMax][40];

  // The answer first; its slot is shuffled at the end.
  Option answer;
  switch (format) {
    case Format::ChooseMeaning:
    case Format::ChooseWord:
      answer = Option{OptionKind::Lemma, item.a};
      break;
    case Format::ChooseGap:
      answer = Option{OptionKind::Token, item.b};
      break;
    case Format::ChooseForm:
      answer = Option{OptionKind::Form, item.b};
      break;
    case Format::ChooseArticle: {
      pk::Lemma lemma;
      const uint8_t article = pack.lemma(item.a, lemma) ? articleOf(lemma) : 0;
      if (article == 0) return false;
      // Always el (los) first, then la (las): the two keys never swap.
      out.options[0] = Option{OptionKind::Article, 1};
      out.options[1] = Option{OptionKind::Article, 2};
      out.count = 2;
      out.answer = static_cast<uint8_t>(article - 1);
      return true;
    }
    default:
      return false;
  }
  out.options[out.count] = answer;
  optionText(pack, item, format, answer, taken[out.count], sizeof taken[0]);
  if (!taken[0][0]) return false;
  ++out.count;

  const auto usable = [&](uint32_t candidate, Option& option) {
    if (format == Format::ChooseMeaning || format == Format::ChooseWord) {
      pk::Lemma lemma;
      if (!pack.lemma(static_cast<uint16_t>(candidate), lemma)) return false;
      if (lemma.flags & pk::kLemmaDictionaryOnly) return false;
      if (!showVulgar && lemma.reg == pk::Register::Vulgar) return false;
      option = Option{OptionKind::Lemma, candidate};
    } else {
      option = Option{OptionKind::String, candidate};
    }
    char text[40];
    optionText(pack, item, format, option, text, sizeof text);
    if (!text[0]) return false;
    for (uint8_t k = 0; k < out.count; ++k) {
      if (sameText(text, taken[k])) return false;
    }
    std::memcpy(taken[out.count], text, sizeof text);
    return true;
  };
  const auto take = [&](uint32_t candidate) {
    Option option;
    if (out.count < OptionSet::kMax && usable(candidate, option)) out.options[out.count++] = option;
  };

  const uint8_t total = item.candidateCount;
  const uint8_t must =
      static_cast<uint8_t>((item.flags & pk::kItemMustShowMask) < total ? (item.flags & pk::kItemMustShowMask) : total);
  for (uint8_t i = 0; i < must; ++i) take(pack.itemCandidate(item, i));

  // The rest: a seeded shuffle of the best few, then the others in rank order.
  uint8_t pool[kDrawPool];
  uint8_t poolSize = 0;
  for (uint8_t i = must; i < total && poolSize < kDrawPool; ++i) pool[poolSize++] = i;
  for (uint8_t i = poolSize; i > 1; --i) {
    const uint8_t j = static_cast<uint8_t>(nextRandom(seed) % i);
    const uint8_t swap = pool[i - 1];
    pool[i - 1] = pool[j];
    pool[j] = swap;
  }
  for (uint8_t i = 0; i < poolSize; ++i) take(pack.itemCandidate(item, pool[i]));
  for (uint8_t i = static_cast<uint8_t>(must + poolSize); i < total; ++i) take(pack.itemCandidate(item, i));
  if (out.count < 2) return false;

  // Shuffle every option, the answer included.
  for (uint8_t i = out.count; i > 1; --i) {
    const uint8_t j = static_cast<uint8_t>(nextRandom(seed) % i);
    const Option swap = out.options[i - 1];
    out.options[i - 1] = out.options[j];
    out.options[j] = swap;
    if (out.answer == i - 1) {
      out.answer = j;
    } else if (out.answer == j) {
      out.answer = static_cast<uint8_t>(i - 1);
    }
  }
  return true;
}

// ---- Word order --------------------------------------------------------------

bool WordOrder::begin(const pk::Pack& pack, const pk::Sentence& sentence, const uint32_t seed) {
  *this = WordOrder{};
  if (sentence.tokenCount < 2 || sentence.tokenCount > kMaxTiles) return false;
  const uint32_t textLength = pack.copyStr(sentence.es, text_, sizeof text_);
  for (uint8_t i = 0; i < sentence.tokenCount; ++i) {
    pk::Token token;
    if (!pack.token(sentence, i, token)) return false;
    const uint32_t start = token.start < textLength ? token.start : textLength;
    const uint32_t room = textLength - start;
    start_[i] = static_cast<uint8_t>(start);
    length_[i] = static_cast<uint8_t>(token.length < room ? token.length : room);
    tray_[i] = i;
  }
  count_ = sentence.tokenCount;
  uint32_t state = seed ? seed : 1;
  for (uint8_t i = count_; i > 1; --i) {
    const uint8_t j = static_cast<uint8_t>(nextRandom(state) % i);
    const uint8_t swap = tray_[i - 1];
    tray_[i - 1] = tray_[j];
    tray_[j] = swap;
  }
  // A tray already in order would give the answer away: rotate it by one.
  bool inOrder = true;
  for (uint8_t i = 0; i < count_ && inOrder; ++i) inOrder = sameText(i, i);
  if (inOrder) {
    const uint8_t first = tray_[0];
    for (uint8_t i = 0; i + 1 < count_; ++i) tray_[i] = tray_[i + 1];
    tray_[count_ - 1] = first;
  }
  return true;
}

bool WordOrder::sameText(const uint8_t tile, const uint8_t word) const {
  const uint8_t a = tray_[tile];
  return spanEquals(pk::TextSpan{text_ + start_[a], length_[a]}, pk::TextSpan{text_ + start_[word], length_[word]});
}

pk::TextSpan WordOrder::tile(const uint8_t i) const {
  if (i >= count_) return pk::TextSpan{"", 0};
  const uint8_t w = tray_[i];
  return pk::TextSpan{text_ + start_[w], length_[w]};
}

bool WordOrder::pick(const uint8_t i) {
  if (i >= count_ || placed(i) || done()) return false;
  if (!sameText(i, next_)) {
    ++mistakes_;
    return false;
  }
  placedMask_ = static_cast<uint16_t>(placedMask_ | (1u << i));
  order_[next_++] = i;
  return true;
}

}  // namespace tinta::core::session
