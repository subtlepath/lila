#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>

#include "lib/Companion/CompanionWifiRequest.h"

using namespace companion;
namespace {
Identity identity(uint8_t byte) {
  Identity value{};
  value.fill(byte);
  return value;
}
}  // namespace
TEST(CompanionWifiRequestTest, OnlyBoundTransactionTransferCommandsAreAccepted) {
  const auto transaction = identity(1), owner = identity(2), generation = identity(3);
  std::array<uint8_t, MAX_CONTROL_PAYLOAD + 1> bytes{};
  std::copy(transaction.begin(), transaction.end(), bytes.begin());
  FrameView exportRequest{Command::ExchangeChanges, false, 1, std::span(bytes).first(56)};
  EXPECT_TRUE(validWifiTransferRequest(exportRequest, transaction, owner, generation));
  EXPECT_FALSE(validWifiTransferRequest(exportRequest, identity(4), owner, generation));
  exportRequest.response = true;
  EXPECT_FALSE(validWifiTransferRequest(exportRequest, transaction, owner, generation));
  exportRequest.response = false;
  for (size_t length = 0; length <= MAX_CONTROL_PAYLOAD; ++length) {
    if (length == 56) continue;
    exportRequest.payload = std::span(bytes).first(length);
    EXPECT_FALSE(validWifiTransferRequest(exportRequest, transaction, owner, generation));
  }
  for (const auto command : {Command::TransferStatus, Command::Commit, Command::Abort, Command::JournalFormats}) {
    FrameView request{command, false, 1, std::span(bytes).first(16)};
    EXPECT_TRUE(validWifiTransferRequest(request, transaction, owner, generation));
    request.response = true;
    EXPECT_FALSE(validWifiTransferRequest(request, transaction, owner, generation));
    request.response = false;
    for (const size_t length : {size_t{0}, size_t{15}, size_t{17}}) {
      request.payload = std::span(bytes).first(length);
      EXPECT_FALSE(validWifiTransferRequest(request, transaction, owner, generation));
    }
    request.payload = std::span(bytes).first(16);
    EXPECT_FALSE(validWifiTransferRequest(request, identity(4), owner, generation));
  }
  FrameView chunk{Command::TransferChunk, false, 1, std::span(bytes).first(25)};
  EXPECT_TRUE(validWifiTransferRequest(chunk, transaction, owner, generation));
  chunk.payload = std::span(bytes).first(MAX_CONTROL_PAYLOAD);
  EXPECT_TRUE(validWifiTransferRequest(chunk, transaction, owner, generation));
  chunk.payload = bytes;
  EXPECT_FALSE(validWifiTransferRequest(chunk, transaction, owner, generation));
  for (size_t length = 0; length <= CHUNK_HEADER_SIZE; ++length) {
    chunk.payload = std::span(bytes).first(length);
    EXPECT_FALSE(validWifiTransferRequest(chunk, transaction, owner, generation));
  }
  chunk.payload = std::span(bytes).first(25);
  EXPECT_FALSE(validWifiTransferRequest(chunk, identity(4), owner, generation));
  for (const auto command :
       {Command::Discover, Command::Inventory, Command::ExchangeChanges, Command::WifiHandoff, Command::InstallFirmware,
        Command::Error, Command::RegisterInstallation, Command::AuthenticateInstallation}) {
    FrameView request{command, false, 1, std::span(bytes).first(16)};
    EXPECT_FALSE(validWifiTransferRequest(request, transaction, owner, generation));
  }
}
TEST(CompanionWifiRequestTest, JournalMergeRequiresBothHandoffAndDeclarationBindings) {
  JournalMergeIntent intent;
  intent.transaction = identity(1);
  intent.owner = identity(2);
  intent.generation = identity(3);
  intent.previous = {0, 1024, {}};
  intent.previous.frontier.fill(4);
  intent.merged = {1, 1024, {}};
  intent.merged.frontier.fill(5);
  std::array<uint8_t, JOURNAL_MERGE_INTENT_SIZE> declaration{};
  ASSERT_TRUE(encodeJournalMergeIntent(intent, declaration));
  std::array<uint8_t, MAX_CONTROL_PAYLOAD> bytes{};
  std::copy(intent.transaction.begin(), intent.transaction.end(), bytes.begin());
  JournalMergeRequestView merge{JournalMergeOperation::Begin, intent.transaction, declaration, {}, {}};
  const auto length = encodeJournalMergeRequest(merge, std::span(bytes).subspan(16));
  ASSERT_GT(length, 0u);
  FrameView request{Command::ExchangeChanges, false, 1, std::span(bytes).first(16 + length)};
  EXPECT_TRUE(validWifiTransferRequest(request, intent.transaction, intent.owner, intent.generation));
  EXPECT_FALSE(validWifiTransferRequest(request, identity(9), intent.owner, intent.generation));
  EXPECT_FALSE(validWifiTransferRequest(request, intent.transaction, identity(9), intent.generation));
  EXPECT_FALSE(validWifiTransferRequest(request, intent.transaction, intent.owner, identity(9)));
  bytes[24] ^= 1;
  EXPECT_FALSE(validWifiTransferRequest(request, intent.transaction, intent.owner, intent.generation));
}
TEST(CompanionWifiRequestTest, LegacyAndDeclaredBeginsRequireAllBindingsAndValidEncoding) {
  TransferDeclaration declaration;
  declaration.state.transaction = identity(1);
  declaration.state.owner = identity(2);
  declaration.state.storageGeneration = identity(3);
  declaration.state.contentHash.fill(4);
  declaration.state.length = 10;
  declaration.manifest.kind = ContentKind::Epub;
  declaration.manifest.length = 10;
  declaration.manifest.contentHash = declaration.state.contentHash;
  const auto& state = declaration.state;
  for (const bool declared : {false, true}) {
    std::array<uint8_t, MAX_CONTROL_PAYLOAD> bytes{};
    const auto size = declared ? encodeTransferDeclaration(declaration, bytes) : encodeRecord(state, bytes);
    ASSERT_NE(size, 0u);
    static constexpr char PATH[] = "/Books/Companion/book.epub";
    bytes[size] = sizeof(PATH) - 1;
    std::memcpy(bytes.data() + size + 1, PATH, sizeof(PATH) - 1);
    FrameView request{Command::BeginTransfer, false, 1, std::span(bytes).first(size + sizeof(PATH))};
    ASSERT_TRUE(validWifiTransferRequest(request, state.transaction, state.owner, state.storageGeneration));
    EXPECT_FALSE(validWifiTransferRequest(request, identity(9), state.owner, state.storageGeneration));
    EXPECT_FALSE(validWifiTransferRequest(request, state.transaction, identity(9), state.storageGeneration));
    EXPECT_FALSE(validWifiTransferRequest(request, state.transaction, state.owner, identity(9)));
    const auto original = request.payload;
    for (size_t length = 0; length < original.size(); ++length) {
      request.payload = original.first(length);
      EXPECT_FALSE(validWifiTransferRequest(request, state.transaction, state.owner, state.storageGeneration));
    }
    request.payload = original;
    bytes[0] = 2;
    EXPECT_FALSE(validWifiTransferRequest(request, state.transaction, state.owner, state.storageGeneration));
    bytes[0] = 1;
    bytes[size] += 1;
    EXPECT_FALSE(validWifiTransferRequest(request, state.transaction, state.owner, state.storageGeneration));
  }
}

TEST(CompanionWifiRequestTest, BackupExchangeRequiresMatchingHandoffTransactionAndCard) {
  LegacyBackupRequest backup;
  backup.course = identity(1);
  backup.transaction = identity(2);
  backup.generation = identity(3);
  std::array<uint8_t, 80> bytes{};
  std::copy(backup.transaction.begin(), backup.transaction.end(), bytes.begin());
  ASSERT_TRUE(encodeLegacyBackupRequest(backup, std::span(bytes).subspan(16)));
  FrameView request{Command::ExchangeChanges, false, 1, bytes};
  EXPECT_TRUE(validWifiTransferRequest(request, backup.transaction, identity(4), backup.generation));
  EXPECT_FALSE(validWifiTransferRequest(request, identity(5), identity(4), backup.generation));
  EXPECT_FALSE(validWifiTransferRequest(request, backup.transaction, identity(4), identity(5)));
  bytes[40] ^= 1;
  EXPECT_FALSE(validWifiTransferRequest(request, backup.transaction, identity(4), backup.generation));
}

TEST(CompanionWifiRequestTest, CourseSwitchConsentRequiresLeaseTransactionAndGeneration) {
  const auto transaction = identity(1), owner = identity(2), generation = identity(3);
  CourseSwitchRequest consent;
  consent.generation = generation;
  consent.transaction = transaction;
  consent.previousCourse = identity(4);
  consent.nextCourse = identity(5);
  consent.previousHash.fill(6);
  consent.nextHash.fill(7);
  std::array<uint8_t, 16 + COURSE_SWITCH_REQUEST_SIZE> bytes;
  std::copy(transaction.begin(), transaction.end(), bytes.begin());
  ASSERT_TRUE(encodeCourseSwitchRequest(consent, std::span(bytes).subspan(16)));
  FrameView request{Command::ExchangeChanges, false, 1, bytes};
  EXPECT_TRUE(validWifiTransferRequest(request, transaction, owner, generation));
  EXPECT_FALSE(validWifiTransferRequest(request, transaction, owner, identity(8)));
  bytes[36] ^= 1;
  EXPECT_FALSE(validWifiTransferRequest(request, transaction, owner, generation));
  bytes[36] ^= 1;
  bytes[0] ^= 1;
  EXPECT_FALSE(validWifiTransferRequest(request, transaction, owner, generation));
}

TEST(CompanionWifiRequestTest, RemovalBindsFullRequestToHandoffOwnerAndCard) {
  ContentRemovalRequest removal;
  removal.transaction = identity(1);
  removal.owner = identity(2);
  removal.generation = identity(3);
  removal.manifest.kind = ContentKind::Epub;
  removal.manifest.formatVersion = 1;
  removal.manifest.length = 123;
  removal.manifest.contentHash.fill(4);
  std::array<uint8_t, CONTENT_REMOVAL_REQUEST_SIZE + 1> bytes{};
  ASSERT_EQ(encodeContentRemovalRequest(removal, bytes), CONTENT_REMOVAL_REQUEST_SIZE);
  FrameView request{Command::RemoveContent, false, 1, std::span(bytes).first(CONTENT_REMOVAL_REQUEST_SIZE)};
  EXPECT_TRUE(validWifiTransferRequest(request, removal.transaction, removal.owner, removal.generation));
  EXPECT_FALSE(validWifiTransferRequest(request, identity(9), removal.owner, removal.generation));
  EXPECT_FALSE(validWifiTransferRequest(request, removal.transaction, identity(9), removal.generation));
  EXPECT_FALSE(validWifiTransferRequest(request, removal.transaction, removal.owner, identity(9)));
  request.response = true;
  EXPECT_FALSE(validWifiTransferRequest(request, removal.transaction, removal.owner, removal.generation));
  request.response = false;
  for (size_t length = 0; length <= bytes.size(); ++length) {
    if (length == CONTENT_REMOVAL_REQUEST_SIZE) continue;
    request.payload = std::span(bytes).first(length);
    EXPECT_FALSE(validWifiTransferRequest(request, removal.transaction, removal.owner, removal.generation));
  }
}

TEST(CompanionWifiRequestTest, MigrationAdmissionBindsHandoffOwnerAndGenerationAndValidatesEntireRecord) {
  TintaMigrationAdmission admission;
  admission.merge.generation = identity(1);
  admission.merge.owner = identity(2);
  admission.merge.transaction = identity(3);
  admission.merge.previous = {0, 512, {}};
  admission.merge.previous.frontier.fill(4);
  admission.merge.merged = {1, 1024, {}};
  admission.merge.merged.frontier.fill(5);
  admission.course = identity(6);
  admission.resource.fill(7);
  admission.backupTransaction = identity(8);
  admission.reader = identity(9);
  admission.backupManifest.fill(10);
  std::array<uint8_t, 272> bytes{};
  std::copy(admission.merge.transaction.begin(), admission.merge.transaction.end(), bytes.begin());
  ASSERT_TRUE(encodeTintaMigrationAdmission(admission, std::span(bytes).subspan(16)));
  FrameView request{Command::ExchangeChanges, false, 7, bytes};
  EXPECT_TRUE(validWifiTransferRequest(request, identity(3), identity(2), identity(1)));
  EXPECT_FALSE(validWifiTransferRequest(request, identity(11), identity(2), identity(1)));
  EXPECT_FALSE(validWifiTransferRequest(request, identity(3), identity(11), identity(1)));
  EXPECT_FALSE(validWifiTransferRequest(request, identity(3), identity(2), identity(11)));
  for (size_t at = 16; at < bytes.size(); ++at) {
    bytes[at] ^= 1;
    EXPECT_FALSE(validWifiTransferRequest(request, identity(3), identity(2), identity(1))) << at;
    bytes[at] ^= 1;
  }
  for (const size_t offset : {140u, 156u, 188u, 204u, 220u}) {
    auto corrupt = bytes;
    const auto length = offset == 156 || offset == 220 ? 32 : 16;
    std::fill_n(corrupt.begin() + 16 + offset, length, 0);
    tinta_body_detail::write(std::span(corrupt).subspan(16), 252, binary_record::crc32(corrupt.data() + 16, 252), 4);
    request.payload = corrupt;
    EXPECT_FALSE(validWifiTransferRequest(request, identity(3), identity(2), identity(1)));
  }
}

TEST(CompanionWifiRequestTest, ActiveJournalStateUsesSharedFixturesAndBindsCurrentCard) {
  const auto fixture = [](const char* name) {
    std::ifstream input(std::string(COMPANION_FIXTURE_DIR) + "/" + name, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  };
  const auto request = fixture("JournalStateRequest-v1.fixture"), reply = fixture("JournalStateReply-v1.fixture");
  ASSERT_EQ(request.size(), JOURNAL_STATE_REQUEST_SIZE);
  ASSERT_EQ(reply.size(), JOURNAL_STATE_REPLY_SIZE);
  Identity generation{};
  ASSERT_TRUE(decodeJournalStateRequest(request, generation));
  EXPECT_EQ(generation, identity(2));
  for (size_t size = 0; size < request.size(); ++size) {
    Identity preserved = identity(99);
    EXPECT_FALSE(decodeJournalStateRequest(std::span(request).first(size), preserved));
    EXPECT_EQ(preserved, identity(99));
  }
  JournalMergeSnapshot snapshot{0, 512, {}};
  std::copy_n(reply.begin() + 12, 32, snapshot.frontier.begin());
  std::array<uint8_t, JOURNAL_STATE_REPLY_SIZE> encoded{};
  ASSERT_TRUE(encodeJournalStateReply(snapshot, encoded));
  EXPECT_TRUE(std::equal(encoded.begin(), encoded.end(), reply.begin()));
  const auto unchanged = encoded;
  snapshot.recordSize = 513;
  EXPECT_FALSE(encodeJournalStateReply(snapshot, encoded));
  EXPECT_EQ(encoded, unchanged);
  snapshot.recordSize = 512;
  EXPECT_FALSE(encodeJournalStateReply(snapshot, std::span(encoded).first(43)));
  EXPECT_EQ(encoded, unchanged);
  std::vector<uint8_t> bound;
  bound.reserve(36);
  const auto transaction = identity(3);
  bound.insert(bound.end(), transaction.begin(), transaction.end());
  bound.insert(bound.end(), request.begin(), request.end());
  FrameView frame{Command::ExchangeChanges, false, 42, bound};
  EXPECT_TRUE(validWifiTransferRequest(frame, transaction, identity(4), generation));
  EXPECT_FALSE(validWifiTransferRequest(frame, identity(99), identity(4), generation));
  EXPECT_FALSE(validWifiTransferRequest(frame, transaction, identity(4), identity(99)));
  bound[19] ^= 1;
  EXPECT_FALSE(validWifiTransferRequest(frame, transaction, identity(4), generation));
}

TEST(CompanionWifiRequestTest, JournalMergeReadinessIsBoundAndMalformedInputPreservesOutputs) {
  std::array<uint8_t, JOURNAL_MERGE_READINESS_REQUEST_SIZE> request{};
  request[0] = 'J';
  request[1] = 'R';
  request[2] = 'D';
  request[3] = 1;
  std::fill_n(request.begin() + 4, 16, uint8_t{2});
  request[25] = 2;
  std::ifstream requestFile(std::string(COMPANION_FIXTURE_DIR) + "/JournalMergeReadinessRequest-v1.fixture",
                            std::ios::binary);
  const std::vector<uint8_t> fixture{std::istreambuf_iterator<char>(requestFile), std::istreambuf_iterator<char>()};
  ASSERT_EQ(fixture.size(), request.size());
  std::copy_n(fixture.begin() + 28, 32, request.begin() + 28);
  EXPECT_TRUE(std::equal(request.begin(), request.end(), fixture.begin()));
  Identity generation{};
  JournalMergeSnapshot snapshot;
  ASSERT_TRUE(decodeJournalMergeReadinessRequest(request, generation, snapshot));
  EXPECT_EQ(generation, identity(2));
  EXPECT_EQ(snapshot.count, 0u);
  EXPECT_EQ(snapshot.recordSize, 512u);
  request[26] = 1;
  auto preserved = snapshot;
  generation = identity(99);
  EXPECT_FALSE(decodeJournalMergeReadinessRequest(request, generation, snapshot));
  EXPECT_EQ(generation, identity(99));
  EXPECT_EQ(snapshot.count, preserved.count);
  EXPECT_EQ(snapshot.frontier, preserved.frontier);
  request[26] = 0;
  std::array<uint8_t, 76> bound{};
  const auto transaction = identity(3);
  std::copy(transaction.begin(), transaction.end(), bound.begin());
  std::copy(request.begin(), request.end(), bound.begin() + 16);
  FrameView frame{Command::ExchangeChanges, false, 42, bound};
  EXPECT_TRUE(validWifiTransferRequest(frame, transaction, identity(4), identity(2)));
  EXPECT_FALSE(validWifiTransferRequest(frame, transaction, identity(4), identity(99)));
  std::array<uint8_t, 8> reply{};
  ASSERT_TRUE(encodeJournalMergeReadinessReply(JournalMergeReadiness::Ready, reply));
  const std::array<uint8_t, 8> expected{'J', 'R', 'R', 1, 0, 0, 0, 0};
  EXPECT_EQ(reply, expected);
  std::ifstream replyFile(std::string(COMPANION_FIXTURE_DIR) + "/JournalMergeReadinessReady-v1.fixture",
                          std::ios::binary);
  const std::vector<uint8_t> replyFixture{std::istreambuf_iterator<char>(replyFile), std::istreambuf_iterator<char>()};
  ASSERT_EQ(replyFixture.size(), reply.size());
  EXPECT_TRUE(std::equal(reply.begin(), reply.end(), replyFixture.begin()));
}
