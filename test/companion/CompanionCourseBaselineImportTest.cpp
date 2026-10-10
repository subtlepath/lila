#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "lib/Companion/CompanionCourseBaselineImportConsent.h"
#include "lib/Companion/CompanionCourseBaselineOrphanConsent.h"
#include "lib/Companion/CompanionCourseBaselinePublication.h"
#include "lib/Companion/CompanionCourseBaselinePublicationStore.h"
#include "lib/Companion/CompanionCourseBaselineReview.h"
#include "lib/Companion/CompanionCourseBaselineReviewPage.h"
#include "lib/Companion/CompanionUnboundCourseMigrationCoordinator.h"
#include "lib/Companion/CompanionUnboundCourseMigrationIntent.h"
#include "lib/Companion/CompanionUnboundCourseMigrationIntentStore.h"
#include "lib/Companion/CompanionUnboundCourseMigrationRequest.h"

using namespace companion;
namespace {
class Storage final : public TransferStorage {
 public:
  std::map<std::string, std::vector<uint8_t>> files;
  unsigned mutations = 0, fail = 0, verifications = 0;
  bool after = false, allowed = true, statError = false, readError = false;
  bool allowStageRemoval = false;
  bool revokeWrite = false, revokeRename = false, revokeRead = false;
  void (*onStat)(void*) = nullptr;
  void* statContext = nullptr;
  bool prepare() override { return true; }
  FileStatus stat(const char* path, uint64_t& size) override {
    if (onStat) onStat(statContext);
    if (statError) return FileStatus::Error;
    const auto found = files.find(path);
    if (found == files.end()) return FileStatus::Missing;
    size = found->second.size();
    return FileStatus::Present;
  }
  bool read(const char* path, uint64_t offset, std::span<uint8_t> bytes) override {
    const auto found = files.find(path);
    if (readError || found == files.end() || offset > found->second.size() ||
        bytes.size() > found->second.size() - offset)
      return false;
    std::copy_n(found->second.begin() + offset, bytes.size(), bytes.begin());
    if (revokeRead) allowed = false;
    return true;
  }
  bool write(const char* path, uint64_t offset, std::span<const uint8_t> bytes, bool truncate) override {
    const bool failed = ++mutations == fail;
    if (failed && !after) return false;
    auto& data = files[path];
    if (truncate) data.clear();
    if (offset > data.size()) return false;
    data.resize(offset + bytes.size());
    std::copy(bytes.begin(), bytes.end(), data.begin() + offset);
    if (revokeWrite) allowed = false;
    return !failed;
  }
  bool rename(const char* from, const char* to) override {
    const bool failed = ++mutations == fail;
    if (failed && !after) return false;
    if (!files.contains(from) || files.contains(to)) return false;
    files[to] = std::move(files.at(from));
    files.erase(from);
    if (revokeRename) allowed = false;
    return !failed;
  }
  bool resize(const char*, uint64_t) override {
    ADD_FAILURE();
    return false;
  }
  bool remove(const char* path) override {
    if (!allowStageRemoval || !std::string_view(path).ends_with(".tmp")) {
      ADD_FAILURE();
      return false;
    }
    const bool failed = ++mutations == fail;
    if (failed && !after) return false;
    if (!files.erase(path)) return false;
    return !failed;
  }
  bool verify(const char*, uint64_t, const Digest&, std::span<uint8_t>) override {
    ++verifications;
    return false;
  }
};
struct Fixture {
  Storage storage;
  std::array<uint8_t, 256> scratch{};
  CourseBaselineImportRequest request;
  Fixture() {
    request.generation.fill(1);
    request.owner.fill(2);
    request.transaction.fill(3);
    request.manifest.kind = ContentKind::Course;
    request.manifest.formatVersion = 1;
    request.manifest.length = 4097;
    request.manifest.logicalIdentity.fill(4);
    request.manifest.contentHash.fill(5);
    request.reviewHash.fill(6);
    storage.files[ACTIVE_COURSE_PATH] = {9};
    storage.files["/tinta/courses/04040404040404040404040404040404/items.bin"] = {17};
  }
  static bool permitted(void* context) { return static_cast<Storage*>(context)->allowed; }
  CourseBaselineImportConsent consent() { return CourseBaselineImportConsent(storage, scratch, permitted, &storage); }
  std::string canonical() const {
    return "/.crosspoint/companion/course-baseline-03030303030303030303030303030303.consent";
  }
  std::vector<uint8_t> encoded() const {
    std::vector<uint8_t> result(COURSE_BASELINE_IMPORT_REQUEST_SIZE);
    EXPECT_TRUE(encodeCourseBaselineImportRequest(request, result));
    return result;
  }
};
void seal(std::span<uint8_t> bytes) {
  const auto crc = binary_record::crc32(bytes.data(), bytes.size() - 4);
  for (unsigned at = 0; at < 4; ++at) bytes[bytes.size() - 4 + at] = static_cast<uint8_t>(crc >> (8 * at));
}
}  // namespace

TEST(CompanionCourseBaselineImport, SharedFixtureAndCorruptionsPreserveOutput) {
  Fixture f;
  std::array<uint8_t, COURSE_BASELINE_IMPORT_REQUEST_SIZE> bytes{}, encoded{};
  std::ifstream input(COURSE_BASELINE_IMPORT_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()));
  ASSERT_EQ(input.peek(), std::char_traits<char>::eof());
  CourseBaselineImportRequest decoded;
  ASSERT_TRUE(decodeCourseBaselineImportRequest(bytes, decoded));
  EXPECT_EQ(decoded, f.request);
  ASSERT_TRUE(encodeCourseBaselineImportRequest(decoded, encoded));
  EXPECT_EQ(encoded, bytes);
  const std::string requestPath = COURSE_BASELINE_IMPORT_FIXTURE;
  std::ifstream framedInput(
      requestPath.substr(0, requestPath.find_last_of('/') + 1) + "CourseBaselineBeginFrame-v1.fixture",
      std::ios::binary);
  ASSERT_TRUE(framedInput);
  const std::vector<uint8_t> framed((std::istreambuf_iterator<char>(framedInput)), {});
  FrameView frame;
  EXPECT_EQ(decodeFrame(framed, false, frame), FrameError::Unauthorized);
  ASSERT_EQ(decodeFrame(framed, true, frame), FrameError::None);
  EXPECT_EQ(frame.command, Command::BeginCourseBaseline);
  EXPECT_EQ(frame.requestId, 0x12345678u);
  EXPECT_TRUE(std::equal(frame.payload.begin(), frame.payload.end(), bytes.begin(), bytes.end()));
  for (size_t at = 0; at < bytes.size(); ++at) {
    SCOPED_TRACE(at);
    auto corrupt = bytes;
    corrupt[at] ^= 1;
    auto output = f.request;
    EXPECT_FALSE(decodeCourseBaselineImportRequest(corrupt, output));
    EXPECT_EQ(output, f.request);
  }
  for (auto [at, length] : {std::pair<size_t, size_t>{8, 16}, {24, 16}, {40, 16}, {58, 32}, {103, 16}, {119, 32}}) {
    auto invalid = bytes;
    std::fill_n(invalid.begin() + at, length, 0);
    seal(invalid);
    auto output = f.request;
    EXPECT_FALSE(decodeCourseBaselineImportRequest(invalid, output));
    EXPECT_EQ(output, f.request);
  }
  for (size_t at : {4u, 5u, 6u, 7u}) {
    auto invalid = bytes;
    invalid[at] ^= 1;
    seal(invalid);
    auto unchanged = f.request;
    EXPECT_FALSE(decodeCourseBaselineImportRequest(invalid, unchanged));
    EXPECT_EQ(unchanged, f.request);
  }
  auto output = f.request;
  EXPECT_FALSE(decodeCourseBaselineImportRequest(std::span(bytes).first(bytes.size() - 1), output));
  EXPECT_EQ(output, f.request);
}

TEST(CompanionCourseBaselineImport, RejectsInvalidManifestAndOverlappingBuffers) {
  Fixture f;
  const auto expected = f.encoded();
  for (unsigned field = 0; field < 4; ++field) {
    auto invalid = f.request;
    if (field == 0) invalid.manifest.kind = ContentKind::Epub;
    if (field == 1) invalid.manifest.formatVersion = 2;
    if (field == 2) invalid.manifest.length = 0;
    if (field == 3) invalid.manifest.length = uint64_t{UINT32_MAX} + 1;
    auto output = expected;
    EXPECT_FALSE(encodeCourseBaselineImportRequest(invalid, output));
    EXPECT_EQ(output, expected);
  }
  struct Aliased {
    CourseBaselineImportRequest request;
    std::array<uint8_t, 256> rest{};
  } aliased{f.request};
  auto bytes = std::span(reinterpret_cast<uint8_t*>(&aliased), COURSE_BASELINE_IMPORT_REQUEST_SIZE);
  EXPECT_FALSE(encodeCourseBaselineImportRequest(aliased.request, bytes));
  EXPECT_EQ(aliased.request, f.request);
  EXPECT_FALSE(decodeCourseBaselineImportRequest(bytes, aliased.request));
  EXPECT_EQ(aliased.request, f.request);
}

TEST(CompanionCourseBaselineImport, ConfirmationCannotBeRetargetedOrReusedWithAChangedReview) {
  Fixture f;
  TransferDeclaration declaration;
  declaration.manifest = f.request.manifest;
  declaration.state.storageGeneration = f.request.generation;
  declaration.state.owner = f.request.owner;
  declaration.state.transaction = f.request.transaction;
  declaration.state.contentHash = declaration.manifest.contentHash;
  declaration.state.length = declaration.manifest.length;
  ASSERT_TRUE(matchesCourseBaselineImportRequest(f.request, f.request.generation, f.request.owner, f.request.reviewHash,
                                                 declaration));
  for (unsigned field = 0; field < 10; ++field) {
    auto changed = declaration;
    auto generation = f.request.generation, owner = f.request.owner;
    auto review = f.request.reviewHash;
    if (field == 0) generation[0] ^= 1;
    if (field == 1) owner[0] ^= 1;
    if (field == 2) review[0] ^= 1;
    if (field == 3) changed.state.transaction[0] ^= 1;
    if (field == 4) changed.state.owner[0] ^= 1;
    if (field == 5) changed.state.storageGeneration[0] ^= 1;
    if (field == 6) {
      changed.manifest.contentHash[0] ^= 1;
      changed.state.contentHash = changed.manifest.contentHash;
    }
    if (field == 7) changed.manifest.logicalIdentity[0] ^= 1;
    if (field == 8) changed.state.durableOffset = 1;
    if (field == 9) changed.state.phase = TransferPhase::Committed;
    EXPECT_FALSE(matchesCourseBaselineImportRequest(f.request, generation, owner, review, changed));
  }
}

TEST(CompanionCourseBaselineImport, SavesImmutableConsentWithoutChangingPackOrLearnerState) {
  Fixture f;
  const auto original = f.storage.files;
  auto consent = f.consent();
  EXPECT_EQ(consent.load(f.request.transaction, f.request), CourseBaselineConsentResult::Missing);
  ASSERT_EQ(consent.persist(f.request), CourseBaselineConsentResult::Ok);
  EXPECT_EQ(f.storage.files.at(f.canonical()), f.encoded());
  EXPECT_EQ(f.storage.files.size(), original.size() + 1);
  for (const auto& [path, bytes] : original) EXPECT_EQ(f.storage.files.at(path), bytes);
  const auto writes = f.storage.mutations;
  EXPECT_EQ(consent.persist(f.request), CourseBaselineConsentResult::Ok);
  EXPECT_EQ(f.storage.mutations, writes);
  CourseBaselineImportRequest output;
  EXPECT_EQ(consent.load(f.request.transaction, output), CourseBaselineConsentResult::Ok);
  EXPECT_EQ(output, f.request);
  EXPECT_EQ(f.storage.verifications, 0u);
  auto changed = f.request;
  changed.reviewHash[0] ^= 1;
  const auto saved = f.storage.files;
  EXPECT_EQ(consent.persist(changed), CourseBaselineConsentResult::Conflict);
  EXPECT_EQ(f.storage.files, saved);
  EXPECT_EQ(f.storage.mutations, writes);
}

TEST(CompanionCourseBaselineImport, RecoversWriteAndRenameFailuresWithoutRepeatingApproval) {
  for (unsigned mutation = 1; mutation <= 2; ++mutation) {
    for (bool after : {false, true}) {
      SCOPED_TRACE(mutation);
      SCOPED_TRACE(after);
      Fixture f;
      const auto original = f.storage.files;
      f.storage.fail = mutation;
      f.storage.after = after;
      auto consent = f.consent();
      EXPECT_EQ(consent.persist(f.request), CourseBaselineConsentResult::IoError);
      for (const auto& [path, bytes] : original) EXPECT_EQ(f.storage.files.at(path), bytes);
      f.storage.fail = 0;
      auto restarted = f.consent();
      EXPECT_EQ(restarted.persist(f.request), CourseBaselineConsentResult::Ok);
      EXPECT_EQ(f.storage.files.at(f.canonical()), f.encoded());
      EXPECT_FALSE(f.storage.files.contains(f.canonical() + ".tmp"));
    }
  }
}

TEST(CompanionCourseBaselineImport, ExplicitApprovalRecoversEveryMatchingTornStage) {
  for (size_t length = 0; length < COURSE_BASELINE_IMPORT_REQUEST_SIZE; ++length) {
    Fixture f;
    f.storage.allowStageRemoval = true;
    auto bytes = f.encoded();
    bytes.resize(length);
    f.storage.files[f.canonical() + ".tmp"] = bytes;
    const auto original = f.storage.files;
    auto consent = f.consent();
    EXPECT_EQ(consent.persist(f.request), CourseBaselineConsentResult::Corrupt);
    EXPECT_EQ(f.storage.files, original);
    ASSERT_EQ(consent.persist(f.request, true), CourseBaselineConsentResult::Ok);
    EXPECT_EQ(f.storage.files.at(f.canonical()), f.encoded());
    EXPECT_FALSE(f.storage.files.contains(f.canonical() + ".tmp"));
    for (const auto& [path, data] : original) {
      if (path != f.canonical() + ".tmp") {
        EXPECT_EQ(f.storage.files.at(path), data);
      }
    }
  }
}

TEST(CompanionCourseBaselineImport, TornStageRecoveryPreservesMismatchAndRetriesRemovalPowerCuts) {
  for (const bool after : {false, true}) {
    Fixture f;
    f.storage.allowStageRemoval = true;
    auto bytes = f.encoded();
    bytes.resize(100);
    bytes.back() ^= 1;
    f.storage.files[f.canonical() + ".tmp"] = bytes;
    auto consent = f.consent();
    EXPECT_EQ(consent.persist(f.request, true), CourseBaselineConsentResult::Corrupt);
    EXPECT_EQ(f.storage.mutations, 0u);
    EXPECT_EQ(f.storage.files.at(f.canonical() + ".tmp"), bytes);
    bytes.back() ^= 1;
    f.storage.files[f.canonical() + ".tmp"] = bytes;
    f.storage.fail = 1;
    f.storage.after = after;
    EXPECT_EQ(consent.persist(f.request, true), CourseBaselineConsentResult::IoError);
    EXPECT_EQ(f.storage.files.contains(f.canonical() + ".tmp"), !after);
    EXPECT_FALSE(f.storage.files.contains(f.canonical()));
    f.storage.fail = 0;
    auto restarted = f.consent();
    ASSERT_EQ(restarted.persist(f.request, true), CourseBaselineConsentResult::Ok);
    EXPECT_EQ(f.storage.files.at(f.canonical()), f.encoded());
  }
}

TEST(CompanionCourseBaselineImport, TornRecoveryRefusesPermissionErrorsAndInsufficientScratch) {
  for (unsigned fault = 0; fault < 5; ++fault) {
    SCOPED_TRACE(fault);
    Fixture f;
    f.storage.allowStageRemoval = true;
    auto bytes = f.encoded();
    bytes.resize(100);
    f.storage.files[f.canonical() + ".tmp"] = bytes;
    const auto evidence = f.storage.files;
    if (fault == 0) f.storage.allowed = false;
    if (fault == 1) f.storage.revokeRead = true;
    if (fault == 2) f.storage.readError = true;
    if (fault == 3) f.storage.statError = true;
    std::span<uint8_t> loan = f.scratch;
    if (fault == 4) loan = loan.first(COURSE_BASELINE_IMPORT_REQUEST_SIZE);
    CourseBaselineImportConsent consent(f.storage, loan, Fixture::permitted, &f.storage);
    EXPECT_EQ(consent.persist(f.request, true), fault < 2    ? CourseBaselineConsentResult::Busy
                                                : fault == 4 ? CourseBaselineConsentResult::Invalid
                                                             : CourseBaselineConsentResult::IoError);
    EXPECT_EQ(f.storage.files, evidence);
    EXPECT_EQ(f.storage.mutations, 0u);
  }
}

TEST(CompanionCourseBaselineImport, TornRecoveryRefusesCanonicalConsentAppearingBeforeRemoval) {
  Fixture f;
  f.storage.allowStageRemoval = true;
  auto bytes = f.encoded();
  bytes.resize(100);
  f.storage.files[f.canonical() + ".tmp"] = bytes;
  struct Context {
    Fixture* fixture;
    unsigned calls = 0;
  } context{&f};
  f.storage.statContext = &context;
  f.storage.onStat = [](void* raw) {
    auto& state = *static_cast<Context*>(raw);
    if (++state.calls == 4) state.fixture->storage.files[state.fixture->canonical()] = state.fixture->encoded();
  };
  auto consent = f.consent();
  EXPECT_EQ(consent.persist(f.request, true), CourseBaselineConsentResult::Conflict);
  EXPECT_EQ(f.storage.files.at(f.canonical() + ".tmp"), bytes);
  EXPECT_EQ(f.storage.files.at(f.canonical()), f.encoded());
  EXPECT_EQ(f.storage.mutations, 0u);
}

TEST(CompanionCourseBaselineOrphanConsent, PreservesEveryStageLengthWithoutGrantingApproval) {
  for (size_t length = 0; length <= COURSE_BASELINE_IMPORT_REQUEST_SIZE; ++length) {
    Fixture f;
    auto bytes = f.encoded();
    bytes.resize(length);
    if (length) bytes.back() ^= 1;
    f.storage.files[f.canonical() + ".tmp"] = bytes;
    const auto before = f.storage.files;
    CourseBaselineOrphanConsent recovery(f.storage, Fixture::permitted, &f.storage);
    ASSERT_EQ(recovery.rollback(f.request.transaction), CourseBaselineConsentResult::Ok);
    EXPECT_FALSE(f.storage.files.contains(f.canonical() + ".tmp"));
    EXPECT_FALSE(f.storage.files.contains(f.canonical()));
    EXPECT_EQ(f.storage.files.at(f.canonical() + ".orphan"), bytes);
    const auto complete = f.storage.files;
    EXPECT_EQ(recovery.rollback(f.request.transaction), CourseBaselineConsentResult::Ok);
    EXPECT_EQ(f.storage.files, complete);
    for (const auto& [path, data] : before) {
      if (path != f.canonical() + ".tmp") {
        EXPECT_EQ(f.storage.files.at(path), data);
      }
    }
  }
}

TEST(CompanionCourseBaselineOrphanConsent, RefusesAuthorityConflictsErrorsAndUnavailablePermission) {
  for (unsigned fault = 0; fault < 10; ++fault) {
    Fixture f;
    f.storage.files[f.canonical() + ".tmp"] = {1, 2, 3};
    const auto prefix = f.canonical().substr(0, f.canonical().size() - std::string_view(".consent").size());
    static constexpr const char* PROTECTED[] = {".consent", ".prepared", ".prepared.tmp", ".published",
                                                ".published.tmp"};
    if (fault < 5) f.storage.files[prefix + PROTECTED[fault]] = {9};
    if (fault == 5) {
      f.storage.files[f.canonical() + ".orphan"] = {8};
      for (unsigned slot = 0; slot < 256; ++slot) {
        char suffix[16]{};
        snprintf(suffix, sizeof(suffix), ".orphan-%02x", slot);
        f.storage.files[f.canonical() + suffix] = {9};
      }
    }
    if (fault == 6) f.storage.allowed = false;
    if (fault == 7) f.storage.statError = true;
    if (fault == 8) f.storage.files.at(f.canonical() + ".tmp").resize(COURSE_BASELINE_IMPORT_REQUEST_SIZE + 1);
    const auto before = f.storage.files;
    CourseBaselineOrphanConsent recovery(f.storage, Fixture::permitted, &f.storage);
    EXPECT_EQ(recovery.rollback(fault == 9 ? Identity{} : f.request.transaction),
              fault < 6    ? CourseBaselineConsentResult::Conflict
              : fault == 6 ? CourseBaselineConsentResult::Busy
              : fault == 7 ? CourseBaselineConsentResult::IoError
              : fault == 8 ? CourseBaselineConsentResult::Corrupt
                           : CourseBaselineConsentResult::Invalid);
    EXPECT_EQ(f.storage.files, before);
    EXPECT_EQ(f.storage.mutations, 0u);
  }
}

TEST(CompanionCourseBaselineOrphanConsent, RepeatedAttemptsPreserveEachDiagnosticCopy) {
  Fixture f;
  CourseBaselineOrphanConsent recovery(f.storage, Fixture::permitted, &f.storage);
  f.storage.files[f.canonical() + ".orphan"] = {9};
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    f.storage.files[f.canonical() + ".tmp"] = {static_cast<uint8_t>(attempt)};
    ASSERT_EQ(recovery.rollback(f.request.transaction), CourseBaselineConsentResult::Ok);
    EXPECT_EQ(f.storage.files.at(f.canonical() + ".orphan"), std::vector<uint8_t>{9});
    for (unsigned retained = 0; retained <= attempt; ++retained) {
      char suffix[16]{};
      snprintf(suffix, sizeof(suffix), ".orphan-%02x", retained);
      EXPECT_EQ(f.storage.files.at(f.canonical() + suffix), std::vector<uint8_t>{static_cast<uint8_t>(retained)});
    }
    const auto complete = f.storage.files;
    EXPECT_EQ(recovery.rollback(f.request.transaction), CourseBaselineConsentResult::Ok);
    EXPECT_EQ(f.storage.files, complete);
    EXPECT_FALSE(f.storage.files.contains(f.canonical()));
  }
}

TEST(CompanionCourseBaselineOrphanConsent, RecoversRenameAcknowledgementLossWithoutRemovingEvidence) {
  for (const bool after : {false, true}) {
    Fixture f;
    const std::vector<uint8_t> bytes{1, 2, 3};
    f.storage.files[f.canonical() + ".tmp"] = bytes;
    f.storage.fail = 1;
    f.storage.after = after;
    CourseBaselineOrphanConsent recovery(f.storage, Fixture::permitted, &f.storage);
    EXPECT_EQ(recovery.rollback(f.request.transaction), CourseBaselineConsentResult::IoError);
    EXPECT_EQ(f.storage.files.at(f.canonical() + (after ? ".orphan" : ".tmp")), bytes);
    EXPECT_FALSE(f.storage.files.contains(f.canonical()));
    f.storage.fail = 0;
    CourseBaselineOrphanConsent restarted(f.storage, Fixture::permitted, &f.storage);
    EXPECT_EQ(restarted.rollback(f.request.transaction), CourseBaselineConsentResult::Ok);
    EXPECT_EQ(f.storage.files.at(f.canonical() + ".orphan"), bytes);
  }
}

TEST(CompanionCourseBaselineImport, PreservesTornForeignAndDuplicateEvidence) {
  for (unsigned fault = 0; fault < 5; ++fault) {
    SCOPED_TRACE(fault);
    Fixture f;
    auto encoded = f.encoded();
    if (fault == 0) encoded.resize(encoded.size() / 2);
    if (fault == 1) encoded.back() ^= 1;
    if (fault == 2) {
      auto foreign = f.request;
      foreign.transaction[0] ^= 1;
      ASSERT_TRUE(encodeCourseBaselineImportRequest(foreign, encoded));
    }
    f.storage.files[f.canonical() + ".tmp"] = encoded;
    if (fault >= 3) f.storage.files[f.canonical()] = f.encoded();
    if (fault == 4) f.storage.files.at(f.canonical()).back() ^= 1;
    const auto before = f.storage.files;
    auto consent = f.consent();
    EXPECT_EQ(consent.persist(f.request),
              fault == 3 ? CourseBaselineConsentResult::Conflict : CourseBaselineConsentResult::Corrupt);
    EXPECT_EQ(f.storage.files, before);
    EXPECT_EQ(f.storage.mutations, 0u);
    if (fault >= 3) {
      auto output = f.request;
      output.reviewHash[0] ^= 1;
      const auto previous = output;
      EXPECT_EQ(consent.load(f.request.transaction, output),
                fault == 3 ? CourseBaselineConsentResult::Conflict : CourseBaselineConsentResult::Corrupt);
      EXPECT_EQ(output, previous);
      EXPECT_EQ(f.storage.files, before);
    }
  }
}

TEST(CompanionCourseBaselineImport, RejectsChangedCompleteStageAndPreservesLoadOutputOnErrors) {
  Fixture f;
  auto altered = f.request;
  altered.manifest.logicalIdentity[0] ^= 1;
  auto bytes = f.encoded();
  ASSERT_TRUE(encodeCourseBaselineImportRequest(altered, bytes));
  f.storage.files[f.canonical() + ".tmp"] = bytes;
  const auto staged = f.storage.files;
  auto consent = f.consent();
  EXPECT_EQ(consent.persist(f.request), CourseBaselineConsentResult::Conflict);
  EXPECT_EQ(f.storage.files, staged);
  f.storage.files.erase(f.canonical() + ".tmp");
  ASSERT_EQ(consent.persist(f.request), CourseBaselineConsentResult::Ok);
  for (unsigned fault = 0; fault < 3; ++fault) {
    f.storage.statError = fault == 0;
    f.storage.readError = fault == 1;
    f.storage.allowed = fault != 2;
    auto output = altered;
    EXPECT_EQ(consent.load(f.request.transaction, output),
              fault == 2 ? CourseBaselineConsentResult::Busy : CourseBaselineConsentResult::IoError);
    EXPECT_EQ(output, altered);
  }
}

TEST(CompanionCourseBaselineImport, PermissionLossPreservesEvidenceAndRetryCompletes) {
  for (bool afterRename : {false, true}) {
    Fixture f;
    f.storage.revokeWrite = !afterRename;
    f.storage.revokeRename = afterRename;
    auto consent = f.consent();
    EXPECT_EQ(consent.persist(f.request), CourseBaselineConsentResult::Busy);
    f.storage.allowed = true;
    f.storage.revokeWrite = f.storage.revokeRename = false;
    EXPECT_EQ(consent.persist(f.request), CourseBaselineConsentResult::Ok);
    EXPECT_EQ(f.storage.files.at(f.canonical()), f.encoded());
    const auto saved = f.storage.files;
    auto output = f.request;
    output.reviewHash[0] ^= 1;
    const auto previous = output;
    f.storage.revokeRead = true;
    EXPECT_EQ(consent.load(f.request.transaction, output), CourseBaselineConsentResult::Busy);
    EXPECT_EQ(output, previous);
    EXPECT_EQ(f.storage.files, saved);
    f.storage.allowed = true;
    f.storage.revokeRead = false;
    EXPECT_EQ(consent.load(f.request.transaction, output), CourseBaselineConsentResult::Ok);
    EXPECT_EQ(output, f.request);
  }
}

TEST(CompanionCourseBaselineImport, RefusesReentryAndInsufficientScratchBeforeMutation) {
  Fixture f;
  auto consent = f.consent();
  struct Reentry {
    CourseBaselineImportConsent* consent;
    const Identity* transaction;
    unsigned calls = 0;
  } reentry{&consent, &f.request.transaction};
  f.storage.statContext = &reentry;
  f.storage.onStat = [](void* context) {
    auto& reentry = *static_cast<Reentry*>(context);
    CourseBaselineImportRequest output;
    EXPECT_EQ(reentry.consent->load(*reentry.transaction, output), CourseBaselineConsentResult::Busy);
    ++reentry.calls;
  };
  EXPECT_EQ(consent.persist(f.request), CourseBaselineConsentResult::Ok);
  EXPECT_GT(reentry.calls, 0u);
  f.storage.onStat = nullptr;
  CourseBaselineImportConsent shortOwner(f.storage, std::span(f.scratch).first(COURSE_BASELINE_IMPORT_REQUEST_SIZE - 1),
                                         Fixture::permitted, &f.storage);
  const auto mutations = f.storage.mutations;
  EXPECT_EQ(shortOwner.persist(f.request), CourseBaselineConsentResult::Invalid);
  EXPECT_EQ(f.storage.mutations, mutations);
}

namespace {
struct MigrationFixture : Fixture {
  UnboundCourseMigrationIntent intent;
  bool bound = false, isolated = false;
  unsigned boundMutations = 0, isolationMutations = 0, verifications = 0, rejectPhase = 0;
  MigrationFixture() {
    intent.reader.fill(7);
    intent.request.original = request;
    intent.activePack = request.manifest;
    storage.allowStageRemoval = true;
  }
  UnboundCourseMigrationHooks hooks() {
    return {this,
            [](void* raw, const UnboundCourseMigrationIntent& record, bool) {
              auto& f = *static_cast<MigrationFixture*>(raw);
              ++f.verifications;
              const auto phase = static_cast<unsigned>(record.phase);
              return phase != f.rejectPhase && (phase == 1 || (phase == 2 && f.bound) || (phase == 3 && f.isolated));
            },
            [](void* raw, const UnboundCourseMigrationIntent&) {
              auto& f = *static_cast<MigrationFixture*>(raw);
              EXPECT_TRUE(f.storage.files.contains(UNBOUND_COURSE_INTENT_PATHS[0]));
              if (!f.bound) ++f.boundMutations;
              f.bound = true;
              return true;
            },
            [](void* raw, const UnboundCourseMigrationIntent&) {
              auto& f = *static_cast<MigrationFixture*>(raw);
              EXPECT_TRUE(f.storage.files.contains(UNBOUND_COURSE_INTENT_PATHS[1]));
              EXPECT_TRUE(f.bound);
              if (!f.isolated) ++f.isolationMutations;
              f.isolated = true;
              return true;
            }};
  }
};
}  // namespace

TEST(UnboundCourseMigrationCoordinator, EveryPublicationCutResumesWithoutRepeatingCompletedMutations) {
  for (unsigned cut = 1; cut <= 6; ++cut) {
    for (const bool after : {false, true}) {
      MigrationFixture f;
      f.storage.fail = cut;
      f.storage.after = after;
      UnboundCourseMigrationCoordinator coordinator(f.storage, f.scratch, Fixture::permitted, &f.storage);
      EXPECT_EQ(coordinator.run(f.intent, f.hooks()), UnboundCourseIntentResult::IoError);
      EXPECT_EQ(coordinator.completed(), nullptr);
      f.storage.fail = 0;
      UnboundCourseMigrationCoordinator restored(f.storage, f.scratch, Fixture::permitted, &f.storage);
      ASSERT_EQ(restored.run(f.intent, f.hooks()), UnboundCourseIntentResult::Ok);
      ASSERT_NE(restored.completed(), nullptr);
      EXPECT_EQ(restored.completed()->phase, UnboundCourseMigrationPhase::Isolated);
      EXPECT_EQ(f.boundMutations, 1u);
      EXPECT_EQ(f.isolationMutations, 1u);
      const auto files = f.storage.files;
      const auto writes = f.storage.mutations;
      ASSERT_EQ(restored.run(f.intent, f.hooks()), UnboundCourseIntentResult::Ok);
      EXPECT_EQ(f.storage.files, files);
      EXPECT_EQ(f.storage.mutations, writes);
      EXPECT_EQ(f.boundMutations, 1u);
      EXPECT_EQ(f.isolationMutations, 1u);
      f.storage.allowed = false;
      EXPECT_EQ(restored.completed(), nullptr);
      f.storage.allowed = true;
      EXPECT_EQ(restored.completed(), nullptr);
    }
  }
}

TEST(UnboundCourseMigrationCoordinator, NativeVerificationFailureWithholdsCompletionAndRetryPreservesPhase) {
  for (unsigned phase = 1; phase <= 3; ++phase) {
    MigrationFixture f;
    f.rejectPhase = phase;
    UnboundCourseMigrationCoordinator coordinator(f.storage, f.scratch, Fixture::permitted, &f.storage);
    EXPECT_EQ(coordinator.run(f.intent, f.hooks()), UnboundCourseIntentResult::VerificationFailed);
    EXPECT_EQ(coordinator.completed(), nullptr);
    if (phase == 1) EXPECT_EQ(f.storage.mutations, 0u);
    if (phase == 2) EXPECT_EQ(f.isolationMutations, 0u);
    f.rejectPhase = 0;
    ASSERT_EQ(coordinator.run(f.intent, f.hooks()), UnboundCourseIntentResult::Ok);
    EXPECT_EQ(f.boundMutations, 1u);
    EXPECT_EQ(f.isolationMutations, 1u);
  }
}

TEST(UnboundCourseMigrationCoordinator, EveryTornPhaseResumesItsNativeStateAndRetainedPredecessors) {
  for (unsigned phase = 1; phase <= 3; ++phase) {
    for (size_t length = 0; length < UNBOUND_COURSE_MIGRATION_INTENT_SIZE; ++length) {
      MigrationFixture f;
      UnboundCourseMigrationIntentStore intents(f.storage, f.scratch, Fixture::permitted, &f.storage);
      auto record = f.intent;
      for (unsigned prior = 1; prior < phase; ++prior) {
        record.phase = static_cast<UnboundCourseMigrationPhase>(prior);
        ASSERT_EQ(intents.persist(record), UnboundCourseIntentResult::Ok);
      }
      f.bound = phase >= 2;
      f.boundMutations = f.bound ? 1 : 0;
      f.isolated = phase == 3;
      f.isolationMutations = f.isolated ? 1 : 0;
      record.phase = static_cast<UnboundCourseMigrationPhase>(phase);
      std::array<uint8_t, UNBOUND_COURSE_MIGRATION_INTENT_SIZE> bytes{};
      ASSERT_TRUE(encodeUnboundCourseMigrationIntent(record, bytes));
      f.storage.files[UNBOUND_COURSE_INTENT_STAGES[phase - 1]] = {bytes.begin(), bytes.begin() + length};
      UnboundCourseMigrationCoordinator coordinator(f.storage, f.scratch, Fixture::permitted, &f.storage);
      ASSERT_EQ(coordinator.run(f.intent, f.hooks()), UnboundCourseIntentResult::Ok);
      ASSERT_NE(coordinator.completed(), nullptr);
      EXPECT_EQ(f.boundMutations, 1u);
      EXPECT_EQ(f.isolationMutations, 1u);
      for (const auto* stage : UNBOUND_COURSE_INTENT_STAGES) EXPECT_FALSE(f.storage.files.contains(stage));
      for (const auto* path : UNBOUND_COURSE_INTENT_PATHS) EXPECT_TRUE(f.storage.files.contains(path));
    }
  }
}

TEST(UnboundCourseMigrationCoordinator, ForeignEvidenceAndCallbackChangesCannotStartBinding) {
  for (const bool duringVerification : {false, true}) {
    MigrationFixture f;
    UnboundCourseMigrationIntentStore intents(f.storage, f.scratch, Fixture::permitted, &f.storage);
    ASSERT_EQ(intents.persist(f.intent), UnboundCourseIntentResult::Ok);
    auto hooks = f.hooks();
    if (duringVerification) {
      hooks.verify = [](void* raw, const UnboundCourseMigrationIntent&, bool) {
        auto& f = *static_cast<MigrationFixture*>(raw);
        f.storage.files.at(UNBOUND_COURSE_INTENT_PATHS[0])[8] ^= 1;
        return true;
      };
    } else
      f.storage.files.at(UNBOUND_COURSE_INTENT_PATHS[0])[8] ^= 1;
    UnboundCourseMigrationCoordinator coordinator(f.storage, f.scratch, Fixture::permitted, &f.storage);
    EXPECT_NE(coordinator.run(f.intent, hooks), UnboundCourseIntentResult::Ok);
    EXPECT_EQ(f.boundMutations, 0u);
    EXPECT_EQ(f.isolationMutations, 0u);
    EXPECT_EQ(coordinator.completed(), nullptr);
  }
}

TEST(UnboundCourseMigrationIntentStore, EveryMatchingTornPhaseRequiresFreshVerificationBeforeRecovery) {
  for (unsigned phase = 1; phase <= 3; ++phase) {
    for (size_t length = 0; length < UNBOUND_COURSE_MIGRATION_INTENT_SIZE; ++length) {
      for (const bool verified : {false, true}) {
        Fixture f;
        UnboundCourseMigrationIntent intent;
        intent.reader.fill(7);
        intent.request.original = f.request;
        intent.activePack = f.request.manifest;
        UnboundCourseMigrationIntentStore store(f.storage, f.scratch, Fixture::permitted, &f.storage);
        for (unsigned previous = 1; previous < phase; ++previous) {
          intent.phase = static_cast<UnboundCourseMigrationPhase>(previous);
          ASSERT_EQ(store.persist(intent), UnboundCourseIntentResult::Ok);
        }
        intent.phase = static_cast<UnboundCourseMigrationPhase>(phase);
        std::array<uint8_t, UNBOUND_COURSE_MIGRATION_INTENT_SIZE> bytes{};
        ASSERT_TRUE(encodeUnboundCourseMigrationIntent(intent, bytes));
        const auto* staged = UnboundCourseMigrationIntentStore::STAGES[phase - 1];
        f.storage.files[staged] = {bytes.begin(), bytes.begin() + length};
        const auto files = f.storage.files;
        const auto mutations = f.storage.mutations;
        EXPECT_EQ(store.persist(intent), UnboundCourseIntentResult::Corrupt);
        struct Context {
          bool verified;
          unsigned calls = 0;
        } context{verified};
        auto verify = [](void* raw, const UnboundCourseMigrationIntent&) {
          auto& context = *static_cast<Context*>(raw);
          ++context.calls;
          return context.verified;
        };
        f.storage.allowStageRemoval = true;
        const auto result = store.persist(intent, verify, &context);
        EXPECT_EQ(context.calls, 1u);
        if (verified) {
          ASSERT_EQ(result, UnboundCourseIntentResult::Ok);
          EXPECT_FALSE(f.storage.files.contains(staged));
          UnboundCourseMigrationIntent output;
          ASSERT_EQ(store.load(output), UnboundCourseIntentResult::Ok);
          EXPECT_EQ(output, intent);
        } else {
          EXPECT_EQ(result, UnboundCourseIntentResult::VerificationFailed);
          EXPECT_EQ(f.storage.files, files);
          EXPECT_EQ(f.storage.mutations, mutations);
        }
      }
    }
  }
}

TEST(UnboundCourseMigrationIntentStore, TornRecoveryPowerCutsRetainPredecessorsAndRetrySafely) {
  for (unsigned cut = 1; cut <= 3; ++cut) {
    for (const bool after : {false, true}) {
      Fixture f;
      UnboundCourseMigrationIntent intent;
      intent.reader.fill(7);
      intent.request.original = f.request;
      intent.activePack = f.request.manifest;
      UnboundCourseMigrationIntentStore store(f.storage, f.scratch, Fixture::permitted, &f.storage);
      ASSERT_EQ(store.persist(intent), UnboundCourseIntentResult::Ok);
      const auto predecessor = f.storage.files.at(UnboundCourseMigrationIntentStore::PATHS[0]);
      intent.phase = UnboundCourseMigrationPhase::Bound;
      std::array<uint8_t, UNBOUND_COURSE_MIGRATION_INTENT_SIZE> bytes{};
      ASSERT_TRUE(encodeUnboundCourseMigrationIntent(intent, bytes));
      f.storage.files[UnboundCourseMigrationIntentStore::STAGES[1]] = {bytes.begin(), bytes.begin() + 99};
      f.storage.allowStageRemoval = true;
      f.storage.fail = f.storage.mutations + cut;
      f.storage.after = after;
      auto verified = [](void*, const UnboundCourseMigrationIntent&) { return true; };
      EXPECT_EQ(store.persist(intent, verified), UnboundCourseIntentResult::IoError);
      EXPECT_EQ(f.storage.files.at(UnboundCourseMigrationIntentStore::PATHS[0]), predecessor);
      f.storage.fail = 0;
      UnboundCourseMigrationIntentStore restored(f.storage, f.scratch, Fixture::permitted, &f.storage);
      ASSERT_EQ(restored.persist(intent, verified), UnboundCourseIntentResult::Ok);
      UnboundCourseMigrationIntent output;
      ASSERT_EQ(restored.load(output), UnboundCourseIntentResult::Ok);
      EXPECT_EQ(output, intent);
      EXPECT_EQ(f.storage.files.at(UnboundCourseMigrationIntentStore::PATHS[0]), predecessor);
    }
  }
}

TEST(UnboundCourseMigrationIntentStore, NativeVerificationCannotEraseChangedPrefixOrPhaseEvidence) {
  for (unsigned fault = 0; fault < 6; ++fault) {
    Fixture f;
    UnboundCourseMigrationIntent intent;
    intent.reader.fill(7);
    intent.request.original = f.request;
    intent.activePack = f.request.manifest;
    UnboundCourseMigrationIntentStore store(f.storage, f.scratch, Fixture::permitted, &f.storage);
    std::array<uint8_t, UNBOUND_COURSE_MIGRATION_INTENT_SIZE> bytes{};
    ASSERT_TRUE(encodeUnboundCourseMigrationIntent(intent, bytes));
    const auto* path = UnboundCourseMigrationIntentStore::STAGES[0];
    f.storage.files[path] = {bytes.begin(), bytes.begin() + 99};
    struct Context {
      Fixture* f;
      unsigned fault;
      UnboundCourseMigrationIntentStore* store;
    } context{&f, fault, &store};
    const auto mutations = f.storage.mutations;
    f.storage.allowStageRemoval = true;
    auto verify = [](void* raw, const UnboundCourseMigrationIntent& expected) {
      auto& c = *static_cast<Context*>(raw);
      auto& storage = c.f->storage;
      EXPECT_EQ(c.store->persist(expected), UnboundCourseIntentResult::Busy);
      const auto* path = UnboundCourseMigrationIntentStore::STAGES[0];
      if (c.fault == 0) storage.files.at(path)[98] ^= 1;
      if (c.fault == 1) storage.files.at(path).pop_back();
      if (c.fault == 2) storage.allowed = false;
      if (c.fault == 3) storage.files.erase(path);
      if (c.fault == 4) storage.files[UnboundCourseMigrationIntentStore::PATHS[0]] = {1};
      if (c.fault == 5) storage.files[UnboundCourseMigrationIntentStore::STAGES[1]] = {1};
      return true;
    };
    EXPECT_NE(store.persist(intent, verify, &context), UnboundCourseIntentResult::Ok);
    EXPECT_EQ(f.storage.mutations, mutations);
  }
}

TEST(UnboundCourseMigrationIntentStore, ImmutablePhasesSurvivePublicationPowerCutsAndRepeatWithoutWrites) {
  for (unsigned cut = 1; cut <= 6; ++cut) {
    for (const bool after : {false, true}) {
      Fixture f;
      UnboundCourseMigrationIntent intent;
      intent.reader.fill(7);
      intent.request.original = f.request;
      intent.activePack = f.request.manifest;
      intent.activePack.contentHash[0] ^= 1;
      UnboundCourseMigrationIntentStore store(f.storage, f.scratch, Fixture::permitted, &f.storage);
      f.storage.fail = cut;
      f.storage.after = after;
      bool interrupted = false;
      for (unsigned phase = 1; phase <= 3; ++phase) {
        intent.phase = static_cast<UnboundCourseMigrationPhase>(phase);
        const auto result = store.persist(intent);
        if (result != UnboundCourseIntentResult::Ok) {
          EXPECT_EQ(result, UnboundCourseIntentResult::IoError);
          interrupted = true;
          break;
        }
      }
      EXPECT_TRUE(interrupted);
      f.storage.fail = 0;
      UnboundCourseMigrationIntentStore restored(f.storage, f.scratch, Fixture::permitted, &f.storage);
      const auto resumePhase = static_cast<unsigned>(intent.phase);
      for (unsigned phase = resumePhase; phase <= 3; ++phase) {
        intent.phase = static_cast<UnboundCourseMigrationPhase>(phase);
        ASSERT_EQ(restored.persist(intent), UnboundCourseIntentResult::Ok);
      }
      const auto files = f.storage.files;
      const auto mutations = f.storage.mutations;
      UnboundCourseMigrationIntent output;
      ASSERT_EQ(restored.load(output), UnboundCourseIntentResult::Ok);
      EXPECT_EQ(output, intent);
      for (unsigned phase = 1; phase <= 3; ++phase) {
        intent.phase = static_cast<UnboundCourseMigrationPhase>(phase);
        ASSERT_EQ(restored.persist(intent), UnboundCourseIntentResult::Ok);
      }
      EXPECT_EQ(f.storage.mutations, mutations);
      EXPECT_EQ(f.storage.files, files);
      EXPECT_EQ(f.storage.files.at(ACTIVE_COURSE_PATH), std::vector<uint8_t>({9}));
    }
  }
}

TEST(UnboundCourseMigrationIntentStore, PhaseGapsAndForeignOrTornEvidenceCannotAuthorizePublication) {
  for (unsigned fault = 0; fault < 7; ++fault) {
    Fixture f;
    UnboundCourseMigrationIntent intent;
    intent.reader.fill(7);
    intent.request.original = f.request;
    intent.activePack = f.request.manifest;
    UnboundCourseMigrationIntentStore store(f.storage, f.scratch, Fixture::permitted, &f.storage);
    std::array<uint8_t, UNBOUND_COURSE_MIGRATION_INTENT_SIZE> bytes{};
    if (fault == 0) intent.phase = UnboundCourseMigrationPhase::Bound;
    if (fault == 1) {
      auto orphan = intent;
      orphan.phase = UnboundCourseMigrationPhase::Isolated;
      ASSERT_TRUE(encodeUnboundCourseMigrationIntent(orphan, bytes));
      f.storage.files[UnboundCourseMigrationIntentStore::PATHS[2]] = {bytes.begin(), bytes.end()};
    }
    if (fault >= 2) {
      auto foreign = intent;
      if (fault == 2) foreign.reader[0] ^= 1;
      if (fault == 3) foreign.request.original.owner[0] ^= 1;
      if (fault == 4) foreign.activePack.contentHash[0] ^= 1;
      ASSERT_TRUE(encodeUnboundCourseMigrationIntent(foreign, bytes));
      f.storage.files[UnboundCourseMigrationIntentStore::STAGES[0]] = {bytes.begin(), bytes.end()};
      if (fault == 5) f.storage.files.at(UnboundCourseMigrationIntentStore::STAGES[0]).resize(99);
      if (fault == 6) {
        f.storage.files[UnboundCourseMigrationIntentStore::PATHS[0]] = {bytes.begin(), bytes.end()};
      }
    }
    const auto files = f.storage.files;
    EXPECT_NE(store.persist(intent), UnboundCourseIntentResult::Ok);
    EXPECT_EQ(f.storage.files, files);
    EXPECT_EQ(f.storage.mutations, 0u);
    auto output = intent;
    EXPECT_NE(store.load(output), UnboundCourseIntentResult::Ok);
    EXPECT_EQ(output, intent);
  }
}

TEST(UnboundCourseMigrationIntentStore, PermissionLossAndReadErrorsPreserveRecordsWithoutFurtherMutation) {
  for (unsigned fault = 0; fault < 4; ++fault) {
    Fixture f;
    UnboundCourseMigrationIntent intent;
    intent.reader.fill(7);
    intent.request.original = f.request;
    intent.activePack = f.request.manifest;
    UnboundCourseMigrationIntentStore store(f.storage, f.scratch, Fixture::permitted, &f.storage);
    ASSERT_EQ(store.persist(intent), UnboundCourseIntentResult::Ok);
    intent.phase = UnboundCourseMigrationPhase::Bound;
    if (fault == 0) f.storage.allowed = false;
    if (fault == 1) f.storage.readError = true;
    if (fault == 2) f.storage.statError = true;
    if (fault == 3) f.storage.revokeRead = true;
    const auto files = f.storage.files;
    const auto mutations = f.storage.mutations;
    EXPECT_NE(store.persist(intent), UnboundCourseIntentResult::Ok);
    auto output = intent;
    EXPECT_NE(store.load(output), UnboundCourseIntentResult::Ok);
    EXPECT_EQ(output, intent);
    EXPECT_EQ(f.storage.files, files);
    EXPECT_EQ(f.storage.mutations, mutations);
  }
}

TEST(UnboundCourseMigrationIntent, RecoveryRetainsOriginalAndUpdatedActivePackIdentities) {
  const std::string path = COURSE_BASELINE_IMPORT_FIXTURE;
  std::ifstream input(path.substr(0, path.find_last_of('/') + 1) + "UnboundCourseMigrationIntent-v1.fixture",
                      std::ios::binary);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  UnboundCourseMigrationIntent intent;
  ASSERT_TRUE(decodeUnboundCourseMigrationIntent(bytes, intent));
  EXPECT_NE(intent.activePack.contentHash, intent.request.original.manifest.contentHash);
  EXPECT_EQ(intent.activePack.logicalIdentity, intent.request.original.manifest.logicalIdentity);
  std::array<uint8_t, UNBOUND_COURSE_MIGRATION_INTENT_SIZE> encoded{};
  ASSERT_TRUE(encodeUnboundCourseMigrationIntent(intent, encoded));
  EXPECT_TRUE(std::equal(encoded.begin(), encoded.end(), bytes.begin(), bytes.end()));
  for (const auto phase : {UnboundCourseMigrationPhase::Prepared, UnboundCourseMigrationPhase::Bound,
                           UnboundCourseMigrationPhase::Isolated}) {
    intent.phase = phase;
    ASSERT_TRUE(encodeUnboundCourseMigrationIntent(intent, encoded));
    UnboundCourseMigrationIntent restored;
    ASSERT_TRUE(decodeUnboundCourseMigrationIntent(encoded, restored));
    EXPECT_EQ(restored, intent);
  }
  const auto unchanged = intent;
  for (size_t size = 0; size < bytes.size(); ++size) {
    EXPECT_FALSE(decodeUnboundCourseMigrationIntent(std::span(bytes).first(size), intent));
    EXPECT_EQ(intent, unchanged);
  }
  for (size_t index = 0; index < bytes.size(); ++index) {
    auto corrupt = bytes;
    corrupt[index] ^= 1;
    EXPECT_FALSE(decodeUnboundCourseMigrationIntent(corrupt, intent));
    EXPECT_EQ(intent, unchanged);
  }
  // Correct envelope CRC cannot authorize a foreign course, reader or unknown phase.
  for (unsigned fault = 0; fault < 6; ++fault) {
    auto invalid = bytes;
    if (fault == 0) invalid[226] ^= 1;
    if (fault == 1) std::fill(invalid.begin() + 8, invalid.begin() + 24, 0);
    if (fault == 2) invalid[5] = 4;
    if (fault == 3) invalid[6] = 1;
    if (fault == 4) std::fill(invalid.begin() + 181, invalid.begin() + 213, 0);
    if (fault == 5) invalid[222] = 2;
    course_review_detail::number(invalid, 242, binary_record::crc32(invalid.data(), 242), 4);
    EXPECT_FALSE(decodeUnboundCourseMigrationIntent(invalid, intent));
    EXPECT_EQ(intent, unchanged);
  }
  auto foreign = unchanged;
  foreign.activePack.logicalIdentity[0] ^= 1;
  encoded.fill(99);
  EXPECT_FALSE(encodeUnboundCourseMigrationIntent(foreign, encoded));
  EXPECT_TRUE(std::all_of(encoded.begin(), encoded.end(), [](uint8_t byte) { return byte == 99; }));
}

TEST(UnboundCourseMigrationRequest, SharedConsentBindsReviewAndTransferWithoutBecomingArchiveConsent) {
  const std::string path = COURSE_BASELINE_IMPORT_FIXTURE;
  const auto directory = path.substr(0, path.find_last_of('/') + 1);
  std::ifstream input(directory + "UnboundCourseMigrationRequest-v1.fixture", std::ios::binary);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  UnboundCourseMigrationRequest request;
  ASSERT_TRUE(decodeUnboundCourseMigrationRequest(bytes, request));
  std::array<uint8_t, UNBOUND_COURSE_MIGRATION_REQUEST_SIZE> encoded{};
  ASSERT_TRUE(encodeUnboundCourseMigrationRequest(request, encoded));
  EXPECT_TRUE(std::equal(encoded.begin(), encoded.end(), bytes.begin()));
  CourseBaselineImportRequest archive;
  EXPECT_FALSE(decodeCourseBaselineImportRequest(bytes, archive));
  std::ifstream roster(directory + "CourseBaselineReview-unbound-v2.fixture", std::ios::binary);
  const std::vector<uint8_t> review{std::istreambuf_iterator<char>(roster), std::istreambuf_iterator<char>()};
  Identity reader{};
  reader.fill(1);
  TransferDeclaration transfer;
  transfer.manifest = request.original.manifest;
  transfer.state.transaction = request.original.transaction;
  transfer.state.owner = request.original.owner;
  transfer.state.storageGeneration = request.original.generation;
  transfer.state.contentHash = request.original.manifest.contentHash;
  transfer.state.length = request.original.manifest.length;
  ASSERT_TRUE(matchesUnboundCourseMigrationRequest(request, review, request.original.reviewHash, reader,
                                                   request.original.generation, request.original.owner, transfer));
  auto foreign = reader;
  foreign[0] ^= 1;
  EXPECT_FALSE(matchesUnboundCourseMigrationRequest(request, review, request.original.reviewHash, foreign,
                                                    request.original.generation, request.original.owner, transfer));
  auto changed = transfer;
  changed.state.transaction[0] ^= 1;
  EXPECT_FALSE(matchesUnboundCourseMigrationRequest(request, review, request.original.reviewHash, reader,
                                                    request.original.generation, request.original.owner, changed));
  std::ifstream isolated(directory + "CourseBaselineReview-v1.fixture", std::ios::binary);
  const std::vector<uint8_t> wrongScope{std::istreambuf_iterator<char>(isolated), std::istreambuf_iterator<char>()};
  EXPECT_FALSE(matchesUnboundCourseMigrationRequest(request, wrongScope, request.original.reviewHash, reader,
                                                    request.original.generation, request.original.owner, transfer));
  for (size_t size = 0; size < bytes.size(); ++size)
    EXPECT_FALSE(decodeUnboundCourseMigrationRequest(std::span(bytes).first(size), request));
  for (size_t index = 0; index < bytes.size(); ++index) {
    auto corrupt = bytes;
    corrupt[index] ^= 1;
    EXPECT_FALSE(decodeUnboundCourseMigrationRequest(corrupt, request));
  }
}

TEST(CompanionCourseBaselineReview, SharedUnboundFixtureRequiresExplicitReadOnlyDecode) {
  const std::string reviewPath = COURSE_BASELINE_REVIEW_FIXTURE;
  std::ifstream input(
      reviewPath.substr(0, reviewPath.find_last_of('/') + 1) + "CourseBaselineReview-unbound-v2.fixture",
      std::ios::binary);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  ASSERT_FALSE(bytes.empty());
  CourseBaselineReviewView view;
  EXPECT_FALSE(view.decode(bytes));
  ASSERT_TRUE(view.decode(bytes, true));
  EXPECT_FALSE(view.isolated());
  EXPECT_EQ(view.count(), 8u);
  for (size_t at = 0; at < view.count(); ++at) {
    const auto entry = view.entry(at);
    if (entry[0] == uint8_t(CourseBaselineReviewDomain::Isolation)) {
      EXPECT_EQ(entry[1], 0);
      EXPECT_EQ(course_review_detail::number(entry, 28, 8), 0u);
      EXPECT_FALSE(course_review_detail::nonzero(entry.subspan(36, 32)));
    }
  }
  for (size_t size = 0; size < bytes.size(); ++size) EXPECT_FALSE(view.decode(std::span(bytes).first(size), true));
  for (size_t at = 0; at < bytes.size(); ++at) {
    auto invalid = bytes;
    invalid[at] ^= 1;
    EXPECT_FALSE(view.decode(invalid, true));
  }
  for (size_t at : {4u, 6u, 7u}) {
    auto invalid = bytes;
    invalid[at] = at == 4 ? 1 : at == 6 ? 0 : 1;
    course_review_detail::number(invalid, invalid.size() - 4, binary_record::crc32(invalid.data(), invalid.size() - 4),
                                 4);
    EXPECT_FALSE(view.decode(invalid, true));
  }
}

TEST(CompanionCourseBaselineReview, SharedFixtureRefusesMalformedNoncanonicalAndOverlimitRecords) {
  std::array<uint8_t, 64 + 8 * COURSE_BASELINE_REVIEW_ENTRY_SIZE> bytes{};
  std::ifstream input(COURSE_BASELINE_REVIEW_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()));
  ASSERT_EQ(input.peek(), std::char_traits<char>::eof());
  CourseBaselineReviewView view;
  ASSERT_TRUE(view.decode(bytes));
  EXPECT_EQ(view.count(), 8u);
  EXPECT_EQ(view.reader()[0], 1);
  EXPECT_EQ(view.generation()[0], 2);
  EXPECT_EQ(view.course()[0], 3);
  EXPECT_TRUE(view.entry(8).empty());
  for (size_t at = 0; at < bytes.size(); ++at) {
    auto corrupt = bytes;
    corrupt[at] ^= 1;
    EXPECT_FALSE(view.decode(corrupt));
    EXPECT_EQ(view.count(), 0u);
    EXPECT_TRUE(view.reader().empty());
  }
  for (size_t at : {5u, 6u, 7u, 58u, 59u, 60u, 61u, 62u, 63u, 64u}) {
    SCOPED_TRACE(at);
    auto invalid = bytes;
    invalid[at] = at == 62 ? 24 : 1;
    if (at == 60) invalid[at] = 4;
    if (at == 61) invalid[at] = 0;
    if (at == 64) invalid[at] = 'I';
    seal(invalid);
    EXPECT_FALSE(view.decode(invalid));
  }
  auto invalid = bytes;
  static constexpr char PENDING[] = "items.tmp";
  std::copy_n(PENDING, sizeof(PENDING) - 1, invalid.begin() + 64);
  seal(invalid);
  EXPECT_FALSE(view.decode(invalid));
  invalid = bytes;
  std::swap_ranges(invalid.begin() + 128, invalid.begin() + 196, invalid.begin() + 196);
  seal(invalid);
  EXPECT_FALSE(view.decode(invalid));
  invalid = bytes;
  invalid[56] = COURSE_BASELINE_REVIEW_MAX_FILES + 1;
  seal(invalid);
  EXPECT_FALSE(view.decode(invalid));
  EXPECT_FALSE(view.decode(std::span(bytes).first(bytes.size() - 1)));
  EXPECT_TRUE(view.decode(bytes));
}

TEST(CompanionCourseBaselineReview, PageRequestsBindCaptureAndImmutableResume) {
  const std::string reviewPath = COURSE_BASELINE_REVIEW_FIXTURE;
  std::ifstream input(
      reviewPath.substr(0, reviewPath.find_last_of('/') + 1) + "CourseBaselineReviewPageRequest-v1.fixture",
      std::ios::binary);
  const std::vector<uint8_t> fixture((std::istreambuf_iterator<char>(input)), {});
  CourseBaselineReviewPageRequest request;
  ASSERT_TRUE(decodeCourseBaselineReviewPageRequest(fixture, request));
  EXPECT_EQ(request.offset, 0u);
  EXPECT_EQ(request.limit, 97u);
  EXPECT_EQ(request.hash, Digest{});
  std::array<uint8_t, COURSE_BASELINE_REVIEW_PAGE_REQUEST_SIZE> bytes{};
  ASSERT_TRUE(encodeCourseBaselineReviewPageRequest(request, bytes));
  ASSERT_TRUE(std::equal(bytes.begin(), bytes.end(), fixture.begin()));
  for (size_t size = 0; size < bytes.size(); ++size) {
    auto output = request;
    EXPECT_FALSE(decodeCourseBaselineReviewPageRequest(std::span(bytes).first(size), output));
    EXPECT_EQ(output, request);
  }
  for (unsigned fault = 0; fault < 7; ++fault) {
    auto changed = bytes;
    if (fault == 0) changed[5] = 1;
    if (fault == 1) std::fill_n(changed.begin() + 6, 16, 0);
    if (fault == 2) std::fill_n(changed.begin() + 22, 16, 0);
    if (fault == 3) course_review_detail::number(changed, 70, 1, 2);
    if (fault == 4) course_review_detail::number(changed, 70, COURSE_BASELINE_REVIEW_MAX_SIZE, 2);
    if (fault == 5) course_review_detail::number(changed, 72, 0, 2);
    if (fault == 6) course_review_detail::number(changed, 72, COURSE_BASELINE_REVIEW_PAGE_MAX_BYTES + 1, 2);
    auto output = request;
    EXPECT_FALSE(decodeCourseBaselineReviewPageRequest(changed, output));
    EXPECT_EQ(output, request);
  }
  request.hash.fill(9);
  request.offset = 97;
  request.limit = COURSE_BASELINE_REVIEW_PAGE_MAX_BYTES;
  ASSERT_TRUE(encodeCourseBaselineReviewPageRequest(request, bytes));
  CourseBaselineReviewPageRequest resumed;
  ASSERT_TRUE(decodeCourseBaselineReviewPageRequest(bytes, resumed));
  EXPECT_EQ(resumed, request);
  std::ifstream framedInput(
      reviewPath.substr(0, reviewPath.find_last_of('/') + 1) + "CourseBaselineReviewRequestFrame-v1.fixture",
      std::ios::binary);
  const std::vector<uint8_t> framed((std::istreambuf_iterator<char>(framedInput)), {});
  FrameView frame;
  EXPECT_EQ(decodeFrame(framed, false, frame), FrameError::Unauthorized);
  ASSERT_EQ(decodeFrame(framed, true, frame), FrameError::None);
  EXPECT_EQ(frame.command, Command::CourseBaselineReview);
  EXPECT_EQ(frame.requestId, 0x12345678u);
  EXPECT_TRUE(std::equal(frame.payload.begin(), frame.payload.end(), fixture.begin(), fixture.end()));
}

TEST(CompanionCourseBaselineReview, BoundedPagesRetainOneWholeReviewBinding) {
  std::ifstream input(COURSE_BASELINE_REVIEW_FIXTURE, std::ios::binary);
  const std::vector<uint8_t> review((std::istreambuf_iterator<char>(input)), {});
  CourseBaselineReviewView original;
  ASSERT_TRUE(original.decode(review));
  Digest verifiedHash{};
  verifiedHash.fill(9);
  std::array<uint8_t, MAX_CONTROL_PAYLOAD> page{};
  for (const size_t limit : {size_t(1), size_t(97), COURSE_BASELINE_REVIEW_PAGE_MAX_BYTES}) {
    std::vector<uint8_t> assembled(review.size());
    size_t offset = 0;
    while (offset < review.size()) {
      const auto length = encodeCourseBaselineReviewPage(review, verifiedHash, offset, limit, page);
      ASSERT_GT(length, COURSE_BASELINE_REVIEW_PAGE_OVERHEAD);
      ASSERT_LE(length, MAX_CONTROL_PAYLOAD);
      CourseBaselineReviewPageView decoded;
      ASSERT_TRUE(decodeCourseBaselineReviewPage(std::span(page).first(length), decoded));
      EXPECT_EQ(decoded.total, review.size());
      EXPECT_EQ(decoded.offset, offset);
      EXPECT_EQ(decoded.hash, verifiedHash);
      std::copy(decoded.bytes.begin(), decoded.bytes.end(), assembled.begin() + offset);
      offset += decoded.bytes.size();
    }
    EXPECT_EQ(assembled, review);
  }
  const auto length = encodeCourseBaselineReviewPage(review, verifiedHash, 0, 97, page);
  ASSERT_NE(length, 0u);
  CourseBaselineReviewPageView saved;
  ASSERT_TRUE(decodeCourseBaselineReviewPage(std::span(page).first(length), saved));
  for (size_t at = 0; at < length; ++at) {
    auto changed = page;
    changed[at] ^= 1;
    auto output = saved;
    EXPECT_FALSE(decodeCourseBaselineReviewPage(std::span(changed).first(length), output));
    EXPECT_EQ(output.bytes.data(), saved.bytes.data());
    EXPECT_EQ(output.hash, saved.hash);
    EXPECT_FALSE(decodeCourseBaselineReviewPage(std::span(page).first(at), output));
  }
  EXPECT_EQ(encodeCourseBaselineReviewPage(review, verifiedHash, review.size(), 1, page), 0u);
  EXPECT_EQ(encodeCourseBaselineReviewPage(review, verifiedHash, 0, 0, page), 0u);
  EXPECT_EQ(encodeCourseBaselineReviewPage(review, verifiedHash, 0, COURSE_BASELINE_REVIEW_PAGE_MAX_BYTES + 1, page),
            0u);
  EXPECT_EQ(encodeCourseBaselineReviewPage(review, {}, 0, 1, page), 0u);
  EXPECT_EQ(encodeCourseBaselineReviewPage(review, verifiedHash, 0, 97, std::span(page).first(length - 1)), 0u);
  auto overlapping = review;
  EXPECT_EQ(encodeCourseBaselineReviewPage(overlapping, verifiedHash, 0, 1, overlapping), 0u);
  for (unsigned fault = 0; fault < 5; ++fault) {
    auto changed = page;
    auto bytes = std::span(changed).first(length);
    if (fault == 0) course_review_detail::number(bytes, 6, review.size() + 1, 2);
    if (fault == 1) course_review_detail::number(bytes, 8, review.size(), 2);
    if (fault == 2) course_review_detail::number(bytes, 10, 0, 2);
    if (fault == 3) course_review_detail::number(bytes, 10, 98, 2);
    if (fault == 4) std::fill_n(bytes.begin() + 12, 32, 0);
    seal(bytes);
    auto output = saved;
    EXPECT_FALSE(decodeCourseBaselineReviewPage(bytes, output));
    EXPECT_EQ(output.bytes.data(), saved.bytes.data());
    EXPECT_EQ(output.hash, saved.hash);
  }
  const std::string reviewPath = COURSE_BASELINE_REVIEW_FIXTURE;
  const auto pagePath = reviewPath.substr(0, reviewPath.find_last_of('/') + 1) + "CourseBaselineReviewPage-v1.fixture";
  std::ifstream sharedInput(pagePath, std::ios::binary);
  const std::vector<uint8_t> shared((std::istreambuf_iterator<char>(sharedInput)), {});
  CourseBaselineReviewPageView native;
  ASSERT_TRUE(decodeCourseBaselineReviewPage(shared, native));
  EXPECT_EQ(native.total, review.size());
  EXPECT_EQ(native.offset, 0u);
  ASSERT_EQ(native.bytes.size(), 97u);
  ASSERT_EQ(encodeCourseBaselineReviewPage(review, native.hash, 0, 97, page), shared.size());
  EXPECT_TRUE(std::equal(shared.begin(), shared.end(), page.begin()));
}

TEST(CompanionCourseBaselinePublication, SharedFixtureAndBothPhasesRoundTripWithCorruptionRefusal) {
  Fixture f;
  std::array<uint8_t, COURSE_BASELINE_PUBLICATION_SIZE> bytes{}, encoded{};
  std::ifstream input(COURSE_BASELINE_PUBLICATION_FIXTURE, std::ios::binary);
  ASSERT_TRUE(input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()));
  ASSERT_EQ(input.peek(), std::char_traits<char>::eof());
  CourseBaselinePublicationRecord expected;
  expected.reader.fill(7);
  expected.request = f.request;
  CourseBaselinePublicationRecord decoded;
  ASSERT_TRUE(decodeCourseBaselinePublicationRecord(bytes, decoded));
  EXPECT_EQ(decoded, expected);
  ASSERT_TRUE(encodeCourseBaselinePublicationRecord(expected, encoded));
  EXPECT_EQ(encoded, bytes);
  for (const auto phase : {CourseBaselinePublicationPhase::Prepared, CourseBaselinePublicationPhase::Published}) {
    expected.phase = phase;
    ASSERT_TRUE(encodeCourseBaselinePublicationRecord(expected, encoded));
    ASSERT_TRUE(decodeCourseBaselinePublicationRecord(encoded, decoded));
    EXPECT_EQ(decoded, expected);
    for (size_t at = 0; at < encoded.size(); ++at) {
      auto corrupt = encoded;
      corrupt[at] ^= 1;
      const auto previous = decoded;
      EXPECT_FALSE(decodeCourseBaselinePublicationRecord(corrupt, decoded));
      EXPECT_EQ(decoded, previous);
    }
  }
}

TEST(CompanionCourseBaselinePublication, ResignedMalformedRecordsAndAliasedBuffersPreserveOutput) {
  Fixture f;
  CourseBaselinePublicationRecord record;
  record.reader.fill(7);
  record.request = f.request;
  std::array<uint8_t, COURSE_BASELINE_PUBLICATION_SIZE> encoded{};
  ASSERT_TRUE(encodeCourseBaselinePublicationRecord(record, encoded));
  auto output = record;
  output.reader.fill(8);
  const auto previous = output;
  for (unsigned fault = 0; fault < 8; ++fault) {
    auto invalid = encoded;
    if (fault == 0) invalid[5] = 0;
    if (fault == 1) invalid[5] = 3;
    if (fault == 2) invalid[6] = 1;
    if (fault == 3) invalid[7] = 1;
    if (fault == 4) std::fill_n(invalid.begin() + 8, 16, 0);
    if (fault == 5) {
      invalid[29] = 0;
      seal(std::span(invalid).subspan(24, COURSE_BASELINE_IMPORT_REQUEST_SIZE));
    }
    if (fault == 6) {
      std::fill_n(invalid.begin() + 32, 16, 0);
      seal(std::span(invalid).subspan(24, COURSE_BASELINE_IMPORT_REQUEST_SIZE));
    }
    if (fault == 7) invalid[24 + 151] ^= 1;
    seal(invalid);
    EXPECT_FALSE(decodeCourseBaselinePublicationRecord(invalid, output));
    EXPECT_EQ(output, previous);
  }
  EXPECT_FALSE(decodeCourseBaselinePublicationRecord(std::span(encoded).first(encoded.size() - 1), output));
  EXPECT_EQ(output, previous);
  struct Aliased {
    CourseBaselinePublicationRecord record;
    std::array<uint8_t, 256> tail{};
  } aliased{record};
  const auto bytes =
      std::span(reinterpret_cast<uint8_t*>(&aliased), sizeof(aliased)).first(COURSE_BASELINE_PUBLICATION_SIZE);
  EXPECT_FALSE(encodeCourseBaselinePublicationRecord(aliased.record, bytes));
  EXPECT_EQ(aliased.record, record);
  EXPECT_FALSE(decodeCourseBaselinePublicationRecord(bytes, aliased.record));
  EXPECT_EQ(aliased.record, record);
  encoded.fill(0xa5);
  const auto untouched = encoded;
  record.reader = {};
  EXPECT_FALSE(encodeCourseBaselinePublicationRecord(record, encoded));
  EXPECT_EQ(encoded, untouched);
}

namespace {
struct PublicationFixture {
  Fixture base;
  CourseBaselinePublicationRecord record;
  bool fresh = true, archive = false, copiesValid = true, failArchive = false, applyArchiveBeforeFailure = false;
  unsigned preparationCalls = 0, publicationCalls = 0, verificationCalls = 0;
  unsigned rejectVerification = 0;
  bool recoveredPreparation = false;
  PublicationFixture() {
    base.storage.allowStageRemoval = true;
    record.reader.fill(7);
    record.request = base.request;
  }
  std::string prepared() const {
    auto path = base.canonical();
    path.replace(path.size() - 8, 8, ".prepared");
    return path;
  }
  std::string published() const {
    auto path = base.canonical();
    path.replace(path.size() - 8, 8, ".published");
    return path;
  }
  CourseBaselinePublicationHooks hooks() {
    return {this,
            [](void* context, const CourseBaselinePublicationRecord& record, bool recovering) {
              auto& f = *static_cast<PublicationFixture*>(context);
              ++f.preparationCalls;
              f.recoveredPreparation = recovering;
              EXPECT_EQ(record, f.record);
              return f.fresh && f.copiesValid && (!f.archive || recovering);
            },
            [](void* context, const CourseBaselinePublicationRecord& record) {
              auto& f = *static_cast<PublicationFixture*>(context);
              ++f.publicationCalls;
              EXPECT_EQ(record, f.record);
              EXPECT_TRUE(f.base.storage.files.contains(f.prepared()));
              EXPECT_FALSE(f.base.storage.files.contains(f.prepared() + ".tmp"));
              if (f.failArchive && !f.applyArchiveBeforeFailure) return false;
              f.archive = true;
              return !f.failArchive;
            },
            [](void* context, const CourseBaselinePublicationRecord& record) {
              auto& f = *static_cast<PublicationFixture*>(context);
              ++f.verificationCalls;
              EXPECT_EQ(record, f.record);
              return f.archive && f.copiesValid && f.verificationCalls != f.rejectVerification;
            }};
  }
  CourseBaselinePublicationStore store() {
    return CourseBaselinePublicationStore(base.storage, base.scratch, Fixture::permitted, &base.storage);
  }
};
}  // namespace

TEST(CompanionCourseBaselinePublicationStore, OrdersIntentArchiveAndCompletionThenReplaysWithoutFreshApproval) {
  PublicationFixture f;
  const auto original = f.base.storage.files;
  auto store = f.store();
  ASSERT_EQ(store.publish(f.record, f.hooks()), CourseBaselinePublicationResult::Ok);
  ASSERT_NE(store.published(), nullptr);
  EXPECT_EQ(store.published()->phase, CourseBaselinePublicationPhase::Published);
  EXPECT_EQ(store.published()->request, f.record.request);
  EXPECT_EQ(f.preparationCalls, 1u);
  EXPECT_EQ(f.publicationCalls, 1u);
  EXPECT_EQ(f.verificationCalls, 2u);
  CourseBaselinePublicationRecord decoded;
  ASSERT_TRUE(decodeCourseBaselinePublicationRecord(f.base.storage.files.at(f.prepared()), decoded));
  EXPECT_EQ(decoded, f.record);
  ASSERT_TRUE(decodeCourseBaselinePublicationRecord(f.base.storage.files.at(f.published()), decoded));
  EXPECT_EQ(decoded.phase, CourseBaselinePublicationPhase::Published);
  for (const auto& [path, data] : original) EXPECT_EQ(f.base.storage.files.at(path), data);
  f.base.storage.files["/tinta/courses/04040404040404040404040404040404/items.bin"] = {31};
  f.fresh = false;
  const auto files = f.base.storage.files;
  const auto mutations = f.base.storage.mutations;
  auto restarted = f.store();
  ASSERT_EQ(restarted.publish(f.record, f.hooks()), CourseBaselinePublicationResult::Ok);
  EXPECT_EQ(f.preparationCalls, 1u);
  EXPECT_EQ(f.publicationCalls, 1u);
  EXPECT_EQ(f.base.storage.files, files);
  EXPECT_EQ(f.base.storage.mutations, mutations);
  f.copiesValid = false;
  EXPECT_EQ(restarted.publish(f.record, f.hooks()), CourseBaselinePublicationResult::VerificationFailed);
  EXPECT_EQ(restarted.published(), nullptr);
  EXPECT_EQ(f.base.storage.files, files);
}

TEST(CompanionCourseBaselinePublicationStore, RecoversEveryRecordWriteAndRenameBoundary) {
  for (unsigned mutation = 1; mutation <= 4; ++mutation) {
    for (const bool after : {false, true}) {
      SCOPED_TRACE(mutation);
      SCOPED_TRACE(after);
      PublicationFixture f;
      f.base.storage.fail = mutation;
      f.base.storage.after = after;
      auto store = f.store();
      EXPECT_EQ(store.publish(f.record, f.hooks()), CourseBaselinePublicationResult::IoError);
      EXPECT_EQ(store.published(), nullptr);
      f.base.storage.fail = 0;
      // Complete Published bytes are historical evidence even before the rename.
      if (f.base.storage.files.contains(f.published()) || f.base.storage.files.contains(f.published() + ".tmp"))
        f.fresh = false;
      auto restored = f.store();
      ASSERT_EQ(restored.publish(f.record, f.hooks()), CourseBaselinePublicationResult::Ok);
      EXPECT_TRUE(f.base.storage.files.contains(f.prepared()));
      EXPECT_TRUE(f.base.storage.files.contains(f.published()));
      EXPECT_FALSE(f.base.storage.files.contains(f.prepared() + ".tmp"));
      EXPECT_FALSE(f.base.storage.files.contains(f.published() + ".tmp"));
    }
  }
}

TEST(CompanionCourseBaselinePublicationStore, ResumesAppliedArchiveBeforeCompletionAndRefusesUnverifiedPublication) {
  for (const bool applied : {false, true}) {
    PublicationFixture f;
    f.failArchive = true;
    f.applyArchiveBeforeFailure = applied;
    auto store = f.store();
    EXPECT_EQ(store.publish(f.record, f.hooks()), CourseBaselinePublicationResult::IoError);
    EXPECT_EQ(store.published(), nullptr);
    EXPECT_TRUE(f.base.storage.files.contains(f.prepared()));
    EXPECT_FALSE(f.base.storage.files.contains(f.published()));
    f.failArchive = false;
    auto restored = f.store();
    ASSERT_EQ(restored.publish(f.record, f.hooks()), CourseBaselinePublicationResult::Ok);
    EXPECT_TRUE(f.recoveredPreparation);
    EXPECT_TRUE(f.archive);
  }
  PublicationFixture f;
  f.fresh = false;
  const auto files = f.base.storage.files;
  auto store = f.store();
  EXPECT_EQ(store.publish(f.record, f.hooks()), CourseBaselinePublicationResult::VerificationFailed);
  EXPECT_EQ(f.base.storage.files, files);
  EXPECT_EQ(f.publicationCalls, 0u);
}

TEST(CompanionCourseBaselinePublicationStore, RefusesOrphanForeignTornAndDuplicatePhaseEvidence) {
  for (unsigned fault = 0; fault < 7; ++fault) {
    PublicationFixture f;
    std::array<uint8_t, COURSE_BASELINE_PUBLICATION_SIZE> bytes{};
    auto record = f.record;
    if (fault == 0) {
      record.phase = CourseBaselinePublicationPhase::Published;
      ASSERT_TRUE(encodeCourseBaselinePublicationRecord(record, bytes));
      f.base.storage.files[f.published()] = {bytes.begin(), bytes.end()};
    } else if (fault == 1) {
      f.base.storage.files[f.prepared() + ".tmp"] = {1};
    } else if (fault == 2) {
      f.base.storage.files[f.published() + ".tmp"] = {1};
    } else if (fault == 3) {
      record.request.reviewHash[0] ^= 1;
      ASSERT_TRUE(encodeCourseBaselinePublicationRecord(record, bytes));
      f.base.storage.files[f.prepared()] = {bytes.begin(), bytes.end()};
    } else if (fault == 4) {
      record.phase = CourseBaselinePublicationPhase::Published;
      ASSERT_TRUE(encodeCourseBaselinePublicationRecord(record, bytes));
      f.base.storage.files[f.prepared()] = {bytes.begin(), bytes.end()};
    } else if (fault == 5) {
      ASSERT_TRUE(encodeCourseBaselinePublicationRecord(record, bytes));
      f.base.storage.files[f.prepared()] = {bytes.begin(), bytes.end()};
      f.base.storage.files[f.prepared() + ".tmp"] = {bytes.begin(), bytes.end()};
    } else {
      ASSERT_TRUE(encodeCourseBaselinePublicationRecord(record, bytes));
      f.base.storage.files[f.prepared() + ".tmp"] = {bytes.begin(), bytes.end()};
      record.phase = CourseBaselinePublicationPhase::Published;
      ASSERT_TRUE(encodeCourseBaselinePublicationRecord(record, bytes));
      f.base.storage.files[f.published() + ".tmp"] = {bytes.begin(), bytes.end()};
    }
    const auto files = f.base.storage.files;
    auto store = f.store();
    EXPECT_NE(store.publish(f.record, f.hooks()), CourseBaselinePublicationResult::Ok);
    EXPECT_EQ(store.published(), nullptr);
    EXPECT_EQ(f.base.storage.files, files);
    EXPECT_EQ(f.preparationCalls, 0u);
    EXPECT_EQ(f.publicationCalls, 0u);
  }
}

TEST(CompanionCourseBaselinePublicationStore, EveryMatchingTornStageRecoversOnlyAfterNativeVerification) {
  for (const auto phase : {CourseBaselinePublicationPhase::Prepared, CourseBaselinePublicationPhase::Published}) {
    for (size_t length = 0; length < COURSE_BASELINE_PUBLICATION_SIZE; ++length) {
      for (const bool verified : {false, true}) {
        SCOPED_TRACE(::testing::Message() << unsigned(phase) << " length=" << length << " verified=" << verified);
        PublicationFixture f;
        std::array<uint8_t, COURSE_BASELINE_PUBLICATION_SIZE> bytes{};
        auto record = f.record;
        if (phase == CourseBaselinePublicationPhase::Published) {
          ASSERT_TRUE(encodeCourseBaselinePublicationRecord(record, bytes));
          f.base.storage.files[f.prepared()] = {bytes.begin(), bytes.end()};
          f.archive = true;
        }
        record.phase = phase;
        ASSERT_TRUE(encodeCourseBaselinePublicationRecord(record, bytes));
        const auto path = (phase == CourseBaselinePublicationPhase::Prepared ? f.prepared() : f.published()) + ".tmp";
        f.base.storage.files[path] = {bytes.begin(), bytes.begin() + length};
        const auto files = f.base.storage.files;
        f.copiesValid = verified;
        auto store = f.store();
        if (verified) {
          ASSERT_EQ(store.publish(f.record, f.hooks()), CourseBaselinePublicationResult::Ok);
          ASSERT_NE(store.published(), nullptr);
          EXPECT_FALSE(f.base.storage.files.contains(path));
          EXPECT_TRUE(f.base.storage.files.contains(f.prepared()));
          EXPECT_TRUE(f.base.storage.files.contains(f.published()));
          EXPECT_EQ(f.recoveredPreparation, phase == CourseBaselinePublicationPhase::Published);
        } else {
          EXPECT_EQ(store.publish(f.record, f.hooks()), CourseBaselinePublicationResult::VerificationFailed);
          EXPECT_EQ(store.published(), nullptr);
          EXPECT_EQ(f.base.storage.files, files);
          EXPECT_EQ(f.base.storage.mutations, 0u);
          EXPECT_EQ(f.publicationCalls, 0u);
        }
      }
    }
  }
}

TEST(CompanionCourseBaselinePublicationStore, MismatchedTornBytesArePreservedBeforeAnyNativeCallback) {
  for (size_t length = 1; length < COURSE_BASELINE_PUBLICATION_SIZE; ++length) {
    PublicationFixture f;
    std::array<uint8_t, COURSE_BASELINE_PUBLICATION_SIZE> bytes{};
    ASSERT_TRUE(encodeCourseBaselinePublicationRecord(f.record, bytes));
    bytes[length - 1] ^= 1;
    f.base.storage.files[f.prepared() + ".tmp"] = {bytes.begin(), bytes.begin() + length};
    const auto files = f.base.storage.files;
    auto store = f.store();
    EXPECT_EQ(store.publish(f.record, f.hooks()), CourseBaselinePublicationResult::Corrupt);
    EXPECT_EQ(f.base.storage.files, files);
    EXPECT_EQ(f.base.storage.mutations, 0u);
    EXPECT_EQ(f.preparationCalls, 0u);
    EXPECT_EQ(f.publicationCalls, 0u);
    EXPECT_EQ(f.verificationCalls, 0u);
  }
}

TEST(CompanionCourseBaselinePublicationStore, TornRemovalPowerCutsCanRetryWithoutLosingCanonicalEvidence) {
  for (const auto phase : {CourseBaselinePublicationPhase::Prepared, CourseBaselinePublicationPhase::Published}) {
    for (const bool after : {false, true}) {
      PublicationFixture f;
      std::array<uint8_t, COURSE_BASELINE_PUBLICATION_SIZE> bytes{};
      auto record = f.record;
      if (phase == CourseBaselinePublicationPhase::Published) {
        ASSERT_TRUE(encodeCourseBaselinePublicationRecord(record, bytes));
        f.base.storage.files[f.prepared()] = {bytes.begin(), bytes.end()};
        f.archive = true;
      }
      record.phase = phase;
      ASSERT_TRUE(encodeCourseBaselinePublicationRecord(record, bytes));
      const auto path = (phase == CourseBaselinePublicationPhase::Prepared ? f.prepared() : f.published()) + ".tmp";
      f.base.storage.files[path] = {bytes.begin(), bytes.begin() + 100};
      f.base.storage.fail = 1;
      f.base.storage.after = after;
      auto store = f.store();
      EXPECT_EQ(store.publish(f.record, f.hooks()), CourseBaselinePublicationResult::IoError);
      EXPECT_EQ(store.published(), nullptr);
      EXPECT_EQ(f.base.storage.files.contains(path), !after);
      if (phase == CourseBaselinePublicationPhase::Published) {
        EXPECT_TRUE(f.base.storage.files.contains(f.prepared()));
      }
      f.base.storage.fail = 0;
      auto restored = f.store();
      ASSERT_EQ(restored.publish(f.record, f.hooks()), CourseBaselinePublicationResult::Ok);
      EXPECT_NE(restored.published(), nullptr);
    }
  }
}

TEST(CompanionCourseBaselinePublicationStore, TornStageIsRecheckedAfterVerificationBeforeAnyRemoval) {
  for (unsigned fault = 0; fault < 7; ++fault) {
    PublicationFixture f;
    std::array<uint8_t, COURSE_BASELINE_PUBLICATION_SIZE> bytes{};
    ASSERT_TRUE(encodeCourseBaselinePublicationRecord(f.record, bytes));
    const auto path = f.prepared() + ".tmp";
    f.base.storage.files[path] = {bytes.begin(), bytes.begin() + 100};
    struct Context {
      PublicationFixture* fixture;
      unsigned fault;
    } context{&f, fault};
    auto hooks = f.hooks();
    hooks.context = &context;
    hooks.verifyPrepared = [](void* raw, const CourseBaselinePublicationRecord& record, bool) {
      auto& c = *static_cast<Context*>(raw);
      auto& fixture = *c.fixture;
      const auto staged = fixture.prepared() + ".tmp";
      if (c.fault == 0) fixture.base.storage.allowed = false;
      if (c.fault == 1) fixture.base.storage.files.at(staged)[99] ^= 1;
      if (c.fault == 2) fixture.base.storage.files.erase(staged);
      if (c.fault == 3) fixture.base.storage.readError = true;
      if (c.fault == 4) fixture.base.storage.statError = true;
      if (c.fault == 5 || c.fault == 6) {
        std::array<uint8_t, COURSE_BASELINE_PUBLICATION_SIZE> full{};
        EXPECT_TRUE(encodeCourseBaselinePublicationRecord(record, full));
        fixture.base.storage.files.at(staged).assign(full.begin(), full.end());
        if (c.fault == 6) fixture.base.storage.files.at(staged).push_back(0);
      }
      return true;
    };
    hooks.publishArchive = [](void*, const CourseBaselinePublicationRecord&) {
      ADD_FAILURE() << "Archive must not be published after changed stage evidence";
      return false;
    };
    hooks.verifyPublished = [](void*, const CourseBaselinePublicationRecord&) {
      ADD_FAILURE() << "Completion must not be verified after changed stage evidence";
      return false;
    };
    auto store = f.store();
    EXPECT_NE(store.publish(f.record, hooks), CourseBaselinePublicationResult::Ok);
    EXPECT_EQ(store.published(), nullptr);
    EXPECT_EQ(f.base.storage.mutations, 0u);
    EXPECT_FALSE(f.base.storage.files.contains(f.prepared()));
    EXPECT_FALSE(f.base.storage.files.contains(f.published()));
  }
}

TEST(CompanionCourseBaselinePublicationStore, EveryTruncatedCanonicalRecordPreservesEvidenceWithoutPublication) {
  for (const auto phase : {CourseBaselinePublicationPhase::Prepared, CourseBaselinePublicationPhase::Published}) {
    for (const bool staged : {false}) {
      for (size_t length = 0; length < COURSE_BASELINE_PUBLICATION_SIZE; ++length) {
        SCOPED_TRACE(::testing::Message()
                     << "phase=" << unsigned(phase) << " staged=" << staged << " length=" << length);
        PublicationFixture f;
        std::array<uint8_t, COURSE_BASELINE_PUBLICATION_SIZE> bytes{};
        auto record = f.record;
        if (phase == CourseBaselinePublicationPhase::Published) {
          ASSERT_TRUE(encodeCourseBaselinePublicationRecord(record, bytes));
          f.base.storage.files[f.prepared()] = {bytes.begin(), bytes.end()};
          f.archive = true;
        }
        record.phase = phase;
        ASSERT_TRUE(encodeCourseBaselinePublicationRecord(record, bytes));
        const auto path =
            (phase == CourseBaselinePublicationPhase::Prepared ? f.prepared() : f.published()) + (staged ? ".tmp" : "");
        f.base.storage.files[path] = {bytes.begin(), bytes.begin() + length};
        const auto files = f.base.storage.files;
        auto store = f.store();
        EXPECT_EQ(store.publish(f.record, f.hooks()), CourseBaselinePublicationResult::Corrupt);
        EXPECT_EQ(store.published(), nullptr);
        EXPECT_EQ(f.base.storage.files, files);
        EXPECT_EQ(f.base.storage.mutations, 0u);
        EXPECT_EQ(f.preparationCalls, 0u);
        EXPECT_EQ(f.publicationCalls, 0u);
        EXPECT_EQ(f.verificationCalls, 0u);
      }
    }
  }
}

TEST(CompanionCourseBaselinePublicationStore, PermissionLossRetainsEvidenceAndRevokesPublishedLoan) {
  PublicationFixture f;
  auto store = f.store();
  f.base.storage.revokeWrite = true;
  EXPECT_EQ(store.publish(f.record, f.hooks()), CourseBaselinePublicationResult::Busy);
  EXPECT_EQ(store.published(), nullptr);
  EXPECT_FALSE(f.archive);
  f.base.storage.revokeWrite = false;
  f.base.storage.allowed = true;
  ASSERT_EQ(store.publish(f.record, f.hooks()), CourseBaselinePublicationResult::Ok);
  ASSERT_NE(store.published(), nullptr);
  f.base.storage.allowed = false;
  EXPECT_EQ(store.published(), nullptr);
  f.base.storage.allowed = true;
  EXPECT_EQ(store.published(), nullptr);
  ASSERT_EQ(store.publish(f.record, f.hooks()), CourseBaselinePublicationResult::Ok);
  store.close();
  EXPECT_EQ(store.published(), nullptr);
}

TEST(CompanionCourseBaselinePublicationStore, CopiesInputsBeforeScratchReuseAndRefusesReentry) {
  PublicationFixture f;
  auto store = f.store();
  struct Context {
    PublicationFixture* fixture;
    CourseBaselinePublicationStore* store;
    CourseBaselinePublicationRecord* caller;
    CourseBaselinePublicationResult reentry = CourseBaselinePublicationResult::Ok;
  } context{&f, &store, &f.record};
  const auto original = f.record;
  auto hooks = f.hooks();
  hooks.context = &context;
  hooks.verifyPrepared = [](void* raw, const CourseBaselinePublicationRecord& expected, bool) {
    auto& ctx = *static_cast<Context*>(raw);
    ctx.reentry = ctx.store->publish(*ctx.caller, ctx.fixture->hooks());
    *ctx.caller = {};
    EXPECT_NE(expected.reader, Identity{});
    return true;
  };
  hooks.publishArchive = [](void* raw, const CourseBaselinePublicationRecord&) {
    static_cast<Context*>(raw)->fixture->archive = true;
    return true;
  };
  hooks.verifyPublished = [](void* raw, const CourseBaselinePublicationRecord&) {
    return static_cast<Context*>(raw)->fixture->archive;
  };
  ASSERT_EQ(store.publish(f.record, hooks), CourseBaselinePublicationResult::Ok);
  EXPECT_EQ(context.reentry, CourseBaselinePublicationResult::Busy);
  ASSERT_NE(store.published(), nullptr);
  EXPECT_EQ(store.published()->request, original.request);
  EXPECT_EQ(store.published()->reader, original.reader);
}

TEST(CompanionCourseBaselinePublicationStore, FinalVerificationFailureRetainsCompletionButWithholdsLoan) {
  PublicationFixture f;
  f.rejectVerification = 2;
  auto store = f.store();
  EXPECT_EQ(store.publish(f.record, f.hooks()), CourseBaselinePublicationResult::VerificationFailed);
  EXPECT_EQ(store.published(), nullptr);
  EXPECT_TRUE(f.base.storage.files.contains(f.prepared()));
  EXPECT_TRUE(f.base.storage.files.contains(f.published()));
  const auto files = f.base.storage.files;
  const auto mutations = f.base.storage.mutations;
  f.rejectVerification = 0;
  f.fresh = false;
  auto restored = f.store();
  ASSERT_EQ(restored.publish(f.record, f.hooks()), CourseBaselinePublicationResult::Ok);
  EXPECT_EQ(f.preparationCalls, 1u);
  EXPECT_EQ(f.publicationCalls, 1u);
  EXPECT_EQ(f.base.storage.mutations, mutations);
  EXPECT_EQ(f.base.storage.files, files);
}

TEST(CompanionCourseBaselinePublicationStore, ReadOrPermissionFailuresCannotFallBackToFreshPublication) {
  for (unsigned fault = 0; fault < 3; ++fault) {
    PublicationFixture f;
    auto store = f.store();
    ASSERT_EQ(store.publish(f.record, f.hooks()), CourseBaselinePublicationResult::Ok);
    const auto files = f.base.storage.files;
    const auto calls = f.publicationCalls;
    if (fault == 0) f.base.storage.statError = true;
    if (fault == 1) f.base.storage.readError = true;
    if (fault == 2) f.base.storage.revokeRead = true;
    EXPECT_NE(store.publish(f.record, f.hooks()), CourseBaselinePublicationResult::Ok);
    EXPECT_EQ(store.published(), nullptr);
    EXPECT_EQ(f.publicationCalls, calls);
    EXPECT_EQ(f.base.storage.files, files);
  }
}
