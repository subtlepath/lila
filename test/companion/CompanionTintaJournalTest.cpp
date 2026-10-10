#include <gtest/gtest.h>
#include <openssl/sha.h>

#include <array>
#include <fstream>
#include <iterator>
#include <vector>

#include "../../lib/Companion/CompanionBookmarkIdentityCursor.h"
#include "../../lib/Companion/CompanionBookmarkResolution.h"
#include "../../lib/Companion/CompanionPortablePreferenceResolution.h"
#include "../../lib/Companion/CompanionReaderPreferenceApplication.h"
#include "../../lib/Companion/CompanionReaderPreferenceCapture.h"
#include "../../lib/Companion/CompanionReaderPreferenceChangeCapture.h"
#include "../../lib/Companion/CompanionReaderPreferenceEncoding.h"
#include "../../lib/Companion/CompanionReaderPreferencePlan.h"
#include "../../lib/Companion/CompanionReadingPositionResolution.h"
#include "../../src/util/BookmarkIdentity.h"
#include "../tinta/fakes.h"
#include "core/session/SessionFile.h"
#include "lib/Companion/CompanionJournalCausalRelation.h"
#include "lib/Companion/CompanionJournalCausalValidation.h"
#include "lib/Companion/CompanionJournalCourseMembership.h"
#include "lib/Companion/CompanionJournalExportPage.h"
#include "lib/Companion/CompanionJournalIdentityIndex.h"
#include "lib/Companion/CompanionJournalIdentityIndexBuilder.h"
#include "lib/Companion/CompanionJournalIdentitySorter.h"
#include "lib/Companion/CompanionJournalIncomingCourseValidation.h"
#include "lib/Companion/CompanionJournalKnowledgeHeads.h"
#include "lib/Companion/CompanionJournalMergeIntent.h"
#include "lib/Companion/CompanionJournalMergePublication.h"
#include "lib/Companion/CompanionJournalMergeRequest.h"
#include "lib/Companion/CompanionJournalMigration.h"
#include "lib/Companion/CompanionJournalMigrationIntent.h"
#include "lib/Companion/CompanionJournalMigrationPublication.h"
#include "lib/Companion/CompanionJournalReplayOrder.h"
#include "lib/Companion/CompanionJournalTintaUndoValidation.h"
#include "lib/Companion/CompanionLegacyBackupReply.h"
#include "lib/Companion/CompanionLegacyBackupRequest.h"
#include "lib/Companion/CompanionLegacyTintaBackupManifest.h"
#include "lib/Companion/CompanionLegacyTintaBackupPaths.h"
#include "lib/Companion/CompanionLegacyTintaEventConversion.h"
#include "lib/Companion/CompanionLegacyTintaEventCursor.h"
#include "lib/Companion/CompanionLegacyTintaJournal.h"
#include "lib/Companion/CompanionLegacyTintaMutation.h"
#include "lib/Companion/CompanionLegacyTintaReplay.h"
#include "lib/Companion/CompanionTintaApplicationReceipt.h"
#include "lib/Companion/CompanionTintaAuthorityCheckpoint.h"
#include "lib/Companion/CompanionTintaItemReplay.h"
#include "lib/Companion/CompanionTintaJournal.h"
#include "lib/Companion/CompanionTintaJournalFrontier.h"
#include "lib/Companion/CompanionTintaLessonJournal.h"
#include "lib/Companion/CompanionTintaMarkJournal.h"
#include "lib/Companion/CompanionTintaMigrationAdmission.h"
#include "lib/Companion/CompanionTintaNativeMarkSnapshot.h"
#include "lib/Companion/CompanionTintaPreferenceCapture.h"
#include "lib/Companion/CompanionTintaPreferenceResolution.h"
#include "lib/Companion/CompanionTintaPreferences.h"
#include "lib/Companion/CompanionTintaProfileConfiguration.h"
#include "lib/Companion/CompanionTintaProgressJournal.h"
#include "lib/Companion/CompanionTintaReplayReducer.h"
#include "lib/Companion/CompanionTintaTimestamp.h"
#include "lib/Companion/CompanionTintaWriter.h"
#include "lib/Companion/CompanionUnboundCourseReviewEpochUse.h"

using namespace companion;

TEST(CompanionBookmarkIdentity, CanonicalRoundTripAndInvalidInputsPreserveOutputs) {
  BookmarkIdentity::Value identity{};
  for (size_t i = 0; i < identity.size(); ++i) identity[i] = static_cast<uint8_t>(i * 17);
  std::array<char, 33> encoded{};
  ASSERT_TRUE(BookmarkIdentity::encode(identity, encoded));
  EXPECT_STREQ(encoded.data(), "00112233445566778899aabbccddeeff");
  BookmarkIdentity::Value decoded{};
  ASSERT_TRUE(BookmarkIdentity::decode(encoded.data(), decoded));
  EXPECT_EQ(decoded, identity);
  const auto unchanged = decoded;
  for (const auto text : {"", "00112233445566778899AABBCCDDEEFF", "00000000000000000000000000000000",
                          "00112233445566778899aabbccddeefg", "00112233445566778899aabbccddeeff0"}) {
    EXPECT_FALSE(BookmarkIdentity::decode(text, decoded));
    EXPECT_EQ(decoded, unchanged);
  }
  const auto before = encoded;
  EXPECT_FALSE(BookmarkIdentity::encode(identity, std::span(encoded).first(32)));
  EXPECT_EQ(encoded, before);
  EXPECT_FALSE(BookmarkIdentity::encode({}, encoded));
  EXPECT_EQ(encoded, before);
}

TEST(CompanionBookmarkEncoding, BoundedPutAndDeleteRoundTripAndInvalidInputPreservesBuffer) {
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> output;
  output.fill(0xa5);
  const auto untouched = output;
  std::array<uint8_t, 128> name;
  std::array<uint8_t, 512> summary;
  name.fill('n');
  summary.fill('s');
  BookmarkBodyView bookmark;
  bookmark.anchor = {65535, UINT32_MAX};
  bookmark.name = name;
  bookmark.summary = summary;
  EXPECT_EQ(encodeBookmarkBody(bookmark, output), 0u);
  EXPECT_EQ(output, untouched);
  bookmark.identity[15] = 9;
  EXPECT_EQ(encodeBookmarkBody(bookmark, std::span(output).first(output.size() - 1)), 0u);
  EXPECT_EQ(output, untouched);
  name[0] = 0;
  EXPECT_EQ(encodeBookmarkBody(bookmark, output), 0u);
  EXPECT_EQ(output, untouched);
  name[0] = 0xff;
  EXPECT_EQ(encodeBookmarkBody(bookmark, output), 0u);
  EXPECT_EQ(output, untouched);
  name[0] = 'n';
  ASSERT_EQ(encodeBookmarkBody(bookmark, output), output.size());
  BookmarkBodyView decoded;
  ASSERT_TRUE(decodeBookmarkBody(output, decoded));
  EXPECT_EQ(decoded.identity, bookmark.identity);
  EXPECT_EQ(decoded.anchor, bookmark.anchor);
  EXPECT_TRUE(std::equal(decoded.name.begin(), decoded.name.end(), name.begin(), name.end()));
  EXPECT_TRUE(std::equal(decoded.summary.begin(), decoded.summary.end(), summary.begin(), summary.end()));
  bookmark.deleted = true;
  ASSERT_EQ(encodeBookmarkBody(bookmark, output), 18u);
  ASSERT_TRUE(decodeBookmarkBody(std::span(output).first(18), decoded));
  EXPECT_TRUE(decoded.deleted);
  EXPECT_EQ(decoded.identity, bookmark.identity);
  EXPECT_TRUE(decoded.name.empty());
  EXPECT_TRUE(decoded.summary.empty());
}

TEST(CompanionTintaJournal, AppleSnapshotFixtureOpensInProductionProgressStoreAndSupportsReview) {
  std::ifstream input(TINTA_ITEM_SNAPSHOT_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.good());
  tinta_test::MemStore local;
  local.files["items.bin"] = {std::istreambuf_iterator<char>(input), {}};
  local.files["reviews.log"] = {};
  const auto catalog = tinta_test::FakeCatalog::vocab(1, 1);
  tinta::core::Fsrs fsrs;
  tinta::core::ProgressStore progress(local, catalog, fsrs);
  std::array<uint16_t, 2> slots{};
  ASSERT_EQ(progress.open(slots.data(), slots.size(), nullptr, 0), tinta::core::ProgressStore::OpenResult::Opened);
  EXPECT_EQ(progress.lastStudyDay(), 42);
  EXPECT_EQ(progress.newOn(42), 7);
  EXPECT_EQ(progress.reviewsOn(42), 11);
  tinta::core::ItemState state;
  for (uint32_t i = 0; i < 2; ++i) {
    ASSERT_TRUE(progress.load(i, state));
    EXPECT_EQ(state.uid, 1000 + i);
    EXPECT_TRUE(state.isNew());
  }
  const auto result = progress.review(0, tinta::core::Grade::Good, 0, 1234, 42, 0);
  EXPECT_EQ(result.status, tinta::core::ProgressStore::Status::Stored);
  EXPECT_EQ(progress.newOn(42), 8);
  ASSERT_TRUE(progress.load(0, state));
  EXPECT_FALSE(state.isNew());
}
namespace {
class Storage final : public TintaJournalStorage {
 public:
  std::vector<uint8_t> data;
  std::array<std::vector<uint8_t>, 2> headers;
  size_t partialRecord = SIZE_MAX;
  unsigned skipRecordCuts = 0;
  size_t partialHeader = SIZE_MAX;
  bool lostHeaderReply = false;
  bool refuseTruncate = false;
  int failedHeader = -1;
  size_t writes = 0;
  size_t reads = 0, failedReadAt = SIZE_MAX;
  Storage() {
    data.reserve(4096);
    for (auto& header : headers) header.reserve(64);
  }
  bool size(uint32_t& bytes) override {
    bytes = data.size();
    return true;
  }
  bool read(uint32_t offset, std::span<uint8_t> bytes) override {
    ++reads;
    if (reads == failedReadAt) return false;
    if (offset > data.size() || bytes.size() > data.size() - offset) return false;
    std::copy_n(data.begin() + offset, bytes.size(), bytes.begin());
    return true;
  }
  bool write(uint32_t offset, std::span<const uint8_t> bytes) override {
    ++writes;
    const bool cut = skipRecordCuts == 0;
    if (skipRecordCuts) --skipRecordCuts;
    const auto length = cut ? std::min(partialRecord, bytes.size()) : bytes.size();
    data.resize(offset + length);
    std::copy_n(bytes.begin(), length, data.begin() + offset);
    if (cut && partialRecord != SIZE_MAX) {
      partialRecord = SIZE_MAX;
      return false;
    }
    return true;
  }
  bool truncate(uint32_t bytes) override {
    if (refuseTruncate) return false;
    data.resize(bytes);
    return true;
  }
  bool readHeader(uint8_t slot, std::span<uint8_t> bytes, size_t& length) override {
    if (slot == failedHeader) return false;
    length = headers[slot].size();
    std::copy(headers[slot].begin(), headers[slot].end(), bytes.begin());
    return true;
  }
  bool writeHeader(uint8_t slot, std::span<const uint8_t> bytes) override {
    ++writes;
    const auto length = std::min(partialHeader, bytes.size());
    headers[slot].assign(bytes.begin(), bytes.begin() + length);
    if (partialHeader != SIZE_MAX) {
      partialHeader = SIZE_MAX;
      return false;
    }
    if (lostHeaderReply) {
      lostHeaderReply = false;
      return false;
    }
    return true;
  }
  bool digest(std::span<const uint8_t> bytes, Digest& output) override {
    return SHA256(bytes.data(), bytes.size(), output.data()) != nullptr;
  }
};
struct Fixture {
  Storage storage;
  std::array<uint8_t, 512> scratch{};
  std::array<uint8_t, 54> bytes{};
  SyncEvent event;
  size_t length = 0;
  TintaJournal journal{storage, scratch};
  Fixture() {
    TintaBody body;
    body.course.fill(7);
    body.uid = 1;
    body.grade = 3;
    body.responseMilliseconds = 1000;
    length = encodeTintaBody(body, bytes);
    event.identity.origin.fill(1);
    event.identity.epoch = 1;
    event.identity.sequence = 1;
    event.storageGeneration.fill(2);
    event.kind = EventKind::Review;
    event.resource.fill(3);
    event.schedulerVersion = 1;
    storage.digest(std::span(bytes).first(length), event.bodyHash);
    std::array<uint8_t, 6> configuration;
    encodeTintaConfiguration(body.configuration, configuration);
    storage.digest(configuration, event.schedulerConfiguration);
  }
  auto body() { return std::span(bytes).first(length); }
};
}  // namespace
namespace {
TEST(CompanionTintaJournal, ExportPagesBindCursorAndRetryUntilJournalChanges) {
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  JournalExportPage exporter(f.journal);
  Digest frontier{};
  frontier.fill(9);
  std::array<uint8_t, JOURNAL_EXPORT_REQUEST_SIZE> request{};
  std::array<uint8_t, MAX_CONTROL_PAYLOAD> output{};
  output.fill(0xcc);
  const auto unchanged = output;
  EXPECT_EQ(exporter.page(request, output), 0u);
  EXPECT_EQ(output, unchanged);
  ASSERT_TRUE(exporter.begin(frontier));
  const auto length = exporter.page(request, output);
  ASSERT_EQ(length, JOURNAL_EXPORT_HEADER_SIZE + SYNC_EVENT_BASE_SIZE + f.length);
  EXPECT_EQ(output[0], 1);
  EXPECT_EQ(output[1], 0);
  EXPECT_EQ(tinta_body_detail::read(output, 4, 4), 1u);
  EXPECT_EQ(tinta_body_detail::read(output, 8, 4), 1u);
  SyncEvent decoded;
  ASSERT_TRUE(decodeRecord(std::span(output).subspan(48, SYNC_EVENT_BASE_SIZE), decoded));
  EXPECT_EQ(decoded.identity, f.event.identity);
  EXPECT_TRUE(std::equal(f.body().begin(), f.body().end(), output.begin() + 48 + SYNC_EVENT_BASE_SIZE));
  const auto retry = output;
  EXPECT_EQ(exporter.page(request, output), length);
  EXPECT_EQ(output, retry);
  std::copy(frontier.begin(), frontier.end(), request.begin());
  tinta_body_detail::write(request, 32, 1, 4);
  tinta_body_detail::write(request, 36, 1, 4);
  request[0] ^= 1;
  EXPECT_EQ(exporter.page(request, output), 0u);
  EXPECT_EQ(output, retry);
  request[0] ^= 1;
  EXPECT_EQ(exporter.page(request, output), JOURNAL_EXPORT_HEADER_SIZE);
  EXPECT_EQ(output[1], 1);
  EXPECT_EQ(tinta_body_detail::read(output, 44, 4), 0u);
  f.event.identity.sequence = 2;
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  const auto beforeChange = output;
  EXPECT_EQ(exporter.page(request, output), 0u);
  EXPECT_EQ(output, beforeChange);
  EXPECT_EQ(exporter.page(request, output), 0u);
}

TEST(CompanionTintaJournal, ExportEmptyJournalAndReadFailurePreserveCursorSafety) {
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  JournalExportPage exporter(f.journal);
  Digest frontier{};
  EXPECT_FALSE(exporter.begin(frontier));
  frontier.fill(1);
  ASSERT_TRUE(exporter.begin(frontier));
  std::array<uint8_t, JOURNAL_EXPORT_REQUEST_SIZE> request{};
  std::array<uint8_t, MAX_CONTROL_PAYLOAD> output{};
  EXPECT_EQ(exporter.page(request, output), JOURNAL_EXPORT_HEADER_SIZE);
  EXPECT_EQ(output[1], 1);
  EXPECT_EQ(tinta_body_detail::read(output, 4, 8), 0u);
  const auto before = output;
  EXPECT_EQ(exporter.page(std::span(request).first(39), output), 0u);
  EXPECT_EQ(exporter.page(request, std::span(output).first(47)), 0u);
  EXPECT_EQ(output, before);
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  ASSERT_TRUE(exporter.begin(frontier));
  f.storage.data[20] ^= 1;
  EXPECT_EQ(exporter.page(request, output), 0u);
  EXPECT_EQ(output, before);
  f.storage.data[20] ^= 1;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  EXPECT_EQ(exporter.page(request, output), 0u);
  ASSERT_TRUE(exporter.begin(frontier));
  EXPECT_GT(exporter.page(request, output), JOURNAL_EXPORT_HEADER_SIZE);
  exporter.reset();
  EXPECT_EQ(exporter.page(request, output), 0u);
}

TEST(CompanionTintaJournal, ExportWireBytesMatchSharedAppleFixture) {
  Fixture f;
  TintaBody body;
  body.kind = EventKind::Star;
  body.course.fill(7);
  body.uid = 1;
  body.enabled = true;
  f.length = encodeTintaBody(body, f.bytes);
  f.event.kind = EventKind::Star;
  f.event.schedulerVersion = 0;
  f.event.schedulerConfiguration.fill(0);
  ASSERT_TRUE(f.storage.digest(f.body(), f.event.bodyHash));
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  Digest frontier{};
  frontier.fill(9);
  JournalExportPage exporter(f.journal);
  ASSERT_TRUE(exporter.begin(frontier));
  std::array<uint8_t, JOURNAL_EXPORT_REQUEST_SIZE> request{};
  std::array<uint8_t, MAX_CONTROL_PAYLOAD> output{};
  const auto length = exporter.page(request, output);
  std::ifstream input(JOURNAL_EXPORT_PAGE_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> expected{std::istreambuf_iterator<char>(input), {}};
  ASSERT_EQ(length, expected.size());
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), output.begin()));
}

class MergePublication final : public JournalMergePublicationStorage {
 public:
  JournalMergeIntent pending;
  std::array<std::optional<JournalMergeSnapshot>, 3> directories;
  bool pendingPresent = true, receipt = false, lostAck = false;
  unsigned operation = 0, failAt = 0, inspections = 0;
  MergePublication() {
    pending.generation.fill(1);
    pending.transaction.fill(2);
    pending.owner.fill(3);
    pending.previous = {1, 512, {}};
    pending.previous.frontier.fill(4);
    pending.merged = {3, 1024, {}};
    pending.merged.frontier.fill(5);
    directories[0] = pending.previous;
    directories[1] = pending.merged;
  }
  JournalMigrationPresence intent(JournalMergeIntent& output) override {
    output = pending;
    return pendingPresent ? JournalMigrationPresence::Present : JournalMigrationPresence::Missing;
  }
  JournalMigrationPresence directory(JournalMergeDirectory location) override {
    ++inspections;
    return directories[static_cast<size_t>(location)] ? JournalMigrationPresence::Present
                                                      : JournalMigrationPresence::Missing;
  }
  bool verify(JournalMergeDirectory location, const JournalMergeSnapshot& expected) override {
    return directories[static_cast<size_t>(location)] == expected;
  }
  bool move(JournalMergeDirectory from, JournalMergeDirectory to) override {
    ++operation;
    if (operation == failAt && !lostAck) return false;
    auto& source = directories[static_cast<size_t>(from)];
    auto& destination = directories[static_cast<size_t>(to)];
    if (!source || destination) return false;
    destination = source;
    source.reset();
    return operation != failAt;
  }
  bool complete(const JournalMergeIntent& expected) override {
    if (expected.transaction != pending.transaction || expected.owner != pending.owner) return false;
    ++operation;
    if (operation == failAt && !lostAck) return false;
    receipt = true;
    if (operation == failAt) return false;
    ++operation;
    if (operation == failAt && !lostAck) return false;
    pendingPresent = false;
    return operation != failAt;
  }
};

TEST(CompanionTintaJournal, MergePublicationRecoversRenameAndReceiptInterruptions) {
  for (unsigned failAt = 0; failAt <= 4; ++failAt) {
    for (const bool lostAck : {false, true}) {
      MergePublication storage;
      storage.failAt = failAt;
      storage.lostAck = lostAck;
      const auto first = recoverJournalMerge(storage, storage.pending.generation);
      EXPECT_EQ(first,
                failAt ? JournalMigrationPublicationResult::IoError : JournalMigrationPublicationResult::Complete);
      storage.failAt = 0;
      const auto retry = recoverJournalMerge(storage, storage.pending.generation);
      EXPECT_TRUE(retry == JournalMigrationPublicationResult::Complete ||
                  retry == JournalMigrationPublicationResult::NoPending);
      EXPECT_EQ(storage.directories[0], storage.pending.merged);
      EXPECT_EQ(storage.directories[2], storage.pending.previous);
      EXPECT_FALSE(storage.directories[1]);
      EXPECT_TRUE(storage.receipt);
      EXPECT_FALSE(storage.pendingPresent);
    }
  }
}

TEST(CompanionTintaJournal, MergeRequestsBindTransactionAndPreserveOutputOnMalformedPayloads) {
  MergePublication storage;
  std::array<uint8_t, JOURNAL_MERGE_INTENT_SIZE> declaration{};
  ASSERT_TRUE(encodeJournalMergeIntent(storage.pending, declaration));
  std::array<uint8_t, MAX_CONTROL_PAYLOAD> bytes{};
  for (auto operation : {JournalMergeOperation::Begin, JournalMergeOperation::Commit, JournalMergeOperation::Abort}) {
    JournalMergeRequestView request{operation, storage.pending.transaction, declaration, {}, {}};
    const auto length = encodeJournalMergeRequest(request, bytes);
    ASSERT_EQ(length, JOURNAL_MERGE_REQUEST_HEADER_SIZE + declaration.size());
    JournalMergeRequestView decoded;
    ASSERT_TRUE(decodeJournalMergeRequest(std::span(bytes).first(length), decoded));
    EXPECT_EQ(decoded.operation, operation);
    EXPECT_EQ(decoded.transaction, request.transaction);
    EXPECT_EQ(decoded.declaration.data(), bytes.data() + JOURNAL_MERGE_REQUEST_HEADER_SIZE);
    for (size_t offset : {size_t{0}, size_t{3}, size_t{5}, size_t{8}, size_t{24}, length - 1}) {
      bytes[offset] ^= 1;
      auto preserved = decoded;
      EXPECT_FALSE(decodeJournalMergeRequest(std::span(bytes).first(length), decoded));
      EXPECT_EQ(decoded.transaction, preserved.transaction);
      EXPECT_EQ(decoded.declaration.data(), preserved.declaration.data());
      bytes[offset] ^= 1;
    }
    const auto preserved = bytes;
    request.transaction[0] ^= 1;
    EXPECT_EQ(encodeJournalMergeRequest(request, bytes), 0u);
    EXPECT_EQ(bytes, preserved);
  }
}

TEST(CompanionTintaJournal, MergeAppendCarriesBorrowedEnvelopeAndBodyWithinControlLimit) {
  std::array<uint8_t, MAX_RECORD_SIZE> envelope{};
  std::array<uint8_t, TintaJournal::MAX_BODY_SIZE> body{};
  std::array<uint8_t, MAX_CONTROL_PAYLOAD> output{};
  JournalMergeRequestView request;
  request.operation = JournalMergeOperation::Append;
  request.transaction.fill(2);
  request.envelope = envelope;
  request.body = body;
  const auto length = encodeJournalMergeRequest(request, output);
  ASSERT_GT(length, 0u);
  EXPECT_LE(length, MAX_CONTROL_PAYLOAD);
  JournalMergeRequestView decoded;
  ASSERT_TRUE(decodeJournalMergeRequest(std::span(output).first(length), decoded));
  EXPECT_EQ(decoded.envelope.size(), envelope.size());
  EXPECT_EQ(decoded.body.size(), body.size());
  EXPECT_EQ(decoded.envelope.data(), output.data() + JOURNAL_MERGE_REQUEST_HEADER_SIZE);
  EXPECT_EQ(decoded.body.data(), output.data() + JOURNAL_MERGE_REQUEST_HEADER_SIZE + envelope.size());
  EXPECT_FALSE(decodeJournalMergeRequest(std::span(output).first(length - 1), decoded));
  request.body = {};
  const auto preserved = output;
  EXPECT_EQ(encodeJournalMergeRequest(request, output), 0u);
  EXPECT_EQ(output, preserved);
}

TEST(CompanionTintaJournal, MergeWireBytesMatchSharedAppleFixtures) {
  const auto fixture = [](const char* name) {
    std::string path = JOURNAL_EXPORT_PAGE_FIXTURE;
    path.resize(path.find_last_of('/') + 1);
    std::ifstream input(path + name, std::ios::binary);
    return std::vector<uint8_t>{std::istreambuf_iterator<char>(input), {}};
  };
  MergePublication storage;
  std::array<uint8_t, JOURNAL_MERGE_INTENT_SIZE> declaration{};
  ASSERT_TRUE(encodeJournalMergeIntent(storage.pending, declaration));
  JournalMergeRequestView request{JournalMergeOperation::Begin, storage.pending.transaction, declaration, {}, {}};
  std::array<uint8_t, MAX_CONTROL_PAYLOAD> output{};
  auto length = encodeJournalMergeRequest(request, output);
  auto expected = fixture("JournalMergeBegin-v1.fixture");
  ASSERT_EQ(length, expected.size());
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), output.begin()));
  const auto page = fixture("JournalExportPage-v1.fixture");
  ASSERT_GE(page.size(), JOURNAL_EXPORT_HEADER_SIZE);
  const auto envelope = tinta_body_detail::read(page, 44, 2);
  request = {JournalMergeOperation::Append,
             storage.pending.transaction,
             {},
             std::span(page).subspan(JOURNAL_EXPORT_HEADER_SIZE, envelope),
             std::span(page).subspan(JOURNAL_EXPORT_HEADER_SIZE + envelope)};
  length = encodeJournalMergeRequest(request, output);
  expected = fixture("JournalMergeAppend-v1.fixture");
  ASSERT_EQ(length, expected.size());
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), output.begin()));
}

TEST(CompanionTintaJournal, MergeIntentRoundTripsAndRejectsCorruptionWithoutChangingOutput) {
  MergePublication storage;
  std::array<uint8_t, JOURNAL_MERGE_INTENT_SIZE> bytes{};
  ASSERT_TRUE(encodeJournalMergeIntent(storage.pending, bytes));
  JournalMergeIntent decoded;
  ASSERT_TRUE(decodeJournalMergeIntent(bytes, decoded));
  EXPECT_EQ(decoded, storage.pending);
  const auto original = bytes;
  for (size_t at = 0; at < bytes.size(); ++at) {
    bytes[at] ^= 1;
    EXPECT_FALSE(decodeJournalMergeIntent(bytes, decoded));
    EXPECT_EQ(decoded, storage.pending);
    bytes = original;
  }
  EXPECT_FALSE(decodeJournalMergeIntent(std::span(bytes).first(bytes.size() - 1), decoded));
  storage.pending.merged.count = 0;
  EXPECT_FALSE(encodeJournalMergeIntent(storage.pending, bytes));
  EXPECT_EQ(bytes, original);
}

TEST(CompanionTintaJournal, MergePublicationRejectsChangedCardCorruptionAndAmbiguousDirectories) {
  MergePublication storage;
  Identity otherGeneration{};
  otherGeneration.fill(6);
  EXPECT_EQ(recoverJournalMerge(storage, otherGeneration), JournalMigrationPublicationResult::Conflict);
  EXPECT_EQ(storage.inspections, 0u);
  EXPECT_EQ(storage.operation, 0u);
  for (unsigned mask = 0; mask < 8; ++mask) {
    if (mask == 3 || mask == 5 || mask == 6) continue;
    MergePublication invalid;
    for (unsigned at = 0; at < 3; ++at) {
      invalid.directories[at].reset();
      if (mask & (1u << at)) invalid.directories[at] = at == 1 ? invalid.pending.merged : invalid.pending.previous;
    }
    EXPECT_EQ(recoverJournalMerge(invalid, invalid.pending.generation), JournalMigrationPublicationResult::Conflict);
    EXPECT_EQ(invalid.operation, 0u);
  }
  storage.directories[1]->frontier[0] ^= 1;
  EXPECT_EQ(recoverJournalMerge(storage, storage.pending.generation), JournalMigrationPublicationResult::Conflict);
  EXPECT_EQ(storage.operation, 0u);
  storage.pending.merged.count = storage.pending.previous.count;
  EXPECT_FALSE(validJournalMergeIntent(storage.pending));
}

TEST(CompanionTintaJournal, BoundedAppendRetriesAtCapacityWithoutGrowingCandidate) {
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.appendBounded(f.event, f.body(), 2), TintaJournalResult::Ok);
  f.event.identity.sequence = 2;
  ASSERT_EQ(f.journal.appendBounded(f.event, f.body(), 2), TintaJournalResult::Ok);
  const auto before = f.storage.data;
  const auto headers = f.storage.headers;
  const auto writes = f.storage.writes;
  EXPECT_EQ(f.journal.appendBounded(f.event, f.body(), 2), TintaJournalResult::Duplicate);
  EXPECT_EQ(f.storage.writes, writes);
  f.event.identity.sequence = 3;
  EXPECT_EQ(f.journal.appendBounded(f.event, f.body(), 2), TintaJournalResult::Exhausted);
  EXPECT_EQ(f.journal.count(), 2u);
  EXPECT_EQ(f.storage.data, before);
  EXPECT_EQ(f.storage.headers, headers);
  EXPECT_EQ(f.journal.appendBounded(f.event, f.body(), 1), TintaJournalResult::Conflict);
  EXPECT_EQ(f.journal.appendBounded(f.event, f.body(), UINT32_MAX), TintaJournalResult::Invalid);
}

TEST(CompanionTintaJournal, BoundedAppendRecoversLostFinalHeaderAcknowledgmentAndRejectsEquivocation) {
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.appendBounded(f.event, f.body(), 2), TintaJournalResult::Ok);
  f.event.identity.sequence = 2;
  f.storage.lostHeaderReply = true;
  EXPECT_EQ(f.journal.appendBounded(f.event, f.body(), 2), TintaJournalResult::IoError);
  EXPECT_EQ(f.journal.appendBounded(f.event, f.body(), 2), TintaJournalResult::Unavailable);
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.count(), 2u);
  EXPECT_EQ(f.journal.appendBounded(f.event, f.body(), 2), TintaJournalResult::Duplicate);
  const auto before = f.storage.data;
  f.bytes[22] = 4;
  ASSERT_TRUE(f.storage.digest(f.body(), f.event.bodyHash));
  EXPECT_EQ(f.journal.appendBounded(f.event, f.body(), 2), TintaJournalResult::Conflict);
  EXPECT_EQ(f.storage.data, before);
  EXPECT_EQ(f.journal.count(), 2u);
}

TEST(CompanionTintaJournal, IncomingCourseValidationChecksOnlyNewTintaSubjectsAndAllowsReadingOnlyMerges) {
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  const std::array<uint8_t, 8> anchor{1, 1, 0, 0, 0, 0, 0, 0};
  SyncEvent reading = f.event;
  reading.identity.sequence = 2;
  reading.kind = EventKind::ReadingPosition;
  reading.schedulerVersion = 0;
  reading.schedulerConfiguration.fill(0);
  ASSERT_TRUE(f.storage.digest(anchor, reading.bodyHash));
  ASSERT_EQ(f.journal.append(reading, anchor), TintaJournalResult::Ok);
  JournalIncomingCourseValidation validation;
  EXPECT_EQ(validation.validate(f.journal, 1, nullptr, nullptr), TintaJournalResult::Ok);
  EXPECT_EQ(validation.validate(f.journal, 3, nullptr, nullptr), TintaJournalResult::Conflict);
  class Catalog final : public TintaSubjectCatalog {
   public:
    unsigned calls = 0;
    TintaSubjectMembership result = TintaSubjectMembership::Present;
    TintaSubjectMembership contains(EventKind kind, uint32_t uid) override {
      ++calls;
      EXPECT_EQ(kind, EventKind::Review);
      EXPECT_EQ(uid, 1u);
      return result;
    }
  } catalog;
  Identity course{};
  course.fill(8);
  EXPECT_EQ(validation.validate(f.journal, 1, &course, &catalog), TintaJournalResult::Ok);
  EXPECT_EQ(catalog.calls, 0u);
  EXPECT_EQ(validation.validate(f.journal, 1, nullptr, &catalog), TintaJournalResult::Invalid);
  f.event.identity.sequence = 3;
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  EXPECT_EQ(validation.validate(f.journal, 1, nullptr, nullptr), TintaJournalResult::Invalid);
  EXPECT_EQ(validation.validate(f.journal, 1, &course, &catalog), TintaJournalResult::Invalid);
  EXPECT_EQ(catalog.calls, 0u);
  course.fill(7);
  catalog.result = TintaSubjectMembership::Missing;
  EXPECT_EQ(validation.validate(f.journal, 1, &course, &catalog), TintaJournalResult::Invalid);
  catalog.result = TintaSubjectMembership::IoError;
  EXPECT_EQ(validation.validate(f.journal, 1, &course, &catalog), TintaJournalResult::IoError);
  catalog.result = TintaSubjectMembership::Present;
  EXPECT_EQ(validation.validate(f.journal, 1, &course, &catalog), TintaJournalResult::Ok);
}

class JournalSortRuns final : public JournalIdentitySortStorage {
 public:
  std::array<std::vector<uint8_t>, 2> files;
  unsigned operations = 0, failAt = 0;
  JournalSortRuns() {
    for (auto& file : files) file.reserve(4096);
  }
  bool operation() { return ++operations != failAt; }
  bool reset() override {
    if (!operation()) return false;
    for (auto& file : files) file.clear();
    return true;
  }
  bool read(unsigned run, uint64_t at, std::span<uint8_t> bytes) override {
    if (!operation() || run > 1 || at > files[run].size() || bytes.size() > files[run].size() - at) return false;
    std::copy_n(files[run].begin() + at, bytes.size(), bytes.begin());
    return true;
  }
  bool write(unsigned run, uint64_t at, std::span<const uint8_t> bytes) override {
    if (!operation() || run > 1 || at > files[run].size()) return false;
    files[run].resize(std::max(files[run].size(), size_t(at + bytes.size())));
    std::copy(bytes.begin(), bytes.end(), files[run].begin() + at);
    return true;
  }
  bool finish(unsigned run, uint64_t size) override {
    if (!operation() || run > 1 || size > files[run].size()) return false;
    files[run].resize(size);
    return true;
  }
};
}  // namespace
TEST(CompanionTintaJournal, MembershipUsesSelectedCourseAndDistinctSubjectNamespaces) {
  class Catalog final : public TintaSubjectCatalog {
   public:
    bool failed = false;
    TintaSubjectMembership contains(EventKind kind, uint32_t uid) override {
      if (failed) return TintaSubjectMembership::IoError;
      const uint32_t expected = kind == EventKind::LessonComplete ? 3 : kind == EventKind::ReadingComplete ? 4 : 1;
      return uid == expected ? TintaSubjectMembership::Present : TintaSubjectMembership::Missing;
    }
  } catalog;
  for (const auto kind : {EventKind::Review, EventKind::Star, EventKind::LessonComplete, EventKind::ReadingComplete}) {
    Fixture f;
    ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
    TintaBody body;
    ASSERT_TRUE(decodeTintaBody(f.body(), body));
    body.kind = kind;
    f.length = encodeTintaBody(body, f.bytes);
    f.event.kind = kind;
    if (kind != EventKind::Review) {
      f.event.schedulerVersion = 0;
      f.event.schedulerConfiguration.fill(0);
    }
    ASSERT_TRUE(f.storage.digest(f.body(), f.event.bodyHash));
    ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
    Identity course{};
    course.fill(7);
    JournalCourseMembershipValidation check;
    EXPECT_EQ(check.validate(f.journal, course, catalog), kind == EventKind::Review || kind == EventKind::Star
                                                              ? TintaJournalResult::Ok
                                                              : TintaJournalResult::Invalid);
    course.fill(8);
    EXPECT_EQ(check.validate(f.journal, course, catalog), TintaJournalResult::Ok);
    course.fill(7);
    catalog.failed = true;
    EXPECT_EQ(check.validate(f.journal, course, catalog), TintaJournalResult::IoError);
    catalog.failed = false;
  }
}
TEST(CompanionTintaJournal, IndexedUndoValidationRequiresReviewOfSameCourseAndItem) {
  class Index final : public JournalIdentityIndex {
   public:
    EventIdentity target{};
    JournalIdentityLookup find(const EventIdentity& identity, uint32_t& record) override {
      if (identity != target) return JournalIdentityLookup::Missing;
      record = 0;
      return JournalIdentityLookup::Found;
    }
  };
  for (unsigned mode = 0; mode < 4; ++mode) {
    Fixture f;
    ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
    if (mode == 3) {
      TintaBody star;
      ASSERT_TRUE(decodeTintaBody(f.body(), star));
      star.kind = EventKind::Star;
      star.enabled = true;
      f.length = encodeTintaBody(star, f.bytes);
      f.event.kind = EventKind::Star;
      f.event.schedulerVersion = 0;
      f.event.schedulerConfiguration.fill(0);
      ASSERT_TRUE(f.storage.digest(f.body(), f.event.bodyHash));
    }
    ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
    TintaBody undo;
    undo.kind = EventKind::UndoReview;
    undo.course.fill(mode == 2 ? 8 : 7);
    undo.uid = mode == 1 ? 2 : 1;
    undo.undoTarget = f.event.identity;
    std::array<uint8_t, MAX_TINTA_BODY_SIZE> bytes{};
    const auto length = encodeTintaBody(undo, bytes);
    auto event = f.event;
    event.kind = EventKind::UndoReview;
    event.identity.sequence = 2;
    event.ancestorCount = 1;
    event.ancestors[0] = undo.undoTarget;
    event.schedulerVersion = 0;
    event.schedulerConfiguration.fill(0);
    ASSERT_TRUE(f.storage.digest(std::span(bytes).first(length), event.bodyHash));
    ASSERT_EQ(f.journal.append(event, std::span(bytes).first(length)), TintaJournalResult::Ok);
    Index index;
    index.target = undo.undoTarget;
    JournalTintaUndoValidation audit;
    EXPECT_EQ(audit.validate(f.journal, index), mode == 0 ? TintaJournalResult::Ok : TintaJournalResult::Corrupt);
  }
}
TEST(CompanionTintaJournal, ExternalIdentitySortPreservesPositionsAcrossTinyAndLargerBanks) {
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  constexpr std::array<uint64_t, 19> epochs{256, 255, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 2, 1};
  for (const auto epoch : epochs) {
    f.event.identity.epoch = epoch;
    ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  }
  for (const size_t size : {size_t{120}, size_t{121}, size_t{511}}) {
    std::array<uint8_t, 511> scratch{};
    JournalSortRuns runs;
    CommittedJournalIdentitySource source(f.journal);
    JournalIdentitySorter sorter(runs, std::span(scratch).first(size));
    ASSERT_TRUE(sorter.build(source));
    JournalIdentityEntry previous{}, entry{};
    for (unsigned at = 0; at < epochs.size(); ++at) {
      ASSERT_EQ(sorter.next(entry), JournalIdentitySourceResult::Entry);
      ASSERT_LT(entry.record, epochs.size());
      EXPECT_EQ(entry.identity.epoch, epochs[entry.record]);
      if (at) {
        EXPECT_TRUE(journalIdentityBefore(previous.identity, entry.identity));
      }
      previous = entry;
    }
    EXPECT_EQ(sorter.next(entry), JournalIdentitySourceResult::End);
    const auto operations = runs.operations;
    for (unsigned fail = 1; fail <= operations; ++fail) {
      JournalSortRuns broken;
      broken.failAt = fail;
      CommittedJournalIdentitySource retry(f.journal);
      JournalIdentitySorter attempt(broken, std::span(scratch).first(size));
      const bool built = attempt.build(retry);
      if (built) {
        JournalIdentitySourceResult result;
        do {
          result = attempt.next(entry);
        } while (result == JournalIdentitySourceResult::Entry);
        EXPECT_EQ(result, JournalIdentitySourceResult::Error);
      } else
        EXPECT_EQ(attempt.next(entry), JournalIdentitySourceResult::Error);
    }
  }
}
TEST(CompanionTintaJournal, ComposedSortIndexBuildAndCausalAuditPreservePriorIndexOnFailure) {
  class IndexFile final : public JournalIdentityIndexSink, public JournalIdentityIndexStorage {
   public:
    std::vector<uint8_t> staged, published;
    unsigned operations = 0, failAt = 0;
    IndexFile() {
      staged.reserve(4096);
      published.reserve(4096);
    }
    bool operation() { return ++operations != failAt; }
    bool begin() override {
      staged.clear();
      return operation();
    }
    bool write(uint64_t at, std::span<const uint8_t> bytes) override {
      if (!operation() || at > staged.size()) return false;
      staged.resize(std::max(staged.size(), size_t(at + bytes.size())));
      std::copy(bytes.begin(), bytes.end(), staged.begin() + at);
      return true;
    }
    bool finish(uint64_t bytes) override {
      if (!operation() || bytes != staged.size()) return false;
      published = staged;
      return true;
    }
    void abort() override { staged.clear(); }
    bool size(uint64_t& bytes) override {
      bytes = published.size();
      return true;
    }
    bool read(uint64_t at, std::span<uint8_t> bytes) override {
      if (at > published.size() || bytes.size() > published.size() - at) return false;
      std::copy_n(published.begin() + at, bytes.size(), bytes.begin());
      return true;
    }
  } file;
  Fixture f;
  f.event.identity.origin.fill(2);
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  auto second = f.event;
  second.identity.origin.fill(1);
  second.ancestorCount = 1;
  second.ancestors[0] = f.event.identity;
  ASSERT_EQ(f.journal.append(second, f.body()), TintaJournalResult::Ok);
  JournalSortRuns runs;
  std::array<uint8_t, 120> sortScratch{};
  std::array<uint8_t, 40> indexScratch{};
  JournalIdentitySorter sorter(runs, sortScratch);
  JournalIdentityIndexBuilder builder(file, indexScratch);
  auto build = [&] {
    CommittedJournalIdentitySource source(f.journal);
    return sorter.build(source) && builder.build(sorter, f.journal.count());
  };
  ASSERT_TRUE(build());
  IndexedJournalIdentities index(file, indexScratch);
  ASSERT_TRUE(index.open(f.journal.count()));
  JournalCausalValidation audit;
  EXPECT_EQ(audit.validate(f.journal, index), TintaJournalResult::Ok);
  const auto prior = file.published;
  const auto operations = file.operations;
  for (unsigned fail = 1; fail <= operations; ++fail) {
    file.operations = 0;
    file.failAt = fail;
    EXPECT_FALSE(build());
    EXPECT_EQ(file.published, prior);
    EXPECT_TRUE(file.staged.empty());
  }
  file.failAt = 0;
  file.operations = 0;
  CommittedJournalIdentitySource source(f.journal);
  ASSERT_TRUE(sorter.build(source));
  EXPECT_FALSE(builder.build(sorter, 1));
  EXPECT_EQ(file.published, prior);
}
TEST(CompanionTintaJournal, ChecksummedIdentityIndexUsesNumericOrderAndFailsClosedOnReadCorruption) {
  class IndexStorage final : public JournalIdentityIndexStorage {
   public:
    std::array<uint8_t, JOURNAL_IDENTITY_HEADER_SIZE + 3 * JOURNAL_IDENTITY_ENTRY_SIZE> bytes{};
    bool fail = false;
    bool size(uint64_t& length) override {
      length = bytes.size();
      return !fail;
    }
    bool read(uint64_t at, std::span<uint8_t> output) override {
      if (fail || at > bytes.size() || output.size() > bytes.size() - at) return false;
      std::copy_n(bytes.begin() + at, output.size(), output.begin());
      return true;
    }
  } storage;
  std::array<JournalIdentityEntry, 3> entries{};
  for (unsigned at = 0; at < 3; ++at) {
    entries[at].identity.origin.fill(1);
    entries[at].identity.epoch = 1;
    entries[at].identity.sequence = at == 0 ? 1 : at == 1 ? 255 : 256;
    entries[at].record = 2 - at;
    ASSERT_TRUE(encodeJournalIdentityEntry(
        entries[at],
        std::span(storage.bytes)
            .subspan(JOURNAL_IDENTITY_HEADER_SIZE + at * JOURNAL_IDENTITY_ENTRY_SIZE, JOURNAL_IDENTITY_ENTRY_SIZE)));
  }
  auto header = [&] {
    ASSERT_TRUE(encodeJournalIdentityHeader(3,
                                            binary_record::crc32(storage.bytes.data() + JOURNAL_IDENTITY_HEADER_SIZE,
                                                                 storage.bytes.size() - JOURNAL_IDENTITY_HEADER_SIZE),
                                            std::span(storage.bytes).first(JOURNAL_IDENTITY_HEADER_SIZE)));
  };
  header();
  std::array<uint8_t, JOURNAL_IDENTITY_ENTRY_SIZE> scratch{};
  IndexedJournalIdentities index(storage, scratch);
  ASSERT_TRUE(index.open(3));
  for (const auto& entry : entries) {
    uint32_t record = UINT32_MAX;
    ASSERT_EQ(index.find(entry.identity, record), JournalIdentityLookup::Found);
    EXPECT_EQ(record, entry.record);
  }
  auto missing = entries[0].identity;
  missing.sequence = 2;
  uint32_t record = 99;
  EXPECT_EQ(index.find(missing, record), JournalIdentityLookup::Missing);
  EXPECT_EQ(record, 99u);
  const auto valid = storage.bytes;
  storage.bytes[JOURNAL_IDENTITY_HEADER_SIZE + JOURNAL_IDENTITY_ENTRY_SIZE] ^= 1;
  EXPECT_EQ(index.find(entries[1].identity, record), JournalIdentityLookup::IoError);
  EXPECT_EQ(index.find(entries[0].identity, record), JournalIdentityLookup::IoError);
  EXPECT_FALSE(index.open(3));
  storage.bytes = valid;
  entries[1].identity = entries[0].identity;
  ASSERT_TRUE(encodeJournalIdentityEntry(
      entries[1],
      std::span(storage.bytes)
          .subspan(JOURNAL_IDENTITY_HEADER_SIZE + JOURNAL_IDENTITY_ENTRY_SIZE, JOURNAL_IDENTITY_ENTRY_SIZE)));
  header();
  EXPECT_FALSE(index.open(3));
  storage.bytes = valid;
  EXPECT_FALSE(index.open(2));
  storage.fail = true;
  EXPECT_FALSE(index.open(3));
}
TEST(CompanionTintaJournal, IndexedCausalAuditVerifiesRecordIdentityAndPredecessorOrder) {
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  auto second = f.event;
  second.identity.sequence = 2;
  second.ancestorCount = 1;
  second.ancestors[0] = f.event.identity;
  ASSERT_EQ(f.journal.append(second, f.body()), TintaJournalResult::Ok);
  class Index final : public JournalIdentityIndex {
   public:
    EventIdentity first{}, second{};
    unsigned mode = 0, calls = 0;
    JournalIdentityLookup find(const EventIdentity& identity, uint32_t& record) override {
      ++calls;
      if (mode == 1) return JournalIdentityLookup::IoError;
      if (mode == 2) return JournalIdentityLookup::Missing;
      if (identity == second) {
        record = 1;
        return JournalIdentityLookup::Found;
      }
      if (identity == first) {
        record = mode == 3 ? 1 : 0;
        return JournalIdentityLookup::Found;
      }
      if (mode == 4) {
        record = 0;
        return JournalIdentityLookup::Found;
      }
      return JournalIdentityLookup::Missing;
    }
  } index;
  index.first = f.event.identity;
  index.second = second.identity;
  JournalCausalValidation audit;
  const auto files = f.storage.data;
  const auto headers = f.storage.headers;
  EXPECT_EQ(audit.validate(f.journal, index), TintaJournalResult::Ok);
  EXPECT_EQ(index.calls, 4u);
  for (unsigned mode = 1; mode <= 3; ++mode) {
    index.mode = mode;
    EXPECT_EQ(audit.validate(f.journal, index), mode == 1 ? TintaJournalResult::IoError : TintaJournalResult::Corrupt);
    EXPECT_EQ(f.storage.data, files);
    EXPECT_EQ(f.storage.headers, headers);
  }
  auto rewrite = [&](const SyncEvent& event) {
    auto record = std::span(f.storage.data).subspan(TintaJournal::RECORD_SIZE, TintaJournal::RECORD_SIZE);
    ASSERT_NE(encodeRecord(event, record.subspan(12, MAX_RECORD_SIZE)), 0u);
    uint32_t crc = UINT32_MAX;
    for (const auto byte : record.first(508)) {
      crc ^= byte;
      for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    tinta_body_detail::write(record, 508, ~crc, 4);
    ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  };
  auto orphan = second;
  orphan.ancestors[0].origin.fill(9);
  rewrite(orphan);
  index.mode = 4;
  EXPECT_EQ(audit.validate(f.journal, index), TintaJournalResult::Corrupt);
  orphan = second;
  orphan.identity.sequence = 3;
  rewrite(orphan);
  index.second = orphan.identity;
  index.mode = 0;
  EXPECT_EQ(audit.validate(f.journal, index), TintaJournalResult::Corrupt);
}
TEST(CompanionTintaJournal, MissingAncestorsAndOriginSequenceGapsCannotPublishRecords) {
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  auto event = f.event;
  event.identity.sequence = 2;
  auto writes = f.storage.writes;
  EXPECT_EQ(f.journal.append(event, f.body()), TintaJournalResult::Invalid);
  EXPECT_EQ(f.storage.writes, writes);
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  event.identity.sequence = 3;
  writes = f.storage.writes;
  EXPECT_EQ(f.journal.append(event, f.body()), TintaJournalResult::Invalid);
  EXPECT_EQ(f.storage.writes, writes);
  event.identity.sequence = 2;
  event.ancestorCount = 1;
  event.ancestors[0] = f.event.identity;
  event.ancestors[0].origin.fill(9);
  EXPECT_EQ(f.journal.append(event, f.body()), TintaJournalResult::Invalid);
  EXPECT_EQ(f.storage.writes, writes);
  event.ancestors[0] = f.event.identity;
  ASSERT_EQ(f.journal.append(event, f.body()), TintaJournalResult::Ok);
  std::array<SyncEvent, 2> pair{event, event};
  pair[0].identity.sequence = 3;
  pair[0].ancestors[0] = event.identity;
  pair[1].identity.sequence = 4;
  pair[1].ancestors[0] = pair[0].identity;
  std::array<std::span<const uint8_t>, 2> bodies{f.body(), f.body()};
  auto invalid = pair;
  invalid[0].ancestors[0].origin.fill(9);
  writes = f.storage.writes;
  EXPECT_EQ(f.journal.appendPair(invalid, bodies), TintaJournalResult::Invalid);
  EXPECT_EQ(f.storage.writes, writes);
  ASSERT_EQ(f.journal.appendPair(pair, bodies), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.count(), 4u);
}
TEST(CompanionTintaJournal, PersistsPreferencesAndReviewsInOneFixedRecordJournal) {
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  std::array<uint8_t, 69> bytes{1, 4, 11, 3, 1};
  bytes[5] = 1;
  bytes[37] = 31;
  std::fill(bytes.begin() + 38, bytes.end(), 'a');
  auto preference = f.event;
  preference.kind = EventKind::Preference;
  preference.resource = PREFERENCE_SCOPE;
  preference.schedulerVersion = 0;
  preference.schedulerConfiguration.fill(0);
  ASSERT_TRUE(f.storage.digest(bytes, preference.bodyHash));
  for (unsigned field = 0; field < 4; ++field) {
    auto invalid = preference;
    if (field == 0) invalid.resource[0] ^= 1;
    if (field == 1) invalid.bodyHash[0] ^= 1;
    if (field == 2) invalid.schedulerConfiguration[0] = 1;
    if (field == 3) invalid.ancestorCount = MAX_ANCESTORS + 1;
    const auto writes = f.storage.writes;
    EXPECT_EQ(f.journal.append(invalid, bytes), TintaJournalResult::Invalid);
    EXPECT_EQ(f.storage.writes, writes);
  }
  ASSERT_EQ(f.journal.append(preference, bytes), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.append(preference, bytes), TintaJournalResult::Duplicate);
  f.event.identity.sequence = 2;
  f.event.ancestorCount = 1;
  f.event.ancestors[0] = preference.identity;
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  ASSERT_EQ(f.storage.data.size(), 2 * TintaJournal::RECORD_SIZE);
  TintaJournal restarted(f.storage, f.scratch);
  ASSERT_EQ(restarted.open(), TintaJournalResult::Ok);
  ASSERT_EQ(restarted.count(), 2u);
  ASSERT_EQ(restarted.read(0), TintaJournalResult::Ok);
  EXPECT_EQ(restarted.event().kind, EventKind::Preference);
  EXPECT_TRUE(std::equal(restarted.body().begin(), restarted.body().end(), bytes.begin(), bytes.end()));
  ASSERT_EQ(restarted.read(1), TintaJournalResult::Ok);
  EXPECT_EQ(restarted.event().ancestors[0], preference.identity);
  EXPECT_EQ(restarted.event().kind, EventKind::Review);
}
TEST(CompanionTintaJournal, SharedFrontierFixtureMatchesReaderEncodingAndIndependentSha256) {
  std::ifstream input(TINTA_FRONTIER_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> fixture{std::istreambuf_iterator<char>(input), {}};
  ASSERT_EQ(fixture.size(), 410u);
  std::vector<uint8_t> output;
  output.reserve(fixture.size());
  std::array<uint8_t, 512> scratch{};
  TintaJournalFrontierEncoding frontier(scratch, &output, [](void* ctx, std::span<const uint8_t> bytes) {
    auto& out = *static_cast<std::vector<uint8_t>*>(ctx);
    out.insert(out.end(), bytes.begin(), bytes.end());
    return true;
  });
  ASSERT_TRUE(frontier.begin(2));
  size_t offset = 12;
  for (unsigned i = 0; i < 2; ++i) {
    const auto length = fixture[offset] | (static_cast<size_t>(fixture[offset + 1]) << 8);
    offset += 2;
    SyncEvent event;
    ASSERT_TRUE(decodeRecord(std::span(fixture).subspan(offset, length), event));
    ASSERT_TRUE(frontier.append(event));
    offset += length;
  }
  ASSERT_TRUE(frontier.complete());
  ASSERT_EQ(offset, fixture.size() - 32);
  EXPECT_TRUE(std::equal(output.begin(), output.end(), fixture.begin(), fixture.begin() + offset));
  Digest digest;
  ASSERT_NE(SHA256(output.data(), output.size(), digest.data()), nullptr);
  EXPECT_TRUE(std::equal(digest.begin(), digest.end(), fixture.begin() + offset));
}
TEST(CompanionTintaJournal, FrontierEncodingMatchesCanonicalBytesAndRejectsIncompleteOrUnorderedInput) {
  Fixture f;
  std::vector<uint8_t> output;
  output.reserve(1024);
  auto sink = [](void* ctx, std::span<const uint8_t> bytes) {
    auto& out = *static_cast<std::vector<uint8_t>*>(ctx);
    out.insert(out.end(), bytes.begin(), bytes.end());
    return true;
  };
  TintaJournalFrontierEncoding frontier(f.scratch, &output, sink);
  ASSERT_TRUE(frontier.begin(1));
  EXPECT_FALSE(frontier.complete());
  ASSERT_TRUE(frontier.append(f.event));
  EXPECT_TRUE(frontier.complete());
  std::vector<uint8_t> expected{'T', 'J', 'F', '1', 1, 0, 0, 0, 0, 0, 0, 0};
  expected.reserve(1024);
  const auto length = encodeRecord(f.event, f.scratch);
  expected.push_back(length & 255);
  expected.push_back(length >> 8);
  expected.insert(expected.end(), f.scratch.begin(), f.scratch.begin() + length);
  EXPECT_EQ(output, expected);
  EXPECT_FALSE(frontier.append(f.event));
  EXPECT_FALSE(frontier.complete());
  output.clear();
  ASSERT_TRUE(frontier.begin(2));
  ASSERT_TRUE(frontier.append(f.event));
  EXPECT_FALSE(frontier.append(f.event));
  EXPECT_FALSE(frontier.complete());
  ASSERT_TRUE(frontier.begin(2));
  f.event.identity.epoch = 255;
  ASSERT_TRUE(frontier.append(f.event));
  f.event.identity.epoch = 256;
  ASSERT_TRUE(frontier.append(f.event));
  EXPECT_TRUE(frontier.complete());
  ASSERT_TRUE(frontier.begin(0));
  EXPECT_TRUE(frontier.complete());
  TintaJournalFrontierEncoding failing(f.scratch, nullptr, [](void*, std::span<const uint8_t>) { return false; });
  EXPECT_FALSE(failing.begin(0));
  EXPECT_FALSE(failing.complete());
  TintaJournalFrontierEncoding small(std::span(f.scratch).first(12), &output, sink);
  EXPECT_FALSE(small.begin(0));
}
TEST(CompanionTintaJournal, AtomicPairRecoversEveryRecordAndHeaderWriteCut) {
  for (unsigned phase = 0; phase < 3; ++phase) {
    const size_t maximum = phase == 2 ? 64 : 512;
    for (size_t cut = 0; cut <= maximum; ++cut) {
      SCOPED_TRACE(phase);
      SCOPED_TRACE(cut);
      Fixture f;
      ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
      ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
      for (auto& header : f.storage.headers) {
        header[3] = 1;
        uint32_t crc = UINT32_MAX;
        for (size_t at = 0; at < 60; ++at) {
          crc ^= header[at];
          for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
        tinta_body_detail::write(header, 60, ~crc, 4);
      }
      ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
      std::array<SyncEvent, 2> events{f.event, f.event};
      events[0].identity.sequence = 2;
      events[1].identity.sequence = 3;
      events[0].ancestorCount = events[1].ancestorCount = 1;
      events[0].ancestors[0] = f.event.identity;
      events[1].ancestors[0] = events[0].identity;
      std::array<std::span<const uint8_t>, 2> bodies{f.body(), f.body()};
      if (phase == 2)
        f.storage.partialHeader = cut;
      else {
        f.storage.skipRecordCuts = phase;
        f.storage.partialRecord = cut;
      }
      EXPECT_EQ(f.journal.appendPair(events, bodies), TintaJournalResult::IoError);
      TintaJournal recovered(f.storage, f.scratch);
      ASSERT_EQ(recovered.open(), TintaJournalResult::Ok);
      EXPECT_EQ(recovered.count(), phase == 2 && cut == 64 ? 3u : 1u);
      const auto retry = recovered.appendPair(events, bodies);
      EXPECT_EQ(retry, phase == 2 && cut == 64 ? TintaJournalResult::Duplicate : TintaJournalResult::Ok);
      EXPECT_EQ(recovered.count(), 3u);
      ASSERT_EQ(recovered.read(2), TintaJournalResult::Ok);
      EXPECT_EQ(recovered.event().identity, events[1].identity);
      const auto writes = f.storage.writes;
      EXPECT_EQ(recovered.appendPair(events, bodies), TintaJournalResult::Duplicate);
      EXPECT_EQ(f.storage.writes, writes);
      events[1].identity.sequence = 4;
      EXPECT_EQ(recovered.appendPair(events, bodies), TintaJournalResult::Conflict);
      EXPECT_EQ(f.storage.writes, writes);
    }
  }
}
TEST(CompanionTintaJournal, ItemReplayUsesOriginalSchedulerAndCountsEachStudyDay) {
  Fixture f;
  TintaBody body;
  ASSERT_TRUE(decodeTintaBody(f.body(), body));
  body.configuration = {8750, 730};
  auto state = tinta::core::ItemState::fresh(body.uid);
  auto expected = state;
  tinta::core::Fsrs scheduler;
  tinta::core::Fsrs reference(0.875f, 730);
  TintaReplayCounts counts;
  ASSERT_TRUE(applyTintaItemReplay(body, 10, scheduler, state, counts));
  tinta::core::applyReview(reference, expected, tinta::core::Grade::Good, 10);
  EXPECT_EQ(state, expected);
  EXPECT_TRUE(counts.newItem);
  EXPECT_FALSE(counts.review);
  ASSERT_TRUE(applyTintaItemReplay(body, 10, scheduler, state, counts));
  EXPECT_FALSE(counts.newItem);
  EXPECT_FALSE(counts.review);
  ASSERT_TRUE(applyTintaItemReplay(body, 11, scheduler, state, counts));
  EXPECT_FALSE(counts.newItem);
  EXPECT_TRUE(counts.review);
  state.flags |= tinta::core::item_flag::kLeech;
  body.kind = EventKind::Suspension;
  body.enabled = true;
  ASSERT_TRUE(applyTintaItemReplay(body, 11, scheduler, state, counts));
  body.kind = EventKind::Star;
  ASSERT_TRUE(applyTintaItemReplay(body, 11, scheduler, state, counts));
  EXPECT_EQ(state.flags, tinta::core::item_flag::kAll);
  EXPECT_FALSE(counts.newItem);
  EXPECT_FALSE(counts.review);
  body.enabled = false;
  ASSERT_TRUE(applyTintaItemReplay(body, 11, scheduler, state, counts));
  EXPECT_EQ(state.flags, tinta::core::item_flag::kLeech | tinta::core::item_flag::kSuspended);
  const auto original = state;
  body.kind = EventKind::LessonComplete;
  EXPECT_FALSE(applyTintaItemReplay(body, 11, scheduler, state, counts));
  EXPECT_EQ(state, original);
  body.kind = EventKind::Review;
  EXPECT_FALSE(applyTintaItemReplay(body, UINT32_MAX, scheduler, state, counts));
  EXPECT_EQ(state, original);
}
TEST(CompanionTintaJournal, DurableAppendReopenAndDuplicates) {
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.count(), 1U);
  const auto writes = f.storage.writes;
  EXPECT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Duplicate);
  EXPECT_EQ(f.storage.writes, writes);
  TintaJournal reopened(f.storage, f.scratch);
  ASSERT_EQ(reopened.open(), TintaJournalResult::Ok);
  ASSERT_EQ(reopened.read(0), TintaJournalResult::Ok);
  EXPECT_EQ(reopened.event(), f.event);
  EXPECT_TRUE(std::equal(reopened.body().begin(), reopened.body().end(), f.body().begin()));
  auto conflict = f.event;
  conflict.timestamp = 1;
  EXPECT_EQ(reopened.append(conflict, f.body()), TintaJournalResult::Conflict);
  EXPECT_EQ(f.storage.writes, writes);
}
TEST(CompanionTintaJournal, HeaderReadFailurePreservesHistoryAndBlocksAppendUntilRecovery) {
  for (bool populated : {false, true}) {
    for (int slot : {0, 1}) {
      Fixture f;
      ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
      if (populated) {
        ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
      }
      const auto data = f.storage.data;
      const auto headers = f.storage.headers;
      const auto writes = f.storage.writes;
      f.storage.failedHeader = slot;
      EXPECT_EQ(f.journal.open(), TintaJournalResult::IoError);
      EXPECT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Unavailable);
      EXPECT_EQ(f.storage.data, data);
      EXPECT_EQ(f.storage.headers, headers);
      EXPECT_EQ(f.storage.writes, writes);
      f.storage.failedHeader = -1;
      ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
      EXPECT_EQ(f.journal.count(), populated ? 1u : 0u);
    }
  }
}
TEST(CompanionTintaJournal, EveryRecordWriteCutRollsBackUncommittedTail) {
  for (size_t cut = 0; cut <= 512; ++cut) {
    Fixture f;
    ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
    f.storage.partialRecord = cut;
    ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::IoError);
    EXPECT_FALSE(f.journal.available());
    ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
    EXPECT_EQ(f.journal.count(), 0U);
    EXPECT_TRUE(f.storage.data.empty());
    EXPECT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  }
}
TEST(CompanionTintaJournal, EveryHeaderWriteCutAndLostAcknowledgement) {
  for (size_t cut = 0; cut <= 64; ++cut) {
    Fixture f;
    ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
    ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
    ++f.event.identity.sequence;
    f.storage.partialHeader = cut;
    ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::IoError);
    ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
    EXPECT_EQ(f.journal.count(), cut == 64 ? 2U : 1U);
    EXPECT_EQ(f.journal.append(f.event, f.body()), cut == 64 ? TintaJournalResult::Duplicate : TintaJournalResult::Ok);
  }
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  f.storage.lostHeaderReply = true;
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::IoError);
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Duplicate);
}
TEST(CompanionTintaJournal, CorruptCommittedHistoryAndMissingHeadersArePreserved) {
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  f.storage.data[40] ^= 1;
  const auto preserved = f.storage.data;
  EXPECT_EQ(f.journal.open(), TintaJournalResult::Corrupt);
  EXPECT_EQ(f.storage.data, preserved);
  for (auto& header : f.storage.headers) header.clear();
  EXPECT_EQ(f.journal.open(), TintaJournalResult::Corrupt);
  EXPECT_EQ(f.storage.data, preserved);
}
TEST(CompanionTintaJournal, InvalidInputsAndFailedRecoveryCannotAppend) {
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  auto invalid = f.event;
  invalid.bodyHash.fill(0);
  EXPECT_EQ(f.journal.append(invalid, f.body()), TintaJournalResult::Invalid);
  f.storage.partialRecord = 200;
  EXPECT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::IoError);
  f.storage.refuseTruncate = true;
  EXPECT_EQ(f.journal.open(), TintaJournalResult::IoError);
  EXPECT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Unavailable);
  TintaJournal tooSmall(f.storage, std::span(f.scratch).first(511));
  EXPECT_EQ(tooSmall.open(), TintaJournalResult::Unavailable);
}

TEST(CompanionTintaJournal, BorrowedViewsCannotBeAppendedAndCommittedTruncationFailsClosed) {
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.read(0), TintaJournalResult::Ok);
  const auto writes = f.storage.writes;
  EXPECT_EQ(f.journal.append(f.event, f.journal.body()), TintaJournalResult::Invalid);
  EXPECT_EQ(f.journal.append(f.journal.event(), f.body()), TintaJournalResult::Invalid);
  EXPECT_EQ(f.storage.writes, writes);
  f.storage.data.resize(511);
  const auto preserved = f.storage.data;
  EXPECT_EQ(f.journal.open(), TintaJournalResult::Corrupt);
  EXPECT_EQ(f.storage.data, preserved);
}

namespace {
class Identities final : public IdentityStorage {
 public:
  std::array<uint8_t, IDENTITY_RECORD_SIZE> binding{};
  Identity marker{};
  uint8_t random = 5;
  bool saved = false;
  bool failBinding = false;
  bool hardwareIdentity(Identity& output) override {
    output.fill(1);
    return true;
  }
  bool cardIdentity(Identity& output) override {
    output.fill(2);
    return true;
  }
  IdentityRead readBinding(std::span<uint8_t> output) override {
    if (!saved) return IdentityRead::Missing;
    std::copy(binding.begin(), binding.end(), output.begin());
    return IdentityRead::Present;
  }
  bool writeBinding(std::span<const uint8_t> input) override {
    if (failBinding) return false;
    std::copy(input.begin(), input.end(), binding.begin());
    saved = true;
    return true;
  }
  IdentityRead readMarker(Identity& output) override {
    if (marker[0] == 0) return IdentityRead::Missing;
    output = marker;
    return IdentityRead::Present;
  }
  bool createMarker(const Identity& input) override {
    marker = input;
    return true;
  }
  bool randomIdentity(Identity& output) override {
    output.fill(random++);
    return true;
  }
};
}  // namespace
TEST(CompanionTintaJournal, WriterFlagsCommitTogetherAndAdvanceFrontierOnlyAfterSuccess) {
  for (bool fail : {false, true}) {
    Fixture f;
    Identities identities;
    TintaWriter writer(f.journal, f.storage);
    ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
    TintaBody review;
    ASSERT_TRUE(decodeTintaBody(f.body(), review));
    ASSERT_EQ(writer.record(review, f.event.resource, 1, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
    const auto previous = writer.committedIdentity();
    std::array<TintaBody, 2> bodies{review, review};
    bodies[0].kind = EventKind::Suspension;
    bodies[1].kind = EventKind::Star;
    bodies[0].enabled = bodies[1].enabled = true;
    if (fail) f.storage.partialHeader = 0;
    const auto result = writer.recordFlags(bodies, f.event.resource, 1, 0, ClockQuality::Unknown);
    EXPECT_EQ(result, fail ? TintaJournalResult::IoError : TintaJournalResult::Ok);
    if (fail) {
      EXPECT_FALSE(writer.available());
      EXPECT_EQ(writer.committedIdentity(), previous);
      ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
      EXPECT_EQ(f.journal.count(), 1u);
    } else {
      EXPECT_EQ(f.journal.count(), 3u);
      ASSERT_EQ(f.journal.read(1), TintaJournalResult::Ok);
      EXPECT_EQ(f.journal.event().identity.sequence, 2u);
      EXPECT_EQ(f.journal.event().ancestors[0], previous);
      const auto first = f.journal.event().identity;
      ASSERT_EQ(f.journal.read(2), TintaJournalResult::Ok);
      EXPECT_EQ(f.journal.event().ancestors[0], first);
      EXPECT_EQ(writer.committedIdentity(), f.journal.event().identity);
      ASSERT_EQ(writer.record(review, f.event.resource, 1, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
      EXPECT_EQ(writer.committedIdentity().sequence, 4u);
    }
  }
}
TEST(CompanionTintaJournal, ActualProgressStoreJournalsBeforeLocalWritesAndBlocksUncertainRetries) {
  for (unsigned failure = 0; failure < 3; ++failure) {
    SCOPED_TRACE(failure);
    Fixture f;
    Identities identities;
    TintaWriter writer(f.journal, f.storage);
    ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
    Identity course{};
    course.fill(7);
    TintaProgressJournal adapter(writer, course, f.event.resource);
    tinta_test::MemStore local;
    const auto catalog = tinta_test::FakeCatalog::vocab(1, 1);
    tinta::core::Fsrs fsrs;
    tinta::core::ProgressStore progress(local, catalog, fsrs);
    std::array<uint16_t, 2> slots{};
    ASSERT_EQ(progress.open(slots.data(), slots.size(), nullptr, 0), tinta::core::ProgressStore::OpenResult::Created);
    const auto original = local.files;
    struct Context {
      TintaProgressJournal& adapter;
      tinta_test::MemStore& local;
      decltype(local.files) original;
      unsigned failure;
      unsigned calls = 0;
    } context{adapter, local, original, failure};
    progress.setMutationJournal(
        {&context, [](void* raw, const tinta::core::JournalEntry& entry, const tinta::core::ItemState& before,
                      const tinta::core::ItemState& after, uint32_t milliseconds) {
           auto& context = *static_cast<Context*>(raw);
           ++context.calls;
           EXPECT_EQ(context.local.files, context.original);
           const auto result = context.adapter.persist(entry, before, after, milliseconds, {}, ClockQuality::Unknown);
           if (context.failure == 2) context.local.failFrom(context.local.calls);
           return result == TintaJournalResult::Ok;
         }});
    if (failure == 1) f.storage.partialHeader = 0;
    const auto review = progress.review(0, tinta::core::Grade::Good, 0, 1234, 5, 0);
    EXPECT_EQ(review.status,
              failure == 0 ? tinta::core::ProgressStore::Status::Stored : tinta::core::ProgressStore::Status::Failed);
    EXPECT_EQ(context.calls, 1u);
    if (failure) {
      EXPECT_EQ(local.files, original);
      EXPECT_TRUE(progress.failed());
      EXPECT_EQ(progress.review(0, tinta::core::Grade::Good, 0, 1234, 5, 0).status,
                tinta::core::ProgressStore::Status::Failed);
      EXPECT_EQ(context.calls, 1u);
    }
    ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
    EXPECT_EQ(f.journal.count(), failure == 1 ? 0u : 1u);
    if (failure != 1) {
      ASSERT_EQ(f.journal.read(0), TintaJournalResult::Ok);
      TintaBody body;
      ASSERT_TRUE(decodeTintaBody(f.journal.body(), body));
      EXPECT_EQ(body.responseMilliseconds, 1234u);
      EXPECT_EQ(f.journal.event().identity, adapter.undoIdentity());
    }
  }
}
TEST(CompanionTintaJournal, ProgressAdapterPreservesExactReviewAndClearsUndoAfterFlags) {
  Fixture f;
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  Identity course{};
  course.fill(7);
  TintaProgressJournal adapter(writer, course, f.event.resource);
  auto before = tinta::core::ItemState::fresh(1);
  auto after = before;
  tinta::core::JournalEntry entry;
  entry.uid = 1;
  entry.op = 3;
  entry.day = 5;
  ASSERT_EQ(adapter.persist(entry, before, after, 1234, {}, ClockQuality::Unknown), TintaJournalResult::Ok);
  EXPECT_TRUE(adapter.pendingApplication());
  EXPECT_EQ(adapter.applicationIdentity(), writer.committedIdentity());
  adapter.didApply();
  EXPECT_FALSE(adapter.pendingApplication());
  const auto review = adapter.undoIdentity();
  ASSERT_EQ(f.journal.read(0), TintaJournalResult::Ok);
  TintaBody decoded;
  ASSERT_TRUE(decodeTintaBody(f.journal.body(), decoded));
  EXPECT_EQ(decoded.responseMilliseconds, 1234u);
  entry.op = tinta::core::JournalEntry::kUndo << 3;
  ASSERT_EQ(adapter.persist(entry, before, after, 0, {}, ClockQuality::Unknown), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.read(1), TintaJournalResult::Ok);
  ASSERT_TRUE(decodeTintaBody(f.journal.body(), decoded));
  EXPECT_EQ(decoded.undoTarget, review);
  entry.op = 3;
  ASSERT_EQ(adapter.persist(entry, before, after, 1000, {}, ClockQuality::Unknown), TintaJournalResult::Ok);
  entry.op = tinta::core::JournalEntry::kSetFlags << 3;
  entry.arg = after.flags = tinta::core::item_flag::kSuspended | tinta::core::item_flag::kStarred;
  ASSERT_EQ(adapter.persist(entry, before, after, 0, {}, ClockQuality::Unknown), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.count(), 5u);
  EXPECT_EQ(adapter.undoIdentity(), EventIdentity{});
  entry.op = tinta::core::JournalEntry::kUndo << 3;
  EXPECT_EQ(adapter.persist(entry, before, after, 0, {}, ClockQuality::Unknown), TintaJournalResult::Invalid);
  EXPECT_EQ(f.journal.count(), 5u);
}
TEST(CompanionTintaJournal, WriterReservesEpochAndAddsExactUndoDependency) {
  Fixture f;
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  TintaBody body;
  ASSERT_TRUE(decodeTintaBody(f.body(), body));
  ASSERT_EQ(writer.record(body, f.event.resource, 1, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  const auto review = writer.committedIdentity();
  EXPECT_EQ(review.sequence, 1U);
  body.kind = EventKind::Star;
  body.enabled = true;
  ASSERT_EQ(writer.record(body, f.event.resource, 1, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  const auto star = writer.committedIdentity();
  body.kind = EventKind::UndoReview;
  body.undoTarget = review;
  const auto writes = f.storage.writes;
  body.undoTarget.sequence += 100;
  EXPECT_EQ(writer.record(body, f.event.resource, 1, 0, ClockQuality::Unknown), TintaJournalResult::Invalid);
  body.undoTarget = star;
  EXPECT_EQ(writer.record(body, f.event.resource, 1, 0, ClockQuality::Unknown), TintaJournalResult::Invalid);
  body.undoTarget = review;
  ++body.uid;
  EXPECT_EQ(writer.record(body, f.event.resource, 1, 0, ClockQuality::Unknown), TintaJournalResult::Invalid);
  --body.uid;
  body.course[0] ^= 1;
  EXPECT_EQ(writer.record(body, f.event.resource, 1, 0, ClockQuality::Unknown), TintaJournalResult::Invalid);
  body.course[0] ^= 1;
  EXPECT_EQ(f.storage.writes, writes);
  EXPECT_EQ(writer.committedIdentity(), star);
  EXPECT_TRUE(writer.available());
  ASSERT_EQ(writer.record(body, f.event.resource, 1, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.read(2), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.event().ancestorCount, 2);
  EXPECT_EQ(f.journal.event().ancestors[0], star);
  EXPECT_EQ(f.journal.event().ancestors[1], review);
  const auto undo = writer.committedIdentity();
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  ASSERT_TRUE(decodeTintaBody(f.body(), body));
  ASSERT_EQ(writer.record(body, f.event.resource, 2, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  EXPECT_GT(writer.committedIdentity().epoch, review.epoch);
  EXPECT_EQ(writer.committedIdentity().sequence, 1U);
  ASSERT_EQ(f.journal.read(3), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.event().ancestors[0], undo);
}
TEST(CompanionTintaJournal, FreshWriterAppendsWithoutHistoryReadsAndDetectsOtherOwners) {
  Fixture f;
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  TintaBody body;
  ASSERT_TRUE(decodeTintaBody(f.body(), body));
  const auto reads = f.storage.reads;
  for (unsigned at = 0; at < 64; ++at)
    ASSERT_EQ(writer.record(body, f.event.resource, 1, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  EXPECT_EQ(f.storage.reads, reads);
  EXPECT_EQ(f.journal.count(), 64u);
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  const auto writes = f.storage.writes;
  EXPECT_EQ(writer.record(body, f.event.resource, 1, 0, ClockQuality::Unknown), TintaJournalResult::Conflict);
  EXPECT_EQ(f.storage.writes, writes);
  EXPECT_FALSE(writer.available());
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  const auto reopenedReads = f.storage.reads;
  ASSERT_EQ(writer.record(body, f.event.resource, 1, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  EXPECT_EQ(f.storage.reads, reopenedReads);
}
TEST(CompanionTintaJournal, ReusedReservedEpochCannotEnableFreshAppend) {
  Fixture f;
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  const auto previousBinding = identities.binding;
  TintaBody body;
  ASSERT_TRUE(decodeTintaBody(f.body(), body));
  ASSERT_EQ(writer.record(body, f.event.resource, 1, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  ASSERT_EQ(writer.record(body, f.event.resource, 1, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  identities.binding = previousBinding;
  const auto writes = f.storage.writes;
  EXPECT_EQ(writer.start(identities), TintaJournalResult::Conflict);
  EXPECT_FALSE(writer.available());
  EXPECT_EQ(f.storage.writes, writes);
}
TEST(CompanionTintaJournal, WriterBlocksAfterFailureUntilFreshDurableEpoch) {
  Fixture f;
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  identities.failBinding = true;
  EXPECT_EQ(writer.start(identities), TintaJournalResult::IoError);
  EXPECT_FALSE(writer.available());
  identities.failBinding = false;
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  TintaBody body;
  ASSERT_TRUE(decodeTintaBody(f.body(), body));
  f.storage.lostHeaderReply = true;
  EXPECT_EQ(writer.record(body, f.event.resource, 1, 0, ClockQuality::Unknown), TintaJournalResult::IoError);
  EXPECT_FALSE(writer.available());
  EXPECT_EQ(writer.record(body, f.event.resource, 1, 0, ClockQuality::Unknown), TintaJournalResult::Unavailable);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.count(), 1U);
  ASSERT_EQ(writer.record(body, f.event.resource, 2, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.count(), 2U);
}

TEST(CompanionTintaJournal, DuplicateIgnoresUnserializedAncestorSlots) {
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  const auto writes = f.storage.writes;
  auto replay = f.event;
  replay.ancestors[3].origin.fill(9);
  replay.ancestors[3].epoch = 10;
  replay.ancestors[3].sequence = 20;
  EXPECT_EQ(f.journal.append(replay, f.body()), TintaJournalResult::Duplicate);
  EXPECT_EQ(f.storage.writes, writes);
  replay.timestamp = 1;
  EXPECT_EQ(f.journal.append(replay, f.body()), TintaJournalResult::Conflict);
}

TEST(CompanionTintaJournal, ExtendedBookmarksRecoverAndLegacyWorkspaceCannotTruncateThem) {
  Storage storage;
  std::array<uint8_t, TintaJournal::EXTENDED_RECORD_SIZE> scratch{};
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> body{};
  body[0] = 1;
  body[1] = 2;
  body[2] = 7;
  body[24] = 128;
  std::fill(body.begin() + 26, body.begin() + 154, 'a');
  body[155] = 2;
  std::fill(body.begin() + 156, body.end(), 'b');
  SyncEvent event;
  event.identity.origin.fill(1);
  event.identity.epoch = event.identity.sequence = 1;
  event.storageGeneration.fill(2);
  event.resource.fill(3);
  event.kind = EventKind::BookmarkPut;
  ASSERT_TRUE(storage.digest(body, event.bodyHash));
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
  ASSERT_EQ(storage.data.size(), 1024u);
  EXPECT_EQ(storage.headers[0][3], 3);
  const auto committed = storage.data;
  std::array<uint8_t, 512> legacyScratch{};
  TintaJournal legacy(storage, legacyScratch);
  EXPECT_EQ(legacy.open(), TintaJournalResult::Unavailable);
  EXPECT_EQ(storage.data, committed);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(journal.read(0), TintaJournalResult::Ok);
  EXPECT_TRUE(std::equal(journal.body().begin(), journal.body().end(), body.begin(), body.end()));
  EXPECT_EQ(journal.append(event, body), TintaJournalResult::Duplicate);
  event.identity.sequence = 2;
  storage.partialRecord = 800;
  EXPECT_EQ(journal.append(event, body), TintaJournalResult::IoError);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  EXPECT_EQ(journal.count(), 1u);
  EXPECT_EQ(storage.data, committed);
  ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
  storage.data[1024 + 200] ^= 1;
  EXPECT_EQ(journal.open(), TintaJournalResult::Corrupt);
}

TEST(CompanionTintaJournal, LargerWorkspaceRetainsLegacyRecordOffsets) {
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  std::array<uint8_t, TintaJournal::EXTENDED_RECORD_SIZE> scratch{};
  TintaJournal larger(f.storage, scratch);
  ASSERT_EQ(larger.open(), TintaJournalResult::Ok);
  ++f.event.identity.sequence;
  ASSERT_EQ(larger.append(f.event, f.body()), TintaJournalResult::Ok);
  EXPECT_EQ(f.storage.data.size(), 1024u);
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.count(), 2u);
}

TEST(CompanionTintaJournal, MigrationResumesEveryHeaderCutAndPreservesSourceAndExactPrefix) {
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  ++f.event.identity.sequence;
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  const auto original = f.storage.data;
  const auto headers = f.storage.headers;
  class Index final : public JournalIdentityIndex {
   public:
    JournalIdentityLookup find(const EventIdentity& identity, uint32_t& record) override {
      Identity origin{};
      origin.fill(1);
      if (identity.origin != origin || identity.epoch != 1 || identity.sequence < 1 || identity.sequence > 2)
        return JournalIdentityLookup::Missing;
      record = identity.sequence - 1;
      return JournalIdentityLookup::Found;
    }
  } index;
  for (size_t cut = 0; cut <= TintaJournal::HEADER_SIZE; ++cut) {
    Storage target;
    std::array<uint8_t, TintaJournal::EXTENDED_RECORD_SIZE> scratch{};
    TintaJournal destination(target, scratch);
    ASSERT_EQ(destination.open(), TintaJournalResult::Ok);
    JournalMigration migration;
    target.partialHeader = cut;
    EXPECT_EQ(migration.copy(f.journal, destination, index), TintaJournalResult::IoError);
    ASSERT_EQ(destination.open(), TintaJournalResult::Ok);
    ASSERT_EQ(migration.copy(f.journal, destination, index), TintaJournalResult::Ok);
    EXPECT_EQ(destination.count(), 2u);
    EXPECT_EQ(target.data.size(), 2048u);
    EXPECT_EQ(migration.copy(f.journal, destination, index), TintaJournalResult::Ok);
    EXPECT_EQ(f.storage.data, original);
    EXPECT_EQ(f.storage.headers, headers);
  }
  Storage conflicting;
  std::array<uint8_t, TintaJournal::EXTENDED_RECORD_SIZE> scratch{};
  TintaJournal destination(conflicting, scratch);
  ASSERT_EQ(destination.open(), TintaJournalResult::Ok);
  f.event.identity.sequence = 1;
  f.event.timestamp = 123;
  ASSERT_EQ(destination.append(f.event, f.body()), TintaJournalResult::Ok);
  const auto before = conflicting.data;
  JournalMigration migration;
  EXPECT_EQ(migration.copy(f.journal, destination, index), TintaJournalResult::Conflict);
  EXPECT_EQ(conflicting.data, before);
  EXPECT_EQ(migration.copy(f.journal, f.journal, index), TintaJournalResult::Unavailable);
}

TEST(CompanionTintaJournal, MigrationPublicationRecoversLostRenameRepliesAndRetainsBackup) {
  class Publication final : public JournalMigrationPublicationStorage {
   public:
    std::array<bool, 3> present{true, true, false};
    bool pending = true, valid = true, failClear = false;
    unsigned moves = 0, failBefore = 0, failAfter = 0;
    JournalMigrationPresence intent(JournalMigrationIntent& output) override {
      output.count = 2;
      output.frontier.fill(7);
      return pending ? JournalMigrationPresence::Present : JournalMigrationPresence::Missing;
    }
    JournalMigrationPresence directory(JournalMigrationDirectory location) override {
      return present[static_cast<unsigned>(location)] ? JournalMigrationPresence::Present
                                                      : JournalMigrationPresence::Missing;
    }
    bool verify(JournalMigrationDirectory, const JournalMigrationIntent&, uint16_t) override { return valid; }
    bool move(JournalMigrationDirectory from, JournalMigrationDirectory to) override {
      ++moves;
      if (moves == failBefore) return false;
      const auto f = static_cast<unsigned>(from), t = static_cast<unsigned>(to);
      if (!present[f] || present[t]) return false;
      present[f] = false;
      present[t] = true;
      return moves != failAfter;
    }
    bool clearIntent() override {
      if (failClear) return false;
      pending = false;
      return true;
    }
  };
  for (unsigned step = 1; step <= 2; ++step) {
    for (bool after : {false, true}) {
      Publication storage;
      if (after)
        storage.failAfter = step;
      else
        storage.failBefore = step;
      EXPECT_EQ(recoverJournalMigration(storage), JournalMigrationPublicationResult::IoError);
      storage.failBefore = storage.failAfter = 0;
      EXPECT_EQ(recoverJournalMigration(storage), JournalMigrationPublicationResult::Complete);
      EXPECT_EQ(storage.present, (std::array<bool, 3>{true, false, true}));
      EXPECT_EQ(recoverJournalMigration(storage), JournalMigrationPublicationResult::NoPending);
    }
  }
  Publication storage;
  storage.failClear = true;
  EXPECT_EQ(recoverJournalMigration(storage), JournalMigrationPublicationResult::IoError);
  EXPECT_EQ(storage.moves, 2u);
  storage.failClear = false;
  EXPECT_EQ(recoverJournalMigration(storage), JournalMigrationPublicationResult::Complete);
  EXPECT_EQ(storage.moves, 2u);
  Publication invalid;
  invalid.valid = false;
  EXPECT_EQ(recoverJournalMigration(invalid), JournalMigrationPublicationResult::Conflict);
  EXPECT_EQ(invalid.moves, 0u);
  for (unsigned mask = 0; mask < 8; ++mask) {
    if (mask == 3 || mask == 5 || mask == 6) continue;
    Publication ambiguous;
    for (unsigned i = 0; i < 3; ++i) ambiguous.present[i] = (mask & (1u << i)) != 0;
    EXPECT_EQ(recoverJournalMigration(ambiguous), JournalMigrationPublicationResult::Conflict);
    EXPECT_EQ(ambiguous.moves, 0u);
    EXPECT_TRUE(ambiguous.pending);
  }
}

TEST(CompanionTintaJournal, MigrationIntentRejectsTruncationCorruptionAndInvalidBindings) {
  JournalMigrationIntent intent;
  intent.count = UINT32_MAX / 1024;
  intent.frontier.fill(7);
  std::array<uint8_t, JOURNAL_MIGRATION_INTENT_SIZE> bytes{};
  ASSERT_TRUE(encodeJournalMigrationIntent(intent, bytes));
  JournalMigrationIntent decoded;
  ASSERT_TRUE(decodeJournalMigrationIntent(bytes, decoded));
  EXPECT_EQ(decoded.count, intent.count);
  EXPECT_EQ(decoded.frontier, intent.frontier);
  for (size_t cut = 0; cut < bytes.size(); ++cut) {
    EXPECT_FALSE(decodeJournalMigrationIntent(std::span(bytes).first(cut), decoded));
    EXPECT_EQ(decoded.count, intent.count);
    EXPECT_EQ(decoded.frontier, intent.frontier);
  }
  for (size_t at = 0; at < bytes.size(); ++at) {
    auto corrupted = bytes;
    corrupted[at] ^= 1;
    EXPECT_FALSE(decodeJournalMigrationIntent(corrupted, decoded));
  }
  for (size_t at : {size_t{3}, size_t{40}, size_t{41}, size_t{42}, size_t{59}}) {
    auto corrupted = bytes;
    corrupted[at] ^= 1;
    tinta_body_detail::write(corrupted, 60, binary_record::crc32(corrupted.data(), 60), 4);
    EXPECT_FALSE(decodeJournalMigrationIntent(corrupted, decoded));
  }
  auto invalid = bytes;
  tinta_body_detail::write(invalid, 4, UINT32_MAX, 4);
  tinta_body_detail::write(invalid, 60, binary_record::crc32(invalid.data(), 60), 4);
  EXPECT_FALSE(decodeJournalMigrationIntent(invalid, decoded));
  invalid = bytes;
  std::fill(invalid.begin() + 8, invalid.begin() + 40, 0);
  tinta_body_detail::write(invalid, 60, binary_record::crc32(invalid.data(), 60), 4);
  EXPECT_FALSE(decodeJournalMigrationIntent(invalid, decoded));
  intent.count = UINT32_MAX;
  const auto original = bytes;
  EXPECT_FALSE(encodeJournalMigrationIntent(intent, bytes));
  EXPECT_EQ(bytes, original);
}

TEST(CompanionTintaJournal, NativeLegacyDecoderStreamsSharedFixtureAndPreservesInvalidOutput) {
  std::string path = JOURNAL_EXPORT_PAGE_FIXTURE;
  path.resize(path.find_last_of('/') + 1);
  std::ifstream input(path + "LegacyTintaJournal-v1.fixture", std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  ASSERT_EQ(bytes.size(), 48u);
  LegacyTintaJournalDecoder decoder;
  LegacyTintaEntry entry;
  ASSERT_EQ(decoder.next(std::span(bytes).first(12), entry), LegacyTintaDecodeResult::Record);
  EXPECT_EQ(entry.uid, 5u);
  EXPECT_EQ(entry.timestamp, 7200u);
  EXPECT_EQ(entry.studyDay, 1u);
  EXPECT_EQ(entry.grade, 3);
  EXPECT_EQ(entry.format, 2);
  EXPECT_EQ(entry.responseQuarterSeconds, 4);
  ASSERT_EQ(decoder.next(std::span(bytes).subspan(12, 12), entry), LegacyTintaDecodeResult::Record);
  EXPECT_EQ(entry.operation, LegacyTintaOperation::Undo);
  EXPECT_EQ(entry.undoRecord, 0u);
  const auto preserved = entry;
  EXPECT_EQ(decoder.next(std::span(bytes).subspan(12, 12), entry), LegacyTintaDecodeResult::Invalid);
  EXPECT_EQ(entry.timestamp, preserved.timestamp);
  EXPECT_EQ(decoder.count(), 2u);
  ASSERT_EQ(decoder.next(std::span(bytes).subspan(24, 12), entry), LegacyTintaDecodeResult::Record);
  EXPECT_EQ(entry.operation, LegacyTintaOperation::Flags);
  EXPECT_EQ(entry.flags, 2);
  EXPECT_EQ(decoder.next(std::span(bytes).subspan(36), entry), LegacyTintaDecodeResult::ZeroTail);
  EXPECT_EQ(decoder.next(std::span(bytes).first(12), entry), LegacyTintaDecodeResult::Invalid);
  EXPECT_EQ(decoder.count(), 3u);
}

TEST(CompanionTintaJournal, LegacyDecoderRejectsInvalidRecordsWithoutConsumingStateAndBoundsZeroTail) {
  std::array<uint8_t, 12> record{5, 0, 0, 0, 0, 0, 0, 0, 1, 0, 19, 4};
  for (const auto invalidCode : {uint8_t{5}, uint8_t{83}, uint8_t{8}, uint8_t{24}}) {
    LegacyTintaJournalDecoder decoder;
    LegacyTintaEntry output;
    output.uid = 99;
    const auto preserved = output;
    auto invalid = record;
    invalid[10] = invalidCode;
    EXPECT_EQ(decoder.next(invalid, output), LegacyTintaDecodeResult::Invalid);
    EXPECT_EQ(output, preserved);
    EXPECT_EQ(decoder.count(), 0u);
  }
  LegacyTintaJournalDecoder decoder;
  LegacyTintaEntry output;
  for (uint32_t at = 0; at < LegacyTintaJournalDecoder::MAX_BYTES / 12; ++at)
    ASSERT_EQ(decoder.next(record, output), LegacyTintaDecodeResult::Record);
  std::array<uint8_t, 4> zero{};
  ASSERT_EQ(decoder.next(zero, output), LegacyTintaDecodeResult::ZeroTail);
  const auto preserved = output;
  EXPECT_EQ(decoder.next(std::span(zero).first(1), output), LegacyTintaDecodeResult::Exhausted);
  EXPECT_EQ(output, preserved);
}

TEST(CompanionTintaJournal, LegacyBackupManifestMatchesSharedFixtureAndRejectsMalformedMetadata) {
  LegacyTintaBackupManifest manifest;
  manifest.reader.fill(1);
  manifest.generation.fill(2);
  manifest.course.fill(3);
  manifest.transaction.fill(4);
  for (size_t at : {size_t{0}, size_t{1}, size_t{2}, size_t{7}, size_t{8}}) {
    manifest.files[at].present = true;
    manifest.files[at].length = at + 12;
    manifest.files[at].hash.fill(static_cast<uint8_t>(at + 5));
  }
  std::array<uint8_t, LEGACY_TINTA_BACKUP_MANIFEST_SIZE> bytes{};
  ASSERT_TRUE(encodeLegacyTintaBackupManifest(manifest, bytes));
  std::string path = JOURNAL_EXPORT_PAGE_FIXTURE;
  path.resize(path.find_last_of('/') + 1);
  std::ifstream input(path + "LegacyReaderBackupManifest-v1.fixture", std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> expected{std::istreambuf_iterator<char>(input), {}};
  ASSERT_EQ(expected.size(), bytes.size());
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), bytes.begin()));
  LegacyTintaBackupManifestView view;
  ASSERT_TRUE(view.decode(bytes));
  EXPECT_TRUE(view.present(LegacyTintaBackupRole::Days));
  EXPECT_EQ(view.length(LegacyTintaBackupRole::Session), 20u);
  EXPECT_FALSE(view.present(LegacyTintaBackupRole::Usage));
  EXPECT_EQ(view.transaction()[0], 4);
  for (size_t offset : {size_t{0}, size_t{3}, size_t{68}, size_t{69}, size_t{70}, size_t{192}}) {
    auto corrupt = bytes;
    corrupt[offset] ^= 0x80;
    tinta_body_detail::write(corrupt, 432, binary_record::crc32(corrupt.data(), 432), 4);
    EXPECT_FALSE(view.decode(corrupt));
    EXPECT_TRUE(view.reader().empty());
  }
  auto oversized = bytes;
  tinta_body_detail::write(oversized, 72, 16 * 1024 * 1024 + 1, 8);
  tinta_body_detail::write(oversized, 432, binary_record::crc32(oversized.data(), 432), 4);
  EXPECT_FALSE(view.decode(oversized));
  manifest.files[0].present = false;
  const auto preserved = bytes;
  EXPECT_FALSE(encodeLegacyTintaBackupManifest(manifest, bytes));
  EXPECT_EQ(bytes, preserved);
}

TEST(CompanionTintaJournal, ProgressBindingRecordsExactReviewAndReportsUnavailableWriter) {
  Fixture f;
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  TintaBody review;
  ASSERT_TRUE(decodeTintaBody(f.body(), review));
  TintaProgressJournal adapter(writer, review.course, f.event.resource);
  TintaJournalResult error = TintaJournalResult::Ok;
  const auto hook = adapter.binding(
      review.configuration, ClockQuality::Unknown, &error,
      [](void* context, TintaJournalResult result) { *static_cast<TintaJournalResult*>(context) = result; });
  auto before = tinta::core::ItemState::fresh(review.uid);
  auto after = before;
  const auto entry = tinta::core::JournalEntry::review(review.uid, tinta::core::Grade::Good, review.format, 1234, 1, 0);
  ASSERT_TRUE(hook.persist(hook.context, entry, before, after, 1234));
  ASSERT_EQ(f.journal.read(0), TintaJournalResult::Ok);
  TintaBody recorded;
  ASSERT_TRUE(decodeTintaBody(f.journal.body(), recorded));
  EXPECT_EQ(recorded.responseMilliseconds, 1234u);
  EXPECT_EQ(adapter.undoIdentity(), f.journal.event().identity);
  f.storage.partialHeader = 0;
  EXPECT_FALSE(hook.persist(hook.context, entry, before, after, 1234));
  EXPECT_EQ(error, TintaJournalResult::IoError);
  EXPECT_FALSE(hook.persist(hook.context, entry, before, after, 1234));
  EXPECT_EQ(error, TintaJournalResult::Unavailable);
}

TEST(CompanionTintaJournal, DynamicProgressBindingRefreshesConfigurationAndRejectsProviderFailureBeforeWrite) {
  Fixture f;
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  TintaBody review;
  ASSERT_TRUE(decodeTintaBody(f.body(), review));
  TintaProgressJournal adapter(writer, review.course, f.event.resource);
  struct Configuration {
    TintaSchedulerConfiguration scheduler;
    ClockQuality quality = ClockQuality::Unknown;
    bool available = true;
  } configuration;
  TintaJournalResult error = TintaJournalResult::Ok;
  const auto hook = adapter.binding(
      &configuration,
      [](void* context, TintaSchedulerConfiguration& output, ClockQuality& quality) {
        auto& current = *static_cast<Configuration*>(context);
        output = current.scheduler;
        quality = current.quality;
        return current.available;
      },
      &error, [](void* context, TintaJournalResult result) { *static_cast<TintaJournalResult*>(context) = result; });
  auto before = tinta::core::ItemState::fresh(review.uid);
  auto after = before;
  const auto entry = tinta::core::JournalEntry::review(review.uid, tinta::core::Grade::Good, review.format, 1234, 1, 0);
  ASSERT_TRUE(hook.persist(hook.context, entry, before, after, 1234));
  configuration.scheduler = {8500, 120};
  ASSERT_TRUE(hook.persist(hook.context, entry, before, after, 1234));
  ASSERT_EQ(f.journal.read(1), TintaJournalResult::Ok);
  TintaBody recorded;
  ASSERT_TRUE(decodeTintaBody(f.journal.body(), recorded));
  EXPECT_EQ(recorded.configuration, configuration.scheduler);
  const auto persisted = f.storage.data;
  const auto headers = f.storage.headers;
  configuration.available = false;
  EXPECT_FALSE(hook.persist(hook.context, entry, before, after, 1234));
  EXPECT_EQ(error, TintaJournalResult::Invalid);
  EXPECT_EQ(f.storage.data, persisted);
  EXPECT_EQ(f.storage.headers, headers);
  EXPECT_EQ(f.journal.count(), 2u);
  configuration.available = true;
  configuration.scheduler.retentionBasisPoints = 0;
  EXPECT_FALSE(hook.persist(hook.context, entry, before, after, 1234));
  EXPECT_EQ(f.storage.data, persisted);
  EXPECT_EQ(f.storage.headers, headers);
  const auto fixed = adapter.binding(
      review.configuration, ClockQuality::Unknown, &error,
      [](void* context, TintaJournalResult result) { *static_cast<TintaJournalResult*>(context) = result; });
  ASSERT_TRUE(fixed.persist(fixed.context, entry, before, after, 1234));
  EXPECT_EQ(f.journal.count(), 3u);
}

TEST(CompanionTintaJournal, TimestampNormalizationHandlesOffsetsAndPreservesOutputOnInvalidOffset) {
  uint64_t timestamp = 99;
  ASSERT_TRUE(tintaLocalSecondsToUnixUtc(7200, 3600, timestamp));
  EXPECT_EQ(timestamp, 1704070800u);
  ASSERT_TRUE(tintaLocalSecondsToUnixUtc(0, 3600, timestamp));
  EXPECT_EQ(timestamp, 1704063600u);
  ASSERT_TRUE(tintaLocalSecondsToUnixUtc(UINT32_MAX, -3600, timestamp));
  EXPECT_EQ(timestamp, uint64_t{1704067200} + UINT32_MAX + 3600);
  const auto preserved = timestamp;
  EXPECT_FALSE(tintaLocalSecondsToUnixUtc(0, 86401, timestamp));
  EXPECT_EQ(timestamp, preserved);
  EXPECT_FALSE(tintaLocalSecondsToUnixUtc(0, -86401, timestamp));
  EXPECT_EQ(timestamp, preserved);
}

TEST(CompanionTintaJournal, TrustedProgressTimestampRequiresExplicitNormalizationBeforeWriting) {
  Fixture f;
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  TintaBody review;
  ASSERT_TRUE(decodeTintaBody(f.body(), review));
  TintaProgressJournal adapter(writer, review.course, f.event.resource);
  auto before = tinta::core::ItemState::fresh(review.uid);
  auto after = before;
  const auto entry =
      tinta::core::JournalEntry::review(review.uid, tinta::core::Grade::Good, review.format, 1234, 1, 7200);
  const auto initial = f.storage.data;
  EXPECT_EQ(adapter.persist(entry, before, after, 1234, {}, ClockQuality::Trusted), TintaJournalResult::Invalid);
  EXPECT_EQ(f.storage.data, initial);
  TintaJournalResult error = TintaJournalResult::Ok;
  int32_t offset = 3600;
  const auto hook = adapter.binding(
      &offset,
      [](void*, TintaSchedulerConfiguration& configuration, ClockQuality& quality) {
        configuration = {};
        quality = ClockQuality::Trusted;
        return true;
      },
      &error, [](void* context, TintaJournalResult result) { *static_cast<TintaJournalResult*>(context) = result; },
      [](void* context, const tinta::core::JournalEntry& entry, ClockQuality, uint64_t& output) {
        return tintaLocalSecondsToUnixUtc(entry.time, *static_cast<int32_t*>(context), output);
      });
  ASSERT_TRUE(hook.persist(hook.context, entry, before, after, 1234));
  ASSERT_EQ(f.journal.read(0), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.event().timestamp, 1704070800u);
  EXPECT_EQ(f.journal.event().clockQuality, ClockQuality::Trusted);
  const auto persisted = f.storage.data;
  offset = 86401;
  EXPECT_FALSE(hook.persist(hook.context, entry, before, after, 1234));
  EXPECT_EQ(error, TintaJournalResult::Invalid);
  EXPECT_EQ(f.storage.data, persisted);
  EXPECT_EQ(f.journal.count(), 1u);
}

TEST(CompanionTintaJournal, ProductionBindingSupportsReviewUndoAndAtomicFlagsBeforeLocalState) {
  for (const bool fail : {false, true}) {
    SCOPED_TRACE(fail);
    Fixture f;
    Identities identities;
    TintaWriter writer(f.journal, f.storage);
    ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
    Identity course{};
    course.fill(7);
    TintaProgressJournal adapter(writer, course, f.event.resource);
    tinta_test::MemStore local;
    const auto catalog = tinta_test::FakeCatalog::vocab(1, 1);
    tinta::core::Fsrs fsrs;
    tinta::core::ProgressStore progress(local, catalog, fsrs);
    std::array<uint16_t, 2> slots{};
    ASSERT_EQ(progress.open(slots.data(), slots.size(), nullptr, 0), tinta::core::ProgressStore::OpenResult::Created);
    TintaJournalResult reported = TintaJournalResult::Ok;
    progress.setMutationJournal(adapter.binding(
        {}, ClockQuality::Unknown, &reported,
        [](void* context, TintaJournalResult result) { *static_cast<TintaJournalResult*>(context) = result; }));
    const auto original = local.files;
    if (fail) f.storage.partialHeader = 0;
    const auto result = progress.review(0, tinta::core::Grade::Good, 0, 1234, 5, 0);
    if (fail) {
      EXPECT_EQ(result.status, tinta::core::ProgressStore::Status::Failed);
      EXPECT_EQ(reported, TintaJournalResult::IoError);
      EXPECT_EQ(local.files, original);
      EXPECT_TRUE(progress.failed());
      ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
      EXPECT_EQ(f.journal.count(), 0u);
      continue;
    }
    ASSERT_EQ(result.status, tinta::core::ProgressStore::Status::Stored);
    const auto target = adapter.undoIdentity();
    ASSERT_EQ(progress.undo(5, 0), tinta::core::ProgressStore::Status::Stored);
    ASSERT_EQ(f.journal.read(1), TintaJournalResult::Ok);
    TintaBody body;
    ASSERT_TRUE(decodeTintaBody(f.journal.body(), body));
    EXPECT_EQ(body.kind, EventKind::UndoReview);
    EXPECT_EQ(body.undoTarget, target);
    EXPECT_EQ(adapter.undoIdentity(), EventIdentity{});
    ASSERT_EQ(progress.setFlags(0, tinta::core::item_flag::kSuspended | tinta::core::item_flag::kStarred, 5, 0),
              tinta::core::ProgressStore::Status::Stored);
    EXPECT_EQ(f.journal.count(), 4u);
    EXPECT_EQ(progress.journalCount(), 3u);
    ASSERT_EQ(f.journal.read(2), TintaJournalResult::Ok);
    ASSERT_TRUE(decodeTintaBody(f.journal.body(), body));
    EXPECT_EQ(body.kind, EventKind::Suspension);
    EXPECT_TRUE(body.enabled);
    ASSERT_EQ(f.journal.read(3), TintaJournalResult::Ok);
    ASSERT_TRUE(decodeTintaBody(f.journal.body(), body));
    EXPECT_EQ(body.kind, EventKind::Star);
    EXPECT_TRUE(body.enabled);
  }
}

TEST(CompanionTintaJournal, ReplayOrderSelectsReadyEventsAndIgnoresUntrustedClocks) {
  class Index final : public JournalIdentityIndex {
   public:
    std::vector<EventIdentity> ids;
    JournalIdentityLookup find(const EventIdentity& id, uint32_t& record) override {
      for (uint32_t at = 0; at < ids.size(); ++at)
        if (ids[at] == id) {
          record = at;
          return JournalIdentityLookup::Found;
        }
      return JournalIdentityLookup::Missing;
    }
  } index;
  class Visits final : public JournalReplayVisits {
   public:
    std::vector<bool> done;
    bool failMark = false;
    bool reset(uint32_t count) override {
      done.assign(count, false);
      return true;
    }
    bool visited(uint32_t at, bool& value) override {
      if (at >= done.size()) return false;
      value = done[at];
      return true;
    }
    bool mark(uint32_t at) override {
      if (failMark) return false;
      done[at] = true;
      return true;
    }
  } visits;
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  index.ids.reserve(4);
  for (unsigned at = 0; at < 3; ++at) {
    auto event = f.event;
    event.identity.origin.fill(at + 1);
    event.identity.sequence = 1;
    event.ancestorCount = 0;
    event.studyDay = at == 2 ? 0 : 5;
    event.timestamp = at == 0 ? 1 : 999999;
    event.clockQuality = at == 0 ? ClockQuality::Trusted : ClockQuality::Device;
    if (at == 2) {
      event.ancestorCount = 1;
      event.ancestors[0] = index.ids[0];
    }
    ASSERT_EQ(f.journal.append(event, f.body()), TintaJournalResult::Ok);
    index.ids.push_back(event.identity);
  }
  JournalCausalRelation relation;
  bool related = false;
  ASSERT_EQ(relation.precedes(f.journal, index, visits, 0, 2, related), TintaJournalResult::Ok);
  EXPECT_TRUE(related);
  ASSERT_EQ(relation.precedes(f.journal, index, visits, 1, 2, related), TintaJournalResult::Ok);
  EXPECT_FALSE(related);
  related = true;
  ASSERT_EQ(relation.precedes(f.journal, index, visits, 2, 0, related), TintaJournalResult::Ok);
  EXPECT_FALSE(related);
  ASSERT_EQ(relation.precedes(f.journal, index, visits, 1, 1, related), TintaJournalResult::Ok);
  EXPECT_FALSE(related);
  related = true;
  EXPECT_EQ(relation.precedes(f.journal, index, visits, 0, 3, related), TintaJournalResult::Invalid);
  EXPECT_TRUE(related);
  visits.failMark = true;
  EXPECT_EQ(relation.precedes(f.journal, index, visits, 0, 2, related), TintaJournalResult::IoError);
  EXPECT_TRUE(related);
  visits.failMark = false;
  JournalReplayOrder order;
  ASSERT_EQ(order.begin(f.journal, index, visits), TintaJournalResult::Ok);
  uint32_t record = 99;
  bool complete = true;
  for (uint32_t expected : {1u, 0u, 2u}) {
    ASSERT_EQ(order.next(record, complete), TintaJournalResult::Ok);
    EXPECT_FALSE(complete);
    EXPECT_EQ(record, expected);
  }
  ASSERT_EQ(order.next(record, complete), TintaJournalResult::Ok);
  EXPECT_TRUE(complete);
  auto descendant = f.event;
  descendant.identity = index.ids[2];
  descendant.identity.sequence = 2;
  descendant.ancestorCount = 0;
  ASSERT_EQ(f.journal.append(descendant, f.body()), TintaJournalResult::Ok);
  index.ids.push_back(descendant.identity);
  ASSERT_EQ(relation.precedes(f.journal, index, visits, 0, 3, related), TintaJournalResult::Ok);
  EXPECT_TRUE(related);
  ASSERT_EQ(relation.precedes(f.journal, index, visits, 2, 3, related), TintaJournalResult::Ok);
  EXPECT_TRUE(related);
  ASSERT_EQ(relation.precedes(f.journal, index, visits, 1, 3, related), TintaJournalResult::Ok);
  EXPECT_FALSE(related);
  ASSERT_EQ(order.begin(f.journal, index, visits), TintaJournalResult::Ok);
  visits.failMark = true;
  record = 99;
  complete = true;
  EXPECT_EQ(order.next(record, complete), TintaJournalResult::IoError);
  EXPECT_EQ(record, 99u);
  EXPECT_TRUE(complete);
  EXPECT_EQ(order.next(record, complete), TintaJournalResult::Unavailable);
}

TEST(CompanionTintaJournal, ReplayReducerPreservesUndoneItemsAndCountsOnlyAppliedReviews) {
  class Store final : public TintaReplayStore {
   public:
    std::map<uint32_t, tinta::core::ItemState> items;
    TintaReplayDay totals;
    bool failDay = false;
    bool item(uint32_t uid, tinta::core::ItemState& value) override {
      value = items.contains(uid) ? items.at(uid) : tinta::core::ItemState::fresh(uid);
      return true;
    }
    bool putItem(const tinta::core::ItemState& value) override {
      items[value.uid] = value;
      return true;
    }
    bool day(uint16_t, TintaReplayDay& value) override {
      value = totals;
      return true;
    }
    bool putDay(uint16_t, const TintaReplayDay& value) override {
      if (failDay) return false;
      totals = value;
      return true;
    }
    bool completion(EventKind, uint32_t, bool) override { return true; }
  } store;
  Fixture f;
  TintaBody body;
  ASSERT_TRUE(decodeTintaBody(f.body(), body));
  TintaReplayReducer reducer(store, body.course);
  ASSERT_EQ(reducer.apply(f.event, f.body(), true), TintaJournalResult::Ok);
  ASSERT_EQ(store.items.size(), 1u);
  EXPECT_TRUE(store.items.at(body.uid).isNew());
  EXPECT_EQ(store.totals.gradedReviews, 0u);
  ASSERT_EQ(reducer.apply(f.event, f.body()), TintaJournalResult::Ok);
  EXPECT_EQ(store.totals.gradedReviews, 1u);
  EXPECT_EQ(store.totals.newItems, 1u);
  EXPECT_EQ(store.totals.responseMilliseconds, body.responseMilliseconds);
  tinta::core::Fsrs scheduler;
  auto expected = tinta::core::ItemState::fresh(body.uid);
  TintaReplayCounts counts;
  ASSERT_TRUE(applyTintaItemReplay(body, f.event.studyDay, scheduler, expected, counts));
  EXPECT_EQ(store.items.at(body.uid), expected);
  const auto previous = store.totals;
  store.failDay = true;
  EXPECT_EQ(reducer.apply(f.event, f.body()), TintaJournalResult::IoError);
  EXPECT_EQ(store.totals.gradedReviews, previous.gradedReviews);
  EXPECT_EQ(reducer.apply(f.event, f.body()), TintaJournalResult::Unavailable);
}

TEST(CompanionTintaJournalTest, LegacyBackupPathsBindAllIdentitiesAndSeparateCandidates) {
  Identity reader{}, generation{}, course{}, transaction{};
  reader[0] = 1;
  generation[0] = 2;
  course[0] = 3;
  transaction[0] = 4;
  std::array<char, LEGACY_TINTA_BACKUP_PATH_SIZE> path{};
  ASSERT_TRUE(
      legacyTintaBackupFilePath(reader, generation, course, transaction, LegacyTintaBackupRole::Reviews, false, path));
  const std::string original(path.data());
  EXPECT_TRUE(original.ends_with("/reviews.log"));
  ASSERT_TRUE(
      legacyTintaBackupFilePath(reader, generation, course, transaction, LegacyTintaBackupRole::Reviews, true, path));
  EXPECT_EQ(std::string(path.data()), original + "-next");
  for (auto* identity : {&reader, &generation, &course, &transaction}) {
    (*identity)[0] ^= 0x10;
    ASSERT_TRUE(legacyTintaBackupFilePath(reader, generation, course, transaction, LegacyTintaBackupRole::Reviews,
                                          false, path));
    EXPECT_NE(std::string(path.data()), original);
    (*identity)[0] ^= 0x10;
  }
  ASSERT_TRUE(legacyTintaBackupMetadataPath(reader, generation, course, transaction,
                                            LegacyTintaBackupMetadata::IntentStage, path));
  EXPECT_TRUE(std::string(path.data()).ends_with("/intent-next"));
  EXPECT_FALSE(legacyTintaBackupFilePath(reader, generation, course, transaction,
                                         static_cast<LegacyTintaBackupRole>(99), false, path));
  EXPECT_EQ(path[0], 0);
  EXPECT_FALSE(legacyTintaBackupDirectory(reader, generation, course, transaction, std::span<char>(path).first(8)));
  EXPECT_EQ(path[0], 0);
  transaction = {};
  EXPECT_FALSE(legacyTintaBackupDirectory(reader, generation, course, transaction, path));
  EXPECT_EQ(path[0], 0);
}

TEST(CompanionTintaJournalTest, LegacyBackupRequestBoundsAndReservedFieldsPreserveOutput) {
  LegacyBackupRequest request;
  request.course[0] = 1;
  request.transaction[0] = 2;
  request.generation[0] = 3;
  request.operation = LegacyBackupOperation::File;
  request.role = static_cast<uint8_t>(LegacyTintaBackupRole::Reviews);
  request.offset = 0x12345678;
  request.count = 768;
  request.bound = true;
  std::array<uint8_t, LEGACY_BACKUP_REQUEST_SIZE> bytes{};
  ASSERT_TRUE(encodeLegacyBackupRequest(request, bytes));
  std::string fixturePath = TINTA_FRONTIER_FIXTURE;
  fixturePath.resize(fixturePath.find_last_of('/') + 1);
  std::ifstream fixture(fixturePath + "LegacyBackupFileRequest-v1.fixture", std::ios::binary);
  ASSERT_TRUE(fixture.good());
  const std::vector<uint8_t> expected{std::istreambuf_iterator<char>(fixture), {}};
  EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), expected.begin(), expected.end()));
  EXPECT_EQ(bytes[56], 0x78);
  EXPECT_EQ(bytes[59], 0x12);
  LegacyBackupRequest decoded;
  ASSERT_TRUE(decodeLegacyBackupRequest(bytes, decoded));
  EXPECT_EQ(decoded.offset, request.offset);
  EXPECT_EQ(decoded.count, request.count);
  for (const size_t at : {size_t{7}, size_t{62}, size_t{63}}) {
    auto invalid = bytes;
    invalid[at] = 1;
    EXPECT_FALSE(decodeLegacyBackupRequest(invalid, decoded));
    EXPECT_EQ(decoded.offset, request.offset);
  }
  request.count = 769;
  const auto preserved = bytes;
  EXPECT_FALSE(encodeLegacyBackupRequest(request, bytes));
  EXPECT_EQ(bytes, preserved);
  request.operation = LegacyBackupOperation::Manifest;
  request.role = 0xff;
  request.count = 0;
  request.offset = 0;
  ASSERT_TRUE(encodeLegacyBackupRequest(request, bytes));
  ASSERT_TRUE(decodeLegacyBackupRequest(bytes, decoded));
  EXPECT_EQ(decoded.operation, LegacyBackupOperation::Manifest);
  bytes[6] = 2;
  EXPECT_FALSE(decodeLegacyBackupRequest(bytes, decoded));
  EXPECT_EQ(decoded.operation, LegacyBackupOperation::Manifest);
}

TEST(CompanionTintaJournalTest, LegacyBackupReplyRejectsMalformedChunksAndErrorBodies) {
  std::array<uint8_t, 768> body{};
  LegacyBackupReply reply;
  reply.result = LegacyBackupResult::Ok;
  reply.role = 0;
  reply.transaction[0] = 2;
  reply.offset = 31;
  reply.body = body;
  std::array<uint8_t, 800> bytes{};
  ASSERT_TRUE(encodeLegacyBackupReply(reply, bytes));
  std::string fixturePath = TINTA_FRONTIER_FIXTURE;
  fixturePath.resize(fixturePath.find_last_of('/') + 1);
  std::ifstream fixture(fixturePath + "LegacyBackupFileReply-v1.fixture", std::ios::binary);
  ASSERT_TRUE(fixture.good());
  const std::vector<uint8_t> expected{std::istreambuf_iterator<char>(fixture), {}};
  EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), expected.begin(), expected.end()));
  LegacyBackupReply decoded;
  ASSERT_TRUE(decodeLegacyBackupReply(bytes, decoded));
  EXPECT_EQ(decoded.offset, 31);
  EXPECT_EQ(decoded.body.size(), 768);
  for (const size_t at : {size_t{4}, size_t{5}, size_t{6}, size_t{7}, size_t{28}, size_t{30}, size_t{31}}) {
    auto corrupt = bytes;
    corrupt[at] = 0xfe;
    EXPECT_FALSE(decodeLegacyBackupReply(corrupt, decoded));
    EXPECT_EQ(decoded.offset, 31);
  }
  const auto preserved = bytes;
  reply.result = LegacyBackupResult::IoError;
  EXPECT_FALSE(encodeLegacyBackupReply(reply, bytes));
  EXPECT_EQ(bytes, preserved);
  reply.body = {};
  reply.final = false;
  std::array<uint8_t, 32> error{};
  ASSERT_TRUE(encodeLegacyBackupReply(reply, error));
  ASSERT_TRUE(decodeLegacyBackupReply(error, decoded));
  EXPECT_EQ(decoded.result, LegacyBackupResult::IoError);
}

TEST(CompanionTintaJournal, MarkBindingRecordsStableIdentityAndRejectsUnresolvedKeysBeforeWrites) {
  Fixture f;
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  TintaBody review;
  ASSERT_TRUE(decodeTintaBody(f.body(), review));
  struct Context {
    bool recover = true;
    TintaJournalResult error = TintaJournalResult::Ok;
  } context;
  TintaMarkJournal adapter(writer, review.course, f.event.resource, EventKind::ReadingComplete,
                           {&context,
                            [](void*, uint32_t key, uint32_t& uid) {
                              if (key != 7) return false;
                              uid = 123;
                              return true;
                            },
                            [](void*, uint32_t& day, uint64_t& time, ClockQuality& quality) {
                              day = 42;
                              time = 100;
                              quality = ClockQuality::Trusted;
                              return true;
                            },
                            [](void* ctx) { return static_cast<Context*>(ctx)->recover; },
                            [](void* ctx, TintaJournalResult result) { static_cast<Context*>(ctx)->error = result; }});
  const auto hook = adapter.binding();
  ASSERT_TRUE(hook.recover(hook.context));
  const auto writes = f.storage.writes;
  EXPECT_FALSE(hook.persist(hook.context, 8, true));
  EXPECT_EQ(context.error, TintaJournalResult::Invalid);
  EXPECT_EQ(f.storage.writes, writes);
  ASSERT_TRUE(hook.persist(hook.context, 7, true));
  ASSERT_EQ(f.journal.read(0), TintaJournalResult::Ok);
  TintaBody recorded;
  ASSERT_TRUE(decodeTintaBody(f.journal.body(), recorded));
  EXPECT_EQ(recorded.uid, 123u);
  EXPECT_EQ(recorded.course, review.course);
  EXPECT_EQ(recorded.kind, EventKind::ReadingComplete);
  EXPECT_TRUE(recorded.enabled);
  EXPECT_EQ(f.journal.event().studyDay, 42u);
  EXPECT_EQ(f.journal.event().timestamp, 100u);
  ASSERT_TRUE(hook.persist(hook.context, 7, false));
  ASSERT_EQ(f.journal.read(1), TintaJournalResult::Ok);
  ASSERT_TRUE(decodeTintaBody(f.journal.body(), recorded));
  EXPECT_FALSE(recorded.enabled);
  context.recover = false;
  EXPECT_FALSE(hook.recover(hook.context));
  EXPECT_EQ(context.error, TintaJournalResult::IoError);
  f.storage.partialHeader = 0;
  EXPECT_FALSE(hook.persist(hook.context, 7, true));
  EXPECT_EQ(context.error, TintaJournalResult::IoError);
  EXPECT_FALSE(hook.persist(hook.context, 7, true));
  EXPECT_EQ(context.error, TintaJournalResult::Unavailable);
}

TEST(CompanionTintaJournal, MarkRecoveryRestoresCommittedEventAfterLostLocalAppend) {
  for (const auto tear : {tinta_test::MemStore::Tear::Nothing, tinta_test::MemStore::Tear::Prefix,
                          tinta_test::MemStore::Tear::Zeros, tinta_test::MemStore::Tear::Garbage}) {
    Fixture f;
    Identities identities;
    TintaWriter writer(f.journal, f.storage);
    ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
    TintaBody review;
    ASSERT_TRUE(decodeTintaBody(f.body(), review));
    tinta_test::MemStore local;
    local.files["read.bin"] = {'T', 'M', 'K', '1'};
    struct Context {
      Fixture& fixture;
      tinta_test::MemStore& local;
      bool failRecovery = false;
      TintaJournalResult error = TintaJournalResult::Ok;
    } context{f, local};
    TintaMarkJournal adapter(
        writer, review.course, f.event.resource, EventKind::ReadingComplete,
        {&context,
         [](void*, uint32_t key, uint32_t& uid) {
           if (key != 7) return false;
           uid = 123;
           return true;
         },
         [](void*, uint32_t& day, uint64_t& time, ClockQuality& quality) {
           day = 42;
           time = 100;
           quality = ClockQuality::Trusted;
           return true;
         },
         [](void* ctx) {
           auto& owner = *static_cast<Context*>(ctx);
           if (owner.failRecovery) return false;
           bool enabled = false;
           for (uint32_t i = 0; i < owner.fixture.journal.count(); ++i) {
             if (owner.fixture.journal.read(i) != TintaJournalResult::Ok) return false;
             TintaBody body;
             if (!decodeTintaBody(owner.fixture.journal.body(), body) || body.uid != 123 ||
                 body.kind != EventKind::ReadingComplete)
               return false;
             enabled = body.enabled;
           }
           std::array<uint8_t, TINTA_NATIVE_MARK_SNAPSHOT_SIZE> scratch{};
           return restoreTintaNativeMarks(
                      owner.local, "read.bin", enabled ? 1 : 0, nullptr,
                      [](void*, uint32_t, uint32_t& identity) {
                        identity = 123;
                        return true;
                      },
                      [](void*, uint32_t identity, uint32_t& key) {
                        if (identity != 123) return false;
                        key = 7;
                        return true;
                      },
                      scratch) == TintaNativeMarkSnapshotResult::Ok;
         },
         [](void* ctx, TintaJournalResult result) { static_cast<Context*>(ctx)->error = result; }});
    {
      tinta::core::library::MarkLog marks(local, "read.bin");
      marks.setMutationJournal(adapter.binding());
      marks.open();
      ASSERT_FALSE(marks.journalFailed());
      local.cutAt(local.calls, tear);
      EXPECT_FALSE(marks.add(7));
      ASSERT_EQ(f.journal.count(), 1u);
    }
    local.powerOn();
    {
      tinta::core::library::MarkLog reopened(local, "read.bin");
      reopened.setMutationJournal(adapter.binding());
      reopened.open();
      EXPECT_FALSE(reopened.journalFailed());
      EXPECT_TRUE(reopened.contains(7));
      EXPECT_TRUE(reopened.add(7));
      EXPECT_EQ(f.journal.count(), 1u);
      EXPECT_TRUE(reopened.remove(7));
      EXPECT_EQ(f.journal.count(), 2u);
    }
    {
      tinta::core::library::MarkLog reopened(local, "read.bin");
      reopened.setMutationJournal(adapter.binding());
      reopened.open();
      EXPECT_FALSE(reopened.contains(7));
      context.failRecovery = true;
      const auto reads = local.readCalls;
      reopened.open();
      EXPECT_TRUE(reopened.journalFailed());
      EXPECT_EQ(local.readCalls, reads);
      EXPECT_FALSE(reopened.add(7));
      EXPECT_EQ(f.journal.count(), 2u);
    }
  }
}

TEST(CompanionTintaJournal, NativeMarkSnapshotRejectsUnresolvedAndCollidingKeysWithoutReplacingState) {
  tinta_test::MemStore local;
  local.files["read.bin"] = {'T', 'M', 'K', '1'};
  const auto original = local.files["read.bin"];
  std::array<uint8_t, TINTA_NATIVE_MARK_SNAPSHOT_SIZE> scratch{};
  struct Context {
    bool resolved = false;
    bool collide = false;
    bool readable = true;
  } context;
  const auto identityAt = [](void* ctx, uint32_t at, uint32_t& identity) {
    identity = 100 + at;
    return static_cast<Context*>(ctx)->readable;
  };
  const auto legacyKey = [](void* ctx, uint32_t identity, uint32_t& key) {
    auto& owner = *static_cast<Context*>(ctx);
    key = owner.collide ? 7 : identity + 1;
    return owner.resolved;
  };
  EXPECT_EQ(restoreTintaNativeMarks(local, "read.bin", 2, &context, identityAt, legacyKey, scratch),
            TintaNativeMarkSnapshotResult::Unresolved);
  context.resolved = context.collide = true;
  EXPECT_EQ(restoreTintaNativeMarks(local, "read.bin", 2, &context, identityAt, legacyKey, scratch),
            TintaNativeMarkSnapshotResult::Duplicate);
  context.readable = false;
  EXPECT_EQ(restoreTintaNativeMarks(local, "read.bin", 2, &context, identityAt, legacyKey, scratch),
            TintaNativeMarkSnapshotResult::IoError);
  EXPECT_EQ(restoreTintaNativeMarks(local, "read.bin", 97, &context, identityAt, legacyKey, scratch),
            TintaNativeMarkSnapshotResult::Invalid);
  EXPECT_EQ(local.files["read.bin"], original);
  EXPECT_EQ(local.calls, 0);
  context.readable = true;
  context.collide = false;
  ASSERT_EQ(restoreTintaNativeMarks(local, "read.bin", 96, &context, identityAt, legacyKey, scratch),
            TintaNativeMarkSnapshotResult::Ok);
  tinta::core::library::MarkLog marks(local, "read.bin");
  marks.open();
  EXPECT_EQ(marks.count(), 96);
  EXPECT_TRUE(marks.contains(101));
  EXPECT_TRUE(marks.contains(196));
  ASSERT_EQ(restoreTintaNativeMarks(local, "read.bin", 0, &context, identityAt, legacyKey, scratch),
            TintaNativeMarkSnapshotResult::Ok);
  marks.open();
  EXPECT_EQ(marks.count(), 0);
}

TEST(CompanionTintaJournal, LegacyMutationMappingPreservesReviewTimingAndRejectsUnrepresentableLeechChanges) {
  LegacyTintaEntry entry;
  entry.uid = 123;
  entry.grade = 4;
  entry.format = 9;
  entry.responseQuarterSeconds = 255;
  auto before = tinta::core::ItemState::fresh(entry.uid);
  Identity course{};
  course.fill(1);
  TintaSchedulerConfiguration configuration{8700, 730};
  TintaProgressMutation mapped;
  ASSERT_TRUE(mapLegacyTintaMutation(entry, before, course, configuration, {}, mapped));
  EXPECT_EQ(mapped.count, 1);
  EXPECT_EQ(mapped.bodies[0].kind, EventKind::Review);
  EXPECT_EQ(mapped.bodies[0].responseMilliseconds, 63750u);
  EXPECT_EQ(mapped.bodies[0].configuration, configuration);
  EXPECT_EQ(mapped.bodies[0].course, course);
  entry.operation = LegacyTintaOperation::Undo;
  EventIdentity target;
  target.origin.fill(2);
  target.epoch = 3;
  target.sequence = 4;
  ASSERT_TRUE(mapLegacyTintaMutation(entry, before, course, configuration, target, mapped));
  EXPECT_EQ(mapped.bodies[0].kind, EventKind::UndoReview);
  EXPECT_EQ(mapped.bodies[0].undoTarget, target);
  const auto unchanged = mapped.bodies;
  EXPECT_FALSE(mapLegacyTintaMutation(entry, before, course, configuration, {}, mapped));
  EXPECT_EQ(mapped.bodies, unchanged);
  for (uint8_t prior = 0; prior < 8; ++prior) {
    for (uint8_t flags = 0; flags < 8; ++flags) {
      before.flags = prior;
      entry.operation = LegacyTintaOperation::Flags;
      entry.flags = flags;
      const auto previous = mapped;
      const bool valid = mapLegacyTintaMutation(entry, before, course, configuration, {}, mapped);
      EXPECT_EQ(valid, ((prior ^ flags) & tinta::core::item_flag::kLeech) == 0);
      if (!valid) {
        EXPECT_EQ(mapped.count, previous.count);
        EXPECT_EQ(mapped.bodies, previous.bodies);
        continue;
      }
      EXPECT_EQ(mapped.count, 2);
      EXPECT_EQ(mapped.bodies[0].kind, EventKind::Suspension);
      EXPECT_EQ(mapped.bodies[1].kind, EventKind::Star);
      EXPECT_EQ(mapped.bodies[0].enabled, (flags & 1) != 0);
      EXPECT_EQ(mapped.bodies[1].enabled, (flags & 4) != 0);
    }
  }
  entry.uid = 0;
  const auto previous = mapped;
  EXPECT_FALSE(mapLegacyTintaMutation(entry, before, course, configuration, target, mapped));
  EXPECT_EQ(mapped.bodies, previous.bodies);
}

TEST(CompanionTintaJournal, StoppedWriterCannotPersistAndRestartReservesFreshEpoch) {
  Fixture f;
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  TintaBody body;
  ASSERT_TRUE(decodeTintaBody(f.body(), body));
  ASSERT_EQ(writer.record(body, f.event.resource, 1, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  const auto previous = writer.committedIdentity();
  const auto writes = f.storage.writes;
  writer.stop();
  writer.stop();
  EXPECT_FALSE(writer.available());
  EXPECT_EQ(writer.record(body, f.event.resource, 1, 0, ClockQuality::Unknown), TintaJournalResult::Unavailable);
  EXPECT_EQ(f.storage.writes, writes);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  ASSERT_EQ(writer.record(body, f.event.resource, 1, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  EXPECT_NE(writer.committedIdentity().epoch, previous.epoch);
  EXPECT_EQ(writer.committedIdentity().sequence, 1U);
  EXPECT_EQ(f.journal.count(), 2U);
}

TEST(CompanionTintaJournal, NativeProfileConfigurationPreservesExactSettingsAndRejectsInvalidValues) {
  tinta::core::Profile profile;
  TintaSchedulerConfiguration configuration;
  for (const auto retention : {uint16_t{700}, uint16_t{900}, uint16_t{970}}) {
    for (const auto maximum : {uint16_t{1}, uint16_t{365}, uint16_t{36500}}) {
      profile.retentionPermille = retention;
      profile.maxInterval = maximum;
      ASSERT_TRUE(tintaConfigurationFromProfile(profile, configuration));
      EXPECT_EQ(configuration.retentionBasisPoints, retention * 10);
      EXPECT_EQ(configuration.maximumInterval, maximum);
      std::array<uint8_t, 6> encoded{};
      EXPECT_EQ(encodeTintaConfiguration(configuration, encoded), encoded.size());
    }
  }
  const auto sentinel = configuration;
  for (unsigned fault = 0; fault < 4; ++fault) {
    profile.retentionPermille = fault == 0 ? 699 : fault == 1 ? 971 : 900;
    profile.maxInterval = fault == 2 ? 0 : fault == 3 ? 36501 : 365;
    EXPECT_FALSE(tintaConfigurationFromProfile(profile, configuration));
    EXPECT_EQ(configuration.retentionBasisPoints, sentinel.retentionBasisPoints);
    EXPECT_EQ(configuration.maximumInterval, sentinel.maximumInterval);
  }
}

TEST(CompanionTintaJournal, LessonRangePreflightsBeforeJournalAndProfileAdvancement) {
  Fixture f;
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  class Lessons final : public IdentityKeys {
   public:
    uint32_t count() const override { return 3; }
    bool read(uint32_t index, uint32_t& uid) override {
      uid = 100 + index;
      return index < 3;
    }
  } lessons;
  class Catalog final : public TintaSubjectCatalog {
   public:
    bool reject = true;
    TintaSubjectMembership contains(EventKind kind, uint32_t uid) override {
      return kind != EventKind::LessonComplete || (reject && uid == 102) ? TintaSubjectMembership::Missing
                                                                         : TintaSubjectMembership::Present;
    }
  } catalog;
  struct Context {
    TintaJournalResult error = TintaJournalResult::Ok;
  } context;
  const auto clock = [](void*, uint32_t& day, uint64_t& time, ClockQuality& quality) {
    day = 9;
    time = 0;
    quality = ClockQuality::Unknown;
    return true;
  };
  const auto recover = [](void*) { return true; };
  const auto report = [](void* ctx, TintaJournalResult result) { static_cast<Context*>(ctx)->error = result; };
  Identity course{};
  course.fill(7);
  TintaLessonJournal journal(writer, lessons, catalog, course, f.event.resource, {&context, clock, recover, report});
  tinta::core::LessonCompletion completion;
  completion.setMutationJournal(journal.binding());
  ASSERT_TRUE(completion.recover(true));
  tinta::core::Profile profile;
  const auto writes = f.storage.writes;
  EXPECT_FALSE(completion.apply(profile, 2, 3));
  EXPECT_EQ(profile.currentLesson, 0U);
  EXPECT_EQ(f.storage.writes, writes);
  EXPECT_EQ(context.error, TintaJournalResult::Invalid);
  catalog.reject = false;
  ASSERT_TRUE(completion.recover(true));
  ASSERT_TRUE(completion.apply(profile, 2, 3));
  EXPECT_EQ(profile.currentLesson, 3U);
  ASSERT_EQ(f.journal.count(), 3U);
  for (uint32_t at = 0; at < 3; ++at) {
    ASSERT_EQ(f.journal.read(at), TintaJournalResult::Ok);
    TintaBody body;
    ASSERT_TRUE(decodeTintaBody(f.journal.body(), body));
    EXPECT_EQ(body.kind, EventKind::LessonComplete);
    EXPECT_EQ(body.uid, 100 + at);
    EXPECT_TRUE(body.enabled);
  }
  const auto completedWrites = f.storage.writes;
  EXPECT_TRUE(completion.apply(profile, 2, 3));
  EXPECT_EQ(f.storage.writes, completedWrites);

  Fixture interrupted;
  Identities otherIdentities;
  TintaWriter otherWriter(interrupted.journal, interrupted.storage);
  ASSERT_EQ(otherWriter.start(otherIdentities), TintaJournalResult::Ok);
  TintaLessonJournal otherJournal(otherWriter, lessons, catalog, course, f.event.resource,
                                  {&context, clock, recover, report});
  tinta::core::LessonCompletion otherCompletion;
  otherCompletion.setMutationJournal(otherJournal.binding());
  ASSERT_TRUE(otherCompletion.recover(true));
  tinta::core::Profile previous;
  interrupted.storage.skipRecordCuts = 1;
  interrupted.storage.partialRecord = 0;
  EXPECT_FALSE(otherCompletion.apply(previous, 2, 3));
  EXPECT_EQ(previous.currentLesson, 0U);
  EXPECT_FALSE(otherWriter.available());
  ASSERT_EQ(interrupted.journal.open(), TintaJournalResult::Ok);
  EXPECT_EQ(interrupted.journal.count(), 1U);
  EXPECT_EQ(context.error, TintaJournalResult::IoError);
}

TEST(CompanionTintaJournal, GuestProgressMutationsRemainRamOnlyWithAnAvailableAuthoritativeWriter) {
  Fixture f;
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  Identity course{};
  course.fill(7);
  TintaProgressJournal journal(writer, course, f.event.resource);
  tinta_test::MemStore local;
  local.present = false;
  const auto catalog = tinta_test::FakeCatalog::vocab(1, 1);
  tinta::core::Fsrs fsrs;
  tinta::core::ProgressStore progress(local, catalog, fsrs);
  unsigned errors = 0;
  progress.setMutationJournal(
      journal.binding({}, ClockQuality::Unknown, &errors,
                      [](void* context, TintaJournalResult) { ++*static_cast<unsigned*>(context); }));
  std::array<uint16_t, 2> slots{};
  std::array<tinta::core::ItemState, 2> guest{};
  ASSERT_EQ(progress.open(slots.data(), slots.size(), guest.data(), guest.size()),
            tinta::core::ProgressStore::OpenResult::Guest);
  const auto writes = f.storage.writes;
  ASSERT_EQ(progress.review(0, tinta::core::Grade::Good, 0, 1234, 5, 0).status,
            tinta::core::ProgressStore::Status::Stored);
  ASSERT_EQ(progress.undo(5, 1), tinta::core::ProgressStore::Status::Stored);
  ASSERT_EQ(progress.setFlags(0, tinta::core::item_flag::kSuspended | tinta::core::item_flag::kStarred, 5, 2),
            tinta::core::ProgressStore::Status::Stored);
  tinta::core::ItemState state;
  ASSERT_TRUE(progress.load(0, state));
  EXPECT_TRUE(state.suspended());
  EXPECT_NE(state.flags & tinta::core::item_flag::kStarred, 0);
  EXPECT_EQ(f.storage.writes, writes);
  EXPECT_EQ(f.journal.count(), 0U);
  EXPECT_EQ(local.calls, 0);
  EXPECT_EQ(errors, 0U);
  tinta::core::library::MarkLog marks(local, "starred.bin");
  marks.setMutationJournal({&errors,
                            [](void* context, uint32_t, bool) {
                              ++*static_cast<unsigned*>(context);
                              return false;
                            },
                            nullptr});
  marks.open();
  EXPECT_FALSE(marks.add(1000));
  EXPECT_TRUE(marks.contains(1000));
  EXPECT_FALSE(marks.remove(1000));
  EXPECT_FALSE(marks.contains(1000));
  EXPECT_FALSE(marks.journalFailed());
  tinta::core::LessonCompletion completion;
  completion.setMutationJournal({&errors,
                                 [](void* context, uint16_t, uint16_t) {
                                   ++*static_cast<unsigned*>(context);
                                   return false;
                                 },
                                 nullptr});
  tinta::core::Profile profile;
  ASSERT_TRUE(completion.recover(false));
  EXPECT_TRUE(completion.apply(profile, 0, 1, false));
  EXPECT_EQ(profile.currentLesson, 1U);
  EXPECT_EQ(errors, 0U);
  EXPECT_EQ(local.calls, 0);
  EXPECT_EQ(f.storage.writes, writes);
}

TEST(CompanionTintaJournal, DerivedAcknowledgementFollowsDurableWritesAndFailureStopsFurtherMutations) {
  tinta_test::MemStore local;
  const auto catalog = tinta_test::FakeCatalog::vocab(1, 1);
  tinta::core::Fsrs fsrs;
  tinta::core::ProgressStore progress(local, catalog, fsrs);
  std::array<uint16_t, 2> slots{};
  ASSERT_EQ(progress.open(slots.data(), slots.size(), nullptr, 0), tinta::core::ProgressStore::OpenResult::Created);
  struct Context {
    tinta_test::MemStore& local;
    tinta::core::ProgressStore& progress;
    int beforeWrites = 0;
    unsigned persisted = 0, acknowledged = 0;
    bool fail = false;
  } context{local, progress};
  const auto persist = [](void* opaque, const tinta::core::JournalEntry&, const tinta::core::ItemState&,
                          const tinta::core::ItemState&, uint32_t) {
    auto& owner = *static_cast<Context*>(opaque);
    ++owner.persisted;
    owner.beforeWrites = owner.local.calls;
    return true;
  };
  const auto committed = [](void* opaque, const tinta::core::JournalEntry&, const tinta::core::ItemState&,
                            const tinta::core::ItemState& after, uint32_t) {
    auto& owner = *static_cast<Context*>(opaque);
    tinta::core::ItemState stored;
    EXPECT_TRUE(owner.progress.load(0, stored));
    EXPECT_EQ(stored, after);
    EXPECT_EQ(owner.progress.journalCount(), owner.persisted);
    EXPECT_GT(owner.local.calls, owner.beforeWrites);
    ++owner.acknowledged;
    return !owner.fail;
  };
  progress.setMutationJournal({&context, persist, nullptr, committed});
  ASSERT_EQ(progress.review(0, tinta::core::Grade::Good, 0, 1234, 5, 0).status,
            tinta::core::ProgressStore::Status::Stored);
  EXPECT_EQ(context.persisted, 1U);
  EXPECT_EQ(context.acknowledged, 1U);
  context.fail = true;
  EXPECT_EQ(progress.review(0, tinta::core::Grade::Good, 0, 1234, 6, 0).status,
            tinta::core::ProgressStore::Status::Failed);
  EXPECT_EQ(context.acknowledged, 2U);
  const auto writes = local.calls;
  EXPECT_EQ(progress.review(0, tinta::core::Grade::Good, 0, 1234, 7, 0).status,
            tinta::core::ProgressStore::Status::Failed);
  EXPECT_EQ(local.calls, writes);
  EXPECT_EQ(context.persisted, 2U);
  tinta::core::ProgressStore reopened(local, catalog, fsrs);
  ASSERT_EQ(reopened.open(slots.data(), slots.size(), nullptr, 0), tinta::core::ProgressStore::OpenResult::Opened);
  EXPECT_EQ(reopened.journalCount(), 2U);
}

TEST(CompanionTintaJournal, NoopFlagsDoNotAcknowledgeAnUnrelatedPriorEvent) {
  Fixture f;
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  Identity course{};
  course.fill(7);
  TintaProgressJournal adapter(writer, course, f.event.resource);
  auto before = tinta::core::ItemState::fresh(1);
  auto after = before;
  tinta::core::JournalEntry entry;
  entry.uid = 1;
  entry.op = 3;
  entry.day = 5;
  ASSERT_EQ(adapter.persist(entry, before, after, 1234, {}, ClockQuality::Unknown), TintaJournalResult::Ok);
  ASSERT_TRUE(adapter.pendingApplication());
  EXPECT_EQ(adapter.applicationIdentity(), writer.committedIdentity());
  adapter.didApply();
  const auto writes = f.storage.writes;
  entry.op = tinta::core::JournalEntry::kSetFlags << 3;
  entry.arg = 0;
  ASSERT_EQ(adapter.persist(entry, before, after, 0, {}, ClockQuality::Unknown), TintaJournalResult::Ok);
  EXPECT_FALSE(adapter.pendingApplication());
  EXPECT_EQ(f.storage.writes, writes);
  EXPECT_EQ(f.journal.count(), 1U);
}

TEST(CompanionTintaJournal, ApplicationReceiptRoundtripCorruptionAndBindingValidation) {
  TintaApplicationReceipt value;
  value.event.origin.fill(1);
  value.event.epoch = 2;
  value.event.sequence = 3;
  value.course.fill(4);
  value.generation.fill(5);
  value.resource.fill(6);
  value.entry = tinta::core::JournalEntry::review(1, tinta::core::Grade::Good, 0, 1234, 5, 100);
  value.before = tinta::core::ItemState::fresh(1);
  value.after = value.before;
  value.after.setPhase(tinta::core::Phase::Review);
  value.after.dueDay = 6;
  value.after.lastDay = 5;
  value.after.stability = 16;
  value.after.difficulty = 150;
  value.after.reps = 1;
  value.responseMilliseconds = 1234;
  std::array<uint8_t, TINTA_APPLICATION_RECEIPT_SIZE + 2> storage{};
  auto bytes = std::span(storage).subspan(1, TINTA_APPLICATION_RECEIPT_SIZE);
  ASSERT_EQ(encodeTintaApplicationReceipt(value, bytes), bytes.size());
  std::ifstream fixture(TINTA_APPLICATION_RECEIPT_FIXTURE, std::ios::binary);
  ASSERT_TRUE(fixture.good());
  const std::vector<uint8_t> expected{std::istreambuf_iterator<char>(fixture), {}};
  ASSERT_EQ(expected.size(), bytes.size());
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), bytes.begin()));
  TintaApplicationReceipt output;
  ASSERT_TRUE(decodeTintaApplicationReceipt(bytes, output));
  EXPECT_EQ(output.event, value.event);
  EXPECT_EQ(output.course, value.course);
  EXPECT_EQ(output.generation, value.generation);
  EXPECT_EQ(output.resource, value.resource);
  EXPECT_EQ(output.entry.uid, value.entry.uid);
  EXPECT_EQ(output.entry.time, value.entry.time);
  EXPECT_EQ(output.before, value.before);
  EXPECT_EQ(output.after, value.after);
  EXPECT_EQ(output.responseMilliseconds, 1234U);
  for (size_t at = 0; at < bytes.size(); ++at) {
    bytes[at] ^= 1;
    EXPECT_FALSE(decodeTintaApplicationReceipt(bytes, output));
    EXPECT_EQ(output.event, value.event);
    EXPECT_EQ(output.before, value.before);
    bytes[at] ^= 1;
    EXPECT_FALSE(decodeTintaApplicationReceipt(bytes.first(at), output));
    EXPECT_EQ(output.event, value.event);
  }
  auto invalid = storage;
  auto invalidBytes = std::span(invalid).subspan(1, TINTA_APPLICATION_RECEIPT_SIZE);
  std::fill_n(invalidBytes.begin() + 52, 16, 0);
  binary_record::putU32(invalidBytes.data() + 148, binary_record::crc32(invalidBytes.data(), 148));
  EXPECT_FALSE(decodeTintaApplicationReceipt(invalidBytes, output));
  EXPECT_EQ(output.generation, value.generation);
  struct Arena {
    TintaApplicationReceipt value;
    std::array<uint8_t, TINTA_APPLICATION_RECEIPT_SIZE> tail{};
  } arena{value};
  auto aliased =
      std::span<uint8_t>(reinterpret_cast<uint8_t*>(&arena), sizeof(arena)).first(TINTA_APPLICATION_RECEIPT_SIZE);
  EXPECT_EQ(encodeTintaApplicationReceipt(arena.value, aliased), 0U);
  EXPECT_EQ(arena.value.event, value.event);
  std::copy(bytes.begin(), bytes.end(), aliased.begin());
  EXPECT_FALSE(decodeTintaApplicationReceipt(aliased, arena.value));
  EXPECT_TRUE(std::equal(aliased.begin(), aliased.end(), bytes.begin()));
  value.generation.fill(0);
  bytes[0] = 42;
  EXPECT_EQ(encodeTintaApplicationReceipt(value, bytes), 0U);
  EXPECT_EQ(bytes[0], 42);
}

TEST(CompanionTintaJournal, ReviewApplicationReceiptRequiresAuthorityAndExactSchedulingTransition) {
  TintaApplicationReceipt receipt;
  receipt.event.origin.fill(1);
  receipt.event.epoch = 2;
  receipt.event.sequence = 3;
  receipt.course.fill(4);
  receipt.generation.fill(5);
  receipt.resource.fill(6);
  receipt.entry = tinta::core::JournalEntry::review(1, tinta::core::Grade::Good, 2, 1234, 5, 100);
  receipt.before = tinta::core::ItemState::fresh(1);
  receipt.after = receipt.before;
  tinta::core::Fsrs scheduler(0.86f, 700);
  tinta::core::applyReview(scheduler, receipt.after, receipt.entry.grade(), receipt.entry.day);
  receipt.responseMilliseconds = 1234;
  TintaBody body;
  body.course = receipt.course;
  body.uid = 1;
  body.grade = 3;
  body.format = 2;
  body.responseMilliseconds = 1234;
  body.configuration = {8600, 700};
  Digest bodyHash{}, configurationHash{};
  std::array<uint8_t, MAX_TINTA_BODY_SIZE> bodyBytes{};
  std::array<uint8_t, 6> configurationBytes{};
  const auto bodySize = encodeTintaBody(body, bodyBytes);
  ASSERT_EQ(bodySize, 34U);
  ASSERT_EQ(encodeTintaConfiguration(body.configuration, configurationBytes), configurationBytes.size());
  ASSERT_NE(SHA256(bodyBytes.data(), bodySize, bodyHash.data()), nullptr);
  ASSERT_NE(SHA256(configurationBytes.data(), configurationBytes.size(), configurationHash.data()), nullptr);
  SyncEvent event;
  event.identity = receipt.event;
  event.storageGeneration = receipt.generation;
  event.resource = receipt.resource;
  event.studyDay = 5;
  event.kind = EventKind::Review;
  event.schedulerVersion = 1;
  event.bodyHash = bodyHash;
  event.schedulerConfiguration = configurationHash;
  const auto verify = [&] {
    return verifyTintaReviewApplicationReceipt(receipt, event, body, bodyHash, configurationHash);
  };
  ASSERT_TRUE(verify());
  receipt.after.stability ^= 1;
  EXPECT_FALSE(verify());
  receipt.after.stability ^= 1;
  event.storageGeneration[0] ^= 1;
  EXPECT_FALSE(verify());
  event.storageGeneration = receipt.generation;
  body.responseMilliseconds += 1;
  EXPECT_FALSE(verify());
  body.responseMilliseconds -= 1;
  bodyHash[0] ^= 1;
  EXPECT_FALSE(verify());
  bodyHash[0] ^= 1;
  configurationHash[0] ^= 1;
  EXPECT_FALSE(verify());
  configurationHash[0] ^= 1;
  body.kind = EventKind::Star;
  EXPECT_FALSE(verify());
}

TEST(CompanionTintaJournal, FlagApplicationReceiptRequiresCompleteOrderedAuthorityBatch) {
  TintaApplicationReceipt receipt;
  receipt.event = {Identity{}, 2, 4};
  receipt.event.origin.fill(1);
  receipt.course.fill(4);
  receipt.generation.fill(5);
  receipt.resource.fill(6);
  receipt.before = tinta::core::ItemState::fresh(1);
  receipt.after = receipt.before;
  receipt.after.flags = tinta::core::item_flag::kSuspended | tinta::core::item_flag::kStarred;
  receipt.entry =
      tinta::core::JournalEntry::control(1, tinta::core::JournalEntry::kSetFlags, receipt.after.flags, 5, 100);
  std::array<SyncEvent, 2> events;
  std::array<TintaBody, 2> bodies;
  std::array<Digest, 2> digests;
  for (size_t at = 0; at < 2; ++at) {
    auto& body = bodies[at];
    body.course = receipt.course;
    body.uid = 1;
    body.kind = at == 0 ? EventKind::Suspension : EventKind::Star;
    body.enabled = true;
    std::array<uint8_t, MAX_TINTA_BODY_SIZE> bytes{};
    const auto size = encodeTintaBody(body, bytes);
    ASSERT_EQ(size, 23U);
    ASSERT_NE(SHA256(bytes.data(), size, digests[at].data()), nullptr);
    auto& event = events[at];
    event.identity = receipt.event;
    event.identity.sequence = 3 + at;
    event.storageGeneration = receipt.generation;
    event.kind = body.kind;
    event.resource = receipt.resource;
    event.studyDay = 5;
    event.bodyHash = digests[at];
    if (at) {
      event.ancestorCount = 1;
      event.ancestors[0] = events[0].identity;
    }
  }
  const auto verify = [&] { return verifyTintaFlagsApplicationReceipt(receipt, events, bodies, digests); };
  ASSERT_TRUE(verify());
  receipt.after.dueDay += 1;
  EXPECT_FALSE(verify());
  receipt.after.dueDay -= 1;
  EXPECT_FALSE(verifyTintaFlagsApplicationReceipt(receipt, std::span(events).last(1), std::span(bodies).last(1),
                                                  std::span(digests).last(1)));
  events[1].ancestorCount = 0;
  EXPECT_FALSE(verify());
  events[1].ancestorCount = 1;
  events[0].identity.sequence = 2;
  EXPECT_FALSE(verify());
  events[0].identity.sequence = 3;
  bodies[0].enabled = false;
  EXPECT_FALSE(verify());
  bodies[0].enabled = true;
  events[0].storageGeneration[0] ^= 1;
  EXPECT_FALSE(verify());
  events[0].storageGeneration = receipt.generation;
  digests[1][0] ^= 1;
  EXPECT_FALSE(verify());
  digests[1][0] ^= 1;
  receipt.before.flags = tinta::core::item_flag::kSuspended;
  ASSERT_TRUE(verifyTintaFlagsApplicationReceipt(receipt, std::span(events).last(1), std::span(bodies).last(1),
                                                 std::span(digests).last(1)));
}

TEST(CompanionTintaJournal, UndoApplicationReceiptRequiresVerifiedTargetAndExactRestoration) {
  TintaApplicationReceipt target;
  target.event.origin.fill(1);
  target.event.epoch = 2;
  target.event.sequence = 3;
  target.course.fill(4);
  target.generation.fill(5);
  target.resource.fill(6);
  target.entry = tinta::core::JournalEntry::review(1, tinta::core::Grade::Good, 2, 1234, 5, 100);
  target.before = tinta::core::ItemState::fresh(1);
  target.after = target.before;
  tinta::core::Fsrs scheduler;
  tinta::core::applyReview(scheduler, target.after, target.entry.grade(), target.entry.day);
  target.responseMilliseconds = 1234;
  TintaBody review;
  review.course = target.course;
  review.uid = 1;
  review.grade = 3;
  review.format = 2;
  review.responseMilliseconds = 1234;
  std::array<uint8_t, MAX_TINTA_BODY_SIZE> bytes{};
  std::array<uint8_t, 6> configurationBytes{};
  Digest reviewHash{}, configurationHash{}, undoHash{};
  auto size = encodeTintaBody(review, bytes);
  ASSERT_EQ(size, 34U);
  ASSERT_NE(SHA256(bytes.data(), size, reviewHash.data()), nullptr);
  ASSERT_EQ(encodeTintaConfiguration(review.configuration, configurationBytes), 6U);
  ASSERT_NE(SHA256(configurationBytes.data(), configurationBytes.size(), configurationHash.data()), nullptr);
  SyncEvent reviewEvent;
  reviewEvent.identity = target.event;
  reviewEvent.storageGeneration = target.generation;
  reviewEvent.kind = EventKind::Review;
  reviewEvent.resource = target.resource;
  reviewEvent.studyDay = 5;
  reviewEvent.bodyHash = reviewHash;
  reviewEvent.schedulerVersion = 1;
  reviewEvent.schedulerConfiguration = configurationHash;
  auto receipt = target;
  receipt.event.sequence = 4;
  receipt.entry = tinta::core::JournalEntry::control(1, tinta::core::JournalEntry::kUndo, 0, 6, 200);
  receipt.before = target.after;
  receipt.after = target.before;
  receipt.responseMilliseconds = 0;
  TintaBody undo;
  undo.course = target.course;
  undo.uid = 1;
  undo.kind = EventKind::UndoReview;
  undo.undoTarget = target.event;
  size = encodeTintaBody(undo, bytes);
  ASSERT_EQ(size, 54U);
  ASSERT_NE(SHA256(bytes.data(), size, undoHash.data()), nullptr);
  SyncEvent event;
  event.identity = receipt.event;
  event.storageGeneration = receipt.generation;
  event.kind = EventKind::UndoReview;
  event.resource = receipt.resource;
  event.studyDay = 6;
  event.bodyHash = undoHash;
  event.ancestorCount = 1;
  event.ancestors[0] = target.event;
  const auto verify = [&] {
    return verifyTintaUndoApplicationReceipt(receipt, event, undo, undoHash, target, reviewEvent, review, reviewHash,
                                             configurationHash);
  };
  ASSERT_TRUE(verify());
  receipt.after.flags = tinta::core::item_flag::kStarred;
  EXPECT_FALSE(verify());
  receipt.after = target.before;
  receipt.before.stability ^= 1;
  EXPECT_FALSE(verify());
  receipt.before = target.after;
  event.ancestorCount = 0;
  EXPECT_FALSE(verify());
  event.ancestorCount = 1;
  undo.undoTarget.sequence = 2;
  EXPECT_FALSE(verify());
  undo.undoTarget = target.event;
  target.after.stability ^= 1;
  receipt.before = target.after;
  EXPECT_FALSE(verify());
  target.after.stability ^= 1;
  receipt.before = target.after;
  reviewHash[0] ^= 1;
  EXPECT_FALSE(verify());
  reviewHash[0] ^= 1;
  event.resource[0] ^= 1;
  EXPECT_FALSE(verify());
}

TEST(CompanionTintaJournal, NativeCommittedMutationProofRejectsTornAndAlteredLocalFilesWithoutWrites) {
  tinta_test::MemStore store;
  const auto catalog = tinta_test::FakeCatalog::vocab(1, 1);
  tinta::core::Fsrs scheduler;
  tinta::core::ProgressStore progress(store, catalog, scheduler);
  std::array<uint16_t, 2> slots{};
  ASSERT_EQ(progress.open(slots.data(), slots.size(), nullptr, 0), tinta::core::ProgressStore::OpenResult::Created);
  const auto result = progress.review(0, tinta::core::Grade::Good, 2, 1234, 5, 100);
  ASSERT_EQ(result.status, tinta::core::ProgressStore::Status::Stored);
  const auto entry = tinta::core::JournalEntry::review(result.before.uid, tinta::core::Grade::Good, 2, 1234, 5, 100);
  ASSERT_TRUE(progress.verifyCommittedMutation(entry, result.before, result.after));
  const auto writes = store.calls;
  const auto files = store.files;
  store.files["items.bin"][1024 + 8] ^= 1;
  EXPECT_FALSE(progress.verifyCommittedMutation(entry, result.before, result.after));
  store.files = files;
  store.files["reviews.log"][4] ^= 1;
  EXPECT_FALSE(progress.verifyCommittedMutation(entry, result.before, result.after));
  store.files = files;
  store.files["reviews.log"].push_back(0);
  EXPECT_FALSE(progress.verifyCommittedMutation(entry, result.before, result.after));
  store.files = files;
  store.files["items.bin"][76] ^= 1;
  store.files["items.bin"][512 + 76] ^= 1;
  EXPECT_FALSE(progress.verifyCommittedMutation(entry, result.before, result.after));
  store.files = files;
  EXPECT_TRUE(progress.verifyCommittedMutation(entry, result.before, result.after));
  EXPECT_EQ(store.calls, writes);
  EXPECT_EQ(store.files, files);
  tinta::core::ItemState restored;
  ASSERT_EQ(progress.undo(6, 200, &restored), tinta::core::ProgressStore::Status::Stored);
  const auto undo = tinta::core::JournalEntry::control(entry.uid, tinta::core::JournalEntry::kUndo, 0, 6, 200);
  EXPECT_TRUE(progress.verifyCommittedMutation(undo, result.after, restored));
  EXPECT_FALSE(progress.verifyCommittedMutation(entry, result.before, result.after));
  const auto beforeFlags = restored;
  ASSERT_EQ(progress.setFlags(0, tinta::core::item_flag::kStarred, 6, 300), tinta::core::ProgressStore::Status::Stored);
  ASSERT_TRUE(progress.load(0, restored));
  const auto flags = tinta::core::JournalEntry::control(entry.uid, tinta::core::JournalEntry::kSetFlags,
                                                        tinta::core::item_flag::kStarred, 6, 300);
  EXPECT_TRUE(progress.verifyCommittedMutation(flags, beforeFlags, restored));
  store.dropAtRead(store.readCalls);
  EXPECT_FALSE(progress.verifyCommittedMutation(flags, beforeFlags, restored));
  store.powerOn();
  EXPECT_TRUE(progress.verifyCommittedMutation(flags, beforeFlags, restored));
}

TEST(CompanionTintaJournal, AuthorityCheckpointBindsBaselineAndRejectsCorruptionOrTruncation) {
  TintaAuthorityCheckpoint value;
  value.manifest.fill(1);
  value.frontier.fill(2);
  value.count = 3;
  std::array<uint8_t, TINTA_AUTHORITY_CHECKPOINT_SIZE> bytes{};
  ASSERT_TRUE(encodeTintaAuthorityCheckpoint(value, bytes));
  std::ifstream fixture(TINTA_AUTHORITY_CHECKPOINT_FIXTURE, std::ios::binary);
  ASSERT_TRUE(fixture.good());
  const std::vector<uint8_t> expected{std::istreambuf_iterator<char>(fixture), {}};
  ASSERT_EQ(expected.size(), bytes.size());
  EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), expected.begin()));
  TintaAuthorityCheckpoint output;
  ASSERT_TRUE(decodeTintaAuthorityCheckpoint(bytes, output));
  EXPECT_EQ(output, value);
  for (size_t at = 0; at < bytes.size(); ++at) {
    bytes[at] ^= 1;
    EXPECT_FALSE(decodeTintaAuthorityCheckpoint(bytes, output));
    EXPECT_EQ(output, value);
    bytes[at] ^= 1;
    EXPECT_FALSE(decodeTintaAuthorityCheckpoint(std::span(bytes).first(at), output));
  }
  EXPECT_TRUE(verifyTintaAuthorityCheckpoint(value, value.manifest, value.frontier, 4, 1024, value.frontier));
  EXPECT_FALSE(verifyTintaAuthorityCheckpoint(value, value.manifest, value.frontier, 2, 1024, value.frontier));
  EXPECT_FALSE(verifyTintaAuthorityCheckpoint(value, value.manifest, value.frontier, 4, 512, value.frontier));
  auto foreign = value.manifest;
  foreign[0] ^= 1;
  EXPECT_FALSE(verifyTintaAuthorityCheckpoint(value, foreign, value.frontier, 4, 1024, value.frontier));
  foreign = value.frontier;
  foreign[0] ^= 1;
  EXPECT_FALSE(verifyTintaAuthorityCheckpoint(value, value.manifest, value.frontier, 4, 1024, foreign));
  value.recordSize = 512;
  EXPECT_TRUE(verifyTintaAuthorityCheckpoint(value, value.manifest, value.frontier, 4, 1024, value.frontier));
  value.count = UINT32_MAX;
  const auto prior = bytes;
  EXPECT_FALSE(encodeTintaAuthorityCheckpoint(value, bytes));
  EXPECT_EQ(bytes, prior);
  bytes[74] = 1;
  binary_record::putU32(bytes.data() + 76, binary_record::crc32(bytes.data(), 76));
  EXPECT_FALSE(decodeTintaAuthorityCheckpoint(bytes, output));
}

TEST(CompanionTintaJournalTest, PortableTintaPreferencesPreserveDeviceFieldsAndRejectInvalidInput) {
  tinta::core::Profile source, target;
  source.newPerDay = 200;
  source.reviewCap = 9999;
  source.retentionPermille = 970;
  source.maxInterval = 36500;
  source.sessionSize = 500;
  source.textSize = static_cast<tinta::core::TextSize>(2);
  source.uiLanguage = static_cast<tinta::core::UiLanguage>(2);
  source.showVulgar = true;
  source.typedAnswers = true;
  target.fullRefreshEvery = 17;
  target.frontlightWarmth = 73;
  target.sleepCount = 123;
  target.lastConfirmedDay = 42;
  std::array<uint8_t, 8> bytes{};
  for (uint8_t key = 32; key <= 40; ++key) {
    ASSERT_EQ(companion::encodeTintaPreference(source, key, bytes), 8U);
    ASSERT_TRUE(companion::applyTintaPreference(bytes, target));
    std::array<uint8_t, 8> reencoded{};
    ASSERT_EQ(companion::encodeTintaPreference(target, key, reencoded), 8U);
    EXPECT_EQ(reencoded, bytes);
  }
  EXPECT_EQ(target.fullRefreshEvery, 17);
  EXPECT_EQ(target.frontlightWarmth, 73);
  EXPECT_EQ(target.sleepCount, 123);
  EXPECT_EQ(target.lastConfirmedDay, 42);
  ASSERT_EQ(companion::encodeTintaPreference(source, 34, bytes), 8U);
  const std::array<uint8_t, 8> retention{1, 4, 34, 1, 0xca, 3, 0, 0};
  EXPECT_EQ(bytes, retention);
  bytes[4]++;
  EXPECT_FALSE(companion::applyTintaPreference(bytes, target));
  EXPECT_EQ(target.retentionPermille, 970);
  bytes = {1, 4, 2, 1, 16, 0, 0, 0};
  EXPECT_FALSE(companion::applyTintaPreference(bytes, target));
  EXPECT_EQ(target.textSize, source.textSize);
  bytes.fill(0x55);
  const auto unchanged = bytes;
  EXPECT_EQ(companion::encodeTintaPreference(source, 31, bytes), 0U);
  EXPECT_EQ(bytes, unchanged);
  source.retentionPermille = 971;
  EXPECT_EQ(companion::encodeTintaPreference(source, 34, bytes), 0U);
  EXPECT_EQ(bytes, unchanged);
  EXPECT_EQ(companion::encodeTintaPreference(source, 32, std::span(bytes).first(7)), 0U);
  EXPECT_EQ(bytes, unchanged);
}

TEST(CompanionTintaJournalTest, NativePreferenceResolutionRetainsConcurrencyUntilCausalResolution) {
  class Index final : public JournalIdentityIndex {
   public:
    std::vector<EventIdentity> ids;
    JournalIdentityLookup find(const EventIdentity& id, uint32_t& record) override {
      for (uint32_t at = 0; at < ids.size(); ++at)
        if (ids[at] == id) {
          record = at;
          return JournalIdentityLookup::Found;
        }
      return JournalIdentityLookup::Missing;
    }
  } index;
  class Visits final : public JournalReplayVisits {
   public:
    std::vector<bool> done;
    bool failMark = false;
    bool reset(uint32_t count) override {
      done.assign(count, false);
      return true;
    }
    bool visited(uint32_t at, bool& value) override {
      if (at >= done.size()) return false;
      value = done[at];
      return true;
    }
    bool mark(uint32_t at) override {
      if (failMark) return false;
      done[at] = true;
      return true;
    }
  } visits;
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  index.ids.reserve(4);
  tinta::core::Profile profile;
  std::array<uint8_t, 8> body{};
  auto event = f.event;
  event.kind = EventKind::Preference;
  event.resource = PREFERENCE_SCOPE;
  event.schedulerVersion = 0;
  event.schedulerConfiguration = {};
  event.identity.sequence = 1;
  event.ancestorCount = 0;
  profile.newPerDay = 20;
  ASSERT_EQ(encodeTintaPreference(profile, 32, body), 8U);
  ASSERT_TRUE(f.storage.digest(body, event.bodyHash));
  ASSERT_EQ(f.journal.append(event, body), TintaJournalResult::Ok);
  index.ids.push_back(event.identity);
  event.identity.origin.fill(9);
  profile.newPerDay = 30;
  ASSERT_EQ(encodeTintaPreference(profile, 32, body), 8U);
  ASSERT_TRUE(f.storage.digest(body, event.bodyHash));
  ASSERT_EQ(f.journal.append(event, body), TintaJournalResult::Ok);
  index.ids.push_back(event.identity);
  TintaPreferenceResolution resolution;
  EXPECT_EQ(resolution.run(f.journal, index, visits), TintaJournalResult::Conflict);
  EXPECT_EQ(resolution.conflictMask(), 1);
  EXPECT_TRUE(resolution.bodies().empty());
  event.identity.origin.fill(8);
  event.ancestorCount = 2;
  event.ancestors[0] = index.ids[0];
  event.ancestors[1] = index.ids[1];
  profile.newPerDay = 40;
  ASSERT_EQ(encodeTintaPreference(profile, 32, body), 8U);
  ASSERT_TRUE(f.storage.digest(body, event.bodyHash));
  ASSERT_EQ(f.journal.append(event, body), TintaJournalResult::Ok);
  index.ids.push_back(event.identity);
  ASSERT_EQ(resolution.run(f.journal, index, visits), TintaJournalResult::Ok);
  EXPECT_EQ(resolution.conflictMask(), 0);
  ASSERT_EQ(resolution.bodies().size(), 1U);
  PreferenceBodyView value;
  ASSERT_TRUE(decodePreferenceBody(resolution.bodies()[0], value));
  EXPECT_EQ(value.integer, 40);
  event.identity.origin.fill(7);
  event.ancestorCount = 0;
  ASSERT_EQ(f.journal.append(event, body), TintaJournalResult::Ok);
  index.ids.push_back(event.identity);
  ASSERT_EQ(resolution.run(f.journal, index, visits), TintaJournalResult::Ok);
  ASSERT_EQ(resolution.bodies().size(), 1U);
  visits.failMark = true;
  EXPECT_EQ(resolution.run(f.journal, index, visits), TintaJournalResult::IoError);
  EXPECT_TRUE(resolution.bodies().empty());
}

TEST(CompanionTintaJournalTest, PreferenceWriterSharesDurableIdentityAndCausalFrontierWithLearningMutations) {
  Fixture f;
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  tinta::core::Profile profile;
  profile.newPerDay = 20;
  std::array<uint8_t, 8> body{};
  ASSERT_EQ(encodeTintaPreference(profile, 32, body), 8U);
  ASSERT_EQ(writer.recordPreference(body, 5, 123, ClockQuality::Device), TintaJournalResult::Ok);
  const auto first = writer.committedIdentity();
  ASSERT_EQ(f.journal.read(0), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.event().kind, EventKind::Preference);
  EXPECT_EQ(f.journal.event().resource, PREFERENCE_SCOPE);
  EXPECT_EQ(f.journal.event().schedulerVersion, 0);
  EXPECT_EQ(f.journal.event().ancestorCount, 0);
  body[4] = 21;
  ASSERT_EQ(writer.recordPreference(body, 5, 124, ClockQuality::Device), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.read(1), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.event().identity.sequence, first.sequence + 1);
  EXPECT_EQ(f.journal.event().ancestors[0], first);
  EXPECT_EQ(f.journal.body()[4], 21);
  const auto second = writer.committedIdentity();
  TintaBody learning;
  learning.kind = EventKind::Star;
  learning.course.fill(4);
  learning.uid = 1;
  learning.enabled = true;
  ASSERT_EQ(writer.record(learning, f.event.resource, 5, 124, ClockQuality::Device), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.read(2), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.event().kind, EventKind::Star);
  EXPECT_EQ(f.journal.event().identity.sequence, second.sequence + 1);
  EXPECT_EQ(f.journal.event().ancestors[0], second);
  const auto committed = writer.committedIdentity();
  body[0] = 2;
  EXPECT_EQ(writer.recordPreference(body, 5, 125, ClockQuality::Device), TintaJournalResult::Invalid);
  EXPECT_EQ(f.journal.count(), 3);
  EXPECT_EQ(writer.committedIdentity(), committed);
  EXPECT_TRUE(writer.available());
  body[0] = 1;
  f.storage.partialRecord = 0;
  EXPECT_EQ(writer.recordPreference(body, 5, 126, ClockQuality::Device), TintaJournalResult::IoError);
  EXPECT_FALSE(writer.available());
}

TEST(CompanionTintaJournalTest, PreferenceCapturePreflightsChangesAndStopsAfterTornPublication) {
  Fixture f;
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  TintaPreferenceCapture capture(writer);
  tinta::core::Profile profile;
  EXPECT_EQ(capture.persist(profile, 5, 123, ClockQuality::Device), TintaJournalResult::Unavailable);
  ASSERT_TRUE(capture.initialize(profile));
  profile.fullRefreshEvery = 17;
  bool changed = true;
  ASSERT_TRUE(capture.hasChanges(profile, changed));
  EXPECT_FALSE(changed);
  ASSERT_EQ(capture.persist(profile, 5, 123, ClockQuality::Device), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.count(), 0);
  profile.newPerDay = 20;
  ASSERT_TRUE(capture.hasChanges(profile, changed));
  EXPECT_TRUE(changed);
  EXPECT_FALSE(capture.initialize(profile));
  profile.retentionPermille = 971;
  changed = false;
  EXPECT_FALSE(capture.hasChanges(profile, changed));
  EXPECT_FALSE(changed);
  EXPECT_EQ(capture.persist(profile, 5, 123, ClockQuality::Device), TintaJournalResult::Invalid);
  EXPECT_EQ(f.journal.count(), 0);
  EXPECT_TRUE(writer.available());
  profile.retentionPermille = 950;
  ASSERT_EQ(capture.persist(profile, 5, 123, ClockQuality::Device), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.count(), 2);
  ASSERT_EQ(f.journal.read(0), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.body()[2], 32);
  const auto first = f.journal.event().identity;
  ASSERT_EQ(f.journal.read(1), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.body()[2], 34);
  EXPECT_EQ(f.journal.event().ancestors[0], first);
  const auto writes = f.storage.writes;
  ASSERT_EQ(capture.persist(profile, 5, 123, ClockQuality::Device), TintaJournalResult::Ok);
  EXPECT_EQ(f.storage.writes, writes);
  profile.newPerDay = 21;
  profile.retentionPermille = 960;
  f.storage.skipRecordCuts = 1;
  f.storage.partialRecord = 0;
  EXPECT_EQ(capture.persist(profile, 5, 123, ClockQuality::Device), TintaJournalResult::IoError);
  EXPECT_FALSE(writer.available());
  EXPECT_FALSE(capture.initialize(profile));
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.count(), 3);
  EXPECT_EQ(capture.persist(profile, 5, 123, ClockQuality::Device), TintaJournalResult::Unavailable);
}

TEST(CompanionTintaJournalTest, NativeSessionSnapshotBindingDetectsChangedAuthorityAtSameLocalJournalCount) {
  std::array<uint8_t, 52> bytes{};
  std::copy_n("TSES", 4, bytes.begin());
  tinta::core::putU16(bytes.data() + 4, 3);
  bytes[6] = 1;
  bytes[7] = 1;
  tinta::core::putU16(bytes.data() + 8, 4);
  std::fill_n(bytes.begin() + 14, 32, 1);
  tinta::core::putU32(bytes.data() + 46, tinta::core::crc32(bytes.data(), 46));
  tinta::core::SessionFileView saved;
  ASSERT_TRUE(tinta::core::decodeSessionFile(bytes, 4, 10, saved));
  std::array<uint8_t, 32> current{};
  current.fill(1);
  ASSERT_EQ(saved.session.size(), 4U);
  EXPECT_EQ(tinta::core::getU32(saved.session.data()), 0U);
  EXPECT_FALSE(tinta::core::sessionSnapshotChanged(saved, current));
  current[0] = 2;
  EXPECT_TRUE(tinta::core::sessionSnapshotChanged(saved, current));
  current[0] = 1;
  EXPECT_FALSE(tinta::core::sessionSnapshotChanged(saved, current));
  for (size_t at = 0; at < 50; ++at) {
    auto corrupt = bytes;
    corrupt[at] ^= 1;
    EXPECT_FALSE(tinta::core::decodeSessionFile(corrupt, 4, 10, saved));
  }
  for (size_t length = 0; length < 50; ++length)
    EXPECT_FALSE(tinta::core::decodeSessionFile(std::span(bytes).first(length), 4, 10, saved));
  EXPECT_EQ(saved.version, 3);
  tinta::core::putU16(bytes.data() + 4, 2);
  tinta::core::putU32(bytes.data() + 14, tinta::core::crc32(bytes.data(), 14));
  ASSERT_TRUE(tinta::core::decodeSessionFile(bytes, 4, 10, saved));
  EXPECT_TRUE(tinta::core::sessionSnapshotChanged(saved, current));
}

TEST(TintaMigrationAdmission, SharedFixtureBindsBackupCoursePackAndFullMerge) {
  std::string path = JOURNAL_EXPORT_PAGE_FIXTURE;
  path.resize(path.find_last_of('/') + 1);
  std::ifstream input(path + "TintaMigrationAdmission-v1.fixture", std::ios::binary);
  const std::vector<uint8_t> fixture{std::istreambuf_iterator<char>(input), {}};
  ASSERT_EQ(fixture.size(), TINTA_MIGRATION_ADMISSION_SIZE);
  TintaMigrationAdmission expected;
  expected.merge.generation.fill(1);
  expected.merge.owner.fill(2);
  expected.merge.transaction.fill(3);
  expected.merge.previous = {0, 512, {}};
  expected.merge.previous.frontier.fill(4);
  expected.merge.merged = {1, 1024, {}};
  expected.merge.merged.frontier.fill(5);
  expected.course.fill(6);
  expected.resource.fill(7);
  expected.backupTransaction.fill(8);
  expected.reader.fill(9);
  expected.backupManifest.fill(10);
  std::array<uint8_t, TINTA_MIGRATION_ADMISSION_SIZE> encoded{};
  ASSERT_TRUE(encodeTintaMigrationAdmission(expected, encoded));
  EXPECT_TRUE(std::equal(fixture.begin(), fixture.end(), encoded.begin()));
  TintaMigrationAdmission output;
  ASSERT_TRUE(decodeTintaMigrationAdmission(fixture, output));
  EXPECT_EQ(output, expected);
  for (size_t at = 0; at < fixture.size(); ++at) {
    auto damaged = encoded;
    damaged[at] ^= 1;
    EXPECT_FALSE(decodeTintaMigrationAdmission(damaged, output));
    EXPECT_EQ(output, expected);
    EXPECT_FALSE(decodeTintaMigrationAdmission(std::span(encoded).first(at), output));
    EXPECT_EQ(output, expected);
  }
  for (const auto offset : {140U, 156U, 188U, 204U, 220U}) {
    auto damaged = encoded;
    const auto length = offset == 156 || offset == 220 ? 32 : 16;
    std::fill_n(damaged.begin() + offset, length, 0);
    tinta_body_detail::write(damaged, 252, binary_record::crc32(damaged.data(), 252), 4);
    EXPECT_FALSE(decodeTintaMigrationAdmission(damaged, output));
    EXPECT_EQ(output, expected);
  }
  auto unchanged = encoded;
  expected.merge.merged = expected.merge.previous;
  expected.merge.merged.recordSize = 1024;
  EXPECT_FALSE(encodeTintaMigrationAdmission(expected, encoded));
  EXPECT_EQ(encoded, unchanged);
  struct Overlap {
    TintaMigrationAdmission value;
    std::array<uint8_t, TINTA_MIGRATION_ADMISSION_SIZE> tail{};
  } overlap;
  overlap.value = output;
  auto aliased = std::span<uint8_t>(reinterpret_cast<uint8_t*>(&overlap), TINTA_MIGRATION_ADMISSION_SIZE);
  EXPECT_FALSE(encodeTintaMigrationAdmission(overlap.value, aliased));
  EXPECT_EQ(overlap.value, output);
  std::copy(unchanged.begin(), unchanged.end(), aliased.begin());
  const auto aliasedBefore = overlap.value;
  EXPECT_FALSE(decodeTintaMigrationAdmission(aliased, overlap.value));
  EXPECT_EQ(overlap.value, aliasedBefore);
}

TEST(CompanionTintaJournalTest, PortableMarginResolutionRetainsConcurrencyUntilCausalResolution) {
  class Index final : public JournalIdentityIndex {
   public:
    std::vector<EventIdentity> ids;
    JournalIdentityLookup find(const EventIdentity& id, uint32_t& record) override {
      for (uint32_t at = 0; at < ids.size(); ++at)
        if (ids[at] == id) {
          record = at;
          return JournalIdentityLookup::Found;
        }
      return JournalIdentityLookup::Missing;
    }
  } index;
  class Visits final : public JournalReplayVisits {
   public:
    std::vector<bool> done;
    bool failMark = false;
    bool reset(uint32_t count) override {
      done.assign(count, false);
      return true;
    }
    bool visited(uint32_t at, bool& value) override {
      if (at >= done.size()) return false;
      value = done[at];
      return true;
    }
    bool mark(uint32_t at) override {
      if (failMark) return false;
      done[at] = true;
      return true;
    }
  } visits;
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  index.ids.reserve(4);
  tinta::core::Profile profile;
  std::array<uint8_t, 8> body{};
  auto event = f.event;
  event.kind = EventKind::Preference;
  event.resource = PREFERENCE_SCOPE;
  event.schedulerVersion = 0;
  event.schedulerConfiguration = {};
  event.identity.sequence = 1;
  event.ancestorCount = 0;
  profile.newPerDay = 20;
  ASSERT_EQ(encodeTintaPreference(profile, 32, body), 8U);
  body[2] = 8;
  ASSERT_TRUE(f.storage.digest(body, event.bodyHash));
  ASSERT_EQ(f.journal.append(event, body), TintaJournalResult::Ok);
  index.ids.push_back(event.identity);
  event.identity.origin.fill(9);
  profile.newPerDay = 30;
  ASSERT_EQ(encodeTintaPreference(profile, 32, body), 8U);
  body[2] = 8;
  ASSERT_TRUE(f.storage.digest(body, event.bodyHash));
  ASSERT_EQ(f.journal.append(event, body), TintaJournalResult::Ok);
  index.ids.push_back(event.identity);
  PortablePreferenceResolution resolution;
  uint16_t missing = 0x1234;
  EXPECT_FALSE(resolution.missingReaderKeys(missing));
  EXPECT_EQ(missing, 0x1234);
  EXPECT_EQ(resolution.run(f.journal, index, visits), TintaJournalResult::Conflict);
  EXPECT_FALSE(resolution.missingReaderKeys(missing));
  EXPECT_EQ(missing, 0x1234);
  EXPECT_EQ(resolution.conflictMask(), 1U << 7);
  EXPECT_TRUE(resolution.bodies().empty());
  EXPECT_TRUE(resolution.readerBody(8).empty());
  EXPECT_TRUE(resolution.readerBody(0).empty());
  EXPECT_TRUE(resolution.readerBody(15).empty());
  event.identity.origin.fill(8);
  event.ancestorCount = 2;
  event.ancestors[0] = index.ids[0];
  event.ancestors[1] = index.ids[1];
  profile.newPerDay = 40;
  ASSERT_EQ(encodeTintaPreference(profile, 32, body), 8U);
  body[2] = 8;
  ASSERT_TRUE(f.storage.digest(body, event.bodyHash));
  ASSERT_EQ(f.journal.append(event, body), TintaJournalResult::Ok);
  index.ids.push_back(event.identity);
  ASSERT_EQ(resolution.run(f.journal, index, visits), TintaJournalResult::Ok);
  EXPECT_EQ(resolution.conflictMask(), 0);
  ASSERT_TRUE(resolution.missingReaderKeys(missing));
  EXPECT_EQ(missing, 0x3fff & ~(1U << 7));
  ASSERT_EQ(resolution.bodies().size(), 1U);
  PreferenceBodyView value;
  ASSERT_TRUE(decodePreferenceBody(resolution.bodies()[0], value));
  EXPECT_EQ(value.integer, 40);
  event.identity.origin.fill(7);
  event.ancestorCount = 0;
  ASSERT_EQ(f.journal.append(event, body), TintaJournalResult::Ok);
  index.ids.push_back(event.identity);
  ASSERT_EQ(resolution.run(f.journal, index, visits), TintaJournalResult::Ok);
  ASSERT_EQ(resolution.bodies().size(), 1U);
  visits.failMark = true;
  EXPECT_EQ(resolution.run(f.journal, index, visits), TintaJournalResult::IoError);
  const auto preserved = missing;
  EXPECT_FALSE(resolution.missingReaderKeys(missing));
  EXPECT_EQ(missing, preserved);
  EXPECT_TRUE(resolution.bodies().empty());
}

TEST(CompanionTintaJournalTest, PortableLanguageResolutionPreservesVariableLengthBodies) {
  class Index final : public JournalIdentityIndex {
   public:
    std::vector<EventIdentity> ids;
    JournalIdentityLookup find(const EventIdentity& id, uint32_t& record) override {
      for (uint32_t at = 0; at < ids.size(); ++at)
        if (ids[at] == id) {
          record = at;
          return JournalIdentityLookup::Found;
        }
      return JournalIdentityLookup::Missing;
    }
  } index;
  class Visits final : public JournalReplayVisits {
   public:
    std::vector<bool> done;
    bool failMark = false;
    bool reset(uint32_t count) override {
      done.assign(count, false);
      return true;
    }
    bool visited(uint32_t at, bool& value) override {
      if (at >= done.size()) return false;
      value = done[at];
      return true;
    }
    bool mark(uint32_t at) override {
      if (failMark) return false;
      done[at] = true;
      return true;
    }
  } visits;
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  index.ids.reserve(4);
  std::vector<uint8_t> body;
  body.reserve(69);
  auto event = f.event;
  event.kind = EventKind::Preference;
  event.resource = PREFERENCE_SCOPE;
  event.schedulerVersion = 0;
  event.schedulerConfiguration = {};
  event.identity.sequence = 1;
  event.ancestorCount = 0;
  body = {1, static_cast<uint8_t>(EventKind::Preference), 10, 2, 2, 'e', 'n'};
  ASSERT_TRUE(f.storage.digest(body, event.bodyHash));
  ASSERT_EQ(f.journal.append(event, body), TintaJournalResult::Ok);
  index.ids.push_back(event.identity);
  event.identity.origin.fill(9);
  body = {1, static_cast<uint8_t>(EventKind::Preference), 10, 2, 5, 'e', 's', '-', 'M', 'X'};
  ASSERT_TRUE(f.storage.digest(body, event.bodyHash));
  ASSERT_EQ(f.journal.append(event, body), TintaJournalResult::Ok);
  index.ids.push_back(event.identity);
  PortablePreferenceResolution resolution;
  EXPECT_EQ(resolution.run(f.journal, index, visits), TintaJournalResult::Conflict);
  EXPECT_EQ(resolution.conflictMask(), 1U << 9);
  EXPECT_TRUE(resolution.bodies().empty());
  event.identity.origin.fill(8);
  event.ancestorCount = 2;
  event.ancestors[0] = index.ids[0];
  event.ancestors[1] = index.ids[1];
  body = {1, static_cast<uint8_t>(EventKind::Preference), 10, 2, 2, 'f', 'r'};
  ASSERT_TRUE(f.storage.digest(body, event.bodyHash));
  ASSERT_EQ(f.journal.append(event, body), TintaJournalResult::Ok);
  index.ids.push_back(event.identity);
  ASSERT_EQ(resolution.run(f.journal, index, visits), TintaJournalResult::Ok);
  EXPECT_EQ(resolution.conflictMask(), 0);
  ASSERT_EQ(resolution.bodies().size(), 1U);
  PreferenceBodyView value;
  ASSERT_TRUE(decodePreferenceBody(resolution.bodies()[0], value));
  EXPECT_EQ(value.key, 10);
  ASSERT_EQ(value.text.size(), 2U);
  EXPECT_EQ(value.text[0], 'f');
  EXPECT_EQ(value.text[1], 'r');
  EXPECT_EQ(resolution.bodies()[0].size(), 7U);
  event.identity.origin.fill(7);
  event.ancestorCount = 0;
  ASSERT_EQ(f.journal.append(event, body), TintaJournalResult::Ok);
  index.ids.push_back(event.identity);
  ASSERT_EQ(resolution.run(f.journal, index, visits), TintaJournalResult::Ok);
  ASSERT_EQ(resolution.bodies().size(), 1U);
  visits.failMark = true;
  EXPECT_EQ(resolution.run(f.journal, index, visits), TintaJournalResult::IoError);
  EXPECT_TRUE(resolution.bodies().empty());
}

TEST(CompanionTintaJournalTest, PortableDictionaryResolutionPreservesMaximumLengthSelection) {
  class Index final : public JournalIdentityIndex {
   public:
    std::vector<EventIdentity> ids;
    JournalIdentityLookup find(const EventIdentity& id, uint32_t& record) override {
      for (uint32_t at = 0; at < ids.size(); ++at)
        if (ids[at] == id) {
          record = at;
          return JournalIdentityLookup::Found;
        }
      return JournalIdentityLookup::Missing;
    }
  } index;
  class Visits final : public JournalReplayVisits {
   public:
    std::vector<bool> done;
    bool failMark = false;
    bool reset(uint32_t count) override {
      done.assign(count, false);
      return true;
    }
    bool visited(uint32_t at, bool& value) override {
      if (at >= done.size()) return false;
      value = done[at];
      return true;
    }
    bool mark(uint32_t at) override {
      if (failMark) return false;
      done[at] = true;
      return true;
    }
  } visits;
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  index.ids.reserve(4);
  std::vector<uint8_t> body;
  body.reserve(69);
  const auto selection = [&](uint8_t hash, size_t length, uint8_t name) {
    body.assign(38 + length, name);
    body[0] = 1;
    body[1] = static_cast<uint8_t>(EventKind::Preference);
    body[2] = 11;
    body[3] = 3;
    body[4] = 1;
    std::fill(body.begin() + 5, body.begin() + 37, hash);
    body[37] = static_cast<uint8_t>(length);
  };
  auto event = f.event;
  event.kind = EventKind::Preference;
  event.resource = PREFERENCE_SCOPE;
  event.schedulerVersion = 0;
  event.schedulerConfiguration = {};
  event.identity.sequence = 1;
  event.ancestorCount = 0;
  selection(1, 31, 'a');
  ASSERT_TRUE(f.storage.digest(body, event.bodyHash));
  ASSERT_EQ(f.journal.append(event, body), TintaJournalResult::Ok);
  index.ids.push_back(event.identity);
  event.identity.origin.fill(9);
  selection(2, 1, 'b');
  ASSERT_TRUE(f.storage.digest(body, event.bodyHash));
  ASSERT_EQ(f.journal.append(event, body), TintaJournalResult::Ok);
  index.ids.push_back(event.identity);
  PortablePreferenceResolution resolution;
  EXPECT_EQ(resolution.run(f.journal, index, visits), TintaJournalResult::Conflict);
  EXPECT_EQ(resolution.conflictMask(), 1U << 10);
  EXPECT_TRUE(resolution.bodies().empty());
  event.identity.origin.fill(8);
  event.ancestorCount = 2;
  event.ancestors[0] = index.ids[0];
  event.ancestors[1] = index.ids[1];
  selection(3, 31, 'c');
  ASSERT_TRUE(f.storage.digest(body, event.bodyHash));
  ASSERT_EQ(f.journal.append(event, body), TintaJournalResult::Ok);
  index.ids.push_back(event.identity);
  ASSERT_EQ(resolution.run(f.journal, index, visits), TintaJournalResult::Ok);
  EXPECT_EQ(resolution.conflictMask(), 0);
  ASSERT_EQ(resolution.bodies().size(), 1U);
  PreferenceBodyView value;
  ASSERT_TRUE(decodePreferenceBody(resolution.bodies()[0], value));
  EXPECT_EQ(value.key, 11);
  ASSERT_EQ(value.text.size(), 31U);
  EXPECT_TRUE(std::all_of(value.text.begin(), value.text.end(), [](uint8_t byte) { return byte == 'c'; }));
  ASSERT_EQ(value.contentHash.size(), 32U);
  EXPECT_TRUE(std::all_of(value.contentHash.begin(), value.contentHash.end(), [](uint8_t byte) { return byte == 3; }));
  EXPECT_EQ(resolution.bodies()[0].size(), 69U);
  event.identity.origin.fill(7);
  event.ancestorCount = 0;
  ASSERT_EQ(f.journal.append(event, body), TintaJournalResult::Ok);
  index.ids.push_back(event.identity);
  ASSERT_EQ(resolution.run(f.journal, index, visits), TintaJournalResult::Ok);
  ASSERT_EQ(resolution.bodies().size(), 1U);
  visits.failMark = true;
  EXPECT_EQ(resolution.run(f.journal, index, visits), TintaJournalResult::IoError);
  EXPECT_TRUE(resolution.bodies().empty());
}

namespace {
class ReaderPlanDependencies final : public companion::ReaderPreferenceDependencies {
 public:
  bool available = true, fontAvailable = true, dictionaryAvailable = true;
  companion::Digest fontHash{}, dictionaryHash{};
  size_t fontNameLength = 0, dictionaryNameLength = 0;
  uint8_t lastPointSize = 0;
  bool language(std::span<const uint8_t>, uint8_t& value) override {
    value = 7;
    return available;
  }
  bool font(uint8_t, uint8_t pointSize, std::span<const char> name, std::span<const uint8_t> hash) override {
    lastPointSize = pointSize;
    fontNameLength = name.size();
    if (!hash.empty()) std::copy(hash.begin(), hash.end(), fontHash.begin());
    return available && fontAvailable;
  }
  bool dictionary(std::span<const char> name, std::span<const uint8_t> hash) override {
    dictionaryNameLength = name.size();
    std::copy(hash.begin(), hash.end(), dictionaryHash.begin());
    return available && dictionaryAvailable;
  }
};
}  // namespace
TEST(CompanionTintaJournalTest, ReaderPreferencePlanMapsFullAllowlistBeforeExposingValues) {
  using namespace companion;
  const std::array<int32_t, 14> values{1, 14, 3, 2, 1, 150, -2, 40, 0, 0, 0, 0, 0, 1};
  std::array<std::array<uint8_t, 8>, 14> integers{};
  std::array<std::span<const uint8_t>, 14> bodies{};
  for (size_t at = 0; at < integers.size(); ++at) {
    auto& body = integers[at];
    body[0] = 1;
    body[1] = static_cast<uint8_t>(EventKind::Preference);
    body[2] = at + 1;
    body[3] = 1;
    const auto number = std::bit_cast<uint32_t>(values[at]);
    for (unsigned byte = 0; byte < 4; ++byte) body[4 + byte] = static_cast<uint8_t>(number >> (byte * 8));
    bodies[at] = body;
  }
  const std::array<uint8_t, 7> language{1, 4, 10, 2, 2, 'e', 's'};
  const std::array<uint8_t, 5> dictionary{1, 4, 11, 3, 0};
  bodies[9] = language;
  bodies[10] = dictionary;
  ReaderPlanDependencies dependencies;
  ReaderPreferenceValues original;
  ReaderPreferencePlan plan;
  ASSERT_EQ(plan.run(bodies, original, dependencies), ReaderPreferencePlanResult::Ok);
  ASSERT_NE(plan.values(), nullptr);
  const auto& result = *plan.values();
  EXPECT_EQ(result.fontFamily, 1);
  EXPECT_EQ(result.fontPointSize, 14);
  EXPECT_EQ(dependencies.lastPointSize, 14);
  EXPECT_EQ(result.lineSpacing, 3);
  EXPECT_EQ(result.paragraphAlignment, 2);
  EXPECT_EQ(result.extraParagraphSpacing, 1);
  EXPECT_EQ(result.wordSpacing, 150);
  EXPECT_EQ(result.characterSpacing, 0);
  EXPECT_EQ(result.screenMargin, 40);
  EXPECT_EQ(result.hyphenationEnabled, 0);
  EXPECT_EQ(result.language, 7);
  EXPECT_EQ(result.textAntiAliasing, 0);
  EXPECT_EQ(result.embeddedStyle, 0);
  EXPECT_EQ(result.focusReadingEnabled, 1);
  EXPECT_EQ(result.dictionaryName[0], 0);
  EXPECT_EQ(original, ReaderPreferenceValues{});
  dependencies.available = false;
  EXPECT_EQ(plan.run(bodies, original, dependencies), ReaderPreferencePlanResult::UnavailableDependency);
  EXPECT_EQ(plan.values(), nullptr);
  dependencies.available = true;
  bodies[1] = bodies[0];
  EXPECT_EQ(plan.run(bodies, original, dependencies), ReaderPreferencePlanResult::InvalidBody);
  EXPECT_EQ(plan.values(), nullptr);
}

TEST(CompanionTintaJournalTest, ReaderPlanRequiresExactFontAndDictionaryDependenciesAndFinalPointSize) {
  using namespace companion;
  std::array<uint8_t, 69> font{}, dictionary{};
  const auto selection = [](std::array<uint8_t, 69>& body, uint8_t key, uint8_t hash, uint8_t name) {
    body.fill(name);
    body[0] = 1;
    body[1] = static_cast<uint8_t>(EventKind::Preference);
    body[2] = key;
    body[3] = 3;
    body[4] = 1;
    body[37] = 31;
    std::fill(body.begin() + 5, body.begin() + 37, hash);
  };
  selection(font, 1, 7, 'f');
  selection(dictionary, 11, 8, 'd');
  const std::array<uint8_t, 8> size{1, 4, 2, 1, 18, 0, 0, 0};
  const std::array<std::span<const uint8_t>, 3> bodies{size, dictionary, font};
  ReaderPlanDependencies dependencies;
  ReaderPreferenceValues original;
  ReaderPreferencePlan plan;
  dependencies.dictionaryAvailable = false;
  EXPECT_EQ(plan.run(bodies, original, dependencies), ReaderPreferencePlanResult::UnavailableDependency);
  EXPECT_EQ(plan.values(), nullptr);
  EXPECT_EQ(dependencies.lastPointSize, 18);
  EXPECT_EQ(dependencies.fontNameLength, 31U);
  EXPECT_EQ(dependencies.dictionaryNameLength, 31U);
  Digest expectedFont, expectedDictionary;
  expectedFont.fill(7);
  expectedDictionary.fill(8);
  EXPECT_EQ(dependencies.fontHash, expectedFont);
  EXPECT_EQ(dependencies.dictionaryHash, expectedDictionary);
  dependencies.dictionaryAvailable = true;
  dependencies.fontAvailable = false;
  EXPECT_EQ(plan.run(bodies, original, dependencies), ReaderPreferencePlanResult::UnavailableDependency);
  EXPECT_EQ(plan.values(), nullptr);
  dependencies.fontAvailable = true;
  ASSERT_EQ(plan.run(bodies, original, dependencies), ReaderPreferencePlanResult::Ok);
  ASSERT_NE(plan.values(), nullptr);
  EXPECT_EQ(plan.values()->fontPointSize, 18);
  EXPECT_EQ(plan.values()->sdFontFamilyName[30], 'f');
  EXPECT_EQ(plan.values()->sdFontFamilyName[31], 0);
  EXPECT_EQ(plan.values()->dictionaryName[30], 'd');
  EXPECT_EQ(plan.values()->dictionaryName[31], 0);
  EXPECT_EQ(original, ReaderPreferenceValues{});
}

TEST(CompanionTintaJournalTest, ReaderPreferenceApplicationRetriesFailureAndDebouncesDurableReplacement) {
  using namespace companion;
  class Store final : public ReaderPreferenceStore {
   public:
    ReaderPreferenceValues saved;
    bool readFailure = false, concurrentChange = false;
    ReaderPreferenceStoreResult result = ReaderPreferenceStoreResult::IoError;
    unsigned writes = 0;
    bool read(ReaderPreferenceValues& output) override {
      if (readFailure) return false;
      output = saved;
      return true;
    }
    ReaderPreferenceStoreResult replace(const ReaderPreferenceValues& expected,
                                        const ReaderPreferenceValues& replacement) override {
      ++writes;
      if (concurrentChange) {
        saved.screenMargin = 30;
        return ReaderPreferenceStoreResult::Conflict;
      }
      EXPECT_EQ(saved, expected);
      if (result == ReaderPreferenceStoreResult::Ok) saved = replacement;
      return result;
    }
  } store;
  const std::array<uint8_t, 8> margin{1, 4, 8, 1, 40, 0, 0, 0};
  const std::array<std::span<const uint8_t>, 1> bodies{margin};
  ReaderPlanDependencies dependencies;
  ReaderPreferenceApplication application;
  EXPECT_EQ(application.run(bodies, dependencies, store), ReaderPreferenceApplicationResult::IoError);
  EXPECT_EQ(store.saved.screenMargin, 5);
  EXPECT_EQ(store.writes, 1U);
  store.result = ReaderPreferenceStoreResult::Conflict;
  EXPECT_EQ(application.run(bodies, dependencies, store), ReaderPreferenceApplicationResult::Conflict);
  EXPECT_EQ(store.saved.screenMargin, 5);
  EXPECT_EQ(store.writes, 2U);
  store.result = ReaderPreferenceStoreResult::Ok;
  ASSERT_EQ(application.run(bodies, dependencies, store), ReaderPreferenceApplicationResult::Applied);
  EXPECT_EQ(store.saved.screenMargin, 40);
  EXPECT_EQ(store.writes, 3U);
  EXPECT_EQ(application.run(bodies, dependencies, store), ReaderPreferenceApplicationResult::Unchanged);
  EXPECT_EQ(store.writes, 3U);
  store.readFailure = true;
  EXPECT_EQ(application.run(bodies, dependencies, store), ReaderPreferenceApplicationResult::IoError);
  EXPECT_EQ(store.writes, 3U);
  store.readFailure = false;
  store.saved.screenMargin = 5;
  store.concurrentChange = true;
  EXPECT_EQ(application.run(bodies, dependencies, store), ReaderPreferenceApplicationResult::Conflict);
  EXPECT_EQ(store.saved.screenMargin, 30);
  EXPECT_EQ(store.writes, 4U);
  store.concurrentChange = false;
  EXPECT_EQ(application.run(bodies, dependencies, store), ReaderPreferenceApplicationResult::Applied);
  EXPECT_EQ(store.saved.screenMargin, 40);
  EXPECT_EQ(store.writes, 5U);
}

TEST(CompanionReaderPreferenceEncoding, NativeValuesRoundTripThroughFullPortablePlan) {
  ReaderPreferenceValues input;
  input.fontFamily = 1;
  input.fontPointSize = 16;
  input.lineSpacing = 3;
  input.wordSpacing = 175;
  input.characterSpacing = 0;
  input.screenMargin = 35;
  input.language = 7;
  input.focusReadingEnabled = 1;
  std::copy_n("dict", 5, input.dictionaryName.begin());
  Digest dictionaryHash{};
  dictionaryHash.fill(9);
  std::array<std::array<uint8_t, 69>, 14> encoded{};
  std::array<std::span<const uint8_t>, 14> bodies{};
  for (uint8_t key = 1; key <= 14; ++key) {
    const auto size = encodeReaderPreference(input, key, "pt-BR", {}, dictionaryHash, encoded[key - 1]);
    ASSERT_NE(size, 0U) << unsigned(key);
    bodies[key - 1] = std::span(encoded[key - 1]).first(size);
  }
  ReaderPlanDependencies dependencies;
  ReaderPreferencePlan plan;
  ASSERT_EQ(plan.run(bodies, {}, dependencies), ReaderPreferencePlanResult::Ok);
  EXPECT_EQ(*plan.values(), input);
  EXPECT_EQ(dependencies.dictionaryHash, dictionaryHash);
  input.sdFontFamilyName.fill('f');
  input.sdFontFamilyName.back() = 0;
  Digest fontHash{};
  fontHash.fill(4);
  const auto size = encodeReaderPreference(input, 1, "pt-BR", fontHash, dictionaryHash, encoded[0]);
  ASSERT_EQ(size, 69U);
  bodies[0] = std::span(encoded[0]).first(size);
  ASSERT_EQ(plan.run(bodies, {}, dependencies), ReaderPreferencePlanResult::Ok);
  EXPECT_EQ(plan.values()->sdFontFamilyName, input.sdFontFamilyName);
  EXPECT_EQ(dependencies.fontHash, fontHash);
}

TEST(CompanionReaderPreferenceEncoding, RejectsInvalidMetadataAndEncodesDictionaryRemoval) {
  ReaderPreferenceValues input;
  std::array<uint8_t, 69> body{};
  EXPECT_EQ(encodeReaderPreference(input, 10, "bad--tag", {}, {}, body), 0U);
  EXPECT_EQ(encodeReaderPreference(input, 0, "en", {}, {}, body), 0U);
  EXPECT_EQ(encodeReaderPreference(input, 32, "en", {}, {}, body), 0U);
  EXPECT_EQ(encodeReaderPreference(input, 8, "en", {}, {}, std::span(body).first(8)), 0U);
  input.screenMargin = 41;
  EXPECT_EQ(encodeReaderPreference(input, 8, "en", {}, {}, body), 0U);
  input.sdFontFamilyName.fill('x');
  Digest hash{};
  hash.fill(1);
  EXPECT_EQ(encodeReaderPreference(input, 1, "en", hash, {}, body), 0U);
  input.sdFontFamilyName.back() = 0;
  EXPECT_EQ(encodeReaderPreference(input, 1, "en", {}, {}, body), 0U);
  hash.fill(0);
  EXPECT_EQ(encodeReaderPreference(input, 1, "en", hash, {}, body), 0U);
  input.dictionaryName[0] = '/';
  hash.fill(1);
  EXPECT_EQ(encodeReaderPreference(input, 11, "en", {}, hash, body), 0U);
  input.dictionaryName = {};
  const auto size = encodeReaderPreference(input, 11, "en", {}, {}, body);
  ASSERT_EQ(size, 5U);
  PreferenceBodyView decoded;
  ASSERT_TRUE(decodePreferenceBody(std::span(body).first(size), decoded));
  EXPECT_FALSE(decoded.hasContent);
}

TEST(CompanionReaderPreferenceCapture, ChangesAreCausalAndTornBatchRequiresRecovery) {
  Fixture f;
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  ReaderPreferenceCapture capture(writer);
  ReaderPreferenceValues values;
  EXPECT_EQ(capture.persist(values, "en", {}, {}, 0, 0, ClockQuality::Unknown), TintaJournalResult::Unavailable);
  ASSERT_TRUE(capture.initialize(values, "en", {}, {}));
  EXPECT_FALSE(capture.initialize(values, "en", {}, {}));
  ASSERT_EQ(capture.persist(values, "en", {}, {}, 0, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.count(), 0U);
  values.screenMargin = 20;
  values.characterSpacing = 0;
  bool changed = false;
  ASSERT_TRUE(capture.hasChanges(values, "en", {}, {}, changed));
  EXPECT_TRUE(changed);
  ASSERT_EQ(capture.persist(values, "en", {}, {}, 0, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.count(), 2U);
  ASSERT_EQ(f.journal.read(0), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.body()[2], 7);
  const auto first = f.journal.event().identity;
  EXPECT_EQ(f.journal.event().resource, PREFERENCE_SCOPE);
  ASSERT_EQ(f.journal.read(1), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.body()[2], 8);
  EXPECT_EQ(f.journal.event().ancestors[0], first);
  const auto writes = f.storage.writes;
  ASSERT_EQ(capture.persist(values, "en", {}, {}, 0, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  EXPECT_EQ(f.storage.writes, writes);
  values.wordSpacing = 175;
  values.screenMargin = 41;
  EXPECT_EQ(capture.persist(values, "en", {}, {}, 0, 0, ClockQuality::Unknown), TintaJournalResult::Invalid);
  EXPECT_EQ(f.storage.writes, writes);
  values.screenMargin = 25;
  f.storage.skipRecordCuts = 1;
  f.storage.partialRecord = 0;
  EXPECT_EQ(capture.persist(values, "en", {}, {}, 0, 0, ClockQuality::Unknown), TintaJournalResult::IoError);
  EXPECT_FALSE(writer.available());
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.count(), 3U);
  EXPECT_FALSE(capture.initialize(values, "en", {}, {}));
  EXPECT_EQ(capture.persist(values, "en", {}, {}, 0, 0, ClockQuality::Unknown), TintaJournalResult::Unavailable);
}

TEST(CompanionReaderPreferenceCapture, ContentIdentityAndVariableLengthLanguageChangesAreCaptured) {
  Fixture f;
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  ReaderPreferenceCapture capture(writer);
  ReaderPreferenceValues values;
  std::copy_n("font", 5, values.sdFontFamilyName.begin());
  Digest fontHash{};
  fontHash.fill(3);
  ASSERT_TRUE(capture.initialize(values, "en", fontHash, {}));
  fontHash.fill(4);
  ASSERT_EQ(capture.persist(values, "pt-BR", fontHash, {}, 0, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.count(), 2U);
  ASSERT_EQ(f.journal.read(0), TintaJournalResult::Ok);
  PreferenceBodyView decoded;
  ASSERT_TRUE(decodePreferenceBody(f.journal.body(), decoded));
  EXPECT_EQ(decoded.key, 1);
  EXPECT_TRUE(std::equal(decoded.contentHash.begin(), decoded.contentHash.end(), fontHash.begin()));
  ASSERT_EQ(f.journal.read(1), TintaJournalResult::Ok);
  ASSERT_TRUE(decodePreferenceBody(f.journal.body(), decoded));
  EXPECT_EQ(decoded.key, 10);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(decoded.text.data()), decoded.text.size()), "pt-BR");
  const auto writes = f.storage.writes;
  EXPECT_EQ(capture.persist(values, "pt-BR", {}, {}, 0, 0, ClockQuality::Unknown), TintaJournalResult::Invalid);
  EXPECT_EQ(f.storage.writes, writes);
  ASSERT_EQ(capture.persist(values, "en", fontHash, {}, 0, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.count(), 3U);
  bool changed = true;
  ASSERT_TRUE(capture.hasChanges(values, "en", fontHash, {}, changed));
  EXPECT_FALSE(changed);
}

TEST(CompanionReaderPreferenceChangeCapture, UnavailableLegacySelectionCanBeKeptOrRemovedWithoutUnrelatedProof) {
  Fixture f;
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  ReaderPreferenceChangeCapture capture(writer);
  ReaderPreferenceValues values;
  std::copy_n("missing", 8, values.dictionaryName.begin());
  values.language = 255;
  ASSERT_TRUE(capture.initialize(values));
  const auto encode = [](void*, const ReaderPreferenceValues& values, uint8_t key, std::span<uint8_t> output) {
    return encodeReaderPreference(values, key, {}, {}, {}, output);
  };
  values.screenMargin = 20;
  ASSERT_EQ(capture.persist(values, encode, nullptr, 0, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.count(), 1U);
  ASSERT_EQ(f.journal.read(0), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.body()[2], 8);
  values.dictionaryName = {};
  ASSERT_EQ(capture.persist(values, encode, nullptr, 0, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.count(), 2U);
  ASSERT_EQ(f.journal.read(1), TintaJournalResult::Ok);
  PreferenceBodyView decoded;
  ASSERT_TRUE(decodePreferenceBody(f.journal.body(), decoded));
  EXPECT_EQ(decoded.key, 11);
  EXPECT_FALSE(decoded.hasContent);
  const auto writes = f.storage.writes;
  values.dictionaryName[5] = 'x';
  ASSERT_EQ(capture.persist(values, encode, nullptr, 0, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  EXPECT_EQ(f.storage.writes, writes);
  values.screenMargin = 25;
  values.language = 0;
  EXPECT_EQ(capture.persist(values, encode, nullptr, 0, 0, ClockQuality::Unknown), TintaJournalResult::Invalid);
  EXPECT_EQ(f.storage.writes, writes);
}

TEST(CompanionReaderPreferenceChangeCapture, SdSizeChangesIncludeFontIdentityAndPartialBatchesStopCapture) {
  ReaderPreferenceValues before, after;
  std::copy_n("font", 5, before.sdFontFamilyName.begin());
  after = before;
  after.fontPointSize = 16;
  EXPECT_EQ(ReaderPreferenceChangeCapture::changedKeys(before, after), 3U);
  Fixture f;
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  ReaderPreferenceChangeCapture capture(writer);
  ASSERT_TRUE(capture.initialize(before));
  Digest hash{};
  hash.fill(4);
  const auto encode = [](void* context, const ReaderPreferenceValues& values, uint8_t key, std::span<uint8_t> output) {
    return encodeReaderPreference(values, key, {}, *static_cast<Digest*>(context), {}, output);
  };
  f.storage.skipRecordCuts = 1;
  f.storage.partialRecord = 0;
  EXPECT_EQ(capture.persist(after, encode, &hash, 0, 0, ClockQuality::Unknown), TintaJournalResult::IoError);
  EXPECT_FALSE(writer.available());
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.count(), 1U);
  EXPECT_EQ(capture.persist(after, encode, &hash, 0, 0, ClockQuality::Unknown), TintaJournalResult::Unavailable);
}

namespace {
class KnowledgeIndex final : public JournalIdentityIndex {
 public:
  std::vector<EventIdentity> ids;
  JournalIdentityLookup find(const EventIdentity& id, uint32_t& record) override {
    for (uint32_t at = 0; at < ids.size(); ++at) {
      if (ids[at] != id) continue;
      record = at;
      return JournalIdentityLookup::Found;
    }
    return JournalIdentityLookup::Missing;
  }
};
class KnowledgeVisits final : public JournalReplayVisits {
 public:
  std::vector<bool> values;
  bool failMark = false;
  bool failRead = false;
  bool reset(uint32_t count) override {
    values.assign(count, false);
    return true;
  }
  bool visited(uint32_t at, bool& output) override {
    if (failRead || at >= values.size()) return false;
    output = values[at];
    return true;
  }
  bool mark(uint32_t at) override {
    if (failMark || at >= values.size()) return false;
    values[at] = true;
    return true;
  }
};
}  // namespace
TEST(CompanionTintaJournalTest, KnowledgeHeadsRetainEveryIndependentBranchAndImplicitPredecessor) {
  Fixture f;
  KnowledgeIndex index;
  KnowledgeVisits visits;
  JournalKnowledgeHeads heads;
  index.ids.reserve(9);
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  for (uint8_t origin = 1; origin <= 6; ++origin) {
    f.event.identity.origin.fill(origin);
    ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
    index.ids.push_back(f.event.identity);
  }
  f.event.identity.origin.fill(1);
  f.event.identity.sequence = 2;
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  index.ids.push_back(f.event.identity);
  f.event.identity.origin.fill(7);
  f.event.identity.sequence = 1;
  f.event.ancestorCount = 2;
  f.event.ancestors[0] = index.ids[1];
  f.event.ancestors[1] = index.ids[6];
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  index.ids.push_back(f.event.identity);
  ASSERT_EQ(heads.begin(f.journal, index, visits), TintaJournalResult::Ok);
  EventIdentity output;
  bool complete = true;
  for (const auto at : {2U, 3U, 4U, 5U, 7U}) {
    ASSERT_EQ(heads.next(output, complete), TintaJournalResult::Ok);
    EXPECT_FALSE(complete);
    EXPECT_EQ(output, index.ids[at]);
  }
  const auto previous = output;
  ASSERT_EQ(heads.next(output, complete), TintaJournalResult::Ok);
  EXPECT_TRUE(complete);
  EXPECT_EQ(output, previous);
}
TEST(CompanionTintaJournalTest, KnowledgeHeadsInvalidateAfterVisitFailureOrJournalGrowth) {
  Fixture f;
  KnowledgeIndex index;
  KnowledgeVisits visits;
  JournalKnowledgeHeads heads;
  index.ids.reserve(2);
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  index.ids.push_back(f.event.identity);
  ASSERT_EQ(heads.begin(f.journal, index, visits), TintaJournalResult::Ok);
  visits.failRead = true;
  EventIdentity output;
  output.origin.fill(99);
  const auto original = output;
  bool complete = false;
  EXPECT_EQ(heads.next(output, complete), TintaJournalResult::IoError);
  EXPECT_EQ(output, original);
  EXPECT_EQ(heads.next(output, complete), TintaJournalResult::Unavailable);
  visits.failRead = false;
  f.event.identity.sequence = 2;
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  index.ids.push_back(f.event.identity);
  visits.failMark = true;
  EXPECT_EQ(heads.begin(f.journal, index, visits), TintaJournalResult::IoError);
  EXPECT_EQ(heads.next(output, complete), TintaJournalResult::Unavailable);
  visits.failMark = false;
  ASSERT_EQ(heads.begin(f.journal, index, visits), TintaJournalResult::Ok);
  f.event.identity.sequence = 3;
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  EXPECT_EQ(heads.next(output, complete), TintaJournalResult::Conflict);
  EXPECT_EQ(output, original);
}
TEST(CompanionTintaJournalTest, KnowledgeHeadsHandleEmptyAuthorityAndRejectMissingPredecessor) {
  Fixture f;
  KnowledgeIndex index;
  KnowledgeVisits visits;
  JournalKnowledgeHeads heads;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(heads.begin(f.journal, index, visits), TintaJournalResult::Ok);
  EventIdentity output;
  bool complete = false;
  EXPECT_EQ(heads.next(output, complete), TintaJournalResult::Ok);
  EXPECT_TRUE(complete);
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  f.event.identity.sequence = 2;
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  index.ids.reserve(2);
  auto missing = f.event.identity;
  missing.origin.fill(9);
  missing.sequence = 1;
  index.ids.push_back(missing);
  index.ids.push_back(f.event.identity);
  EXPECT_EQ(heads.begin(f.journal, index, visits), TintaJournalResult::Corrupt);
  EXPECT_EQ(heads.next(output, complete), TintaJournalResult::Unavailable);
}

namespace {
class PreferenceHeadSnapshot final : public PreferenceKnowledgeHeads {
 public:
  std::vector<EventIdentity> identities;
  uint32_t failureAt = UINT32_MAX;
  uint32_t count() const override { return identities.size(); }
  bool read(uint32_t at, EventIdentity& output) override {
    if (at >= identities.size() || at == failureAt) return false;
    output = identities[at];
    return true;
  }
};
}  // namespace
TEST(CompanionTintaJournalTest, PreferenceEditResolvesMoreThanFourBranchesThroughRealValueBatches) {
  Fixture f;
  KnowledgeIndex index;
  KnowledgeVisits visits;
  PreferenceHeadSnapshot snapshot;
  index.ids.reserve(8);
  snapshot.identities.reserve(6);
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  std::array<uint8_t, 8> body{1, 4, 8, 1, 20, 0, 0, 0};
  f.event.kind = EventKind::Preference;
  f.event.resource = PREFERENCE_SCOPE;
  f.event.schedulerVersion = 0;
  f.event.schedulerConfiguration = {};
  for (uint8_t origin = 1; origin <= 6; ++origin) {
    f.event.identity.origin.fill(origin);
    body[4] = 5 * origin;
    ASSERT_TRUE(f.storage.digest(body, f.event.bodyHash));
    ASSERT_EQ(f.journal.append(f.event, body), TintaJournalResult::Ok);
    index.ids.push_back(f.event.identity);
  }
  PortablePreferenceResolution resolution;
  ASSERT_EQ(resolution.run(f.journal, index, visits), TintaJournalResult::Conflict);
  JournalKnowledgeHeads heads;
  ASSERT_EQ(heads.begin(f.journal, index, visits), TintaJournalResult::Ok);
  EventIdentity identity;
  for (;;) {
    bool complete = false;
    ASSERT_EQ(heads.next(identity, complete), TintaJournalResult::Ok);
    if (complete) break;
    snapshot.identities.push_back(identity);
  }
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  body[4] = 40;
  ReaderPreferenceChangeCapture capture(writer, &snapshot);
  ReaderPreferenceValues values;
  values.screenMargin = 5;
  ASSERT_TRUE(capture.initialize(values));
  const auto encoder = [](void*, const ReaderPreferenceValues& input, uint8_t key, std::span<uint8_t> output) {
    return encodeReaderPreference(input, key, "en", {}, {}, output);
  };
  ASSERT_EQ(capture.persist(values, encoder, nullptr, 0, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.count(), 6U);
  values.screenMargin = 40;
  ASSERT_EQ(capture.persist(values, encoder, nullptr, 0, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.count(), 8U);
  for (uint32_t at = 6; at < 8; ++at) {
    ASSERT_EQ(f.journal.read(at), TintaJournalResult::Ok);
    index.ids.push_back(f.journal.event().identity);
    EXPECT_EQ(f.journal.event().ancestorCount, at == 6 ? 4U : 2U);
    EXPECT_EQ(f.journal.event().identity.sequence, at - 5);
    EXPECT_TRUE(std::equal(body.begin(), body.end(), f.journal.body().begin()));
  }
  ASSERT_EQ(resolution.run(f.journal, index, visits), TintaJournalResult::Ok);
  EXPECT_EQ(resolution.conflictMask(), 0U);
  ASSERT_EQ(resolution.bodies().size(), 1U);
  EXPECT_EQ(resolution.bodies()[0][4], 40);
  snapshot.failureAt = 0;
  values.screenMargin = 35;
  ASSERT_EQ(capture.persist(values, encoder, nullptr, 0, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.count(), 9U);
  ASSERT_EQ(f.journal.read(8), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.event().ancestorCount, 1U);
  EXPECT_EQ(f.journal.event().ancestors[0], index.ids[7]);
}
TEST(CompanionTintaJournalTest, PreferenceKnowledgeFailureStopsPartialBatchUntilRecovery) {
  Fixture f;
  PreferenceHeadSnapshot snapshot;
  snapshot.identities.reserve(5);
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  for (uint8_t origin = 1; origin <= 5; ++origin) {
    f.event.identity.origin.fill(origin);
    ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
    snapshot.identities.push_back(f.event.identity);
  }
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  std::array<uint8_t, 8> body{1, 4, 8, 1, 20, 0, 0, 0};
  snapshot.failureAt = 4;
  EXPECT_EQ(writer.recordPreferenceResolving(body, snapshot, 0, 0, ClockQuality::Unknown), TintaJournalResult::IoError);
  EXPECT_EQ(f.journal.count(), 6U);
  EXPECT_FALSE(writer.available());
  snapshot.failureAt = UINT32_MAX;
  EXPECT_EQ(writer.recordPreferenceResolving(body, snapshot, 0, 0, ClockQuality::Unknown),
            TintaJournalResult::Unavailable);
  EXPECT_EQ(f.journal.open(), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.count(), 6U);
}

TEST(CompanionReaderPreferenceChangeCapture, PreflightBeforeWriterStartRetainsExactCandidateAndCommitsOnce) {
  Fixture f;
  TintaWriter writer(f.journal, f.storage);
  ReaderPreferenceChangeCapture capture(writer);
  ReaderPreferenceValues values;
  values.screenMargin = 5;
  ASSERT_TRUE(capture.initialize(values));
  values.screenMargin = 20;
  const auto encoder = [](void*, const ReaderPreferenceValues& input, uint8_t key, std::span<uint8_t> bytes) {
    return encodeReaderPreference(input, key, "en", {}, {}, bytes);
  };
  ASSERT_EQ(capture.prepare(values, encoder, nullptr), TintaJournalResult::Ok);
  EXPECT_TRUE(capture.preparedChanges());
  EXPECT_EQ(capture.persistPrepared(0, 0, ClockQuality::Unknown), TintaJournalResult::Unavailable);
  Identities identities;
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  PreferenceHeadSnapshot heads;
  ASSERT_TRUE(capture.setInitialKnowledge(heads));
  values.screenMargin = 25;
  ASSERT_EQ(capture.persistPrepared(0, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  EXPECT_EQ(capture.baselineValues().screenMargin, 20);
  EXPECT_EQ(f.journal.count(), 1U);
  EXPECT_FALSE(capture.setInitialKnowledge(heads));
  EXPECT_EQ(capture.persistPrepared(0, 0, ClockQuality::Unknown), TintaJournalResult::Unavailable);
  ASSERT_EQ(f.journal.read(0), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.body()[4], 20);
}

TEST(CompanionReaderPreferenceChangeCapture, SelectedImportPreflightsOnlyAbsentKeysAndRejectsUnknownMask) {
  Fixture f;
  TintaWriter writer(f.journal, f.storage);
  ReaderPreferenceChangeCapture capture(writer);
  ReaderPreferenceValues values;
  values.screenMargin = 20;
  ASSERT_TRUE(capture.initialize(values));
  unsigned calls = 0;
  const auto encoder = [](void* context, const ReaderPreferenceValues& input, uint8_t key, std::span<uint8_t> bytes) {
    ++*static_cast<unsigned*>(context);
    return encodeReaderPreference(input, key, "en", {}, {}, bytes);
  };
  ASSERT_EQ(capture.prepareSelected(values, encoder, &calls, 1U << 7), TintaJournalResult::Ok);
  EXPECT_EQ(calls, 1u);
  ASSERT_TRUE(capture.preparedChanges());
  EXPECT_EQ(capture.prepareSelected(values, encoder, &calls, 1U << 14), TintaJournalResult::Invalid);
  EXPECT_FALSE(capture.preparedChanges());
  EXPECT_EQ(calls, 1u);
  ASSERT_EQ(capture.prepareSelected(values, encoder, &calls, 1U << 7), TintaJournalResult::Ok);
  Identities identities;
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  ASSERT_EQ(capture.persistPrepared(0, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.count(), 1u);
  ASSERT_EQ(f.journal.read(0), TintaJournalResult::Ok);
  PreferenceBodyView decoded;
  ASSERT_TRUE(decodePreferenceBody(f.journal.body(), decoded));
  EXPECT_EQ(decoded.key, 8);
  EXPECT_EQ(f.journal.body()[4], 20);
}

TEST(CompanionTintaJournalTest, ReadingPositionWriterJoinsSixHeadsAndRejectsInvalidIdentity) {
  Fixture f;
  KnowledgeIndex index;
  KnowledgeVisits visits;
  index.ids.reserve(8);
  ReadingPositionResolution resolution;
  PreferenceHeadSnapshot snapshot;
  snapshot.identities.reserve(6);
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  std::array<uint8_t, 8> body{1, 1, 2, 0, 10, 0, 0, 0};
  f.event.kind = EventKind::ReadingPosition;
  f.event.resource.fill(7);
  f.event.schedulerVersion = 0;
  f.event.schedulerConfiguration = {};
  for (uint8_t origin = 1; origin <= 6; ++origin) {
    f.event.identity.origin.fill(origin);
    body[4] = origin;
    ASSERT_TRUE(f.storage.digest(body, f.event.bodyHash));
    ASSERT_EQ(f.journal.append(f.event, body), TintaJournalResult::Ok);
    snapshot.identities.push_back(f.event.identity);
    index.ids.push_back(f.event.identity);
  }
  ReadingAnchor resolved{9, 99};
  EXPECT_EQ(resolution.run(f.journal, index, visits, f.event.resource, resolved), TintaJournalResult::Conflict);
  EXPECT_EQ(resolved, (ReadingAnchor{9, 99}));
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  Digest empty{};
  EXPECT_EQ(writer.recordReadingPosition(body, empty, snapshot, 0, ClockQuality::Unknown), TintaJournalResult::Invalid);
  EXPECT_EQ(f.journal.count(), 6u);
  body[4] = 40;
  ASSERT_EQ(writer.recordReadingPosition(body, f.event.resource, snapshot, 0, ClockQuality::Unknown),
            TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.count(), 8u);
  ASSERT_EQ(f.journal.read(6), TintaJournalResult::Ok);
  index.ids.push_back(f.journal.event().identity);
  EXPECT_EQ(f.journal.event().ancestorCount, 4);
  EXPECT_EQ(f.journal.event().kind, EventKind::ReadingPosition);
  ASSERT_EQ(f.journal.read(7), TintaJournalResult::Ok);
  index.ids.push_back(f.journal.event().identity);
  EXPECT_EQ(f.journal.event().ancestorCount, 2);
  ReadingAnchor anchor;
  ASSERT_TRUE(decodeReadingAnchor(f.journal.body(), anchor));
  EXPECT_EQ(anchor.spine, 2);
  EXPECT_EQ(anchor.visibleTextOffset, 40u);
  EXPECT_EQ(f.journal.event().resource, f.event.resource);
  ASSERT_EQ(resolution.run(f.journal, index, visits, f.event.resource, resolved), TintaJournalResult::Ok);
  EXPECT_EQ(resolved, anchor);
}

TEST(CompanionTintaJournalTest, ReadingKnowledgeFailureStopsAfterDurableFirstBatch) {
  Fixture f;
  PreferenceHeadSnapshot snapshot;
  snapshot.identities.reserve(5);
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  for (uint8_t origin = 1; origin <= 5; ++origin) {
    f.event.identity.origin.fill(origin);
    ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
    snapshot.identities.push_back(f.event.identity);
  }
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  const std::array<uint8_t, 8> body{1, 1, 2, 0, 40, 0, 0, 0};
  snapshot.failureAt = 4;
  EXPECT_EQ(writer.recordReadingPosition(body, f.event.resource, snapshot, 0, ClockQuality::Unknown),
            TintaJournalResult::IoError);
  EXPECT_EQ(f.journal.count(), 6u);
  EXPECT_FALSE(writer.available());
  snapshot.failureAt = UINT32_MAX;
  EXPECT_EQ(writer.recordReadingPosition(body, f.event.resource, snapshot, 0, ClockQuality::Unknown),
            TintaJournalResult::Unavailable);
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.count(), 6u);
  ASSERT_EQ(f.journal.read(5), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.event().ancestorCount, 4);
  ReadingAnchor anchor;
  ASSERT_TRUE(decodeReadingAnchor(f.journal.body(), anchor));
  EXPECT_EQ(anchor.visibleTextOffset, 40u);
}

TEST(CompanionTintaJournalTest, BookmarkWriterPersistsStablePutAndDeleteAndRefusesInvalidIdentity) {
  Fixture f;
  PreferenceHeadSnapshot heads;
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  std::array<uint8_t, 28> put{};
  put[0] = 1;
  put[1] = static_cast<uint8_t>(EventKind::BookmarkPut);
  EXPECT_EQ(writer.recordBookmark(put, f.event.resource, heads, 0, ClockQuality::Unknown), TintaJournalResult::Invalid);
  EXPECT_EQ(f.journal.count(), 0u);
  put[2] = 7;
  put[18] = 2;
  put[20] = 42;
  ASSERT_EQ(writer.recordBookmark(put, f.event.resource, heads, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.read(0), TintaJournalResult::Ok);
  const auto first = f.journal.event().identity;
  BookmarkBodyView decoded;
  ASSERT_TRUE(decodeBookmarkBody(f.journal.body(), decoded));
  EXPECT_EQ(decoded.identity[0], 7);
  EXPECT_FALSE(decoded.deleted);
  EXPECT_EQ(decoded.anchor, (ReadingAnchor{2, 42}));
  std::array<uint8_t, 18> removed{};
  std::copy_n(put.begin(), removed.size(), removed.begin());
  removed[1] = static_cast<uint8_t>(EventKind::BookmarkDelete);
  ASSERT_EQ(writer.recordBookmark(removed, f.event.resource, heads, 0, ClockQuality::Unknown), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.read(1), TintaJournalResult::Ok);
  EXPECT_EQ(f.journal.event().ancestors[0], first);
  ASSERT_TRUE(decodeBookmarkBody(f.journal.body(), decoded));
  EXPECT_TRUE(decoded.deleted);
  EXPECT_EQ(decoded.identity[0], 7);
}

TEST(CompanionBookmarkIdentityCursor, BoundedSortedDedupIncludesTombstonesAndDetectsJournalChanges) {
  Fixture f;
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  f.storage.data.reserve(80 * TintaJournal::EXTENDED_RECORD_SIZE);
  f.event.schedulerVersion = 0;
  f.event.schedulerConfiguration = {};
  f.event.resource.fill(8);
  const auto edition = f.event.resource;
  BookmarkBodyView bookmark;
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> body{};
  for (uint32_t at = 0; at < 71; ++at) {
    bookmark.identity[0] = at % 2 ? 7 : 3;
    bookmark.deleted = bookmark.identity[0] == 7;
    f.event.identity.sequence = at + 1;
    f.event.kind = bookmark.deleted ? EventKind::BookmarkDelete : EventKind::BookmarkPut;
    if (at == 70) {
      f.event.resource.fill(9);
      bookmark.identity[0] = 1;
    }
    const auto length = encodeBookmarkBody(bookmark, body);
    ASSERT_TRUE(f.storage.digest(std::span(body).first(length), f.event.bodyHash));
    ASSERT_EQ(f.journal.append(f.event, std::span(body).first(length)), TintaJournalResult::Ok);
  }
  BookmarkIdentityCursor cursor;
  ASSERT_TRUE(cursor.begin(f.journal, edition));
  Identity output{};
  output.fill(99);
  const auto original = output;
  EXPECT_EQ(cursor.step(f.journal, output), BookmarkCursorResult::Pending);
  EXPECT_EQ(output, original);
  EXPECT_EQ(cursor.step(f.journal, output), BookmarkCursorResult::Found);
  EXPECT_EQ(output[0], 3);
  const auto first = output;
  EXPECT_EQ(cursor.step(f.journal, output), BookmarkCursorResult::Pending);
  EXPECT_EQ(output, first);
  EXPECT_EQ(cursor.step(f.journal, output), BookmarkCursorResult::Found);
  EXPECT_EQ(output[0], 7);
  const auto second = output;
  EXPECT_EQ(cursor.step(f.journal, output), BookmarkCursorResult::Pending);
  EXPECT_EQ(output, second);
  EXPECT_EQ(cursor.step(f.journal, output), BookmarkCursorResult::End);
  EXPECT_EQ(cursor.step(f.journal, output), BookmarkCursorResult::End);
  EXPECT_EQ(output, second);
  f.event.identity.sequence = 72;
  const auto length = encodeBookmarkBody(bookmark, body);
  ASSERT_EQ(f.journal.append(f.event, std::span(body).first(length)), TintaJournalResult::Ok);
  EXPECT_EQ(cursor.step(f.journal, output), BookmarkCursorResult::Error);
  EXPECT_EQ(cursor.error(), TintaJournalResult::Conflict);
  EXPECT_EQ(output, second);
}

TEST(CompanionBookmarkIdentityCursor, InvalidBeginEmptyJournalAndCorruptReadsPreserveOutput) {
  Fixture f;
  BookmarkIdentityCursor cursor;
  Identity output{};
  output.fill(99);
  const auto original = output;
  EXPECT_FALSE(cursor.begin(f.journal, f.event.resource));
  EXPECT_EQ(cursor.error(), TintaJournalResult::Unavailable);
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  EXPECT_FALSE(cursor.begin(f.journal, {}));
  EXPECT_EQ(cursor.error(), TintaJournalResult::Invalid);
  ASSERT_TRUE(cursor.begin(f.journal, f.event.resource));
  EXPECT_EQ(cursor.step(f.journal, output), BookmarkCursorResult::End);
  EXPECT_EQ(output, original);
  ASSERT_EQ(f.journal.append(f.event, std::span(f.bytes).first(f.length)), TintaJournalResult::Ok);
  ASSERT_TRUE(cursor.begin(f.journal, f.event.resource));
  f.storage.data[30] ^= 1;
  EXPECT_EQ(cursor.step(f.journal, output), BookmarkCursorResult::Error);
  EXPECT_EQ(cursor.error(), TintaJournalResult::Corrupt);
  EXPECT_EQ(output, original);
}

TEST(CompanionBookmarkResolution, ConcurrentPutAndDeleteRequireChoiceAndJoinedDeleteWins) {
  Fixture f;
  KnowledgeIndex index;
  KnowledgeVisits visits;
  BookmarkResolution resolution;
  PreferenceHeadSnapshot heads;
  index.ids.reserve(4);
  heads.identities.reserve(2);
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  BookmarkBodyView bookmark;
  bookmark.identity[0] = 7;
  bookmark.anchor = {2, 123};
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> body{};
  f.event.resource.fill(8);
  f.event.schedulerVersion = 0;
  f.event.schedulerConfiguration = {};
  for (uint8_t origin = 1; origin <= 2; ++origin) {
    bookmark.deleted = origin == 2;
    const auto size = encodeBookmarkBody(bookmark, body);
    ASSERT_NE(size, 0u);
    f.event.kind = bookmark.deleted ? EventKind::BookmarkDelete : EventKind::BookmarkPut;
    f.event.identity.origin.fill(origin);
    ASSERT_TRUE(f.storage.digest(std::span(body).first(size), f.event.bodyHash));
    ASSERT_EQ(f.journal.append(f.event, std::span(body).first(size)), TintaJournalResult::Ok);
    index.ids.push_back(f.event.identity);
    heads.identities.push_back(f.event.identity);
  }
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> output;
  output.fill(0xa5);
  const auto untouched = output;
  size_t length = 99;
  EXPECT_EQ(resolution.run(f.journal, index, visits, f.event.resource, bookmark.identity, output, length),
            TintaJournalResult::Conflict);
  EXPECT_EQ(output, untouched);
  EXPECT_EQ(length, 99u);
  struct Choices {
    std::vector<EventIdentity> sources;
    std::vector<bool> deleted;
  } choices;
  choices.sources.reserve(4);
  choices.deleted.reserve(4);
  const auto visit = +[](void* raw, const EventIdentity& source, std::span<const uint8_t> body) {
    auto& choices = *static_cast<Choices*>(raw);
    BookmarkBodyView decoded;
    if (!decodeBookmarkBody(body, decoded)) return false;
    choices.sources.push_back(source);
    choices.deleted.push_back(decoded.deleted);
    return true;
  };
  ASSERT_EQ(resolution.visitHeads(f.journal, index, visits, f.event.resource, bookmark.identity, visit, &choices),
            TintaJournalResult::Ok);
  ASSERT_EQ(choices.sources.size(), 2u);
  EXPECT_EQ(choices.sources[0].origin[0], 2u);
  EXPECT_EQ(choices.sources[1].origin[0], 1u);
  EXPECT_TRUE(choices.deleted[0]);
  EXPECT_FALSE(choices.deleted[1]);
  EXPECT_EQ(resolution.visitHeads(
                f.journal, index, visits, f.event.resource, bookmark.identity,
                +[](void*, const EventIdentity&, std::span<const uint8_t>) { return false; }, nullptr),
            TintaJournalResult::IoError);
  Identity other{};
  other[0] = 9;
  EXPECT_EQ(resolution.run(f.journal, index, visits, f.event.resource, other, output, length),
            TintaJournalResult::Unavailable);
  EXPECT_EQ(output, untouched);
  EXPECT_EQ(length, 99u);
  Identities identities;
  TintaWriter writer(f.journal, f.storage);
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  const auto size = encodeBookmarkBody(bookmark, body);
  ASSERT_EQ(writer.recordBookmark(std::span(body).first(size), f.event.resource, heads, 0, ClockQuality::Unknown),
            TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.read(2), TintaJournalResult::Ok);
  index.ids.push_back(f.journal.event().identity);
  ASSERT_EQ(resolution.run(f.journal, index, visits, f.event.resource, bookmark.identity, output, length),
            TintaJournalResult::Ok);
  EXPECT_EQ(length, 18u);
  BookmarkBodyView decoded;
  ASSERT_TRUE(decodeBookmarkBody(std::span(output).first(length), decoded));
  EXPECT_TRUE(decoded.deleted);
  EXPECT_EQ(decoded.identity, bookmark.identity);
  choices.sources.clear();
  choices.deleted.clear();
  ASSERT_EQ(resolution.visitHeads(f.journal, index, visits, f.event.resource, bookmark.identity, visit, &choices),
            TintaJournalResult::Ok);
  ASSERT_EQ(choices.sources.size(), 1u);
  EXPECT_EQ(choices.sources[0], index.ids.back());
  EXPECT_TRUE(choices.deleted[0]);
}

TEST(CompanionTintaJournal, LegacyIdentityCursorPreservesUndoAfterExpandedFlagsAndRestartsDeterministically) {
  EventIdentity first;
  first.origin.fill(17);
  first.epoch = 19;
  first.sequence = 1;
  std::array<LegacyTintaEntry, 4> records{};
  for (auto& record : records) record.uid = 123;
  records[0].operation = LegacyTintaOperation::Flags;
  records[0].flags = tinta::core::item_flag::kSuspended;
  records[1].grade = 3;
  records[2].operation = LegacyTintaOperation::Undo;
  records[2].undoRecord = 1;
  records[3].operation = LegacyTintaOperation::Flags;
  records[3].flags = tinta::core::item_flag::kStarred;
  std::array<LegacyTintaEventIdentities, 4> planned{};
  for (unsigned restart = 0; restart < 2; ++restart) {
    LegacyTintaEventCursor cursor;
    ASSERT_TRUE(cursor.begin(first, records.size()));
    EXPECT_FALSE(cursor.complete());
    uint64_t sequence = 1;
    for (unsigned index = 0; index < records.size(); ++index) {
      LegacyTintaEventIdentities result;
      ASSERT_TRUE(cursor.assign(index, records[index], result));
      EXPECT_EQ(result.events[0].sequence, sequence);
      EXPECT_EQ(result.events[0].origin, first.origin);
      EXPECT_EQ(result.events[0].epoch, first.epoch);
      EXPECT_EQ(result.count, records[index].operation == LegacyTintaOperation::Flags ? 2 : 1);
      if (result.count == 2) {
        EXPECT_EQ(result.events[1].sequence, sequence + 1);
      }
      if (index == 2) {
        EXPECT_EQ(result.undoTarget, planned[1].events[0]);
        EXPECT_EQ(result.undoTarget.sequence, 3u);
      }
      TintaProgressMutation mutation;
      Identity course{};
      course.fill(1);
      ASSERT_TRUE(mapLegacyTintaMutation(records[index], tinta::core::ItemState::fresh(123), course, {},
                                         result.undoTarget, mutation));
      EXPECT_EQ(mutation.count, result.count);
      if (restart == 0)
        planned[index] = result;
      else
        EXPECT_EQ(result, planned[index]);
      sequence += result.count;
    }
    EXPECT_TRUE(cursor.complete());
    EXPECT_EQ(cursor.records(), 4u);
    EXPECT_EQ(cursor.events(), 6u);
    auto sentinel = planned.back();
    EXPECT_FALSE(cursor.assign(4, records[1], sentinel));
    EXPECT_EQ(sentinel, planned.back());
  }
}

TEST(CompanionTintaJournal, LegacyIdentityCursorRejectsBrokenOrderAndUndoWithoutConsumingSequences) {
  EventIdentity first;
  first.origin.fill(17);
  first.epoch = 19;
  first.sequence = 1;
  LegacyTintaEventCursor cursor;
  ASSERT_TRUE(cursor.begin(first, 3));
  LegacyTintaEntry review;
  review.uid = 123;
  review.grade = 3;
  LegacyTintaEventIdentities output;
  output.count = 99;
  const auto sentinel = output;
  EXPECT_FALSE(cursor.assign(1, review, output));
  EXPECT_EQ(output, sentinel);
  auto invalid = review;
  invalid.grade = 0;
  EXPECT_FALSE(cursor.assign(0, invalid, output));
  EXPECT_EQ(cursor.records(), 0u);
  EXPECT_EQ(cursor.events(), 0u);
  ASSERT_TRUE(cursor.assign(0, review, output));
  const auto identity = output.events[0];
  LegacyTintaEntry undo;
  undo.operation = LegacyTintaOperation::Undo;
  undo.uid = 123;
  undo.undoRecord = 1;
  output = sentinel;
  EXPECT_FALSE(cursor.assign(1, undo, output));
  undo.undoRecord = 0;
  undo.uid = 456;
  EXPECT_FALSE(cursor.assign(1, undo, output));
  EXPECT_EQ(output, sentinel);
  EXPECT_EQ(cursor.events(), 1u);
  undo.uid = 123;
  ASSERT_TRUE(cursor.assign(1, undo, output));
  EXPECT_EQ(output.undoTarget, identity);
  output = sentinel;
  EXPECT_FALSE(cursor.assign(2, undo, output));
  EXPECT_EQ(output, sentinel);
  EXPECT_EQ(cursor.events(), 2u);
  ASSERT_TRUE(cursor.assign(2, review, output));
  EXPECT_EQ(output.events[0].sequence, 3u);
  EXPECT_TRUE(cursor.complete());
  first.sequence = 2;
  EXPECT_FALSE(cursor.begin(first, 3));
  EXPECT_FALSE(cursor.complete());
  EXPECT_FALSE(cursor.assign(0, review, output));
  first.sequence = 1;
  first.epoch = 0;
  EXPECT_FALSE(cursor.begin(first, 3));
  first.epoch = 19;
  first.origin = {};
  EXPECT_FALSE(cursor.begin(first, 3));
  first.origin.fill(17);
  EXPECT_FALSE(cursor.begin(first, UINT32_MAX));
  ASSERT_TRUE(cursor.begin(first, 0));
  EXPECT_TRUE(cursor.complete());
  EXPECT_EQ(cursor.events(), 0u);
}

namespace {
UnboundCourseReviewReservation journalEpochReservation(const Fixture& f) {
  UnboundCourseReviewReservation value;
  value.intent.reader = f.event.identity.origin;
  value.intent.request.original.generation = f.event.storageGeneration;
  value.intent.request.original.owner.fill(3);
  value.intent.request.original.transaction.fill(4);
  value.intent.request.original.reviewHash.fill(5);
  auto& pack = value.intent.request.original.manifest;
  pack.kind = ContentKind::Course;
  pack.formatVersion = 1;
  pack.length = 100;
  pack.logicalIdentity.fill(7);
  pack.contentHash.fill(8);
  value.intent.activePack = pack;
  value.epoch = f.event.identity.epoch;
  value.records = 2;
  value.events = 3;
  return value;
}
}  // namespace

TEST(CompanionTintaJournal, ReservedReviewEpochScanCountsAllKindsAndAncestorReferencesWithoutWriting) {
  Fixture f;
  const auto reservation = journalEpochReservation(f);
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  auto permitted = [](void*) { return true; };
  UnboundCourseReviewEpochUse report;
  ASSERT_TRUE(inspectUnboundCourseReviewEpochUse(f.journal, reservation, report, permitted, nullptr));
  EXPECT_EQ(report.matchedEvents, 0u);
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  auto descendant = f.event;
  descendant.identity.origin.fill(9);
  descendant.ancestorCount = 1;
  descendant.ancestors[0] = f.event.identity;
  ASSERT_EQ(f.journal.append(descendant, f.body()), TintaJournalResult::Ok);
  TintaBody star;
  star.course.fill(7);
  star.uid = 1;
  star.kind = EventKind::Star;
  std::array<uint8_t, 23> body{};
  ASSERT_EQ(encodeTintaBody(star, body), body.size());
  auto event = f.event;
  event.identity.sequence = 2;
  event.kind = EventKind::Star;
  event.schedulerVersion = 0;
  event.schedulerConfiguration = {};
  ASSERT_TRUE(f.storage.digest(body, event.bodyHash));
  ASSERT_EQ(f.journal.append(event, body), TintaJournalResult::Ok);
  const auto writes = f.storage.writes;
  const auto data = f.storage.data;
  const auto headers = f.storage.headers;
  ASSERT_TRUE(inspectUnboundCourseReviewEpochUse(f.journal, reservation, report, permitted, nullptr));
  EXPECT_EQ(report.matchedEvents, 2u);
  EXPECT_EQ(report.ancestorReferences, 1u);
  EXPECT_EQ(report.greatestSequence, 2u);
  EXPECT_FALSE(report.foreignGeneration);
  EXPECT_FALSE(report.outsideReviewRange);
  EXPECT_EQ(f.storage.writes, writes);
  EXPECT_EQ(f.storage.data, data);
  EXPECT_EQ(f.storage.headers, headers);
  auto foreign = reservation;
  foreign.epoch += 1;
  ASSERT_TRUE(inspectUnboundCourseReviewEpochUse(f.journal, foreign, report, permitted, nullptr));
  EXPECT_EQ(report, UnboundCourseReviewEpochUse{});
  foreign = reservation;
  foreign.intent.reader.fill(10);
  ASSERT_TRUE(inspectUnboundCourseReviewEpochUse(f.journal, foreign, report, permitted, nullptr));
  EXPECT_EQ(report, UnboundCourseReviewEpochUse{});
  foreign = reservation;
  foreign.intent.request.original.generation.fill(11);
  foreign.records = foreign.events = 0;
  ASSERT_TRUE(inspectUnboundCourseReviewEpochUse(f.journal, foreign, report, permitted, nullptr));
  EXPECT_EQ(report.matchedEvents, 2u);
  EXPECT_TRUE(report.foreignGeneration);
  EXPECT_TRUE(report.outsideReviewRange);
}

TEST(CompanionTintaJournal, ReservedReviewEpochScanPreservesOutputOnCorruptionIoCancellationAndChangedCount) {
  for (unsigned fault = 0; fault < 4; ++fault) {
    Fixture f;
    const auto reservation = journalEpochReservation(f);
    ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
    ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
    UnboundCourseReviewEpochUse report;
    report.matchedEvents = 123;
    const auto sentinel = report;
    if (fault == 0) f.storage.data[30] ^= 1;
    if (fault == 1) f.storage.failedReadAt = f.storage.reads + 1;
    struct Context {
      Fixture* fixture;
      unsigned calls = 0, fault;
    } context{&f, 0, fault};
    auto permitted = [](void* raw) {
      auto& state = *static_cast<Context*>(raw);
      ++state.calls;
      if (state.fault == 2 && state.calls == 3) return false;
      if (state.fault == 3 && state.calls == 3) {
        auto other = state.fixture->event;
        other.identity.origin.fill(9);
        EXPECT_EQ(state.fixture->journal.append(other, state.fixture->body()), TintaJournalResult::Ok);
      }
      return true;
    };
    const auto writes = f.storage.writes;
    EXPECT_FALSE(inspectUnboundCourseReviewEpochUse(f.journal, reservation, report, permitted, &context));
    EXPECT_EQ(report, sentinel);
    if (fault != 3) {
      EXPECT_EQ(f.storage.writes, writes);
    }
  }
}

TEST(CompanionTintaJournal, ReservedEpochBodyVerifierRejectsMismatchAndSkipsOtherOrigins) {
  Fixture f;
  const auto reservation = journalEpochReservation(f);
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.append(f.event, f.body()), TintaJournalResult::Ok);
  auto foreign = f.event;
  foreign.identity.origin.fill(9);
  ASSERT_EQ(f.journal.append(foreign, f.body()), TintaJournalResult::Ok);
  struct Context {
    Fixture* fixture;
    unsigned calls = 0;
    bool accept = false;
  } context{&f};
  auto verify = [](void* raw, const SyncEvent& event, std::span<const uint8_t> body) {
    auto& state = *static_cast<Context*>(raw);
    ++state.calls;
    EXPECT_EQ(event.identity, state.fixture->event.identity);
    EXPECT_TRUE(std::equal(body.begin(), body.end(), state.fixture->body().begin(), state.fixture->body().end()));
    return state.accept;
  };
  auto permitted = [](void*) { return true; };
  UnboundCourseReviewEpochUse report;
  report.matchedEvents = 123;
  const auto sentinel = report;
  const auto writes = f.storage.writes;
  EXPECT_FALSE(inspectUnboundCourseReviewEpochUse(f.journal, reservation, report, permitted, &context, verify));
  EXPECT_EQ(report, sentinel);
  EXPECT_EQ(context.calls, 1u);
  context.accept = true;
  context.calls = 0;
  ASSERT_TRUE(inspectUnboundCourseReviewEpochUse(f.journal, reservation, report, permitted, &context, verify));
  EXPECT_EQ(context.calls, 1u);
  EXPECT_EQ(report.matchedEvents, 1u);
  EXPECT_EQ(f.storage.writes, writes);
}

TEST(CompanionTintaJournal, LegacyConversionProducesCanonicalReviewUndoAndAtomicFlagPair) {
  Fixture f;
  auto reservation = journalEpochReservation(f);
  reservation.records = 3;
  reservation.events = 4;
  auto hash = [](void* raw, std::span<const uint8_t> bytes, Digest& output) {
    return static_cast<Storage*>(raw)->digest(bytes, output);
  };
  LegacyTintaEventConversion conversion(hash, [](void*) { return true; }, &f.storage);
  const TintaSchedulerConfiguration configuration{8700, 730};
  ASSERT_TRUE(conversion.begin(reservation, configuration));
  ASSERT_EQ(f.journal.open(), TintaJournalResult::Ok);
  LegacyTintaEntry entry;
  entry.uid = 1;
  entry.grade = 4;
  entry.format = 9;
  entry.responseQuarterSeconds = 255;
  entry.studyDay = 123;
  entry.timestamp = UINT32_MAX;
  auto before = tinta::core::ItemState::fresh(1);
  ASSERT_TRUE(conversion.next(0, entry, before));
  ASSERT_NE(conversion.event(0), nullptr);
  EXPECT_EQ(conversion.event(0)->identity, reservation.first());
  EXPECT_EQ(conversion.event(0)->studyDay, 123u);
  EXPECT_EQ(conversion.event(0)->timestamp, 0u);
  EXPECT_EQ(conversion.event(0)->clockQuality, ClockQuality::Unknown);
  EXPECT_EQ(conversion.event(0)->ancestorCount, 0u);
  EXPECT_EQ(conversion.event(0)->resource, reservation.intent.request.original.manifest.contentHash);
  TintaBody decoded;
  ASSERT_TRUE(decodeTintaBody(conversion.body(0), decoded));
  EXPECT_EQ(decoded.responseMilliseconds, 63750u);
  EXPECT_EQ(decoded.configuration, configuration);
  EXPECT_EQ(decoded.course, reservation.intent.request.original.manifest.logicalIdentity);
  const auto review = *conversion.event(0);
  const std::vector<uint8_t> reviewBytes(conversion.body(0).begin(), conversion.body(0).end());
  ASSERT_EQ(f.journal.append(*conversion.event(0), conversion.body(0)), TintaJournalResult::Ok);
  ASSERT_EQ(f.journal.read(0), TintaJournalResult::Ok);
  EXPECT_TRUE(conversion.matches(0, f.journal.event(), f.journal.body()));
  auto changed = f.journal.event();
  changed.timestamp = 1;
  EXPECT_FALSE(conversion.matches(0, changed, f.journal.body()));
  EXPECT_FALSE(conversion.complete());
  entry.operation = LegacyTintaOperation::Undo;
  entry.undoRecord = 0;
  ASSERT_TRUE(conversion.next(1, entry, before));
  EXPECT_EQ(conversion.event(0)->identity.sequence, 2u);
  EXPECT_EQ(conversion.event(0)->ancestorCount, 1u);
  EXPECT_EQ(conversion.event(0)->ancestors[0], review.identity);
  ASSERT_TRUE(decodeTintaBody(conversion.body(0), decoded));
  EXPECT_EQ(decoded.undoTarget, review.identity);
  ASSERT_EQ(f.journal.append(*conversion.event(0), conversion.body(0)), TintaJournalResult::Ok);
  entry.operation = LegacyTintaOperation::Flags;
  entry.flags = tinta::core::item_flag::kSuspended | tinta::core::item_flag::kStarred;
  ASSERT_TRUE(conversion.next(2, entry, before));
  ASSERT_NE(conversion.event(1), nullptr);
  EXPECT_EQ(conversion.event(0)->identity.sequence, 3u);
  EXPECT_EQ(conversion.event(1)->identity.sequence, 4u);
  EXPECT_EQ(conversion.event(0)->kind, EventKind::Suspension);
  EXPECT_EQ(conversion.event(1)->kind, EventKind::Star);
  const std::array<std::span<const uint8_t>, 2> bodies{conversion.body(0), conversion.body(1)};
  ASSERT_EQ(f.journal.appendPair(std::span<const SyncEvent, 2>(conversion.event(0), 2), bodies),
            TintaJournalResult::Ok);
  EXPECT_TRUE(conversion.complete());
  EXPECT_EQ(conversion.count(), 4u);
  EXPECT_EQ(conversion.records(), 3u);
  ASSERT_TRUE(conversion.begin(reservation, configuration));
  entry.operation = LegacyTintaOperation::Review;
  ASSERT_TRUE(conversion.next(0, entry, before));
  EXPECT_EQ(*conversion.event(0), review);
  EXPECT_TRUE(std::equal(conversion.body(0).begin(), conversion.body(0).end(), reviewBytes.begin(), reviewBytes.end()));
}

TEST(CompanionTintaJournal, LegacyConversionFailuresNeverConsumeReservedIdentitiesOrExposePartialPackets) {
  for (unsigned fault = 0; fault < 6; ++fault) {
    Fixture f;
    auto reservation = journalEpochReservation(f);
    reservation.records = 1;
    reservation.events = 2;
    struct Context {
      Storage* storage;
      LegacyTintaEventConversion* owner = nullptr;
      unsigned fault, calls = 0;
      bool active = true;
    } context{&f.storage, nullptr, fault};
    auto hash = [](void* raw, std::span<const uint8_t> bytes, Digest& output) {
      auto& ctx = *static_cast<Context*>(raw);
      ++ctx.calls;
      EXPECT_EQ(ctx.owner->event(0), nullptr);
      EXPECT_TRUE(ctx.owner->body(0).empty());
      if (ctx.active && ctx.fault == 2) ctx.owner->close();
      if (ctx.active && ctx.fault == 1 && ctx.calls == 2) return false;
      return ctx.storage->digest(bytes, output);
    };
    auto permitted = [](void* raw) {
      auto& ctx = *static_cast<Context*>(raw);
      return !ctx.active || ctx.fault != 3;
    };
    LegacyTintaEventConversion conversion(hash, permitted, &context);
    context.owner = &conversion;
    context.active = false;
    ASSERT_TRUE(conversion.begin(reservation, {}));
    context.active = true;
    LegacyTintaEntry entry;
    entry.uid = 1;
    entry.operation = LegacyTintaOperation::Flags;
    entry.flags = tinta::core::item_flag::kStarred;
    auto before = tinta::core::ItemState::fresh(1);
    if (fault == 0) entry.flags |= tinta::core::item_flag::kLeech;
    if (fault == 4) before.uid = 2;
    if (fault == 5) entry.flags = 8;
    EXPECT_FALSE(conversion.next(0, entry, before));
    EXPECT_EQ(conversion.event(0), nullptr);
    EXPECT_EQ(conversion.event(1), nullptr);
    EXPECT_EQ(conversion.count(), 0u);
    context.active = false;
    if (fault == 2) {
      ASSERT_TRUE(conversion.begin(reservation, {}));
    }
    entry.flags = tinta::core::item_flag::kStarred;
    before.uid = 1;
    ASSERT_TRUE(conversion.next(0, entry, before));
    EXPECT_EQ(conversion.event(0)->identity.sequence, 1u);
    EXPECT_EQ(conversion.event(1)->identity.sequence, 2u);
    EXPECT_TRUE(conversion.complete());
    conversion.close();
    EXPECT_EQ(conversion.event(0), nullptr);
    EXPECT_FALSE(conversion.complete());
  }
}

namespace {
class LegacyCandidateStore final : public TintaReplayStore {
 public:
  std::map<uint32_t, tinta::core::ItemState> items;
  std::map<uint16_t, TintaReplayDay> days;
  unsigned calls = 0, failAt = 0;
  bool failAfterWrite = false;
  LegacyTintaReplay* cancel = nullptr;
  bool step() {
    ++calls;
    if (cancel) cancel->close();
    return calls != failAt;
  }
  bool item(uint32_t uid, tinta::core::ItemState& value) override {
    if (!step()) return false;
    value = items.contains(uid) ? items.at(uid) : tinta::core::ItemState::fresh(uid);
    return true;
  }
  bool putItem(const tinta::core::ItemState& value) override {
    const bool valid = step();
    if (valid || failAfterWrite) items[value.uid] = value;
    return valid;
  }
  bool day(uint16_t day, TintaReplayDay& value) override {
    if (!step()) return false;
    value = days.contains(day) ? days.at(day) : TintaReplayDay{};
    return true;
  }
  bool putDay(uint16_t day, const TintaReplayDay& value) override {
    const bool valid = step();
    if (valid || failAfterWrite) days[day] = value;
    return valid;
  }
  bool completion(EventKind, uint32_t, bool) override { return false; }
};
}  // namespace
TEST(CompanionTintaJournal, LegacyReplayMatchesNativeProgressAndRestoresUndoneDayTotals) {
  Fixture fixture;
  auto reservation = journalEpochReservation(fixture);
  reservation.records = 6;
  reservation.events = 7;
  tinta_test::MemStore nativeFiles;
  auto catalog = tinta_test::FakeCatalog::vocab(1, 1, 1);
  tinta::core::Fsrs scheduler(0.87f, 730);
  tinta::core::ProgressStore native(nativeFiles, catalog, scheduler);
  std::array<uint16_t, 2> slots{};
  ASSERT_NE(native.open(slots.data(), slots.size(), nullptr, 0), tinta::core::ProgressStore::OpenResult::Failed);
  LegacyCandidateStore candidate;
  LegacyTintaReplay replay(candidate, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(replay.begin(reservation, {8700, 730}));
  LegacyCandidateStore other;
  EXPECT_TRUE(replay.at(reservation, candidate, 0));
  EXPECT_FALSE(replay.at(reservation, other, 0));
  EXPECT_FALSE(replay.at(reservation, candidate, 1));
  LegacyTintaJournalDecoder decoder;
  for (uint32_t index = 0; index < 6; ++index) {
    const uint32_t itemIndex = index == 1 || index == 2 ? 1 : 0;
    tinta::core::ItemState expectedPrior;
    ASSERT_TRUE(native.load(itemIndex, expectedPrior));
    using Status = tinta::core::ProgressStore::Status;
    if (index == 0 || index == 1 || index == 4) {
      const auto day = index == 0 ? 5 : index == 1 ? 9 : 12;
      const auto grade = index == 1 ? tinta::core::Grade::Again : tinta::core::Grade::Good;
      ASSERT_EQ(native.review(itemIndex, grade, 0, 1000, day, 1234).status, Status::Stored);
    } else if (index == 2 || index == 5) {
      ASSERT_EQ(native.undo(index == 2 ? 10 : 13, 1235), Status::Stored);
    } else {
      ASSERT_EQ(native.setFlags(0, tinta::core::item_flag::kSuspended | tinta::core::item_flag::kStarred, 11, 1236),
                Status::Stored);
    }
    const auto& bytes = nativeFiles.files["reviews.log"];
    LegacyTintaEntry entry;
    ASSERT_EQ(decoder.next(std::span(bytes).subspan(index * 12, 12), entry), LegacyTintaDecodeResult::Record);
    tinta::core::ItemState prior;
    ASSERT_TRUE(replay.apply(reservation, index, entry, prior));
    EXPECT_EQ(prior, expectedPrior);
    for (uint32_t at = 0; at < 2; ++at) {
      tinta::core::ItemState expected, actual;
      ASSERT_TRUE(native.load(at, expected));
      ASSERT_TRUE(candidate.item(at + 1, actual));
      EXPECT_EQ(actual, expected);
    }
  }
  EXPECT_TRUE(replay.complete(reservation));
  EXPECT_EQ(candidate.days.at(5).newItems, 1u);
  EXPECT_EQ(candidate.days.at(5).gradedReviews, 1u);
  EXPECT_EQ(candidate.days.at(5).responseMilliseconds, 1000u);
  EXPECT_EQ(candidate.days.at(9).gradedReviews, 0u);
  EXPECT_EQ(candidate.days.at(9).responseMilliseconds, 0u);
  EXPECT_EQ(candidate.days.at(12).gradedReviews, 0u);
  EXPECT_EQ(candidate.days.at(12).reviews, 0u);
  auto foreign = reservation;
  ++foreign.epoch;
  EXPECT_FALSE(replay.complete(foreign));
  EXPECT_TRUE(replay.complete(reservation));
}
TEST(CompanionTintaJournal, LegacyReplayStoreFailuresPoisonCandidateAndPreservePriorOutput) {
  for (const auto operation : {LegacyTintaOperation::Review, LegacyTintaOperation::Undo, LegacyTintaOperation::Flags}) {
    const unsigned operations = operation == LegacyTintaOperation::Review ? 4
                                : operation == LegacyTintaOperation::Undo ? 3
                                                                          : 2;
    for (unsigned failAt = 1; failAt <= operations; ++failAt) {
      for (const bool failAfter : {false, true}) {
        Fixture fixture;
        auto reservation = journalEpochReservation(fixture);
        reservation.records = 2;
        reservation.events = 3;
        LegacyCandidateStore candidate;
        LegacyTintaReplay replay(candidate, [](void*) { return true; }, nullptr);
        ASSERT_TRUE(replay.begin(reservation, {}));
        LegacyTintaEntry entry;
        entry.uid = 1;
        entry.grade = 3;
        entry.studyDay = 5;
        tinta::core::ItemState prior;
        if (operation == LegacyTintaOperation::Undo) {
          ASSERT_TRUE(replay.apply(reservation, 0, entry, prior));
        }
        entry.operation = operation;
        entry.flags = tinta::core::item_flag::kStarred;
        entry.undoRecord = 0;
        candidate.calls = 0;
        candidate.failAt = failAt;
        candidate.failAfterWrite = failAfter;
        prior = tinta::core::ItemState::fresh(123);
        const auto sentinel = prior;
        const uint32_t index = operation == LegacyTintaOperation::Undo ? 1 : 0;
        EXPECT_FALSE(replay.apply(reservation, index, entry, prior));
        EXPECT_EQ(prior, sentinel);
        candidate.failAt = 0;
        EXPECT_FALSE(replay.apply(reservation, index, entry, prior));
        EXPECT_EQ(prior, sentinel);
        EXPECT_FALSE(replay.complete(reservation));
      }
    }
  }

  Fixture fixture;
  const auto reservation = journalEpochReservation(fixture);
  LegacyCandidateStore candidate;
  LegacyTintaReplay replay(candidate, [](void*) { return true; }, nullptr);
  ASSERT_TRUE(replay.begin(reservation, {}));
  candidate.cancel = &replay;
  LegacyTintaEntry entry;
  entry.uid = 1;
  entry.grade = 3;
  auto prior = tinta::core::ItemState::fresh(123);
  const auto sentinel = prior;
  EXPECT_FALSE(replay.apply(reservation, 0, entry, prior));
  EXPECT_EQ(prior, sentinel);
  EXPECT_TRUE(candidate.items.empty());
  candidate.cancel = nullptr;
  EXPECT_FALSE(replay.apply(reservation, 0, entry, prior));
  EXPECT_EQ(prior, sentinel);
}
