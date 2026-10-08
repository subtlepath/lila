#pragma once

#include "CompanionIdentityUniqueness.h"
#include "core/pack/Pack.h"

namespace companion {
class CourseStoryKeys final : public IdentityKeys {
 public:
  CourseStoryKeys(tinta::core::pack::Pack& pack, tinta::core::pack::PackSource& source, uint32_t stringsOffset,
                  uint32_t stringsSize)
      : pack(pack), source(source), stringsOffset(stringsOffset), stringsSize(stringsSize) {}
  uint32_t count() const override { return pack.count(tinta::core::pack::Section::Stor); }
  bool read(uint32_t index, uint32_t& identity) override {
    if (stringsOffset > source.size() || stringsSize > source.size() - stringsOffset) return false;
    tinta::core::pack::Story story{};
    if (index > UINT16_MAX || !pack.story(static_cast<uint16_t>(index), story) ||
        static_cast<uint8_t>(story.kind) > 1 || story.title == 0 || story.title >= stringsSize)
      return false;
    uint32_t lessonIdentity = UINT32_MAX;
    if (story.lesson != UINT16_MAX) {
      tinta::core::pack::Lesson lesson{};
      tinta::core::pack::Unit unit{};
      if (!pack.lesson(story.lesson, lesson) || !pack.unit(lesson.unit, unit) || lesson.number == 0) return false;
      lessonIdentity = (uint32_t(unit.number) << 16) | lesson.number;
      if (lessonIdentity == UINT32_MAX) return false;
    }
    uint8_t buffer[64];
    static constexpr uint8_t DOMAIN[] = {'T', 'S', 'T', '1'};
    uint32_t position = story.title, hash = 2166136261U;
    for (const uint8_t byte : DOMAIN) hash = (hash ^ byte) * 16777619U;
    hash = (hash ^ static_cast<uint8_t>(story.kind)) * 16777619U;
    for (unsigned i = 0; i < 4; ++i) hash = (hash ^ static_cast<uint8_t>(lessonIdentity >> (8 * i))) * 16777619U;
    while (position < stringsSize) {
      const uint32_t bytes = std::min<uint32_t>(sizeof(buffer), stringsSize - position);
      if (!source.read(stringsOffset + position, buffer, bytes)) return false;
      for (uint32_t i = 0; i < bytes; ++i) {
        if (buffer[i] == 0) {
          identity = hash == 0 ? 1 : hash;
          return identity != UINT32_MAX;
        }
        hash = (hash ^ buffer[i]) * 16777619U;
      }
      position += bytes;
    }
    return false;
  }

 private:
  tinta::core::pack::Pack& pack;
  tinta::core::pack::PackSource& source;
  uint32_t stringsOffset, stringsSize;
};
inline bool validCourseStoryIdentities(tinta::core::pack::Pack& pack, tinta::core::pack::PackSource& source,
                                       std::span<uint8_t> scratch) {
  if (!pack.isOpen() || pack.count(tinta::core::pack::Section::Stor) > UINT16_MAX) return false;
  tinta::core::pack::Header header{};
  if (!source.read(0, &header, sizeof(header))) return false;
  for (uint16_t index = 0; index < header.sectionCount; ++index) {
    tinta::core::pack::DirEntry entry{};
    if (!source.read(header.directoryOffset + uint32_t(index) * sizeof(entry), &entry, sizeof(entry))) return false;
    if (entry.tag == tinta::core::pack::makeTag("STRS")) {
      if (entry.offset > source.size() || entry.size > source.size() - entry.offset) return false;
      CourseStoryKeys keys(pack, source, entry.offset, entry.size);
      return uniqueIdentityKeys(keys, scratch);
    }
  }
  return false;
}
}  // namespace companion
