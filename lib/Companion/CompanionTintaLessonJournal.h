#pragma once

#include "CompanionIdentityUniqueness.h"
#include "CompanionJournalCourseMembership.h"
#include "CompanionTintaWriter.h"
#include "core/profile/LessonCompletion.h"

namespace companion {
// Borrow validated immutable lesson identities/catalog; recovery precedes profile loading.
class TintaLessonJournal {
 public:
  struct Callbacks {
    void* context = nullptr;
    bool (*clock)(void*, uint32_t&, uint64_t&, ClockQuality&) = nullptr;
    bool (*recover)(void*) = nullptr;
    void (*reportError)(void*, TintaJournalResult) = nullptr;
  };
  TintaLessonJournal(TintaWriter& writer, IdentityKeys& lessons, TintaSubjectCatalog& catalog, const Identity& course,
                     const Digest& resource, Callbacks callbacks)
      : writer(writer), lessons(lessons), catalog(catalog), course(course), resource(resource), callbacks(callbacks) {}
  tinta::core::LessonCompletion::MutationJournal binding() { return {this, &persistCallback, &recoverCallback}; }
  bool persist(uint16_t first, uint16_t last) {
    if (!callbacks.reportError || !callbacks.clock || !callbacks.recover || first > last || last >= lessons.count())
      return error(TintaJournalResult::Invalid);
    if (!writer.available()) return error(TintaJournalResult::Unavailable);
    body = {};
    body.kind = EventKind::LessonComplete;
    body.course = course;
    body.enabled = true;
    // Validate every subject before creating any authoritative event.
    for (uint32_t at = first; at <= last; ++at)
      if (!subject(at)) return false;
    uint32_t day = 0;
    uint64_t timestamp = 0;
    ClockQuality quality = ClockQuality::Unknown;
    if (!callbacks.clock(callbacks.context, day, timestamp, quality)) return error(TintaJournalResult::Invalid);
    for (uint32_t at = first; at <= last; ++at) {
      if (!subject(at)) return false;
      const auto result = writer.record(body, resource, day, timestamp, quality);
      if (result != TintaJournalResult::Ok && result != TintaJournalResult::Duplicate) return error(result);
    }
    return true;
  }

 private:
  bool subject(uint32_t index) {
    if (!lessons.read(index, body.uid)) return error(TintaJournalResult::IoError);
    if (!tinta_body_detail::valid(body)) return error(TintaJournalResult::Invalid);
    const auto membership = catalog.contains(EventKind::LessonComplete, body.uid);
    if (membership != TintaSubjectMembership::Present)
      return error(membership == TintaSubjectMembership::IoError ? TintaJournalResult::IoError
                                                                 : TintaJournalResult::Invalid);
    return true;
  }
  bool error(TintaJournalResult result) {
    if (callbacks.reportError) callbacks.reportError(callbacks.context, result);
    return false;
  }
  static bool persistCallback(void* context, uint16_t first, uint16_t last) {
    return static_cast<TintaLessonJournal*>(context)->persist(first, last);
  }
  static bool recoverCallback(void* context) {
    auto& owner = *static_cast<TintaLessonJournal*>(context);
    if (!owner.callbacks.reportError || !owner.callbacks.recover) return owner.error(TintaJournalResult::Invalid);
    return owner.callbacks.recover(owner.callbacks.context) || owner.error(TintaJournalResult::IoError);
  }
  TintaWriter& writer;
  IdentityKeys& lessons;
  TintaSubjectCatalog& catalog;
  Identity course;
  Digest resource;
  Callbacks callbacks;
  TintaBody body;
};
}  // namespace companion
