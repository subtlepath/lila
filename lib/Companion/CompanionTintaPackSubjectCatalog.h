#pragma once

#include "CompanionCourseItemIdentities.h"
#include "CompanionIdentityUniqueness.h"
#include "CompanionJournalCourseMembership.h"
#include "CompanionTintaPackStoryIdentity.h"

namespace companion {
class TintaPackSubjectKeys final : public IdentityKeys {
 public:
  TintaPackSubjectKeys(const tinta::core::pack::Pack& pack, bool readings) : pack(pack), readings(readings) {}
  uint32_t count() const override {
    return pack.count(readings ? tinta::core::pack::Section::Stor : tinta::core::pack::Section::Less);
  }
  bool read(uint32_t index, uint32_t& identity) override {
    namespace pk = tinta::core::pack;
    if (index > UINT16_MAX) return false;
    if (readings) {
      pk::Story story;
      return pack.story(static_cast<uint16_t>(index), story) && tintaPackStoryIdentity(pack, story, identity);
    }
    pk::Lesson lesson;
    pk::Unit unit;
    if (!pack.lesson(static_cast<uint16_t>(index), lesson) || !pack.unit(lesson.unit, unit) || !lesson.number)
      return false;
    identity = (static_cast<uint32_t>(unit.number) << 16) | lesson.number;
    return identity != 0 && identity != UINT32_MAX;
  }

 private:
  const tinta::core::pack::Pack& pack;
  bool readings;
};
// Pack/source remain immutable and must have passed full structure/content/CRC validation.
class TintaPackSubjectCatalog final : public TintaSubjectCatalog {
 public:
  TintaPackSubjectCatalog(const tinta::core::pack::Pack& pack, tinta::core::pack::PackSource& source)
      : pack(pack), source(source), lessons(pack, false), readings(pack, true) {}
  bool prepare(std::span<uint8_t> scratch) {
    ready = false;
    tinta::core::pack::DirEntry history{};
    if (!pack.isOpen() || lessons.count() > UINT16_MAX || readings.count() > UINT16_MAX ||
        !readCourseItemIdentityTable(source, history, hasHistory))
      return false;
    historyCount = history.count;
    for (uint32_t at = 0; at < pack.itemCount(); ++at) {
      const auto uid = pack.uidAt(at);
      if (!uid || uid == UINT32_MAX || (hasHistory && uid > historyCount)) return false;
    }
    ready = uniqueIdentityKeys(lessons, scratch) && uniqueIdentityKeys(readings, scratch);
    return ready;
  }
  TintaSubjectMembership contains(EventKind kind, uint32_t uid) override {
    if (!ready) return TintaSubjectMembership::IoError;
    if (!uid || uid == UINT32_MAX) return TintaSubjectMembership::Missing;
    if (kind == EventKind::LessonComplete) return containsKey(lessons, uid);
    if (kind == EventKind::ReadingComplete) return containsKey(readings, uid);
    if (kind != EventKind::Review && kind != EventKind::UndoReview && kind != EventKind::Suspension &&
        kind != EventKind::Star)
      return TintaSubjectMembership::Missing;
    if (hasHistory) return uid <= historyCount ? TintaSubjectMembership::Present : TintaSubjectMembership::Missing;
    for (uint32_t at = 0; at < pack.itemCount(); ++at) {
      const auto candidate = pack.uidAt(at);
      if (!candidate || candidate == UINT32_MAX) return TintaSubjectMembership::IoError;
      if (candidate == uid) return TintaSubjectMembership::Present;
    }
    return TintaSubjectMembership::Missing;
  }

 private:
  static TintaSubjectMembership containsKey(IdentityKeys& keys, uint32_t uid) {
    for (uint32_t at = 0; at < keys.count(); ++at) {
      uint32_t candidate = 0;
      if (!keys.read(at, candidate)) return TintaSubjectMembership::IoError;
      if (candidate == uid) return TintaSubjectMembership::Present;
    }
    return TintaSubjectMembership::Missing;
  }
  const tinta::core::pack::Pack& pack;
  tinta::core::pack::PackSource& source;
  TintaPackSubjectKeys lessons, readings;
  uint32_t historyCount = 0;
  bool hasHistory = false, ready = false;
};
}  // namespace companion
