#pragma once

#if LILA_TINTA
#include "CompanionCourseSource.h"
#include "CompanionCourseValidation.h"
#include "CompanionTintaPackSubjectCatalog.h"
#include "HalCompanionHeapAdmission.h"
#include "HalInventoryIndexStorage.h"
#include "HalJournalCausalAuditSession.h"

namespace companion {
// Off-stack handles/catalog borrow the transfer parser and its existing workspace.
// Caller validates the candidate hash and excludes content/history writers.
class HalTintaCandidateAuthority final {
 public:
  explicit HalTintaCandidateAuthority(tinta::core::pack::Pack& parser)
      : parser(parser), source(storage), catalog(parser, source) {}
  ~HalTintaCandidateAuthority() {
    parser.close();
    if (!storage.close()) failure("candidate close");
  }
  bool verify(const char* path, const Identity& course, std::span<uint8_t> scratch) {
    if (!storage.open(path) || !source.attach() ||
        validateCourseCandidate(parser, source, scratch) != CourseValidationResult::Ok || !catalog.prepare(scratch))
      return failure("candidate catalog");
    // Audit state exceeds the task stack; release it before installation proceeds.
    if (!admitCompanionHeap(sizeof(HalJournalCausalAuditSession), sizeof(HalJournalCausalAuditSession)))
      return failure("audit heap admission");
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit) return failure("OOM: candidate history audit");
    const bool verified = audit->run(nullptr, &course, &catalog);
    audit.reset();
    parser.close();
    const bool closed = storage.close();
    return (verified && closed) || failure("candidate history membership/close");
  }

 private:
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Tinta candidate authority failed: %s", reason);
    return false;
  }
  tinta::core::pack::Pack& parser;
  HalInventoryIndexStorage storage;
  StoredCourseSource source;
  TintaPackSubjectCatalog catalog;
};
}  // namespace companion
#endif
