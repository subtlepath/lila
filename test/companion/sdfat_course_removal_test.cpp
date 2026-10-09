#include <openssl/evp.h>

#include <cassert>

#include "lib/hal/HalCompletedRemovalJournalRelease.h"
#include "lib/hal/HalContentRemovalJournalStorage.h"
#include "lib/hal/HalCourseRemovalBaseline.h"
#include "lib/hal/HalCourseRemovalProofStorage.h"
#include "lib/hal/HalCourseRemovalStorage.h"
using namespace companion;
namespace {
struct References final : CourseRemovalReferences {
  ContentRemovalJournal* journal;
  HalCourseRemovalProofStorage& proofs;
  HalCourseRemovalBaseline* baseline;
  ContentRemovalRecord proof;
  References(ContentRemovalJournal& journal, HalCourseRemovalProofStorage& proofs, HalCourseRemovalBaseline& baseline)
      : journal(&journal), proofs(proofs), baseline(&baseline) {}
  bool verifyPlan(const ContentRemovalRecord&, const CourseRemovalPlan&) override { return true; }
  bool publish(const ContentRemovalRecord& record, const CourseRemovalPlan&) override {
    return proofs.persist(record, *journal) == CourseRemovalProofStorageResult::Ok;
  }
  bool verify(const ContentRemovalRecord& record, const CourseRemovalPlan&) override {
    return proofs.load(record.request.generation, proof) == CourseRemovalProofStorageResult::Ok &&
           matchesCourseRemovalProof(proof, record);
  }
  bool retire(const ContentRemovalRecord& record, const CourseRemovalPlan& plan) override {
    return verify(record, plan) && baseline->retain(record, proof);
  }
  bool verifyRetired(const ContentRemovalRecord& record, const CourseRemovalPlan& plan) override {
    return verify(record, plan) && baseline->verify(record, proof);
  }
};
void digest(std::span<const uint8_t> bytes, Digest& output) {
  assert(EVP_Digest(bytes.data(), bytes.size(), output.data(), nullptr, EVP_sha256(), nullptr));
}
}  // namespace
int main() {
  for (unsigned fault = 0; fault < 11; ++fault) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    state.falseExists = true;
    state.directories["/"] = {};
    state.directories["/tinta"] = {};
    CourseRemovalPlan plan;
    auto& request = plan.request;
    request.transaction.fill(1);
    request.owner.fill(2);
    request.generation.fill(3);
    request.manifest.kind = ContentKind::Course;
    request.manifest.formatVersion = 1;
    request.manifest.logicalIdentity.fill(5);
    const std::vector<uint8_t> pack(123, 7);
    request.manifest.length = pack.size();
    digest(pack, request.manifest.contentHash);
    state.files[ACTIVE_COURSE_PATH] = pack;
    const std::string history = "/tinta/learner-state";
    state.files[history] = {90, 91};
    std::array<uint8_t, COURSE_REMOVAL_PLAN_SIZE> bytes{};
    assert(CourseRemovalPlanCodec::encode(plan, bytes) == bytes.size());
    ContentRemovalRecord initial;
    initial.request = request;
    digest(bytes, initial.planHash);
    std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalScratch{};
    std::array<uint8_t, 64> scratch{};
    HalContentRemovalJournalStorage journalStorage;
    ContentRemovalJournal journal(journalStorage, journalScratch);
    HalCourseRemovalStorage members(journal, bytes, scratch);
    std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> proofScratch{}, receiptScratch{}, releaseScratch{};
    HalCourseRemovalProofStorage proofs(proofScratch);
    HalCourseRemovalBaseline baseline(journal, scratch);
    References refs(journal, proofs, baseline);
    CourseRemovalParticipant participant(journal, members, refs);
    HalCourseRemovalStorage overlapping(journal, bytes, bytes);
    assert(!overlapping.verifyPlan(plan, initial.planHash));
    if (fault == 8) {
      bytes[20] ^= 1;
      assert(!participant.bind(plan, initial.planHash));
      bytes[20] ^= 1;
    }
    assert(participant.bind(plan, initial.planHash));
    assert(members.stat(1, false) == FileStatus::Error);
    assert(!members.quarantine(0));
    if (fault == 1) state.failRename = 1;
    if (fault == 2) state.failRenameAfter = 1;
    if (fault == 3) state.failRenameAfter = 3;
    if (fault == 10) state.failRename = 3;
    if (fault == 4) state.files[ACTIVE_COURSE_PATH][0] ^= 1;
    if (fault == 5) state.readErrorPath = ACTIVE_COURSE_PATH;
    if (fault == 6) state.failSyncPath = ACTIVE_COURSE_PATH;
    if (fault == 7) state.failClosePath = ACTIVE_COURSE_PATH;
    auto proofRecord = initial;
    proofRecord.phase = ContentRemovalPhase::Quarantined;
    proofRecord.revision = 2;
    std::array<char, COURSE_REMOVAL_CACHE_PATH_CAPACITY> cachePath{};
    assert(courseRemovalCachePath(proofRecord, cachePath));
    if (fault == 9) state.files[cachePath.data()] = {90};
    ContentRemoval removal(journal, participant);
    const auto result = removal.remove(initial);
    assert((result == ContentRemovalJournalResult::Ok) == (fault == 0 || fault == 8));
    if (fault >= 4 && fault <= 7) {
      assert(!journal.current());
      assert(state.renames == 0);
      assert(state.files.contains(ACTIVE_COURSE_PATH));
    }
    if (fault == 4) state.files[ACTIVE_COURSE_PATH][0] ^= 1;
    if (fault == 9) state.files[cachePath.data()] = pack;
    state.failRename = state.failRenameAfter = 0;
    state.failRemoveAfter = false;
    state.readErrorPath.clear();
    state.failSyncPath.clear();
    state.failClosePath.clear();
    ContentRemovalJournal recovered(journalStorage, journalScratch);
    HalCourseRemovalStorage restoredMembers(recovered, bytes, scratch);
    HalCourseRemovalBaseline restoredBaseline(recovered, scratch);
    refs.journal = &recovered;
    refs.baseline = &restoredBaseline;
    CourseRemovalParticipant restored(recovered, restoredMembers, refs);
    assert(restored.bind(plan, initial.planHash));
    ContentRemoval retry(recovered, restored);
    assert(retry.remove(initial) == ContentRemovalJournalResult::Ok);
    assert(!state.files.contains(ACTIVE_COURSE_PATH));
    assert(state.files.at(history) == std::vector<uint8_t>({90, 91}));
    assert(state.files.at(cachePath.data()) == pack);
    const auto retained = state.files;
    assert(retry.remove(initial) == ContentRemovalJournalResult::Ok);
    assert(state.files == retained);
    const auto completed = *recovered.current();
    assert(!restoredBaseline.verifyCompleted(proofRecord, completed, request.manifest, request.generation));
    HalCompletedContentRemovals completions(receiptScratch);
    assert(completions.persist(completed, recovered) == CompletedRemovalResult::Ok);
    HalCompletedRemovalJournalRelease release(recovered, journalStorage, completions, releaseScratch);
    assert(release.release() == CompletedRemovalResult::Ok);
    assert(restoredBaseline.verifyCompleted(proofRecord, completed, request.manifest, request.generation));
    assert(std::string(restoredBaseline.path()) == cachePath.data());
    auto replaced = request.generation;
    replaced[0] ^= 1;
    assert(!restoredBaseline.verifyCompleted(proofRecord, completed, request.manifest, replaced));
    assert(!restoredBaseline.path());
    state.files[ACTIVE_COURSE_PATH] = pack;
    assert(!restoredBaseline.verifyCompleted(proofRecord, completed, request.manifest, request.generation));
    state.files.erase(ACTIVE_COURSE_PATH);
    state.files[cachePath.data()][0] ^= 1;
    assert(!restoredBaseline.verifyCompleted(proofRecord, completed, request.manifest, request.generation));
    assert(!restoredBaseline.path());
    state.files[cachePath.data()] = pack;
    assert(restoredBaseline.verifyCompleted(proofRecord, completed, request.manifest, request.generation));
  }
}
