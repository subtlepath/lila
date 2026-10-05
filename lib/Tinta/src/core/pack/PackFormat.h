#pragma once

#include <cstddef>
#include <cstdint>

#include "core/ItemCatalog.h"

namespace tinta::core::pack {

// Binary layout of the course pack, docs/pack-format.md (format 1.0). Field
// names and order follow the document; tools/packc/emit.py writes the same
// layouts. Every struct is padding-free and matches its on-disk size, so a
// record is read with one memcpy into a local (the pack base may be unaligned,
// and the C3 traps on some unaligned loads). Byte fields with a meaning are
// typed as enums with the same width; memcpy keeps any stored value.

inline constexpr uint16_t kFormatMajor = 1;
inline constexpr uint16_t kFormatMinor = 1;  // 1.1: deck sentences, dictionary-only lemmas
inline constexpr uint32_t kHeaderSize = 48;
inline constexpr char kMagic[4] = {'T', 'N', 'T', 'A'};

inline constexpr uint16_t kNone16 = 0xFFFF;      // "no id"
inline constexpr uint32_t kNone32 = 0xFFFFFFFF;  // "no candidate" (Pack::itemCandidate)

// ---- Enumerations (3.4) -----------------------------------------------------

enum class PartOfSpeech : uint8_t {
  Noun = 1,
  Verb = 2,
  Adjective = 3,
  Adverb = 4,
  Pronoun = 5,
  Determiner = 6,
  Preposition = 7,
  Conjunction = 8,
  Interjection = 9,
  Numeral = 10,
  Expression = 11,  // multi-word lemma
  ProperNoun = 12,
};

enum class Gender : uint8_t { None = 0, Masculine = 1, Feminine = 2, Both = 3 };
enum class Level : uint8_t { Unknown = 0, A1 = 1, A2 = 2, B1 = 3, B2 = 4 };  // Unknown: dictionary rows
enum class Register : uint8_t { Neutral = 0, Formal = 1, Informal = 2, Vulgar = 3 };

enum class Tense : uint8_t {
  Present = 0,
  Preterite = 1,
  Imperfect = 2,
  Future = 3,
  Conditional = 4,
  Subjunctive = 5,         // present subjunctive
  Imperative = 6,          // affirmative; persons Tu, Usted, Ustedes only
  NegativeImperative = 7,  // stored without "no"
};
inline constexpr uint8_t kTenseCount = 8;

// No vosotros: Mexican Spanish uses ustedes.
enum class Person : uint8_t {
  Yo = 0,
  Tu = 1,
  Usted = 2,  // él / ella / usted
  Nosotros = 3,
  Ustedes = 4,  // ellos / ellas / ustedes
};
inline constexpr uint8_t kPersonCount = 5;

// VerbTable.flags bits 1-3.
enum class StemChange : uint8_t { None = 0, EIe = 1, OUe = 2, EI = 3, UUe = 4, IIe = 5 };

enum class SentenceSource : uint8_t { Lesson = 0, Dialogue = 1, Story = 2, Phrasebook = 3, Deck = 4 };
enum class NoteKind : uint8_t { Grammar = 0, Culture = 1, Pronunciation = 2, Usage = 3 };
enum class StoryKind : uint8_t { Dialogue = 0, Reading = 1 };

// NoteSpan.style (3.10). Paragraph and Bullet are breaks and carry no text.
enum class SpanStyle : uint8_t {
  Text = 0,           // English body
  Strong = 1,         // English bold
  Spanish = 2,        // Times
  SpanishStrong = 3,  // Times Bold
  Respelling = 4,     // Helvetica Oblique
  NotMexican = 5,     // quoted for contrast; the UI marks it
  Paragraph = 6,
  Bullet = 7,  // paragraph break that starts a bullet item
};

// ---- Form tags (3.4) --------------------------------------------------------

inline constexpr uint16_t kTagBase = 0x0000;      // the headword itself, or unknown
inline constexpr uint16_t kTagEnclitic = 0x0100;  // finite form with attached pronouns
inline constexpr uint16_t kTagInfinitive = 0x2000;
inline constexpr uint16_t kTagGerund = 0x2001;
inline constexpr uint16_t kTagParticiple = 0x2002;  // masculine singular
inline constexpr uint16_t kTagParticipleFeminine = 0x2003;
inline constexpr uint16_t kTagParticipleMasculinePlural = 0x2004;
inline constexpr uint16_t kTagParticipleFemininePlural = 0x2005;
inline constexpr uint16_t kTagSingular = 0x3000;
inline constexpr uint16_t kTagPlural = 0x3001;
inline constexpr uint16_t kTagMasculine = 0x3010;
inline constexpr uint16_t kTagMasculinePlural = 0x3011;
inline constexpr uint16_t kTagFeminine = 0x3020;
inline constexpr uint16_t kTagFemininePlural = 0x3021;
inline constexpr uint16_t kTagApocope = 0x4001;  // buen, gran, primer

constexpr uint16_t makeVerbTag(Tense tense, Person person) {
  return static_cast<uint16_t>(0x1000 | static_cast<unsigned>(tense) << 4 | static_cast<unsigned>(person));
}
constexpr bool isVerbTag(uint16_t tag) { return (tag & 0xF000) == 0x1000; }
constexpr bool isNonFiniteTag(uint16_t tag) { return (tag & 0xF000) == 0x2000; }
constexpr bool isNominalTag(uint16_t tag) { return (tag & 0xF000) == 0x3000; }
constexpr bool hasEnclitic(uint16_t tag) { return isVerbTag(tag) && (tag & kTagEnclitic) != 0; }
// Only meaningful when isVerbTag(tag); a corrupt tag can yield values past the
// last enumerator, which Pack::verbForm rejects.
constexpr Tense tagTense(uint16_t tag) { return static_cast<Tense>((tag >> 4) & 0xF); }
constexpr Person tagPerson(uint16_t tag) { return static_cast<Person>(tag & 0xF); }

// ---- Flags ------------------------------------------------------------------

inline constexpr uint32_t kHeaderRelease = 1u << 0;  // built with --release

inline constexpr uint16_t kLemmaMexico = 1u << 0;
inline constexpr uint16_t kLemmaPluralOnly = 1u << 1;
inline constexpr uint16_t kLemmaElFeminine = 1u << 2;  // el agua
inline constexpr uint16_t kLemmaHasItems = 1u << 3;
inline constexpr uint16_t kLemmaFrequencyDeck = 1u << 4;
inline constexpr uint16_t kLemmaConfusable = 1u << 5;
inline constexpr uint16_t kLemmaDictionaryOnly = 1u << 6;  // lookup only: no items, never an option

inline constexpr uint16_t kVerbIrregular = 1u << 0;
inline constexpr uint16_t kVerbSpelling = 1u << 4;  // busqué
inline constexpr uint16_t kVerbReflexive = 1u << 5;
constexpr StemChange verbStemChange(uint16_t flags) { return static_cast<StemChange>((flags >> 1) & 0x7); }

inline constexpr uint8_t kSentenceHasTarget = 1u << 0;
inline constexpr uint8_t kSentenceWordOrder = 1u << 1;

inline constexpr uint8_t kTokenTarget = 1u << 0;
inline constexpr uint8_t kTokenMultiword = 1u << 1;
inline constexpr uint8_t kTokenNumber = 1u << 2;  // digits, no lemma
inline constexpr uint8_t kTokenName = 1u << 3;

inline constexpr uint8_t kItemMustShowMask = 0x03;  // leading candidates always shown
inline constexpr uint8_t kItemFrequencyDeck = 1u << 2;
inline constexpr uint8_t kItemPhrasebook = 1u << 3;

inline constexpr uint8_t kLessonReviewed = 1u << 0;
inline constexpr uint16_t kLineNewParagraph = 1u << 0;
inline constexpr uint8_t kQuestionSpanish = 1u << 0;

// ---- Header and directory (3.1, 3.2) ----------------------------------------

struct Header {
  char magic[4];
  uint16_t formatMajor;
  uint16_t formatMinor;
  uint32_t contentVersion;
  uint32_t buildTime;  // Unix seconds, UTC
  uint32_t size;       // whole pack
  uint32_t crc32;      // of the whole pack with this field read as zero
  char locale[8];      // NUL-padded, not necessarily terminated
  uint32_t directoryOffset;
  uint16_t sectionCount;
  uint16_t headerSize;
  uint32_t flags;
  uint32_t reserved;
};

struct DirEntry {
  uint32_t tag;     // four ASCII characters, first in the lowest byte
  uint32_t offset;  // from the start of the pack
  uint32_t size;    // payload bytes, padding excluded
  uint32_t count;   // records; strings for STRS
};

// ---- Records (3.5 - 3.12) ---------------------------------------------------
// String fields are u32 offsets into STRS (0 = empty / none). Ids are u16
// indices into their section (kNone16 = none).

struct Lemma {
  uint32_t es;
  uint32_t en;
  uint32_t pron;
  uint32_t note;
  uint32_t alt;
  uint32_t feminine;
  uint32_t plural;
  uint32_t firstExample;  // LEXS
  uint16_t exampleCount;
  uint16_t freqRank;  // 1 = most frequent, 0 = unranked
  uint16_t lesson;
  uint16_t verbTable;
  PartOfSpeech pos;
  Gender gender;
  Level level;
  Register reg;
  uint16_t flags;
  uint16_t reserved;
};

struct LemmaKey {
  uint32_t key;
  uint16_t lemma;
  uint16_t reserved;
};

struct EnglishKey {
  uint32_t key;
  uint16_t lemma;
  uint8_t rank;  // 0 whole sense, 1 one word of a multi-word sense
  uint8_t reserved;
};

struct FormKey {
  uint32_t key;
  uint32_t form;  // exact spelling for display
  uint16_t lemma;
  uint16_t tag;
};

struct VerbTable {
  uint32_t forms[kTenseCount][kPersonCount];  // 0 = no such form
  uint32_t infinitive;
  uint32_t gerund;
  uint32_t participle;  // masculine singular
  uint16_t lemma;
  uint16_t flags;
};

// The tail of a VerbTable, read without the 160-byte form grid.
struct VerbInfo {
  uint32_t infinitive;
  uint32_t gerund;
  uint32_t participle;
  uint16_t lemma;
  uint16_t flags;
};

struct Sentence {
  uint32_t es;
  uint32_t en;
  uint32_t note;
  uint32_t firstToken;  // TOKS
  uint16_t lesson;
  uint8_t tokenCount;
  Level level;
  SentenceSource source;
  uint8_t flags;
  Register reg;  // highest register among its lemmas
  uint8_t reserved;
};

struct Token {
  uint16_t start;  // byte offset in Sentence.es
  uint8_t length;  // bytes
  uint8_t flags;
  uint16_t lemma;  // kNone16 for numbers
  uint16_t tag;
};

struct Item {
  uint32_t uid;
  ItemKind kind;
  uint8_t candidateCount;
  uint16_t lesson;
  uint16_t a;  // operands by kind, 3.9
  uint16_t b;
  uint16_t prereq;  // item index, kNone16 if none
  uint8_t flags;
  uint8_t reserved;
  uint32_t firstCandidate;  // DIST
};

struct ItemUid {
  uint32_t uid;
  uint16_t index;
  uint16_t reserved;
};

struct Unit {
  uint32_t title;
  uint32_t titleEn;
  uint32_t goals;  // one goal per line
  uint16_t number;
  uint16_t firstLesson;
  uint16_t lessonCount;
  uint16_t reserved;
};

struct Lesson {
  uint32_t title;
  uint32_t titleEn;
  uint32_t firstItem;
  uint16_t itemCount;
  uint16_t unit;
  uint16_t number;
  uint16_t firstNote;
  uint16_t noteCount;
  uint16_t dialogue;  // story id, kNone16 if none
  uint16_t firstSentence;
  uint16_t sentenceCount;
  uint16_t newCount;
  Level level;
  uint8_t flags;
};

struct Note {
  uint32_t title;
  uint32_t firstSpan;  // NSPN
  uint16_t spanCount;
  uint16_t lesson;
  NoteKind kind;
  uint8_t flags;
  uint16_t reserved;
};

struct NoteSpan {
  uint32_t text;  // 0 for break styles
  SpanStyle style;
  uint8_t flags;
  uint16_t reserved;
};

struct Story {
  uint32_t title;
  uint32_t titleEn;
  uint32_t firstLine;  // SLIN
  uint16_t lineCount;
  uint16_t lesson;
  uint16_t firstQuestion;  // SQST
  uint8_t questionCount;
  Level level;
  StoryKind kind;
  uint8_t flags;
  uint16_t reserved;
};

struct StoryLine {
  uint32_t speaker;  // 0 = narration
  uint16_t sentence;
  uint16_t flags;
};

struct Question {
  uint32_t text;
  uint32_t options[4];  // 0 = unused
  uint8_t answer;
  uint8_t optionCount;
  uint8_t flags;
  uint8_t reserved;
};

struct PhraseCategory {
  uint32_t title;
  uint32_t titleEn;
  uint16_t firstEntry;  // PENT
  uint16_t entryCount;
};

struct PhraseEntry {
  uint32_t pron;
  uint16_t sentence;
  uint16_t category;
};

struct ConfusableSet {
  uint32_t name;
  uint32_t note;
  uint16_t members[6];  // lemma ids, unused slots kNone16
  uint16_t memberCount;
  uint16_t reserved;
};

static_assert(sizeof(Header) == 48 && offsetof(Header, crc32) == 20 && offsetof(Header, locale) == 24 &&
              offsetof(Header, directoryOffset) == 32 && offsetof(Header, headerSize) == 38 &&
              offsetof(Header, flags) == 40);
static_assert(sizeof(DirEntry) == 16);
static_assert(sizeof(Lemma) == 48 && offsetof(Lemma, firstExample) == 28 && offsetof(Lemma, verbTable) == 38 &&
              offsetof(Lemma, pos) == 40 && offsetof(Lemma, flags) == 44);
static_assert(sizeof(LemmaKey) == 8 && offsetof(LemmaKey, lemma) == 4);
static_assert(sizeof(EnglishKey) == 8 && offsetof(EnglishKey, rank) == 6);
static_assert(sizeof(FormKey) == 12 && offsetof(FormKey, lemma) == 8 && offsetof(FormKey, tag) == 10);
static_assert(sizeof(VerbTable) == 176 && offsetof(VerbTable, infinitive) == 160 && offsetof(VerbTable, lemma) == 172 &&
              offsetof(VerbTable, flags) == 174);
static_assert(sizeof(VerbInfo) == sizeof(VerbTable) - offsetof(VerbTable, infinitive));
static_assert(sizeof(Sentence) == 24 && offsetof(Sentence, lesson) == 16 && offsetof(Sentence, tokenCount) == 18 &&
              offsetof(Sentence, reg) == 22);
static_assert(sizeof(Token) == 8 && offsetof(Token, lemma) == 4 && offsetof(Token, tag) == 6);
static_assert(sizeof(Item) == 20 && offsetof(Item, kind) == 4 && offsetof(Item, prereq) == 12 &&
              offsetof(Item, flags) == 14 && offsetof(Item, firstCandidate) == 16);
static_assert(sizeof(ItemUid) == 8 && offsetof(ItemUid, index) == 4);
static_assert(sizeof(Unit) == 20 && offsetof(Unit, number) == 12);
static_assert(sizeof(Lesson) == 32 && offsetof(Lesson, itemCount) == 12 && offsetof(Lesson, dialogue) == 22 &&
              offsetof(Lesson, newCount) == 28 && offsetof(Lesson, level) == 30);
static_assert(sizeof(Note) == 16 && offsetof(Note, spanCount) == 8 && offsetof(Note, kind) == 12);
static_assert(sizeof(NoteSpan) == 8 && offsetof(NoteSpan, style) == 4);
static_assert(sizeof(Story) == 24 && offsetof(Story, lineCount) == 12 && offsetof(Story, firstQuestion) == 16 &&
              offsetof(Story, kind) == 20);
static_assert(sizeof(StoryLine) == 8 && offsetof(StoryLine, sentence) == 4);
static_assert(sizeof(Question) == 24 && offsetof(Question, answer) == 20);
static_assert(sizeof(PhraseCategory) == 12 && offsetof(PhraseCategory, firstEntry) == 8);
static_assert(sizeof(PhraseEntry) == 8 && offsetof(PhraseEntry, sentence) == 4);
static_assert(sizeof(ConfusableSet) == 24 && offsetof(ConfusableSet, members) == 8 &&
              offsetof(ConfusableSet, memberCount) == 20);

// ---- Sections (3.3) ---------------------------------------------------------

constexpr uint32_t makeTag(const char (&name)[5]) {
  return static_cast<uint32_t>(static_cast<uint8_t>(name[0])) |
         static_cast<uint32_t>(static_cast<uint8_t>(name[1])) << 8 |
         static_cast<uint32_t>(static_cast<uint8_t>(name[2])) << 16 |
         static_cast<uint32_t>(static_cast<uint8_t>(name[3])) << 24;
}
static_assert(makeTag("LEMM") == 0x4D4D454C);

// Every section of format 1.x, all required. Indexes kSections.
enum class Section : uint8_t {
  Lemm,
  Lkey,
  Ekey,
  Form,
  Verb,
  Sent,
  Toks,
  Item,
  Iuid,
  Dist,
  Unit,
  Less,
  Note,
  Nspn,
  Stor,
  Slin,
  Sqst,
  Phrs,
  Pent,
  Conf,
  Lexs,
  Strs,
};
inline constexpr size_t kSectionCount = 22;

struct SectionInfo {
  uint32_t tag;
  uint32_t recordSize;  // minimum stride; 0 for STRS, which is a byte heap
};

inline constexpr SectionInfo kSections[kSectionCount] = {
    {makeTag("LEMM"), sizeof(Lemma)},       {makeTag("LKEY"), sizeof(LemmaKey)},
    {makeTag("EKEY"), sizeof(EnglishKey)},  {makeTag("FORM"), sizeof(FormKey)},
    {makeTag("VERB"), sizeof(VerbTable)},   {makeTag("SENT"), sizeof(Sentence)},
    {makeTag("TOKS"), sizeof(Token)},       {makeTag("ITEM"), sizeof(Item)},
    {makeTag("IUID"), sizeof(ItemUid)},     {makeTag("DIST"), sizeof(uint32_t)},
    {makeTag("UNIT"), sizeof(Unit)},        {makeTag("LESS"), sizeof(Lesson)},
    {makeTag("NOTE"), sizeof(Note)},        {makeTag("NSPN"), sizeof(NoteSpan)},
    {makeTag("STOR"), sizeof(Story)},       {makeTag("SLIN"), sizeof(StoryLine)},
    {makeTag("SQST"), sizeof(Question)},    {makeTag("PHRS"), sizeof(PhraseCategory)},
    {makeTag("PENT"), sizeof(PhraseEntry)}, {makeTag("CONF"), sizeof(ConfusableSet)},
    {makeTag("LEXS"), sizeof(uint16_t)},    {makeTag("STRS"), 0},
};
static_assert(static_cast<size_t>(Section::Strs) + 1 == kSectionCount);

constexpr const SectionInfo& sectionInfo(Section s) { return kSections[static_cast<size_t>(s)]; }

}  // namespace tinta::core::pack
