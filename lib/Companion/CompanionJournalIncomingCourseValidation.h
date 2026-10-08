#pragma once

#include "CompanionJournalCourseMembership.h"

namespace companion {
// Existing prefix is checked by retention proof; all new Tinta subjects need the selected validated catalog.
class JournalIncomingCourseValidation {
 public:
  TintaJournalResult validate(TintaJournal& journal, uint32_t previousCount, const Identity* course,
                              TintaSubjectCatalog* catalog) {
    if (!journal.available()) return TintaJournalResult::Unavailable;
    if ((course == nullptr) != (catalog == nullptr) || (course && !tinta_body_detail::nonzero(*course)))
      return TintaJournalResult::Invalid;
    const auto count = journal.count();
    if (previousCount > count) return TintaJournalResult::Conflict;
    for (uint32_t at = previousCount; at < count; ++at) {
      const auto read = journal.read(at);
      if (read != TintaJournalResult::Ok) return read;
      if (journal.event().kind < EventKind::Review) continue;
      if (!decodeTintaBody(journal.body(), body)) return TintaJournalResult::Corrupt;
      if (!course || body.course != *course) return TintaJournalResult::Invalid;
      const auto membership = catalog->contains(body.kind, body.uid);
      if (membership == TintaSubjectMembership::IoError) return TintaJournalResult::IoError;
      if (membership != TintaSubjectMembership::Present) return TintaJournalResult::Invalid;
    }
    return journal.count() == count ? TintaJournalResult::Ok : TintaJournalResult::Conflict;
  }

 private:
  TintaBody body;
};
}  // namespace companion
