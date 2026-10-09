#include <gtest/gtest.h>

#include "lib/Companion/CompanionCourseRemovalProof.h"
#include "lib/Companion/CompanionHistoricalCourseBaseline.h"

using namespace companion;
namespace {
ContentRemovalRecord proof() {
  ContentRemovalRecord r;
  r.request.transaction.fill(1);
  r.request.owner.fill(2);
  r.request.generation.fill(3);
  r.request.manifest.kind = ContentKind::Course;
  r.request.manifest.formatVersion = 1;
  r.request.manifest.length = 1000;
  r.request.manifest.contentHash.fill(4);
  r.request.manifest.logicalIdentity.fill(5);
  r.planHash.fill(0x66);
  r.phase = ContentRemovalPhase::Quarantined;
  r.revision = 2;
  return r;
}
}  // namespace
TEST(CourseRemovalProof, OnlyExactCompletedRemovalAndRetainedBindingAuthorizeBaseline) {
  const auto p = proof();
  auto checkpoint = p;
  EXPECT_TRUE(validCourseRemovalProof(p));
  for (auto phase : {ContentRemovalPhase::Quarantined, ContentRemovalPhase::Committed, ContentRemovalPhase::Retired}) {
    checkpoint.phase = phase;
    checkpoint.revision = uint64_t{static_cast<uint8_t>(phase)} + 1;
    EXPECT_TRUE(matchesCourseRemovalProof(p, checkpoint));
    EXPECT_EQ(completedCourseRemovalProof(p, checkpoint, p.request.manifest, p.request.generation),
              phase == ContentRemovalPhase::Retired);
  }
  checkpoint.phase = ContentRemovalPhase::Prepared;
  checkpoint.revision = 1;
  EXPECT_FALSE(matchesCourseRemovalProof(p, checkpoint));
}
TEST(CourseRemovalProof, ForeignRequestsHashesAndBindingsCannotReuseCompletedProof) {
  const auto p = proof();
  auto completed = p;
  completed.phase = ContentRemovalPhase::Retired;
  completed.revision = 4;
  Identity replacedCard;
  replacedCard.fill(9);
  EXPECT_FALSE(completedCourseRemovalProof(p, completed, p.request.manifest, replacedCard));
  for (unsigned field = 0; field < 9; ++field) {
    auto changed = completed;
    switch (field) {
      case 0:
        changed.request.transaction[0] ^= 1;
        break;
      case 1:
        changed.request.owner[0] ^= 1;
        break;
      case 2:
        changed.request.generation[0] ^= 1;
        break;
      case 3:
        changed.request.manifest.contentHash[0] ^= 1;
        break;
      case 4:
        changed.request.manifest.logicalIdentity[0] ^= 1;
        break;
      case 5:
        ++changed.request.manifest.length;
        break;
      case 6:
        changed.request.manifest.formatVersion = 2;
        break;
      case 7:
        changed.planHash[0] ^= 1;
        break;
      case 8:
        ++changed.revision;
        break;
    }
    EXPECT_FALSE(completedCourseRemovalProof(p, changed, p.request.manifest, p.request.generation));
  }
  auto binding = p.request.manifest;
  binding.contentHash[0] ^= 1;
  EXPECT_FALSE(completedCourseRemovalProof(p, completed, binding, p.request.generation));
  binding = p.request.manifest;
  binding.logicalIdentity[0] ^= 1;
  EXPECT_FALSE(completedCourseRemovalProof(p, completed, binding, p.request.generation));
}
TEST(CourseRemovalProof, CacheAddressIsBoundToSealedPlanAndRefusalPreservesOutput) {
  const auto p = proof();
  std::array<char, COURSE_REMOVAL_CACHE_PATH_CAPACITY + 1> path;
  path.fill('!');
  ASSERT_TRUE(courseRemovalCachePath(p, path));
  EXPECT_EQ(std::string(path.data()), std::string(COURSE_REMOVAL_CACHE_PREFIX) + std::string(64, '6'));
  EXPECT_EQ(path.back(), '!');
  auto other = p;
  other.planHash[0] = 7;
  ASSERT_TRUE(courseRemovalCachePath(other, path));
  EXPECT_EQ(std::string(path.data()), std::string(COURSE_REMOVAL_CACHE_PREFIX) + "07" + std::string(62, '6'));
  const auto unchanged = path;
  EXPECT_FALSE(courseRemovalCachePath(p, std::span(path).first(COURSE_REMOVAL_CACHE_PATH_CAPACITY - 1)));
  EXPECT_EQ(path, unchanged);
  other.phase = ContentRemovalPhase::Prepared;
  other.revision = 1;
  EXPECT_FALSE(courseRemovalCachePath(other, path));
  EXPECT_EQ(path, unchanged);
}

TEST(CourseRemovalProof, HistoricalCacheAddressUsesCompletedReceiptAndExactSealedPlan) {
  auto completed = proof();
  completed.phase = ContentRemovalPhase::Retired;
  completed.revision = 4;
  CourseRemovalPlan plan{completed.request};
  std::array<char, COURSE_REMOVAL_CACHE_PATH_CAPACITY> historical{}, original{};
  ASSERT_TRUE(historicalCourseRemovalCachePath(completed, plan, completed.planHash, completed.request.generation,
                                               completed.request.manifest.logicalIdentity, historical));
  ASSERT_TRUE(courseRemovalCachePath(proof(), original));
  EXPECT_EQ(historical, original);
  // A historical address does not change the current-course proof requirement.
  EXPECT_FALSE(validCourseRemovalProof(completed));
}

TEST(CourseRemovalProof, HistoricalCacheRefusesForeignOrUnfinishedEvidenceWithoutChangingOutput) {
  auto completed = proof();
  completed.phase = ContentRemovalPhase::Retired;
  completed.revision = 4;
  const auto original = completed;
  std::array<char, COURSE_REMOVAL_CACHE_PATH_CAPACITY> path;
  path.fill('!');
  const auto unchanged = path;
  for (unsigned fault = 0; fault < 8; ++fault) {
    SCOPED_TRACE(fault);
    completed = original;
    CourseRemovalPlan plan{completed.request};
    auto digest = completed.planHash;
    auto generation = completed.request.generation;
    auto course = completed.request.manifest.logicalIdentity;
    switch (fault) {
      case 0:
        completed.phase = ContentRemovalPhase::Committed;
        completed.revision = 3;
        break;
      case 1:
        plan.request.owner[0] ^= 1;
        break;
      case 2:
        digest[0] ^= 1;
        break;
      case 3:
        generation[0] ^= 1;
        break;
      case 4:
        course[0] ^= 1;
        break;
      case 5:
        completed.request.manifest.formatVersion = 2;
        plan.request = completed.request;
        break;
      case 6:
        completed.request.manifest.kind = ContentKind::Epub;
        plan.request = completed.request;
        break;
      case 7:
        completed.revision = 3;
        break;
    }
    EXPECT_FALSE(historicalCourseRemovalCachePath(completed, plan, digest, generation, course, path));
    EXPECT_EQ(path, unchanged);
  }
  CourseRemovalPlan plan{original.request};
  EXPECT_FALSE(historicalCourseRemovalCachePath(original, plan, original.planHash, original.request.generation,
                                                original.request.manifest.logicalIdentity,
                                                std::span(path).first(path.size() - 1)));
  EXPECT_EQ(path, unchanged);
}
