#include <openssl/evp.h>

#include <cassert>

#include "lib/hal/HalCompletedRemovalJournalRelease.h"
#include "lib/hal/HalContentRemovalJournalStorage.h"
#include "lib/hal/HalCourseRemovalBaseline.h"
#include "lib/hal/HalCourseRemovalBoundParticipant.h"
#include "lib/hal/HalCourseRemovalMetadata.h"
#include "lib/hal/HalCourseRemovalRecovery.h"
#include "lib/hal/HalCourseRemovalReferences.h"
#include "lib/hal/HalCourseRemovalSession.h"
#include "lib/hal/HalCourseRemovalStorage.h"
using namespace companion;
namespace {
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
struct Snapshot final : InventoryIndexStorage {
  std::vector<uint8_t> bytes;
  bool fail = false;
  bool size(uint64_t& output) override {
    output = bytes.size();
    return !fail;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    if (fail || offset > bytes.size() || output.size() > bytes.size() - offset) return false;
    std::copy_n(bytes.begin() + offset, output.size(), output.begin());
    return true;
  }
};
struct SessionContext {
  ContentManifest manifest;
  bool allowed = true, inventoryReady = true, stateReady = true, refreshReady = true;
  unsigned inventories = 0, preparations = 0, refreshes = 0;
  static bool permission(void* ctx) { return static_cast<SessionContext*>(ctx)->allowed; }
  static bool inventory(void* ctx, uint64_t& revision) {
    auto& self = *static_cast<SessionContext*>(ctx);
    ++self.inventories;
    revision = 9;
    return self.inventoryReady;
  }
  static bool prepare(void* ctx, const ContentManifest& manifest) {
    auto& self = *static_cast<SessionContext*>(ctx);
    ++self.preparations;
    return self.stateReady && self.manifest == manifest;
  }
  static bool refresh(void* ctx) {
    auto& self = *static_cast<SessionContext*>(ctx);
    ++self.refreshes;
    return self.refreshReady;
  }
};
void sessionRoundTrip() {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  state.directories["/"] = {};
  state.directories["/tinta"] = {};
  state.directories["/tinta/courses"] = {};
  ContentRemovalRequest request;
  request.transaction.fill(21);
  request.owner.fill(22);
  request.generation.fill(23);
  request.manifest.kind = ContentKind::Course;
  request.manifest.formatVersion = 1;
  request.manifest.logicalIdentity.fill(24);
  const std::vector<uint8_t> pack(123, 25);
  request.manifest.length = pack.size();
  digest(pack, request.manifest.contentHash);
  state.files[ACTIVE_COURSE_PATH] = pack;
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> directory{};
  assert(courseStateDirectory(request.manifest.logicalIdentity, directory));
  state.directories[directory.data()] = {};
  const std::string history = std::string(directory.data()) + "/reviews.log";
  state.files[history] = {31, 32};
  std::array<uint8_t, COURSE_BINDING_SIZE> binding{};
  assert(encodeCourseBinding(request.manifest, binding) == binding.size());
  state.files[COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
  receipt(COURSE_STATE_MIGRATION_PATHS, request.manifest.logicalIdentity);
  receipt(COURSE_MARK_MIGRATION_PATHS, request.manifest.logicalIdentity);
  Snapshot snapshot;
  snapshot.bytes.resize(INVENTORY_INDEX_HEADER_SIZE + INVENTORY_PATH_MAX_RECORD);
  const auto recordSize = encodeInventoryPath(request.manifest, ACTIVE_COURSE_PATH,
                                              std::span(snapshot.bytes).subspan(INVENTORY_INDEX_HEADER_SIZE));
  snapshot.bytes.resize(INVENTORY_INDEX_HEADER_SIZE + recordSize);
  InventoryIndexHeader header{request.generation, 9, 1,
                              inventoryIndexCrc(std::span(snapshot.bytes).subspan(INVENTORY_INDEX_HEADER_SIZE))};
  assert(encodeInventoryPathsHeader(header, snapshot.bytes) == INVENTORY_INDEX_HEADER_SIZE);
  std::array<uint8_t, INVENTORY_PATH_MAX_RECORD> pathBytes{};
  InventoryPaths paths(snapshot, pathBytes);
  SessionContext context;
  context.manifest = request.manifest;
  HalCourseRemovalMetadata metadata(SessionContext::permission, &context);
  std::array<uint8_t, 512> io{};
  uint64_t revision = 9;
  std::array<uint8_t, CONTENT_REMOVAL_REQUEST_SIZE> command{};
  assert(encodeContentRemovalRequest(request, command) == command.size());
  std::array<uint8_t, CONTENT_REMOVAL_REPLY_SIZE> reply{};
  {
    HalCourseRemovalSession session(request.generation, paths, revision, metadata, io, SessionContext::permission,
                                    SessionContext::refresh, &context, SessionContext::inventory,
                                    SessionContext::prepare);
    assert(session.handle(true, request.owner, command, reply) == 0);
    assert(session.prepare());
    const auto before = state.files;
    assert(session.handle(false, request.owner, command, reply) == reply.size());
    assert(reply[0] == static_cast<uint8_t>(ContentRemovalResult::Unauthorized));
    assert(state.files == before);
    auto foreign = request.owner;
    foreign[0] ^= 1;
    assert(session.handle(true, foreign, command, reply) == reply.size());
    assert(reply[0] == static_cast<uint8_t>(ContentRemovalResult::Unauthorized));
    assert(state.files == before);
    context.refreshReady = false;
    assert(session.handle(true, request.owner, command, reply) == reply.size());
    assert(reply[0] == static_cast<uint8_t>(ContentRemovalResult::IoError));
    assert(!state.files.contains(ACTIVE_COURSE_PATH));
    assert(state.files.at(history) == std::vector<uint8_t>({31, 32}));
    assert(state.files.at(COURSE_BINDING_PATH) == std::vector<uint8_t>(binding.begin(), binding.end()));
    assert(session.closeReaders());
  }
  const auto removed = state.files;
  snapshot.fail = true;
  context.inventoryReady = context.stateReady = false;
  context.refreshReady = true;
  HalCourseRemovalSession reopened(request.generation, paths, revision, metadata, io, SessionContext::permission,
                                   SessionContext::refresh, &context, SessionContext::inventory,
                                   SessionContext::prepare);
  assert(reopened.prepare());
  assert(reopened.handle(true, request.owner, command, reply) == reply.size());
  assert(reply[0] == static_cast<uint8_t>(ContentRemovalResult::Ok));
  assert(std::equal(request.transaction.begin(), request.transaction.end(), reply.begin() + 1));
  assert(state.files == removed);
  assert(context.inventories == 1 && context.preparations == 1 && context.refreshes == 2);
  context.allowed = false;
  assert(reopened.handle(true, request.owner, command, reply) == reply.size());
  assert(reply[0] == static_cast<uint8_t>(ContentRemovalResult::Busy));
  assert(state.files == removed);
  assert(reopened.closeReaders());
}
}  // namespace
int main() {
  sessionRoundTrip();
  for (unsigned fault = 0; fault < 24; ++fault) {
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
    HalCourseRemovalMetadata metadata(permitted, &allowed);
    std::array<uint8_t, COURSE_BINDING_SIZE> metadataScratch{};
    HalCourseStateIsolation isolation(metadata, metadataScratch, permitted, &allowed);
    HalCourseRemovalReferences refs(journal, metadata, metadataScratch, isolation, proofs, baseline, permitted,
                                    nullptr);
    CourseRemovalParticipant participant(journal, members, refs);
    std::array<uint8_t, REMOVAL_STAGE_CLAIM_SIZE> comparisonBytes{};
    HalCourseRemovalPlanStorage plans(comparisonBytes);
    Digest publishedHash{};
    assert(plans.publish(bytes, 9, publishedHash) == CourseRemovalPlanStorageResult::Ok);
    assert(publishedHash == initial.planHash);
    const auto initialRenames = state.renames;
    HalCourseRemovalBoundParticipant bound(journal, plans, refs, scratch, permitted, &allowed);
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
    if (fault == 1) state.failRename = initialRenames + 1;
    if (fault == 2) state.failRenameAfter = initialRenames + 1;
    if (fault == 3) state.failRenameAfter = initialRenames + 3;
    if (fault == 10) state.failRename = initialRenames + 3;
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
    if (fault == 19) state.directoryErrorPath = "/.crosspoint/companion";
    if (fault == 20) state.failClosePath = COURSE_BINDING_PATH;
    if (fault == 21) state.failSyncPath = COURSE_BINDING_PATH;
    if (fault == 22) state.files["/.crosspoint/companion/COURSE-BINDING"] = state.files[COURSE_BINDING_PATH];
    if (fault == 23) state.directories.erase("/tinta");
    ContentRemoval removal(journal, bound);
    const auto result = removal.remove(initial);
    assert((result == ContentRemovalJournalResult::Ok) == (fault == 0 || fault == 8));
    if ((fault >= 4 && fault <= 7) || fault >= 11) {
      assert(!journal.current());
      assert(state.renames == initialRenames);
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
      state.directoryErrorPath.clear();
      state.files.erase("/.crosspoint/companion/COURSE-BINDING");
      state.directories["/tinta"] = {};
      allowed = true;
    }
    ContentRemovalJournal recovered(journalStorage, journalScratch);
    HalCourseRemovalStorage restoredMembers(recovered, bytes, scratch);
    HalCourseRemovalBaseline restoredBaseline(recovered, scratch, permitted, &allowed);
    HalCourseRemovalReferences restoredRefs(recovered, metadata, metadataScratch, isolation, proofs, restoredBaseline,
                                            permitted, &allowed);
    CourseRemovalParticipant restored(recovered, restoredMembers, restoredRefs);
    assert(restored.bind(plan, initial.planHash));
    HalCourseRemovalBoundParticipant restoredBound(recovered, plans, restoredRefs, scratch, permitted, &allowed);
    ContentRemoval retry(recovered, restoredBound);
    const auto recoveredResult = recovered.recover(request.generation);
    if (recoveredResult == ContentRemovalJournalResult::Ok) {
      const auto checkpoint = *recovered.current();
      HalCourseRemovalRecovery recovery(recovered, metadata, metadataScratch, permitted, &allowed);
      assert(recovery.run(checkpoint));
      assert(recovery.closeReaders());
    } else {
      assert(recoveredResult == ContentRemovalJournalResult::Missing);
      assert(retry.remove(initial) == ContentRemovalJournalResult::Ok);
    }
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
