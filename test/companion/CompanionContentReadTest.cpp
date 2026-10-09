#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "lib/Companion/CompanionContentRead.h"
using namespace companion;
namespace {
std::vector<uint8_t> fixture(const char* name) {
  std::ifstream file(std::string(COMPANION_FIXTURE_DIR) + "/" + name + ".json");
  const std::string json{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  const auto key = json.find("\"binaryHex\": \"");
  if (key == std::string::npos) return {};
  const auto start = key + 14, end = json.find('"', start);
  if (end == std::string::npos || (end - start) % 2) return {};
  std::vector<uint8_t> bytes;
  bytes.reserve((end - start) / 2);
  for (size_t at = start; at < end; at += 2)
    bytes.push_back(static_cast<uint8_t>(std::stoul(json.substr(at, 2), nullptr, 16)));
  return bytes;
}
ContentReadRequest request() {
  ContentReadRequest value;
  value.generation.fill(2);
  value.manifest.contentHash.fill(0x12);
  value.manifest.kind = ContentKind::Epub;
  value.manifest.length = 5;
  value.manifest.formatVersion = 1;
  value.maximumBytes = 3;
  return value;
}
}  // namespace
TEST(CompanionContentRead, SharedRequestAndReplyFixtures) {
  const auto wire = fixture("ReaderContentReadRequest");
  ASSERT_EQ(wire.size(), CONTENT_READ_REQUEST_SIZE);
  ContentReadRequest parsed;
  ASSERT_TRUE(decodeContentReadRequest(wire, parsed));
  EXPECT_EQ(parsed, request());
  std::array<uint8_t, CONTENT_READ_REQUEST_SIZE> encoded{};
  ASSERT_EQ(encodeContentReadRequest(parsed, encoded), encoded.size());
  EXPECT_TRUE(std::equal(encoded.begin(), encoded.end(), wire.begin()));
  const auto reply = fixture("ReaderContentReadReply");
  ContentReadReplyView view;
  ASSERT_TRUE(decodeContentReadReply(reply, parsed, view));
  EXPECT_EQ(view.result, ContentReadResult::Ok);
  ASSERT_EQ(view.bytes.size(), 3U);
  EXPECT_EQ(view.bytes.data(), reply.data() + CONTENT_READ_REPLY_HEADER_SIZE);
  std::array<uint8_t, CONTENT_READ_REPLY_HEADER_SIZE + 3> output{};
  ASSERT_EQ(encodeContentReadReply(parsed, ContentReadResult::Ok, view.bytes, output), output.size());
  EXPECT_TRUE(std::equal(output.begin(), output.end(), reply.begin()));
}
TEST(CompanionContentRead, RequestTruncationInvalidBoundsAndNoPartialOutput) {
  const auto wire = fixture("ReaderContentReadRequest");
  ASSERT_EQ(wire.size(), CONTENT_READ_REQUEST_SIZE);
  for (size_t count = 0; count < wire.size(); ++count) {
    auto output = request();
    output.offset = 1;
    const auto before = output;
    EXPECT_FALSE(decodeContentReadRequest(std::span(wire).first(count), output));
    EXPECT_EQ(output, before);
  }
  auto extra = wire;
  extra.push_back(0);
  ContentReadRequest parsed;
  EXPECT_FALSE(decodeContentReadRequest(extra, parsed));
  for (const uint16_t limit : {uint16_t{0}, uint16_t{962}, std::numeric_limits<uint16_t>::max()}) {
    auto invalid = request();
    invalid.maximumBytes = limit;
    std::array<uint8_t, 93> output;
    output.fill(0xa5);
    const auto before = output;
    EXPECT_EQ(encodeContentReadRequest(invalid, output), 0U);
    EXPECT_EQ(output, before);
  }
  auto invalid = request();
  invalid.offset = invalid.manifest.length;
  EXPECT_FALSE(validContentReadRequest(invalid));
  invalid = request();
  invalid.generation.fill(0);
  EXPECT_FALSE(validContentReadRequest(invalid));
  invalid = request();
  invalid.manifest.contentHash.fill(0);
  EXPECT_FALSE(validContentReadRequest(invalid));
}
TEST(CompanionContentRead, ContentKindFormatAndIdentityContracts) {
  auto value = request();
  value.manifest.kind = ContentKind::Course;
  EXPECT_FALSE(validContentReadRequest(value));
  value.manifest.logicalIdentity.fill(7);
  EXPECT_TRUE(validContentReadRequest(value));
  value.manifest.formatVersion = 2;
  EXPECT_FALSE(validContentReadRequest(value));
  value = request();
  value.manifest.kind = ContentKind::Font;
  for (uint32_t version : {1U, 4U}) {
    value.manifest.formatVersion = version;
    EXPECT_TRUE(validContentReadRequest(value));
  }
  for (uint32_t version : {0U, 2U, 3U, 5U}) {
    value.manifest.formatVersion = version;
    EXPECT_FALSE(validContentReadRequest(value));
  }
  value = request();
  value.manifest.kind = ContentKind::Dictionary;
  EXPECT_TRUE(validContentReadRequest(value));
  value.manifest.logicalIdentity.fill(7);
  EXPECT_FALSE(validContentReadRequest(value));
  value = request();
  value.manifest.kind = ContentKind::Firmware;
  EXPECT_FALSE(validContentReadRequest(value));
}
TEST(CompanionContentRead, ReplyBindingTruncationCountsAndUnchangedFailure) {
  const auto wire = fixture("ReaderContentReadReply");
  ASSERT_EQ(wire.size(), 66U);
  const auto value = request();
  for (size_t count = 0; count < wire.size(); ++count) {
    ContentReadReplyView view{ContentReadResult::Busy, {}};
    EXPECT_FALSE(decodeContentReadReply(std::span(wire).first(count), value, view));
    EXPECT_EQ(view.result, ContentReadResult::Busy);
  }
  for (size_t at : {size_t{3}, size_t{4}, size_t{5}, size_t{21}, size_t{53}, size_t{61}}) {
    auto bad = wire;
    bad[at] ^= 0xff;
    ContentReadReplyView view;
    EXPECT_FALSE(decodeContentReadReply(bad, value, view));
  }
  auto bad = wire;
  bad.push_back(0);
  ContentReadReplyView view;
  EXPECT_FALSE(decodeContentReadReply(bad, value, view));
  std::array<uint8_t, 66> output;
  output.fill(0xa5);
  const auto before = output;
  const std::array<uint8_t, 2> shortBytes{1, 2};
  EXPECT_EQ(encodeContentReadReply(value, ContentReadResult::Ok, shortBytes, output), 0U);
  EXPECT_EQ(output, before);
}
TEST(CompanionContentRead, ErrorsNeverContainFileBytesAndLastChunkIsExact) {
  auto value = request();
  const std::array<uint8_t, 1> byte{42};
  std::array<uint8_t, 66> output{};
  for (uint8_t raw = 1; raw <= 7; ++raw) {
    const auto result = static_cast<ContentReadResult>(raw);
    ASSERT_EQ(encodeContentReadReply(value, result, {}, output), CONTENT_READ_REPLY_HEADER_SIZE);
    ContentReadReplyView view;
    ASSERT_TRUE(decodeContentReadReply(std::span(output).first(63), value, view));
    EXPECT_EQ(view.result, result);
    EXPECT_TRUE(view.bytes.empty());
    const auto before = output;
    EXPECT_EQ(encodeContentReadReply(value, result, byte, output), 0U);
    EXPECT_EQ(output, before);
  }
  EXPECT_EQ(encodeContentReadReply(value, static_cast<ContentReadResult>(8), {}, output), 0U);
  value.offset = 4;
  ASSERT_EQ(encodeContentReadReply(value, ContentReadResult::Ok, byte, output), 64U);
  ContentReadReplyView view;
  EXPECT_TRUE(decodeContentReadReply(std::span(output).first(64), value, view));
}
TEST(CompanionContentRead, MaximumChunkAndOverlappingPayloadUseCallerWorkspace) {
  auto value = request();
  value.manifest.length = MAX_CONTENT_READ_BYTES;
  value.maximumBytes = MAX_CONTENT_READ_BYTES;
  std::array<uint8_t, MAX_CONTROL_PAYLOAD> output{};
  std::fill_n(output.begin(), MAX_CONTENT_READ_BYTES, 0x7e);
  ASSERT_EQ(
      encodeContentReadReply(value, ContentReadResult::Ok, std::span(output).first(MAX_CONTENT_READ_BYTES), output),
      output.size());
  ContentReadReplyView view;
  ASSERT_TRUE(decodeContentReadReply(output, value, view));
  ASSERT_EQ(view.bytes.size(), 961U);
  EXPECT_TRUE(std::all_of(view.bytes.begin(), view.bytes.end(), [](uint8_t byte) { return byte == 0x7e; }));
  ASSERT_EQ(encodeContentReadReply(value, ContentReadResult::Ok, view.bytes, output), output.size());
  EXPECT_TRUE(decodeContentReadReply(output, value, view));
}

#include "lib/Companion/CompanionContentMetadata.h"

TEST(CompanionContentMetadataTest, RequestRequiresExactLengthAndValidManifest) {
  const auto read = request();
  std::array<uint8_t, CONTENT_READ_REQUEST_SIZE> bytes{};
  ASSERT_EQ(encodeContentReadRequest(read, bytes), bytes.size());
  bytes[2] = 'M';
  ContentMetadataRequest metadata;
  ASSERT_TRUE(decodeContentMetadataRequest(std::span(bytes).first(83), metadata));
  EXPECT_EQ(metadata.generation, read.generation);
  EXPECT_EQ(metadata.manifest, read.manifest);
  for (size_t length = 0; length < 83; ++length)
    EXPECT_FALSE(decodeContentMetadataRequest(std::span(bytes).first(length), metadata));
  EXPECT_FALSE(decodeContentMetadataRequest(bytes, metadata));
}
TEST(CompanionContentMetadataTest, FilenameRejectsPathsControlsAndWrongFontFormat) {
  auto manifest = request().manifest;
  EXPECT_TRUE(validContentMetadataFilename(manifest, "読書.EPUB"));
  for (const auto name : {"", "../book.epub", "folder\\book.epub", "book\n.epub", "book.zip", "\xff.epub"})
    EXPECT_FALSE(validContentMetadataFilename(manifest, name));
  manifest.kind = ContentKind::Font;
  manifest.formatVersion = 4;
  EXPECT_TRUE(validContentMetadataFilename(manifest, "font.cpfont"));
  EXPECT_FALSE(validContentMetadataFilename(manifest, "font.ttf"));
  manifest.formatVersion = 1;
  EXPECT_TRUE(validContentMetadataFilename(manifest, "font.ttc"));
  EXPECT_FALSE(validContentMetadataFilename(manifest, "font.cpfont"));
}
TEST(CompanionContentMetadataTest, ReplyIsBoundedAndErrorsHaveNoFilename) {
  const auto read = request();
  ContentMetadataRequest metadata{read.generation, read.manifest};
  std::array<uint8_t, 340> output{};
  const auto size = encodeContentMetadataReply(metadata, ContentReadResult::Ok, "book.epub", output);
  ASSERT_EQ(size, 94);
  EXPECT_EQ(output[84], 9);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(output.data() + 85), 9), "book.epub");
  EXPECT_EQ(encodeContentMetadataReply(metadata, ContentReadResult::IoError, {}, output), 85);
  output.fill(42);
  const auto before = output;
  EXPECT_EQ(encodeContentMetadataReply(metadata, ContentReadResult::IoError, "book.epub", output), 0);
  EXPECT_EQ(output, before);
  EXPECT_EQ(encodeContentMetadataReply(metadata, ContentReadResult::Ok, "book.epub", std::span(output).first(93)), 0);
  EXPECT_EQ(output, before);
}

TEST(CompanionContentMetadataTest, SharedSwiftWireFixturesMatchReaderReply) {
  const auto input = fixture("ReaderContentMetadataRequest");
  ContentMetadataRequest metadata;
  ASSERT_TRUE(decodeContentMetadataRequest(input, metadata));
  EXPECT_EQ(metadata.generation, request().generation);
  EXPECT_EQ(metadata.manifest, request().manifest);
  std::array<uint8_t, 340> output{};
  const auto size = encodeContentMetadataReply(metadata, ContentReadResult::Ok, "読書.epub", output);
  const auto expected = fixture("ReaderContentMetadataReply");
  ASSERT_EQ(size, expected.size());
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), output.begin()));
}

#include "lib/Companion/CompanionContentHandoff.h"

TEST(CompanionContentHandoffTest, SharedRequestAndReplyBindTransactionManifestAndOffset) {
  const auto input = fixture("ReaderContentHandoffRequest");
  ContentHandoffRequest request;
  ASSERT_TRUE(decodeContentHandoffRequest(input, request));
  EXPECT_EQ(request.read.offset, 17U);
  EXPECT_EQ(request.read.manifest.length, 2 * 1024 * 1024U);
  EXPECT_EQ(request.transaction.front(), 3);
  std::array<uint8_t, CONTENT_HANDOFF_REPLY_SIZE> output{};
  ASSERT_EQ(encodeContentHandoffReply(request, ContentReadResult::Ok, output), output.size());
  const auto expected = fixture("ReaderContentHandoffReply");
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), output.begin(), output.end()));
  const auto before = request;
  for (size_t count = 0; count < input.size(); ++count) {
    EXPECT_FALSE(decodeContentHandoffRequest(std::span(input).first(count), request));
    EXPECT_EQ(request.transaction, before.transaction);
    EXPECT_EQ(request.read, before.read);
  }
  request.read.offset = 1024 * 1024;
  EXPECT_FALSE(validContentHandoffRequest(request));
  output.fill(42);
  EXPECT_EQ(encodeContentHandoffReply(request, ContentReadResult::Ok, output), 0U);
  EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](uint8_t value) { return value == 42; }));
}

#include "lib/Companion/CompanionContentExportBinding.h"

TEST(CompanionContentExportBindingTest, OnlyVerifiedSourceAdmitsAndRequestsStayBound) {
  ContentHandoffRequest request;
  ASSERT_TRUE(decodeContentHandoffRequest(fixture("ReaderContentHandoffRequest"), request));
  Identity owner{};
  owner.fill(7);
  ContentExportBinding binding;
  EXPECT_EQ(binding.admit(request, false, owner, request.read.generation, 1, ContentReadResult::Ok),
            ContentReadResult::Unauthorized);
  EXPECT_EQ(binding.admit(request, true, owner, request.read.generation, 1, ContentReadResult::Corrupt),
            ContentReadResult::Corrupt);
  EXPECT_FALSE(binding.boundTo(request.transaction, owner, request.read.generation, 1));
  ASSERT_EQ(binding.admit(request, true, owner, request.read.generation, 1, ContentReadResult::Ok),
            ContentReadResult::Ok);
  auto read = request.read;
  read.maximumBytes = MAX_CONTENT_READ_BYTES;
  EXPECT_TRUE(binding.permits(request.transaction, owner, 1, read));
  read.offset--;
  EXPECT_FALSE(binding.permits(request.transaction, owner, 1, read));
  read.offset += 2;
  EXPECT_TRUE(binding.permits(request.transaction, owner, 1, read));
  read.manifest.formatVersion = 0;
  EXPECT_FALSE(binding.permits(request.transaction, owner, 1, read));
  read = request.read;
  EXPECT_FALSE(binding.permits(request.transaction, owner, 2, read));
  auto foreign = owner;
  foreign[0] ^= 1;
  EXPECT_FALSE(binding.permits(request.transaction, foreign, 1, read));
  auto changed = request;
  changed.transaction[0] ^= 1;
  EXPECT_EQ(binding.admit(changed, true, owner, request.read.generation, 1, ContentReadResult::Ok),
            ContentReadResult::Busy);
  binding.reset();
  EXPECT_FALSE(binding.permits(request.transaction, owner, 1, read));
}

#include "lib/Companion/CompanionWifiContentRead.h"

TEST(CompanionWifiContentReadTest, EncryptedRequestCannotEscapeAdmittedSourceOrRevision) {
  ContentHandoffRequest admission;
  ASSERT_TRUE(decodeContentHandoffRequest(fixture("ReaderContentHandoffRequest"), admission));
  Identity owner{};
  owner.fill(7);
  ContentExportBinding binding;
  ASSERT_EQ(binding.admit(admission, true, owner, admission.read.generation, 9, ContentReadResult::Ok),
            ContentReadResult::Ok);
  auto read = admission.read;
  read.maximumBytes = MAX_CONTENT_READ_BYTES;
  std::array<uint8_t, WIFI_CONTENT_READ_REQUEST_SIZE> wire{};
  std::copy(admission.transaction.begin(), admission.transaction.end(), wire.begin());
  ASSERT_EQ(encodeContentReadRequest(read, std::span(wire).subspan(16)), CONTENT_READ_REQUEST_SIZE);
  ContentReadRequest output;
  ASSERT_TRUE(decodeWifiContentRead(wire, admission.transaction, owner, 9, binding, output));
  EXPECT_EQ(output, read);
  const auto before = output;
  for (size_t count = 0; count < wire.size(); ++count) {
    EXPECT_FALSE(decodeWifiContentRead(std::span(wire).first(count), admission.transaction, owner, 9, binding, output));
    EXPECT_EQ(output, before);
  }
  for (const size_t index : {size_t{0}, size_t{20}, size_t{36}}) {
    wire[index] ^= 1;
    EXPECT_FALSE(decodeWifiContentRead(wire, admission.transaction, owner, 9, binding, output));
    EXPECT_EQ(output, before);
    wire[index] ^= 1;
  }
  EXPECT_FALSE(decodeWifiContentRead(wire, admission.transaction, owner, 8, binding, output));
  owner[0] ^= 1;
  EXPECT_FALSE(decodeWifiContentRead(wire, admission.transaction, owner, 9, binding, output));
  EXPECT_EQ(output, before);
}
