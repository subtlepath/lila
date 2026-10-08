#pragma once

#include <algorithm>
#include <span>
#include <string_view>

#include "CompanionCourseItemIdentities.h"
#include "CompanionCourseStoryIdentities.h"
#include "core/pack/Pack.h"

namespace companion {
enum class CourseValidationResult { Ok, InvalidStructure, Integrity, InvalidIdentity, InvalidContent };
inline bool validCourseLocale(std::string_view locale) {
  if (locale.empty() || locale.size() > 8) return false;
  unsigned component = 0;
  bool primary = true;
  for (const unsigned char byte : locale) {
    if (byte == '-') {
      if (!component) return false;
      component = 0;
      primary = false;
    } else {
      if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (!primary && byte >= '0' && byte <= '9')))
        return false;
      ++component;
    }
  }
  return component != 0;
}
inline bool validCourseUtf8(tinta::core::pack::PackSource& source, uint32_t offset, uint32_t size) {
  if (offset > source.size() || size > source.size() - offset) return false;
  uint8_t buffer[128];
  uint8_t remaining = 0;
  uint32_t codepoint = 0, minimum = 0, consumed = 0;
  while (consumed < size) {
    const uint32_t count = size - consumed < sizeof(buffer) ? size - consumed : sizeof(buffer);
    if (!source.read(offset + consumed, buffer, count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
      const uint8_t byte = buffer[index];
      if (remaining != 0) {
        if ((byte & 0xc0) != 0x80) return false;
        codepoint = (codepoint << 6) | (byte & 0x3f);
        if (--remaining == 0 &&
            (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)))
          return false;
      } else if (byte <= 0x7f) {
        continue;
      } else if (byte >= 0xc2 && byte <= 0xdf) {
        codepoint = byte & 0x1f;
        minimum = 0x80;
        remaining = 1;
      } else if (byte >= 0xe0 && byte <= 0xef) {
        codepoint = byte & 0x0f;
        minimum = 0x800;
        remaining = 2;
      } else if (byte >= 0xf0 && byte <= 0xf4) {
        codepoint = byte & 7;
        minimum = 0x10000;
        remaining = 3;
      } else
        return false;
    }
    consumed += count;
  }
  return remaining == 0;
}
inline constexpr uint8_t courseStringFields(uint32_t tag) {
  using namespace tinta::core::pack;
  switch (tag) {
    case makeTag("LEMM"):
      return 7;
    case makeTag("LKEY"):
    case makeTag("EKEY"):
    case makeTag("NOTE"):
    case makeTag("NSPN"):
    case makeTag("SLIN"):
    case makeTag("PENT"):
    case makeTag("CONF"):
      return 1;
    case makeTag("FORM"):
    case makeTag("LESS"):
    case makeTag("STOR"):
    case makeTag("PHRS"):
      return 2;
    case makeTag("SENT"):
    case makeTag("UNIT"):
      return 3;
    case makeTag("SQST"):
      return 5;
    case makeTag("VERB"):
      return kTenseCount * kPersonCount + 3;
    default:
      return 0;
  }
}
inline bool validCourseStringFields(tinta::core::pack::PackSource& source, const tinta::core::pack::Header& header,
                                    uint32_t stringBytes) {
  for (uint16_t section = 0; section < header.sectionCount; ++section) {
    tinta::core::pack::DirEntry entry{};
    if (!source.read(header.directoryOffset + uint32_t(section) * sizeof(entry), &entry, sizeof(entry))) return false;
    const uint8_t fields = courseStringFields(entry.tag);
    if (fields == 0 || entry.count == 0) continue;
    const uint32_t stride = entry.size / entry.count;
    if (stride < uint32_t(fields) * 4) return false;
    for (uint32_t record = 0; record < entry.count; ++record) {
      for (uint8_t field = 0; field < fields; ++field) {
        uint32_t offset = 0;
        const uint64_t at = uint64_t(entry.offset) + uint64_t(record) * stride + uint32_t(field) * 4;
        if (at > UINT32_MAX || !source.read(static_cast<uint32_t>(at), &offset, sizeof(offset)) ||
            offset >= stringBytes)
          return false;
      }
    }
  }
  return true;
}
inline bool validCourseOperands(tinta::core::pack::Pack& pack, const tinta::core::pack::Item& item,
                                uint32_t stringBytes) {
  using tinta::core::ItemKind;
  using tinta::core::pack::Section;
  switch (item.kind) {
    case ItemKind::VocabRecognise:
    case ItemKind::VocabProduce:
    case ItemKind::Gender:
      if (item.a >= pack.count(Section::Lemm) || item.b != 0) return false;
      break;
    case ItemKind::Conjugation: {
      using namespace tinta::core::pack;
      Lemma lemma{};
      if (!pack.lemma(item.a, lemma) || lemma.pos != PartOfSpeech::Verb || lemma.verbTable >= pack.count(Section::Verb))
        return false;
      if (isVerbTag(item.b)) {
        if (static_cast<uint8_t>(tagTense(item.b)) >= kTenseCount ||
            static_cast<uint8_t>(tagPerson(item.b)) >= kPersonCount)
          return false;
      } else if (item.b < kTagInfinitive || item.b > kTagParticiple)
        return false;
      break;
    }
    case ItemKind::Cloze: {
      tinta::core::pack::Sentence sentence{};
      if (!pack.sentence(item.a, sentence) || item.b >= sentence.tokenCount ||
          sentence.firstToken > pack.count(Section::Toks) ||
          sentence.tokenCount > pack.count(Section::Toks) - sentence.firstToken)
        return false;
      break;
    }
    case ItemKind::Phrase: {
      tinta::core::pack::PhraseEntry phrase{};
      if (item.a >= pack.count(Section::Sent) || !pack.phraseEntry(item.b, phrase) || phrase.sentence != item.a)
        return false;
      break;
    }
    case ItemKind::WordOrder:
      if (item.a >= pack.count(Section::Sent) || item.b != 0) return false;
      break;
    default:
      return false;
  }
  if ((item.kind == ItemKind::Gender || item.kind == ItemKind::WordOrder) && item.candidateCount != 0) return false;
  for (uint16_t index = 0; index < item.candidateCount; ++index) {
    const uint32_t candidate = pack.itemCandidate(item, static_cast<uint8_t>(index));
    if ((item.kind == ItemKind::VocabRecognise || item.kind == ItemKind::VocabProduce) &&
        candidate >= pack.count(Section::Lemm))
      return false;
    if (item.kind == ItemKind::Phrase && candidate >= pack.count(Section::Sent)) return false;
    if ((item.kind == ItemKind::Cloze || item.kind == ItemKind::Conjugation) && candidate >= stringBytes) return false;
  }
  return true;
}
inline bool validCourseRanges(tinta::core::pack::Pack& pack) {
  using tinta::core::pack::Section;
  const uint32_t units = pack.count(Section::Unit), lessons = pack.count(Section::Less),
                 stories = pack.count(Section::Stor);
  if (units > UINT16_MAX || lessons > UINT16_MAX || stories > UINT16_MAX) return false;
  for (uint32_t index = 0; index < units; ++index) {
    tinta::core::pack::Unit unit{};
    if (!pack.unit(static_cast<uint16_t>(index), unit) || unit.firstLesson > lessons ||
        unit.lessonCount > lessons - unit.firstLesson)
      return false;
  }
  for (uint32_t index = 0; index < lessons; ++index) {
    tinta::core::pack::Lesson lesson{};
    if (!pack.lesson(static_cast<uint16_t>(index), lesson) || lesson.unit >= units || lesson.number == 0 ||
        lesson.firstItem > pack.itemCount() || lesson.itemCount > pack.itemCount() - lesson.firstItem ||
        lesson.firstNote > pack.count(Section::Note) ||
        lesson.noteCount > pack.count(Section::Note) - lesson.firstNote ||
        lesson.firstSentence > pack.count(Section::Sent) ||
        lesson.sentenceCount > pack.count(Section::Sent) - lesson.firstSentence ||
        (lesson.dialogue != UINT16_MAX && lesson.dialogue >= stories))
      return false;
  }
  for (uint32_t index = 0; index < stories; ++index) {
    tinta::core::pack::Story story{};
    if (!pack.story(static_cast<uint16_t>(index), story) || (story.lesson != UINT16_MAX && story.lesson >= lessons) ||
        story.firstLine > pack.count(Section::Slin) || story.lineCount > pack.count(Section::Slin) - story.firstLine ||
        story.firstQuestion > pack.count(Section::Sqst) ||
        story.questionCount > pack.count(Section::Sqst) - story.firstQuestion)
      return false;
  }
  return true;
}
// The 16 passes cover all 16-bit numbers without an 8 KiB bitmap.
inline bool validCourseAuthoredIdentities(tinta::core::pack::Pack& pack, std::span<uint8_t> scratch) {
  using namespace tinta::core::pack;
  static constexpr uint32_t BITMAP_BYTES = 512, NUMBERS_PER_PASS = BITMAP_BYTES * 8;
  if (!pack.isOpen() || scratch.size() < BITMAP_BYTES) return false;
  const uint32_t units = pack.count(Section::Unit), lessons = pack.count(Section::Less);
  if (units > UINT16_MAX || lessons > UINT16_MAX) return false;
  const auto insert = [&](uint16_t number, uint32_t base) {
    if (number < base || uint32_t(number) - base >= NUMBERS_PER_PASS) return true;
    const uint32_t bit = uint32_t(number) - base;
    const uint8_t mask = static_cast<uint8_t>(1U << (bit % 8));
    if (scratch[bit / 8] & mask) return false;
    scratch[bit / 8] |= mask;
    return true;
  };
  for (uint32_t base = 0; base <= UINT16_MAX; base += NUMBERS_PER_PASS) {
    std::fill_n(scratch.begin(), BITMAP_BYTES, 0);
    for (uint32_t index = 0; index < units; ++index) {
      Unit unit{};
      if (!pack.unit(static_cast<uint16_t>(index), unit) || !insert(unit.number, base)) return false;
    }
  }
  for (uint32_t index = 0; index < units; ++index) {
    Unit unit{};
    if (!pack.unit(static_cast<uint16_t>(index), unit) || unit.firstLesson > lessons ||
        unit.lessonCount > lessons - unit.firstLesson)
      return false;
    if (unit.lessonCount == 0) continue;
    for (uint32_t base = 0; base <= UINT16_MAX; base += NUMBERS_PER_PASS) {
      std::fill_n(scratch.begin(), BITMAP_BYTES, 0);
      for (uint32_t child = 0; child < unit.lessonCount; ++child) {
        Lesson lesson{};
        if (!pack.lesson(static_cast<uint16_t>(unit.firstLesson + child), lesson) || lesson.unit != index ||
            lesson.number == 0 || (unit.number == UINT16_MAX && lesson.number == UINT16_MAX) ||
            !insert(lesson.number, base))
          return false;
      }
    }
  }
  for (uint32_t index = 0; index < lessons; ++index) {
    Lesson lesson{};
    Unit unit{};
    if (!pack.lesson(static_cast<uint16_t>(index), lesson) || lesson.unit >= units || !pack.unit(lesson.unit, unit) ||
        index < unit.firstLesson || index - unit.firstLesson >= unit.lessonCount)
      return false;
  }
  return true;
}
// The caller owns Pack outside the stack and supplies a bounded/yielding source.
// Failed candidates are closed; success leaves metadata available to the caller.
inline CourseValidationResult validateCourseCandidateRecords(tinta::core::pack::Pack& pack,
                                                             tinta::core::pack::PackSource& source) {
  if (pack.open(source) != tinta::core::pack::PackStatus::Ok) return CourseValidationResult::InvalidStructure;
  if (!pack.verifyCrc()) {
    pack.close();
    return CourseValidationResult::Integrity;
  }
  using tinta::core::pack::Section;
  const uint32_t items = pack.itemCount(), lessons = pack.count(Section::Less), candidates = pack.count(Section::Dist);
  if (items > UINT16_MAX || lessons > UINT16_MAX || pack.count(Section::Iuid) != items) {
    pack.close();
    return CourseValidationResult::InvalidIdentity;
  }
  tinta::core::pack::Header header{};
  tinta::core::pack::DirEntry table{};
  if (!source.read(0, &header, sizeof(header))) {
    pack.close();
    return CourseValidationResult::Integrity;
  }
  static constexpr uint32_t IUID_TAG = 0x44495549;
  size_t localeLength = 0;
  while (localeLength < sizeof(header.locale) && header.locale[localeLength]) ++localeLength;
  if (!validCourseLocale(std::string_view(header.locale, localeLength)) ||
      !std::all_of(header.locale + localeLength, header.locale + sizeof(header.locale),
                   [](char byte) { return byte == 0; })) {
    pack.close();
    return CourseValidationResult::InvalidContent;
  }
  bool found = false;
  uint32_t stringBytes = 0, stringOffset = 0;
  static constexpr uint32_t STRS_TAG = 0x53525453;
  for (uint16_t index = 0; index < header.sectionCount; ++index) {
    tinta::core::pack::DirEntry entry{};
    if (!source.read(header.directoryOffset + uint32_t(index) * sizeof(entry), &entry, sizeof(entry))) {
      pack.close();
      return CourseValidationResult::Integrity;
    }
    if (entry.tag == IUID_TAG) {
      table = entry;
      found = true;
    }
    if (entry.tag == STRS_TAG) {
      stringBytes = entry.size;
      stringOffset = entry.offset;
    }
  }
  if (!found || table.count != items || (items != 0 && table.size / items < sizeof(tinta::core::pack::ItemUid))) {
    pack.close();
    return CourseValidationResult::InvalidIdentity;
  }
  uint32_t previous = 0;
  const uint32_t stride = items == 0 ? 0 : table.size / items;
  for (uint32_t index = 0; index < items; ++index) {
    tinta::core::pack::ItemUid row{};
    tinta::core::pack::Item item{};
    const uint64_t offset = uint64_t(table.offset) + uint64_t(index) * stride;
    if (offset > UINT32_MAX || !source.read(static_cast<uint32_t>(offset), &row, sizeof(row))) {
      pack.close();
      return CourseValidationResult::Integrity;
    }
    if (row.uid <= previous || row.uid == UINT32_MAX || row.index >= items || !pack.item(row.index, item) ||
        item.uid != row.uid) {
      pack.close();
      return CourseValidationResult::InvalidIdentity;
    }
    previous = row.uid;
  }
  for (uint32_t index = 0; index < items; ++index) {
    tinta::core::pack::Item item{};
    if (!pack.item(index, item) || item.uid == 0 || item.uid == UINT32_MAX) {
      pack.close();
      return CourseValidationResult::InvalidIdentity;
    }
    if (static_cast<uint8_t>(item.kind) > 6 || (item.lesson != UINT16_MAX && item.lesson >= lessons) ||
        (item.prereq != UINT16_MAX && (item.prereq >= items || item.prereq == index)) ||
        item.firstCandidate > candidates || item.candidateCount > candidates - item.firstCandidate ||
        !validCourseOperands(pack, item, stringBytes)) {
      pack.close();
      return CourseValidationResult::InvalidContent;
    }
  }
  if (!validCourseRanges(pack) || !validCourseStringFields(source, header, stringBytes) ||
      !validCourseUtf8(source, stringOffset, stringBytes)) {
    pack.close();
    return CourseValidationResult::InvalidContent;
  }
  return CourseValidationResult::Ok;
}
}  // namespace companion

namespace companion {
inline CourseValidationResult validateCourseCandidate(tinta::core::pack::Pack& pack,
                                                      tinta::core::pack::PackSource& source,
                                                      std::span<uint8_t> scratch) {
  const auto result = validateCourseCandidateRecords(pack, source);
  if (result != CourseValidationResult::Ok) return result;
  if (!validCourseAuthoredIdentities(pack, scratch) || !validCourseStoryIdentities(pack, source, scratch) ||
      !validCourseItemIdentities(pack, source)) {
    pack.close();
    return CourseValidationResult::InvalidIdentity;
  }
  return CourseValidationResult::Ok;
}
}  // namespace companion
