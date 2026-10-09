#include <openssl/evp.h>

#include <cassert>

#include "lib/hal/HalContentRemovalJournalStorage.h"
#include "lib/hal/HalDictionaryRemovalStorage.h"
using namespace companion;
namespace {
struct References final : DictionaryRemovalReferences {
  bool published = false, retired = false;
  bool verifyPlan(const ContentRemovalRecord&, const DictionaryRemovalPlan&) override { return true; }
  bool publish(const ContentRemovalRecord&, const DictionaryRemovalPlan&) override {
    published = true;
    return true;
  }
  bool verify(const ContentRemovalRecord&, const DictionaryRemovalPlan&) override { return published; }
  bool retire(const ContentRemovalRecord&, const DictionaryRemovalPlan&) override {
    retired = true;
    return true;
  }
  bool verifyRetired(const ContentRemovalRecord&, const DictionaryRemovalPlan&) override {
    return published && retired;
  }
};
void digest(std::span<const uint8_t> bytes, Digest& output) {
  assert(EVP_Digest(bytes.data(), bytes.size(), output.data(), nullptr, EVP_sha256(), nullptr));
}
}  // namespace
int main() {
  for (bool hidden : {false, true})
    for (bool compressed : {false, true})
      for (bool synonyms : {false, true}) {
        for (unsigned fault = 0; fault < 9; ++fault) {
          auto& state = inventory_hal_test::state;
          state = {};
          state.enumerateFileMap = true;
          state.falseExists = true;
          state.directories["/"] = {};
          const std::string root = hidden ? "/.dictionaries" : "/dictionaries";
          state.directories[root] = {};
          state.directories[root + "/Family"] = {};
          DictionaryRemovalPlan plan;
          auto& request = plan.request;
          request.transaction.fill(1);
          request.owner.fill(2);
          request.generation.fill(3);
          request.manifest.kind = ContentKind::Dictionary;
          request.manifest.formatVersion = 1;
          request.manifest.length = 1000;
          request.manifest.contentHash.fill(4);
          auto& installed = plan.installed;
          installed.revision = 1;
          installed.phase = DictionaryInstallationPhase::Committed;
          installed.extraction.revision = 1;
          installed.extraction.transaction = request.transaction;
          installed.extraction.generation = request.generation;
          installed.extraction.archiveHash = request.manifest.contentHash;
          installed.extraction.compressed = compressed;
          installed.extraction.synonyms = synonyms;
          installed.published = installed.extraction.sealed = synonyms ? 15 : 7;
          installed.archives.original = installed.archives.members = request.manifest;
          installed.archives.members.contentHash.fill(9);
          std::strcpy(installed.base.data(), (root + "/Family/dictionary").c_str());
          std::array<std::string, 4> paths;
          std::array<char, 144> memberPath{};
          for (unsigned member = 0; member < (synonyms ? 4U : 3U); ++member) {
            std::vector<uint8_t> data(100 + member, static_cast<uint8_t>(member + 5));
            installed.extraction.lengths[member] = data.size();
            digest(data, installed.extraction.hashes[member]);
          }
          for (unsigned member = 0; member < (synonyms ? 4U : 3U); ++member) {
            assert(dictionaryRemovalMemberPath(plan, member, memberPath));
            paths[member] = memberPath.data();
            state.files[paths[member]] = std::vector<uint8_t>(100 + member, static_cast<uint8_t>(member + 5));
          }
          const auto unrelated = root + "/Family/notes.txt";
          state.files[unrelated] = {90, 91};
          const auto unownedSyn = root + "/Family/dictionary.syn";
          if (!synonyms) state.files[unownedSyn] = {92};
          std::array<uint8_t, DICTIONARY_REMOVAL_PLAN_SIZE> bytes{};
          assert(DictionaryRemovalPlanCodec::encode(plan, bytes) == bytes.size());
          ContentRemovalRecord initial;
          initial.request = request;
          digest(bytes, initial.planHash);
          std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalScratch{};
          std::array<uint8_t, 64> scratch{};
          HalContentRemovalJournalStorage journalStorage;
          ContentRemovalJournal journal(journalStorage, journalScratch);
          HalDictionaryRemovalStorage members(journal, bytes, scratch);
          References refs;
          DictionaryRemovalParticipant participant(journal, members, refs);
          if (fault == 8) {
            bytes[20] ^= 1;
            assert(!participant.bind(plan, initial.planHash));
            bytes[20] ^= 1;
          }
          assert(participant.bind(plan, initial.planHash));
          assert(members.stat(4, false) == FileStatus::Error);
          assert(!members.quarantine(0));  // No durable parent permits mutation.
          if (fault == 1) state.failRename = 2;
          if (fault == 2) state.failRenameAfter = 2;
          if (fault == 3) state.failRemoveAfter = true;
          if (fault == 4) state.files[paths[2]][0] ^= 1;
          if (fault == 5) state.readErrorPath = paths[2];
          if (fault == 6) state.failSyncPath = paths[2];
          if (fault == 7) state.failClosePath = paths[2];
          ContentRemoval removal(journal, participant);
          const auto result = removal.remove(initial);
          assert((result == ContentRemovalJournalResult::Ok) == (fault == 0 || fault == 8));
          if (fault >= 4 && fault <= 7) {
            assert(!journal.current());
            assert(state.renames == 0);
            for (unsigned member = 0; member < (synonyms ? 4U : 3U); ++member)
              assert(state.files.contains(paths[member]));
          }
          if (fault == 4) state.files[paths[2]][0] ^= 1;
          state.failRename = state.failRenameAfter = 0;
          state.failRemoveAfter = false;
          state.readErrorPath.clear();
          state.failSyncPath.clear();
          state.failClosePath.clear();
          ContentRemovalJournal recovered(journalStorage, journalScratch);
          HalDictionaryRemovalStorage restoredMembers(recovered, bytes, scratch);
          DictionaryRemovalParticipant restored(recovered, restoredMembers, refs);
          assert(restored.bind(plan, initial.planHash));
          ContentRemoval retry(recovered, restored);
          assert(retry.remove(initial) == ContentRemovalJournalResult::Ok);
          for (unsigned member = 0; member < (synonyms ? 4U : 3U); ++member)
            assert(!state.files.contains(paths[member]));
          assert(state.files.at(unrelated) == std::vector<uint8_t>({90, 91}));
          if (!synonyms) assert(state.files.at(unownedSyn) == std::vector<uint8_t>({92}));
          const auto retained = state.files;
          assert(retry.remove(initial) == ContentRemovalJournalResult::Ok);
          assert(state.files == retained);
        }
      }
}
