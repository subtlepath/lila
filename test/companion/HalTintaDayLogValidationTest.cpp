#include <gtest/gtest.h>
#include <openssl/sha.h>

#include <fstream>
#include <iterator>

#include "../../lib/Memory/Memory.h"
#include "../tinta/fakes.h"
#include "HalTintaCompletionSetView.h"
#include "HalTintaDayLogValidation.h"
#include "HalTintaLegacyCourseReferences.h"
#include "HalTintaLegacyItemCatalogValidation.h"
#include "HalTintaLegacyItemView.h"
#include "HalTintaLegacyMarkView.h"
#include "HalTintaLegacyProfileValidation.h"
#include "HalTintaLegacyReviewValidation.h"
#include "HalTintaLegacySessionValidation.h"
#include "HalTintaNativeDerivedPreparation.h"
#include "HalTintaNativeLessonRecovery.h"
#include "HalTintaNativeReadingRecovery.h"
#include "HalTintaNativeStarRecovery.h"
#include "HalTintaPreferenceApplication.h"
#include "HalTintaReplayDayCorrespondence.h"
#include "HalTintaReplayLessonCorrespondence.h"
#include "HalTintaReplayMarkCorrespondence.h"
#include "HalUnboundCourseLessonMapping.h"

using namespace companion;

TEST(HalTintaReplayLessonCorrespondence, UsesOriginalPackIndicesAndPreservesProfileChoices) {
  inventory_hal_test::state = {};
  std::ifstream input(TINTA_TEST_PACK, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  auto pack = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(pack);
  ASSERT_EQ(pack->open(bytes.data(), bytes.size()), tinta::core::pack::PackStatus::Ok);
  TintaPackSubjectKeys lessons(*pack, false);
  ASSERT_GT(lessons.count(), 1u);
  uint32_t first = 0;
  ASSERT_TRUE(lessons.read(0, first));
  Identity course{1};
  auto store = makeUniqueNoThrow<HalTintaReplayStore>();
  ASSERT_TRUE(store);
  ASSERT_TRUE(store->begin(course));
  tinta::core::Profile profile;
  profile.unlockedThrough = 1;
  profile.retentionPermille = 870;
  profile.newPerDay = 23;
  auto permission = [](void*) { return true; };
  EXPECT_TRUE(compareTintaReplayLessons(profile, *pack, *store, course, permission, nullptr));
  ASSERT_TRUE(store->completion(EventKind::LessonComplete, first, true));
  EXPECT_FALSE(compareTintaReplayLessons(profile, *pack, *store, course, permission, nullptr));
  profile.currentLesson = 1;
  std::array<uint8_t, tinta::core::Profile::kEncodedSize> retained{}, after{};
  profile.encode(retained.data());
  ASSERT_TRUE(compareTintaReplayLessons(profile, *pack, *store, course, permission, nullptr));
  profile.encode(after.data());
  EXPECT_EQ(after, retained);
  ASSERT_TRUE(store->completion(EventKind::LessonComplete, UINT32_MAX - 1, true));
  EXPECT_FALSE(compareTintaReplayLessons(profile, *pack, *store, course, permission, nullptr));
  ASSERT_TRUE(store->completion(EventKind::LessonComplete, UINT32_MAX - 1, false));
  EXPECT_TRUE(compareTintaReplayLessons(profile, *pack, *store, course, permission, nullptr));
  EXPECT_FALSE(compareTintaReplayLessons(profile, *pack, *store, course, [](void*) { return false; }, nullptr));
  profile.encode(after.data());
  EXPECT_EQ(after, retained);
  ASSERT_TRUE(store->completion(EventKind::LessonComplete, first, false));
  EXPECT_FALSE(compareTintaReplayLessons(profile, *pack, *store, course, permission, nullptr));
}

TEST(HalTintaReplayDayCorrespondence, UnsortedSplitRecordsMustMatchBothDirections) {
  inventory_hal_test::state = {};
  Identity course{1};
  auto store = makeUniqueNoThrow<HalTintaReplayStore>();
  ASSERT_TRUE(store);
  ASSERT_TRUE(store->begin(course));
  TintaReplayDay first;
  first.gradedReviews = 70000;
  first.correctReviews = 60000;
  first.newItems = 10;
  first.responseMilliseconds = 1234500;
  ASSERT_TRUE(store->putDay(UINT16_MAX, first));
  TintaReplayDay second;
  second.gradedReviews = second.correctReviews = 1;
  second.responseMilliseconds = 1499;
  ASSERT_TRUE(store->putDay(0, second));
  constexpr const char* PATH = "/reviewed-days.bin";
  auto& bytes = inventory_hal_test::state.files[PATH];
  bytes.resize(4 + 3 * 12);
  std::memcpy(bytes.data(), "TDL1", 4);
  auto record = [&](size_t at, uint16_t day, uint16_t reviews, uint16_t correct, uint16_t fresh, uint16_t seconds) {
    auto* out = bytes.data() + 4 + at * 12;
    binary_record::putU16(out, day);
    binary_record::putU16(out + 2, reviews);
    binary_record::putU16(out + 4, correct);
    binary_record::putU16(out + 6, fresh);
    binary_record::putU16(out + 8, seconds);
    binary_record::putU16(out + 10, uint16_t(binary_record::crc32(out, 10)));
  };
  record(0, UINT16_MAX, 65535, 60000, 10, 1235);
  record(1, 0, 1, 1, 0, 1);
  record(2, UINT16_MAX, 4465, 0, 0, 0);
  const auto retained = bytes;
  HalFile file(PATH);
  std::array<uint8_t, 8192> scratch{};
  bool allowed = true;
  auto permission = [](void* context) { return *static_cast<bool*>(context); };
  ASSERT_TRUE(compareTintaReplayDays(file, *store, course, scratch, permission, &allowed));
  EXPECT_EQ(bytes, retained);
  record(2, UINT16_MAX, 4464, 0, 0, 0);
  EXPECT_FALSE(compareTintaReplayDays(file, *store, course, scratch, permission, &allowed));
  bytes = retained;
  bytes.resize(4 + 2 * 12);
  EXPECT_FALSE(compareTintaReplayDays(file, *store, course, scratch, permission, &allowed));
  bytes = retained;
  std::copy_n(retained.data() + 4 + 2 * 12, 12, bytes.data() + 4 + 12);
  bytes.resize(4 + 2 * 12);
  EXPECT_FALSE(compareTintaReplayDays(file, *store, course, scratch, permission, &allowed));
  bytes = retained;
  record(1, 1, 1, 1, 0, 1);
  EXPECT_FALSE(compareTintaReplayDays(file, *store, course, scratch, permission, &allowed));
  bytes = retained;
  EXPECT_FALSE(compareTintaReplayDays(file, *store, course, std::span(scratch).first(8191), permission, &allowed));
  allowed = false;
  EXPECT_FALSE(compareTintaReplayDays(file, *store, course, scratch, permission, &allowed));
  EXPECT_EQ(bytes, retained);
}

TEST(HalTintaReplayDayCorrespondence, ZeroDaysCrcRoundingAndReadFailuresDoNotRewriteEvidence) {
  inventory_hal_test::state = {};
  auto& state = inventory_hal_test::state;
  Identity course{1};
  auto store = makeUniqueNoThrow<HalTintaReplayStore>();
  ASSERT_TRUE(store);
  ASSERT_TRUE(store->begin(course));
  ASSERT_TRUE(store->putDay(17, {}));
  constexpr const char* PATH = "/zero-days.bin";
  auto& bytes = state.files[PATH];
  bytes.resize(16);
  std::memcpy(bytes.data(), "TDL1", 4);
  binary_record::putU16(bytes.data() + 4, 18);
  binary_record::putU16(bytes.data() + 14, uint16_t(binary_record::crc32(bytes.data() + 4, 10)));
  HalFile file(PATH);
  std::array<uint8_t, 8192> scratch{};
  auto permission = [](void*) { return true; };
  ASSERT_TRUE(compareTintaReplayDays(file, *store, course, scratch, permission, nullptr));
  TintaReplayDay projected;
  projected.gradedReviews = projected.correctReviews = 1;
  projected.responseMilliseconds = 1500;
  ASSERT_TRUE(store->putDay(18, projected));
  binary_record::putU16(bytes.data() + 6, 1);
  binary_record::putU16(bytes.data() + 8, 1);
  binary_record::putU16(bytes.data() + 12, 1);
  binary_record::putU16(bytes.data() + 14, uint16_t(binary_record::crc32(bytes.data() + 4, 10)));
  EXPECT_FALSE(compareTintaReplayDays(file, *store, course, scratch, permission, nullptr));
  binary_record::putU16(bytes.data() + 12, 2);
  binary_record::putU16(bytes.data() + 14, uint16_t(binary_record::crc32(bytes.data() + 4, 10)));
  ASSERT_TRUE(compareTintaReplayDays(file, *store, course, scratch, permission, nullptr));
  const auto retained = bytes;
  bytes.back() ^= 1;
  const auto corrupt = bytes;
  EXPECT_FALSE(compareTintaReplayDays(file, *store, course, scratch, permission, nullptr));
  EXPECT_EQ(bytes, corrupt);
  bytes = retained;
  unsigned calls = 0;
  auto cancel = [](void* context) { return ++*static_cast<unsigned*>(context) < 5; };
  EXPECT_FALSE(compareTintaReplayDays(file, *store, course, scratch, cancel, &calls));
  EXPECT_EQ(bytes, retained);
  state.failRead = state.reads + 1;
  EXPECT_FALSE(compareTintaReplayDays(file, *store, course, scratch, permission, nullptr));
  EXPECT_EQ(bytes, retained);
}

TEST(HalTintaReplayDayCorrespondence, RefusesCounterOverflowInsteadOfAcceptingWrappedTotals) {
  inventory_hal_test::state = {};
  Identity course{1};
  auto store = makeUniqueNoThrow<HalTintaReplayStore>();
  ASSERT_TRUE(store);
  ASSERT_TRUE(store->begin(course));
  TintaReplayDay wrapped;
  wrapped.gradedReviews = 65534;
  ASSERT_TRUE(store->putDay(27, wrapped));
  constexpr const char* PATH = "/overflow-days.bin";
  auto& bytes = inventory_hal_test::state.files[PATH];
  bytes.resize(4 + 65538 * 12);
  std::memcpy(bytes.data(), "TDL1", 4);
  binary_record::putU16(bytes.data() + 4, 27);
  binary_record::putU16(bytes.data() + 6, UINT16_MAX);
  binary_record::putU16(bytes.data() + 14, uint16_t(binary_record::crc32(bytes.data() + 4, 10)));
  for (size_t at = 16; at < bytes.size(); at += 12) std::copy_n(bytes.data() + 4, 12, bytes.data() + at);
  const auto retained = bytes;
  HalFile file(PATH);
  std::array<uint8_t, 8192> scratch{};
  EXPECT_FALSE(compareTintaReplayDays(file, *store, course, scratch, [](void*) { return true; }, nullptr));
  EXPECT_EQ(bytes, retained);
}

TEST(HalTintaCompletionSetView, MaximumSetUsesBoundedSearchAndRejectsExtentChange) {
  inventory_hal_test::state = {};
  constexpr const char* path = "/large-completions.bin";
  auto& state = inventory_hal_test::state;
  auto& bytes = state.files[path];
  bytes.resize(16 + 65535 * 4);
  std::memcpy(bytes.data(), "TCS1", 4);
  bytes[4] = 1;
  binary_record::putU32(bytes.data() + 8, 65535);
  for (uint32_t i = 0; i < 65535; ++i) binary_record::putU32(bytes.data() + 12 + i * 4, (i + 1) * 2);
  binary_record::putU32(bytes.data() + bytes.size() - 4, binary_record::crc32(bytes.data(), bytes.size() - 4));
  HalFile file(path);
  HalTintaCompletionSetView view(file);
  std::array<uint8_t, 128> scratch{};
  ASSERT_TRUE(view.begin(TintaCompletionKind::Lessons, scratch));
  for (uint32_t key : {1u, 2u, 65536u, 131070u, 131071u}) {
    const auto before = state.reads;
    bool found = false;
    ASSERT_TRUE(view.contains(key, found));
    EXPECT_EQ(found, key >= 2 && key <= 131070 && key % 2 == 0);
    EXPECT_LE(state.reads - before, 16u);
  }
  bool found = true;
  bytes.push_back(0);
  EXPECT_FALSE(view.contains(3, found));
  EXPECT_TRUE(found);
  bytes.pop_back();
  unsigned calls = 0;
  const auto cancel = [](void* context) { return ++*static_cast<unsigned*>(context) < 2; };
  EXPECT_FALSE(view.begin(TintaCompletionKind::Lessons, scratch, cancel, &calls));
  EXPECT_FALSE(view.contains(2, found));
  EXPECT_FALSE(view.begin(TintaCompletionKind::Lessons, std::span(scratch).first(11)));
}

TEST(HalTintaCompletionSetView, ValidatesSharedFixtureAndDistinguishesAbsentFromReadError) {
  inventory_hal_test::state = {};
  constexpr const char* path = "/completions.bin";
  auto& state = inventory_hal_test::state;
  state.files[path] = {84, 67, 83, 49, 2, 0, 0, 0, 2, 0, 0, 0, 7, 0, 0, 0, 9, 0, 0, 0, 215, 125, 242, 166};
  HalFile file(path);
  HalTintaCompletionSetView view(file);
  std::array<uint8_t, 128> scratch{};
  ASSERT_TRUE(view.begin(TintaCompletionKind::Readings, scratch));
  for (uint32_t key : {1u, 7u, 8u, 9u, 10u}) {
    bool found = false;
    ASSERT_TRUE(view.contains(key, found));
    EXPECT_EQ(found, key == 7 || key == 9);
  }
  bool found = true;
  state.failRead = state.reads + 1;
  EXPECT_FALSE(view.contains(8, found));
  EXPECT_TRUE(found);
  state.failRead = 0;
  EXPECT_FALSE(view.contains(8, found));
  ASSERT_TRUE(view.begin(TintaCompletionKind::Readings, scratch));
  ASSERT_TRUE(view.contains(8, found));
  EXPECT_FALSE(found);
  EXPECT_FALSE(view.begin(TintaCompletionKind::Lessons, scratch));
  state.files[path].back() ^= 1;
  EXPECT_FALSE(view.begin(TintaCompletionKind::Readings, scratch));
  EXPECT_TRUE(file.isOpen());
}
namespace {
constexpr const char* PATH = "/staged-days.bin";
void seed() {
  inventory_hal_test::state = {};
  inventory_hal_test::state.files[PATH] = {0x54, 0x44, 0x4c, 0x31, 2, 0, 3, 0, 2, 0, 1, 0, 2, 0, 0x44, 0x1b};
}
}  // namespace
TEST(HalTintaDayLogValidation, ReadsExactFixtureAndKeepsBorrowedHandleOpen) {
  seed();
  HalFile file(PATH);
  std::array<uint8_t, 12> scratch{};
  ASSERT_TRUE(validateTintaDayLogFile(file, scratch));
  EXPECT_TRUE(file.isOpen());
  EXPECT_TRUE(validateTintaDayLogFile(file, scratch));
  EXPECT_FALSE(validateTintaDayLogFile(file, std::span(scratch).first(11)));
}
TEST(HalTintaDayLogValidation, RejectsReadFailuresCorruptionTrailingBytesAndCancellation) {
  std::array<uint8_t, 12> scratch{};
  for (unsigned mode = 0; mode < 5; ++mode) {
    seed();
    auto& state = inventory_hal_test::state;
    if (mode == 0) state.files[PATH][14] ^= 1;
    if (mode == 1) state.files[PATH].push_back(0);
    if (mode == 2) state.failRead = 2;
    if (mode == 3) state.shortRead = 2;
    HalFile file(PATH);
    unsigned calls = 0;
    const auto progress = [](void* context) { return ++*static_cast<unsigned*>(context) < 2; };
    EXPECT_FALSE(validateTintaDayLogFile(file, scratch, mode == 4 ? +progress : nullptr, &calls));
    EXPECT_TRUE(file.isOpen());
  }
}

TEST(HalTintaCompletionSetView, IndexedReadsPreserveOutputOnFailureAndRequireRevalidation) {
  inventory_hal_test::state = {};
  auto& state = inventory_hal_test::state;
  constexpr const char* path = "/indexed-completions.bin";
  state.files[path] = {84, 67, 83, 49, 2, 0, 0, 0, 2, 0, 0, 0, 7, 0, 0, 0, 9, 0, 0, 0, 215, 125, 242, 166};
  HalFile file(path);
  HalTintaCompletionSetView view(file);
  std::array<uint8_t, 128> scratch{};
  uint32_t output = 99;
  EXPECT_FALSE(view.entryCount(TintaCompletionKind::Readings, output));
  EXPECT_EQ(output, 99u);
  ASSERT_TRUE(view.begin(TintaCompletionKind::Readings, scratch));
  ASSERT_TRUE(view.entryCount(TintaCompletionKind::Readings, output));
  EXPECT_EQ(output, 2u);
  ASSERT_TRUE(view.identityAt(0, output));
  EXPECT_EQ(output, 7u);
  ASSERT_TRUE(view.identityAt(1, output));
  EXPECT_EQ(output, 9u);
  output = 99;
  EXPECT_FALSE(view.identityAt(2, output));
  EXPECT_EQ(output, 99u);
  EXPECT_FALSE(view.entryCount(TintaCompletionKind::Readings, output));
  ASSERT_TRUE(view.begin(TintaCompletionKind::Readings, scratch));
  state.failRead = state.reads + 1;
  EXPECT_FALSE(view.identityAt(0, output));
  EXPECT_EQ(output, 99u);
  state.failRead = 0;
  EXPECT_FALSE(view.identityAt(0, output));
  ASSERT_TRUE(view.begin(TintaCompletionKind::Readings, scratch));
  state.files[path].push_back(0);
  EXPECT_FALSE(view.entryCount(TintaCompletionKind::Readings, output));
  EXPECT_EQ(output, 99u);
}

TEST(HalTintaNativeReadingRecovery, ProjectsVerifiedStableReadingAndPreservesLocalStateOnFailure) {
  namespace pk = tinta::core::pack;
  std::ifstream input(TINTA_TEST_PACK, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> packBytes{std::istreambuf_iterator<char>(input), {}};
  pk::Pack pack;
  ASSERT_EQ(pack.open(packBytes.data(), packBytes.size()), pk::PackStatus::Ok);
  uint32_t identity = 0, key = 0;
  bool matched = false;
  for (uint32_t i = 0; i < pack.count(pk::Section::Stor); ++i) {
    pk::Story story;
    ASSERT_TRUE(pack.story(i, story));
    ASSERT_TRUE(tintaPackStoryIdentity(pack, story, identity));
    if (resolveTintaStableStoryIdentity(pack, identity, key) == LegacyStoryIdentityResult::Matched) {
      matched = true;
      break;
    }
  }
  ASSERT_TRUE(matched);
  constexpr const char* path = "/recovery-readings.bin";
  auto& state = inventory_hal_test::state;
  for (unsigned mode = 0; mode < 4; ++mode) {
    state = {};
    auto& bytes = state.files[path];
    bytes.resize(20);
    std::memcpy(bytes.data(), "TCS1", 4);
    bytes[4] = mode == 3 ? 1 : 2;
    binary_record::putU32(bytes.data() + 8, 1);
    binary_record::putU32(bytes.data() + 12, mode == 1 ? 123 : identity);
    binary_record::putU32(bytes.data() + 16, binary_record::crc32(bytes.data(), 16));
    HalFile file(path);
    HalTintaCompletionSetView view(file);
    std::array<uint8_t, 128> validation{};
    ASSERT_TRUE(view.begin(mode == 3 ? TintaCompletionKind::Lessons : TintaCompletionKind::Readings, validation));
    tinta_test::MemStore local;
    local.files["read.bin"] = {'T', 'M', 'K', '1'};
    const auto previous = local.files["read.bin"];
    std::array<uint8_t, TINTA_NATIVE_MARK_SNAPSHOT_SIZE> scratch{};
    if (mode == 2) state.failRead = state.reads + 1;
    const auto result = restoreTintaNativeReadings(local, view, pack, scratch);
    if (mode == 0) {
      ASSERT_EQ(result, TintaNativeMarkSnapshotResult::Ok);
      tinta::core::library::MarkLog marks(local, "read.bin");
      marks.open();
      EXPECT_EQ(marks.count(), 1);
      EXPECT_TRUE(marks.contains(key));
    } else {
      EXPECT_NE(result, TintaNativeMarkSnapshotResult::Ok);
      EXPECT_EQ(local.files["read.bin"], previous);
      EXPECT_EQ(local.calls, 0);
    }
  }
  EXPECT_EQ(pack.arenaPeak(), 0u);
}

TEST(HalTintaNativeReadingRecovery, ReceiptBindingAndActualFileHashPrecedeNativeReplacement) {
  namespace pk = tinta::core::pack;
  std::ifstream input(TINTA_TEST_PACK, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> packBytes{std::istreambuf_iterator<char>(input), {}};
  pk::Pack pack;
  ASSERT_EQ(pack.open(packBytes.data(), packBytes.size()), pk::PackStatus::Ok);
  uint32_t identity = 0, key = 0;
  bool matched = false;
  for (uint32_t i = 0; i < pack.count(pk::Section::Stor); ++i) {
    pk::Story story;
    ASSERT_TRUE(pack.story(i, story));
    ASSERT_TRUE(tintaPackStoryIdentity(pack, story, identity));
    if (resolveTintaStableStoryIdentity(pack, identity, key) == LegacyStoryIdentityResult::Matched) {
      matched = true;
      break;
    }
  }
  ASSERT_TRUE(matched);
  Identity course{}, generation{};
  Digest packHash{}, frontier{};
  course.fill(1);
  generation.fill(2);
  packHash.fill(3);
  frontier.fill(4);
  for (unsigned mode = 0; mode < 7; ++mode) {
    inventory_hal_test::state = {};
    auto& bytes = inventory_hal_test::state.files["/bound-readings.bin"];
    bytes.resize(20);
    std::memcpy(bytes.data(), "TCS1", 4);
    bytes[4] = 2;
    binary_record::putU32(bytes.data() + 8, 1);
    binary_record::putU32(bytes.data() + 12, identity);
    binary_record::putU32(bytes.data() + 16, binary_record::crc32(bytes.data(), 16));
    HalFile file("/bound-readings.bin");
    std::array<uint8_t, TINTA_NATIVE_MARK_SNAPSHOT_SIZE> scratch{};
    Digest hash{};
    uint64_t length = 0;
    ASSERT_TRUE(hashInventoryFile(file, scratch, length, hash));
    std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> receipt{};
    std::memcpy(receipt.data(), "TDS1", 4);
    std::copy(course.begin(), course.end(), receipt.begin() + 4);
    std::copy(packHash.begin(), packHash.end(), receipt.begin() + 20);
    std::copy(frontier.begin(), frontier.end(), receipt.begin() + 52);
    std::fill(receipt.begin() + 84, receipt.begin() + 100, 5);
    std::copy(generation.begin(), generation.end(), receipt.begin() + 100);
    receipt[120] = 1;
    const uint32_t lengths[] = {1024, 0, 16, 20, 4};
    for (unsigned at = 0; at < 5; ++at) {
      binary_record::putU32(receipt.data() + 128 + at * 40, lengths[at]);
      std::fill_n(receipt.begin() + 136 + at * 40, 32, 6);
    }
    std::copy(hash.begin(), hash.end(), receipt.begin() + 136 + 3 * 40);
    if (mode >= 1 && mode <= 4) receipt[mode == 1 ? 4 : mode == 2 ? 100 : mode == 3 ? 20 : 52] ^= 1;
    binary_record::putU32(receipt.data() + 328, binary_record::crc32(receipt.data(), 328));
    TintaDerivedManifestView manifest;
    ASSERT_TRUE(manifest.decode(receipt));
    if (mode == 5) bytes[12] ^= 1;
    if (mode == 6) inventory_hal_test::state.failRead = inventory_hal_test::state.reads + 1;
    tinta_test::MemStore local;
    local.files["read.bin"] = {'T', 'M', 'K', '1'};
    const auto previous = local.files["read.bin"];
    const auto result = restoreVerifiedTintaNativeReadings(local, file, manifest, course, generation, packHash,
                                                           frontier, pack, scratch);
    if (mode == 0) {
      ASSERT_EQ(result, TintaNativeMarkSnapshotResult::Ok);
      tinta::core::library::MarkLog marks(local, "read.bin");
      marks.open();
      EXPECT_TRUE(marks.contains(key));
    } else {
      EXPECT_NE(result, TintaNativeMarkSnapshotResult::Ok);
      EXPECT_EQ(local.files["read.bin"], previous);
      EXPECT_EQ(local.calls, 0);
    }
  }
}

TEST(HalTintaNativeLessonRecovery, MapsStableLessonAndRejectsUnknownSubjectsAndReadFailures) {
  namespace pk = tinta::core::pack;
  std::ifstream input(TINTA_TEST_PACK, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  pk::Pack pack;
  ASSERT_EQ(pack.open(bytes.data(), bytes.size()), pk::PackStatus::Ok);
  TintaPackSubjectKeys lessons(pack, false);
  ASSERT_GT(lessons.count(), 1u);
  uint32_t identity = 0;
  ASSERT_TRUE(lessons.read(0, identity));
  for (unsigned mode = 0; mode < 4; ++mode) {
    inventory_hal_test::state = {};
    auto& state = inventory_hal_test::state;
    auto& contents = state.files["/lesson-recovery.bin"];
    contents.resize(20);
    std::memcpy(contents.data(), "TCS1", 4);
    contents[4] = mode == 3 ? 2 : 1;
    binary_record::putU32(contents.data() + 8, 1);
    binary_record::putU32(contents.data() + 12, mode == 1 ? 123 : identity);
    binary_record::putU32(contents.data() + 16, binary_record::crc32(contents.data(), 16));
    HalFile file("/lesson-recovery.bin");
    HalTintaCompletionSetView view(file);
    std::array<uint8_t, 128> scratch{};
    ASSERT_TRUE(view.begin(mode == 3 ? TintaCompletionKind::Readings : TintaCompletionKind::Lessons, scratch));
    tinta::core::Profile profile;
    profile.currentLesson = 4;
    profile.unlockedThrough = 0;
    profile.retentionPermille = 860;
    if (mode == 2) state.failRead = state.reads + 1;
    const bool projected = projectTintaNativeLessons(profile, view, pack);
    if (mode == 0) {
      ASSERT_TRUE(projected);
      EXPECT_EQ(profile.currentLesson, 1);
      EXPECT_EQ(profile.unlockedThrough, 1);
      struct Cancellation {
        uint32_t calls = 0, stop = 0;
      } cancellation;
      const auto progress = [](void* opaque) {
        auto& cancel = *static_cast<Cancellation*>(opaque);
        return ++cancel.calls < cancel.stop;
      };
      for (uint32_t stop = 1; stop <= lessons.count() + 2; ++stop) {
        cancellation = {0, stop};
        profile.currentLesson = 4;
        profile.unlockedThrough = 0;
        EXPECT_FALSE(projectTintaNativeLessons(profile, view, pack, progress, &cancellation));
        EXPECT_EQ(profile.currentLesson, 4);
        EXPECT_EQ(profile.unlockedThrough, 0);
      }
      tinta_test::MemStore cancelledStore;
      cancellation = {0, lessons.count() + 3};
      EXPECT_FALSE(persistTintaNativeLessonProjection(cancelledStore, profile, view, pack, progress, &cancellation));
      EXPECT_EQ(cancelledStore.calls, 0);
      EXPECT_TRUE(cancelledStore.files.empty());
      EXPECT_EQ(profile.currentLesson, 4);
      Identity course{}, generation{};
      Digest packHash{}, frontier{}, hash{};
      course.fill(1);
      generation.fill(2);
      packHash.fill(3);
      frontier.fill(4);
      uint64_t length = 0;
      ASSERT_TRUE(hashInventoryFile(file, scratch, length, hash));
      std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> receipt{};
      std::memcpy(receipt.data(), "TDS1", 4);
      std::copy(course.begin(), course.end(), receipt.begin() + 4);
      std::copy(packHash.begin(), packHash.end(), receipt.begin() + 20);
      std::copy(frontier.begin(), frontier.end(), receipt.begin() + 52);
      std::fill(receipt.begin() + 84, receipt.begin() + 100, 5);
      std::copy(generation.begin(), generation.end(), receipt.begin() + 100);
      receipt[120] = 1;
      const uint32_t lengths[] = {1024, 0, 20, 16, 4};
      for (unsigned at = 0; at < 5; ++at) {
        binary_record::putU32(receipt.data() + 128 + at * 40, lengths[at]);
        std::fill_n(receipt.begin() + 136 + at * 40, 32, 6);
      }
      std::copy(hash.begin(), hash.end(), receipt.begin() + 136 + 2 * 40);
      binary_record::putU32(receipt.data() + 328, binary_record::crc32(receipt.data(), 328));
      TintaDerivedManifestView manifest;
      ASSERT_TRUE(manifest.decode(receipt));
      profile.currentLesson = 4;
      profile.unlockedThrough = 0;
      auto otherCourse = course;
      otherCourse[0] ^= 1;
      EXPECT_FALSE(projectVerifiedTintaNativeLessons(profile, file, manifest, otherCourse, generation, packHash,
                                                     frontier, pack, scratch));
      EXPECT_EQ(profile.currentLesson, 4);
      EXPECT_EQ(profile.unlockedThrough, 0);
      contents[12] ^= 1;
      EXPECT_FALSE(projectVerifiedTintaNativeLessons(profile, file, manifest, course, generation, packHash, frontier,
                                                     pack, scratch));
      EXPECT_EQ(profile.currentLesson, 4);
      contents[12] ^= 1;
      ASSERT_TRUE(projectVerifiedTintaNativeLessons(profile, file, manifest, course, generation, packHash, frontier,
                                                    pack, scratch));
      EXPECT_EQ(profile.currentLesson, 1);
      EXPECT_EQ(profile.unlockedThrough, 1);
      for (const bool verified : {false, true}) {
        for (const auto tear : {tinta_test::MemStore::Tear::Nothing, tinta_test::MemStore::Tear::Prefix,
                                tinta_test::MemStore::Tear::Zeros, tinta_test::MemStore::Tear::Garbage}) {
          tinta_test::MemStore local;
          profile.currentLesson = 4;
          profile.unlockedThrough = 0;
          ASSERT_TRUE(profile.save(local));
          const auto oldBytes = local.files.at(tinta::core::Profile::kFile);
          if (verified) {
            const auto writes = local.calls;
            EXPECT_FALSE(persistVerifiedTintaNativeLessons(local, profile, file, manifest, otherCourse, generation,
                                                           packHash, frontier, pack, scratch));
            EXPECT_EQ(profile.currentLesson, 4);
            EXPECT_EQ(local.calls, writes);
            EXPECT_EQ(local.files.at(tinta::core::Profile::kFile), oldBytes);
          }
          const auto persist = [&] {
            return verified ? persistVerifiedTintaNativeLessons(local, profile, file, manifest, course, generation,
                                                                packHash, frontier, pack, scratch)
                            : persistTintaNativeLessonProjection(local, profile, view, pack);
          };
          local.cutAt(local.calls, tear);
          EXPECT_FALSE(persist());
          EXPECT_EQ(profile.currentLesson, 4);
          EXPECT_EQ(profile.unlockedThrough, 0);
          EXPECT_EQ(local.files.at(tinta::core::Profile::kFile), oldBytes);
          local.powerOn();
          ASSERT_TRUE(persist());
          EXPECT_EQ(profile.currentLesson, 1);
          EXPECT_EQ(profile.unlockedThrough, 1);
          tinta::core::Profile reopened;
          ASSERT_EQ(reopened.load(local), tinta::core::Profile::LoadResult::Loaded);
          EXPECT_EQ(reopened.currentLesson, 1);
          EXPECT_EQ(reopened.retentionPermille, 860);
          const auto writes = local.calls;
          ASSERT_TRUE(persist());
          EXPECT_EQ(local.calls, writes);
          local.present = false;
          EXPECT_FALSE(persist());
          EXPECT_EQ(local.calls, writes);
        }
      }
    } else {
      EXPECT_FALSE(projected);
      EXPECT_EQ(profile.currentLesson, 4);
      EXPECT_EQ(profile.unlockedThrough, 0);
    }
    EXPECT_EQ(profile.retentionPermille, 860);
  }
}

TEST(HalTintaNativeStars, VerifiedFlagsRestoreAndCapacityFailurePreservesNativeMarks) {
  inventory_hal_test::state = {};
  constexpr const char* path = "/items.bin";
  auto& bytes = inventory_hal_test::state.files[path];
  class Catalog final : public TintaSubjectCatalog {
   public:
    TintaSubjectMembership contains(EventKind kind, uint32_t uid) override {
      return kind == EventKind::Star && uid ? TintaSubjectMembership::Present : TintaSubjectMembership::Missing;
    }
  } catalog;
  Identity course{}, generation{};
  Digest packHash{}, frontier{};
  course.fill(1);
  generation.fill(2);
  packHash.fill(3);
  frontier.fill(4);
  for (uint32_t records : {2u, 97u}) {
    bytes.assign(1024 + records * 16, 0);
    for (unsigned slot = 0; slot < 2; ++slot) {
      auto* header = bytes.data() + slot * 512;
      std::memcpy(header, "TIS1", 4);
      binary_record::putU16(header + 4, 1);
      binary_record::putU16(header + 6, 80);
      binary_record::putU32(header + 8, slot + 1);
      binary_record::putU32(header + 12, records);
      binary_record::putU32(header + 76, binary_record::crc32(header, 76));
    }
    for (uint32_t index = 0; index < records; ++index) {
      auto item = tinta::core::ItemState::fresh(index + 1);
      if (records == 97 || index == 0) item.flags |= tinta::core::item_flag::kStarred;
      item.encode(bytes.data() + 1024 + index * 16);
    }
    HalFile file(path);
    std::array<uint8_t, TINTA_NATIVE_STAR_WORKSPACE_SIZE> scratch{};
    Digest hash{};
    uint64_t length = 0;
    ASSERT_TRUE(hashInventoryFile(file, scratch, length, hash));
    std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> receipt{};
    std::memcpy(receipt.data(), "TDS1", 4);
    std::copy(course.begin(), course.end(), receipt.begin() + 4);
    std::copy(packHash.begin(), packHash.end(), receipt.begin() + 20);
    std::copy(frontier.begin(), frontier.end(), receipt.begin() + 52);
    std::fill(receipt.begin() + 84, receipt.begin() + 100, 5);
    std::copy(generation.begin(), generation.end(), receipt.begin() + 100);
    receipt[120] = 1;
    const uint32_t lengths[] = {static_cast<uint32_t>(length), 0, 16, 16, 4};
    for (unsigned at = 0; at < 5; ++at) {
      binary_record::putU32(receipt.data() + 128 + at * 40, lengths[at]);
      std::fill_n(receipt.begin() + 136 + at * 40, 32, 6);
    }
    std::copy(hash.begin(), hash.end(), receipt.begin() + 136);
    binary_record::putU32(receipt.data() + 328, binary_record::crc32(receipt.data(), 328));
    TintaDerivedManifestView manifest;
    ASSERT_TRUE(manifest.decode(receipt));
    tinta_test::MemStore local;
    local.files["starred.bin"] = {'o', 'l', 'd'};
    const auto original = local.files["starred.bin"];
    if (records == 2) {
      local.cutAt(local.calls, tinta_test::MemStore::Tear::Prefix);
      EXPECT_EQ(restoreVerifiedTintaNativeStars(local, file, manifest, course, generation, packHash, frontier, catalog,
                                                scratch),
                TintaNativeMarkSnapshotResult::IoError);
      EXPECT_EQ(local.files["starred.bin"], original);
      local.powerOn();
      auto otherGeneration = generation;
      otherGeneration.fill(9);
      const auto writes = local.calls;
      EXPECT_EQ(restoreVerifiedTintaNativeStars(local, file, manifest, course, otherGeneration, packHash, frontier,
                                                catalog, scratch),
                TintaNativeMarkSnapshotResult::Invalid);
      EXPECT_EQ(local.calls, writes);
      EXPECT_EQ(local.files["starred.bin"], original);
    }
    const auto result = restoreVerifiedTintaNativeStars(local, file, manifest, course, generation, packHash, frontier,
                                                        catalog, scratch);
    if (records == 97) {
      EXPECT_EQ(result, TintaNativeMarkSnapshotResult::Invalid);
      EXPECT_EQ(local.files["starred.bin"], original);
      EXPECT_EQ(local.calls, 0);
    } else {
      ASSERT_EQ(result, TintaNativeMarkSnapshotResult::Ok);
      tinta::core::library::MarkLog marks(local, "starred.bin");
      marks.open();
      EXPECT_EQ(marks.count(), 1);
      EXPECT_TRUE(marks.contains(1));
      EXPECT_FALSE(marks.contains(2));
      const auto saved = local.files["starred.bin"];
      const auto writes = local.calls;
      ASSERT_EQ(restoreVerifiedTintaNativeStars(local, file, manifest, course, generation, packHash, frontier, catalog,
                                                scratch),
                TintaNativeMarkSnapshotResult::Ok);
      EXPECT_EQ(local.calls, writes);
      EXPECT_EQ(local.files["starred.bin"], saved);
      local.dropAtRead(local.readCalls);
      EXPECT_EQ(restoreVerifiedTintaNativeStars(local, file, manifest, course, generation, packHash, frontier, catalog,
                                                scratch),
                TintaNativeMarkSnapshotResult::IoError);
      EXPECT_EQ(local.calls, writes);
      EXPECT_EQ(local.files["starred.bin"], saved);
      local.powerOn();
      bytes[1024] ^= 1;
      EXPECT_EQ(restoreVerifiedTintaNativeStars(local, file, manifest, course, generation, packHash, frontier, catalog,
                                                scratch),
                TintaNativeMarkSnapshotResult::Invalid);
      EXPECT_EQ(local.files["starred.bin"], saved);
    }
  }
}

TEST(HalTintaNativePreparation, CompleteGenerationPreflightPreservesPreferencesAndRetriesWithoutWrites) {
  namespace pk = tinta::core::pack;
  std::ifstream input(TINTA_TEST_PACK, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> packBytes{std::istreambuf_iterator<char>(input), {}};
  pk::Pack pack;
  ASSERT_EQ(pack.open(packBytes.data(), packBytes.size()), pk::PackStatus::Ok);
  TintaPackSubjectKeys lessons(pack, false);
  uint32_t lesson = 0;
  ASSERT_TRUE(lessons.read(0, lesson));
  class Catalog final : public TintaSubjectCatalog {
    TintaSubjectMembership contains(EventKind, uint32_t) override { return TintaSubjectMembership::Present; }
  } catalog;
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  Identity course{}, generation{};
  Digest packHash{}, frontier{};
  course.fill(1);
  generation.fill(2);
  packHash.fill(3);
  frontier.fill(4);
  std::array<char, 96> root{}, path{};
  ASSERT_TRUE(courseStateDirectory(course, root));
  state.directories[root.data()] = {};
  struct ProofGate {
    bool allowed = true;
    unsigned calls = 0;
  } proof;
  const auto prove = [](void* context, const TintaDerivedManifestView&) {
    auto& gate = *static_cast<ProofGate*>(context);
    ++gate.calls;
    return gate.allowed;
  };
  HalTintaNativeDerivedPreparation preparation(course, &proof, prove);
  tinta_test::MemStore local;
  tinta::core::Profile profile;
  profile.retentionPermille = 860;
  profile.currentLesson = 4;
  profile.unlockedThrough = 0;
  EXPECT_EQ(preparation.run(local, profile, pack, catalog, generation, packHash, frontier),
            TintaNativePreparationResult::NoReceipt);
  EXPECT_TRUE(local.files.empty());
  std::array<std::vector<uint8_t>, 5> files;
  files[0].resize(1024);
  for (unsigned at = 0; at < 2; ++at) {
    auto* header = files[0].data() + at * 512;
    std::memcpy(header, "TIS1", 4);
    binary_record::putU16(header + 4, 1);
    binary_record::putU16(header + 6, 80);
    binary_record::putU32(header + 8, at + 1);
    binary_record::putU16(header + 20, 5);
    binary_record::putU32(header + 76, binary_record::crc32(header, 76));
  }
  for (unsigned at = 2; at < 4; ++at) {
    files[at].resize(at == 2 ? 20 : 16);
    auto* header = files[at].data();
    std::memcpy(header, "TCS1", 4);
    header[4] = at == 2 ? 1 : 2;
    binary_record::putU32(header + 8, at == 2 ? 1 : 0);
    if (at == 2) binary_record::putU32(header + 12, lesson);
    binary_record::putU32(header + files[at].size() - 4, binary_record::crc32(header, files[at].size() - 4));
  }
  files[4] = {'T', 'D', 'L', '1'};
  std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> receipt{};
  std::memcpy(receipt.data(), "TDS1", 4);
  std::copy(course.begin(), course.end(), receipt.begin() + 4);
  std::copy(packHash.begin(), packHash.end(), receipt.begin() + 20);
  std::copy(frontier.begin(), frontier.end(), receipt.begin() + 52);
  std::fill(receipt.begin() + 84, receipt.begin() + 100, 5);
  std::copy(generation.begin(), generation.end(), receipt.begin() + 100);
  binary_record::putU16(receipt.data() + 116, 5);
  receipt[120] = 1;
  for (unsigned at = 0; at < 5; ++at) {
    ASSERT_TRUE(tintaDerivedFilePath(course, static_cast<TintaDerivedFile>(at), TintaDerivedRole::Active, path));
    state.files[path.data()] = files[at];
    binary_record::putU32(receipt.data() + 128 + at * 40, files[at].size());
    ASSERT_NE(SHA256(files[at].data(), files[at].size(), receipt.data() + 136 + at * 40), nullptr);
  }
  binary_record::putU32(receipt.data() + 328, binary_record::crc32(receipt.data(), 328));
  ASSERT_TRUE(tintaDerivedRecordPath(course, TintaDerivedRecord::Receipt, path));
  state.files[path.data()] = {receipt.begin(), receipt.end()};
  proof.allowed = false;
  EXPECT_EQ(preparation.run(local, profile, pack, catalog, generation, packHash, frontier),
            TintaNativePreparationResult::Failed);
  EXPECT_TRUE(local.files.empty());
  EXPECT_EQ(proof.calls, 1U);
  proof.allowed = true;
  auto otherGeneration = generation;
  otherGeneration[0] ^= 1;
  EXPECT_EQ(preparation.run(local, profile, pack, catalog, otherGeneration, packHash, frontier),
            TintaNativePreparationResult::Failed);
  EXPECT_TRUE(local.files.empty());
  ASSERT_TRUE(tintaDerivedFilePath(course, TintaDerivedFile::Days, TintaDerivedRole::Active, path));
  state.files[path.data()][0] ^= 1;
  EXPECT_EQ(preparation.run(local, profile, pack, catalog, generation, packHash, frontier),
            TintaNativePreparationResult::Failed);
  EXPECT_TRUE(local.files.empty());
  EXPECT_EQ(profile.currentLesson, 4);
  state.files[path.data()][0] ^= 1;
  ASSERT_EQ(preparation.run(local, profile, pack, catalog, generation, packHash, frontier),
            TintaNativePreparationResult::Prepared);
  EXPECT_EQ(profile.currentLesson, 1);
  EXPECT_EQ(profile.unlockedThrough, 1);
  EXPECT_EQ(profile.retentionPermille, 860);
  const auto writes = local.calls;
  ASSERT_EQ(preparation.run(local, profile, pack, catalog, generation, packHash, frontier),
            TintaNativePreparationResult::Prepared);
  EXPECT_EQ(local.calls, writes);
}

TEST(HalTintaDayLogValidationTest, ResolvedPreferencesValidateWholeBatchAndPersistOnlyChanges) {
  tinta_test::MemStore store;
  tinta::core::Profile profile, source;
  profile.fullRefreshEvery = 17;
  source.newPerDay = 20;
  source.retentionPermille = 950;
  std::array<uint8_t, 8> first{}, second{};
  ASSERT_EQ(companion::encodeTintaPreference(source, 32, first), 8U);
  ASSERT_EQ(companion::encodeTintaPreference(source, 34, second), 8U);
  std::array<std::span<const uint8_t>, 2> batch{first, second};
  second[4] = 0xff;
  EXPECT_FALSE(companion::persistResolvedTintaPreferences(store, profile, batch));
  EXPECT_EQ(store.calls, 0);
  EXPECT_EQ(profile.newPerDay, 10);
  ASSERT_EQ(companion::encodeTintaPreference(source, 34, second), 8U);
  batch[1] = first;
  EXPECT_FALSE(companion::persistResolvedTintaPreferences(store, profile, batch));
  EXPECT_EQ(store.calls, 0);
  batch[1] = second;
  store.failFrom(0);
  EXPECT_FALSE(companion::persistResolvedTintaPreferences(store, profile, batch));
  EXPECT_EQ(profile.newPerDay, 10);
  EXPECT_EQ(profile.retentionPermille, 900);
  store.powerOn();
  ASSERT_TRUE(companion::persistResolvedTintaPreferences(store, profile, batch));
  EXPECT_EQ(profile.newPerDay, 20);
  EXPECT_EQ(profile.retentionPermille, 950);
  EXPECT_EQ(profile.fullRefreshEvery, 17);
  tinta::core::Profile loaded;
  ASSERT_EQ(loaded.load(store), tinta::core::Profile::LoadResult::Loaded);
  EXPECT_EQ(loaded.newPerDay, 20);
  EXPECT_EQ(loaded.fullRefreshEvery, 17);
  const auto calls = store.calls;
  EXPECT_TRUE(companion::persistResolvedTintaPreferences(store, profile, batch));
  EXPECT_EQ(store.calls, calls);
  store.present = false;
  EXPECT_FALSE(companion::persistResolvedTintaPreferences(store, profile, batch));
  EXPECT_EQ(store.calls, calls);
}

namespace {
void legacyItemHeaders(std::vector<uint8_t>& bytes, uint32_t records, uint32_t pendingSlot) {
  bytes.assign(1024 + records * 16, 0);
  for (unsigned copy = 0; copy < 2; ++copy) {
    auto* header = bytes.data() + copy * 512;
    std::memcpy(header, "TIS1", 4);
    binary_record::putU16(header + 4, 1);
    binary_record::putU16(header + 6, 80);
    binary_record::putU32(header + 8, copy + 1);
    binary_record::putU32(header + 12, records);
    binary_record::putU16(header + 26, 1);
    binary_record::putU32(header + 28, pendingSlot);
    tinta::core::ItemState::fresh(101).encode(header + 32);
    binary_record::putU32(header + 76, binary_record::crc32(header, 76));
  }
  for (uint32_t i = 0; i < records; ++i) tinta::core::ItemState::fresh(200 + i).encode(bytes.data() + 1024 + i * 16);
}
}  // namespace

TEST(HalTintaLegacyItemView, PendingRecordIsInspectedWithoutRepairingReviewedBytes) {
  inventory_hal_test::state = {};
  auto& bytes = inventory_hal_test::state.files["/reviewed-items"];
  legacyItemHeaders(bytes, 2, 1);
  const auto original = bytes;
  HalFile file("/reviewed-items");
  HalTintaLegacyItemView view(file);
  std::array<uint8_t, 160> scratch{};
  ASSERT_TRUE(view.begin(scratch));
  ASSERT_NE(view.header(), nullptr);
  EXPECT_EQ(view.header()->seq, 2u);
  EXPECT_EQ(view.count(), 2u);
  tinta::core::ItemState item;
  ASSERT_TRUE(view.record(0, item));
  EXPECT_EQ(item.uid, 200u);
  ASSERT_TRUE(view.record(1, item));
  EXPECT_EQ(item.uid, 101u);
  EXPECT_EQ(bytes, original);
  EXPECT_TRUE(file.isOpen());
}

TEST(HalTintaLegacyItemView, CanInspectRecoverableMissingFinalPendingRecord) {
  inventory_hal_test::state = {};
  auto& bytes = inventory_hal_test::state.files["/reviewed-items"];
  legacyItemHeaders(bytes, 2, 1);
  bytes.resize(1040);
  const auto original = bytes;
  HalFile file("/reviewed-items");
  HalTintaLegacyItemView view(file);
  std::array<uint8_t, 160> scratch{};
  ASSERT_TRUE(view.begin(scratch));
  tinta::core::ItemState item;
  ASSERT_TRUE(view.record(1, item));
  EXPECT_EQ(item.uid, 101u);
  EXPECT_EQ(bytes, original);
}

TEST(HalTintaLegacyItemView, CorruptHeaderFallbackAndAmbiguityAreReadOnly) {
  for (unsigned fault = 0; fault < 4; ++fault) {
    inventory_hal_test::state = {};
    auto& bytes = inventory_hal_test::state.files["/reviewed-items"];
    legacyItemHeaders(bytes, 2, 1);
    if (fault == 0) bytes[0] ^= 1;
    if (fault == 1) bytes[512] ^= 1;
    if (fault == 2) bytes[0] = bytes[512] = 0;
    if (fault == 3) {
      binary_record::putU32(bytes.data() + 512 + 8, 1);
      binary_record::putU32(bytes.data() + 512 + 16, 99);
      binary_record::putU32(bytes.data() + 512 + 76, binary_record::crc32(bytes.data() + 512, 76));
    }
    const auto original = bytes;
    HalFile file("/reviewed-items");
    HalTintaLegacyItemView view(file);
    std::array<uint8_t, 160> scratch{};
    EXPECT_EQ(view.begin(scratch), fault < 2);
    if (fault < 2)
      EXPECT_EQ(view.header()->seq, fault == 0 ? 2u : 1u);
    else
      EXPECT_EQ(view.header(), nullptr);
    EXPECT_EQ(bytes, original);
  }
}

TEST(HalTintaLegacyItemView, ReadFaultExtentChangeAndCancellationInvalidateView) {
  for (unsigned fault = 0; fault < 5; ++fault) {
    inventory_hal_test::state = {};
    auto& state = inventory_hal_test::state;
    auto& bytes = state.files["/reviewed-items"];
    legacyItemHeaders(bytes, 2, 1);
    HalFile file("/reviewed-items");
    HalTintaLegacyItemView view(file);
    std::array<uint8_t, 160> scratch{};
    bool allowed = true;
    ASSERT_TRUE(view.begin(scratch, [](void* context) { return *static_cast<bool*>(context); }, &allowed));
    if (fault == 0) state.failRead = state.reads + 1;
    if (fault == 1) bytes.push_back(0);
    if (fault == 2) allowed = false;
    if (fault == 3) bytes[1024 + 11] = 0x80;
    const auto original = bytes;
    tinta::core::ItemState item = tinta::core::ItemState::fresh(777);
    EXPECT_FALSE(view.record(fault == 4 ? 2 : 0, item));
    EXPECT_EQ(item.uid, 777u);
    EXPECT_EQ(view.header(), nullptr);
    EXPECT_EQ(bytes, original);
  }
}

TEST(HalTintaLegacyItemCatalog, RealPackKeepsRetiredStatesAndRejectsDuplicatesWithoutMutations) {
  std::ifstream input(TINTA_TEST_PACK, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> packBytes{std::istreambuf_iterator<char>(input), {}};
  tinta::core::pack::MemorySource source(packBytes.data(), packBytes.size());
  auto pack = std::make_unique<tinta::core::pack::Pack>();
  ASSERT_EQ(pack->open(source), tinta::core::pack::PackStatus::Ok);
  ASSERT_GT(pack->itemCount(), 0u);
  CourseUidLookup catalog(source);
  for (unsigned duplicate = 0; duplicate < 2; ++duplicate) {
    ASSERT_TRUE(catalog.begin());
    inventory_hal_test::state = {};
    auto& bytes = inventory_hal_test::state.files["/reviewed-items"];
    legacyItemHeaders(bytes, 3, 2);
    for (unsigned copy = 0; copy < 2; ++copy) {
      auto* header = bytes.data() + copy * 512;
      tinta::core::ItemState::fresh(UINT32_MAX).encode(header + 32);
      binary_record::putU32(header + 76, binary_record::crc32(header, 76));
    }
    tinta::core::ItemState::fresh(pack->uidAt(0)).encode(bytes.data() + 1024);
    tinta::core::ItemState::fresh(duplicate ? pack->uidAt(0) : 0xfffffffe).encode(bytes.data() + 1040);
    const auto original = bytes;
    HalFile file("/reviewed-items");
    std::array<uint8_t, 4256> scratch{};
    LegacyItemCatalogReport report{777, 888, 999};
    EXPECT_EQ(inspectTintaLegacyItemCatalog(file, catalog, scratch, report), !duplicate);
    if (duplicate) {
      EXPECT_EQ(report.mapped, 777u);
      EXPECT_EQ(report.retired, 888u);
      EXPECT_EQ(report.tombstones, 999u);
    } else {
      EXPECT_EQ(report.mapped, 1u);
      EXPECT_EQ(report.retired, 1u);
      EXPECT_EQ(report.tombstones, 1u);
    }
    EXPECT_EQ(bytes, original);
  }
}

TEST(HalTintaLegacyReviews, AuditsRetiredReviewUndoAndZeroTailWithoutReplaying) {
  std::ifstream input(TINTA_TEST_PACK, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> packBytes{std::istreambuf_iterator<char>(input), {}};
  tinta::core::pack::MemorySource source(packBytes.data(), packBytes.size());
  CourseUidLookup catalog(source);
  for (unsigned fault = 0; fault < 7; ++fault) {
    ASSERT_TRUE(catalog.begin());
    inventory_hal_test::state = {};
    auto& state = inventory_hal_test::state;
    auto& bytes = state.files["/reviewed-reviews"];
    bytes.assign(36 + 5, 0);
    binary_record::putU32(bytes.data(), 0xfffffffe);
    bytes[10] = 3;
    binary_record::putU32(bytes.data() + 12, 0xfffffffe);
    bytes[22] = 8;
    if (fault == 1) bytes[22] = 0;
    if (fault == 2) bytes[23] = 1;
    if (fault == 3) bytes[12] ^= 1;
    if (fault == 4) bytes.back() = 1;
    if (fault == 5) state.failRead = state.reads + 2;
    const auto original = bytes;
    HalFile file("/reviewed-reviews");
    std::array<uint8_t, 12> scratch{};
    LegacyReviewReport report{777, 888, 999, 666};
    EXPECT_EQ(inspectTintaLegacyReviews(file, catalog, fault == 6 ? 3 : 2, scratch, report), fault == 0);
    if (fault == 0) {
      EXPECT_EQ(report.records, 2u);
      EXPECT_EQ(report.mapped, 0u);
      EXPECT_EQ(report.retired, 2u);
      EXPECT_EQ(report.zeroTailBytes, 17u);
    } else {
      EXPECT_EQ(report.records, 777u);
      EXPECT_EQ(report.mapped, 888u);
      EXPECT_EQ(report.retired, 999u);
      EXPECT_EQ(report.zeroTailBytes, 666u);
    }
    EXPECT_EQ(bytes, original);
  }
}

TEST(HalTintaLegacyProfile, PreservesReviewedProfileAndReportsUpgradeWithoutSaving) {
  for (unsigned upgrade = 0; upgrade < 2; ++upgrade) {
    inventory_hal_test::state = {};
    auto& bytes = inventory_hal_test::state.files["/reviewed-profile"];
    bytes.resize(tinta::core::Profile::kEncodedSize);
    tinta::core::Profile original;
    original.currentLesson = 7;
    original.unlockedThrough = 11;
    original.encode(bytes.data());
    if (upgrade) {
      bytes.resize(12 + 300, 0);
      binary_record::putU16(bytes.data() + 4, 2);
      binary_record::putU16(bytes.data() + 6, 300);
      binary_record::putU32(bytes.data() + 308, binary_record::crc32(bytes.data(), 308));
    }
    const auto reviewed = bytes;
    HalFile file("/reviewed-profile");
    tinta::core::Profile decoded;
    auto result = tinta::core::Profile::LoadResult::Defaults;
    ASSERT_TRUE(inspectTintaLegacyProfile(file, decoded, result));
    EXPECT_EQ(result, upgrade ? tinta::core::Profile::LoadResult::Upgraded : tinta::core::Profile::LoadResult::Loaded);
    EXPECT_EQ(decoded.currentLesson, 7);
    EXPECT_EQ(decoded.unlockedThrough, 11);
    EXPECT_EQ(bytes, reviewed);
    EXPECT_TRUE(file.isOpen());
  }
}
TEST(HalTintaLegacyProfile, CorruptionReadFailureAndCancellationWithholdOutputs) {
  for (unsigned fault = 0; fault < 7; ++fault) {
    inventory_hal_test::state = {};
    auto& state = inventory_hal_test::state;
    auto& bytes = state.files["/reviewed-profile"];
    bytes.resize(tinta::core::Profile::kEncodedSize);
    tinta::core::Profile{}.encode(bytes.data());
    if (fault == 0) bytes.back() ^= 1;
    if (fault == 1) bytes.push_back(0);
    if (fault == 2) bytes.resize(12);
    if (fault == 3) state.failRead = state.reads + 1;
    if (fault == 4) state.failRead = state.reads + 3;
    if (fault == 5) state.failRead = state.reads + 4;
    const auto reviewed = bytes;
    HalFile file("/reviewed-profile");
    tinta::core::Profile decoded;
    decoded.currentLesson = 777;
    auto result = tinta::core::Profile::LoadResult::Defaults;
    bool allowed = fault != 6;
    EXPECT_FALSE(inspectTintaLegacyProfile(
        file, decoded, result, [](void* context) { return *static_cast<bool*>(context); }, &allowed));
    EXPECT_EQ(decoded.currentLesson, 777);
    EXPECT_EQ(result, tinta::core::Profile::LoadResult::Defaults);
    EXPECT_EQ(bytes, reviewed);
  }
}

namespace {
void legacyMarkRecord(std::vector<uint8_t>& bytes, uint32_t key, uint8_t operation) {
  const auto at = bytes.size();
  bytes.resize(at + 8);
  binary_record::putU32(bytes.data() + at, key);
  bytes[at + 4] = operation;
  binary_record::putU16(bytes.data() + at + 6, uint16_t(binary_record::crc32(bytes.data() + at, 6)));
}
}  // namespace
TEST(HalTintaLegacyMarkView, ReconstructsOrderAndIdempotentChangesWithoutCompaction) {
  inventory_hal_test::state = {};
  auto& bytes = inventory_hal_test::state.files["/reviewed-marks"];
  bytes = {'T', 'M', 'K', '1'};
  legacyMarkRecord(bytes, 7, 1);
  legacyMarkRecord(bytes, 8, 1);
  legacyMarkRecord(bytes, 7, 1);
  legacyMarkRecord(bytes, 9, 2);
  legacyMarkRecord(bytes, 7, 2);
  legacyMarkRecord(bytes, 7, 1);
  const auto original = bytes;
  HalFile file("/reviewed-marks");
  HalTintaLegacyMarkView view(file);
  std::array<uint8_t, HalTintaLegacyMarkView::WORKSPACE_SIZE> scratch{};
  ASSERT_TRUE(view.begin(scratch));
  uint16_t count = 0;
  ASSERT_TRUE(view.entryCount(count));
  EXPECT_EQ(count, 2);
  uint32_t key = 0;
  ASSERT_TRUE(view.identityAt(0, key));
  EXPECT_EQ(key, 8u);
  ASSERT_TRUE(view.identityAt(1, key));
  EXPECT_EQ(key, 7u);
  EXPECT_EQ(bytes, original);
  EXPECT_TRUE(file.isOpen());
}
TEST(HalTintaLegacyMarkView, TornCorruptReadFailureAndCapacityOverflowWithholdView) {
  for (unsigned fault = 0; fault < 6; ++fault) {
    inventory_hal_test::state = {};
    auto& state = inventory_hal_test::state;
    auto& bytes = state.files["/reviewed-marks"];
    bytes = {'T', 'M', 'K', '1'};
    legacyMarkRecord(bytes, 7, 1);
    if (fault == 0) bytes.back() ^= 1;
    if (fault == 1) bytes.pop_back();
    if (fault == 2) bytes[8] = 3;
    if (fault == 3) state.failRead = state.reads + 2;
    if (fault == 4) {
      for (uint32_t key = 8; key <= 103; ++key) legacyMarkRecord(bytes, key, 1);
    }
    const auto original = bytes;
    HalFile file("/reviewed-marks");
    HalTintaLegacyMarkView view(file);
    std::array<uint8_t, HalTintaLegacyMarkView::WORKSPACE_SIZE> scratch{};
    bool allowed = fault != 5;
    EXPECT_FALSE(view.begin(scratch, [](void* context) { return *static_cast<bool*>(context); }, &allowed));
    uint16_t count = 777;
    EXPECT_FALSE(view.entryCount(count));
    EXPECT_EQ(count, 777);
    EXPECT_EQ(bytes, original);
  }
}

TEST(HalTintaLegacyReferences, ValidatesNativeLessonIndicesIncludingCompletedSentinel) {
  std::ifstream input(TINTA_TEST_PACK, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  auto pack = std::make_unique<tinta::core::pack::Pack>();
  ASSERT_EQ(pack->open(bytes.data(), bytes.size()), tinta::core::pack::PackStatus::Ok);
  const auto lessons = pack->count(tinta::core::pack::Section::Less);
  ASSERT_GT(lessons, 0u);
  tinta::core::Profile profile;
  EXPECT_TRUE(inspectTintaLegacyLessonReferences(profile, *pack));
  profile.currentLesson = lessons;
  profile.unlockedThrough = lessons - 1;
  EXPECT_TRUE(inspectTintaLegacyLessonReferences(profile, *pack));
  profile.currentLesson = lessons + 1;
  EXPECT_FALSE(inspectTintaLegacyLessonReferences(profile, *pack));
  profile.currentLesson = 0;
  profile.unlockedThrough = lessons;
  EXPECT_FALSE(inspectTintaLegacyLessonReferences(profile, *pack));
  pack->close();
  EXPECT_FALSE(inspectTintaLegacyLessonReferences(profile, *pack));
}
TEST(HalTintaReplayMarkCorrespondence, ChecksStarsAndResolvedReadingsWithoutReplacingLogs) {
  std::ifstream input(TINTA_TEST_PACK, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  tinta::core::pack::MemorySource source(bytes.data(), bytes.size());
  auto pack = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(pack);
  ASSERT_EQ(pack->open(source), tinta::core::pack::PackStatus::Ok);
  CourseUidLookup catalog(source);
  ASSERT_TRUE(catalog.begin());
  for (const auto kind : {TintaReplayMarkKind::Stars, TintaReplayMarkKind::Readings}) {
    inventory_hal_test::state = {};
    Identity course{1};
    auto store = makeUniqueNoThrow<HalTintaReplayStore>();
    ASSERT_TRUE(store);
    ASSERT_TRUE(store->begin(course));
    uint32_t key = pack->uidAt(0), uid = key;
    if (kind == TintaReplayMarkKind::Readings) {
      bool unique = false;
      for (uint32_t at = 0; at < pack->count(tinta::core::pack::Section::Stor); ++at) {
        tinta::core::pack::Story story;
        ASSERT_TRUE(pack->story(at, story));
        ASSERT_TRUE(tintaLegacyStoryKey(*pack, story, key));
        if (resolveTintaLegacyStoryKey(*pack, key, uid) == LegacyStoryIdentityResult::Matched) {
          unique = true;
          break;
        }
      }
      ASSERT_TRUE(unique);
    }
    auto& log = inventory_hal_test::state.files["/reviewed-marks"];
    log = {'T', 'M', 'K', '1'};
    legacyMarkRecord(log, key, 1);
    legacyMarkRecord(log, key, 2);
    legacyMarkRecord(log, key, 1);
    const auto retained = log;
    HalFile file("/reviewed-marks");
    std::array<uint8_t, HalTintaLegacyMarkView::WORKSPACE_SIZE> scratch{};
    auto permission = [](void*) { return true; };
    EXPECT_FALSE(compareTintaReplayMarks(file, *store, course, catalog, *pack, kind, scratch, permission, nullptr));
    auto item = tinta::core::ItemState::fresh(uid);
    item.flags = tinta::core::item_flag::kStarred;
    if (kind == TintaReplayMarkKind::Stars) {
      ASSERT_TRUE(store->putItem(item));
    } else {
      ASSERT_TRUE(store->completion(EventKind::ReadingComplete, uid, true));
    }
    ASSERT_TRUE(compareTintaReplayMarks(file, *store, course, catalog, *pack, kind, scratch, permission, nullptr));
    EXPECT_EQ(log, retained);
    log.resize(4);
    EXPECT_FALSE(compareTintaReplayMarks(file, *store, course, catalog, *pack, kind, scratch, permission, nullptr));
    log = retained;
    EXPECT_FALSE(compareTintaReplayMarks(
        file, *store, course, catalog, *pack, kind, scratch, [](void*) { return false; }, nullptr));
    log.back() ^= 1;
    const auto corrupt = log;
    EXPECT_FALSE(compareTintaReplayMarks(file, *store, course, catalog, *pack, kind, scratch, permission, nullptr));
    EXPECT_EQ(log, corrupt);
    log = retained;
    if (kind == TintaReplayMarkKind::Stars) {
      item.flags = 0;
      ASSERT_TRUE(store->putItem(item));
    } else {
      ASSERT_TRUE(store->completion(EventKind::ReadingComplete, uid, false));
    }
    EXPECT_FALSE(compareTintaReplayMarks(file, *store, course, catalog, *pack, kind, scratch, permission, nullptr));
    EXPECT_EQ(log, retained);
  }
}

TEST(HalTintaLegacyReferences, DistinguishesStarsAndTitleKeysWithoutChangingMarks) {
  std::ifstream input(TINTA_TEST_PACK, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  tinta::core::pack::MemorySource source(bytes.data(), bytes.size());
  auto pack = std::make_unique<tinta::core::pack::Pack>();
  ASSERT_EQ(pack->open(source), tinta::core::pack::PackStatus::Ok);
  ASSERT_GT(pack->itemCount(), 0u);
  CourseUidLookup items(source);
  ASSERT_TRUE(items.begin());
  for (unsigned readings = 0; readings < 2; ++readings) {
    inventory_hal_test::state = {};
    auto& log = inventory_hal_test::state.files["/reviewed-marks"];
    log = {'T', 'M', 'K', '1'};
    uint32_t key = pack->uidAt(0);
    if (readings) {
      bool unique = false;
      for (uint32_t index = 0; index < pack->count(tinta::core::pack::Section::Stor); ++index) {
        tinta::core::pack::Story story;
        ASSERT_TRUE(pack->story(index, story));
        ASSERT_TRUE(tintaLegacyStoryKey(*pack, story, key));
        uint32_t identity = 0;
        if (resolveTintaLegacyStoryKey(*pack, key, identity) == LegacyStoryIdentityResult::Matched) {
          unique = true;
          break;
        }
      }
      ASSERT_TRUE(unique);
    }
    legacyMarkRecord(log, key, 1);
    const auto original = log;
    HalFile file("/reviewed-marks");
    HalTintaLegacyMarkView view(file);
    std::array<uint8_t, HalTintaLegacyMarkView::WORKSPACE_SIZE> scratch{};
    ASSERT_TRUE(view.begin(scratch));
    LegacyMarkCatalogReport report{777, 888};
    ASSERT_TRUE(inspectTintaLegacyMarkReferences(view, items, *pack, readings, report));
    EXPECT_EQ(report.matched, 1);
    EXPECT_EQ(report.retired, 0);
    bool permitted = false;
    report = {777, 888};
    EXPECT_FALSE(inspectTintaLegacyMarkReferences(
        view, items, *pack, readings, report, [](void* context) { return *static_cast<bool*>(context); }, &permitted));
    EXPECT_EQ(report.matched, 777);
    EXPECT_EQ(report.retired, 888);
    EXPECT_EQ(log, original);
    if (readings) {
      tinta::core::pack::Story ambiguous;
      ASSERT_TRUE(pack->story(0, ambiguous));
      ASSERT_TRUE(tintaLegacyStoryKey(*pack, ambiguous, key));
      uint32_t identity = 0;
      ASSERT_EQ(resolveTintaLegacyStoryKey(*pack, key, identity), LegacyStoryIdentityResult::Ambiguous);
      log = {'T', 'M', 'K', '1'};
      legacyMarkRecord(log, key, 1);
      const auto reviewedAmbiguity = log;
      ASSERT_TRUE(view.begin(scratch));
      EXPECT_FALSE(inspectTintaLegacyMarkReferences(view, items, *pack, true, report));
      EXPECT_EQ(report.matched, 777);
      EXPECT_EQ(report.retired, 888);
      EXPECT_EQ(log, reviewedAmbiguity);
    }
  }
}
TEST(HalTintaLegacyReferences, ReservedItemUidIsNotRejectedByGenericTitleKeyLogParser) {
  inventory_hal_test::state = {};
  auto& bytes = inventory_hal_test::state.files["/reviewed-marks"];
  bytes = {'T', 'M', 'K', '1'};
  legacyMarkRecord(bytes, UINT32_MAX, 1);
  HalFile file("/reviewed-marks");
  HalTintaLegacyMarkView view(file);
  std::array<uint8_t, HalTintaLegacyMarkView::WORKSPACE_SIZE> scratch{};
  ASSERT_TRUE(view.begin(scratch));
  uint32_t key = 0;
  ASSERT_TRUE(view.identityAt(0, key));
  EXPECT_EQ(key, UINT32_MAX);
}

namespace {
std::vector<uint8_t> savedLegacySession(uint32_t firstUid) {
  std::vector<uint8_t> bytes(7 + 1 + 2 + 20 + 28 + 10 + 4 + 32 + 4, 0);
  std::memcpy(bytes.data(), "TSES", 4);
  binary_record::putU16(bytes.data() + 4, 3);
  bytes[6] = 1;
  bytes[7] = 1;
  binary_record::putU16(bytes.data() + 8, 62);
  auto* controller = bytes.data() + 10;
  binary_record::putU32(controller, 42);
  binary_record::putU16(controller + 16, 42);
  auto* queue = controller + 20;
  std::memcpy(queue, "TSQ1", 4);
  binary_record::putU16(queue + 4, 2);
  binary_record::putU16(queue + 6, 2);
  binary_record::putU16(queue + 8, 99);
  binary_record::putU32(queue + 28, firstUid);
  binary_record::putU32(queue + 33, 0xfffffffe);
  binary_record::putU32(queue + 38, binary_record::crc32(queue, 38));
  bytes[72] = 1;
  binary_record::putU32(bytes.data() + 104, binary_record::crc32(bytes.data(), 104));
  return bytes;
}
}  // namespace
TEST(HalTintaLegacySession, PreservesSavedQueueAndReportsChangedJournalAndRetiredItems) {
  std::ifstream input(TINTA_TEST_PACK, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> packBytes{std::istreambuf_iterator<char>(input), {}};
  tinta::core::pack::MemorySource source(packBytes.data(), packBytes.size());
  auto pack = std::make_unique<tinta::core::pack::Pack>();
  ASSERT_EQ(pack->open(source), tinta::core::pack::PackStatus::Ok);
  CourseUidLookup items(source);
  ASSERT_TRUE(items.begin());
  inventory_hal_test::state = {};
  auto& bytes = inventory_hal_test::state.files["/reviewed-session"];
  bytes = savedLegacySession(pack->uidAt(0));
  const auto original = bytes;
  HalFile file("/reviewed-session");
  std::array<uint8_t, 256> scratch{};
  LegacySessionReport report;
  ASSERT_TRUE(inspectTintaLegacySession(file, items, 43, 0, 0, 8, 40, 200, scratch, report));
  EXPECT_EQ(report.queued, 2);
  EXPECT_EQ(report.mapped, 1);
  EXPECT_EQ(report.retired, 1);
  EXPECT_EQ(report.day, 99);
  EXPECT_TRUE(report.journalChanged);
  EXPECT_TRUE(report.hasSnapshot);
  EXPECT_EQ(bytes, original);
}
TEST(TintaLegacySession, NestedQueueCorruptionAndEveryEnvelopeBitPreserveOutputOnFailure) {
  const auto original = savedLegacySession(7);
  for (size_t bit = 0; bit < original.size() * 8; ++bit) {
    auto bytes = original;
    bytes[bit / 8] ^= uint8_t(1u << (bit % 8));
    LegacySessionView view;
    view.journalCount = 777;
    EXPECT_FALSE(decodeTintaLegacySession(bytes, 8, 40, 200, view)) << bit;
    EXPECT_EQ(view.journalCount, 777u);
  }
  auto bytes = original;
  bytes[58] = 0;
  binary_record::putU32(bytes.data() + 104, binary_record::crc32(bytes.data(), 104));
  LegacySessionView view;
  EXPECT_FALSE(decodeTintaLegacySession(bytes, 8, 40, 200, view));
}

TEST(TintaLegacySession, AcceptsOlderEnvelopeAndQueueVersionsAndEnforcesNativeLimits) {
  auto bytes = savedLegacySession(7);
  LegacySessionView view;
  ASSERT_TRUE(decodeTintaLegacySession(bytes, 8, 40, 200, view));
  EXPECT_FALSE(decodeTintaLegacySession(bytes, 8, 40, 1, view));
  EXPECT_FALSE(decodeTintaLegacySession(bytes, 0, 40, 200, view));
  EXPECT_FALSE(decodeTintaLegacySession(bytes, 8, 1, 200, view));
  // Version 2 envelope has no snapshot; the controller/queue bytes stay identical.
  bytes.resize(76);
  binary_record::putU16(bytes.data() + 4, 2);
  binary_record::putU32(bytes.data() + 72, binary_record::crc32(bytes.data(), 72));
  ASSERT_TRUE(decodeTintaLegacySession(bytes, 8, 40, 200, view));
  EXPECT_TRUE(view.file.snapshot.empty());
  // Version 1 queue omits the four-byte tag tail of the version 2 header.
  bytes.erase(bytes.begin() + 54, bytes.begin() + 58);
  binary_record::putU16(bytes.data() + 8, 58);
  binary_record::putU16(bytes.data() + 26, 38);
  binary_record::putU16(bytes.data() + 34, 1);
  binary_record::putU32(bytes.data() + 64, binary_record::crc32(bytes.data() + 30, 34));
  binary_record::putU32(bytes.data() + 68, binary_record::crc32(bytes.data(), 68));
  ASSERT_TRUE(decodeTintaLegacySession(bytes, 8, 40, 200, view));
  EXPECT_EQ(view.queued, 2);
  EXPECT_EQ(view.kind, 0);
  EXPECT_EQ(view.tag, 0);
  // Version 1 envelope stores only a screen stack.
  bytes.resize(12);
  binary_record::putU16(bytes.data() + 4, 1);
  binary_record::putU32(bytes.data() + 8, binary_record::crc32(bytes.data(), 8));
  ASSERT_TRUE(decodeTintaLegacySession(bytes, 8, 40, 200, view));
  EXPECT_TRUE(view.file.session.empty());
  EXPECT_EQ(view.queued, 0);
}

TEST(HalTintaLegacySession, ReadCancellationWorkspaceAndPracticeTargetFailuresWithholdReports) {
  std::ifstream input(TINTA_TEST_PACK, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> packBytes{std::istreambuf_iterator<char>(input), {}};
  tinta::core::pack::MemorySource source(packBytes.data(), packBytes.size());
  CourseUidLookup items(source);
  ASSERT_TRUE(items.begin());
  for (unsigned fault = 0; fault < 5; ++fault) {
    inventory_hal_test::state = {};
    auto& state = inventory_hal_test::state;
    auto& bytes = state.files["/reviewed-session"];
    bytes = savedLegacySession(7);
    if (fault == 0) state.failRead = state.reads + 1;
    if (fault == 3 || fault == 4) {
      bytes[48] = 1;
      binary_record::putU16(bytes.data() + 54, fault == 3 ? 1 : 0x8000);
      binary_record::putU32(bytes.data() + 68, binary_record::crc32(bytes.data() + 30, 38));
      binary_record::putU32(bytes.data() + 104, binary_record::crc32(bytes.data(), 104));
    }
    const auto original = bytes;
    HalFile file("/reviewed-session");
    std::array<uint8_t, 256> scratch{};
    LegacySessionReport report;
    report.journalCount = 777;
    report.queued = 888;
    report.hasSnapshot = true;
    bool permitted = fault != 1;
    EXPECT_FALSE(inspectTintaLegacySession(
        file, items, 42, 0, 0, 8, 40, 200, std::span(scratch).first(fault == 2 ? 100 : 256), report,
        [](void* context) { return *static_cast<bool*>(context); }, &permitted));
    EXPECT_EQ(report.journalCount, 777u);
    EXPECT_EQ(report.queued, 888);
    EXPECT_TRUE(report.hasSnapshot);
    EXPECT_EQ(bytes, original);
  }
}

TEST(HalUnboundLessonMapping, MissingProfilesCarryNoProgressAndPresentProfilesUseNativeIdentities) {
  inventory_hal_test::state = {};
  companion_memory_test::internal = {1024 * 1024, 1024 * 1024, 1024 * 1024, 1024 * 1024};
  std::ifstream input(TINTA_TEST_PACK, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  auto pack = makeUniqueNoThrow<tinta::core::pack::Pack>();
  ASSERT_TRUE(pack);
  ASSERT_EQ(pack->open(bytes.data(), bytes.size()), tinta::core::pack::PackStatus::Ok);
  const auto lessons = pack->count(tinta::core::pack::Section::Less);
  ASSERT_GT(lessons, 0u);
  std::array<uint8_t, 512> scratch{};
  UnboundCourseProfileReport profile;
  profile.profile.currentLesson = UINT16_MAX;
  TintaLegacyLessonMapping result;
  ASSERT_TRUE(mapUnboundCourseLessons(profile, *pack, *pack, scratch, result, [](void*) { return true; }, nullptr));
  EXPECT_EQ(result.currentLesson, 0);
  EXPECT_EQ(result.unlockedThrough, 0);
  EXPECT_EQ(result.retainedCompletions, 0);
  profile.present = true;
  profile.status = tinta::core::Profile::LoadResult::Loaded;
  profile.profile.currentLesson = lessons;
  profile.profile.unlockedThrough = lessons - 1;
  ASSERT_TRUE(mapUnboundCourseLessons(profile, *pack, *pack, scratch, result, [](void*) { return true; }, nullptr));
  EXPECT_EQ(result.currentLesson, lessons);
  EXPECT_EQ(result.retainedCompletions, lessons);
  EXPECT_EQ(result.retiredCompletions, 0);
  result.currentLesson = 123;
  uint32_t calls = 0;
  EXPECT_FALSE(mapUnboundCourseLessons(
      profile, *pack, *pack, scratch, result, [](void* context) { return ++*static_cast<uint32_t*>(context) < 4; },
      &calls));
  EXPECT_EQ(result.currentLesson, 123);
  profile.status = tinta::core::Profile::LoadResult::Corrupt;
  EXPECT_FALSE(mapUnboundCourseLessons(profile, *pack, *pack, scratch, result, [](void*) { return true; }, nullptr));
  EXPECT_EQ(result.currentLesson, 123);
}
