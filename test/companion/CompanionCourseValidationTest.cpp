#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "lib/Companion/CompanionCourseSource.h"
#include "lib/Companion/CompanionCourseStatePaths.h"
#include "lib/Companion/CompanionCourseValidation.h"
#include "lib/Companion/CompanionInventoryIndex.h"
#include "lib/Companion/CompanionTintaPackSubjectCatalog.h"
#include "lib/hal/HalCourseValidation.h"
#include "lib/hal/HalInventoryIndexStorage.h"
namespace {
std::vector<uint8_t> fixture() {
  std::ifstream input(COURSE_FIXTURE, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}  // namespace
TEST(CompanionCourseValidation, CourseStatePathsAreIsolatedBoundedAndRejectTraversal) {
  companion::Identity first{}, second{};
  first[0] = 1;
  second[0] = 2;
  char output[companion::COURSE_STATE_PATH_SIZE];
  ASSERT_TRUE(companion::courseStatePath(first, "items.bin", output));
  EXPECT_STREQ(output, "/tinta/courses/01000000000000000000000000000000/items.bin");
  ASSERT_TRUE(companion::courseStatePath(second, "items.bin.tmp", output));
  EXPECT_STREQ(output, "/tinta/courses/02000000000000000000000000000000/items.bin.tmp");
  for (const auto name : {"", ".", "..", "../items.bin", "a/b", "a\\b", "a:b", "123456789012345678901234"}) {
    EXPECT_FALSE(companion::courseStatePath(first, name, output));
    EXPECT_EQ(output[0], '\0');
  }
  EXPECT_FALSE(companion::courseStatePath({}, "items.bin", output));
  EXPECT_EQ(output[0], '\0');
  std::fill_n(output, sizeof(output), 'x');
  EXPECT_FALSE(companion::courseStatePath(first, "items.bin", std::span(output).first(8)));
  EXPECT_EQ(output[0], '\0');
  EXPECT_EQ(output[8], 'x');
}
TEST(CompanionCourseValidation, ValidatesOptionalItemIdentityHistory) {
  auto original = fixture();
  tinta::core::pack::Pack catalog;
  ASSERT_EQ(catalog.open(original.data(), original.size()), tinta::core::pack::PackStatus::Ok);
  uint32_t maximum = 0;
  for (uint32_t index = 0; index < catalog.itemCount(); ++index) maximum = std::max(maximum, catalog.uidAt(index));
  tinta::core::pack::Header header{};
  std::memcpy(&header, original.data(), sizeof(header));
  const uint32_t directory = (original.size() + 3) & ~3u;
  const uint32_t identity = directory + (header.sectionCount + 1) * sizeof(tinta::core::pack::DirEntry);
  auto bytes = original;
  bytes.resize(identity + maximum * 36, 0);
  std::memcpy(bytes.data() + directory, original.data() + header.directoryOffset,
              header.sectionCount * sizeof(tinta::core::pack::DirEntry));
  tinta::core::pack::DirEntry entry{0x4e454449, identity, maximum * 36, maximum};
  std::memcpy(bytes.data() + directory + header.sectionCount * sizeof(entry), &entry, sizeof(entry));
  header.directoryOffset = directory;
  ++header.sectionCount;
  header.size = bytes.size();
  std::memcpy(bytes.data(), &header, sizeof(header));
  for (uint32_t index = 0; index < maximum; ++index) {
    const uint32_t uid = index + 1;
    std::memcpy(bytes.data() + identity + index * 36, &uid, sizeof(uid));
    bytes[identity + index * 36 + 4] = 1;
  }
  const auto validate = [](std::vector<uint8_t>& candidate) {
    std::fill_n(candidate.begin() + 20, 4, 0);
    const uint32_t crc = companion::inventoryIndexCrc(candidate);
    std::memcpy(candidate.data() + 20, &crc, sizeof(crc));
    tinta::core::pack::Pack pack;
    tinta::core::pack::MemorySource source(candidate.data(), candidate.size());
    uint8_t scratch[512];  // Host-only validation workspace.
    return companion::validateCourseCandidate(pack, source, scratch);
  };
  ASSERT_EQ(validate(bytes), companion::CourseValidationResult::Ok);
  {
    auto retired = bytes;
    retired.resize(retired.size() + 36, 0);
    auto extended = entry;
    ++extended.count;
    extended.size += 36;
    std::memcpy(retired.data() + directory + (header.sectionCount - 1) * sizeof(entry), &extended, sizeof(extended));
    const uint32_t uid = maximum + 1;
    std::memcpy(retired.data() + identity + maximum * 36, &uid, sizeof(uid));
    retired[identity + maximum * 36 + 4] = 1;
    auto extendedHeader = header;
    extendedHeader.size = retired.size();
    std::memcpy(retired.data(), &extendedHeader, sizeof(extendedHeader));
    ASSERT_EQ(validate(retired), companion::CourseValidationResult::Ok);
    tinta::core::pack::Pack pack;
    tinta::core::pack::MemorySource source(retired.data(), retired.size());
    ASSERT_EQ(pack.open(source), tinta::core::pack::PackStatus::Ok);
    std::array<uint8_t, 512> scratch{};
    companion::TintaPackSubjectCatalog subjects(pack, source);
    ASSERT_TRUE(subjects.prepare(scratch));
    ASSERT_LT(pack.indexOfUid(uid), 0);
    EXPECT_EQ(subjects.contains(companion::EventKind::Review, uid), companion::TintaSubjectMembership::Present);
    EXPECT_EQ(subjects.contains(companion::EventKind::UndoReview, uid), companion::TintaSubjectMembership::Present);
    EXPECT_EQ(subjects.contains(companion::EventKind::Review, uid + 1), companion::TintaSubjectMembership::Missing);
  }

  const auto compare = [&bytes](std::vector<uint8_t>& next) {
    tinta::core::pack::MemorySource currentSource(bytes.data(), bytes.size());
    tinta::core::pack::MemorySource nextSource(next.data(), next.size());
    return companion::compareCourseItemIdentities(currentSource, nextSource);
  };
  auto identical = bytes;
  EXPECT_EQ(compare(identical), companion::CourseItemContinuity::Compatible);
  EXPECT_EQ(compare(original), companion::CourseItemContinuity::MissingHistory);
  auto reassigned = bytes;
  reassigned[identity + 4] = 2;
  ASSERT_EQ(validate(reassigned), companion::CourseValidationResult::Ok);
  EXPECT_EQ(compare(reassigned), companion::CourseItemContinuity::ReassignedIdentity);
  auto expanded = bytes;
  expanded.resize(bytes.size() + 36, 0);
  const uint32_t addedUid = maximum + 1;
  std::memcpy(expanded.data() + bytes.size(), &addedUid, sizeof(addedUid));
  expanded[bytes.size() + 4] = 3;
  auto expandedEntry = entry;
  ++expandedEntry.count;
  expandedEntry.size += 36;
  std::memcpy(expanded.data() + directory + (header.sectionCount - 1) * sizeof(entry), &expandedEntry,
              sizeof(expandedEntry));
  auto expandedHeader = header;
  expandedHeader.size = expanded.size();
  std::memcpy(expanded.data(), &expandedHeader, sizeof(expandedHeader));
  ASSERT_EQ(validate(expanded), companion::CourseValidationResult::Ok);
  EXPECT_EQ(compare(expanded), companion::CourseItemContinuity::Compatible);
  tinta::core::pack::MemorySource expandedSource(expanded.data(), expanded.size());
  tinta::core::pack::MemorySource previousSource(bytes.data(), bytes.size());
  EXPECT_EQ(companion::compareCourseItemIdentities(expandedSource, previousSource),
            companion::CourseItemContinuity::RemovedHistory);
  auto changedRetired = expanded;
  changedRetired.back() = 1;
  ASSERT_EQ(validate(changedRetired), companion::CourseValidationResult::Ok);
  tinta::core::pack::MemorySource retiredSource(changedRetired.data(), changedRetired.size());
  EXPECT_EQ(companion::compareCourseItemIdentities(expandedSource, retiredSource),
            companion::CourseItemContinuity::ReassignedIdentity);
  for (const uint32_t offset : {identity, identity + 4}) {
    auto damaged = bytes;
    damaged[offset] = 0;
    EXPECT_EQ(validate(damaged), companion::CourseValidationResult::InvalidIdentity);
  }
  auto truncated = bytes;
  entry.count = maximum - 1;
  entry.size = entry.count * 36;
  std::memcpy(truncated.data() + directory + (header.sectionCount - 1) * sizeof(entry), &entry, sizeof(entry));
  EXPECT_EQ(validate(truncated), companion::CourseValidationResult::InvalidIdentity);
}
TEST(CompanionCourseValidation, AcceptsExistingCourseAndRejectsCorruption) {
  auto bytes = fixture();
  ASSERT_GT(bytes.size(), 48);
  // Host-only object; firmware callers must keep Pack outside the stack.
  tinta::core::pack::Pack pack;
  tinta::core::pack::MemorySource source(bytes.data(), bytes.size());
  ASSERT_EQ(companion::validateCourseCandidateRecords(pack, source), companion::CourseValidationResult::Ok);
  ASSERT_TRUE(pack.isOpen());
  EXPECT_GT(pack.itemCount(), 0);
  bytes[20] ^= 1;
  EXPECT_EQ(companion::validateCourseCandidateRecords(pack, source), companion::CourseValidationResult::Integrity);
  EXPECT_FALSE(pack.isOpen());
  bytes[0] ^= 1;
  EXPECT_EQ(companion::validateCourseCandidateRecords(pack, source),
            companion::CourseValidationResult::InvalidStructure);
  EXPECT_FALSE(pack.isOpen());
}
TEST(CompanionCourseValidation, ValidatesLocaleSyntaxAndPaddingWithCorrectChecksum) {
  for (const auto locale : {"es-MX", "fr", "zh-Hant", "es-419", "abcdefgh"})
    EXPECT_TRUE(companion::validCourseLocale(locale));
  for (const auto locale : {"", "es--MX", "-es", "es-", "12-US", "es/MX", "es MX", "abcdefghi"})
    EXPECT_FALSE(companion::validCourseLocale(locale));
  for (const auto locale :
       {std::string("es MX"), std::string("es--MX"), std::string("es\0MX", 5), std::string("\xff", 1), std::string()}) {
    auto bytes = fixture();
    std::fill_n(bytes.begin() + 24, 8, 0);
    std::copy(locale.begin(), locale.end(), bytes.begin() + 24);
    std::fill_n(bytes.begin() + 20, 4, 0);
    const auto crc = companion::inventoryIndexCrc(bytes);
    for (unsigned at = 0; at < 4; ++at) bytes[20 + at] = static_cast<uint8_t>(crc >> (8 * at));
    tinta::core::pack::Pack pack;
    tinta::core::pack::MemorySource source(bytes.data(), bytes.size());
    EXPECT_EQ(companion::validateCourseCandidateRecords(pack, source),
              companion::CourseValidationResult::InvalidContent);
    EXPECT_FALSE(pack.isOpen());
  }
}
TEST(CompanionCourseValidation, RejectsTruncatedCandidate) {
  auto bytes = fixture();
  tinta::core::pack::Pack pack;
  for (uint32_t size : {0U, 47U, static_cast<uint32_t>(bytes.size() - 1)}) {
    tinta::core::pack::MemorySource source(bytes.data(), size);
    EXPECT_EQ(companion::validateCourseCandidateRecords(pack, source),
              companion::CourseValidationResult::InvalidStructure);
    EXPECT_FALSE(pack.isOpen());
  }
}

TEST(CompanionCourseValidation, RejectsInvalidIdentityEvenWithValidCrc) {
  auto bytes = fixture();
  tinta::core::pack::Header header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  bool found = false;
  for (uint16_t i = 0; i < header.sectionCount; ++i) {
    tinta::core::pack::DirEntry entry;
    std::memcpy(&entry, bytes.data() + header.directoryOffset + i * sizeof(entry), sizeof(entry));
    if (std::memcmp(&entry, "ITEM", 4) == 0) {
      std::fill_n(bytes.begin() + entry.offset, 4, 0);
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);
  std::fill_n(bytes.begin() + 20, 4, 0);
  const uint32_t crc = companion::inventoryIndexCrc(bytes);
  for (unsigned i = 0; i < 4; ++i) bytes[20 + i] = static_cast<uint8_t>(crc >> (i * 8));
  tinta::core::pack::Pack pack;
  tinta::core::pack::MemorySource source(bytes.data(), bytes.size());
  EXPECT_EQ(companion::validateCourseCandidateRecords(pack, source),
            companion::CourseValidationResult::InvalidIdentity);
  EXPECT_FALSE(pack.isOpen());
}

namespace {
struct CourseStorage : companion::InventoryIndexStorage {
  std::vector<uint8_t> bytes = fixture();
  uint64_t reportedSize = bytes.size();
  bool failSize = false, failRead = false;
  unsigned reads = 0;
  unsigned failAtRead = 0;
  bool size(uint64_t& output) override {
    if (failSize) return false;
    output = reportedSize;
    return true;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    ++reads;
    if (failRead || reads == failAtRead || offset > bytes.size() || output.size() > bytes.size() - offset) return false;
    std::copy_n(bytes.begin() + offset, output.size(), output.begin());
    return true;
  }
};
}  // namespace
TEST(CompanionCourseValidation, ValidatesBorrowedStorageWithoutMemoryMapping) {
  CourseStorage storage;
  companion::StoredCourseSource source(storage);
  ASSERT_TRUE(source.attach());
  EXPECT_EQ(source.data(), nullptr);
  tinta::core::pack::Pack pack;
  ASSERT_EQ(companion::validateCourseCandidateRecords(pack, source), companion::CourseValidationResult::Ok);
  EXPECT_GT(storage.reads, 1);
  storage.failRead = true;
  EXPECT_EQ(companion::validateCourseCandidateRecords(pack, source),
            companion::CourseValidationResult::InvalidStructure);
  EXPECT_FALSE(pack.isOpen());
  EXPECT_EQ(source.size(), 0);
  storage.failRead = false;
  ASSERT_TRUE(source.attach());
  ASSERT_EQ(companion::validateCourseCandidateRecords(pack, source), companion::CourseValidationResult::Ok);
}
TEST(CompanionCourseValidation, SourceRejectsOversizedFilesAndInvalidReadRanges) {
  CourseStorage storage;
  companion::StoredCourseSource source(storage);
  storage.reportedSize = uint64_t(UINT32_MAX) + 1;
  EXPECT_FALSE(source.attach());
  EXPECT_EQ(source.size(), 0);
  storage.reportedSize = storage.bytes.size();
  ASSERT_TRUE(source.attach());
  uint8_t byte = 0;
  EXPECT_FALSE(source.read(UINT32_MAX, &byte, 1));
  EXPECT_FALSE(source.read(source.size(), &byte, 1));
  EXPECT_FALSE(source.read(0, nullptr, 1));
  EXPECT_EQ(storage.reads, 0);
  source.detach();
  EXPECT_FALSE(source.read(0, &byte, 1));
  storage.failSize = true;
  EXPECT_FALSE(source.attach());
}

TEST(CompanionCourseValidation, ReadsPackThroughActualHalProviderAndYields) {
  inventory_hal_test::state = inventory_hal_test::State{};
  inventory_hal_test::state.files["/candidate.pack"] = fixture();
  companion::HalInventoryIndexStorage storage;
  ASSERT_TRUE(storage.open("/candidate.pack"));
  companion::StoredCourseSource source(storage);
  ASSERT_TRUE(source.attach());
  tinta::core::pack::Pack pack;
  std::array<uint8_t, 512> scratch{};
  ASSERT_EQ(companion::validateCourseCandidate(pack, source, scratch), companion::CourseValidationResult::Ok);
  EXPECT_GT(inventory_hal_test::state.yields, 0);
  pack.close();
  source.detach();
  storage.close();
}

TEST(CompanionCourseValidation, RejectsChecksummedInvalidItemReferences) {
  for (unsigned failure = 0; failure < 4; ++failure) {
    auto bytes = fixture();
    tinta::core::pack::Header header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    uint32_t offset = 0;
    for (uint16_t i = 0; i < header.sectionCount; ++i) {
      tinta::core::pack::DirEntry entry;
      std::memcpy(&entry, bytes.data() + header.directoryOffset + i * sizeof(entry), sizeof(entry));
      if (std::memcmp(&entry, "ITEM", 4) == 0) {
        offset = entry.offset;
        break;
      }
    }
    ASSERT_NE(offset, 0);
    switch (failure) {
      case 0:
        bytes[offset + 4] = 7;
        break;
      case 1:
        bytes[offset + 6] = 0xfe;
        bytes[offset + 7] = 0xff;
        break;
      case 2:
        bytes[offset + 12] = 0;
        bytes[offset + 13] = 0;
        break;
      case 3:
        std::fill_n(bytes.begin() + offset + 16, 4, 0xff);
        break;
    }
    std::fill_n(bytes.begin() + 20, 4, 0);
    const uint32_t crc = companion::inventoryIndexCrc(bytes);
    for (unsigned i = 0; i < 4; ++i) bytes[20 + i] = static_cast<uint8_t>(crc >> (i * 8));
    tinta::core::pack::Pack pack;
    tinta::core::pack::MemorySource source(bytes.data(), bytes.size());
    EXPECT_EQ(companion::validateCourseCandidateRecords(pack, source),
              companion::CourseValidationResult::InvalidContent);
    EXPECT_FALSE(pack.isOpen());
  }
}

TEST(CompanionCourseValidation, RejectsChecksummedUnorderedIdentityTable) {
  auto bytes = fixture();
  tinta::core::pack::Header header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  uint32_t offset = 0, stride = 0;
  for (uint16_t i = 0; i < header.sectionCount; ++i) {
    tinta::core::pack::DirEntry entry;
    std::memcpy(&entry, bytes.data() + header.directoryOffset + i * sizeof(entry), sizeof(entry));
    if (std::memcmp(&entry, "IUID", 4) == 0) {
      ASSERT_GT(entry.count, 1);
      offset = entry.offset;
      stride = entry.size / entry.count;
      break;
    }
  }
  ASSERT_NE(offset, 0);
  for (uint32_t i = 0; i < stride; ++i) std::swap(bytes[offset + i], bytes[offset + stride + i]);
  std::fill_n(bytes.begin() + 20, 4, 0);
  const uint32_t crc = companion::inventoryIndexCrc(bytes);
  for (unsigned i = 0; i < 4; ++i) bytes[20 + i] = static_cast<uint8_t>(crc >> (i * 8));
  tinta::core::pack::Pack pack;
  tinta::core::pack::MemorySource source(bytes.data(), bytes.size());
  EXPECT_EQ(companion::validateCourseCandidateRecords(pack, source),
            companion::CourseValidationResult::InvalidIdentity);
  EXPECT_FALSE(pack.isOpen());
}

TEST(CompanionCourseValidation, RejectsChecksummedLessonAndStoryRangeErrors) {
  for (const auto& [tag, field] : std::vector<std::pair<std::string, uint32_t>>{
           {"UNIT", 14}, {"LESS", 14}, {"LESS", 20}, {"STOR", 14}, {"STOR", 16}}) {
    auto bytes = fixture();
    tinta::core::pack::Header header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    uint32_t offset = 0;
    for (uint16_t i = 0; i < header.sectionCount; ++i) {
      tinta::core::pack::DirEntry entry;
      std::memcpy(&entry, bytes.data() + header.directoryOffset + i * sizeof(entry), sizeof(entry));
      if (std::memcmp(&entry, tag.data(), 4) == 0) {
        ASSERT_GT(entry.count, 0);
        offset = entry.offset;
        break;
      }
    }
    ASSERT_NE(offset, 0);
    bytes[offset + field] = 0xfe;
    bytes[offset + field + 1] = 0xff;
    std::fill_n(bytes.begin() + 20, 4, 0);
    const uint32_t crc = companion::inventoryIndexCrc(bytes);
    for (unsigned i = 0; i < 4; ++i) bytes[20 + i] = static_cast<uint8_t>(crc >> (i * 8));
    tinta::core::pack::Pack pack;
    tinta::core::pack::MemorySource source(bytes.data(), bytes.size());
    EXPECT_EQ(companion::validateCourseCandidateRecords(pack, source),
              companion::CourseValidationResult::InvalidContent);
    EXPECT_FALSE(pack.isOpen());
  }
}

TEST(CompanionCourseValidation, RejectsChecksummedVocabularyOperandErrors) {
  for (uint32_t field : {8U, 10U}) {
    auto bytes = fixture();
    tinta::core::pack::Header header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    uint32_t offset = 0;
    for (uint16_t i = 0; i < header.sectionCount; ++i) {
      tinta::core::pack::DirEntry entry;
      std::memcpy(&entry, bytes.data() + header.directoryOffset + i * sizeof(entry), sizeof(entry));
      if (std::memcmp(&entry, "ITEM", 4) != 0) continue;
      for (uint32_t item = 0; item < entry.count; ++item) {
        const auto at = entry.offset + item * (entry.size / entry.count);
        if (bytes[at + 4] == 0) {
          offset = at;
          break;
        }
      }
      break;
    }
    ASSERT_NE(offset, 0);
    bytes[offset + field] = 0xfe;
    bytes[offset + field + 1] = 0xff;
    std::fill_n(bytes.begin() + 20, 4, 0);
    const uint32_t crc = companion::inventoryIndexCrc(bytes);
    for (unsigned i = 0; i < 4; ++i) bytes[20 + i] = static_cast<uint8_t>(crc >> (i * 8));
    tinta::core::pack::Pack pack;
    tinta::core::pack::MemorySource source(bytes.data(), bytes.size());
    EXPECT_EQ(companion::validateCourseCandidateRecords(pack, source),
              companion::CourseValidationResult::InvalidContent);
    EXPECT_FALSE(pack.isOpen());
  }
}

TEST(CompanionCourseValidation, RejectsChecksummedUnsupportedConjugationTags) {
  for (uint16_t tag : {uint16_t(0x1fff), uint16_t(0x2003), uint16_t(0)}) {
    auto bytes = fixture();
    tinta::core::pack::Header header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    uint32_t offset = 0;
    for (uint16_t i = 0; i < header.sectionCount; ++i) {
      tinta::core::pack::DirEntry entry;
      std::memcpy(&entry, bytes.data() + header.directoryOffset + i * sizeof(entry), sizeof(entry));
      if (std::memcmp(&entry, "ITEM", 4) != 0) continue;
      for (uint32_t item = 0; item < entry.count; ++item) {
        const auto at = entry.offset + item * (entry.size / entry.count);
        if (bytes[at + 4] == 3) {
          offset = at;
          break;
        }
      }
      break;
    }
    ASSERT_NE(offset, 0);
    bytes[offset + 10] = static_cast<uint8_t>(tag);
    bytes[offset + 11] = static_cast<uint8_t>(tag >> 8);
    std::fill_n(bytes.begin() + 20, 4, 0);
    const uint32_t crc = companion::inventoryIndexCrc(bytes);
    for (unsigned i = 0; i < 4; ++i) bytes[20 + i] = static_cast<uint8_t>(crc >> (i * 8));
    tinta::core::pack::Pack pack;
    tinta::core::pack::MemorySource source(bytes.data(), bytes.size());
    EXPECT_EQ(companion::validateCourseCandidateRecords(pack, source),
              companion::CourseValidationResult::InvalidContent);
    EXPECT_FALSE(pack.isOpen());
  }
}

TEST(CompanionCourseValidation, RejectsChecksummedOutOfRangeStringReferences) {
  for (const std::string tag : {"LEMM", "LESS", "SENT", "VERB"}) {
    auto bytes = fixture();
    tinta::core::pack::Header header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    uint32_t offset = 0;
    for (uint16_t i = 0; i < header.sectionCount; ++i) {
      tinta::core::pack::DirEntry entry;
      std::memcpy(&entry, bytes.data() + header.directoryOffset + i * sizeof(entry), sizeof(entry));
      if (std::memcmp(&entry, tag.data(), 4) == 0) {
        ASSERT_GT(entry.count, 0);
        offset = entry.offset;
        break;
      }
    }
    ASSERT_NE(offset, 0);
    std::fill_n(bytes.begin() + offset, 4, 0xff);
    std::fill_n(bytes.begin() + 20, 4, 0);
    const uint32_t crc = companion::inventoryIndexCrc(bytes);
    for (unsigned i = 0; i < 4; ++i) bytes[20 + i] = static_cast<uint8_t>(crc >> (i * 8));
    tinta::core::pack::Pack pack;
    tinta::core::pack::MemorySource source(bytes.data(), bytes.size());
    EXPECT_EQ(companion::validateCourseCandidateRecords(pack, source),
              companion::CourseValidationResult::InvalidContent);
    EXPECT_FALSE(pack.isOpen());
  }
}

TEST(CompanionCourseValidation, Utf8StateSurvivesReadBoundariesAndRejectsInvalidEncodings) {
  std::vector<uint8_t> bytes(133, 'a');
  bytes[127] = 0xc3;
  bytes[128] = 0xb1;
  tinta::core::pack::MemorySource source(bytes.data(), bytes.size());
  EXPECT_TRUE(companion::validCourseUtf8(source, 0, bytes.size()));
  for (const auto& invalid : std::vector<std::vector<uint8_t>>{
           {0xc0, 0xaf}, {0xe0, 0x80, 0x80}, {0xed, 0xa0, 0x80}, {0xf4, 0x90, 0x80, 0x80}, {0x80}, {0xc2}, {0xc2, 0}}) {
    source.reset(invalid.data(), invalid.size());
    EXPECT_FALSE(companion::validCourseUtf8(source, 0, invalid.size()));
  }
  const uint8_t valid[] = {0, 0xf4, 0x8f, 0xbf, 0xbf, 0};
  source.reset(valid, sizeof(valid));
  EXPECT_TRUE(companion::validCourseUtf8(source, 0, sizeof(valid)));
  EXPECT_FALSE(companion::validCourseUtf8(source, UINT32_MAX, 1));
}

TEST(CompanionCourseValidation, RejectsChecksummedInvalidUtf8StringHeap) {
  auto bytes = fixture();
  tinta::core::pack::Header header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  uint32_t offset = 0;
  for (uint16_t i = 0; i < header.sectionCount; ++i) {
    tinta::core::pack::DirEntry entry;
    std::memcpy(&entry, bytes.data() + header.directoryOffset + i * sizeof(entry), sizeof(entry));
    if (std::memcmp(&entry, "STRS", 4) == 0) {
      ASSERT_GT(entry.size, 2);
      offset = entry.offset;
      break;
    }
  }
  ASSERT_NE(offset, 0);
  bytes[offset + 1] = 0x80;
  std::fill_n(bytes.begin() + 20, 4, 0);
  const uint32_t crc = companion::inventoryIndexCrc(bytes);
  for (unsigned i = 0; i < 4; ++i) bytes[20 + i] = static_cast<uint8_t>(crc >> (i * 8));
  tinta::core::pack::Pack pack;
  tinta::core::pack::MemorySource source(bytes.data(), bytes.size());
  EXPECT_EQ(companion::validateCourseCandidateRecords(pack, source), companion::CourseValidationResult::InvalidContent);
  EXPECT_FALSE(pack.isOpen());
}

TEST(CompanionCourseValidation, ReadFailuresThroughoutValidationCloseCandidateAndAllowRetry) {
  CourseStorage baseline;
  companion::StoredCourseSource baselineSource(baseline);
  ASSERT_TRUE(baselineSource.attach());
  tinta::core::pack::Pack pack;
  std::array<uint8_t, 512> scratch{};
  ASSERT_EQ(companion::validateCourseCandidate(pack, baselineSource, scratch), companion::CourseValidationResult::Ok);
  const unsigned reads = baseline.reads;
  ASSERT_GT(reads, 10);
  pack.close();
  for (unsigned failAt : {1U, 2U, reads / 4, reads / 2, reads - 1, reads}) {
    CourseStorage storage;
    storage.failAtRead = failAt;
    companion::StoredCourseSource source(storage);
    ASSERT_TRUE(source.attach());
    EXPECT_NE(companion::validateCourseCandidate(pack, source, scratch), companion::CourseValidationResult::Ok);
    EXPECT_FALSE(pack.isOpen());
    EXPECT_EQ(source.size(), 0);
    storage.failAtRead = 0;
    ASSERT_TRUE(source.attach());
    ASSERT_EQ(companion::validateCourseCandidate(pack, source, scratch), companion::CourseValidationResult::Ok);
    pack.close();
  }
}

TEST(CompanionCourseValidation, AuthoredIdentityValidationBorrowsBoundedScratch) {
  auto bytes = fixture();
  tinta::core::pack::Pack pack;
  ASSERT_EQ(pack.open(bytes.data(), bytes.size()), tinta::core::pack::PackStatus::Ok);
  std::array<uint8_t, 512> scratch{};
  EXPECT_TRUE(companion::validCourseAuthoredIdentities(pack, scratch));
  tinta::core::pack::MemorySource source(bytes.data(), bytes.size());
  EXPECT_EQ(companion::validateCourseCandidate(pack, source, scratch), companion::CourseValidationResult::Ok);
  EXPECT_FALSE(companion::validCourseAuthoredIdentities(pack, std::span(scratch).first(511)));
  pack.close();
  EXPECT_FALSE(companion::validCourseAuthoredIdentities(pack, scratch));
}
TEST(CompanionCourseValidation, AuthoredIdentityValidationRejectsDuplicateLessonNumbers) {
  auto bytes = fixture();
  tinta::core::pack::Header header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  uint32_t offset = 0, stride = 0;
  for (uint16_t i = 0; i < header.sectionCount; ++i) {
    tinta::core::pack::DirEntry entry;
    std::memcpy(&entry, bytes.data() + header.directoryOffset + i * sizeof(entry), sizeof(entry));
    if (std::memcmp(&entry, "LESS", 4) == 0) {
      ASSERT_GT(entry.count, 1);
      offset = entry.offset;
      stride = entry.size / entry.count;
      break;
    }
  }
  ASSERT_NE(offset, 0);
  ASSERT_EQ(bytes[offset + 14], bytes[offset + stride + 14]);
  ASSERT_EQ(bytes[offset + 15], bytes[offset + stride + 15]);
  bytes[offset + stride + 16] = bytes[offset + 16];
  bytes[offset + stride + 17] = bytes[offset + 17];
  tinta::core::pack::Pack pack;
  ASSERT_EQ(pack.open(bytes.data(), bytes.size()), tinta::core::pack::PackStatus::Ok);
  std::array<uint8_t, 512> scratch{};
  EXPECT_FALSE(companion::validCourseAuthoredIdentities(pack, scratch));
}

TEST(CompanionCourseValidation, AcceptsIntroductoryUnitZero) {
  auto bytes = fixture();
  tinta::core::pack::Header header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  bool found = false;
  for (uint16_t i = 0; i < header.sectionCount; ++i) {
    tinta::core::pack::DirEntry entry;
    std::memcpy(&entry, bytes.data() + header.directoryOffset + i * sizeof(entry), sizeof(entry));
    if (std::memcmp(&entry, "UNIT", 4) == 0) {
      bytes[entry.offset + 12] = bytes[entry.offset + 13] = 0;
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);
  std::fill_n(bytes.begin() + 20, 4, 0);
  const uint32_t crc = companion::inventoryIndexCrc(bytes);
  for (unsigned i = 0; i < 4; ++i) bytes[20 + i] = static_cast<uint8_t>(crc >> (i * 8));
  tinta::core::pack::Pack pack;
  tinta::core::pack::MemorySource source(bytes.data(), bytes.size());
  std::array<uint8_t, 512> scratch{};
  EXPECT_EQ(companion::validateCourseCandidate(pack, source, scratch), companion::CourseValidationResult::Ok);
}

TEST(CompanionCourseValidation, AuthoredIdentityValidationRejectsDuplicateUnitNumbers) {
  auto bytes = fixture();
  tinta::core::pack::Header header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  uint32_t directoryEntry = 0, offset = 0, stride = 0;
  for (uint16_t i = 0; i < header.sectionCount; ++i) {
    tinta::core::pack::DirEntry entry;
    const auto at = header.directoryOffset + i * sizeof(entry);
    std::memcpy(&entry, bytes.data() + at, sizeof(entry));
    if (std::memcmp(&entry, "UNIT", 4) == 0) {
      ASSERT_GT(entry.count, 0);
      directoryEntry = at;
      offset = entry.offset;
      stride = entry.size / entry.count;
      break;
    }
  }
  ASSERT_NE(directoryEntry, 0);
  const std::vector<uint8_t> record(bytes.begin() + offset, bytes.begin() + offset + stride);
  bytes.reserve(bytes.size() + 3 + 2 * stride);
  while (bytes.size() % 4 != 0) bytes.push_back(0);
  const uint32_t newOffset = bytes.size();
  bytes.insert(bytes.end(), record.begin(), record.end());
  bytes.insert(bytes.end(), record.begin(), record.end());
  tinta::core::pack::DirEntry entry;
  std::memcpy(&entry, bytes.data() + directoryEntry, sizeof(entry));
  entry.offset = newOffset;
  entry.size = 2 * stride;
  entry.count = 2;
  std::memcpy(bytes.data() + directoryEntry, &entry, sizeof(entry));
  const uint32_t size = bytes.size();
  std::memcpy(bytes.data() + 16, &size, sizeof(size));
  tinta::core::pack::Pack pack;
  ASSERT_EQ(pack.open(bytes.data(), bytes.size()), tinta::core::pack::PackStatus::Ok);
  std::array<uint8_t, 512> scratch{};
  EXPECT_FALSE(companion::validCourseAuthoredIdentities(pack, scratch));
}

TEST(CompanionCourseValidation, AuthoredIdentityValidationRejectsOrphanedLessons) {
  auto bytes = fixture();
  tinta::core::pack::Header header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  uint32_t offset = 0;
  for (uint16_t i = 0; i < header.sectionCount; ++i) {
    tinta::core::pack::DirEntry entry;
    std::memcpy(&entry, bytes.data() + header.directoryOffset + i * sizeof(entry), sizeof(entry));
    if (std::memcmp(&entry, "UNIT", 4) == 0) {
      offset = entry.offset;
      break;
    }
  }
  ASSERT_NE(offset, 0);
  bytes[offset + 16] = 1;
  bytes[offset + 17] = 0;
  tinta::core::pack::Pack pack;
  ASSERT_EQ(pack.open(bytes.data(), bytes.size()), tinta::core::pack::PackStatus::Ok);
  std::array<uint8_t, 512> scratch{};
  EXPECT_FALSE(companion::validCourseAuthoredIdentities(pack, scratch));
}

TEST(CompanionCourseValidation, StoryIdentitiesMatchVersionOneFixture) {
  auto bytes = fixture();
  tinta::core::pack::MemorySource source(bytes.data(), bytes.size());
  tinta::core::pack::Pack pack;
  ASSERT_EQ(pack.open(source), tinta::core::pack::PackStatus::Ok);
  tinta::core::pack::Header header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  uint32_t offset = 0, size = 0;
  for (uint16_t i = 0; i < header.sectionCount; ++i) {
    tinta::core::pack::DirEntry entry;
    std::memcpy(&entry, bytes.data() + header.directoryOffset + i * sizeof(entry), sizeof(entry));
    if (std::memcmp(&entry, "STRS", 4) == 0) {
      offset = entry.offset;
      size = entry.size;
    }
  }
  ASSERT_NE(size, 0);
  companion::CourseStoryKeys keys(pack, source, offset, size);
  const uint32_t expected[] = {3491721030U, 3028353045U, 1274730023U};
  ASSERT_EQ(keys.count(), 3);
  for (uint32_t i = 0; i < keys.count(); ++i) {
    uint32_t identity = 0;
    ASSERT_TRUE(keys.read(i, identity));
    EXPECT_EQ(identity, expected[i]);
  }
}

TEST(CompanionCourseValidation, StoryIdentityValidationRejectsDuplicateSubjects) {
  auto bytes = fixture();
  tinta::core::pack::Header header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  uint32_t offset = 0, stride = 0;
  for (uint16_t i = 0; i < header.sectionCount; ++i) {
    tinta::core::pack::DirEntry entry;
    std::memcpy(&entry, bytes.data() + header.directoryOffset + i * sizeof(entry), sizeof(entry));
    if (std::memcmp(&entry, "STOR", 4) == 0) {
      ASSERT_GT(entry.count, 1);
      offset = entry.offset;
      stride = entry.size / entry.count;
      break;
    }
  }
  ASSERT_NE(offset, 0);
  tinta::core::pack::Pack pack;
  tinta::core::pack::MemorySource source(bytes.data(), bytes.size());
  ASSERT_EQ(pack.open(source), tinta::core::pack::PackStatus::Ok);
  std::array<uint8_t, 512> scratch{};
  ASSERT_TRUE(companion::validCourseStoryIdentities(pack, source, scratch));
  std::copy_n(bytes.begin() + offset, 4, bytes.begin() + offset + stride);
  EXPECT_TRUE(companion::validCourseStoryIdentities(pack, source, scratch));
  std::copy_n(bytes.begin() + offset + 14, 2, bytes.begin() + offset + stride + 14);
  bytes[offset + stride + 20] = bytes[offset + 20];
  EXPECT_FALSE(companion::validCourseStoryIdentities(pack, source, scratch));
  std::fill_n(bytes.begin() + 20, 4, 0);
  const uint32_t crc = companion::inventoryIndexCrc(bytes);
  for (unsigned i = 0; i < 4; ++i) bytes[20 + i] = static_cast<uint8_t>(crc >> (i * 8));
  EXPECT_EQ(companion::validateCourseCandidate(pack, source, scratch),
            companion::CourseValidationResult::InvalidIdentity);
  EXPECT_FALSE(pack.isOpen());
}

TEST(CompanionCourseValidation, HalEntryPointReturnsMetadataAndClosesBorrowedPack) {
  inventory_hal_test::state = inventory_hal_test::State{};
  inventory_hal_test::state.files["/candidate.pack"] = fixture();
  tinta::core::pack::Pack pack;
  std::array<uint8_t, 512> scratch{};
  companion::CourseCandidateDetails details;
  ASSERT_TRUE(companion::validateStagedCourse("/candidate.pack", pack, scratch, details));
  EXPECT_FALSE(pack.isOpen());
  EXPECT_EQ(details.major, 1);
  EXPECT_GT(details.items, 0);
  EXPECT_NE(details.locale[0], 0);
  const auto previous = details;
  EXPECT_FALSE(companion::validateStagedCourse("/missing.pack", pack, scratch, details));
  EXPECT_EQ(details.items, previous.items);
  EXPECT_FALSE(pack.isOpen());
  EXPECT_FALSE(companion::validateStagedCourse("/candidate.pack", pack, std::span(scratch).first(511), details));
  EXPECT_EQ(details.items, previous.items);
}

TEST(CompanionCourseValidation, HalReadFailuresPreserveMetadataAndAllowRetry) {
  inventory_hal_test::state = inventory_hal_test::State{};
  auto& state = inventory_hal_test::state;
  state.files["/candidate.pack"] = fixture();
  tinta::core::pack::Pack pack;
  std::array<uint8_t, 512> scratch{};
  companion::CourseCandidateDetails details;
  ASSERT_TRUE(companion::validateStagedCourse("/candidate.pack", pack, scratch, details));
  const unsigned reads = state.reads;
  ASSERT_GT(reads, 2);
  for (const bool shortRead : {false, true}) {
    for (const unsigned at : {1U, reads / 2, reads}) {
      SCOPED_TRACE(::testing::Message() << "read=" << at << " short=" << shortRead);
      details.major = 7;
      details.minor = 8;
      details.edition = 9;
      details.items = 10;
      details.lessons = 11;
      details.stories = 12;
      std::memcpy(details.locale, "sentinel", 9);
      state.reads = 0;
      state.failRead = shortRead ? 0 : at;
      state.shortRead = shortRead ? at : 0;
      EXPECT_FALSE(companion::validateStagedCourse("/candidate.pack", pack, scratch, details));
      EXPECT_FALSE(pack.isOpen());
      EXPECT_EQ(details.major, 7);
      EXPECT_EQ(details.minor, 8);
      EXPECT_EQ(details.edition, 9);
      EXPECT_EQ(details.items, 10);
      EXPECT_EQ(details.lessons, 11);
      EXPECT_EQ(details.stories, 12);
      EXPECT_STREQ(details.locale, "sentinel");
      state.reads = state.failRead = state.shortRead = 0;
      ASSERT_TRUE(companion::validateStagedCourse("/candidate.pack", pack, scratch, details));
      EXPECT_FALSE(pack.isOpen());
      EXPECT_EQ(details.major, 1);
      EXPECT_GT(details.items, 0);
    }
  }
}

TEST(CompanionCourseValidation, SubjectCatalogValidatesActiveItemsLessonsAndDistributedReadings) {
  auto bytes = fixture();
  tinta::core::pack::MemorySource source(bytes.data(), bytes.size());
  tinta::core::pack::Pack pack;
  std::array<uint8_t, 512> scratch{};
  ASSERT_EQ(companion::validateCourseCandidate(pack, source, scratch), companion::CourseValidationResult::Ok);
  companion::TintaPackSubjectCatalog catalog(pack, source);
  EXPECT_EQ(catalog.contains(companion::EventKind::Review, 1), companion::TintaSubjectMembership::IoError);
  ASSERT_TRUE(catalog.prepare(scratch));
  ASSERT_GT(pack.itemCount(), 0u);
  EXPECT_EQ(catalog.contains(companion::EventKind::Review, pack.uidAt(0)), companion::TintaSubjectMembership::Present);
  EXPECT_EQ(catalog.contains(companion::EventKind::Review, UINT32_MAX - 1), companion::TintaSubjectMembership::Missing);
  companion::TintaPackSubjectKeys lessons(pack, false), readings(pack, true);
  for (auto* keys : {&lessons, &readings}) {
    const auto kind = keys == &lessons ? companion::EventKind::LessonComplete : companion::EventKind::ReadingComplete;
    for (uint32_t at = 0; at < keys->count(); ++at) {
      uint32_t uid = 0;
      ASSERT_TRUE(keys->read(at, uid));
      EXPECT_EQ(catalog.contains(kind, uid), companion::TintaSubjectMembership::Present);
    }
  }
  EXPECT_EQ(catalog.contains(companion::EventKind::Preference, 1), companion::TintaSubjectMembership::Missing);
}
