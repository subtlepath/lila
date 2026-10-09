#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "lib/Companion/CompanionCourseRemovalParticipant.h"
using namespace companion;
namespace {
struct JournalFiles final : ContentRemovalJournalStorage {
  std::map<std::string, std::vector<uint8_t>> files;
  bool prepare() override { return true; }
  FileStatus stat(const char* path, uint64_t& size) override {
    if (!files.contains(path)) return FileStatus::Missing;
    size = files.at(path).size();
    return FileStatus::Present;
  }
  bool read(const char* path, std::span<uint8_t> output) override {
    if (!files.contains(path) || files.at(path).size() != output.size()) return false;
    std::copy(files.at(path).begin(), files.at(path).end(), output.begin());
    return true;
  }
  bool write(const char* path, std::span<const uint8_t> input) override {
    files[path] = {input.begin(), input.end()};
    return true;
  }
};
struct Proof {
  uint64_t length;
  Digest hash;
  bool operator==(const Proof&) const = default;
};
CourseRemovalPlan makePlan() {
  CourseRemovalPlan p;
  p.request.transaction.fill(1);
  p.request.owner.fill(2);
  p.request.generation.fill(3);
  p.request.manifest.kind = ContentKind::Course;
  p.request.manifest.formatVersion = 1;
  p.request.manifest.length = 1000;
  p.request.manifest.contentHash.fill(4);
  p.request.manifest.logicalIdentity.fill(5);
  return p;
}
struct Members final : CourseRemovalStorage {
  CourseRemovalPlan expected;
  Digest digest;
  std::array<std::optional<Proof>, 4> original, saved;
  unsigned operations = 0, failAt = 0, mutations = 0, retired = 0;
  bool afterEffect = false;
  ContentRemovalJournal* invalidate = nullptr;
  ContentRemovalRecord foreign;
  explicit Members(const CourseRemovalPlan& p) : expected(p) {
    digest.fill(8);
    for (unsigned member = 0; member < 4; ++member)
      original[member] = Proof{p.request.manifest.length, p.request.manifest.contentHash};
  }
  bool fail() { return ++operations == failAt; }
  bool verifyPlan(const CourseRemovalPlan& p, const Digest& hash) override {
    return !fail() && p == expected && hash == digest;
  }
  FileStatus stat(unsigned member, bool backup) override {
    if (fail()) return FileStatus::Error;
    return (backup ? saved : original)[member] ? FileStatus::Present : FileStatus::Missing;
  }
  bool verify(unsigned member, bool backup, uint64_t length, const Digest& hash) override {
    const auto& file = (backup ? saved : original)[member];
    return !fail() && file && *file == (Proof{length, hash});
  }
  bool quarantine(unsigned member) override {
    bool failed = fail();
    if (failed && !afterEffect) return false;
    if (!original[member] || saved[member]) return false;
    saved[member] = original[member];
    original[member].reset();
    ++mutations;
    if (invalidate) {
      EXPECT_EQ(invalidate->recover(foreign), ContentRemovalJournalResult::Conflict);
    }
    return !failed;
  }
  bool remove(unsigned member, bool backup) override {
    bool failed = fail();
    if (failed && !afterEffect) return false;
    auto& file = (backup ? saved : original)[member];
    if (!file) return false;
    file.reset();
    ++mutations;
    if (backup) ++retired;
    return !failed;
  }
};
struct References final : CourseRemovalReferences {
  bool isolated = true, published = false, retired = false, failPublish = false, failRetire = false;
  const std::array<unsigned, 3> learnerState = {17, 42, 99};
  Members* corruptOnRetire = nullptr;
  bool verifyPlan(const ContentRemovalRecord&, const CourseRemovalPlan&) override { return isolated; }
  bool publish(const ContentRemovalRecord&, const CourseRemovalPlan&) override {
    published = true;
    return !failPublish;
  }
  bool verify(const ContentRemovalRecord&, const CourseRemovalPlan&) override { return published; }
  bool retire(const ContentRemovalRecord&, const CourseRemovalPlan&) override {
    if (corruptOnRetire) corruptOnRetire->saved[0]->hash[0] ^= 1;
    retired = true;
    return !failRetire;
  }
  bool verifyRetired(const ContentRemovalRecord&, const CourseRemovalPlan&) override { return published && retired; }
};
ContentRemovalRecord seed(const CourseRemovalPlan& p, const Members& members) {
  ContentRemovalRecord r;
  r.request = p.request;
  r.planHash = members.digest;
  return r;
}
}  // namespace
TEST(CourseRemovalParticipant, StorageFaultsRecoverWithoutTouchingOtherContentOrHistory) {
  const auto plan = makePlan();
  unsigned normalOperations = 0;
  for (bool after : {false, true}) {
    for (unsigned fault = 0; fault <= normalOperations; ++fault) {
      JournalFiles files;
      std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> scratch{};
      ContentRemovalJournal journal(files, scratch);
      Members members(plan);
      members.failAt = fault;
      members.afterEffect = after;
      const auto unrelated = members.original[1];
      References refs;
      const auto history = refs.learnerState;
      CourseRemovalParticipant participant(journal, members, refs);
      if (participant.bind(plan, members.digest)) {
        ContentRemoval removal(journal, participant);
        const auto result = removal.remove(seed(plan, members));
        EXPECT_EQ(result == ContentRemovalJournalResult::Ok, fault == 0);
      } else {
        EXPECT_EQ(fault, 1U);
      }
      if (!fault) normalOperations = members.operations;
      members.failAt = 0;
      ContentRemovalJournal recovered(files, scratch);
      CourseRemovalParticipant resumed(recovered, members, refs);
      ASSERT_TRUE(resumed.bind(plan, members.digest));
      ContentRemoval retry(recovered, resumed);
      ASSERT_EQ(retry.remove(seed(plan, members)), ContentRemovalJournalResult::Ok) << fault << "/" << after;
      EXPECT_FALSE(members.original[0]);
      EXPECT_FALSE(members.saved[0]);
      EXPECT_EQ(members.original[1], unrelated);
      EXPECT_EQ(refs.learnerState, history);
      const auto mutations = members.mutations;
      ASSERT_EQ(retry.remove(seed(plan, members)), ContentRemovalJournalResult::Ok);
      EXPECT_EQ(members.mutations, mutations);
    }
  }
  EXPECT_GT(normalOperations, 0U);
}
TEST(CourseRemovalParticipant, UnisolatedStateAndCorruptPackCannotCreateRemovalJournal) {
  for (bool corrupt : {false, true}) {
    const auto plan = makePlan();
    JournalFiles files;
    std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> scratch{};
    ContentRemovalJournal journal(files, scratch);
    Members members(plan);
    References refs;
    if (corrupt)
      members.original[0]->hash[0] ^= 1;
    else
      refs.isolated = false;
    CourseRemovalParticipant participant(journal, members, refs);
    ASSERT_TRUE(participant.bind(plan, members.digest));
    ContentRemoval removal(journal, participant);
    EXPECT_EQ(removal.remove(seed(plan, members)), ContentRemovalJournalResult::Conflict);
    EXPECT_EQ(members.mutations, 0U);
    EXPECT_TRUE(files.files.empty());
  }
}
TEST(CourseRemovalParticipant, ReferencePublicationOrRetirementFailureRetainsVerifiedPack) {
  for (bool retire : {false, true}) {
    const auto plan = makePlan();
    JournalFiles files;
    std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> scratch{};
    ContentRemovalJournal journal(files, scratch);
    Members members(plan);
    References refs;
    refs.failPublish = !retire;
    refs.failRetire = retire;
    CourseRemovalParticipant participant(journal, members, refs);
    ASSERT_TRUE(participant.bind(plan, members.digest));
    ContentRemoval removal(journal, participant);
    EXPECT_EQ(removal.remove(seed(plan, members)), ContentRemovalJournalResult::IoError);
    EXPECT_TRUE(members.saved[0]);
    EXPECT_EQ(members.retired, 0U);
    refs.failPublish = refs.failRetire = false;
    ContentRemovalJournal recovered(files, scratch);
    CourseRemovalParticipant resumed(recovered, members, refs);
    ASSERT_TRUE(resumed.bind(plan, members.digest));
    ContentRemoval retry(recovered, resumed);
    EXPECT_EQ(retry.remove(seed(plan, members)), ContentRemovalJournalResult::Ok);
  }
}
