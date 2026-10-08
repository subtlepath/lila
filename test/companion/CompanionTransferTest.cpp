#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "lib/Companion/CompanionCourseBinding.h"
#include "lib/Companion/CompanionDictionaryJournalPaths.h"
#include "lib/Companion/CompanionFrame.h"
#include "lib/Companion/CompanionTransfer.h"
#include "lib/Companion/CompanionTransferHandler.h"
#include "lib/Companion/CompanionWorkspace.h"

using namespace companion;

namespace {
class FakeStorage final : public TransferStorage {
 public:
  std::map<std::string, std::vector<uint8_t>> files;
  std::vector<uint8_t> expected{1, 2, 3, 4, 5, 6};
  Digest expectedHash{};
  int failMutation = 0;
  int mutations = 0;
  bool failAfterEffect = false;
  bool failRead = false;
  const char* failStatPath = nullptr;
  bool rejectValidation = false;
  bool rejectMetadata = false;
  bool useCourseMetadata = false;
  int validationCalls = 0;
  int metadataCalls = 0;
  TransferState validationState{}, metadataState{};
  bool sawValidationContext = false, sawMetadataContext = false;
  bool dictionaryMembersInstalled = false, failDictionaryPublication = false;
  bool enableDictionaryPublication = false;
  unsigned dictionaryPublicationCalls = 0;
  bool dictionaryArchiveCached = false;

  FakeStorage() { expectedHash[0] = 7; }
  bool prepare() override { return true; }
  FileStatus stat(const char* path, uint64_t& size) override {
    if (failStatPath && std::strcmp(path, failStatPath) == 0) return FileStatus::Error;
    auto found = files.find(path);
    if (found == files.end()) return FileStatus::Missing;
    size = found->second.size();
    return FileStatus::Present;
  }
  bool read(const char* path, uint64_t offset, std::span<uint8_t> bytes) override {
    if (failRead) return false;
    auto found = files.find(path);
    if (found == files.end() || offset > found->second.size() || bytes.size() > found->second.size() - offset)
      return false;
    std::memcpy(bytes.data(), found->second.data() + offset, bytes.size());
    return true;
  }
  bool write(const char* path, uint64_t offset, std::span<const uint8_t> bytes, bool truncate) override {
    const bool fail = ++mutations == failMutation;
    if (fail && !failAfterEffect) return false;
    auto& data = files[path];
    if (truncate) data.clear();
    data.resize(offset + bytes.size());
    if (!bytes.empty()) std::memcpy(data.data() + offset, bytes.data(), bytes.size());
    return !fail;
  }
  bool resize(const char* path, uint64_t size) override {
    const bool fail = ++mutations == failMutation;
    if (fail && !failAfterEffect) return false;
    if (!files.contains(path)) return false;
    files[path].resize(size);
    return !fail;
  }
  bool rename(const char* from, const char* to) override {
    const bool fail = ++mutations == failMutation;
    if (fail && !failAfterEffect) return false;
    if (!files.contains(from) || files.contains(to)) return false;
    files[to] = std::move(files[from]);
    files.erase(from);
    return !fail;
  }
  bool remove(const char* path) override {
    const bool fail = ++mutations == failMutation;
    if (fail && !failAfterEffect) return false;
    const bool removed = files.erase(path) != 0;
    return !fail && removed;
  }
  bool validateContent(const char*, const char*, const ContentManifest&, std::span<uint8_t>) override {
    ++validationCalls;
    return !rejectValidation;
  }
  bool installContentMetadata(const char*, const ContentManifest& manifest, std::span<uint8_t> scratch) override {
    ++metadataCalls;
    if (rejectMetadata) return false;
    return !useCourseMetadata || installCourseBinding(*this, manifest, scratch) == CourseBindingResult::Ok;
  }
  bool validateContent(const char* destination, const char* candidate, const ContentManifest& manifest,
                       const TransferState& state, std::span<uint8_t> scratch) override {
    validationState = state;
    sawValidationContext = true;
    return validateContent(destination, candidate, manifest, scratch);
  }
  bool installContentMetadata(const char* destination, const ContentManifest& manifest, const TransferState& state,
                              std::span<uint8_t> scratch) override {
    metadataState = state;
    sawMetadataContext = true;
    return installContentMetadata(destination, manifest, scratch);
  }
  bool verify(const char* path, uint64_t length, const Digest& hash, std::span<uint8_t> scratch) override {
    std::fill(scratch.begin(), scratch.end(), 0xcc);
    return files.contains(path) && files[path] == expected && length == expected.size() && hash == expectedHash;
  }
  bool installDictionaryMembers(const char*, const ContentManifest&, const TransferState& state,
                                std::span<uint8_t>) override {
    ++dictionaryPublicationCalls;
    EXPECT_EQ(state.phase, TransferPhase::Installing);
    if (!enableDictionaryPublication) return false;
    dictionaryMembersInstalled = true;
    return !failDictionaryPublication;
  }
  bool verifyDictionaryArchive(const char* destination, const ContentManifest& manifest, const TransferState& state,
                               std::span<uint8_t> scratch) override {
    return dictionaryArchiveCached ? verify("/archive-cache", manifest.length, manifest.contentHash, scratch)
                                   : TransferStorage::verifyDictionaryArchive(destination, manifest, state, scratch);
  }
  void inject(int mutation, bool after) {
    mutations = 0;
    failMutation = mutation;
    failAfterEffect = after;
  }
};

class CompanionTransferTest : public testing::Test {
 protected:
  FakeStorage storage;
  std::array<uint8_t, SESSION_WORKSPACE_SIZE> workspace{};
  Identity generation{};
  TransferState initial;
  CompanionTransferTest() {
    generation[0] = 1;
    initial.storageGeneration = generation;
    initial.transaction[0] = 2;
    initial.owner[0] = 3;
    initial.contentHash = storage.expectedHash;
    initial.length = storage.expected.size();
  }
  void receive(Transfer& transfer, bool original = true) {
    if (original) storage.files["/book.epub"] = {99};
    ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
    ASSERT_EQ(transfer.begin(initial, "/book.epub"), TransferResult::Ok);
    ASSERT_EQ(transfer.append(initial.transaction, initial.owner, 0, storage.expected), TransferResult::Ok);
  }
};
}  // namespace

TEST_F(CompanionTransferTest, InstallsAndRepeatedCommitIsHarmless) {
  Transfer transfer(storage, workspace);
  receive(transfer);
  ASSERT_EQ(transfer.commit(initial.transaction, initial.owner), TransferResult::Ok);
  ASSERT_EQ(transfer.commit(initial.transaction, initial.owner), TransferResult::Ok);
  EXPECT_EQ(storage.files["/book.epub"], storage.expected);
  EXPECT_FALSE(storage.files.contains(TRANSFER_BACKUP));
  Transfer reboot(storage, workspace);
  EXPECT_EQ(reboot.recover(generation), TransferResult::Ok);
  EXPECT_EQ(reboot.current()->phase, TransferPhase::Committed);
}

TEST_F(CompanionTransferTest, PendingDictionaryRecoveryBlocksNewTransactionWithoutMutation) {
  Transfer transfer(storage, workspace);
  receive(transfer);
  ASSERT_EQ(transfer.commit(initial.transaction, initial.owner), TransferResult::Ok);
  auto next = initial;
  next.transaction[0] = 9;
  for (const auto paths :
       {DICTIONARY_EXTRACTION_JOURNALS, DICTIONARY_INSTALLATION_JOURNALS, DICTIONARY_RETIREMENT_JOURNALS}) {
    for (unsigned slot = 0; slot < 2; ++slot) {
      storage.files[paths[slot]] = {1};
      const auto before = storage.files;
      EXPECT_EQ(transfer.begin(next, "/next.epub"), TransferResult::Busy);
      EXPECT_EQ(storage.files, before);
      EXPECT_EQ(transfer.destination(), "/book.epub");
      EXPECT_EQ(transfer.current()->transaction, initial.transaction);
      EXPECT_EQ(transfer.begin(initial, "/book.epub"), TransferResult::Ok);
      storage.files.erase(paths[slot]);
      storage.failStatPath = paths[slot];
      const auto beforeError = storage.files;
      EXPECT_EQ(transfer.begin(next, "/next.epub"), TransferResult::IoError);
      EXPECT_EQ(storage.files, beforeError);
      EXPECT_EQ(transfer.current()->transaction, initial.transaction);
      storage.failStatPath = nullptr;
    }
  }
  ASSERT_EQ(transfer.begin(next, "/next.epub"), TransferResult::Ok);
  EXPECT_EQ(transfer.current()->transaction, next.transaction);
}

TEST_F(CompanionTransferTest, ResumesFromDurableOffsetAndRejectsUnrelatedOwners) {
  Transfer transfer(storage, workspace);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(transfer.begin(initial, "/book.epub"), TransferResult::Ok);
  ASSERT_EQ(transfer.append(initial.transaction, initial.owner, 0, std::span(storage.expected).first(3)),
            TransferResult::Ok);
  Transfer reboot(storage, workspace);
  ASSERT_EQ(reboot.recover(generation), TransferResult::Ok);
  EXPECT_EQ(reboot.current()->durableOffset, 3U);
  EXPECT_EQ(reboot.append(initial.transaction, initial.owner, 0, storage.expected), TransferResult::Offset);
  Identity intruder{};
  intruder[0] = 9;
  EXPECT_EQ(reboot.commit(initial.transaction, intruder), TransferResult::Unauthorized);
  EXPECT_EQ(reboot.append(initial.transaction, intruder, 3, std::span(storage.expected).subspan(3)),
            TransferResult::Unauthorized);
  ASSERT_EQ(reboot.append(initial.transaction, initial.owner, 3, std::span(storage.expected).subspan(3)),
            TransferResult::Ok);
  EXPECT_EQ(reboot.commit(initial.transaction, initial.owner), TransferResult::Ok);
}

TEST_F(CompanionTransferTest, RecoversEveryInterruptedCommitMutation) {
  for (bool original : {false, true}) {
    for (bool after : {false, true}) {
      // Existing targets add backup rename and removal to three mutations.
      for (int step = 1; step <= (original ? 5 : 3); ++step) {
        SCOPED_TRACE(testing::Message() << "original=" << original << " after=" << after << " step=" << step);
        storage.files.clear();
        storage.inject(0, false);
        Transfer transfer(storage, workspace);
        receive(transfer, original);
        storage.inject(step, after);
        EXPECT_EQ(transfer.commit(initial.transaction, initial.owner), TransferResult::IoError);
        storage.inject(0, false);
        Transfer reboot(storage, workspace);
        ASSERT_EQ(reboot.recover(generation), TransferResult::Ok);
        ASSERT_EQ(reboot.commit(initial.transaction, initial.owner), TransferResult::Ok);
        EXPECT_EQ(storage.files["/book.epub"], storage.expected);
        EXPECT_FALSE(storage.files.contains(TRANSFER_BACKUP));
      }
    }
  }
}

TEST_F(CompanionTransferTest, RecoversInterruptedBeginAndCheckpointWrites) {
  for (bool after : {false, true}) {
    for (int step = 1; step <= 2; ++step) {
      storage.files.clear();
      storage.inject(0, false);
      Transfer transfer(storage, workspace);
      ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
      storage.inject(step, after);
      EXPECT_EQ(transfer.begin(initial, "/book.epub"), TransferResult::IoError);
      storage.inject(0, false);
      Transfer reboot(storage, workspace);
      ASSERT_EQ(reboot.recover(generation), TransferResult::Ok);
      ASSERT_EQ(reboot.begin(initial, "/book.epub"), TransferResult::Ok);
      storage.inject(step, after);
      EXPECT_EQ(reboot.append(initial.transaction, initial.owner, 0, storage.expected), TransferResult::IoError);
      storage.inject(0, false);
      Transfer retry(storage, workspace);
      ASSERT_EQ(retry.recover(generation), TransferResult::Ok);
      const auto offset = retry.current()->durableOffset;
      EXPECT_EQ(storage.files[TRANSFER_STAGE].size(), offset);
      if (offset == 0) {
        ASSERT_EQ(retry.append(initial.transaction, initial.owner, 0, storage.expected), TransferResult::Ok);
      }
      ASSERT_EQ(retry.commit(initial.transaction, initial.owner), TransferResult::Ok);
    }
  }
}

TEST_F(CompanionTransferTest, CorruptInactiveCheckpointFallsBackToDurableOffset) {
  Transfer transfer(storage, workspace);
  receive(transfer);
  ASSERT_TRUE(storage.files.contains(TRANSFER_JOURNALS[1]));
  storage.files[TRANSFER_JOURNALS[1]][20] ^= 1;
  Transfer reboot(storage, workspace);
  ASSERT_EQ(reboot.recover(generation), TransferResult::Ok);
  EXPECT_EQ(reboot.current()->durableOffset, 0U);
  EXPECT_TRUE(storage.files[TRANSFER_STAGE].empty());
  EXPECT_EQ(storage.files["/book.epub"], std::vector<uint8_t>({99}));
}

TEST_F(CompanionTransferTest, DetectsWrongStorageAndDoesNotMutateClonedCard) {
  Transfer transfer(storage, workspace);
  receive(transfer);
  const auto previous = storage.files;
  Identity other{};
  other[0] = 7;
  Transfer clone(storage, workspace);
  EXPECT_EQ(clone.recover(other), TransferResult::WrongStorage);
  EXPECT_EQ(clone.commit(initial.transaction, initial.owner), TransferResult::NoTransaction);
  EXPECT_EQ(storage.files, previous);
}

TEST_F(CompanionTransferTest, AbortRetainsOriginalAndCanBeRetried) {
  Transfer transfer(storage, workspace);
  receive(transfer);
  EXPECT_EQ(transfer.abort(initial.transaction, initial.owner), TransferResult::Ok);
  EXPECT_EQ(transfer.abort(initial.transaction, initial.owner), TransferResult::Ok);
  EXPECT_FALSE(storage.files.contains(TRANSFER_STAGE));
  EXPECT_EQ(storage.files["/book.epub"], std::vector<uint8_t>({99}));
  Transfer reboot(storage, workspace);
  EXPECT_EQ(reboot.recover(generation), TransferResult::Ok);
}

TEST_F(CompanionTransferTest, RejectsBadHashBeforeRenamingOriginal) {
  Transfer transfer(storage, workspace);
  receive(transfer);
  storage.files[TRANSFER_STAGE][0] ^= 1;
  EXPECT_EQ(transfer.commit(initial.transaction, initial.owner), TransferResult::HashMismatch);
  EXPECT_EQ(storage.files["/book.epub"], std::vector<uint8_t>({99}));
  EXPECT_FALSE(storage.files.contains(TRANSFER_BACKUP));
}

TEST_F(CompanionTransferTest, RejectsTraversalProtectedPathsAndBusyReplacement) {
  Transfer transfer(storage, workspace);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  for (const char* path :
       {"/../book", "/books/./book", "/books//book", "/.crosspoint/companion/backup", "relative", "/books/"}) {
    EXPECT_EQ(transfer.begin(initial, path), TransferResult::Invalid);
  }
  ASSERT_EQ(transfer.begin(initial, "/book.epub"), TransferResult::Ok);
  EXPECT_EQ(transfer.begin(initial, "/book.epub"), TransferResult::Ok);
  auto other = initial;
  other.transaction[0] = 9;
  EXPECT_EQ(transfer.begin(other, "/other.epub"), TransferResult::Busy);
}

TEST_F(CompanionTransferTest, InsufficientStorageDoesNotAdvanceAcknowledgedOffset) {
  Transfer transfer(storage, workspace);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(transfer.begin(initial, "/book.epub"), TransferResult::Ok);
  storage.inject(1, false);
  EXPECT_EQ(transfer.append(initial.transaction, initial.owner, 0, storage.expected), TransferResult::IoError);
  EXPECT_EQ(transfer.current()->durableOffset, 0U);
}

TEST_F(CompanionTransferTest, RejectsEveryTornCheckpointPrefixAndKeepsOriginal) {
  Transfer transfer(storage, workspace);
  receive(transfer);
  const auto saved = storage.files;
  for (size_t prefix = 0; prefix < saved.at(TRANSFER_JOURNALS[1]).size(); ++prefix) {
    storage.files = saved;
    storage.files[TRANSFER_JOURNALS[1]].resize(prefix);
    Transfer reboot(storage, workspace);
    ASSERT_EQ(reboot.recover(generation), TransferResult::Ok) << prefix;
    EXPECT_EQ(reboot.current()->durableOffset, 0U);
    EXPECT_EQ(storage.files["/book.epub"], std::vector<uint8_t>({99}));
  }
}

TEST_F(CompanionTransferTest, BlocksRecoveryWithUnexpectedBackupOrLostStage) {
  Transfer transfer(storage, workspace);
  receive(transfer);
  storage.files[TRANSFER_BACKUP] = {99};
  Transfer reboot(storage, workspace);
  EXPECT_EQ(reboot.recover(generation), TransferResult::Corrupt);
  EXPECT_EQ(reboot.commit(initial.transaction, initial.owner), TransferResult::NoTransaction);
  storage.files.erase(TRANSFER_BACKUP);
  storage.files.erase(TRANSFER_STAGE);
  EXPECT_EQ(reboot.recover(generation), TransferResult::Corrupt);
}

TEST_F(CompanionTransferTest, SessionScratchDoesNotOverwriteRadioOrCommandBuffers) {
  workspace.fill(0xa5);
  Transfer transfer(storage, std::span(workspace).subspan(TRANSFER_OFFSET));
  receive(transfer);
  ASSERT_EQ(transfer.commit(initial.transaction, initial.owner), TransferResult::Ok);
  Transfer reboot(storage, std::span(workspace).subspan(TRANSFER_OFFSET));
  ASSERT_EQ(reboot.recover(generation), TransferResult::Ok);
  EXPECT_TRUE(
      std::all_of(workspace.begin(), workspace.begin() + TRANSFER_OFFSET, [](uint8_t byte) { return byte == 0xa5; }));
}
TEST_F(CompanionTransferTest, RequiresOnlyOneJournalOfScratchButRejectsSmallerBuffers) {
  Transfer tooSmall(storage, std::span(workspace).first(TRANSFER_JOURNAL_SIZE - 1));
  EXPECT_EQ(tooSmall.recover(generation), TransferResult::Invalid);
  Transfer transfer(storage, std::span(workspace).first(TRANSFER_JOURNAL_SIZE));
  receive(transfer);
  EXPECT_EQ(transfer.commit(initial.transaction, initial.owner), TransferResult::Ok);
}

TEST_F(CompanionTransferTest, HandlerUsesAuthenticatedOwnerAndCanonicalHashDestination) {
  Transfer transfer(storage, workspace);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  std::array<uint8_t, 256> body{};
  std::array<uint8_t, 100> response{};
  std::string path = "/Books/Companion/07" + std::string(62, '0') + ".epub";
  encodeRecord(initial, body);
  body[TRANSFER_STATE_SIZE] = path.size();
  std::memcpy(body.data() + TRANSFER_STATE_SIZE + 1, path.data(), path.size());
  auto request = std::span(body).first(TRANSFER_STATE_SIZE + 1 + path.size());
  Identity stranger{};
  stranger[0] = 42;
  ASSERT_EQ(handleTransfer(transfer, Command::BeginTransfer, request, stranger, response), 1);
  EXPECT_EQ(response[0], static_cast<uint8_t>(TransferResult::Unauthorized));
  EXPECT_EQ(storage.mutations, 0);
  body[TRANSFER_STATE_SIZE + 1 + std::string_view("/Books/Companion/").size()] = '8';
  ASSERT_EQ(handleTransfer(transfer, Command::BeginTransfer, request, initial.owner, response), 1);
  EXPECT_EQ(response[0], static_cast<uint8_t>(TransferResult::Invalid));
  body[TRANSFER_STATE_SIZE + 1 + std::string_view("/Books/Companion/").size()] = '0';
  EXPECT_EQ(handleTransfer(transfer, Command::BeginTransfer, request, initial.owner, std::span(response).first(99)), 0);
  EXPECT_EQ(storage.mutations, 0);
  ASSERT_EQ(handleTransfer(transfer, Command::BeginTransfer, request, initial.owner, response), 100);
  TransferState decoded;
  ASSERT_TRUE(decodeRecord(std::span(response).subspan(1), decoded));
  EXPECT_EQ(decoded, initial);
  ASSERT_EQ(handleTransfer(transfer, Command::TransferStatus, initial.transaction, stranger, response), 1);
  EXPECT_EQ(response[0], static_cast<uint8_t>(TransferResult::Unauthorized));
  EXPECT_EQ(handleTransfer(transfer, Command::TransferStatus, initial.transaction, initial.owner, response), 100);
  std::copy(initial.transaction.begin(), initial.transaction.end(), body.begin());
  std::fill(body.begin() + 16, body.begin() + 24, 0);
  std::copy(storage.expected.begin(), storage.expected.end(), body.begin() + 24);
  EXPECT_EQ(handleTransfer(transfer, Command::TransferChunk, std::span(body).first(30), initial.owner, response), 100);
  EXPECT_EQ(handleTransfer(transfer, Command::Commit, initial.transaction, initial.owner, response), 100);
  EXPECT_EQ(handleTransfer(transfer, Command::Commit, initial.transaction, initial.owner, response), 100);
  EXPECT_EQ(storage.files[path], storage.expected);
}

TEST_F(CompanionTransferTest, DurableChunkRetriesCompareBytesWithoutWritesAcrossScratchWindows) {
  storage.expected.reserve(600);
  storage.expected.resize(600);
  for (size_t i = 0; i < storage.expected.size(); ++i) storage.expected[i] = static_cast<uint8_t>(i);
  initial.length = storage.expected.size();
  Transfer transfer(storage, std::span(workspace).first(TRANSFER_JOURNAL_SIZE));
  receive(transfer);
  const int mutations = storage.mutations;
  EXPECT_EQ(transfer.append(initial.transaction, initial.owner, 0, storage.expected), TransferResult::Ok);
  EXPECT_EQ(storage.mutations, mutations);
  storage.expected.back() ^= 1;
  EXPECT_EQ(transfer.append(initial.transaction, initial.owner, 0, storage.expected), TransferResult::Offset);
  EXPECT_EQ(storage.mutations, mutations);
  storage.expected.back() ^= 1;
  EXPECT_EQ(transfer.append(initial.transaction, initial.owner, 599, std::span(storage.expected).first(2)),
            TransferResult::Offset);
  storage.failRead = true;
  EXPECT_EQ(transfer.append(initial.transaction, initial.owner, 0, storage.expected), TransferResult::IoError);
  storage.failRead = false;
  Transfer restarted(storage, std::span(workspace).first(TRANSFER_JOURNAL_SIZE));
  ASSERT_EQ(restarted.recover(generation), TransferResult::Ok);
  EXPECT_EQ(restarted.append(initial.transaction, initial.owner, 0, storage.expected), TransferResult::Ok);
  EXPECT_EQ(storage.mutations, mutations);
}
TEST_F(CompanionTransferTest, RejectsChunkAliasingCheckpointScratch) {
  Transfer transfer(storage, workspace);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(transfer.begin(initial, "/book.epub"), TransferResult::Ok);
  const int mutations = storage.mutations;
  EXPECT_EQ(transfer.append(initial.transaction, initial.owner, 0, std::span(workspace).first(6)),
            TransferResult::Invalid);
  EXPECT_EQ(storage.mutations, mutations);
}

TEST_F(CompanionTransferTest, DeclaredContractSurvivesRestartAndRejectsRebinding) {
  TransferDeclaration declaration;
  declaration.state = initial;
  declaration.manifest.contentHash = initial.contentHash;
  declaration.manifest.length = initial.length;
  declaration.manifest.kind = ContentKind::Course;
  declaration.manifest.formatVersion = 1;
  declaration.manifest.logicalIdentity[0] = 8;
  Transfer transfer(storage, workspace);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(transfer.begin(declaration, "/course.pack"), TransferResult::Ok);
  ASSERT_EQ(storage.files.at(TRANSFER_JOURNALS[0]).size(), TRANSFER_JOURNAL_SIZE);
  EXPECT_EQ(storage.files.at(TRANSFER_JOURNALS[0])[3], 2);
  ASSERT_EQ(transfer.append(initial.transaction, initial.owner, 0, std::span(storage.expected).first(3)),
            TransferResult::Ok);
  Transfer reboot(storage, workspace);
  ASSERT_EQ(reboot.recover(generation), TransferResult::Ok);
  ASSERT_NE(reboot.contentManifest(), nullptr);
  EXPECT_EQ(*reboot.contentManifest(), declaration.manifest);
  EXPECT_EQ(reboot.current()->durableOffset, 3);
  EXPECT_EQ(reboot.begin(declaration, "/course.pack"), TransferResult::Ok);
  auto changed = declaration;
  changed.manifest.logicalIdentity[0] = 9;
  EXPECT_EQ(reboot.begin(changed, "/course.pack"), TransferResult::Invalid);
  EXPECT_EQ(reboot.begin(initial, "/course.pack"), TransferResult::Invalid);
  ASSERT_EQ(reboot.append(initial.transaction, initial.owner, 3, std::span(storage.expected).subspan(3)),
            TransferResult::Ok);
  ASSERT_EQ(reboot.commit(initial.transaction, initial.owner), TransferResult::Ok);
  Transfer installed(storage, workspace);
  ASSERT_EQ(installed.recover(generation), TransferResult::Ok);
  ASSERT_NE(installed.contentManifest(), nullptr);
  EXPECT_EQ(*installed.contentManifest(), declaration.manifest);
}

TEST_F(CompanionTransferTest, LegacyJournalRemainsReadableWithoutInventedManifest) {
  Transfer transfer(storage, workspace);
  receive(transfer);
  EXPECT_EQ(storage.files.at(TRANSFER_JOURNALS[0]).size(), TRANSFER_LEGACY_JOURNAL_SIZE);
  Transfer reboot(storage, workspace);
  ASSERT_EQ(reboot.recover(generation), TransferResult::Ok);
  EXPECT_EQ(reboot.contentManifest(), nullptr);
}

TEST_F(CompanionTransferTest, DictionaryDeferralStillCompletesOtherContentRecovery) {
  for (const auto kind : {ContentKind::Epub, ContentKind::Font, ContentKind::Course}) {
    storage.files.clear();
    storage.rejectMetadata = false;
    TransferDeclaration declaration;
    declaration.state = initial;
    declaration.manifest.contentHash = initial.contentHash;
    declaration.manifest.length = initial.length;
    declaration.manifest.kind = kind;
    declaration.manifest.formatVersion = 1;
    if (kind == ContentKind::Course) declaration.manifest.logicalIdentity[0] = 8;
    Transfer transfer(storage, workspace);
    ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
    ASSERT_EQ(transfer.begin(declaration, "/book.epub"), TransferResult::Ok);
    ASSERT_EQ(transfer.append(initial.transaction, initial.owner, 0, storage.expected), TransferResult::Ok);
    storage.rejectMetadata = true;
    ASSERT_EQ(transfer.commit(initial.transaction, initial.owner), TransferResult::IoError);
    ASSERT_EQ(transfer.current()->phase, TransferPhase::Installing);
    storage.rejectMetadata = false;
    const auto before = storage.metadataCalls;
    Transfer recovered(storage, workspace);
    ASSERT_EQ(recovered.recover(generation, TransferRecoveryMode::DeferDictionaryInstallation), TransferResult::Ok);
    EXPECT_EQ(recovered.current()->phase, TransferPhase::Committed);
    EXPECT_GT(storage.metadataCalls, before);
    storage.rejectMetadata = true;
    const auto beforeCommitted = storage.metadataCalls;
    EXPECT_EQ(recovered.recover(generation, TransferRecoveryMode::DeferDictionaryInstallation),
              TransferResult::IoError);
    EXPECT_TRUE(recovered.destination().empty());
    EXPECT_GT(storage.metadataCalls, beforeCommitted);
  }
}

TEST_F(CompanionTransferTest, DeclaredJournalRecoversEveryInterruptedCommit) {
  TransferDeclaration declaration;
  declaration.state = initial;
  declaration.manifest.contentHash = initial.contentHash;
  declaration.manifest.length = initial.length;
  declaration.manifest.kind = ContentKind::Course;
  declaration.manifest.formatVersion = 1;
  declaration.manifest.logicalIdentity[0] = 8;
  for (bool after : {false, true}) {
    for (int step = 1; step <= 5; ++step) {
      storage.files.clear();
      storage.inject(0, false);
      storage.files["/course.pack"] = {99};
      Transfer transfer(storage, workspace);
      ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
      ASSERT_EQ(transfer.begin(declaration, "/course.pack"), TransferResult::Ok);
      ASSERT_EQ(transfer.append(initial.transaction, initial.owner, 0, storage.expected), TransferResult::Ok);
      storage.inject(step, after);
      EXPECT_EQ(transfer.commit(initial.transaction, initial.owner), TransferResult::IoError);
      storage.inject(0, false);
      Transfer reboot(storage, workspace);
      ASSERT_EQ(reboot.recover(generation), TransferResult::Ok);
      ASSERT_NE(reboot.contentManifest(), nullptr);
      EXPECT_EQ(*reboot.contentManifest(), declaration.manifest);
      ASSERT_EQ(reboot.commit(initial.transaction, initial.owner), TransferResult::Ok);
      EXPECT_EQ(storage.files["/course.pack"], storage.expected);
    }
  }
}

TEST_F(CompanionTransferTest, DeclaredJournalRejectsEveryTornCheckpointPrefix) {
  TransferDeclaration declaration;
  declaration.state = initial;
  declaration.manifest.contentHash = initial.contentHash;
  declaration.manifest.length = initial.length;
  declaration.manifest.kind = ContentKind::Course;
  declaration.manifest.formatVersion = 1;
  declaration.manifest.logicalIdentity[0] = 8;
  Transfer transfer(storage, workspace);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(transfer.begin(declaration, "/course.pack"), TransferResult::Ok);
  ASSERT_EQ(transfer.append(initial.transaction, initial.owner, 0, storage.expected), TransferResult::Ok);
  const auto saved = storage.files;
  for (size_t prefix = 0; prefix < TRANSFER_JOURNAL_SIZE; ++prefix) {
    storage.files = saved;
    storage.files[TRANSFER_JOURNALS[1]].resize(prefix);
    Transfer reboot(storage, workspace);
    ASSERT_EQ(reboot.recover(generation), TransferResult::Ok) << prefix;
    EXPECT_EQ(reboot.current()->durableOffset, 0);
    ASSERT_NE(reboot.contentManifest(), nullptr);
    EXPECT_EQ(*reboot.contentManifest(), declaration.manifest);
  }
}

TEST_F(CompanionTransferTest, DeclaredJournalRejectsChecksummedManifestMismatch) {
  TransferDeclaration declaration;
  declaration.state = initial;
  declaration.manifest.contentHash = initial.contentHash;
  declaration.manifest.length = initial.length;
  declaration.manifest.kind = ContentKind::Course;
  declaration.manifest.formatVersion = 1;
  declaration.manifest.logicalIdentity[0] = 8;
  Transfer transfer(storage, workspace);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(transfer.begin(declaration, "/course.pack"), TransferResult::Ok);
  const auto saved = storage.files;
  for (size_t offset : {size_t{240}, size_t{243}, size_t{276}, size_t{284}, size_t{288}}) {
    storage.files = saved;
    auto& journal = storage.files[TRANSFER_JOURNALS[0]];
    journal[offset] ^= offset == 288 ? 8 : 1;
    uint32_t checksum = 0xffffffff;
    for (size_t i = 0; i < journal.size() - 4; ++i) {
      checksum ^= journal[i];
      for (unsigned bit = 0; bit < 8; ++bit) checksum = (checksum >> 1) ^ (0xedb88320U & (0U - (checksum & 1U)));
    }
    checksum = ~checksum;
    for (size_t i = 0; i < 4; ++i) journal[journal.size() - 4 + i] = static_cast<uint8_t>(checksum >> (8 * i));
    Transfer reboot(storage, workspace);
    EXPECT_EQ(reboot.recover(generation), TransferResult::Corrupt) << offset;
  }
}

TEST_F(CompanionTransferTest, CourseBindingRecoversEveryInterruptedMetadataMutation) {
  ContentManifest candidate;
  candidate.contentHash = initial.contentHash;
  candidate.kind = ContentKind::Course;
  candidate.length = initial.length;
  candidate.formatVersion = 1;
  candidate.logicalIdentity[0] = 8;
  ContentManifest old = candidate;
  old.contentHash[0] = 9;
  std::array<uint8_t, COURSE_BINDING_SIZE> encoded{};
  ASSERT_EQ(encodeCourseBinding(old, encoded), encoded.size());
  for (bool original : {false, true}) {
    for (bool after : {false, true}) {
      for (int step = 1; step <= (original ? 4 : 2); ++step) {
        SCOPED_TRACE(testing::Message() << original << " " << after << " " << step);
        storage.files.clear();
        storage.files[ACTIVE_COURSE_PATH] = storage.expected;
        if (original) storage.files[COURSE_BINDING_PATH] = {encoded.begin(), encoded.end()};
        storage.inject(step, after);
        EXPECT_EQ(installCourseBinding(storage, candidate, workspace), CourseBindingResult::IoError);
        storage.inject(0, false);
        ASSERT_EQ(installCourseBinding(storage, candidate, workspace), CourseBindingResult::Ok);
        ContentManifest installed;
        ASSERT_TRUE(decodeCourseBinding(storage.files.at(COURSE_BINDING_PATH), installed));
        EXPECT_EQ(installed, candidate);
        EXPECT_FALSE(storage.files.contains(COURSE_BINDING_STAGE));
        EXPECT_FALSE(storage.files.contains(COURSE_BINDING_BACKUP));
        const int mutations = storage.mutations;
        EXPECT_EQ(installCourseBinding(storage, candidate, workspace), CourseBindingResult::Ok);
        EXPECT_EQ(storage.mutations, mutations);
      }
    }
  }
}

TEST_F(CompanionTransferTest, CourseBindingRefusesAnotherFamilyAndCorruptionWithoutWrites) {
  ContentManifest candidate;
  candidate.contentHash = initial.contentHash;
  candidate.kind = ContentKind::Course;
  candidate.length = initial.length;
  candidate.formatVersion = 1;
  candidate.logicalIdentity[0] = 8;
  storage.files[ACTIVE_COURSE_PATH] = storage.expected;
  auto old = candidate;
  old.logicalIdentity[0] = 9;
  std::array<uint8_t, COURSE_BINDING_SIZE> encoded{};
  ASSERT_EQ(encodeCourseBinding(old, encoded), encoded.size());
  storage.files[COURSE_BINDING_PATH] = {encoded.begin(), encoded.end()};
  auto saved = storage.files;
  EXPECT_EQ(installCourseBinding(storage, candidate, workspace), CourseBindingResult::DifferentCourse);
  EXPECT_EQ(storage.files, saved);
  EXPECT_EQ(storage.mutations, 0);
  storage.files[COURSE_BINDING_PATH][4] ^= 1;
  saved = storage.files;
  EXPECT_EQ(installCourseBinding(storage, candidate, workspace), CourseBindingResult::Corrupt);
  EXPECT_EQ(storage.files, saved);
  EXPECT_EQ(storage.mutations, 0);
  storage.files.erase(COURSE_BINDING_PATH);
  storage.files[ACTIVE_COURSE_PATH][0] ^= 1;
  EXPECT_EQ(installCourseBinding(storage, candidate, workspace), CourseBindingResult::HashMismatch);
  EXPECT_EQ(storage.mutations, 0);
}

TEST_F(CompanionTransferTest, CourseBindingCodecRejectsMalformedAndTruncatedRecords) {
  ContentManifest candidate;
  candidate.contentHash = initial.contentHash;
  candidate.kind = ContentKind::Course;
  candidate.length = initial.length;
  candidate.formatVersion = 1;
  candidate.logicalIdentity[0] = 8;
  std::array<uint8_t, COURSE_BINDING_SIZE> bytes{};
  ASSERT_EQ(encodeCourseBinding(candidate, bytes), bytes.size());
  for (size_t count = 0; count < bytes.size(); ++count) {
    auto retained = candidate;
    EXPECT_FALSE(decodeCourseBinding(std::span(bytes).first(count), retained));
    EXPECT_EQ(retained, candidate);
  }
  for (size_t offset = 0; offset < bytes.size(); ++offset) {
    auto malformed = bytes;
    malformed[offset] ^= 1;
    auto retained = candidate;
    EXPECT_FALSE(decodeCourseBinding(malformed, retained));
    EXPECT_EQ(retained, candidate);
  }
  candidate.logicalIdentity.fill(0);
  EXPECT_EQ(encodeCourseBinding(candidate, bytes), 0);
  bool present = false;
  EXPECT_EQ(readCourseBinding(storage, COURSE_BINDING_PATH, std::span(workspace).first(COURSE_BINDING_SIZE - 1),
                              candidate, present),
            CourseBindingResult::Invalid);
}

TEST_F(CompanionTransferTest, CourseReplacementRequiresBoundFamilyOrExactLegacyPack) {
  ContentManifest candidate;
  candidate.contentHash = initial.contentHash;
  candidate.kind = ContentKind::Course;
  candidate.length = initial.length;
  candidate.formatVersion = 1;
  candidate.logicalIdentity[0] = 8;
  EXPECT_EQ(authorizeCourseReplacement(storage, candidate, false, workspace), CourseBindingResult::Ok);
  EXPECT_EQ(authorizeCourseReplacement(storage, candidate, true, workspace), CourseBindingResult::UnboundHistory);
  storage.files[ACTIVE_COURSE_PATH] = storage.expected;
  EXPECT_EQ(authorizeCourseReplacement(storage, candidate, true, workspace), CourseBindingResult::Ok);
  auto updated = candidate;
  updated.contentHash[0] = 9;
  EXPECT_EQ(authorizeCourseReplacement(storage, updated, true, workspace), CourseBindingResult::UnboundHistory);
  std::array<uint8_t, COURSE_BINDING_SIZE> encoded{};
  ASSERT_EQ(encodeCourseBinding(candidate, encoded), encoded.size());
  storage.files[COURSE_BINDING_PATH] = {encoded.begin(), encoded.end()};
  EXPECT_EQ(authorizeCourseReplacement(storage, updated, true, workspace), CourseBindingResult::Ok);
  updated.logicalIdentity[0] = 9;
  EXPECT_EQ(authorizeCourseReplacement(storage, updated, true, workspace), CourseBindingResult::DifferentCourse);
  updated.logicalIdentity = candidate.logicalIdentity;
  storage.files[ACTIVE_COURSE_PATH][0] ^= 1;
  EXPECT_EQ(authorizeCourseReplacement(storage, updated, true, workspace), CourseBindingResult::HashMismatch);
  storage.files[ACTIVE_COURSE_PATH] = storage.expected;
  storage.files[COURSE_BINDING_STAGE] = {1};
  EXPECT_EQ(authorizeCourseReplacement(storage, updated, true, workspace), CourseBindingResult::Corrupt);
  EXPECT_EQ(storage.mutations, 0);
}

TEST_F(CompanionTransferTest, RejectsInvalidDeclaredContentBeforePersistingInstallIntent) {
  TransferDeclaration declaration;
  declaration.state = initial;
  declaration.manifest.contentHash = initial.contentHash;
  declaration.manifest.length = initial.length;
  declaration.manifest.kind = ContentKind::Course;
  declaration.manifest.formatVersion = 1;
  declaration.manifest.logicalIdentity[0] = 8;
  storage.files[ACTIVE_COURSE_PATH] = {99};
  Transfer transfer(storage, workspace);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(transfer.begin(declaration, ACTIVE_COURSE_PATH), TransferResult::Ok);
  ASSERT_EQ(transfer.append(initial.transaction, initial.owner, 0, storage.expected), TransferResult::Ok);
  storage.rejectValidation = true;
  const auto saved = storage.files;
  EXPECT_EQ(transfer.commit(initial.transaction, initial.owner), TransferResult::Invalid);
  EXPECT_EQ(storage.files, saved);
  EXPECT_EQ(transfer.current()->phase, TransferPhase::Receiving);
  EXPECT_EQ(storage.metadataCalls, 0);
}
TEST_F(CompanionTransferTest, RecoversPackAndBindingBeforeCommittingEveryInterruptedMutation) {
  TransferDeclaration declaration;
  declaration.state = initial;
  declaration.manifest.contentHash = initial.contentHash;
  declaration.manifest.length = initial.length;
  declaration.manifest.kind = ContentKind::Course;
  declaration.manifest.formatVersion = 1;
  declaration.manifest.logicalIdentity[0] = 8;
  auto old = declaration.manifest;
  old.contentHash[0] = 9;
  std::array<uint8_t, COURSE_BINDING_SIZE> encoded{};
  ASSERT_EQ(encodeCourseBinding(old, encoded), encoded.size());
  storage.useCourseMetadata = true;
  for (bool after : {false, true}) {
    for (int step = 1; step <= 9; ++step) {
      SCOPED_TRACE(testing::Message() << "after=" << after << " step=" << step);
      storage.files.clear();
      storage.files[ACTIVE_COURSE_PATH] = {99};
      storage.files[COURSE_BINDING_PATH] = {encoded.begin(), encoded.end()};
      storage.inject(0, false);
      Transfer transfer(storage, workspace);
      ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
      ASSERT_EQ(transfer.begin(declaration, ACTIVE_COURSE_PATH), TransferResult::Ok);
      ASSERT_EQ(transfer.append(initial.transaction, initial.owner, 0, storage.expected), TransferResult::Ok);
      storage.inject(step, after);
      EXPECT_EQ(transfer.commit(initial.transaction, initial.owner), TransferResult::IoError);
      storage.inject(0, false);
      Transfer reboot(storage, workspace);
      ASSERT_EQ(reboot.recover(generation), TransferResult::Ok);
      ASSERT_EQ(reboot.commit(initial.transaction, initial.owner), TransferResult::Ok);
      EXPECT_EQ(reboot.current()->phase, TransferPhase::Committed);
      EXPECT_EQ(storage.files.at(ACTIVE_COURSE_PATH), storage.expected);
      ContentManifest bound;
      ASSERT_TRUE(decodeCourseBinding(storage.files.at(COURSE_BINDING_PATH), bound));
      EXPECT_EQ(bound, declaration.manifest);
      EXPECT_FALSE(storage.files.contains(TRANSFER_BACKUP));
      EXPECT_FALSE(storage.files.contains(COURSE_BINDING_BACKUP));
      EXPECT_FALSE(storage.files.contains(COURSE_BINDING_STAGE));
    }
  }
}
TEST_F(CompanionTransferTest, MetadataFailureRetainsInstallingStateAndBackupUntilRecovery) {
  TransferDeclaration declaration;
  declaration.state = initial;
  declaration.manifest.contentHash = initial.contentHash;
  declaration.manifest.length = initial.length;
  declaration.manifest.kind = ContentKind::Course;
  declaration.manifest.formatVersion = 1;
  declaration.manifest.logicalIdentity[0] = 8;
  storage.files[ACTIVE_COURSE_PATH] = {99};
  Transfer transfer(storage, workspace);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(transfer.begin(declaration, ACTIVE_COURSE_PATH), TransferResult::Ok);
  ASSERT_EQ(transfer.append(initial.transaction, initial.owner, 0, storage.expected), TransferResult::Ok);
  storage.rejectMetadata = true;
  EXPECT_EQ(transfer.commit(initial.transaction, initial.owner), TransferResult::IoError);
  EXPECT_EQ(transfer.current()->phase, TransferPhase::Installing);
  EXPECT_TRUE(storage.files.contains(TRANSFER_BACKUP));
  Transfer blocked(storage, workspace);
  EXPECT_EQ(blocked.recover(generation), TransferResult::IoError);
  EXPECT_TRUE(storage.files.contains(TRANSFER_BACKUP));
  storage.rejectMetadata = false;
  storage.rejectValidation = true;
  Transfer reboot(storage, workspace);
  ASSERT_EQ(reboot.recover(generation), TransferResult::Ok);
  EXPECT_EQ(reboot.current()->phase, TransferPhase::Committed);
  EXPECT_FALSE(storage.files.contains(TRANSFER_BACKUP));
}

TEST_F(CompanionTransferTest, DeclaredHandlerRequiresCanonicalDestinationOwnerAndFormat) {
  TransferDeclaration declaration;
  declaration.state = initial;
  declaration.manifest.contentHash = initial.contentHash;
  declaration.manifest.length = initial.length;
  declaration.manifest.kind = ContentKind::Course;
  declaration.manifest.formatVersion = 1;
  declaration.manifest.logicalIdentity[0] = 8;
  std::array<uint8_t, 300> body{};
  std::array<uint8_t, 100> response{};
  auto request = [&](const TransferDeclaration& value, std::string_view path) {
    EXPECT_EQ(encodeTransferDeclaration(value, body), TRANSFER_DECLARATION_SIZE);
    body[TRANSFER_DECLARATION_SIZE] = path.size();
    std::copy(path.begin(), path.end(), body.begin() + TRANSFER_DECLARATION_SIZE + 1);
    return std::span(body).first(TRANSFER_DECLARATION_SIZE + 1 + path.size());
  };
  Transfer transfer(storage, workspace);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  const auto saved = storage.files;
  for (const auto path : {"/tinta/other.pack", "/tinta/../tinta/course.pack", "/Books/Companion/course.pack"}) {
    ASSERT_EQ(handleTransfer(transfer, Command::BeginTransfer, request(declaration, path), initial.owner, response), 1);
    EXPECT_EQ(response[0], static_cast<uint8_t>(TransferResult::Invalid));
    EXPECT_EQ(storage.files, saved);
  }
  auto wrong = declaration;
  wrong.manifest.formatVersion = 2;
  ASSERT_EQ(
      handleTransfer(transfer, Command::BeginTransfer, request(wrong, ACTIVE_COURSE_PATH), initial.owner, response), 1);
  EXPECT_EQ(response[0], static_cast<uint8_t>(TransferResult::Invalid));
  for (const auto kind : {ContentKind::Font, ContentKind::Dictionary, ContentKind::Firmware, ContentKind::Epub}) {
    wrong = declaration;
    wrong.manifest.kind = kind;
    wrong.manifest.logicalIdentity.fill(0);
    ASSERT_EQ(
        handleTransfer(transfer, Command::BeginTransfer, request(wrong, ACTIVE_COURSE_PATH), initial.owner, response),
        1);
    EXPECT_EQ(response[0], static_cast<uint8_t>(TransferResult::Invalid));
    EXPECT_EQ(storage.files, saved);
  }
  ASSERT_EQ(encodeRecord(initial, body), TRANSFER_STATE_SIZE);
  constexpr std::string_view legacyPath = ACTIVE_COURSE_PATH;
  body[TRANSFER_STATE_SIZE] = legacyPath.size();
  std::copy(legacyPath.begin(), legacyPath.end(), body.begin() + TRANSFER_STATE_SIZE + 1);
  ASSERT_EQ(handleTransfer(transfer, Command::BeginTransfer,
                           std::span(body).first(TRANSFER_STATE_SIZE + 1 + legacyPath.size()), initial.owner, response),
            1);
  EXPECT_EQ(response[0], static_cast<uint8_t>(TransferResult::Invalid));
  EXPECT_EQ(storage.files, saved);
  Identity stranger{};
  stranger[0] = 9;
  ASSERT_EQ(
      handleTransfer(transfer, Command::BeginTransfer, request(declaration, ACTIVE_COURSE_PATH), stranger, response),
      1);
  EXPECT_EQ(response[0], static_cast<uint8_t>(TransferResult::Unauthorized));
  EXPECT_EQ(storage.files, saved);
  ASSERT_EQ(handleTransfer(transfer, Command::BeginTransfer, request(declaration, ACTIVE_COURSE_PATH), initial.owner,
                           response),
            100);
  EXPECT_EQ(response[0], static_cast<uint8_t>(TransferResult::Ok));
  ASSERT_NE(transfer.contentManifest(), nullptr);
  EXPECT_EQ(*transfer.contentManifest(), declaration.manifest);
  EXPECT_EQ(handleTransfer(transfer, Command::BeginTransfer, request(declaration, ACTIVE_COURSE_PATH), initial.owner,
                           response),
            100);
  wrong = declaration;
  wrong.manifest.logicalIdentity[0] = 9;
  ASSERT_EQ(
      handleTransfer(transfer, Command::BeginTransfer, request(wrong, ACTIVE_COURSE_PATH), initial.owner, response), 1);
  EXPECT_EQ(response[0], static_cast<uint8_t>(TransferResult::Invalid));
}

TEST_F(CompanionTransferTest, SharedDispatchInvalidatesOnceAcrossCommitRecoveryAndRetry) {
  for (const bool afterEffect : {false, true}) {
    for (int step = 1; step <= 12; ++step) {
      storage.files.clear();
      storage.inject(0, false);
      Transfer transfer(storage, workspace);
      receive(transfer);
      std::array<uint8_t, MAX_CONTROL_PAYLOAD> response{};
      storage.inject(step, afterEffect);
      const auto first =
          dispatchTransfer(transfer, generation, Command::Commit, initial.transaction, initial.owner, response);
      ASSERT_GT(first.length, 0u);
      ASSERT_FALSE(first.recoveryBlocked);
      storage.inject(0, false);
      const auto retry =
          dispatchTransfer(transfer, generation, Command::Commit, initial.transaction, initial.owner, response);
      ASSERT_GT(retry.length, 0u);
      EXPECT_FALSE(retry.recoveryBlocked);
      EXPECT_EQ(static_cast<unsigned>(first.inventoryChanged) + static_cast<unsigned>(retry.inventoryChanged), 1u);
      EXPECT_EQ(storage.files["/book.epub"], storage.expected);
      EXPECT_EQ(transfer.current()->phase, TransferPhase::Committed);
      const int mutations = storage.mutations;
      const auto repeated =
          dispatchTransfer(transfer, generation, Command::Commit, initial.transaction, initial.owner, response);
      EXPECT_FALSE(repeated.inventoryChanged);
      EXPECT_FALSE(repeated.recoveryBlocked);
      EXPECT_EQ(storage.mutations, mutations);
    }
  }
}
TEST_F(CompanionTransferTest, SharedDispatchBlocksUnreadableRecoveryWithoutClaimingInventoryChange) {
  Transfer transfer(storage, workspace);
  receive(transfer);
  std::array<uint8_t, MAX_CONTROL_PAYLOAD> response{};
  storage.failRead = true;
  storage.inject(1, true);
  const auto result =
      dispatchTransfer(transfer, generation, Command::Commit, initial.transaction, initial.owner, response);
  ASSERT_EQ(result.length, 1u);
  EXPECT_EQ(response[0], static_cast<uint8_t>(TransferResult::IoError));
  EXPECT_TRUE(result.recoveryBlocked);
  EXPECT_FALSE(result.inventoryChanged);
}
TEST_F(CompanionTransferTest, SharedDispatchShortReplyAndUnauthorizedCommandDoNotMutate) {
  Transfer transfer(storage, workspace);
  receive(transfer);
  const int mutations = storage.mutations;
  auto result = dispatchTransfer(transfer, generation, Command::Commit, initial.transaction, initial.owner, {});
  EXPECT_EQ(result.length, 0u);
  EXPECT_FALSE(result.inventoryChanged);
  EXPECT_FALSE(result.recoveryBlocked);
  std::array<uint8_t, MAX_CONTROL_PAYLOAD> response{};
  Identity stranger{};
  stranger.fill(9);
  result = dispatchTransfer(transfer, generation, Command::Commit, initial.transaction, stranger, response);
  EXPECT_EQ(response[0], static_cast<uint8_t>(TransferResult::Unauthorized));
  EXPECT_FALSE(result.inventoryChanged);
  EXPECT_FALSE(result.recoveryBlocked);
  EXPECT_EQ(storage.mutations, mutations);
}

TEST_F(CompanionTransferTest, StorageReceivesDurableContextDuringCommitAndRecovery) {
  TransferDeclaration declaration;
  declaration.state = initial;
  declaration.manifest.contentHash = initial.contentHash;
  declaration.manifest.length = initial.length;
  declaration.manifest.kind = ContentKind::Course;
  declaration.manifest.formatVersion = 1;
  declaration.manifest.logicalIdentity[0] = 5;
  Transfer transfer(storage, workspace);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(transfer.begin(declaration, ACTIVE_COURSE_PATH), TransferResult::Ok);
  ASSERT_EQ(transfer.append(initial.transaction, initial.owner, 0, storage.expected), TransferResult::Ok);
  storage.rejectMetadata = true;
  ASSERT_EQ(transfer.commit(initial.transaction, initial.owner), TransferResult::IoError);
  ASSERT_TRUE(storage.sawValidationContext);
  ASSERT_TRUE(storage.sawMetadataContext);
  EXPECT_EQ(storage.validationState.phase, TransferPhase::Receiving);
  EXPECT_EQ(storage.validationState.durableOffset, initial.length);
  EXPECT_EQ(storage.metadataState.phase, TransferPhase::Installing);
  EXPECT_EQ(storage.metadataState.transaction, initial.transaction);
  EXPECT_EQ(storage.metadataState.owner, initial.owner);
  EXPECT_EQ(storage.metadataState.storageGeneration, generation);
  storage.rejectMetadata = false;
  storage.sawMetadataContext = false;
  Transfer reboot(storage, workspace);
  ASSERT_EQ(reboot.recover(generation), TransferResult::Ok);
  EXPECT_TRUE(storage.sawMetadataContext);
  EXPECT_EQ(storage.metadataState.phase, TransferPhase::Installing);
  EXPECT_EQ(storage.metadataState.transaction, initial.transaction);
  storage.sawMetadataContext = false;
  Transfer committedReboot(storage, workspace);
  ASSERT_EQ(committedReboot.recover(generation), TransferResult::Ok);
  EXPECT_TRUE(storage.sawMetadataContext);
  EXPECT_EQ(storage.metadataState.phase, TransferPhase::Committed);
  EXPECT_EQ(storage.metadataState.transaction, initial.transaction);
  EXPECT_EQ(storage.metadataState.storageGeneration, generation);
}

TEST_F(CompanionTransferTest, DeclaredFirmwareRequiresCanonicalPathAndImageFormat) {
  TransferDeclaration declaration;
  declaration.state = initial;
  declaration.state.length = 65536;
  declaration.manifest.kind = ContentKind::Firmware;
  declaration.manifest.contentHash = initial.contentHash;
  declaration.manifest.length = 65536;
  declaration.manifest.formatVersion = 1;
  Transfer transfer(storage, workspace);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  auto begin = [&](const TransferDeclaration& value, std::string_view path) {
    std::array<uint8_t, MAX_CONTROL_PAYLOAD> body{};
    EXPECT_EQ(encodeTransferDeclaration(value, body), TRANSFER_DECLARATION_SIZE);
    body[TRANSFER_DECLARATION_SIZE] = path.size();
    std::copy(path.begin(), path.end(), body.begin() + TRANSFER_DECLARATION_SIZE + 1);
    std::array<uint8_t, 1 + TRANSFER_STATE_SIZE> response;
    EXPECT_GT(
        handleTransfer(transfer, Command::BeginTransfer,
                       std::span(body).first(TRANSFER_DECLARATION_SIZE + 1 + path.size()), initial.owner, response),
        0);
    return static_cast<TransferResult>(response[0]);
  };
  EXPECT_EQ(begin(declaration, "/tinta/course.pack"), TransferResult::Invalid);
  EXPECT_TRUE(storage.files.empty());
  auto wrong = declaration;
  wrong.manifest.formatVersion = 2;
  EXPECT_EQ(begin(wrong, "/Companion/firmware.bin"), TransferResult::Invalid);
  EXPECT_TRUE(storage.files.empty());
  EXPECT_EQ(begin(declaration, "/Companion/firmware.bin"), TransferResult::Ok);
  EXPECT_EQ(transfer.contentManifest()->kind, ContentKind::Firmware);
}

TEST_F(CompanionTransferTest, DictionaryMembersResumeWithoutRenamingArchiveToDestination) {
  TransferDeclaration declaration;
  declaration.state = initial;
  declaration.manifest = {initial.contentHash, ContentKind::Dictionary, initial.length, 1, {}};
  constexpr const char* destination = "/dictionaries/test/dictionary";
  Transfer transfer(storage, workspace);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(transfer.begin(declaration, destination), TransferResult::Ok);
  ASSERT_EQ(transfer.append(initial.transaction, initial.owner, 0, storage.expected), TransferResult::Ok);
  storage.enableDictionaryPublication = true;
  storage.failDictionaryPublication = true;
  ASSERT_EQ(transfer.commit(initial.transaction, initial.owner), TransferResult::IoError);
  EXPECT_TRUE(storage.dictionaryMembersInstalled);
  EXPECT_EQ(transfer.current()->phase, TransferPhase::Installing);
  EXPECT_TRUE(storage.files.contains(TRANSFER_STAGE));
  EXPECT_FALSE(storage.files.contains(destination));
  EXPECT_FALSE(storage.files.contains(TRANSFER_BACKUP));
  const auto saved = storage.files;
  Transfer reboot(storage, workspace);
  ASSERT_EQ(reboot.recover(generation, TransferRecoveryMode::DeferDictionaryInstallation), TransferResult::Ok);
  EXPECT_EQ(storage.files, saved);
  EXPECT_EQ(storage.dictionaryPublicationCalls, 1u);
  storage.failDictionaryPublication = false;
  ASSERT_EQ(reboot.commit(initial.transaction, initial.owner), TransferResult::Ok);
  EXPECT_EQ(reboot.current()->phase, TransferPhase::Committed);
  EXPECT_EQ(storage.dictionaryPublicationCalls, 2u);
  EXPECT_EQ(storage.metadataState.phase, TransferPhase::Installing);
  EXPECT_FALSE(storage.files.contains(destination));
  Transfer completed(storage, workspace);
  ASSERT_EQ(completed.recover(generation), TransferResult::Ok);
  EXPECT_EQ(completed.current()->phase, TransferPhase::Committed);
  EXPECT_EQ(storage.dictionaryPublicationCalls, 2u);
  storage.files.at(TRANSFER_STAGE)[0] ^= 1;
  EXPECT_EQ(completed.recover(generation), TransferResult::HashMismatch);
}

TEST_F(CompanionTransferTest, MissingDictionaryInstallerCannotCommitUploadedZip) {
  TransferDeclaration declaration;
  declaration.state = initial;
  declaration.manifest = {initial.contentHash, ContentKind::Dictionary, initial.length, 1, {}};
  Transfer transfer(storage, workspace);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(transfer.begin(declaration, "/dictionaries/test/dictionary"), TransferResult::Ok);
  ASSERT_EQ(transfer.append(initial.transaction, initial.owner, 0, storage.expected), TransferResult::Ok);
  ASSERT_EQ(transfer.commit(initial.transaction, initial.owner), TransferResult::IoError);
  EXPECT_EQ(transfer.current()->phase, TransferPhase::Installing);
  EXPECT_TRUE(storage.files.contains(TRANSFER_STAGE));
  EXPECT_FALSE(storage.files.contains("/dictionaries/test/dictionary"));
  EXPECT_EQ(storage.metadataCalls, 0);
}

TEST_F(CompanionTransferTest, DictionaryCommitReceiptTearsRecoverPublishedMembers) {
  for (const bool afterEffect : {false, true}) {
    storage.files.clear();
    storage.enableDictionaryPublication = true;
    storage.dictionaryPublicationCalls = 0;
    storage.inject(0, false);
    TransferDeclaration declaration;
    declaration.state = initial;
    declaration.manifest = {initial.contentHash, ContentKind::Dictionary, initial.length, 1, {}};
    Transfer transfer(storage, workspace);
    ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
    ASSERT_EQ(transfer.begin(declaration, "/dictionaries/test/dictionary"), TransferResult::Ok);
    ASSERT_EQ(transfer.append(initial.transaction, initial.owner, 0, storage.expected), TransferResult::Ok);
    // Installing receipt succeeds; committed receipt fails before/after its write.
    storage.inject(2, afterEffect);
    ASSERT_EQ(transfer.commit(initial.transaction, initial.owner), TransferResult::IoError);
    EXPECT_TRUE(storage.dictionaryMembersInstalled);
    storage.inject(0, false);
    Transfer reboot(storage, workspace);
    ASSERT_EQ(reboot.recover(generation), TransferResult::Ok);
    EXPECT_EQ(reboot.current()->phase, TransferPhase::Committed);
    EXPECT_EQ(storage.dictionaryPublicationCalls, afterEffect ? 1u : 2u);
    EXPECT_FALSE(storage.files.contains("/dictionaries/test/dictionary"));
    EXPECT_TRUE(storage.files.contains(TRANSFER_STAGE));
  }
}

TEST_F(CompanionTransferTest, DictionaryRecoveryVerifiesRetainedArchiveAfterStageCleanup) {
  TransferDeclaration declaration;
  declaration.state = initial;
  declaration.manifest = {initial.contentHash, ContentKind::Dictionary, initial.length, 1, {}};
  Transfer transfer(storage, workspace);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(transfer.begin(declaration, "/dictionaries/test/dictionary"), TransferResult::Ok);
  ASSERT_EQ(transfer.append(initial.transaction, initial.owner, 0, storage.expected), TransferResult::Ok);
  storage.enableDictionaryPublication = true;
  ASSERT_EQ(transfer.commit(initial.transaction, initial.owner), TransferResult::Ok);
  storage.files["/archive-cache"] = storage.files.at(TRANSFER_STAGE);
  storage.dictionaryArchiveCached = true;
  storage.files.erase(TRANSFER_STAGE);
  Transfer reboot(storage, workspace);
  ASSERT_EQ(reboot.recover(generation, TransferRecoveryMode::DeferDictionaryInstallation), TransferResult::Ok);
  EXPECT_EQ(reboot.current()->phase, TransferPhase::Committed);
  ASSERT_EQ(reboot.recover(generation), TransferResult::Ok);
  EXPECT_EQ(storage.dictionaryPublicationCalls, 1u);
  storage.files.at("/archive-cache")[0] ^= 1;
  EXPECT_EQ(reboot.recover(generation, TransferRecoveryMode::DeferDictionaryInstallation),
            TransferResult::HashMismatch);
  EXPECT_TRUE(reboot.destination().empty());
}
