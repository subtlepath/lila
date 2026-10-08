#include <openssl/evp.h>

#include <cassert>
#include <string>

#include "lib/hal/HalContentRemovalJournalStorage.h"
#include "lib/hal/HalContentRemovalStartupRecovery.h"
#include "lib/hal/HalContentRemovalTransactions.h"
#include "lib/hal/HalEpubRemovalReferences.h"
using namespace companion;
static void cohortMetadata() {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  state.directories["/"] = {};
  state.directories["/.crosspoint"] = {};
  const std::string recent = R"({"books":[{"path":"/one.epub"},{"path":"/two.epub"},{"path":"/keep.epub"}]})";
  state.files["/.crosspoint/recent.json"] = {recent.begin(), recent.end()};
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalBytes{};
  std::array<uint8_t, REMOVAL_METADATA_SNAPSHOT_SIZE> declarations{};
  std::array<uint8_t, 128> io{};
  HalContentRemovalJournalStorage storage;
  ContentRemovalJournal journal(storage, journalBytes);
  ContentRemovalRecord initial;
  initial.request.transaction.fill(1);
  initial.request.owner.fill(2);
  initial.request.generation.fill(3);
  initial.request.manifest.kind = ContentKind::Epub;
  initial.request.manifest.formatVersion = 1;
  initial.request.manifest.length = 1;
  initial.request.manifest.contentHash.fill(4);
  initial.planHash.fill(5);
  assert(journal.begin(initial) == ContentRemovalJournalResult::Ok);
  assert(journal.advance(ContentRemovalPhase::Quarantined) == ContentRemovalJournalResult::Ok);
  HalEpubRemovalReferences references(journal, declarations, io);
  auto match = [](void*, std::string_view path, bool& matched) {
    matched = path == "/one.epub" || path == "/two.epub";
    return true;
  };
  assert(references.publishMatching(*journal.current(), match, nullptr));
  assert(references.publishMatching(*journal.current(), match, nullptr));
  const auto& bytes = state.files.at("/.crosspoint/recent.json");
  const std::string result(bytes.begin(), bytes.end());
  assert(result == R"({"books":[{"path":"/keep.epub"}]})");
}
int main() {
  cohortMetadata();
  {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = state.falseExists = true;
    state.directories["/"] = {};
    auto startup = makeUniqueNoThrow<HalContentRemovalStartupRecovery>();
    assert(startup);
    bool pending = true;
    assert(startup->pending(pending) && !pending);
    Identity generation{};
    generation.fill(3);
    assert(startup->run(generation));
    state.directoryErrorPath = TRANSFER_DIRECTORY;
    assert(!startup->pending(pending));
    state.directoryErrorPath.clear();
    state.files[CONTENT_REMOVAL_JOURNALS[0]] = {1, 2};
    const auto before = state.files;
    assert(!startup->run(generation));
    assert(state.files == before);
  }
  for (unsigned fault = 0; fault < 9; ++fault) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = state.falseExists = true;
    state.directories["/"] = {};
    state.directories["/Books"] = {};
    state.directories["/.crosspoint"] = {};
    const char* book = "/Books/caf\xc3\xa9.epub";
    const std::vector<uint8_t> content{1, 2, 3, 4, 5};
    state.files[book] = content;
    const std::string appState = R"({"openEpubPath":"/books/CAF\u00c9.EPUB","other":99})";
    const std::string recent = R"({"books":[{"path":"/Books/caf\u00e9.epub"},{"path":"/kept.epub","progress":33}]})";
    state.files["/.crosspoint/state.json"] = {appState.begin(), appState.end()};
    state.files["/.crosspoint/recent.json"] = {recent.begin(), recent.end()};
    state.files["/.crosspoint/bookmarks"] = {9, 8, 7};
    ContentRemovalRecord initial;
    initial.request.transaction.fill(1);
    initial.request.owner.fill(2);
    initial.request.generation.fill(3);
    initial.request.manifest.kind = ContentKind::Epub;
    initial.request.manifest.formatVersion = 1;
    initial.request.manifest.length = content.size();
    assert(EVP_Digest(content.data(), content.size(), initial.request.manifest.contentHash.data(), nullptr,
                      EVP_sha256(), nullptr) == 1);
    std::array<uint8_t, SINGLE_FILE_REMOVAL_PLAN_MAX_SIZE> plan{};
    const auto size = encodeSingleFileRemovalPlan({initial.request, book}, plan);
    assert(size);
    assert(EVP_Digest(plan.data(), size, initial.planHash.data(), nullptr, EVP_sha256(), nullptr) == 1);
    std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalBytes{};
    std::array<uint8_t, REMOVAL_METADATA_SNAPSHOT_SIZE> declarationBytes{};
    std::array<uint8_t, 128> io{};
    std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> completionBytes{}, releaseBytes{};
    HalSingleFileRemovalPlanStorage planStorage(io);
    Digest persistedHash{};
    assert(planStorage.publish(std::span(plan).first(size), persistedHash) == RemovalPlanStorageResult::Ok);
    assert(persistedHash == initial.planHash);
    const std::string persistedPath = planStorage.publishedPath();
    HalCompletedContentRemovals completions(completionBytes);
    HalContentRemovalJournalStorage journalStorage;
    ContentRemovalJournal journal(journalStorage, journalBytes);
    std::array<char, 112> candidate{}, backup{};
    assert(removalMetadataPaths(initial.planHash, RemovalMetadataFile::State, candidate, backup));
    if (fault == 1) state.failRemoveAfter = true;
    if (fault == 2) state.failSyncPath = candidate.data();
    if (fault == 3) state.failClosePath = "/.crosspoint/state.json";
    if (fault == 4) state.failRenameAfter = 2;
    if (fault == 5) state.readErrorPath = "/.crosspoint/state.json";
    if (fault == 6) state.corruptWritePath = candidate.data();
    if (fault == 7) state.files[backup.data()] = {11};
    if (fault == 8) state.failRemove = true;
    {
      HalEpubRemovalReferences references(journal, declarationBytes, io);
      HalEpubRemovalParticipant participant(journal, references, io);
      assert(participant.bind(std::span(plan).first(size), initial.planHash));
      HalContentRemovalTransactions removal(initial.request.generation, journal, journalStorage, completions,
                                            participant, releaseBytes);
      const auto result = removal.remove(initial);
      assert((result == ContentRemovalJournalResult::Ok) == !fault);
      if (fault) {
        assert(std::any_of(state.files.begin(), state.files.end(), [](const auto& entry) {
          return entry.first.starts_with("/.crosspoint/companion/removal-bytes-");
        }));
      }
    }
    if (fault == 8) {
      assert(journal.current()->phase == ContentRemovalPhase::Committed);
      std::array<char, 112> recentCandidate{}, recentBackup{};
      assert(removalMetadataPaths(initial.planHash, RemovalMetadataFile::Recent, recentCandidate, recentBackup));
      const auto correct = state.files.at(recentBackup.data());
      state.files[recentBackup.data()][0] ^= 1;
      state.failRemove = false;
      const auto before = state.files;
      HalEpubRemovalReferences guarded(journal, declarationBytes, io);
      assert(!guarded.retire(*journal.current(), book));
      assert(state.files == before);
      state.files[recentBackup.data()] = correct;
    }
    state.failRemoveAfter = false;
    state.failSyncPath.clear();
    state.failClosePath.clear();
    state.readErrorPath.clear();
    state.corruptWritePath.clear();
    state.failRenameAfter = 0;
    if (fault == 7) {
      assert(state.files.at(backup.data()) == std::vector<uint8_t>({11}));
      // Simulate explicit external resolution of the conflicting private file.
      state.files.erase(backup.data());
    }
    {
      auto startup = makeUniqueNoThrow<HalContentRemovalStartupRecovery>();
      assert(startup);
      bool pending = false;
      assert(startup->pending(pending) && pending);
      auto wrongGeneration = initial.request.generation;
      wrongGeneration.fill(99);
      const auto before = state.files;
      assert(!startup->run(wrongGeneration));
      assert(state.files == before);
      if (fault == 2) {
        state.files[persistedPath][0] ^= 1;
        const auto corrupt = state.files;
        assert(!startup->run(initial.request.generation));
        assert(state.files == corrupt);
        state.files[persistedPath][0] ^= 1;
      }
      if (!fault) {
        state.failRemoveAfter = true;
        assert(!startup->run(initial.request.generation));
        state.failRemoveAfter = false;
        startup = makeUniqueNoThrow<HalContentRemovalStartupRecovery>();
        assert(startup);
      }
      if (fault == 8) {
        std::string receiptStage = "/.crosspoint/companion/removal-done-";
        for (unsigned at = 0; at < initial.request.transaction.size(); ++at) receiptStage += "01";
        state.failSyncPath = receiptStage + ".tmp";
        assert(!startup->run(initial.request.generation));
        state.failSyncPath.clear();
        startup = makeUniqueNoThrow<HalContentRemovalStartupRecovery>();
        assert(startup);
      }
      assert(startup->run(initial.request.generation));
      assert(startup->pending(pending) && !pending);
      const auto completedFiles = state.files;
      assert(startup->run(initial.request.generation));
      assert(state.files == completedFiles);
    }
    ContentRemovalJournal recovered(journalStorage, journalBytes);
    HalEpubRemovalReferences references(recovered, declarationBytes, io);
    HalEpubRemovalParticipant participant(recovered, references, io);
    assert(participant.bind(std::span(plan).first(size), initial.planHash));
    HalContentRemovalTransactions removal(initial.request.generation, recovered, journalStorage, completions,
                                          participant, releaseBytes);
    assert(removal.remove(initial) == ContentRemovalJournalResult::Ok);
    ContentRemovalRecord completedRecord;
    assert(removal.lookup(initial.request, completedRecord) == CompletedRemovalResult::Ok);
    assert(completedRecord.phase == ContentRemovalPhase::Retired);
    assert(recovered.recover(initial.request.generation) == ContentRemovalJournalResult::Missing);
    assert(!state.files.contains(book));
    for (const auto& [path, bytes] : state.files) {
      assert(!path.starts_with("/.crosspoint/companion/removal-bytes-"));
      if (path.starts_with("/.crosspoint/companion/removal-meta-"))
        assert(!path.ends_with(".old") && !path.ends_with(".next"));
    }
    const auto& stateBytes = state.files.at("/.crosspoint/state.json");
    const std::string app(stateBytes.begin(), stateBytes.end());
    assert(app == R"({"openEpubPath":"","other":99})");
    const auto& recentBytes = state.files.at("/.crosspoint/recent.json");
    assert(std::string(recentBytes.begin(), recentBytes.end()) == R"({"books":[{"path":"/kept.epub","progress":33}]})");
    assert(state.files.at("/.crosspoint/bookmarks") == std::vector<uint8_t>({9, 8, 7}));
    const std::string changedState = R"({"openEpubPath":"/other.epub","other":100})";
    state.files["/.crosspoint/state.json"] = {changedState.begin(), changedState.end()};
    const auto renames = state.renames;
    assert(removal.remove(initial) == ContentRemovalJournalResult::Ok);
    assert(state.renames == renames);
    assert(std::string(state.files.at("/.crosspoint/state.json").begin(),
                       state.files.at("/.crosspoint/state.json").end()) == changedState);
    if (!fault) {
      const char* secondBook = "/Books/second.epub";
      state.files[secondBook] = content;
      const std::string secondRecent =
          R"({"books":[{"path":"/Books/second.epub"},{"path":"/kept.epub","progress":33}]})";
      state.files["/.crosspoint/recent.json"] = {secondRecent.begin(), secondRecent.end()};
      auto second = initial;
      second.request.transaction.fill(9);
      const auto secondSize = encodeSingleFileRemovalPlan({second.request, secondBook}, plan);
      assert(secondSize);
      assert(EVP_Digest(plan.data(), secondSize, second.planHash.data(), nullptr, EVP_sha256(), nullptr) == 1);
      HalEpubRemovalReferences secondReferences(recovered, declarationBytes, io);
      HalEpubRemovalParticipant secondParticipant(recovered, secondReferences, io);
      assert(secondParticipant.bind(std::span(plan).first(secondSize), second.planHash));
      HalContentRemovalTransactions secondRemoval(second.request.generation, recovered, journalStorage, completions,
                                                  secondParticipant, releaseBytes);
      assert(secondRemoval.remove(second) == ContentRemovalJournalResult::Ok);
      assert(!state.files.contains(secondBook));
      assert(std::string(state.files.at("/.crosspoint/state.json").begin(),
                         state.files.at("/.crosspoint/state.json").end()) == changedState);
      const auto afterSecond = state.files;
      assert(removal.remove(initial) == ContentRemovalJournalResult::Ok);
      assert(state.files == afterSecond);
      assert(recovered.current()->request == second.request);
    }
  }
}
