#include <openssl/evp.h>

#include <cassert>

#include "lib/hal/HalContentRemovalJournalStorage.h"
#include "lib/hal/HalRemovalMetadataAuthorization.h"
#include "lib/hal/HalRemovalMetadataPublisher.h"
using namespace companion;
struct Authorization {
  RemovalMetadataSnapshot expected;
  unsigned calls = 0, rejectAt = 0;
  bool permitted = true;
  static bool check(void* context, const RemovalMetadataSnapshot& value) {
    auto& self = *static_cast<Authorization*>(context);
    if (++self.calls == self.rejectAt) self.permitted = false;
    return self.permitted && value == self.expected;
  }
};
static Digest digest(const std::vector<uint8_t>& bytes) {
  Digest result;
  assert(EVP_Digest(bytes.data(), bytes.size(), result.data(), nullptr, EVP_sha256(), nullptr) == 1);
  return result;
}
int main() {
  for (const auto location : {RemovalMetadataFile::State, RemovalMetadataFile::Recent}) {
    for (unsigned fault = 0; fault < 15; ++fault) {
      for (unsigned interrupt = 0; interrupt < (fault ? 1 : 80); ++interrupt) {
        auto& state = inventory_hal_test::state;
        state = {};
        state.enumerateFileMap = state.falseExists = true;
        state.directories["/.crosspoint"] = {};
        state.directories["/.crosspoint/companion"] = {};
        const char* target =
            location == RemovalMetadataFile::State ? "/.crosspoint/state.json" : "/.crosspoint/recent.json";
        const std::vector<uint8_t> previous{'o', 'l', 'd'}, next{'n', 'e', 'w'};
        Authorization proof;
        auto& expected = proof.expected;
        expected.request.transaction.fill(1);
        expected.request.owner.fill(2);
        expected.request.generation.fill(3);
        expected.request.manifest.kind = ContentKind::Epub;
        expected.request.manifest.formatVersion = 1;
        expected.request.manifest.length = 1;
        expected.request.manifest.contentHash.fill(4);
        expected.planHash.fill(5);
        expected.previousHash = digest(previous);
        expected.nextHash = digest(next);
        expected.previousLength = previous.size();
        expected.nextLength = next.size();
        expected.file = location;
        state.files[target] = previous;
        if (fault == 7) {
          state.files.erase(target);
          expected.previousLength = 0;
          expected.previousHash = {};
        }
        std::array<uint8_t, 128> scratch{};
        if (fault == 14) {
          expected.nextLength = expected.previousLength;
          expected.nextHash = expected.previousHash;
        }
        HalRemovalMetadataPublisher publisher(scratch, Authorization::check, &proof);
        assert(publisher.bind(expected));
        const std::string candidate = publisher.candidatePath(), backup = publisher.backupPath();
        if (fault != 14) state.files[candidate] = next;
        if (!fault && !interrupt) {
          std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalBytes{};
          std::array<uint8_t, REMOVAL_METADATA_SNAPSHOT_SIZE> declarationBytes{};
          HalContentRemovalJournalStorage journalStorage;
          ContentRemovalJournal journal(journalStorage, journalBytes);
          const ContentRemovalRecord initial{expected.request, expected.planHash};
          assert(journal.begin(initial) == ContentRemovalJournalResult::Ok);
          assert(journal.advance(ContentRemovalPhase::Quarantined) == ContentRemovalJournalResult::Ok);
          HalRemovalMetadataSnapshotStorage snapshots(declarationBytes);
          assert(snapshots.persist(expected, journal) == RemovalMetadataStorageResult::Ok);
          HalRemovalMetadataAuthorization authorization(journal, snapshots);
          assert(authorization.bind(expected));
          HalRemovalMetadataPublisher owned(scratch, HalRemovalMetadataAuthorization::check, &authorization);
          assert(owned.bind(expected));
          assert(owned.publish());
          assert(owned.verifyPublished());
          const auto beforeRetirement = state.files;
          assert(!owned.retire(*journal.current(), journal));
          assert(state.files == beforeRetirement);
          assert(journal.advance(ContentRemovalPhase::Committed) == ContentRemovalJournalResult::Ok);
          assert(!owned.verifyPublished());
          assert(authorization.bind(expected));
          assert(owned.verifyPublished());
        }

        if (fault == 1) state.failRename = 1;
        if (fault == 2) state.failRenameAfter = 1;
        if (fault == 3) state.failRename = 2;
        if (fault == 4) state.failRenameAfter = 2;
        if (fault == 5) state.files[candidate][0] ^= 1;
        if (fault == 6) state.files[backup] = {8};
        if (fault == 8) state.files[target][0] ^= 1;
        if (fault == 9) state.failSync = true;
        if (fault == 10) state.failClose = true;
        if (fault == 11) state.statErrorPath = target;
        if (fault == 12) state.directories[backup] = {};
        if (fault == 13) state.readErrorPath = candidate;
        proof.calls = 0;
        proof.rejectAt = interrupt;
        const auto before = state.files;
        const bool published = publisher.publish();
        if (fault && fault != 7 && fault != 14) assert(!published);
        if (interrupt && proof.calls >= interrupt) assert(!published);
        if (fault == 5 || fault == 6 || fault == 8 || fault == 12) {
          assert(state.files == before);
          continue;
        }
        state.failRename = state.failRenameAfter = 0;
        state.failSync = state.failClose = false;
        state.statErrorPath.clear();
        state.readErrorPath.clear();
        proof.rejectAt = 0;
        proof.permitted = true;
        HalRemovalMetadataPublisher recovered(scratch, Authorization::check, &proof);
        assert(recovered.bind(expected));
        assert(recovered.publish());
        assert(recovered.verifyPublished());
        assert(state.files.at(target) == (fault == 14 ? previous : next));
        assert(!state.files.contains(candidate));
        if (fault != 7 && fault != 14) assert(state.files.at(backup) == previous);
        if (fault == 14) {
          assert(!state.files.contains(backup));
          assert(state.renames == 0);
        }
        const auto renames = state.renames;
        assert(recovered.publish());
        assert(state.renames == renames);
      }
    }
  }
}
