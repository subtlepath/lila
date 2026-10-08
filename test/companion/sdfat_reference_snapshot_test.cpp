#include <cassert>
#include <string>

#include "lib/hal/HalContentRemovalJournalStorage.h"
#include "lib/hal/HalEpubReferenceSnapshot.h"
#include "lib/hal/HalRemovalMetadataAuthorization.h"
#include "lib/hal/HalRemovalMetadataPublisher.h"
using namespace companion;
int main() {
  auto match = [](void*, std::string_view path, bool& matched) {
    matched = path == "/gone.epub" || path == "/second.epub";
    return true;
  };
  for (const bool cohort : {false, true}) {
    for (const auto kind : {RemovalMetadataFile::State, RemovalMetadataFile::Recent}) {
      for (unsigned fault = 0; fault < 14; ++fault) {
        auto& state = inventory_hal_test::state;
        state = {};
        state.enumerateFileMap = state.falseExists = true;
        state.directories["/.crosspoint"] = {};
        const char* target =
            kind == RemovalMetadataFile::State ? "/.crosspoint/state.json" : "/.crosspoint/recent.json";
        const std::string original =
            kind == RemovalMetadataFile::State
                ? R"({"openEpubPath":"/gone.epub","other":123})"
                : R"({"books":[{"path":"/gone.epub"},{"path":"/second.epub"},{"path":"/kept.epub","progress":33}],"other":123})";
        if (fault != 7) state.files[target] = {original.begin(), original.end()};
        if (fault == 8 || fault == 13) state.files[target] = {'{', '}'};
        state.files["/.crosspoint/history"] = {1, 2, 3};
        std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalBytes{};
        std::array<uint8_t, REMOVAL_METADATA_SNAPSHOT_SIZE> declarationBytes{};
        std::array<uint8_t, 128> scratch{};
        HalContentRemovalJournalStorage journalStorage;
        ContentRemovalJournal journal(journalStorage, journalBytes);
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
        HalRemovalMetadataSnapshotStorage declarations(declarationBytes);
        EpubReferenceJson json;
        RemovalMetadataSnapshot output;
        output.planHash.fill(0x99);
        const auto sentinel = output;
        std::array<char, 112> candidate{}, backup{};
        assert(removalMetadataPaths(initial.planHash, kind, candidate, backup));
        if (fault == 9) {
          RemovalMetadataSnapshot foreign;
          foreign.request = initial.request;
          foreign.request.owner.fill(9);
          foreign.planHash = initial.planHash;
          foreign.previousHash.fill(6);
          foreign.nextHash.fill(7);
          foreign.previousLength = 1;
          foreign.nextLength = 2;
          foreign.file = kind;
          assert(encodeRemovalMetadataSnapshot(foreign, declarationBytes) == declarationBytes.size());
          std::string receipt = "/.crosspoint/companion/removal-snapshot-";
          for (unsigned at = 0; at < 32; ++at) receipt += "05";
          receipt += kind == RemovalMetadataFile::State ? 's' : 'r';
          state.files[receipt] = {declarationBytes.begin(), declarationBytes.end()};
        }
        if (fault == 10) state.files[backup.data()] = {9};
        if (fault == 11) state.directories[candidate.data()] = {};
        if (fault == 12) state.files[candidate.data()] = std::vector<uint8_t>(REMOVAL_METADATA_MAX_BYTES + 1, 9);
        if (fault == 13) state.files[candidate.data()] = {9};
        const auto before = state.files;

        if (fault == 1) state.failWrite = true;
        if (fault == 2) state.corruptWrite = true;
        if (fault == 3) state.failSyncPath = target;
        if (fault == 4) state.failClosePath = target;
        if (fault == 5) state.readErrorPath = target;
        if (fault == 6) state.failRenameAfter = 1;
        {
          HalEpubReferenceSnapshot snapshots(journal, declarations, json, scratch);
          const bool result = cohort ? snapshots.prepareMatching(*journal.current(), kind, match, nullptr, output)
                                     : snapshots.prepare(*journal.current(), kind, "/gone.epub", output);
          assert(result == (!fault || fault == 7 || fault == 8));
          if (!result) assert(output == sentinel);
        }
        if (fault >= 9) {
          assert(state.files == before);
          continue;
        }
        if (fault != 7 && fault != 8)
          assert(state.files.at(target) == std::vector<uint8_t>(original.begin(), original.end()));
        assert(state.files.at("/.crosspoint/history") == std::vector<uint8_t>({1, 2, 3}));
        state.failWrite = state.corruptWrite = false;
        state.failSyncPath.clear();
        state.failClosePath.clear();
        state.readErrorPath.clear();
        state.failRenameAfter = 0;
        HalEpubReferenceSnapshot resumed(journal, declarations, json, scratch);
        assert(cohort ? resumed.prepareMatching(*journal.current(), kind, match, nullptr, output)
                      : resumed.prepare(*journal.current(), kind, "/gone.epub", output));
        HalRemovalMetadataAuthorization authorization(journal, declarations);
        assert(authorization.bind(output));
        HalRemovalMetadataPublisher publisher(scratch, HalRemovalMetadataAuthorization::check, &authorization);
        assert(publisher.bind(output));
        assert(publisher.publish());
        if (fault == 8) {
          assert(unchangedRemovalMetadataSnapshot(output));
          assert(state.files.at(target) == std::vector<uint8_t>({'{', '}'}));
        } else {
          const std::string final(state.files.at(target).begin(), state.files.at(target).end());
          assert(final.find("/gone.epub") == std::string::npos);
          if (cohort) assert(final.find("/second.epub") == std::string::npos);
          if (fault != 7) assert(final.find("\"other\":123") != std::string::npos);
        }
        assert(state.files.at("/.crosspoint/history") == std::vector<uint8_t>({1, 2, 3}));
      }
    }
  }
}
