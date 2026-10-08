#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "lib/Companion/CompanionTintaBody.h"
#include "lib/Companion/CompanionTintaCompletionSet.h"
#include "lib/Companion/CompanionTintaDayLog.h"
#include "lib/Companion/CompanionTintaDerivedManifest.h"
#include "lib/Companion/CompanionTintaLegacyStoryIdentity.h"
#include "lib/Companion/CompanionTintaNativeMarkIdentity.h"
#include "lib/Companion/CompanionTintaPackStoryIdentity.h"
#include "lib/Companion/CompanionTintaProgressMutation.h"
#include "lib/Companion/CompanionTintaStoryIdentity.h"

using namespace companion;

TEST(TintaDerivedManifest, SharedFixtureBindingsAndCorruptionChecks) {
  std::ifstream input(std::string(COMPANION_FIXTURE_DIR) + "/TintaDerivedManifest-v1.fixture", std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  TintaDerivedManifestView view;
  ASSERT_TRUE(view.decode(bytes));
  Identity course{}, storage{};
  Digest pack{}, frontier{};
  course.fill(7);
  storage.fill(1);
  pack.fill(3);
  frontier.fill(4);
  EXPECT_TRUE(view.matches(course, storage, pack, frontier));
  EXPECT_EQ(view.revision(), 1u);
  EXPECT_EQ(view.studyDay(), 42);
  EXPECT_EQ(view.length(TintaDerivedFile::Items), 1056u);
  EXPECT_EQ(view.length(TintaDerivedFile::LocalReviews), 0u);
  EXPECT_EQ(view.hash(TintaDerivedFile::Days).size(), 32u);
  course[0] ^= 1;
  EXPECT_FALSE(view.matches(course, storage, pack, frontier));
  for (size_t i = 0; i < bytes.size(); ++i) {
    auto corrupt = bytes;
    corrupt[i] ^= 1;
    EXPECT_FALSE(view.decode(corrupt));
    EXPECT_EQ(view.revision(), 0u);
    EXPECT_TRUE(view.hash(TintaDerivedFile::Items).empty());
  }
  for (size_t length = 0; length < bytes.size(); ++length) EXPECT_FALSE(view.decode(std::span(bytes).first(length)));
  for (size_t offset : {118u, 168u}) {
    auto invalid = bytes;
    invalid[offset] = 1;
    binary_record::putU32(invalid.data() + 328, binary_record::crc32(invalid.data(), 328));
    EXPECT_FALSE(view.decode(invalid));
  }
}

TEST(TintaCompletionSet, SharedFixtureAndStrictTypeOrderExtentChecks) {
  constexpr std::array<uint8_t, 24> bytes{84, 67, 83, 49, 2, 0, 0, 0, 2,   0,   0,   0,
                                          7,  0,  0,  0,  9, 0, 0, 0, 215, 125, 242, 166};
  const std::span input(bytes);
  TintaCompletionSetValidator validator;
  ASSERT_TRUE(validator.begin(input.first(12), bytes.size(), TintaCompletionKind::Readings));
  ASSERT_TRUE(validator.identity(input.subspan(12, 4)));
  ASSERT_TRUE(validator.identity(input.subspan(16, 4)));
  EXPECT_TRUE(validator.finish(input.last(4)));
  EXPECT_FALSE(validator.begin(input.first(12), bytes.size(), TintaCompletionKind::Lessons));
  EXPECT_FALSE(validator.begin(input.first(12), bytes.size() - 1, TintaCompletionKind::Readings));
  ASSERT_TRUE(validator.begin(input.first(12), bytes.size(), TintaCompletionKind::Readings));
  EXPECT_FALSE(validator.finish(input.last(4)));
  EXPECT_FALSE(validator.identity(input.subspan(12, 4)));
  ASSERT_TRUE(validator.begin(input.first(12), bytes.size(), TintaCompletionKind::Readings));
  ASSERT_TRUE(validator.identity(input.subspan(16, 4)));
  EXPECT_FALSE(validator.identity(input.subspan(12, 4)));
  ASSERT_TRUE(validator.begin(input.first(12), bytes.size(), TintaCompletionKind::Readings));
  ASSERT_TRUE(validator.identity(input.subspan(12, 4)));
  ASSERT_TRUE(validator.identity(input.subspan(16, 4)));
  std::array<uint8_t, 4> corrupt{};
  EXPECT_FALSE(validator.finish(corrupt));
}

TEST(TintaStoryIdentity, EverySourceReadFailurePreservesOutputAndRetryUsesCompleteTitle) {
  namespace pk = tinta::core::pack;
  std::ifstream input(TINTA_TEST_PACK, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  class Source final : public pk::PackSource {
   public:
    explicit Source(const std::vector<uint8_t>& bytes) : bytes(bytes) {}
    uint32_t size() const override { return bytes.size(); }
    bool read(uint32_t offset, void* output, uint32_t length) override {
      ++reads;
      if (reads == failRead || offset > bytes.size() || length > bytes.size() - offset) return false;
      std::memcpy(output, bytes.data() + offset, length);
      return true;
    }
    const std::vector<uint8_t>& bytes;
    uint32_t reads = 0;
    uint32_t failRead = 0;
  } source(bytes);
  pk::Pack pack;
  ASSERT_EQ(pack.open(source), pk::PackStatus::Ok);
  ASSERT_GT(pack.count(pk::Section::Stor), 0u);
  for (uint32_t i = 0; i < pack.count(pk::Section::Stor); ++i) {
    pk::Story story;
    source.failRead = 0;
    ASSERT_TRUE(pack.story(i, story));
    source.reads = 0;
    uint32_t expected = 0;
    ASSERT_TRUE(tintaPackStoryIdentity(pack, story, expected));
    const uint32_t readCount = source.reads;
    ASSERT_GT(readCount, 0u);
    for (uint32_t failure = 1; failure <= readCount; ++failure) {
      source.reads = 0;
      source.failRead = failure;
      uint32_t actual = 123;
      EXPECT_FALSE(tintaPackStoryIdentity(pack, story, actual));
      EXPECT_EQ(actual, 123u);
      source.failRead = 0;
      source.reads = 0;
      ASSERT_TRUE(tintaPackStoryIdentity(pack, story, actual));
      EXPECT_EQ(actual, expected);
    }
  }
  EXPECT_EQ(pack.arenaPeak(), 0u);
  EXPECT_EQ(pack.arenaOverflows(), 0u);
}

TEST(TintaLegacyStoryIdentity, ResolvesUniqueKeysAndRejectsAmbiguityAndReadFailures) {
  namespace pk = tinta::core::pack;
  std::ifstream input(TINTA_TEST_PACK, std::ios::binary);
  ASSERT_TRUE(input.good());
  std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  class Source final : public pk::PackSource {
   public:
    explicit Source(const std::vector<uint8_t>& bytes) : bytes(bytes) {}
    uint32_t size() const override { return bytes.size(); }
    bool read(uint32_t offset, void* output, uint32_t length) override {
      if (++reads == failRead || offset > bytes.size() || length > bytes.size() - offset) return false;
      std::memcpy(output, bytes.data() + offset, length);
      return true;
    }
    const std::vector<uint8_t>& bytes;
    uint32_t reads = 0, failRead = 0;
  } source(bytes);
  pk::Pack pack;
  ASSERT_EQ(pack.open(source), pk::PackStatus::Ok);
  ASSERT_GT(pack.count(pk::Section::Stor), 1u);
  pk::Story first;
  uint32_t key = 0;
  bool foundUnique = false;
  for (uint32_t i = 0; i < pack.count(pk::Section::Stor); ++i) {
    ASSERT_TRUE(pack.story(i, first));
    key = pack.hashStr(first.title);
    unsigned matches = 0;
    for (uint32_t j = 0; j < pack.count(pk::Section::Stor); ++j) {
      pk::Story candidate;
      ASSERT_TRUE(pack.story(j, candidate));
      if (pack.hashStr(candidate.title) == key) ++matches;
    }
    if (matches == 1) {
      foundUnique = true;
      break;
    }
  }
  ASSERT_TRUE(foundUnique);
  ASSERT_NE(key, 0u);
  uint32_t expected = 0, actual = 99;
  ASSERT_TRUE(tintaPackStoryIdentity(pack, first, expected));
  class Catalog final : public TintaSubjectCatalog {
   public:
    TintaSubjectMembership result = TintaSubjectMembership::Present;
    uint32_t requested = 0;
    TintaSubjectMembership contains(EventKind, uint32_t uid) override {
      requested = uid;
      return result;
    }
  } catalog;
  ASSERT_TRUE(resolveTintaNativeMarkIdentity(pack, catalog, EventKind::ReadingComplete, key, actual));
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(catalog.requested, expected);
  ASSERT_TRUE(resolveTintaNativeMarkIdentity(pack, catalog, EventKind::Star, 321, actual));
  EXPECT_EQ(actual, 321U);
  for (auto membership : {TintaSubjectMembership::Missing, TintaSubjectMembership::IoError}) {
    catalog.result = membership;
    actual = 99;
    EXPECT_FALSE(resolveTintaNativeMarkIdentity(pack, catalog, EventKind::ReadingComplete, key, actual));
    EXPECT_EQ(actual, 99U);
    EXPECT_FALSE(resolveTintaNativeMarkIdentity(pack, catalog, EventKind::Star, 321, actual));
    EXPECT_EQ(actual, 99U);
  }
  EXPECT_FALSE(resolveTintaNativeMarkIdentity(pack, catalog, EventKind::Review, 321, actual));
  EXPECT_EQ(actual, 99U);
  source.reads = 0;
  ASSERT_EQ(resolveTintaLegacyStoryKey(pack, key, actual), LegacyStoryIdentityResult::Matched);
  EXPECT_EQ(actual, expected);
  const auto reads = source.reads;
  for (uint32_t fail = 1; fail <= reads; ++fail) {
    source.reads = 0;
    source.failRead = fail;
    actual = 99;
    EXPECT_EQ(resolveTintaLegacyStoryKey(pack, key, actual), LegacyStoryIdentityResult::IoError);
    EXPECT_EQ(actual, 99u);
  }
  source.reads = source.failRead = 0;
  actual = 99;
  ASSERT_EQ(resolveTintaStableStoryIdentity(pack, expected, actual), LegacyStoryIdentityResult::Matched);
  EXPECT_EQ(actual, key);
  const auto reverseReads = source.reads;
  for (uint32_t fail = 1; fail <= reverseReads; ++fail) {
    source.reads = 0;
    source.failRead = fail;
    actual = 99;
    EXPECT_EQ(resolveTintaStableStoryIdentity(pack, expected, actual), LegacyStoryIdentityResult::IoError);
    EXPECT_EQ(actual, 99u);
  }
  source.reads = source.failRead = 0;
  actual = 99;
  EXPECT_EQ(resolveTintaStableStoryIdentity(pack, 0, actual), LegacyStoryIdentityResult::Invalid);
  EXPECT_EQ(resolveTintaStableStoryIdentity(pack, UINT32_MAX, actual), LegacyStoryIdentityResult::Invalid);
  EXPECT_EQ(resolveTintaStableStoryIdentity(pack, 123, actual), LegacyStoryIdentityResult::Missing);
  EXPECT_EQ(actual, 99u);
  EXPECT_EQ(resolveTintaLegacyStoryKey(pack, 0, actual), LegacyStoryIdentityResult::Invalid);
  EXPECT_EQ(actual, 99u);
  EXPECT_EQ(resolveTintaLegacyStoryKey(pack, 123, actual), LegacyStoryIdentityResult::Missing);
  EXPECT_EQ(actual, 99u);
  pk::Header header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  bool changed = false;
  for (uint16_t i = 0; i < header.sectionCount; ++i) {
    pk::DirEntry entry;
    std::memcpy(&entry, bytes.data() + header.directoryOffset + i * sizeof(entry), sizeof(entry));
    if (entry.tag != pk::makeTag("STOR")) continue;
    ASSERT_GE(entry.count, 2u);
    pk::Story duplicate;
    std::memcpy(&duplicate, bytes.data() + entry.offset, sizeof(duplicate));
    duplicate.title = first.title;
    std::memcpy(bytes.data() + entry.offset, &duplicate, sizeof(duplicate));
    changed = true;
  }
  ASSERT_TRUE(changed);
  actual = 99;
  EXPECT_EQ(resolveTintaLegacyStoryKey(pack, key, actual), LegacyStoryIdentityResult::Ambiguous);
  EXPECT_EQ(resolveTintaStableStoryIdentity(pack, expected, actual), LegacyStoryIdentityResult::Ambiguous);
  EXPECT_EQ(actual, 99u);
  EXPECT_EQ(pack.arenaPeak(), 0u);
}

TEST(TintaStoryIdentity, PackMappingMatchesFullTitleReferenceWithoutStringArena) {
  std::ifstream input(TINTA_TEST_PACK, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  tinta::core::pack::Pack pack;
  ASSERT_EQ(pack.open(bytes.data(), bytes.size()), tinta::core::pack::PackStatus::Ok);
  ASSERT_GT(pack.count(tinta::core::pack::Section::Stor), 0u);
  for (uint32_t i = 0; i < pack.count(tinta::core::pack::Section::Stor); ++i) {
    tinta::core::pack::Story story;
    ASSERT_TRUE(pack.story(i, story));
    uint32_t lessonIdentity = UINT32_MAX;
    if (story.lesson != tinta::core::pack::kNone16) {
      tinta::core::pack::Lesson lesson;
      tinta::core::pack::Unit unit;
      ASSERT_TRUE(pack.lesson(story.lesson, lesson));
      ASSERT_TRUE(pack.unit(lesson.unit, unit));
      lessonIdentity = (static_cast<uint32_t>(unit.number) << 16) | lesson.number;
    }
    uint32_t expected = 2166136261u;
    const auto hashByte = [&](uint8_t byte) { expected = (expected ^ byte) * 16777619u; };
    for (const auto byte : std::string("TST1")) hashByte(byte);
    hashByte(static_cast<uint8_t>(story.kind));
    for (unsigned shift = 0; shift < 32; shift += 8) hashByte(lessonIdentity >> shift);
    for (const char* title = pack.str(story.title); *title; ++title) hashByte(*title);
    uint32_t actual = 99;
    ASSERT_TRUE(tintaPackStoryIdentity(pack, story, actual));
    EXPECT_EQ(actual, expected ? expected : 1u);
    story.title = UINT32_MAX;
    actual = 99;
    EXPECT_FALSE(tintaPackStoryIdentity(pack, story, actual));
    EXPECT_EQ(actual, 99u);
  }
}

TEST(TintaStoryIdentity, MatchesIndependentFixturesAcrossChunkBoundaries) {
  constexpr std::array<uint8_t, 4> title{'H', 'o', 'l', 'a'};
  for (size_t split = 0; split <= title.size(); ++split) {
    TintaStoryIdentity builder;
    uint32_t identity = 99;
    EXPECT_FALSE(builder.finish(identity));
    EXPECT_EQ(identity, 99u);
    ASSERT_TRUE(builder.begin(0, 0x10002));
    ASSERT_TRUE(builder.title(std::span(title).first(split)));
    ASSERT_TRUE(builder.title(std::span(title).subspan(split)));
    ASSERT_TRUE(builder.finish(identity));
    EXPECT_EQ(identity, 3122944624u);
    ASSERT_TRUE(builder.begin(1, 0x10002));
    ASSERT_TRUE(builder.title(title));
    ASSERT_TRUE(builder.finish(identity));
    EXPECT_EQ(identity, 285639611u);
    ASSERT_TRUE(builder.begin(1, UINT32_MAX));
    ASSERT_TRUE(builder.title(title));
    ASSERT_TRUE(builder.finish(identity));
    EXPECT_EQ(identity, 2120389584u);
    EXPECT_FALSE(builder.begin(2, UINT32_MAX));
    EXPECT_FALSE(builder.finish(identity));
    ASSERT_TRUE(builder.begin(1, UINT32_MAX));
    constexpr std::array<uint8_t, 1> nul{0};
    EXPECT_FALSE(builder.title(nul));
    EXPECT_FALSE(builder.title(title));
    EXPECT_FALSE(builder.finish(identity));
  }
}

TEST(TintaDayLog, RejectsAggregateOverflowWithoutWrapping) {
  constexpr std::array<uint8_t, 4> header{'T', 'D', 'L', '1'};
  std::array<uint8_t, 12> record{};
  binary_record::putU16(record.data() + 2, UINT16_MAX);
  binary_record::putU16(record.data() + 10, binary_record::crc32(record.data(), 10));
  TintaDayLogValidator validator;
  ASSERT_TRUE(validator.begin(header, 4 + 65538 * 12));
  for (uint32_t i = 0; i < 65537; ++i) ASSERT_TRUE(validator.record(record));
  EXPECT_FALSE(validator.record(record));
  EXPECT_FALSE(validator.finish());
}

TEST(TintaDayLog, SharedAppleFixtureAndStrictStreamingValidation) {
  constexpr std::array<uint8_t, 4> header{'T', 'D', 'L', '1'};
  constexpr std::array<uint8_t, 12> fixture{2, 0, 3, 0, 2, 0, 1, 0, 2, 0, 0x44, 0x1b};
  TintaDayLogValidator validator;
  EXPECT_FALSE(validator.finish());
  ASSERT_TRUE(validator.begin(header, 16));
  ASSERT_TRUE(validator.record(fixture));
  EXPECT_TRUE(validator.finish());
  EXPECT_FALSE(validator.record(fixture));
  EXPECT_FALSE(validator.finish());
  ASSERT_TRUE(validator.begin(header, 4));
  EXPECT_TRUE(validator.finish());
  EXPECT_FALSE(validator.begin(header, 17));
  ASSERT_TRUE(validator.begin(header, 16));
  EXPECT_FALSE(validator.finish());
  EXPECT_FALSE(validator.record(fixture));
  for (size_t i = 0; i < fixture.size(); ++i) {
    auto corrupt = fixture;
    corrupt[i] ^= 1;
    ASSERT_TRUE(validator.begin(header, 16));
    EXPECT_FALSE(validator.record(corrupt));
  }
  auto record = fixture;
  binary_record::putU16(record.data() + 4, 4);
  binary_record::putU16(record.data() + 10, binary_record::crc32(record.data(), 10));
  ASSERT_TRUE(validator.begin(header, 16));
  ASSERT_TRUE(validator.record(record));
  EXPECT_FALSE(validator.finish());
  ASSERT_TRUE(validator.begin(header, 28));
  ASSERT_TRUE(validator.record(fixture));
  record = fixture;
  record[0] = 1;
  binary_record::putU16(record.data() + 10, binary_record::crc32(record.data(), 10));
  EXPECT_FALSE(validator.record(record));
}

TEST(TintaProgressMutation, ExactReviewUndoAndCombinedFlagsMapWithoutChangingOutputOnFailure) {
  Identity course{};
  course.fill(7);
  auto before = tinta::core::ItemState::fresh(17);
  auto after = before;
  TintaProgressMutation output;
  auto entry = tinta::core::JournalEntry{};
  entry.uid = 17;
  entry.op = 3;
  entry.arg = 5;
  ASSERT_TRUE(mapTintaProgressMutation(entry, before, after, 1234, course, {8750, 730}, {}, output));
  ASSERT_EQ(output.count, 1);
  EXPECT_EQ(output.bodies[0].responseMilliseconds, 1234u);
  EXPECT_EQ(output.bodies[0].configuration.retentionBasisPoints, 8750);
  EXPECT_EQ(output.bodies[0].configuration.maximumInterval, 730);
  const auto original = output.bodies;
  entry.op = tinta::core::JournalEntry::kUndo << 3;
  EXPECT_FALSE(mapTintaProgressMutation(entry, before, after, 0, course, {}, {}, output));
  EXPECT_EQ(output.bodies, original);
  EXPECT_EQ(output.count, 1);
  EventIdentity target{course, 9, 11};
  ASSERT_TRUE(mapTintaProgressMutation(entry, before, after, 0, course, {}, target, output));
  EXPECT_EQ(output.bodies[0].undoTarget, target);
  entry.op = tinta::core::JournalEntry::kSetFlags << 3;
  entry.arg = tinta::core::item_flag::kSuspended | tinta::core::item_flag::kStarred;
  after.flags = entry.arg;
  ASSERT_TRUE(mapTintaProgressMutation(entry, before, after, 0, course, {}, {}, output));
  ASSERT_EQ(output.count, 2);
  EXPECT_EQ(output.bodies[0].kind, EventKind::Suspension);
  EXPECT_EQ(output.bodies[1].kind, EventKind::Star);
  EXPECT_TRUE(output.bodies[0].enabled);
  EXPECT_TRUE(output.bodies[1].enabled);
  before = after;
  after.flags = 0;
  entry.arg = 0;
  ASSERT_TRUE(mapTintaProgressMutation(entry, before, after, 0, course, {}, {}, output));
  EXPECT_FALSE(output.bodies[0].enabled);
  EXPECT_FALSE(output.bodies[1].enabled);
  before.flags = tinta::core::item_flag::kLeech;
  EXPECT_FALSE(mapTintaProgressMutation(entry, before, after, 0, course, {}, {}, output));
  const auto flags = output.bodies;
  before.flags = 0;
  after.uid = 18;
  EXPECT_FALSE(mapTintaProgressMutation(entry, before, after, 0, course, {}, {}, output));
  EXPECT_EQ(output.bodies, flags);
  EXPECT_EQ(output.count, 2);
}

namespace {
TintaBody review() {
  TintaBody body;
  body.course.fill(7);
  body.uid = 1;
  body.grade = 3;
  body.responseMilliseconds = 1000;
  return body;
}
std::vector<uint8_t> fixture(const char* name) {
  std::ifstream file(std::string(COMPANION_FIXTURE_DIR) + "/" + name + ".json");
  const std::string json{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  const auto key = json.find("\"binaryHex\": \"");
  if (key == std::string::npos) return {};
  const auto start = key + 14;
  const auto end = json.find('"', start);
  if (end == std::string::npos) return {};
  std::vector<uint8_t> bytes;
  bytes.reserve((end - start) / 2);
  for (size_t i = start; i < end; i += 2)
    bytes.push_back(static_cast<uint8_t>(std::stoul(json.substr(i, 2), nullptr, 16)));
  return bytes;
}
}  // namespace

TEST(CompanionTintaBody, SharedReviewAndUndoFixtures) {
  for (const auto name : {"TintaReviewBody", "TintaUndoBody"}) {
    const auto bytes = fixture(name);
    ASSERT_FALSE(bytes.empty());
    TintaBody decoded;
    ASSERT_TRUE(decodeTintaBody(bytes, decoded));
    EXPECT_EQ(decoded.uid, 1U);
    Identity course;
    course.fill(7);
    EXPECT_EQ(decoded.course, course);
    if (decoded.kind == EventKind::Review) {
      EXPECT_EQ(decoded, review());
    }
    std::array<uint8_t, MAX_TINTA_BODY_SIZE> encoded{};
    EXPECT_EQ(encodeTintaBody(decoded, encoded), bytes.size());
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), encoded.begin()));
    const auto preserved = decoded;
    for (size_t size = 0; size < bytes.size(); ++size) {
      EXPECT_FALSE(decodeTintaBody(std::span(bytes).first(size), decoded));
      EXPECT_EQ(decoded, preserved);
    }
    auto extra = bytes;
    extra.push_back(0);
    EXPECT_FALSE(decodeTintaBody(extra, decoded));
  }
}

TEST(CompanionTintaBody, ControlsAndInvalidValues) {
  for (auto kind : {EventKind::Suspension, EventKind::Star, EventKind::LessonComplete, EventKind::ReadingComplete}) {
    TintaBody body;
    body.course.fill(7);
    body.uid = 1;
    body.kind = kind;
    for (bool enabled : {false, true}) {
      body.enabled = enabled;
      std::array<uint8_t, 23> bytes{};
      ASSERT_EQ(encodeTintaBody(body, bytes), bytes.size());
      TintaBody decoded;
      ASSERT_TRUE(decodeTintaBody(bytes, decoded));
      EXPECT_EQ(decoded, body);
      bytes[22] = 2;
      EXPECT_FALSE(decodeTintaBody(bytes, decoded));
    }
  }
  auto bytes = fixture("TintaReviewBody");
  for (auto [offset, value] : {std::pair{0, 2}, {1, 1}, {22, 0}, {22, 5}, {23, 10}, {28, 2}, {29, 2}}) {
    auto malformed = bytes;
    malformed[offset] = value;
    TintaBody decoded;
    EXPECT_FALSE(decodeTintaBody(malformed, decoded));
  }
  TintaBody body = review();
  std::array<uint8_t, 34> output;
  output.fill(0xAA);
  EXPECT_EQ(encodeTintaBody(body, std::span(output).first(33)), 0U);
  body.uid = 0;
  EXPECT_EQ(encodeTintaBody(body, output), 0U);
  EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](auto byte) { return byte == 0xAA; }));
}

TEST(CompanionTintaBody, EnvelopeDigestConfigurationAndUndoAncestry) {
  const auto body = review();
  SyncEvent event;
  event.identity.origin.fill(1);
  event.identity.epoch = 1;
  event.identity.sequence = 2;
  event.storageGeneration.fill(2);
  event.kind = body.kind;
  event.resource.fill(3);
  event.bodyHash.fill(4);
  event.schedulerVersion = 1;
  event.schedulerConfiguration.fill(5);
  EXPECT_TRUE(validateTintaEnvelope(event, body, event.bodyHash, event.schedulerConfiguration));
  Digest wrong{};
  EXPECT_FALSE(validateTintaEnvelope(event, body, wrong, event.schedulerConfiguration));
  EXPECT_FALSE(validateTintaEnvelope(event, body, event.bodyHash, wrong));
  event.studyDay = 65536;
  EXPECT_FALSE(validateTintaEnvelope(event, body, event.bodyHash, event.schedulerConfiguration));
  event.studyDay = 1;
  TintaBody undo;
  ASSERT_TRUE(decodeTintaBody(fixture("TintaUndoBody"), undo));
  event.kind = EventKind::UndoReview;
  event.schedulerVersion = 0;
  event.schedulerConfiguration = {};
  EXPECT_FALSE(validateTintaEnvelope(event, undo, event.bodyHash, wrong));
  event.ancestorCount = 1;
  event.ancestors[0] = undo.undoTarget;
  EXPECT_TRUE(validateTintaEnvelope(event, undo, event.bodyHash, wrong));
  event.ancestorCount = 2;
  event.ancestors[1] = event.ancestors[0];
  EXPECT_FALSE(validateTintaEnvelope(event, undo, event.bodyHash, wrong));
}
