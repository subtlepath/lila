#include <CrossPointSettings.h>
#include <openssl/evp.h>

#include <cassert>
#include <cstring>

#include "lib/hal/HalContentRemovalJournalStorage.h"
#include "lib/hal/HalContentRemovalStartupRecovery.h"
#include "lib/hal/HalFontRemovalReferences.h"
using namespace companion;
int main() {
  for (const char* path : {"/fonts/Family/Regular.ttf", "/.fonts/Family/Family_14.cpfont", "/fonts/Family.OTF",
                           "/.FONTS/FAMILY.TTC", "/fonts/Famil\xC3\xA9/Regular.ttf"}) {
    inventory_hal_test::state = {};
    HalContentRemovalJournalStorage storage;
    std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> bytes{};
    ContentRemovalJournal journal(storage, bytes);
    ContentRemovalRecord record;
    record.request.transaction.fill(1);
    record.request.owner.fill(2);
    record.request.generation.fill(3);
    record.request.manifest.kind = ContentKind::Font;
    record.request.manifest.formatVersion = std::strstr(path, "cpfont") ? 4 : 1;
    record.request.manifest.contentHash.fill(4);
    record.request.manifest.length = 123;
    record.planHash.fill(5);
    assert(journal.begin(record) == ContentRemovalJournalResult::Ok);
    CrossPointSettings settings;
    if (std::strstr(path, "Famil\xC3\xA9")) {
      std::strcpy(settings.sdFontFamilyName, "FAMIL\xC3\x89");
      std::strcpy(settings.persisted, settings.sdFontFamilyName);
    }
    HalFontRemovalReferences references(journal, settings);
    assert(!references.publish(record, path));
    assert(settings.saves == 0);
    assert(journal.advance(ContentRemovalPhase::Quarantined) == ContentRemovalJournalResult::Ok);
    record = *journal.current();
    for (const char* invalid : {"/books/Family.ttf", "/fonts/Family/../Regular.ttf", "/fonts/Family/Nested/Regular.ttf",
                                "/fonts/_Family/Regular.ttf"}) {
      assert(!references.publish(record, invalid));
      assert(settings.saves == 0);
    }
    if (record.request.manifest.formatVersion == 4) {
      assert(!references.publish(record, "/fonts/Family.cpfont"));
      assert(settings.saves == 0);
    }
    settings.saveSucceeds = false;
    assert(!references.publish(record, path));
    assert(settings.sdFontFamilyName[0] == 0);
    assert(settings.persisted[0] != 0);
    assert(settings.saves == 1);
    assert(!references.publish(record, path));
    assert(settings.saves == 2);
    settings.saveSucceeds = true;
    assert(references.publish(record, path));
    assert(settings.persisted[0] == 0);
    assert(references.verify(record, path));
    std::strcpy(settings.sdFontFamilyName, "Other");
    HalFontRemovalReferences otherReferences(journal, settings);
    assert(otherReferences.publish(record, path));
    const auto savedOther = settings.saves;
    assert(otherReferences.publish(record, path));
    assert(settings.saves == savedOther);
    assert(std::strcmp(settings.persisted, "Other") == 0);
    assert(references.verify(record, path));
    assert(journal.advance(ContentRemovalPhase::Committed) == ContentRemovalJournalResult::Ok);
    record = *journal.current();
    assert(references.retire(record, path));
    assert(references.verifyRetired(record, path));
    const unsigned saves = settings.saves;
    auto foreign = record;
    foreign.request.owner.fill(9);
    assert(!references.verifyRetired(foreign, path));
    assert(settings.saves == saves);
    std::strcpy(settings.sdFontFamilyName, "Family");
    if (!std::strstr(path, "Famil\xC3\xA9")) assert(!references.verify(record, path));
    std::strcpy(settings.sdFontFamilyName, "Other");
    assert(references.verify(record, path));
    std::memset(settings.sdFontFamilyName, 'a', sizeof(settings.sdFontFamilyName));
    assert(!references.verify(record, path));
    // Exercise actual startup dispatch after settings publication fails.
    inventory_hal_test::state = {};
    auto& state = inventory_hal_test::state;
    state.enumerateFileMap = true;
    state.falseExists = true;
    state.directories["/"] = {};
    const std::string original(path);
    state.directories[original.substr(0, original.find_last_of('/'))] = {};
    const std::vector<uint8_t> content{1, 2, 3, 4};
    state.files[original] = content;
    state.files["/.crosspoint/unrelated"] = {7, 8};
    ContentRemovalRecord initial = record;
    initial.phase = ContentRemovalPhase::Prepared;
    initial.revision = 1;
    initial.request.manifest.length = content.size();
    EVP_Digest(content.data(), content.size(), initial.request.manifest.contentHash.data(), nullptr, EVP_sha256(),
               nullptr);
    std::array<uint8_t, SINGLE_FILE_REMOVAL_PLAN_MAX_SIZE> encoded{};
    const size_t length = encodeSingleFileRemovalPlan({initial.request, original}, encoded);
    assert(length);
    std::array<uint8_t, 128> io{};
    HalSingleFileRemovalPlanStorage plans(io);
    assert(plans.publish(std::span(encoded).first(length), initial.planHash) == RemovalPlanStorageResult::Ok);
    ContentRemovalJournal sourceJournal(storage, bytes);
    SETTINGS = CrossPointSettings{};
    if (std::strstr(path, "Famil\xC3\xA9")) {
      std::strcpy(SETTINGS.sdFontFamilyName, "FAMIL\xC3\x89");
      std::strcpy(SETTINGS.persisted, SETTINGS.sdFontFamilyName);
    }
    SETTINGS.saveSucceeds = false;
    HalFontRemovalReferences sourceReferences(sourceJournal, SETTINGS);
    HalSingleFileRemovalParticipant participant(sourceJournal, sourceReferences, io);
    assert(participant.bind(std::span(encoded).first(length), initial.planHash));
    ContentRemoval removal(sourceJournal, participant);
    assert(removal.remove(initial) == ContentRemovalJournalResult::IoError);
    assert(!state.files.contains(original));
    SETTINGS.loadSucceeds = false;
    auto startup = std::make_unique<HalContentRemovalStartupRecovery>(&SETTINGS);
    const auto before = state.files;
    assert(!startup->run(initial.request.generation));
    assert(state.files == before);
    SETTINGS.loadSucceeds = true;
    assert(!startup->run(initial.request.generation));
    SETTINGS.saveSucceeds = true;
    startup = std::make_unique<HalContentRemovalStartupRecovery>(&SETTINGS);
    assert(startup->run(initial.request.generation));
    bool pending = true;
    assert(startup->pending(pending) && !pending);
    assert(SETTINGS.persisted[0] == 0);
    assert(state.files.at("/.crosspoint/unrelated") == std::vector<uint8_t>({7, 8}));
    assert(startup->run(initial.request.generation));
  }
}
