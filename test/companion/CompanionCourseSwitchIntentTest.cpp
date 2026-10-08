#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "lib/Companion/CompanionCourseMigrationProof.h"
#include "lib/Companion/CompanionCourseSwitchPublication.h"

namespace {
class Storage final : public companion::TransferStorage {
 public:
  std::map<std::string, std::vector<uint8_t>> files;
  int fail = 0, mutations = 0;
  bool after = false;
  bool prepare() override { return true; }
  companion::FileStatus stat(const char* path, uint64_t& size) override {
    if (!files.contains(path)) return companion::FileStatus::Missing;
    size = files[path].size();
    return companion::FileStatus::Present;
  }
  bool read(const char* path, uint64_t offset, std::span<uint8_t> bytes) override {
    if (!files.contains(path) || offset > files[path].size() || bytes.size() > files[path].size() - offset)
      return false;
    std::memcpy(bytes.data(), files[path].data() + offset, bytes.size());
    return true;
  }
  bool write(const char* path, uint64_t offset, std::span<const uint8_t> bytes, bool truncate) override {
    if (offset || !truncate) return false;
    const bool failed = ++mutations == fail;
    if (failed && !after) return false;
    files[path] = {bytes.begin(), bytes.end()};
    return !failed;
  }
  bool rename(const char* from, const char* to) override {
    const bool failed = ++mutations == fail;
    if (failed && !after) return false;
    if (!files.contains(from) || files.contains(to)) return false;
    files[to] = std::move(files[from]);
    files.erase(from);
    return !failed;
  }
  bool resize(const char*, uint64_t) override { return false; }
  bool remove(const char* path) override {
    const bool failed = ++mutations == fail;
    if (failed && !after) return false;
    if (!files.contains(path)) return false;
    files.erase(path);
    return !failed;
  }
  bool verify(const char* path, uint64_t length, const companion::Digest& hash, std::span<uint8_t>) override {
    return files.contains(path) && files[path].size() == length && !files[path].empty() && files[path][0] == hash[0];
  }
};
companion::CourseSwitchRequest request() {
  companion::CourseSwitchRequest result;
  result.generation.fill(1);
  result.transaction.fill(2);
  result.previousCourse.fill(3);
  result.nextCourse.fill(4);
  result.previousHash.fill(5);
  result.nextHash.fill(6);
  return result;
}
}  // namespace
TEST(CompanionCourseSwitchIntent, DurableIdempotentAndConflictingConsentPreserved) {
  Storage storage;
  std::array<uint8_t, companion::COURSE_SWITCH_INTENT_SIZE> scratch;
  companion::CourseSwitchIntent intent(storage, scratch);
  const auto expected = request();
  EXPECT_EQ(intent.persist(expected), companion::CourseSwitchIntentResult::Ok);
  const auto files = storage.files;
  EXPECT_EQ(intent.persist(expected), companion::CourseSwitchIntentResult::Ok);
  EXPECT_EQ(storage.mutations, 2);
  auto other = expected;
  other.transaction[0]++;
  EXPECT_EQ(intent.persist(other), companion::CourseSwitchIntentResult::Conflict);
  EXPECT_EQ(storage.files, files);
  companion::CourseSwitchRequest loaded;
  EXPECT_EQ(intent.load(loaded), companion::CourseSwitchIntentResult::Ok);
  EXPECT_EQ(loaded, expected);
}
TEST(CompanionCourseSwitchIntent, EveryInterruptedMutationResumesExactConsent) {
  for (int failure = 1; failure <= 2; ++failure) {
    for (bool after : {false, true}) {
      Storage storage;
      storage.fail = failure;
      storage.after = after;
      std::array<uint8_t, companion::COURSE_SWITCH_INTENT_SIZE> scratch;
      companion::CourseSwitchIntent intent(storage, scratch);
      EXPECT_EQ(intent.persist(request()), companion::CourseSwitchIntentResult::IoError);
      storage.fail = 0;
      companion::CourseSwitchIntent restarted(storage, scratch);
      EXPECT_EQ(restarted.persist(request()), companion::CourseSwitchIntentResult::Ok);
      companion::CourseSwitchRequest loaded;
      EXPECT_EQ(restarted.load(loaded), companion::CourseSwitchIntentResult::Ok);
      EXPECT_EQ(loaded, request());
    }
  }
}
TEST(CompanionCourseSwitchIntent, TornAndCorruptRecordsPreservedWithoutChangingOutput) {
  for (const auto* path : {companion::COURSE_SWITCH_INTENT_PATH, companion::COURSE_SWITCH_INTENT_STAGE}) {
    Storage storage;
    storage.files[path] = {1, 2, 3};
    const auto saved = storage.files;
    std::array<uint8_t, companion::COURSE_SWITCH_INTENT_SIZE> scratch;
    companion::CourseSwitchIntent intent(storage, scratch);
    EXPECT_EQ(intent.persist(request()), companion::CourseSwitchIntentResult::Corrupt);
    EXPECT_EQ(storage.files, saved);
    EXPECT_EQ(storage.mutations, 0);
  }
  Storage storage;
  std::array<uint8_t, companion::COURSE_SWITCH_INTENT_SIZE> scratch;
  companion::CourseSwitchIntent intent(storage, scratch);
  ASSERT_EQ(intent.persist(request()), companion::CourseSwitchIntentResult::Ok);
  storage.files[companion::COURSE_SWITCH_INTENT_PATH][20] ^= 1;
  auto output = request();
  const auto original = output;
  EXPECT_EQ(intent.load(output), companion::CourseSwitchIntentResult::Corrupt);
  EXPECT_EQ(output, original);
}

TEST(CompanionCourseSwitchPublication, EveryInterruptedPublicationResumesAndPreservesConsent) {
  const auto consent = request();
  companion::ContentManifest previous;
  previous.kind = companion::ContentKind::Course;
  previous.length = 6;
  previous.formatVersion = 1;
  previous.logicalIdentity = consent.previousCourse;
  previous.contentHash = consent.previousHash;
  auto next = previous;
  next.logicalIdentity = consent.nextCourse;
  next.contentHash = consent.nextHash;
  for (int failure = 0; failure <= 4; ++failure) {
    for (bool after : {false, true}) {
      Storage storage;
      std::array<uint8_t, companion::COURSE_SWITCH_INTENT_SIZE> scratch;
      companion::CourseSwitchIntent intent(storage, scratch);
      ASSERT_EQ(intent.persist(consent), companion::CourseSwitchIntentResult::Ok);
      std::array<uint8_t, companion::COURSE_BINDING_SIZE> binding;
      ASSERT_EQ(companion::encodeCourseBinding(previous, binding), binding.size());
      storage.files[companion::COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
      storage.files[companion::ACTIVE_COURSE_PATH] = std::vector<uint8_t>(6, 6);
      const auto originalIntent = storage.files[companion::COURSE_SWITCH_INTENT_PATH];
      storage.mutations = 0;
      storage.fail = failure;
      storage.after = after;
      auto result = companion::publishSwitchedCourseBinding(storage, consent, next, scratch);
      EXPECT_EQ(result,
                failure ? companion::CourseSwitchIntentResult::IoError : companion::CourseSwitchIntentResult::Ok);
      storage.fail = 0;
      EXPECT_EQ(companion::publishSwitchedCourseBinding(storage, consent, next, scratch),
                companion::CourseSwitchIntentResult::Ok);
      companion::ContentManifest loaded;
      bool present = false;
      EXPECT_EQ(companion::readCourseBinding(storage, companion::COURSE_BINDING_PATH, scratch, loaded, present),
                companion::CourseBindingResult::Ok);
      EXPECT_TRUE(present);
      EXPECT_EQ(loaded, next);
      EXPECT_FALSE(storage.files.contains(companion::COURSE_BINDING_BACKUP));
      EXPECT_FALSE(storage.files.contains(companion::COURSE_BINDING_STAGE));
      EXPECT_EQ(storage.files[companion::COURSE_SWITCH_INTENT_PATH], originalIntent);
    }
  }
}

TEST(CompanionCourseSwitchPublication, ForeignBindingsAndUnverifiedPackRemainUntouched) {
  const auto consent = request();
  companion::ContentManifest previous;
  previous.kind = companion::ContentKind::Course;
  previous.length = 6;
  previous.formatVersion = 1;
  previous.logicalIdentity = consent.previousCourse;
  previous.contentHash = consent.previousHash;
  auto next = previous;
  next.logicalIdentity = consent.nextCourse;
  next.contentHash = consent.nextHash;
  for (unsigned conflict = 0; conflict < 5; ++conflict) {
    Storage storage;
    std::array<uint8_t, companion::COURSE_SWITCH_INTENT_SIZE> scratch;
    companion::CourseSwitchIntent intent(storage, scratch);
    ASSERT_EQ(intent.persist(consent), companion::CourseSwitchIntentResult::Ok);
    std::array<uint8_t, companion::COURSE_BINDING_SIZE> binding;
    ASSERT_EQ(companion::encodeCourseBinding(previous, binding), binding.size());
    storage.files[companion::COURSE_BINDING_PATH] = {binding.begin(), binding.end()};
    storage.files[companion::ACTIVE_COURSE_PATH] = std::vector<uint8_t>(6, 6);
    switch (conflict) {
      case 0:
        storage.files[companion::ACTIVE_COURSE_PATH][0] = 9;
        break;
      case 1:
        storage.files[companion::COURSE_BINDING_STAGE] = {1, 2, 3};
        break;
      case 2:
        storage.files[companion::COURSE_BINDING_BACKUP] = {binding.begin(), binding.end()};
        break;
      case 3:
        storage.files.erase(companion::COURSE_BINDING_PATH);
        break;
      case 4:
        storage.files[companion::COURSE_SWITCH_INTENT_PATH][20] ^= 1;
        break;
    }
    const auto original = storage.files;
    const auto mutations = storage.mutations;
    EXPECT_NE(companion::publishSwitchedCourseBinding(storage, consent, next, scratch),
              companion::CourseSwitchIntentResult::Ok);
    EXPECT_EQ(storage.files, original);
    EXPECT_EQ(storage.mutations, mutations);
  }
}

TEST(CompanionCourseMigrationProof, CompletedOriginalMigrationIsReadOnlyAndRejectsCrossedRecords) {
  static constexpr const char* FILES[] = {"items.bin", "reviews.log"};
  static constexpr companion::CourseMigrationPaths PATHS{
      companion::COURSE_STATE_MIGRATION, companion::COURSE_STATE_MIGRATION_STAGE,
      companion::COURSE_STATE_MIGRATION_DONE, companion::COURSE_STATE_MIGRATION_DONE_STAGE};
  const auto consent = request();
  Storage storage;
  std::array<uint8_t, 512> scratch;
  companion::ContentManifest previous;
  previous.kind = companion::ContentKind::Course;
  previous.length = 6;
  previous.formatVersion = 1;
  previous.logicalIdentity = consent.previousCourse;
  previous.contentHash = consent.previousHash;
  ASSERT_EQ(companion::encodeCourseBinding(previous, std::span(scratch).first(companion::COURSE_BINDING_SIZE)),
            companion::COURSE_BINDING_SIZE);
  storage.files[companion::COURSE_BINDING_PATH] = {scratch.begin(), scratch.begin() + companion::COURSE_BINDING_SIZE};
  storage.files[companion::ACTIVE_COURSE_PATH] = std::vector<uint8_t>(6, 5);
  storage.files["/tinta/items.bin"] = {8, 9};
  ASSERT_EQ(companion::migrateLegacyCourseFiles(storage, consent.previousCourse, scratch, FILES, PATHS),
            companion::CourseStateMigrationResult::Ok);
  const auto migrated = storage.files;
  const auto mutations = storage.mutations;
  companion::Identity origin{};
  ASSERT_EQ(companion::completedCourseMigration(storage, PATHS, FILES, scratch, origin),
            companion::CourseStateMigrationResult::Ok);
  EXPECT_EQ(origin, consent.previousCourse);
  EXPECT_EQ(storage.files, migrated);
  EXPECT_EQ(storage.mutations, mutations);
  for (unsigned conflict = 0; conflict < 5; ++conflict) {
    storage.files = migrated;
    switch (conflict) {
      case 0:
        storage.files[PATHS.stage] = {1};
        break;
      case 1:
        storage.files[PATHS.doneStage] = {1};
        break;
      case 2:
        storage.files["/tinta/reviews.log"] = {1};
        break;
      case 3:
        storage.files[PATHS.intent][20] ^= 1;
        break;
      case 4: {
        auto& done = storage.files[PATHS.done];
        done[4] ^= 1;
        const auto crc = companion::courseBindingCrc(std::span(done).first(20));
        for (size_t i = 0; i < 4; ++i) done[20 + i] = static_cast<uint8_t>(crc >> (8 * i));
        break;
      }
    }
    origin = consent.nextCourse;
    const auto before = storage.files;
    EXPECT_NE(companion::completedCourseMigration(storage, PATHS, FILES, scratch, origin),
              companion::CourseStateMigrationResult::Ok);
    EXPECT_EQ(origin, consent.nextCourse);
    EXPECT_EQ(storage.files, before);
    EXPECT_EQ(storage.mutations, mutations);
  }
}
