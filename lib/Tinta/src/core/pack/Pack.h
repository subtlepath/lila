#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "core/ItemCatalog.h"
#include "core/pack/PackFormat.h"
#include "core/pack/PackSource.h"

namespace tinta::core::pack {

enum class PackStatus : uint8_t {
  Ok,
  TooSmall,  // shorter than a header
  BadMagic,
  BadVersion,       // formatMajor is not ours
  BadSize,          // header.size or headerSize disagrees with the data
  BadDirectory,     // directory misplaced, outside the pack, or a tag listed twice
  MissingSection,   // a format 1.x section is absent
  BadSection,       // a section misaligned, outside the pack, or not count x stride
  BadStrings,       // STRS empty or not NUL at both ends
  UnsupportedHost,  // big-endian host
};

// Indices [first, first + count) into a sorted key table (LKEY, EKEY, FORM).
struct KeyRange {
  uint32_t first = 0;
  uint32_t count = 0;
};

// Bytes inside a pack string; not NUL-terminated at `length`.
struct TextSpan {
  const char* text;
  uint32_t length;
};

// Read-only view of a course pack read through a PackSource (a file on the
// SD card, or memory); docs/pack-format.md is the contract. open() does only
// the cheap structural checks; after that every accessor bounds-checks its
// ids and offsets, so a pack that passed open() cannot make the reader touch
// bytes outside the source, however wrong its contents.
//
// Strings: from a source in memory, pointers into the pack. Otherwise copies
// in the string arena (setArena()), valid until the next beginPass(). The app
// begins a pass before handling each event and before each frame, so a string
// must not be kept across them: keep its offset and look it up again.
//
// Getters fill a caller struct and return false (with the struct zeroed) for
// an id out of range. Child records (tokens of a sentence, spans of a note,
// ...) can be read through their parent or by global index.
//
// About 750 bytes: keep the instance static or in the heap, not on the stack.
class Pack final : public ItemCatalog {
 public:
  // Folded queries longer than kMaxQuery - 1 bytes match nothing.
  static constexpr size_t kMaxQuery = 64;

  PackStatus open(PackSource& source);
  // A pack in memory (host tests).
  PackStatus open(const uint8_t* data, uint32_t size);
  void close();
  bool isOpen() const { return source_ != nullptr; }
  // Full CRC-32 over the pack (about 3 MB): call once, not per boot. False
  // when closed or on a read error.
  bool verifyCrc() const;
  uint32_t headerCrc() const { return header_.crc32; }

  // Memory for string copies from a source that is not in memory. Without
  // it, such a source's strings read as "".
  void setArena(char* buffer, uint32_t size);
  // Ends the life of every string handed out so far.
  void beginPass() const;
  // Strings that did not fit the arena (read as "") since open().
  uint32_t arenaOverflows() const { return arenaOverflows_; }
  // Most arena bytes one pass has used since open().
  uint32_t arenaPeak() const { return arenaPeak_; }
  // Tests: overwrite the arena at every beginPass(), so a string kept across
  // passes reads as garbage.
  void setPoison(bool on) { poison_ = on; }

  uint16_t formatMajor() const { return header_.formatMajor; }
  uint16_t formatMinor() const { return header_.formatMinor; }
  uint32_t contentVersion() const { return header_.contentVersion; }
  uint32_t buildTime() const { return header_.buildTime; }
  bool isRelease() const { return (header_.flags & kHeaderRelease) != 0; }
  const char* locale() const { return locale_; }
  // Records in a section (strings for Section::Strs); 0 when closed.
  uint32_t count(Section s) const { return sections_[static_cast<size_t>(s)].count; }

  // "" for offset 0 and for offsets outside STRS.
  const char* str(uint32_t offset) const;
  // Copies a string into out (at most cap - 1 bytes, NUL-terminated) without
  // using the arena; returns the bytes copied.
  uint32_t copyStr(uint32_t offset, char* out, uint32_t cap) const;
  // FNV-1a over a string's bytes, without using the arena.
  uint32_t hashStr(uint32_t offset) const;
  // Visits complete string bytes without the arena; false on read/callback failure.
  bool visitStr(uint32_t offset, void* context, bool (*visit)(void*, const uint8_t*, uint32_t)) const;

  bool lemma(uint16_t id, Lemma& out) const { return read(Section::Lemm, id, out); }
  // n-th example sentence id of a lemma, kNone16 past the end.
  uint16_t lemmaExample(const Lemma& lem, uint16_t n) const;

  bool verbInfo(uint16_t table, VerbInfo& out) const;
  // "" when the verb has no such form (imperative yo, a bad table index).
  const char* verbForm(uint16_t table, Tense tense, Person person) const;
  // Finite tags give the table cell (the enclitic bit is ignored: the bare
  // form), 0x2000-0x2002 the infinitive, gerund and participle; else "".
  const char* verbForm(uint16_t table, uint16_t tag) const;

  bool sentence(uint16_t id, Sentence& out) const { return read(Section::Sent, id, out); }
  bool token(uint32_t index, Token& out) const { return read(Section::Toks, index, out); }
  bool token(const Sentence& s, uint8_t i, Token& out) const {
    return readChild(Section::Toks, s.firstToken, s.tokenCount, i, out);
  }
  // The token's bytes in s.es, clamped to the string.
  TextSpan tokenText(const Sentence& s, const Token& t) const;

  bool item(uint32_t index, Item& out) const { return read(Section::Item, index, out); }
  // n-th distractor candidate (lemma id, sentence id or string offset by
  // kind), kNone32 past the end.
  uint32_t itemCandidate(const Item& it, uint8_t n) const;

  bool unit(uint16_t id, Unit& out) const { return read(Section::Unit, id, out); }
  bool lesson(uint16_t id, Lesson& out) const { return read(Section::Less, id, out); }

  bool note(uint16_t id, Note& out) const { return read(Section::Note, id, out); }
  bool noteSpan(uint32_t index, NoteSpan& out) const { return read(Section::Nspn, index, out); }
  bool noteSpan(const Note& n, uint16_t i, NoteSpan& out) const {
    return readChild(Section::Nspn, n.firstSpan, n.spanCount, i, out);
  }

  bool story(uint16_t id, Story& out) const { return read(Section::Stor, id, out); }
  bool storyLine(uint32_t index, StoryLine& out) const { return read(Section::Slin, index, out); }
  bool storyLine(const Story& s, uint16_t i, StoryLine& out) const {
    return readChild(Section::Slin, s.firstLine, s.lineCount, i, out);
  }
  bool question(uint32_t index, Question& out) const { return read(Section::Sqst, index, out); }
  bool question(const Story& s, uint8_t i, Question& out) const {
    return readChild(Section::Sqst, s.firstQuestion, s.questionCount, i, out);
  }

  bool phraseCategory(uint16_t id, PhraseCategory& out) const { return read(Section::Phrs, id, out); }
  bool phraseEntry(uint32_t index, PhraseEntry& out) const { return read(Section::Pent, index, out); }
  bool phraseEntry(const PhraseCategory& c, uint16_t i, PhraseEntry& out) const {
    return readChild(Section::Pent, c.firstEntry, c.entryCount, i, out);
  }

  bool confusable(uint16_t id, ConfusableSet& out) const { return read(Section::Conf, id, out); }

  // Lookups fold the query (Fold.h) and return the matching range of the key
  // table; ranges are empty for queries whose key is too long.
  //
  // LKEY: one key per lemma in alphabetical order, so the whole table is the
  // dictionary list and a prefix range serves search-as-you-type and the
  // letter-jump menu (an empty query matches every lemma).
  KeyRange findLemma(const char* query) const { return search(Section::Lkey, query, false); }
  KeyRange findLemmaPrefix(const char* query) const { return search(Section::Lkey, query, true); }
  // A prefix within a range already found (a longer prefix of the same keys).
  KeyRange findLemmaPrefix(const char* query, KeyRange within) const {
    return search(Section::Lkey, query, true, within);
  }
  bool lemmaKeyAt(uint32_t index, LemmaKey& out) const { return read(Section::Lkey, index, out); }
  // FORM: inflected forms and token surfaces (not headwords; those are in LKEY).
  KeyRange findForm(const char* query) const { return search(Section::Form, query, false); }
  bool formAt(uint32_t index, FormKey& out) const { return read(Section::Form, index, out); }
  // EKEY: English gloss keywords, rank 0 (whole sense) before rank 1 per key.
  KeyRange findEnglish(const char* query) const { return search(Section::Ekey, query, false); }
  KeyRange findEnglishPrefix(const char* query) const { return search(Section::Ekey, query, true); }
  KeyRange findEnglishPrefix(const char* query, KeyRange within) const {
    return search(Section::Ekey, query, true, within);
  }
  bool englishAt(uint32_t index, EnglishKey& out) const { return read(Section::Ekey, index, out); }

  // ItemCatalog. uidAt is 0, kindAt VocabRecognise and lessonAt kNone16 for
  // an index out of range.
  uint32_t itemCount() const override { return count(Section::Item); }
  uint32_t uidAt(uint32_t index) const override;
  int32_t indexOfUid(uint32_t uid) const override;
  ItemKind kindAt(uint32_t index) const override;
  uint16_t lessonAt(uint32_t index) const override;
  int32_t prerequisiteOf(uint32_t index) const override;

 private:
  struct View {
    uint32_t offset;
    uint32_t size;
    uint32_t count;
    uint32_t stride;
  };

  struct Memo {
    uint32_t offset;
    uint32_t at;
  };
  static constexpr uint8_t kMemoSize = 32;

  PackStatus validate();
  bool readAt(uint32_t offset, void* out, uint32_t len) const;
  // Compares the string at offset with key (n bytes, NUL-terminated): its
  // first n bytes when prefix, else the whole string.
  int compareKey(uint32_t offset, const char* key, uint32_t n, bool prefix) const;

  // Copies len bytes at byte `at` of record `index`; false (out zeroed) unless
  // the record exists and the bytes lie inside its stride.
  bool readBytes(Section s, uint32_t index, uint32_t at, void* out, uint32_t len) const;

  template <class T>
  bool read(Section s, uint32_t index, T& out) const {
    static_assert(std::is_trivially_copyable_v<T>);
    return readBytes(s, index, 0, &out, sizeof(T));
  }

  // Record `first + i` of a parent's run [first, first + n).
  template <class T>
  bool readChild(Section s, uint32_t first, uint32_t n, uint32_t i, T& out) const {
    if (i >= n || first > UINT32_MAX - i) {
      out = T{};
      return false;
    }
    return read(s, first + i, out);
  }

  KeyRange search(Section s, const char* query, bool prefix) const {
    return search(s, query, prefix, KeyRange{0, count(s)});
  }
  KeyRange search(Section s, const char* query, bool prefix, KeyRange within) const;
  uint32_t keyAt(Section s, uint32_t index) const;

  PackSource* source_ = nullptr;
  const uint8_t* data_ = nullptr;  // source_->data()
  uint32_t size_ = 0;
  MemorySource memory_;
  char* arena_ = nullptr;
  uint32_t arenaSize_ = 0;
  mutable uint32_t arenaUsed_ = 0;
  mutable uint32_t arenaPeak_ = 0;
  mutable uint32_t arenaOverflows_ = 0;
  mutable Memo memo_[kMemoSize] = {};
  mutable uint8_t memoCount_ = 0;
  mutable uint8_t memoNext_ = 0;
  bool poison_ = false;
  Header header_{};
  char locale_[sizeof(Header::locale) + 1] = {};
  View sections_[kSectionCount] = {};
};

}  // namespace tinta::core::pack
