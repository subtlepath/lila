#include <openssl/evp.h>

#include <cassert>

#include "lib/hal/HalCompletedRemovalJournalRelease.h"
#include "lib/hal/HalContentRemovalJournalStorage.h"
#include "lib/hal/HalCourseRemovalBaseline.h"
#include "lib/hal/HalCourseRemovalReferences.h"
#include "lib/hal/HalCourseRemovalStorage.h"
using namespace companion;
namespace {
struct Metadata final : TransferStorage {
  bool prepare() override { return false; }
  FileStatus stat(const char* path, uint64_t& length) override {
    const auto& state = inventory_hal_test::state;
    if (path == state.statErrorPath) return FileStatus::Error;
    const auto it = state.files.find(path);
    if (it == state.files.end()) return FileStatus::Missing;
    length = it->second.size();
    return FileStatus::Present;
  }
  bool read(const char* path, uint64_t offset, std::span<uint8_t> bytes) override {
    const auto& state = inventory_hal_test::state;
    if (path == state.readErrorPath) return false;
    const auto it = state.files.find(path);
    if (it == state.files.end() || offset > it->second.size() || bytes.size() > it->second.size() - offset)
      return false;
    std::copy_n(it->second.begin() + offset, bytes.size(), bytes.begin());
    return true;
  }
  bool write(const char*, uint64_t, std::span<const uint8_t>, bool) override { return false; }
  bool resize(const char*, uint64_t) override { return false; }
  bool rename(const char*, const char*) override { return false; }
  bool remove(const char*) override { return false; }
  bool verify(const char*, uint64_t, const Digest&, std::span<uint8_t>) override { return false; }
};
bool permitted(void* context) { return !context || *static_cast<bool*>(context); }
void receipt(const CourseMigrationPaths& paths, const Identity& origin) {
  for (const auto* path : {paths.intent, paths.done}) {
    const bool intent = path == paths.intent;
    auto& bytes = inventory_hal_test::state.files[path];
    bytes.resize(intent ? 28 : 24);
    std::memcpy(bytes.data(), intent ? "CLSM" : "CLSD", 4);
    std::copy(origin.begin(), origin.end(), bytes.begin() + 4);
    const auto end = bytes.size() - 4;
    const auto crc = courseBindingCrc(std::span(bytes).first(end));
    for (unsigned i = 0; i < 4; ++i) bytes[end + i] = static_cast<uint8_t>(crc >> (8 * i));
  }
}
void digest(std::span<const uint8_t> bytes, Digest& output) {
  assert(EVP_Digest(bytes.data(), bytes.size(), output.data(), nullptr, EVP_sha256(), nullptr));
}
}  // namespace
int main() {
  for (unsigned fault = 0; fault < 19; ++fault) {
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
    std::array<char, COURSE_STATE_DIRECTORY_SIZE> statePath{};
    assert(courseStateDirectory(request.manifest.logicalIdentity, statePath));
    state.directories["/tinta/courses"] = {};
    state.directories[statePath.data()] = {};
    const std::string history = std::string(statePath.data()) + "/reviews.log";
    std::array<uint8_t, COURSE_BINDING_SIZE> bindingBytes{};
    assert(encodeCourseBinding(request.manifest, bindingBytes) == bindingBytes.size());
    state.files[COURSE_BINDING_PATH] = {bindingBytes.begin(), bindingBytes.end()};
    receipt(COURSE_STATE_MIGRATION_PATHS, request.manifest.logicalIdentity);
    receipt(COURSE_MARK_MIGRATION_PATHS, request.manifest.logicalIdentity);
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
    bool allowed = true;
    HalCourseRemovalProofStorage proofs(proofScratch, permitted, &allowed);
    HalCourseRemovalBaseline baseline(journal, scratch, permitted, &allowed);
    Metadata metadata;
    std::array<uint8_t, COURSE_BINDING_SIZE> metadataScratch{};
    HalCourseStateIsolation isolation(metadata, metadataScratch, permitted, &allowed);
    HalCourseRemovalReferences refs(journal, metadata, metadataScratch, isolation, proofs, baseline, permitted,
                                    nullptr);
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
    if (fault == 11) state.files[COURSE_BINDING_PATH][0] ^= 1;
    if (fault == 12) state.directories.erase(statePath.data());
    if (fault == 13) state.files["/tinta/reviews.log"] = {1};
    if (fault == 14) {
      auto different = request.manifest.logicalIdentity;
      different[0] ^= 1;
      receipt(COURSE_MARK_MIGRATION_PATHS, different);
    }
    if (fault == 15) state.files[COURSE_BINDING_STAGE] = {1};
    if (fault == 16) {
      auto other = request.manifest;
      other.contentHash[0] ^= 1;
      assert(encodeCourseBinding(other, bindingBytes) == bindingBytes.size());
      state.files[COURSE_BINDING_PATH] = {bindingBytes.begin(), bindingBytes.end()};
    }
    if (fault == 17) state.files[COURSE_REMOVAL_PROOF_STAGE] = {1};
    if (fault == 18) allowed = false;
    ContentRemoval removal(journal, participant);
    const auto result = removal.remove(initial);
    assert((result == ContentRemovalJournalResult::Ok) == (fault == 0 || fault == 8));
    if ((fault >= 4 && fault <= 7) || fault >= 11) {
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
    if (fault >= 11) {
      assert(encodeCourseBinding(request.manifest, bindingBytes) == bindingBytes.size());
      state.files[COURSE_BINDING_PATH] = {bindingBytes.begin(), bindingBytes.end()};
      state.directories[statePath.data()] = {};
      state.files.erase("/tinta/reviews.log");
      receipt(COURSE_MARK_MIGRATION_PATHS, request.manifest.logicalIdentity);
      state.files.erase(COURSE_BINDING_STAGE);
      state.files.erase(COURSE_REMOVAL_PROOF_STAGE);
      allowed = true;
    }
    ContentRemovalJournal recovered(journalStorage, journalScratch);
    HalCourseRemovalStorage restoredMembers(recovered, bytes, scratch);
    HalCourseRemovalBaseline restoredBaseline(recovered, scratch, permitted, &allowed);
    HalCourseRemovalReferences restoredRefs(recovered, metadata, metadataScratch, isolation, proofs, restoredBaseline,
                                            permitted, &allowed);
    CourseRemovalParticipant restored(recovered, restoredMembers, restoredRefs);
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
