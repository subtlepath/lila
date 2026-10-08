#include <cassert>
#include <memory>

#include "lib/hal/HalContentRemovalJournalStorage.h"
#include "lib/hal/HalContentRemovalStartupRecovery.h"
#include "lib/hal/HalEpubRemovalAdmission.h"
#include "lib/hal/HalEpubRemovalBackend.h"
#include "lib/hal/HalEpubRemovalCohortParticipant.h"
#include "lib/hal/HalEpubRemovalParticipant.h"
#include "lib/hal/HalEpubRemovalSession.h"
#include "lib/hal/HalMultiPathRemovalPlanStorage.h"
#include "lib/hal/HalMultiPathRemovalPlanWriter.h"
#include "lib/hal/HalRemovalCohortAddress.h"
#include "lib/hal/HalRemovalPlanPathMatcher.h"
using namespace companion;

struct UnavailableInventory final : InventoryIndexStorage {
  unsigned calls = 0;
  std::vector<uint8_t> bytes;
  bool size(uint64_t& output) override {
    ++calls;
    output = bytes.size();
    return !bytes.empty();
  }
  bool read(uint64_t at, std::span<uint8_t> output) override {
    ++calls;
    if (at > bytes.size() || output.size() > bytes.size() - at) return false;
    std::copy_n(bytes.begin() + at, output.size(), output.begin());
    return true;
  }
};
struct NoReferences final : EpubRemovalReferences {
  bool publish(const ContentRemovalRecord&, const char*) override { return false; }
  bool verify(const ContentRemovalRecord&, const char*) override { return false; }
  bool retire(const ContentRemovalRecord&, const char*) override { return false; }
  bool verifyRetired(const ContentRemovalRecord&, const char*) override { return false; }
};
static void quarantineCohort(unsigned fault) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  state.directories["/"] = {};
  state.directories["/Books"] = {};
  state.directories["/.crosspoint"] = {};
  const std::vector<uint8_t> content{1, 2, 3};
  state.files["/Books/one.epub"] = content;
  state.files["/Books/two.epub"] = content;
  ContentRemovalRecord initial;
  initial.request.transaction.fill(1);
  initial.request.owner.fill(2);
  initial.request.generation.fill(3);
  initial.request.manifest.kind = ContentKind::Epub;
  initial.request.manifest.formatVersion = 1;
  initial.request.manifest.length = content.size();
  EVP_Digest(content.data(), content.size(), initial.request.manifest.contentHash.data(), nullptr, EVP_sha256(),
             nullptr);
  auto writer = std::make_unique<HalMultiPathRemovalPlanWriter>(4096);
  assert(writer->begin(initial.request, 7));
  assert(writer->append("/Books/one.epub"));
  assert(writer->append("/Books/two.epub"));
  assert(writer->finish(2));
  initial.planHash = *writer->publishedDigest();
  auto reader = std::make_unique<HalMultiPathRemovalPlanStorage>();
  assert(reader->open(initial.planHash, initial.request) == MultiPathRemovalStorageResult::Ok);
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalBytes{};
  std::array<uint8_t, 128> io{};
  HalContentRemovalJournalStorage storage;
  ContentRemovalJournal journal(storage, journalBytes);
  UnavailableInventory unavailable;
  std::array<uint8_t, INVENTORY_PATH_MAX_RECORD> pathScratch{};
  InventoryPaths paths(unavailable, pathScratch);
  RemovalPathCollection collection(paths, *writer);
  struct InventoryPreparation {
    unsigned calls = 0;
    bool ready = false;
  } preparation;
  HalEpubRemovalAdmission admission(
      initial.request.generation, journal, collection, *writer, *reader,
      [](void* opaque, uint64_t& revision) {
        auto& state = *static_cast<InventoryPreparation*>(opaque);
        ++state.calls;
        revision = 7;
        return state.ready;
      },
      &preparation);
  ContentRemovalRecord admitted;
  unavailable.bytes.resize(INVENTORY_INDEX_HEADER_SIZE + 2 * INVENTORY_PATH_MAX_RECORD);
  size_t extent = INVENTORY_INDEX_HEADER_SIZE;
  for (const char* path : {"/Books/one.epub", "/Books/two.epub"})
    extent += encodeInventoryPath(initial.request.manifest, path, std::span(unavailable.bytes).subspan(extent));
  unavailable.bytes.resize(extent);
  InventoryIndexHeader header{initial.request.generation, 7, 2,
                              inventoryIndexCrc(std::span(unavailable.bytes).subspan(INVENTORY_INDEX_HEADER_SIZE))};
  assert(encodeInventoryPathsHeader(header, unavailable.bytes) == INVENTORY_INDEX_HEADER_SIZE);
  const auto preservedAdmission = state.files;
  assert(admission.admit(initial.request, 7, admitted) == EpubRemovalAdmissionResult::IoError);
  assert(state.files == preservedAdmission && preparation.calls == 1);
  preparation.ready = true;
  assert(admission.admit(initial.request, 0, admitted) == EpubRemovalAdmissionResult::Ready);
  assert(admitted == initial && unavailable.calls > 0 && preparation.calls == 2);
  unavailable.bytes.clear();
  unavailable.calls = 0;
  assert(journal.begin(initial) == ContentRemovalJournalResult::Ok);

  assert(admission.admit(initial.request, 0, admitted) == EpubRemovalAdmissionResult::Ready);
  assert(admitted == initial && unavailable.calls == 0 && preparation.calls == 2);
  auto otherRequest = initial.request;
  otherRequest.transaction[0] ^= 1;
  assert(admission.admit(otherRequest, 7, admitted) == EpubRemovalAdmissionResult::Busy);
  assert(admitted == initial && unavailable.calls == 0 && preparation.calls == 2);

  NoReferences references;
  auto participant = std::make_unique<HalEpubRemovalParticipant>(journal, references, io);
  for (uint64_t ordinal = 0; ordinal < 2; ++ordinal) {
    assert(participant->bindCohortPath(*reader, initial.planHash, ordinal));
    assert(participant->verifyPlan(initial));
    if (fault == 1 && ordinal == 0) {
      state.failRenameAfter = state.renames + 1;
      assert(!participant->quarantine(initial));
      state.failRenameAfter = 0;
    }
    if (fault == 2 && ordinal == 1) {
      state.failRename = state.renames + 1;
      assert(!participant->quarantine(initial));
      state.failRename = 0;
    }
    assert(participant->quarantine(initial));
    assert(participant->verifyQuarantined(initial));
    assert(participant->unbind());
  }
  assert(!state.files.contains("/Books/one.epub") && !state.files.contains("/Books/two.epub"));
  unsigned backups = 0;
  for (const auto& [path, bytes] : state.files) {
    if (path.starts_with("/.crosspoint/companion/removal-cohort-bytes-")) {
      assert(bytes == content);
      ++backups;
    }
  }
  assert(backups == 2);
  participant.reset();
  participant = std::make_unique<HalEpubRemovalParticipant>(journal, references, io);
  for (uint64_t ordinal = 0; ordinal < 2; ++ordinal) {
    assert(participant->bindCohortPath(*reader, initial.planHash, ordinal));
    assert(participant->verifyPlan(initial));
    assert(participant->quarantine(initial));
    assert(participant->verifyQuarantined(initial));
    assert(participant->unbind());
  }
  assert(!participant->bindCohortPath(*reader, initial.planHash, 2));
  auto wrong = initial.planHash;
  wrong[0] ^= 1;
  assert(!participant->bindCohortPath(*reader, wrong, 0));
  participant.reset();
  const std::string recent =
      R"({"books":[{"path":"/Books/one.epub"},{"path":"/Books/two.epub"},{"path":"/keep.epub"}]})";
  state.files["/.crosspoint/recent.json"] = {recent.begin(), recent.end()};
  std::array<uint8_t, REMOVAL_METADATA_SNAPSHOT_SIZE> declarationBytes{};
  auto metadata = std::make_unique<HalEpubRemovalReferences>(journal, declarationBytes, io);
  auto cohort = std::make_unique<HalEpubRemovalCohortParticipant>(journal, *reader, *metadata, io);
  ContentRemoval removal(journal, *cohort);
  {
    std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> completionBytes{}, releaseBytes{};
    HalCompletedContentRemovals completions(completionBytes);
    HalContentRemovalTransactions transactions(initial.request.generation, journal, storage, completions, *cohort,
                                               releaseBytes);
    auto competing = initial.request;
    competing.transaction[0] ^= 1;
    const auto preserved = state.files;
    assert(transactions.prepare(competing) == ContentRemovalJournalResult::Conflict);
    assert(state.files == preserved);
    assert(transactions.prepare(initial.request) == ContentRemovalJournalResult::Ok);
    assert(state.files == preserved);
  }

  if (fault == 3) state.failSyncPath = "/.crosspoint/recent.json";
  if (fault == 4) state.failRenameAfter = state.renames + 1;
  if (fault == 5) state.failRemoveAfter = true;
  if (fault >= 3) {
    assert(removal.remove(initial) != ContentRemovalJournalResult::Ok);
    state.failSyncPath.clear();
    state.failRenameAfter = 0;
    state.failRemoveAfter = false;
  }
  if (fault >= 3) {
    auto startup = std::make_unique<HalContentRemovalStartupRecovery>();
    const auto preserved = state.files;
    auto wrongGeneration = initial.request.generation;
    wrongGeneration[0] ^= 1;
    assert(!startup->run(wrongGeneration));
    assert(state.files == preserved);
    state.files.at(writer->publishedPath()).back() ^= 1;
    const auto damaged = state.files;
    assert(!startup->run(initial.request.generation));
    assert(state.files == damaged);
    state.files.at(writer->publishedPath()).back() ^= 1;
    assert(startup->run(initial.request.generation));
    assert(startup->run(initial.request.generation));
    for (const char* path : CONTENT_REMOVAL_JOURNALS) assert(!state.files.contains(path));
  } else {
    assert(removal.remove(initial) == ContentRemovalJournalResult::Ok);
    assert(journal.current()->phase == ContentRemovalPhase::Retired);
    assert(removal.remove(initial) == ContentRemovalJournalResult::Ok);
    assert(reader->close());
    assert(admission.admit(initial.request, 0, admitted) == EpubRemovalAdmissionResult::Retired);
    assert(admitted == initial && unavailable.calls == 0 && preparation.calls == 2);
    if (fault == 0) {
      std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> completionBytes{}, releaseBytes{};
      HalCompletedContentRemovals completions(completionBytes);
      HalContentRemovalTransactions transactions(initial.request.generation, journal, storage, completions, *cohort,
                                                 releaseBytes);
      uint64_t revision = 7;
      struct Refresh {
        unsigned calls = 0;
        bool fail = false;
      } refreshed;
      HalEpubRemovalBackend backend(
          admission, transactions, revision, [](void*) { return true; },
          [](void* opaque) {
            auto& r = *static_cast<Refresh*>(opaque);
            ++r.calls;
            return !r.fail;
          },
          &refreshed);
      assert(backend.completed(initial.request) == ContentRemovalResult::NotFound);
      assert(backend.remove(initial.request) == ContentRemovalResult::Ok);
      assert(refreshed.calls == 1);
      refreshed.fail = true;
      assert(backend.completed(initial.request) == ContentRemovalResult::IoError);
      refreshed.fail = false;
      assert(backend.completed(initial.request) == ContentRemovalResult::Ok);
      assert(refreshed.calls == 3 && unavailable.calls == 0);
      auto next = initial.request;
      next.transaction[0] ^= 1;
      assert(transactions.prepare(next) == ContentRemovalJournalResult::Ok);
      assert(!journal.current());
      for (const char* path : CONTENT_REMOVAL_JOURNALS) assert(!state.files.contains(path));
      assert(backend.completed(initial.request) == ContentRemovalResult::Ok);
      auto session = std::make_unique<HalEpubRemovalSession>(
          initial.request.generation, paths, revision, 4096, [](void*) { return true; }, [](void*) { return true; },
          nullptr);
      assert(session->prepare());
      std::array<uint8_t, CONTENT_REMOVAL_REQUEST_SIZE> body{};
      std::array<uint8_t, CONTENT_REMOVAL_REPLY_SIZE> reply{};
      assert(encodeContentRemovalRequest(initial.request, body) == body.size());
      const auto before = state.files;
      assert(session->handle(false, initial.request.owner, body, reply) == reply.size());
      assert(reply[0] == static_cast<uint8_t>(ContentRemovalResult::Unauthorized));
      assert(state.files == before);
      assert(session->handle(true, initial.request.owner, body, reply) == reply.size());
      assert(reply[0] == static_cast<uint8_t>(ContentRemovalResult::Ok));
      assert(std::equal(initial.request.transaction.begin(), initial.request.transaction.end(), reply.begin() + 1));
      assert(unavailable.calls == 0);
    }
  }
  for (const auto& [path, bytes] : state.files) {
    assert(!path.starts_with("/.crosspoint/companion/removal-cohort-bytes-"));
  }
  const auto& recentBytes = state.files.at("/.crosspoint/recent.json");
  assert(std::string(recentBytes.begin(), recentBytes.end()) == R"({"books":[{"path":"/keep.epub"}]})");
}
int main(int argc, char** argv) {
  assert(argc == 5);
  for (unsigned fault = 0; fault < 6; ++fault) quarantineCohort(fault);
  ContentRemovalRequest request;
  request.transaction.fill(1);
  request.owner.fill(2);
  request.generation.fill(3);
  request.manifest.kind = ContentKind::Epub;
  request.manifest.formatVersion = 1;
  request.manifest.length = 1234;
  request.manifest.contentHash.fill(4);
  auto& state = inventory_hal_test::state;
  for (unsigned fault = 0; fault < 7; ++fault) {
    state = {};
    state.enumerateFileMap = true;
    state.directories["/"] = {};
    state.files["/Books/a.epub"] = {1, 2, 3};
    auto writer = std::make_unique<HalMultiPathRemovalPlanWriter>(4096);
    assert(writer->begin(request, 9));
    assert(!writer->publishedPath() && !writer->publishedDigest());
    assert(writer->append("/Books/a.epub"));
    assert(writer->append("/Books/b.epub"));
    if (fault == 1) state.failWrite = true;
    if (fault == 2) state.failSync = true;
    if (fault == 3) state.failClose = true;
    if (fault == 4) state.failTruncate = true;
    if (fault == 5) state.failRename = state.renames + 1;
    if (fault == 6) state.failRenameAfter = state.renames + 1;
    const bool success = writer->finish(2);
    assert(success == (fault == 0));
    assert(state.files.at("/Books/a.epub") == std::vector<uint8_t>({1, 2, 3}));
    if (success) {
      const std::string target = writer->publishedPath();
      const auto bytes = state.files.at(target);
      const unsigned renames = state.renames;
      assert(writer->begin(request, 9));
      assert(writer->append("/Books/a.epub"));
      assert(writer->append("/Books/b.epub"));
      assert(writer->finish(2));
      assert(state.renames == renames);
      assert(state.files.at(target) == bytes);
    } else {
      assert(!writer->publishedPath() && !writer->publishedDigest());
      state.failWrite = state.failSync = state.failClose = state.failTruncate = false;
      state.failRename = state.failRenameAfter = 0;
      assert(!writer->begin(request, 10));
      assert(writer->discard());
      assert(writer->begin(request, 9));
      assert(writer->append("/Books/a.epub"));
      assert(writer->append("/Books/b.epub"));
      assert(writer->finish(2));
    }
  }
  state = {};
  state.enumerateFileMap = true;
  state.directories["/"] = {};
  auto writer = std::make_unique<HalMultiPathRemovalPlanWriter>(4096);
  assert(writer->begin(request, 9));
  assert(writer->append("/Books/A.epub"));
  assert(!writer->append("/books/a.epub"));
  assert(!writer->finish(1));
  assert(writer->discard());
  auto quota = std::make_unique<HalMultiPathRemovalPlanWriter>(MULTI_PATH_REMOVAL_HEADER_SIZE + 8);
  assert(quota->begin(request, 9));
  assert(!quota->append("/Books/a.epub"));
  assert(quota->discard());
  assert(quota->setMaximumBytes(4096));
  assert(quota->begin(request, 9));
  assert(!quota->setMaximumBytes(8192));
  assert(!quota->append("/Books/a.epub"));
  assert(quota->discard());

  state = {};
  state.enumerateFileMap = true;
  state.directories["/"] = {};
  auto golden = std::make_unique<HalMultiPathRemovalPlanWriter>(4096);
  assert(golden->begin(request, 7));
  assert(golden->append("/Books/original.epub"));
  assert(golden->append("/elsewhere/renamed.epub"));
  assert(golden->finish(2));
  const std::string publishedPath = golden->publishedPath();
  const Digest digest = *golden->publishedDigest();
  mbedtls_sha256_context addressHash;
  mbedtls_sha256_init(&addressHash);
  std::array<char, 112> address{};
  unsigned argument = 2;
  for (uint64_t ordinal : {uint64_t{0}, uint64_t{1}, UINT64_MAX}) {
    assert(removalCohortAddress(addressHash, digest, ordinal, address));
    assert(std::string(address.data()) == argv[argument++]);
  }
  address.fill('x');
  assert(!removalCohortAddress(addressHash, Digest{}, 0, address));
  assert(address.front() == 'x');
  assert(!removalCohortAddress(addressHash, digest, 0, std::span(address).first(20)));
  assert(address.front() == 'x');
  mbedtls_sha256_free(&addressHash);

  auto loaded = std::make_unique<HalMultiPathRemovalPlanStorage>();
  assert(loaded->open(digest, request) == MultiPathRemovalStorageResult::Ok);
  assert(loaded->current()->count == 2);
  bool allowed = true;
  auto matcher = std::make_unique<HalRemovalPlanPathMatcher>(
      *loaded, [](void* opaque) { return *static_cast<bool*>(opaque); }, &allowed);
  bool matched = false;
  assert(matcher->match("/ELSEWHERE/renamed.epub", matched) && matched);
  assert(matcher->match("/Books/original.epub", matched) && matched);
  assert(matcher->match("/Books/unrelated.epub", matched) && !matched);
  matched = true;
  allowed = false;
  assert(!matcher->match("/Books/original.epub", matched));
  assert(matched);
  allowed = true;
  assert(loaded->rewind());

  std::array<char, 512> path{};
  assert(loaded->next(path) == InventoryPathRecordResult::Entry);
  assert(std::string(path.data()) == "/Books/original.epub");
  assert(loaded->next(path) == InventoryPathRecordResult::Entry);
  assert(std::string(path.data()) == "/elsewhere/renamed.epub");
  assert(loaded->next(path) == InventoryPathRecordResult::End);
  assert(loaded->rewind());
  auto wrong = request;
  wrong.owner[0] ^= 1;
  assert(loaded->open(digest, wrong) == MultiPathRemovalStorageResult::Corrupt);
  assert(!loaded->current());
  assert(loaded->open(digest, request) == MultiPathRemovalStorageResult::Ok);
  state.files.at(publishedPath).back() ^= 1;
  assert(!loaded->rewind());
  assert(!loaded->current());
  state.files.at(publishedPath).back() ^= 1;
  assert(loaded->open(digest, request) == MultiPathRemovalStorageResult::Ok);
  assert(loaded->close());
  for (unsigned failure = 0; failure < 4; ++failure) {
    if (failure == 0) state.readErrorPath = publishedPath;
    if (failure == 1) state.failSyncPath = publishedPath;
    if (failure == 3) state.statErrorPath = publishedPath;
    if (failure == 2) {
      assert(loaded->open(digest, request) == MultiPathRemovalStorageResult::Ok);
      state.failClosePath = publishedPath;
      assert(!loaded->close());
    } else {
      assert(loaded->open(digest, request) == MultiPathRemovalStorageResult::IoError);
    }
    assert(!loaded->current());
    state.readErrorPath.clear();
    state.failSyncPath.clear();
    state.failClosePath.clear();
    state.statErrorPath.clear();
    assert(loaded->open(digest, request) == MultiPathRemovalStorageResult::Ok);
    assert(loaded->close());
  }
  state.files.at(publishedPath).push_back(0);
  assert(loaded->open(digest, request) == MultiPathRemovalStorageResult::Corrupt);
  assert(!loaded->current());
  state.files.at(publishedPath).pop_back();
  auto absentDigest = digest;
  absentDigest[0] ^= 1;
  assert(loaded->open(absentDigest, request) == MultiPathRemovalStorageResult::Missing);
  assert(!loaded->current());
  assert(loaded->open(Digest{}, request) == MultiPathRemovalStorageResult::Invalid);

  const auto published = state.files.at(golden->publishedPath());
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  std::string hex;
  for (uint8_t byte : published) {
    hex += HEX_DIGITS[byte >> 4];
    hex += HEX_DIGITS[byte & 15];
  }
  assert(hex == argv[1]);
  std::array<uint8_t, REMOVAL_STAGE_CLAIM_SIZE> scratch{};
  HalRemovalStageClaimStorage claims(scratch);
  assert(claims.load({request, 7}) == RemovalStageClaimResult::Ok);
  const std::string stage = claims.planStagePath();
  auto foreign = published;
  // Keep a valid header CRC while changing its complete request owner.
  foreign[8 + 20] ^= 1;
  inventory_detail::write(foreign, 151, inventoryIndexCrc(std::span(foreign).first(151)), 4);
  state.files[stage] = foreign;
  assert(!golden->begin(request, 7));
  assert(golden->discard());
  assert(state.files.at(stage) == foreign);
  assert(state.files.at(claims.markerPath()).size() == REMOVAL_STAGE_CLAIM_SIZE);
  assert(state.files.at(golden->publishedPath()
                            ? golden->publishedPath()
                            : "/.crosspoint/companion/"
                              "removal-plan-dc005ea34d8791784814f4251a51d975fd6229230a979715bfe6a6798cec5e27") ==
         published);
}
