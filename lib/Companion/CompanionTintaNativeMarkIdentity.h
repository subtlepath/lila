#pragma once

#include "CompanionJournalCourseMembership.h"
#include "CompanionTintaLegacyStoryIdentity.h"

namespace companion {
// Pack/catalog are validated, immutable and bound to the same course.
inline bool resolveTintaNativeMarkIdentity(const tinta::core::pack::Pack& pack, TintaSubjectCatalog& catalog,
                                           EventKind kind, uint32_t key, uint32_t& output) {
  if (!pack.isOpen() || !key || (kind != EventKind::Star && kind != EventKind::ReadingComplete)) return false;
  uint32_t identity = key;
  if (kind == EventKind::ReadingComplete &&
      resolveTintaLegacyStoryKey(pack, key, identity) != LegacyStoryIdentityResult::Matched)
    return false;
  if (catalog.contains(kind, identity) != TintaSubjectMembership::Present) return false;
  output = identity;
  return true;
}
}  // namespace companion
