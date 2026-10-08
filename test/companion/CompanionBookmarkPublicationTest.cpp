#include <gtest/gtest.h>

#include "lib/Companion/CompanionBookmarkPublicationRecord.h"

using namespace companion;
namespace {
BookmarkPublicationClaim claim(bool original = true) {
  BookmarkPublicationClaim result;
  result.transaction.fill(1);
  result.storageGeneration.fill(2);
  result.edition.fill(3);
  result.frontier.fill(4);
  result.candidateHash.fill(5);
  result.candidateLength = 100;
  result.recordCount = 3;
  result.recordSize = 1024;
  result.hadOriginal = original;
  if (original) {
    result.originalHash.fill(6);
    result.originalLength = 80;
  }
  return result;
}
class Store final : public BookmarkPublicationStorage {
 public:
  explicit Store(bool original = true)
      : files{original ? BookmarkPublicationFile::Original : BookmarkPublicationFile::Missing,
              BookmarkPublicationFile::Candidate, BookmarkPublicationFile::Missing} {}
  bool validateContext(const BookmarkPublicationClaim&) override { return context; }
  bool validateAuthority(const BookmarkPublicationClaim&) override { return authority; }
  BookmarkPublicationRecord intent(const BookmarkPublicationClaim&) override { return pending; }
  BookmarkPublicationRecord receipt(const BookmarkPublicationClaim&) override { return committed; }
  BookmarkPublicationFile file(BookmarkPublicationRole role, const BookmarkPublicationClaim&) override {
    return files[static_cast<unsigned>(role)];
  }
  bool persistIntent(const BookmarkPublicationClaim&) override {
    return mutate([&] { pending = BookmarkPublicationRecord::Matches; });
  }
  bool move(BookmarkPublicationRole from, BookmarkPublicationRole to) override {
    const auto source = static_cast<unsigned>(from), target = static_cast<unsigned>(to);
    if (files[target] != BookmarkPublicationFile::Missing || files[source] == BookmarkPublicationFile::Missing)
      return false;
    return mutate([&] {
      files[target] = files[source];
      files[source] = BookmarkPublicationFile::Missing;
    });
  }
  bool remove(BookmarkPublicationRole role) override {
    if (role == BookmarkPublicationRole::Backup && committed != BookmarkPublicationRecord::Matches)
      removedBackupBeforeCommit = true;
    return mutate([&] { files[static_cast<unsigned>(role)] = BookmarkPublicationFile::Missing; });
  }
  bool commitReceipt(const BookmarkPublicationClaim&) override {
    return mutate([&] { committed = BookmarkPublicationRecord::Matches; });
  }
  bool clearIntent() override {
    return mutate([&] { pending = BookmarkPublicationRecord::Missing; });
  }
  std::array<BookmarkPublicationFile, 3> files;
  BookmarkPublicationRecord pending = BookmarkPublicationRecord::Missing,
                            committed = BookmarkPublicationRecord::Missing;
  unsigned operations = 0, failAt = 0, changeContextAt = 0;
  bool after = false, context = true, authority = true, removedBackupBeforeCommit = false;

 private:
  template <class Action>
  bool mutate(Action action) {
    ++operations;
    if (operations == failAt && !after) return false;
    action();
    if (operations == changeContextAt) context = false;
    return operations != failAt || !after;
  }
};
}  // namespace

TEST(BookmarkPublication, MutationCutsAndLostAcknowledgementsRetainBackupUntilDurableCommit) {
  for (const bool original : {false, true})
    for (const bool after : {false, true})
      for (unsigned cut = 1; cut <= 8; ++cut) {
        const auto record = claim(original);
        Store store(original);
        store.failAt = cut;
        store.after = after;
        BookmarkPublication publication(store);
        const auto first = publication.publish(record);
        EXPECT_TRUE(first == TintaJournalResult::Ok || first == TintaJournalResult::IoError);
        EXPECT_FALSE(store.removedBackupBeforeCommit);
        store.failAt = 0;
        BookmarkPublication restarted(store);
        if (store.pending == BookmarkPublicationRecord::Missing &&
            store.committed == BookmarkPublicationRecord::Missing) {
          EXPECT_EQ(restarted.recover(record), TintaJournalResult::Unavailable);
          EXPECT_EQ(restarted.publish(record), TintaJournalResult::Ok);
        } else {
          EXPECT_EQ(restarted.recover(record), TintaJournalResult::Ok);
        }
        EXPECT_EQ(store.files[0], BookmarkPublicationFile::Candidate);
        EXPECT_EQ(store.files[1], BookmarkPublicationFile::Missing);
        EXPECT_EQ(store.files[2], BookmarkPublicationFile::Missing);
        EXPECT_EQ(store.pending, BookmarkPublicationRecord::Missing);
        EXPECT_EQ(store.committed, BookmarkPublicationRecord::Matches);
        EXPECT_FALSE(store.removedBackupBeforeCommit);
        const auto operations = store.operations;
        EXPECT_EQ(restarted.publish(record), TintaJournalResult::Ok);
        EXPECT_EQ(restarted.recover(record), TintaJournalResult::Ok);
        EXPECT_EQ(store.operations, operations);
      }
}

TEST(BookmarkPublication, InvalidStaleAndForeignStatesCannotStartMutation) {
  for (unsigned mode = 0; mode < 7; ++mode) {
    auto record = claim();
    Store store;
    if (mode == 0) record.transaction = {};
    if (mode == 1) store.context = false;
    if (mode == 2) store.authority = false;
    if (mode == 3) store.pending = BookmarkPublicationRecord::Other;
    if (mode == 4) store.files[1] = BookmarkPublicationFile::Other;
    if (mode == 5) store.files[2] = BookmarkPublicationFile::Other;
    if (mode == 6) store.files[0] = BookmarkPublicationFile::Error;
    const auto original = store.files;
    BookmarkPublication publication(store);
    const auto expected = mode < 2    ? TintaJournalResult::Invalid
                          : mode == 2 ? TintaJournalResult::Conflict
                          : mode == 6 ? TintaJournalResult::IoError
                                      : TintaJournalResult::Corrupt;
    EXPECT_EQ(publication.publish(record), expected);
    EXPECT_EQ(store.operations, 0u);
    EXPECT_EQ(store.files, original);
  }
}

TEST(BookmarkPublication, ContextChangeAfterBackupStopsBeforeInstallingIntoWrongStorage) {
  const auto record = claim();
  Store store;
  store.changeContextAt = 2;
  BookmarkPublication publication(store);
  EXPECT_EQ(publication.publish(record), TintaJournalResult::Invalid);
  EXPECT_EQ(store.operations, 2u);
  EXPECT_EQ(store.files[0], BookmarkPublicationFile::Missing);
  EXPECT_EQ(store.files[1], BookmarkPublicationFile::Candidate);
  EXPECT_EQ(store.files[2], BookmarkPublicationFile::Original);
  store.context = true;
  store.authority = false;
  EXPECT_EQ(publication.recover(record), TintaJournalResult::Ok);
  EXPECT_FALSE(store.removedBackupBeforeCommit);
}

TEST(BookmarkPublication, IdenticalOriginalBytesCommitWithoutRenamesAndDuplicateWork) {
  auto record = claim();
  record.originalLength = record.candidateLength;
  record.originalHash = record.candidateHash;
  Store store;
  store.files[0] = BookmarkPublicationFile::Candidate;
  BookmarkPublication publication(store);
  EXPECT_EQ(publication.publish(record), TintaJournalResult::Ok);
  EXPECT_EQ(store.operations, 4u);
  EXPECT_EQ(store.files[0], BookmarkPublicationFile::Candidate);
  const auto operations = store.operations;
  EXPECT_EQ(publication.publish(record), TintaJournalResult::Ok);
  EXPECT_EQ(store.operations, operations);
}

TEST(BookmarkPublication, CommittedReceiptCannotAuthorizeRemovingForeignBackup) {
  const auto record = claim();
  Store store;
  store.pending = store.committed = BookmarkPublicationRecord::Matches;
  store.files = {BookmarkPublicationFile::Candidate, BookmarkPublicationFile::Missing, BookmarkPublicationFile::Other};
  BookmarkPublication publication(store);
  EXPECT_EQ(publication.recover(record), TintaJournalResult::Corrupt);
  EXPECT_EQ(store.operations, 0u);
  EXPECT_EQ(store.pending, BookmarkPublicationRecord::Matches);
  EXPECT_EQ(store.files[2], BookmarkPublicationFile::Other);
}

TEST(BookmarkPublication, ClaimRequiresBoundedJournalAndUnambiguousOriginalDescriptor) {
  auto record = claim();
  EXPECT_TRUE(validBookmarkPublicationClaim(record));
  record.recordSize = 256;
  EXPECT_FALSE(validBookmarkPublicationClaim(record));
  record = claim();
  record.recordCount = UINT32_MAX;
  EXPECT_FALSE(validBookmarkPublicationClaim(record));
  record = claim(false);
  EXPECT_TRUE(validBookmarkPublicationClaim(record));
  record.originalLength = 1;
  EXPECT_FALSE(validBookmarkPublicationClaim(record));
  record = claim(false);
  record.originalHash[0] = 1;
  EXPECT_FALSE(validBookmarkPublicationClaim(record));
}

TEST(BookmarkPublication, RecordRoundtripAndCorruptionPreserveOutput) {
  std::array<uint8_t, BOOKMARK_PUBLICATION_RECORD_SIZE> bytes{};
  for (const bool original : {false, true}) {
    const auto expected = claim(original);
    ASSERT_TRUE(encodeBookmarkPublicationRecord(expected, bytes));
    BookmarkPublicationClaim decoded;
    ASSERT_TRUE(decodeBookmarkPublicationRecord(bytes, decoded));
    EXPECT_EQ(decoded, expected);
    for (size_t i = 0; i < bytes.size(); ++i) {
      bytes[i] ^= 1;
      auto output = expected;
      EXPECT_FALSE(decodeBookmarkPublicationRecord(bytes, output));
      EXPECT_EQ(output, expected);
      bytes[i] ^= 1;
    }
    EXPECT_FALSE(decodeBookmarkPublicationRecord(std::span(bytes).first(bytes.size() - 1), decoded));
  }
}
TEST(BookmarkPublication, RecordRejectsInvalidFieldsWithValidChecksum) {
  std::array<uint8_t, BOOKMARK_PUBLICATION_RECORD_SIZE> bytes{};
  const auto expected = claim();
  for (const size_t offset : {size_t(4), size_t(5), size_t(6), size_t(190)}) {
    ASSERT_TRUE(encodeBookmarkPublicationRecord(expected, bytes));
    bytes[offset] = 2;
    bookmark_record_detail::put(std::span(bytes).last(4),
                                bookmark_record_detail::checksum(std::span(bytes).first(192)));
    auto output = expected;
    EXPECT_FALSE(decodeBookmarkPublicationRecord(bytes, output));
    EXPECT_EQ(output, expected);
  }
  auto invalid = expected;
  invalid.recordSize = 0;
  bytes.fill(0xa5);
  const auto untouched = bytes;
  EXPECT_FALSE(encodeBookmarkPublicationRecord(invalid, bytes));
  EXPECT_EQ(bytes, untouched);
}
