#include <gtest/gtest.h>
#include <openssl/sha.h>

#include <map>
#include <string>
#include <vector>

#include "lib/Companion/CompanionCoursePackArchive.h"

using namespace companion;
namespace {
class Storage final : public TransferStorage {
 public:
  std::map<std::string, std::vector<uint8_t>> files;
  unsigned mutations = 0, fail = 0, partial = 0;
  bool after = false, allowed = true;
  std::string revoke;
  bool prepare() override { return true; }
  FileStatus stat(const char* path, uint64_t& size) override {
    const auto found = files.find(path);
    if (found == files.end()) return FileStatus::Missing;
    size = found->second.size();
    return FileStatus::Present;
  }
  bool read(const char* path, uint64_t offset, std::span<uint8_t> bytes) override {
    const auto found = files.find(path);
    if (found == files.end() || offset > found->second.size() || bytes.size() > found->second.size() - offset)
      return false;
    std::copy_n(found->second.begin() + offset, bytes.size(), bytes.begin());
    return true;
  }
  bool write(const char* path, uint64_t offset, std::span<const uint8_t> bytes, bool truncate) override {
    const bool failed = ++mutations == fail;
    if (failed && !after) return false;
    auto& output = files[path];
    if (truncate) output.clear();
    if (offset > output.size()) return false;
    const auto count = mutations == partial ? bytes.size() / 2 : bytes.size();
    output.resize(offset + count);
    std::copy_n(bytes.begin(), count, output.begin() + offset);
    if (revoke == path) allowed = false;
    return !failed && mutations != partial;
  }
  bool rename(const char* from, const char* to) override {
    const bool failed = ++mutations == fail;
    if (failed && !after) return false;
    if (!files.contains(from) || files.contains(to)) return false;
    files[to] = std::move(files.at(from));
    files.erase(from);
    return !failed;
  }
  bool resize(const char*, uint64_t) override { return false; }
  bool remove(const char*) override { return false; }
  bool verify(const char* path, uint64_t length, const Digest& hash, std::span<uint8_t>) override {
    const auto found = files.find(path);
    if (found == files.end() || found->second.size() != length) return false;
    Digest actual{};
    SHA256(found->second.data(), found->second.size(), actual.data());
    return actual == hash;
  }
};
struct Fixture {
  Storage storage;
  std::array<uint8_t, 256> scratch{};
  std::vector<uint8_t> bytes = std::vector<uint8_t>(769, 7);
  ContentManifest manifest;
  Fixture() {
    manifest.kind = ContentKind::Course;
    manifest.formatVersion = 1;
    manifest.logicalIdentity.fill(3);
    manifest.length = bytes.size();
    SHA256(bytes.data(), bytes.size(), manifest.contentHash.data());
    storage.files["/tinta/course.pack"] = bytes;
  }
  static bool permitted(void* ctx) { return static_cast<Storage*>(ctx)->allowed; }
  CoursePackArchive owner() { return CoursePackArchive(storage, scratch, permitted, &storage); }
  std::string cache() const {
    std::string result(COURSE_ARCHIVE_PREFIX);
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    for (auto byte : manifest.contentHash) {
      result += HEX_DIGITS[byte >> 4];
      result += HEX_DIGITS[byte & 15];
    }
    return result;
  }
};
}  // namespace
TEST(CompanionCoursePackArchive, PublishesImmutableScopedReferenceAndVerifiedLoan) {
  Fixture f;
  auto archive = f.owner();
  EXPECT_EQ(archive.open(f.manifest.logicalIdentity, f.manifest.contentHash), CourseArchiveResult::Missing);
  ASSERT_EQ(archive.publish(f.manifest, "/tinta/course.pack"), CourseArchiveResult::Ok);
  ASSERT_NE(archive.path(), nullptr);
  ASSERT_NE(archive.referencePath(), nullptr);
  EXPECT_EQ(*archive.manifest(), f.manifest);
  EXPECT_EQ(f.storage.files.at(archive.path()), f.bytes);
  EXPECT_EQ(f.storage.files.at("/tinta/course.pack"), f.bytes);
  const auto reference = std::string(archive.referencePath());
  EXPECT_TRUE(reference.starts_with("/tinta/courses/03030303030303030303030303030303/pack-"));
  EXPECT_TRUE(reference.ends_with(".ref"));
  const auto files = f.storage.files;
  const auto writes = f.storage.mutations;
  EXPECT_EQ(archive.publish(f.manifest, "/tinta/course.pack"), CourseArchiveResult::Ok);
  EXPECT_EQ(f.storage.files, files);
  EXPECT_EQ(f.storage.mutations, writes);
  archive.close();
  EXPECT_EQ(archive.path(), nullptr);
  EXPECT_EQ(archive.manifest(), nullptr);
  auto reopened = f.owner();
  ASSERT_EQ(reopened.open(f.manifest.logicalIdentity, f.manifest.contentHash), CourseArchiveResult::Ok);
  f.storage.allowed = false;
  EXPECT_EQ(reopened.path(), nullptr);
  f.storage.allowed = true;
  EXPECT_EQ(reopened.path(), nullptr);
}
TEST(CompanionCoursePackArchive, EveryBeforeAndAfterMutationFailureResumesWithoutChangingSource) {
  Fixture baseline;
  auto first = baseline.owner();
  ASSERT_EQ(first.publish(baseline.manifest, "/tinta/course.pack"), CourseArchiveResult::Ok);
  const auto count = baseline.storage.mutations;
  ASSERT_GT(count, 0u);
  for (unsigned failure = 1; failure <= count; ++failure) {
    for (bool after : {false, true}) {
      SCOPED_TRACE(failure);
      SCOPED_TRACE(after);
      Fixture f;
      f.storage.fail = failure;
      f.storage.after = after;
      auto archive = f.owner();
      EXPECT_NE(archive.publish(f.manifest, "/tinta/course.pack"), CourseArchiveResult::Ok);
      EXPECT_EQ(archive.path(), nullptr);
      EXPECT_EQ(f.storage.files.at("/tinta/course.pack"), f.bytes);
      f.storage.fail = 0;
      auto recovered = f.owner();
      EXPECT_EQ(recovered.publish(f.manifest, "/tinta/course.pack"), CourseArchiveResult::Ok);
      EXPECT_EQ(f.storage.files, baseline.storage.files);
    }
  }
}
TEST(CompanionCoursePackArchive, ResumesOwnedPartialCopiesAndReferenceStages) {
  Fixture baseline;
  auto first = baseline.owner();
  ASSERT_EQ(first.publish(baseline.manifest, "/tinta/course.pack"), CourseArchiveResult::Ok);
  for (unsigned mutation = 2; mutation <= baseline.storage.mutations - 1; ++mutation) {
    Fixture f;
    f.storage.partial = mutation;
    auto archive = f.owner();
    const auto result = archive.publish(f.manifest, "/tinta/course.pack");
    if (result == CourseArchiveResult::Ok) continue;  // Rename calls have no partial-write mode.
    f.storage.partial = 0;
    auto recovered = f.owner();
    EXPECT_EQ(recovered.publish(f.manifest, "/tinta/course.pack"), CourseArchiveResult::Ok) << mutation;
    EXPECT_EQ(f.storage.files, baseline.storage.files);
  }
}
TEST(CompanionCoursePackArchive, PreservesTornOrForeignOwnersAndUnownedStages) {
  for (const auto suffix : {".owner", ".tmp", ""}) {
    Fixture f;
    f.storage.files[f.cache() + suffix] = {1, 2, 3};
    const auto files = f.storage.files;
    auto archive = f.owner();
    EXPECT_NE(archive.publish(f.manifest, "/tinta/course.pack"), CourseArchiveResult::Ok);
    EXPECT_EQ(f.storage.files, files);
  }
  Fixture f;
  f.storage.partial = 1;
  auto archive = f.owner();
  EXPECT_EQ(archive.publish(f.manifest, "/tinta/course.pack"), CourseArchiveResult::IoError);
  const auto torn = f.storage.files;
  f.storage.partial = 0;
  EXPECT_EQ(archive.publish(f.manifest, "/tinta/course.pack"), CourseArchiveResult::Corrupt);
  EXPECT_EQ(f.storage.files, torn);
}
TEST(CompanionCoursePackArchive, RefusesChangedCopyPrefixCorruptCacheAndRetargetedIdentity) {
  Fixture f;
  f.storage.partial = 2;
  auto archive = f.owner();
  ASSERT_EQ(archive.publish(f.manifest, "/tinta/course.pack"), CourseArchiveResult::IoError);
  f.storage.partial = 0;
  f.storage.files.at(f.cache() + ".tmp")[0] ^= 1;
  const auto corrupt = f.storage.files;
  EXPECT_EQ(archive.publish(f.manifest, "/tinta/course.pack"), CourseArchiveResult::Conflict);
  EXPECT_EQ(f.storage.files, corrupt);
  Fixture complete;
  auto published = complete.owner();
  ASSERT_EQ(published.publish(complete.manifest, "/tinta/course.pack"), CourseArchiveResult::Ok);
  auto foreign = complete.manifest;
  foreign.logicalIdentity[0] ^= 1;
  const auto files = complete.storage.files;
  EXPECT_EQ(published.publish(foreign, "/tinta/course.pack"), CourseArchiveResult::Conflict);
  EXPECT_EQ(complete.storage.files, files);
  EXPECT_EQ(published.path(), nullptr);
  complete.storage.files.at(complete.cache())[0] ^= 1;
  EXPECT_EQ(published.open(complete.manifest.logicalIdentity, complete.manifest.contentHash),
            CourseArchiveResult::Corrupt);
  EXPECT_EQ(published.path(), nullptr);
}
TEST(CompanionCoursePackArchive, RetainsEveryVersionWithoutEditingEarlierReferences) {
  Fixture f;
  auto archive = f.owner();
  ASSERT_EQ(archive.publish(f.manifest, "/tinta/course.pack"), CourseArchiveResult::Ok);
  const auto previous = f.manifest;
  const auto files = f.storage.files;
  f.bytes[0] ^= 1;
  SHA256(f.bytes.data(), f.bytes.size(), f.manifest.contentHash.data());
  f.storage.files["/candidate.pack"] = f.bytes;
  ASSERT_EQ(archive.publish(f.manifest, "/candidate.pack"), CourseArchiveResult::Ok);
  for (const auto& [path, bytes] : files) EXPECT_EQ(f.storage.files.at(path), bytes);
  EXPECT_EQ(archive.open(previous.logicalIdentity, previous.contentHash), CourseArchiveResult::Ok);
  EXPECT_EQ(*archive.manifest(), previous);
}
TEST(CompanionCoursePackArchive, PermissionLossDuringCopyWithholdsLoanAndCanResumeAfterReauthorization) {
  Fixture f;
  auto archive = f.owner();
  f.storage.allowed = false;
  const auto files = f.storage.files;
  EXPECT_EQ(archive.publish(f.manifest, "/tinta/course.pack"), CourseArchiveResult::Busy);
  EXPECT_EQ(f.storage.files, files);
  f.storage.allowed = true;
  f.storage.revoke = f.cache() + ".tmp";
  EXPECT_EQ(archive.publish(f.manifest, "/tinta/course.pack"), CourseArchiveResult::IoError);
  EXPECT_EQ(archive.path(), nullptr);
  EXPECT_EQ(f.storage.files.at("/tinta/course.pack"), f.bytes);
  f.storage.revoke.clear();
  f.storage.allowed = true;
  EXPECT_EQ(archive.publish(f.manifest, "/tinta/course.pack"), CourseArchiveResult::Ok);
  const auto cached = f.storage.files;
  EXPECT_EQ(archive.publish(f.manifest, archive.path()), CourseArchiveResult::Ok);
  EXPECT_EQ(f.storage.files, cached);
}
