#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "lib/Companion/CompanionDictionaryZipAudit.h"
using namespace companion;
namespace {
class Storage final : public DictionaryExtractionJournalStorage {
 public:
  std::map<std::string, std::vector<uint8_t>> files;
  unsigned writes = 0, failedWrite = 0;
  bool afterEffect = false, failRead = false;
  TransferState* changeOwner = nullptr;
  bool prepare() override { return true; }
  FileStatus stat(const char* path, uint64_t& size) override {
    if (!files.contains(path)) return FileStatus::Missing;
    size = files.at(path).size();
    return FileStatus::Present;
  }
  bool read(const char* path, uint64_t at, std::span<uint8_t> bytes) override {
    if (failRead || at || !files.contains(path) || files.at(path).size() != bytes.size()) return false;
    std::copy(files.at(path).begin(), files.at(path).end(), bytes.begin());
    return true;
  }
  bool write(const char* path, uint64_t at, std::span<const uint8_t> bytes, bool truncate) override {
    if (at || !truncate) return false;
    const bool failure = ++writes == failedWrite;
    if (!failure || afterEffect) files[path] = {bytes.begin(), bytes.end()};
    if (changeOwner) changeOwner->owner[0] ^= 1;
    return !failure;
  }
};
struct Fixture {
  Storage storage;
  ContentManifest manifest;
  TransferState state;
  Identity generation;
  std::array<uint8_t, 384> scratch{};
  Fixture() {
    scratch.fill(0xa5);
    manifest.kind = ContentKind::Dictionary;
    manifest.formatVersion = 1;
    manifest.length = 500;
    manifest.contentHash.fill(9);
    state.length = state.durableOffset = manifest.length;
    state.contentHash = manifest.contentHash;
    state.owner.fill(1);
    state.transaction.fill(2);
    generation.fill(3);
    state.storageGeneration = generation;
  }
  DictionaryZipAuditJournal journal(std::string_view base = "/dictionaries/english/dictionary") {
    return {storage, state, manifest, generation, base, scratch};
  }
};
}  // namespace
TEST(DictionaryZipAudit, PublishesBothCopiesAndRecoversVerifiedOrAbortedParent) {
  Fixture f;
  auto journal = f.journal();
  EXPECT_EQ(journal.recover(), DictionaryJournalResult::Missing);
  ASSERT_EQ(journal.publish(), DictionaryJournalResult::Ok);
  ASSERT_NE(journal.current(), nullptr);
  EXPECT_EQ(f.storage.writes, 2u);
  for (auto phase : {TransferPhase::Receiving, TransferPhase::Verified, TransferPhase::Aborted}) {
    f.state.phase = phase;
    auto fresh = f.journal();
    EXPECT_EQ(fresh.recover(), DictionaryJournalResult::Ok);
    EXPECT_NE(fresh.current(), nullptr);
    EXPECT_EQ(fresh.publish(),
              phase == TransferPhase::Aborted ? DictionaryJournalResult::Invalid : DictionaryJournalResult::Ok);
  }
  EXPECT_EQ(f.storage.writes, 2u);
  EXPECT_TRUE(std::all_of(f.scratch.begin() + DICTIONARY_ZIP_AUDIT_SIZE, f.scratch.end(),
                          [](uint8_t byte) { return byte == 0xa5; }));
}
TEST(DictionaryZipAudit, InterruptedPublicationRecoversAndRepairsOnlyWithSurvivingProof) {
  for (bool after : {false, true}) {
    Fixture f;
    f.storage.failedWrite = 2;
    f.storage.afterEffect = after;
    auto journal = f.journal();
    EXPECT_EQ(journal.publish(), DictionaryJournalResult::IoError);
    EXPECT_EQ(journal.current(), nullptr);
    auto fresh = f.journal();
    ASSERT_EQ(fresh.recover(), DictionaryJournalResult::Ok);
    EXPECT_EQ(fresh.publish(), DictionaryJournalResult::Ok);
    EXPECT_EQ(f.storage.files.size(), 2u);
  }
  Fixture f;
  f.storage.failedWrite = 1;
  f.storage.afterEffect = true;
  EXPECT_EQ(f.journal().publish(), DictionaryJournalResult::IoError);
  EXPECT_EQ(f.journal().publish(), DictionaryJournalResult::Ok);
}
TEST(DictionaryZipAudit, ForeignOwnerTransactionCardArchiveManifestAndDestinationPreserveEvidence) {
  for (unsigned mutation = 0; mutation < 7; ++mutation) {
    Fixture f;
    ASSERT_EQ(f.journal().publish(), DictionaryJournalResult::Ok);
    const auto files = f.storage.files;
    switch (mutation) {
      case 0:
        f.state.owner[0] ^= 1;
        break;
      case 1:
        f.state.transaction[0] ^= 1;
        break;
      case 2:
        f.generation[0] ^= 1;
        f.state.storageGeneration = f.generation;
        break;
      case 3:
        f.manifest.contentHash[0] ^= 1;
        f.state.contentHash = f.manifest.contentHash;
        break;
      case 4:
        f.manifest.length++;
        f.state.length++;
        f.state.durableOffset++;
        break;
      case 5:
        f.manifest.formatVersion++;
        break;
      default:
        break;
    }
    auto fresh = f.journal(mutation == 6 ? "/dictionaries/other/dictionary" : "/dictionaries/english/dictionary");
    EXPECT_EQ(fresh.publish(), mutation == 5 ? DictionaryJournalResult::Invalid : DictionaryJournalResult::Conflict)
        << mutation;
    EXPECT_EQ(fresh.current(), nullptr);
    EXPECT_EQ(f.storage.files, files);
  }
}
TEST(DictionaryZipAudit, CorruptCopyRepairsWhileWhollyCorruptOrForeignValidCopyBlocks) {
  Fixture f;
  ASSERT_EQ(f.journal().publish(), DictionaryJournalResult::Ok);
  f.storage.files.at(DICTIONARY_ZIP_AUDIT_JOURNALS[1])[10] ^= 1;
  ASSERT_EQ(f.journal().publish(), DictionaryJournalResult::Ok);
  for (const auto path : DICTIONARY_ZIP_AUDIT_JOURNALS) f.storage.files.at(path)[10] ^= 1;
  const auto corrupt = f.storage.files;
  EXPECT_EQ(f.journal().publish(), DictionaryJournalResult::Corrupt);
  EXPECT_EQ(f.storage.files, corrupt);
  Fixture other;
  other.state.owner[0] ^= 1;
  ASSERT_EQ(other.journal().publish(), DictionaryJournalResult::Ok);
  Fixture mixed;
  ASSERT_EQ(mixed.journal().publish(), DictionaryJournalResult::Ok);
  mixed.storage.files[DICTIONARY_ZIP_AUDIT_JOURNALS[1]] = other.storage.files.at(DICTIONARY_ZIP_AUDIT_JOURNALS[1]);
  const auto foreign = mixed.storage.files;
  EXPECT_EQ(mixed.journal().publish(), DictionaryJournalResult::Conflict);
  EXPECT_EQ(mixed.storage.files, foreign);
}
TEST(DictionaryZipAudit, RejectsIncompleteOrLaterPhasesAndInvalidIdentityWithoutWriting) {
  for (unsigned mutation = 0; mutation < 5; ++mutation) {
    Fixture f;
    switch (mutation) {
      case 0:
        f.state.durableOffset--;
        break;
      case 1:
        f.state.phase = TransferPhase::Installing;
        break;
      case 2:
        f.state.phase = TransferPhase::Committed;
        break;
      case 3:
        f.state.owner.fill(0);
        break;
      default:
        f.generation[0] ^= 1;
        break;
    }
    EXPECT_EQ(f.journal().publish(), DictionaryJournalResult::Invalid);
    EXPECT_TRUE(f.storage.files.empty());
  }
  Fixture f;
  EXPECT_EQ(f.journal("/dictionaries/../other").publish(), DictionaryJournalResult::Invalid);
}
TEST(DictionaryZipAudit, ParentMutationOrReadErrorNeverExposesReadyProof) {
  Fixture f;
  f.storage.changeOwner = &f.state;
  auto journal = f.journal();
  EXPECT_EQ(journal.publish(), DictionaryJournalResult::IoError);
  EXPECT_EQ(f.storage.writes, 1u);
  EXPECT_EQ(journal.current(), nullptr);
  Fixture healthy;
  ASSERT_EQ(healthy.journal().publish(), DictionaryJournalResult::Ok);
  healthy.storage.failRead = true;
  auto fresh = healthy.journal();
  EXPECT_EQ(fresh.publish(), DictionaryJournalResult::IoError);
  EXPECT_EQ(fresh.current(), nullptr);
}
TEST(DictionaryZipAudit, CodecRejectsEverySingleByteMutationAndTruncation) {
  Fixture f;
  auto journal = f.journal();
  ASSERT_EQ(journal.publish(), DictionaryJournalResult::Ok);
  const auto bytes = f.storage.files.at(DICTIONARY_ZIP_AUDIT_JOURNALS[0]);
  DictionaryZipAuditCodec codec;
  ASSERT_NE(codec.inspect(bytes), nullptr);
  for (size_t at = 0; at < bytes.size(); ++at) {
    auto bad = bytes;
    bad[at] ^= 1;
    EXPECT_EQ(codec.inspect(bad), nullptr) << at;
  }
  for (size_t length = 0; length < bytes.size(); ++length)
    EXPECT_EQ(codec.inspect(std::span(bytes).first(length)), nullptr);
}

TEST(DictionaryZipAudit, ChecksVersionReservedBytesAndDestinationPaddingEvenWithValidCrc) {
  Fixture f;
  ASSERT_EQ(f.journal().publish(), DictionaryJournalResult::Ok);
  const auto bytes = f.storage.files.at(DICTIONARY_ZIP_AUDIT_JOURNALS[0]);
  DictionaryZipAuditCodec codec;
  for (const size_t at : {size_t(4), size_t(5), size_t(6), size_t(7), bytes.size() - 5}) {
    auto changed = bytes;
    changed[at] ^= 1;
    inventory_detail::write(changed, changed.size() - 4,
                            inventoryIndexCrc(std::span(changed).first(changed.size() - 4)), 4);
    EXPECT_EQ(codec.inspect(changed), nullptr) << at;
  }
  auto oversized = bytes;
  oversized.reserve(bytes.size() + 1);
  oversized.push_back(0);
  EXPECT_EQ(codec.inspect(oversized), nullptr);
  auto tooSmall = std::span(f.scratch).first(DICTIONARY_ZIP_AUDIT_SIZE - 1);
  DictionaryZipAuditJournal journal(f.storage, f.state, f.manifest, f.generation, "/dictionaries/english/dictionary",
                                    tooSmall);
  const auto saved = f.storage.files;
  EXPECT_EQ(journal.publish(), DictionaryJournalResult::Invalid);
  EXPECT_EQ(f.storage.files, saved);
}

TEST(DictionaryZipAudit, NeverRetargetsCapturedParentBeforeInitialPublication) {
  Fixture f;
  auto journal = f.journal();
  ASSERT_EQ(journal.recover(), DictionaryJournalResult::Missing);
  f.state.owner[0] ^= 1;
  EXPECT_EQ(journal.publish(), DictionaryJournalResult::Invalid);
  EXPECT_EQ(journal.current(), nullptr);
  EXPECT_TRUE(f.storage.files.empty());
  EXPECT_EQ(f.storage.writes, 0u);
}
