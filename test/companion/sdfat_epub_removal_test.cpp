#include <openssl/evp.h>

#include <cassert>

#include "lib/hal/HalContentRemovalJournalStorage.h"
#include "lib/hal/HalEpubRemovalParticipant.h"
using namespace companion;
struct References final : EpubRemovalReferences {
  bool published = false, retired = false;
  bool failAfterPublish = false;
  ContentRemovalJournal* invalidateJournal = nullptr;
  const ContentRemovalRecord* foreignRecord = nullptr;
  bool publish(const ContentRemovalRecord&, const char*) override {
    published = true;
    if (invalidateJournal) {
      assert(invalidateJournal->recover(*foreignRecord) != ContentRemovalJournalResult::Ok);
    }
    return !failAfterPublish;
  }
  bool verify(const ContentRemovalRecord&, const char*) override { return published; }
  bool retire(const ContentRemovalRecord&, const char*) override {
    retired = true;
    return true;
  }
  bool verifyRetired(const ContentRemovalRecord&, const char*) override { return published && retired; }
};
int main() {
  for (unsigned variant = 0; variant < 5; ++variant) {
    for (unsigned fault = 0; fault < 9; ++fault) {
      auto& state = inventory_hal_test::state;
      state = {};
      state.enumerateFileMap = true;
      state.falseExists = true;
      state.directories["/"] = {};
      state.directories["/Books"] = {};
      static constexpr const char* PATHS[] = {"/Books/caf\xC3\xA9.epub", "/fonts/Family/Regular.ttf",
                                              "/.fonts/Family/Regular.otf", "/fonts/Family/Regular.ttc",
                                              "/.fonts/Family/Regular.cpfont"};
      const char* original = PATHS[variant];
      if (variant) {
        state.directories["/fonts"] = {};
        state.directories["/fonts/Family"] = {};
        state.directories["/.fonts"] = {};
        state.directories["/.fonts/Family"] = {};
      }
      const std::vector<uint8_t> content{1, 2, 3, 4, 5};
      state.files[original] = content;
      state.files["/.crosspoint/progress"] = {9, 8, 7};
      state.files["/fonts/Family/Unrelated.ttf"] = {6, 5, 4};
      ContentRemovalRecord initial;
      initial.request.transaction.fill(1);
      initial.request.owner.fill(2);
      initial.request.generation.fill(3);
      initial.request.manifest.kind = variant ? ContentKind::Font : ContentKind::Epub;
      initial.request.manifest.formatVersion = variant == 4 ? 4 : 1;
      initial.request.manifest.length = content.size();
      EVP_Digest(content.data(), content.size(), initial.request.manifest.contentHash.data(), nullptr, EVP_sha256(),
                 nullptr);
      std::array<uint8_t, SINGLE_FILE_REMOVAL_PLAN_MAX_SIZE> encoded{};
      const size_t size = encodeSingleFileRemovalPlan({initial.request, original}, encoded);
      assert(size);
      EVP_Digest(encoded.data(), size, initial.planHash.data(), nullptr, EVP_sha256(), nullptr);
      std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalScratch{};
      std::array<uint8_t, 128> hashScratch{};
      HalContentRemovalJournalStorage storage;
      ContentRemovalJournal journal(storage, journalScratch);
      References references;
      HalEpubRemovalParticipant participant(journal, references, hashScratch);
      assert(participant.bind(std::span(encoded).first(size), initial.planHash));
      ContentRemoval removal(journal, participant);
      if (fault == 1) state.failRename = 1;
      if (fault == 2) state.failRenameAfter = 1;
      if (fault == 3) state.failRemoveAfter = true;
      if (fault == 4) state.files[original][0] ^= 1;
      if (fault >= 5) references.failAfterPublish = true;
      auto foreignCallback = initial;
      foreignCallback.request.owner.fill(4);
      if (fault == 8) {
        references.failAfterPublish = false;
        references.invalidateJournal = &journal;
        references.foreignRecord = &foreignCallback;
      }
      const auto result = removal.remove(initial);
      if (!fault)
        assert(result == ContentRemovalJournalResult::Ok);
      else
        assert(result != ContentRemovalJournalResult::Ok);
      if (fault == 4) {
        assert(state.files.contains(original));
        assert(!state.files.contains(CONTENT_REMOVAL_JOURNALS[0]));
        continue;
      }
      state.failRename = state.failRenameAfter = 0;
      state.failRemoveAfter = false;
      references.failAfterPublish = false;
      references.invalidateJournal = nullptr;
      if (fault == 8) {
        assert(!state.files.contains(original));
        assert(std::any_of(state.files.begin(), state.files.end(), [](const auto& file) {
          return file.first.starts_with("/.crosspoint/companion/removal-bytes-");
        }));
        assert(!journal.current());
      }
      if (fault == 6) {
        auto backup = std::find_if(state.files.begin(), state.files.end(), [](const auto& file) {
          return file.first.starts_with("/.crosspoint/companion/removal-bytes-");
        });
        assert(backup != state.files.end());
        backup->second[0] ^= 1;
        const auto before = state.files;
        assert(removal.remove(initial) != ContentRemovalJournalResult::Ok);
        for (const auto& [path, bytes] : before) {
          if (path == CONTENT_REMOVAL_JOURNALS[0] || path == CONTENT_REMOVAL_JOURNALS[1]) continue;
          assert(state.files.at(path) == bytes);
        }
        assert(journal.current()->phase == ContentRemovalPhase::Quarantined);
        backup->second[0] ^= 1;
      }
      if (fault == 7) {
        auto foreign = initial;
        foreign.request.owner.fill(4);
        const auto before = state.files;
        assert(removal.remove(foreign) != ContentRemovalJournalResult::Ok);
        assert(state.files == before);
      }
      // Reconstruct the coordinator and participant from durable journal bytes.
      ContentRemovalJournal recovered(storage, journalScratch);
      HalEpubRemovalParticipant resumed(recovered, references, hashScratch);
      assert(resumed.bind(std::span(encoded).first(size), initial.planHash));
      ContentRemoval retry(recovered, resumed);
      assert(retry.remove(initial) == ContentRemovalJournalResult::Ok);
      assert(!state.files.contains(original));
      assert(state.files.at("/.crosspoint/progress") == std::vector<uint8_t>({9, 8, 7}));
      assert(state.files.at("/fonts/Family/Unrelated.ttf") == std::vector<uint8_t>({6, 5, 4}));
      assert(recovered.current()->phase == ContentRemovalPhase::Retired);
      const auto renames = state.renames;
      assert(retry.remove(initial) == ContentRemovalJournalResult::Ok);
      assert(state.renames == renames);
    }
  }
}
