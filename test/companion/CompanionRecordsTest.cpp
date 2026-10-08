#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "lib/Companion/CompanionCapabilities.h"
#include "lib/Companion/CompanionContentRemovalHandler.h"
#include "lib/Companion/CompanionContentRemovalRequest.h"
#include "lib/Companion/CompanionCourseBinding.h"
#include "lib/Companion/CompanionDeclaredTransferCommand.h"
#include "lib/Companion/CompanionPreferenceBody.h"
#include "lib/Companion/CompanionReadingBody.h"
#include "lib/Companion/CompanionRecords.h"
#include "lib/Companion/CompanionTransferDeclaration.h"

using namespace companion;

namespace {
std::vector<uint8_t> fixture(const char* name) {
  std::ifstream file(std::string(COMPANION_FIXTURE_DIR) + "/" + name + ".json");
  const std::string json{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  const auto key = json.find("\"binaryHex\": \"");
  if (key == std::string::npos) return {};
  const size_t start = key + 14;
  const auto end = json.find('"', start);
  if (end == std::string::npos || (end - start) % 2 != 0) return {};
  std::vector<uint8_t> bytes;
  bytes.reserve((end - start) / 2);
  for (size_t i = start; i < end; i += 2)
    bytes.push_back(static_cast<uint8_t>(std::stoul(json.substr(i, 2), nullptr, 16)));
  return bytes;
}

Identity identity(uint8_t first) {
  Identity value;
  for (size_t i = 0; i < value.size(); ++i) value[i] = first + i;
  return value;
}
Digest digest() {
  Digest value;
  for (size_t i = 0; i < value.size(); ++i) value[i] = i;
  return value;
}

template <typename T>
void checkFixture(const char* name, const T& expected) {
  const auto bytes = fixture(name);
  ASSERT_FALSE(bytes.empty());
  T decoded;
  ASSERT_TRUE(decodeRecord(bytes, decoded));
  EXPECT_EQ(decoded, expected);
  std::array<uint8_t, MAX_RECORD_SIZE> encoded{};
  ASSERT_EQ(encodeRecord(expected, encoded), bytes.size());
  EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), encoded.begin()));
  for (size_t size = 0; size < bytes.size(); ++size) {
    decoded = expected;
    EXPECT_FALSE(decodeRecord(std::span(bytes).first(size), decoded));
    EXPECT_EQ(decoded, expected);
  }
  auto extra = bytes;
  extra.push_back(0);
  EXPECT_FALSE(decodeRecord(extra, decoded));
  auto badVersion = bytes;
  badVersion[0] = 2;
  EXPECT_FALSE(decodeRecord(badVersion, decoded));
  auto badType = bytes;
  badType[1] = 0;
  EXPECT_FALSE(decodeRecord(badType, decoded));
  encoded.fill(0xAA);
  EXPECT_EQ(encodeRecord(expected, std::span(encoded).first(bytes.size() - 1)), 0U);
  EXPECT_TRUE(std::all_of(encoded.begin(), encoded.end(), [](uint8_t byte) { return byte == 0xAA; }));
}
}  // namespace

TEST(CompanionRecords, DeviceDescriptorFixture) {
  DeviceDescriptor expected{identity(1), identity(17), Board::X4, 15, 75, 1, 1, digest()};
  checkFixture("DeviceDescriptor", expected);
}
TEST(CompanionRecords, ContentManifestFixture) {
  ContentManifest expected{digest(), ContentKind::Course, 123456, 1, identity(1)};
  checkFixture("ContentManifest", expected);
}
TEST(CompanionRecords, SyncEventFixture) {
  SyncEvent expected;
  expected.identity = {identity(1), 3, 7};
  expected.storageGeneration = identity(17);
  expected.kind = EventKind::Review;
  expected.resource = digest();
  expected.bodyHash = digest();
  expected.studyDay = 20000;
  expected.timestamp = 1728000000;
  expected.clockQuality = ClockQuality::Trusted;
  expected.schedulerVersion = 1;
  expected.schedulerConfiguration = digest();
  expected.ancestorCount = 1;
  expected.ancestors[0] = {identity(1), 3, 6};
  checkFixture("SyncEvent", expected);
}
TEST(CompanionRecords, SyncCheckpointFixture) {
  SyncCheckpoint expected{identity(1), identity(17), {identity(1), 3, 7}, digest()};
  checkFixture("SyncCheckpoint", expected);
}
TEST(CompanionRecords, TransferStateFixture) {
  TransferState expected{identity(1), identity(1), identity(17), digest(), 123456, 8192, TransferPhase::Receiving};
  checkFixture("TransferState", expected);
}
TEST(CompanionRecords, RejectsInvalidTransferStateWithoutModifyingOutput) {
  auto bytes = fixture("TransferState");
  ASSERT_EQ(bytes.size(), TRANSFER_STATE_SIZE);
  TransferState output;
  output.length = 42;
  bytes[98] = static_cast<uint8_t>(TransferPhase::Verified);
  EXPECT_FALSE(decodeRecord(bytes, output));
  EXPECT_EQ(output.length, 42U);
  bytes[98] = 1;
  for (size_t i = 90; i < 98; ++i) bytes[i] = 0xff;
  EXPECT_FALSE(decodeRecord(bytes, output));
  EXPECT_EQ(output.length, 42U);
}
TEST(CompanionRecords, RejectsInvalidEnumsAndCausalTail) {
  DeviceDescriptor device;
  auto bytes = fixture("DeviceDescriptor");
  bytes[39] = 101;
  EXPECT_FALSE(decodeRecord(bytes, device));
  ContentManifest content;
  bytes = fixture("ContentManifest");
  bytes[34] = 0;
  EXPECT_FALSE(decodeRecord(bytes, content));
  SyncEvent event;
  bytes = fixture("SyncEvent");
  bytes[164] = 5;
  EXPECT_FALSE(decodeRecord(bytes, event));
  bytes = fixture("SyncEvent");
  bytes[127] = 3;
  EXPECT_FALSE(decodeRecord(bytes, event));
}
TEST(CompanionRecords, AcceptsMaximumCausalTailAndClearsUnusedSlots) {
  SyncEvent event;
  event.ancestorCount = MAX_ANCESTORS;
  for (auto& ancestor : event.ancestors) ancestor = {identity(1), 1, 2};
  std::array<uint8_t, MAX_RECORD_SIZE> bytes{};
  ASSERT_EQ(encodeRecord(event, bytes), MAX_RECORD_SIZE);
  SyncEvent output;
  ASSERT_TRUE(decodeRecord(bytes, output));
  EXPECT_EQ(output, event);
  event.ancestorCount = 0;
  ASSERT_EQ(encodeRecord(event, bytes), SYNC_EVENT_BASE_SIZE);
  ASSERT_TRUE(decodeRecord(std::span(bytes).first(SYNC_EVENT_BASE_SIZE), output));
  for (const auto& ancestor : output.ancestors) EXPECT_EQ(ancestor, EventIdentity{});
}

TEST(CompanionRecords, SupportsAllReleaseBoardClasses) {
  std::array<uint8_t, DEVICE_DESCRIPTOR_SIZE> bytes{};
  for (Board board : {Board::X4, Board::Sticky, Board::X4Pro, Board::X4Classic, Board::PaperMono}) {
    DeviceDescriptor descriptor;
    descriptor.board = board;
    ASSERT_EQ(encodeRecord(descriptor, bytes), bytes.size());
    DeviceDescriptor decoded;
    ASSERT_TRUE(decodeRecord(bytes, decoded));
    EXPECT_EQ(decoded.board, board);
  }
}

TEST(CompanionRecords, TransferDeclarationBindsCourseIdentity) {
  const auto bytes = fixture("TransferDeclaration");
  ASSERT_EQ(bytes.size(), TRANSFER_DECLARATION_SIZE);
  TransferDeclaration declaration;
  ASSERT_TRUE(decodeTransferDeclaration(bytes, declaration));
  EXPECT_EQ(declaration.manifest.kind, ContentKind::Course);
  EXPECT_EQ(declaration.manifest.length, 2000);
  EXPECT_EQ(declaration.manifest.formatVersion, 1);
  EXPECT_EQ(declaration.manifest.logicalIdentity.front(), 7);
  std::array<uint8_t, TRANSFER_DECLARATION_SIZE> encoded{};
  ASSERT_EQ(encodeTransferDeclaration(declaration, encoded), encoded.size());
  EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), encoded.begin()));
  for (size_t length = 0; length < bytes.size(); ++length) {
    auto retained = declaration;
    EXPECT_FALSE(decodeTransferDeclaration(std::span(bytes).first(length), retained));
    EXPECT_EQ(retained, declaration);
  }
  auto extra = bytes;
  extra.push_back(0);
  EXPECT_FALSE(decodeTransferDeclaration(extra, declaration));
  for (const size_t offset : {size_t{2}, size_t{35}, size_t{43}, size_t{145}, size_t{153}, size_t{161}}) {
    auto malformed = bytes;
    malformed[offset] ^= 1;
    auto retained = declaration;
    EXPECT_FALSE(decodeTransferDeclaration(malformed, retained));
    EXPECT_EQ(retained, declaration);
  }
  auto invalid = declaration;
  invalid.manifest.logicalIdentity.fill(0);
  EXPECT_EQ(encodeTransferDeclaration(invalid, encoded), 0);
  invalid = declaration;
  invalid.state.durableOffset = 1;
  EXPECT_EQ(encodeTransferDeclaration(invalid, encoded), 0);
  EXPECT_EQ(encodeTransferDeclaration(declaration, std::span(encoded).first(encoded.size() - 1)), 0);
}

TEST(CompanionRecords, CourseBindingFixture) {
  const auto bytes = fixture("CourseBinding");
  ASSERT_EQ(bytes.size(), COURSE_BINDING_SIZE);
  ContentManifest decoded;
  ASSERT_TRUE(decodeCourseBinding(bytes, decoded));
  EXPECT_EQ(decoded.kind, ContentKind::Course);
  EXPECT_EQ(decoded.length, 2000);
  EXPECT_EQ(decoded.logicalIdentity.front(), 7);
  std::array<uint8_t, COURSE_BINDING_SIZE> encoded{};
  ASSERT_EQ(encodeCourseBinding(decoded, encoded), encoded.size());
  EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), encoded.begin()));
}

TEST(CompanionRecords, DeclaredBeginTransferFixtureAndMalformedPayloads) {
  const auto bytes = fixture("DeclaredBeginTransfer");
  ASSERT_FALSE(bytes.empty());
  DeclaredBeginTransferCommand decoded;
  ASSERT_TRUE(decodeDeclaredBeginTransfer(bytes, decoded));
  EXPECT_EQ(decoded.destination, "/tinta/course.pack");
  EXPECT_EQ(decoded.declaration.manifest.kind, ContentKind::Course);
  EXPECT_EQ(decoded.declaration.manifest.length, 2000);
  EXPECT_EQ(decoded.declaration.manifest.logicalIdentity.front(), 7);
  const auto saved = decoded;
  for (size_t size = 0; size < bytes.size(); ++size) {
    EXPECT_FALSE(decodeDeclaredBeginTransfer(std::span(bytes).first(size), decoded));
    EXPECT_EQ(decoded.declaration, saved.declaration);
    EXPECT_EQ(decoded.destination, saved.destination);
  }
  auto extra = bytes;
  extra.push_back(0);
  EXPECT_FALSE(decodeDeclaredBeginTransfer(extra, decoded));
  for (const size_t offset : {size_t{0}, size_t{1}, size_t{35}, size_t{43}, size_t{145}, size_t{153}, size_t{161},
                              size_t{162}, size_t{163}}) {
    auto malformed = bytes;
    malformed[offset] = offset == 153 ? 1 : 0;
    EXPECT_FALSE(decodeDeclaredBeginTransfer(malformed, decoded)) << offset;
    EXPECT_EQ(decoded.declaration, saved.declaration);
    EXPECT_EQ(decoded.destination, saved.destination);
  }
  auto malformed = bytes;
  malformed.back() = 127;
  EXPECT_FALSE(decodeDeclaredBeginTransfer(malformed, decoded));
}

TEST(CompanionRecords, CourseTransferCapabilitiesFixture) {
  DeviceDescriptor descriptor;
  ASSERT_TRUE(decodeRecord(fixture("CourseTransferCapabilities"), descriptor));
  EXPECT_EQ(descriptor.capabilities, CAPABILITY_DECLARED_TRANSFERS | CAPABILITY_COURSE_TRANSFERS);
  EXPECT_TRUE(supportsCourseTransfer(descriptor.capabilities));
  EXPECT_TRUE(supportsCourseTransfer(0xffffffff));
  for (const uint32_t capabilities : {uint32_t{0}, uint32_t{1}, uint32_t{2}, uint32_t{4}, uint32_t{0xfffffffc}})
    EXPECT_FALSE(supportsCourseTransfer(capabilities));
}

TEST(PreferenceBody, ValidatesTypedValuesAndPreservesOutputOnFailure) {
  PreferenceBodyView value;
  const std::array<uint8_t, 8> negative{1, 4, 7, 1, 254, 255, 255, 255};
  ASSERT_TRUE(decodePreferenceBody(negative, value));
  EXPECT_EQ(value.integer, -2);
  const std::array<uint8_t, 10> language{1, 4, 10, 2, 5, 'e', 's', '-', 'M', 'X'};
  ASSERT_TRUE(decodePreferenceBody(language, value));
  EXPECT_EQ(value.text.data(), language.data() + 5);
  auto invalid = language;
  invalid[5] = '1';
  EXPECT_FALSE(decodePreferenceBody(invalid, value));
  EXPECT_EQ(value.text.data(), language.data() + 5);
  for (size_t length = 0; length < language.size(); ++length)
    EXPECT_FALSE(decodePreferenceBody(std::span(language).first(length), value));
  std::array<uint8_t, 40> content{1, 4, 11, 3, 1};
  content[5] = 1;
  content[37] = 2;
  content[38] = 0xc3;
  content[39] = 0xb1;
  ASSERT_TRUE(decodePreferenceBody(content, value));
  EXPECT_TRUE(value.hasContent);
  EXPECT_EQ(value.contentHash.data(), content.data() + 5);
  content[39] = '/';
  EXPECT_FALSE(decodePreferenceBody(content, value));
  content[38] = '.';
  content[39] = '.';
  EXPECT_FALSE(decodePreferenceBody(content, value));
  const std::array<uint8_t, 5> none{1, 4, 11, 3, 0};
  ASSERT_TRUE(decodePreferenceBody(none, value));
  EXPECT_FALSE(value.hasContent);
  for (unsigned key = 0; key < 256; ++key) {
    auto bytes = negative;
    bytes[2] = key;
    EXPECT_EQ(decodePreferenceBody(bytes, value), key == 7);
  }
}

TEST(PreferenceBody, SharedAppleReaderBoundaryVectors) {
  const auto bytes = fixture("PreferenceBodies-v1");
  ASSERT_GE(bytes.size(), 2u);
  const auto count = bytes[0] | (static_cast<unsigned>(bytes[1]) << 8);
  size_t at = 2;
  for (unsigned index = 0; index < count; ++index) {
    SCOPED_TRACE(index);
    ASSERT_GE(bytes.size() - at, 3u);
    const bool accepted = bytes[at++] == 1;
    const auto length = bytes[at] | (static_cast<size_t>(bytes[at + 1]) << 8);
    at += 2;
    ASSERT_LE(length, bytes.size() - at);
    PreferenceBodyView output;
    output.key = 255;
    EXPECT_EQ(decodePreferenceBody(std::span(bytes).subspan(at, length), output), accepted);
    if (!accepted) {
      EXPECT_EQ(output.key, 255);
    }
    at += length;
  }
  EXPECT_EQ(at, bytes.size());
}

TEST(CompanionRecords, ReadingAndBookmarkBodiesValidateBoundsAndPreserveOutput) {
  const std::array<uint8_t, 8> anchorBytes{1, 1, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  ReadingAnchor anchor;
  ASSERT_TRUE(decodeReadingAnchor(anchorBytes, anchor));
  EXPECT_EQ(anchor.spine, UINT16_MAX);
  EXPECT_EQ(anchor.visibleTextOffset, UINT32_MAX);
  EXPECT_FALSE(decodeReadingAnchor(std::span(anchorBytes).first(7), anchor));
  EXPECT_EQ(anchor.visibleTextOffset, UINT32_MAX);
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> bytes{};
  bytes[0] = 1;
  bytes[1] = 2;
  bytes[2] = 9;
  std::copy(anchorBytes.begin() + 2, anchorBytes.end(), bytes.begin() + 18);
  bytes[24] = 128;
  std::fill(bytes.begin() + 26, bytes.begin() + 154, 'a');
  bytes[155] = 2;
  std::fill(bytes.begin() + 156, bytes.end(), 'b');
  BookmarkBodyView bookmark;
  ASSERT_TRUE(decodeBookmarkBody(bytes, bookmark));
  EXPECT_EQ(bookmark.anchor, anchor);
  EXPECT_EQ(bookmark.name.size(), 128u);
  EXPECT_EQ(bookmark.summary.size(), 512u);
  EXPECT_EQ(bookmark.summary.data(), bytes.data() + 156);
  for (size_t length = 0; length < bytes.size(); ++length)
    EXPECT_FALSE(decodeBookmarkBody(std::span(bytes).first(length), bookmark));
  bytes[26] = 0;
  EXPECT_FALSE(decodeBookmarkBody(bytes, bookmark));
  EXPECT_EQ(bookmark.name.size(), 128u);
  bytes[26] = 0xc0;
  EXPECT_FALSE(decodeBookmarkBody(bytes, bookmark));
  bytes[26] = 'a';
  bytes[24] = 129;
  EXPECT_FALSE(decodeBookmarkBody(bytes, bookmark));
  bytes[1] = 3;
  ASSERT_TRUE(decodeBookmarkBody(std::span(bytes).first(18), bookmark));
  EXPECT_TRUE(bookmark.deleted);
  EXPECT_TRUE(bookmark.name.empty());
  EXPECT_FALSE(decodeBookmarkBody(std::span(bytes).first(19), bookmark));
  bytes[2] = 0;
  EXPECT_FALSE(decodeBookmarkBody(std::span(bytes).first(18), bookmark));
}

TEST(CompanionRecords, ContentRemovalSharedFixtureAndExactFraming) {
  const auto bytes = fixture("ContentRemovalRequest");
  ASSERT_EQ(bytes.size(), CONTENT_REMOVAL_REQUEST_SIZE);
  ContentRemovalRequest request;
  ASSERT_TRUE(decodeContentRemovalRequest(bytes, request));
  EXPECT_EQ(request.manifest.kind, ContentKind::Dictionary);
  EXPECT_EQ(request.manifest.length, 1234);
  std::array<uint8_t, CONTENT_REMOVAL_REQUEST_SIZE + 2> encoded{};
  encoded.front() = 0xAB;
  encoded.back() = 0xCD;
  ASSERT_EQ(encodeContentRemovalRequest(request, std::span(encoded).subspan(1, CONTENT_REMOVAL_REQUEST_SIZE)),
            CONTENT_REMOVAL_REQUEST_SIZE);
  EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), encoded.begin() + 1));
  EXPECT_EQ(encoded.front(), 0xAB);
  EXPECT_EQ(encoded.back(), 0xCD);
  const auto saved = request;
  for (size_t size = 0; size < bytes.size(); ++size) {
    EXPECT_FALSE(decodeContentRemovalRequest(std::span(bytes).first(size), request));
    EXPECT_EQ(request, saved);
  }
  auto trailing = bytes;
  trailing.push_back(0);
  EXPECT_FALSE(decodeContentRemovalRequest(trailing, request));
  EXPECT_EQ(request, saved);
}
TEST(CompanionRecords, ContentRemovalRejectsUnboundIdentityAndUnsupportedManifest) {
  auto bytes = fixture("ContentRemovalRequest");
  ContentRemovalRequest request;
  ASSERT_TRUE(decodeContentRemovalRequest(bytes, request));
  const auto saved = request;
  for (size_t start : {4U, 20U, 36U, 54U}) {
    auto invalid = bytes;
    std::fill_n(invalid.begin() + start, start == 54 ? 32 : 16, 0);
    EXPECT_FALSE(decodeContentRemovalRequest(invalid, request));
    EXPECT_EQ(request, saved);
  }
  for (const auto kind : {ContentKind::Firmware, static_cast<ContentKind>(0), static_cast<ContentKind>(6)}) {
    request = saved;
    request.manifest.kind = kind;
    EXPECT_FALSE(validContentRemovalRequest(request));
  }
  for (const auto kind : {ContentKind::Epub, ContentKind::Course, ContentKind::Font, ContentKind::Dictionary}) {
    request = saved;
    request.manifest.kind = kind;
    request.manifest.formatVersion = kind == ContentKind::Epub ? 0 : 1;
    request.manifest.logicalIdentity.fill(kind == ContentKind::Course ? 9 : 0);
    EXPECT_TRUE(validContentRemovalRequest(request));
    request.manifest.formatVersion = 99;
    EXPECT_FALSE(validContentRemovalRequest(request));
    request.manifest.formatVersion = kind == ContentKind::Epub ? 0 : 1;
    request.manifest.logicalIdentity.fill(kind == ContentKind::Course ? 0 : 9);
    EXPECT_FALSE(validContentRemovalRequest(request));
  }
}

TEST(CompanionRecords, RemovalReplyMatchesSharedFixture) {
  class CompletedBackend final : public ContentRemovalBackend {
   public:
    ContentRemovalResult completed(const ContentRemovalRequest&) override { return ContentRemovalResult::Ok; }
    ContentRemovalResult remove(const ContentRemovalRequest&) override { return ContentRemovalResult::IoError; }
  } backend;
  ContentRemovalRequest request;
  ASSERT_TRUE(decodeContentRemovalRequest(fixture("ContentRemovalRequest"), request));
  ContentRemovalHandler handler(backend);
  std::array<uint8_t, CONTENT_REMOVAL_REPLY_SIZE> bytes{};
  EXPECT_EQ(handler.handle(true, request.owner, request.generation, fixture("ContentRemovalRequest"), bytes),
            bytes.size());
  const auto golden = fixture("ContentRemovalReply");
  ASSERT_EQ(golden.size(), bytes.size());
  EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), golden.begin()));
}
