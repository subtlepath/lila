#include <gtest/gtest.h>

#include "lib/hal/HalContentRemovalTransactions.h"
using namespace companion;
namespace {
ContentRemovalRecord initial(uint8_t id = 1) {
  ContentRemovalRecord result;
  result.request.transaction.fill(id);
  result.request.owner.fill(2);
  result.request.generation.fill(3);
  result.request.manifest.kind = ContentKind::Epub;
  result.request.manifest.formatVersion = 1;
  result.request.manifest.length = 123;
  result.request.manifest.contentHash.fill(4);
  result.planHash.fill(id + 4);
  return result;
}
struct Participant final : ContentRemovalParticipant {
  unsigned mutations = 0, checks = 0;
  bool deny = false;
  bool verifyPlan(const ContentRemovalRecord&) override {
    ++checks;
    return !deny;
  }
  bool quarantine(const ContentRemovalRecord&) override {
    ++mutations;
    return !deny;
  }
  bool verifyQuarantined(const ContentRemovalRecord&) override {
    ++checks;
    return !deny;
  }
  bool publishRemoval(const ContentRemovalRecord&) override {
    ++mutations;
    return !deny;
  }
  bool verifyPublished(const ContentRemovalRecord&) override {
    ++checks;
    return !deny;
  }
  bool retireBackups(const ContentRemovalRecord&) override {
    ++mutations;
    return !deny;
  }
  bool verifyRetired(const ContentRemovalRecord&) override {
    ++checks;
    return !deny;
  }
};
struct Session {
  std::array<uint8_t, 168> journalScratch{}, receiptScratch{}, releaseScratch{};
  HalContentRemovalJournalStorage storage;
  ContentRemovalJournal journal{storage, journalScratch};
  HalCompletedContentRemovals completions{receiptScratch};
  Participant participant;
  HalContentRemovalTransactions transactions{
      initial().request.generation, journal, storage, completions, participant, releaseScratch};
};
std::string receiptPath(uint8_t id) {
  std::string result = "/.crosspoint/companion/removal-done-";
  for (unsigned at = 0; at < 16; ++at) result += id == 1 ? "01" : "02";
  return result;
}
}  // namespace
TEST(ContentRemovalTransactions, CompletedRetriesIgnoreLaterMetadataAndParticipant) {
  inventory_hal_test::state = {};
  Session session;
  const auto request = initial();
  ASSERT_EQ(session.transactions.remove(request), ContentRemovalJournalResult::Ok);
  session.participant.deny = true;
  const auto mutations = session.participant.mutations, checks = session.participant.checks;
  const std::string changedState = R"({"openEpubPath":"/other.epub"})";
  inventory_hal_test::state.files["/.crosspoint/state.json"] = {changedState.begin(), changedState.end()};
  ASSERT_EQ(session.transactions.remove(request), ContentRemovalJournalResult::Ok);
  EXPECT_EQ(session.participant.mutations, mutations);
  EXPECT_EQ(session.participant.checks, checks);
  ContentRemovalRecord reply;
  EXPECT_EQ(session.transactions.lookup(request.request, reply), CompletedRemovalResult::Ok);
  EXPECT_EQ(reply.phase, ContentRemovalPhase::Retired);
  auto changedCard = request.request;
  changedCard.generation.fill(8);
  EXPECT_EQ(session.transactions.lookup(changedCard, reply), CompletedRemovalResult::Conflict);
}
TEST(ContentRemovalTransactions, NewRequestRotatesOnlyAfterDurableCompletion) {
  inventory_hal_test::state = {};
  Session session;
  const auto a = initial(), b = initial(2);
  ASSERT_EQ(session.transactions.remove(a), ContentRemovalJournalResult::Ok);
  ASSERT_EQ(session.transactions.remove(b), ContentRemovalJournalResult::Ok);
  EXPECT_EQ(session.participant.mutations, 6);
  const auto mutations = session.participant.mutations;
  EXPECT_EQ(session.transactions.remove(a), ContentRemovalJournalResult::Ok);
  EXPECT_EQ(session.participant.mutations, mutations);
  EXPECT_EQ(session.journal.current()->request, b.request);
}
TEST(ContentRemovalTransactions, ActiveForeignRequestBlocksNewParticipant) {
  inventory_hal_test::state = {};
  Session session;
  const auto a = initial(), b = initial(2);
  ASSERT_EQ(session.journal.begin(a), ContentRemovalJournalResult::Ok);
  EXPECT_EQ(session.transactions.remove(b), ContentRemovalJournalResult::Conflict);
  EXPECT_EQ(session.participant.mutations, 0);
  EXPECT_EQ(session.participant.checks, 0);
}
TEST(ContentRemovalTransactions, ReceiptPersistenceAndReleaseFailuresRecoverWithoutRepeatingWork) {
  for (unsigned fault = 0; fault < 2; ++fault) {
    auto& state = inventory_hal_test::state;
    state = {};
    Session session;
    const auto a = initial(), b = initial(2);
    if (!fault) state.failSyncPath = receiptPath(1) + ".tmp";
    if (!fault)
      EXPECT_EQ(session.transactions.remove(a), ContentRemovalJournalResult::IoError);
    else
      ASSERT_EQ(session.transactions.remove(a), ContentRemovalJournalResult::Ok);
    state.failSyncPath.clear();
    session.participant.deny = true;
    EXPECT_EQ(session.transactions.remove(a), ContentRemovalJournalResult::Ok);
    EXPECT_EQ(session.participant.mutations, 3);
    session.participant.deny = false;
    state.failRemoveAfter = true;
    EXPECT_EQ(session.transactions.remove(b), ContentRemovalJournalResult::IoError);
    EXPECT_EQ(session.participant.mutations, 3);
    state.failRemoveAfter = false;
    EXPECT_EQ(session.transactions.remove(b), ContentRemovalJournalResult::Ok);
    EXPECT_EQ(session.participant.mutations, 6);
  }
}
