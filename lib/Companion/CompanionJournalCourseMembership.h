#pragma once

#include "CompanionTintaJournal.h"

namespace companion {
enum class TintaSubjectMembership { Present, Missing, IoError };
class TintaSubjectCatalog {
 public:
  virtual ~TintaSubjectCatalog() = default;
  virtual TintaSubjectMembership contains(EventKind kind, uint32_t uid) = 0;
};
// Caller binds the immutable, fully validated catalog to the selected course/pack.
class JournalCourseMembershipValidation {
 public:
  TintaJournalResult validate(TintaJournal& journal, const Identity& course, TintaSubjectCatalog& catalog) {
    if (!journal.available()) return TintaJournalResult::Unavailable;
    if (!tinta_body_detail::nonzero(course)) return TintaJournalResult::Invalid;
    const auto count = journal.count();
    for (uint32_t at = 0; at < count; ++at) {
      const auto result = journal.read(at);
      if (result != TintaJournalResult::Ok) return result;
      if (journal.event().kind < EventKind::Review) continue;
      if (!decodeTintaBody(journal.body(), body)) return TintaJournalResult::Corrupt;
      if (body.course != course) continue;
      const auto membership = catalog.contains(body.kind, body.uid);
      if (membership == TintaSubjectMembership::IoError) return TintaJournalResult::IoError;
      if (membership != TintaSubjectMembership::Present) return TintaJournalResult::Invalid;
    }
    return journal.count() == count ? TintaJournalResult::Ok : TintaJournalResult::Conflict;
  }

 private:
  TintaBody body{};
};
}  // namespace companion
