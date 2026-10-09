#include <HalMemory.h>
#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <optional>

#include "lib/hal/HalContentReadNativeOwner.h"
#include "lib/hal/HalContentReadSession.h"
using namespace companion;
namespace {
class PathStorage final : public InventoryIndexStorage {
 public:
  std::vector<uint8_t> bytes;
  bool size(uint64_t& size) override {
    size = bytes.size();
    return true;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    if (offset > bytes.size() || output.size() > bytes.size() - offset) return false;
    std::copy_n(bytes.begin() + offset, output.size(), output.begin());
    return true;
  }
};
}  // namespace
class HalContentReadSessionTest : public testing::Test {
 protected:
  ContentReadRequest request;
  Identity installation{};
  PathStorage storage;
  std::array<uint8_t, 1024> scratch{}, output{};
  std::array<uint8_t, CONTENT_READ_REQUEST_SIZE> wire{};
  std::optional<InventoryPaths> paths;
  void SetUp() override {
    inventory_hal_test::state = {};
    companion_memory_test::internal = {1024 * 1024, 1024 * 1024, 1024 * 1024, 1024 * 1024};
    request.generation.fill(1);
    installation.fill(2);
    request.manifest.length = 2003;
    request.manifest.formatVersion = 1;
    request.maximumBytes = MAX_CONTENT_READ_BYTES;
    auto& bytes = inventory_hal_test::state.files["/reader-file"];
    bytes.resize(request.manifest.length);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = i % 251;
    ASSERT_EQ(
        EVP_Digest(bytes.data(), bytes.size(), request.manifest.contentHash.data(), nullptr, EVP_sha256(), nullptr), 1);
    const size_t record = encodeInventoryPath(request.manifest, "/reader-file", scratch);
    ASSERT_GT(record, 0U);
    storage.bytes.reserve(INVENTORY_INDEX_HEADER_SIZE + record);
    storage.bytes.resize(INVENTORY_INDEX_HEADER_SIZE + record);
    std::copy_n(scratch.begin(), record, storage.bytes.begin() + INVENTORY_INDEX_HEADER_SIZE);
    InventoryIndexHeader header{request.generation, 1, 1, inventoryIndexCrc(std::span(scratch).first(record))};
    ASSERT_EQ(encodeInventoryPathsHeader(header, storage.bytes), INVENTORY_INDEX_HEADER_SIZE);
    paths.emplace(storage, scratch);
    ASSERT_TRUE(paths->open(request.generation, 1));
    ASSERT_EQ(encodeContentReadRequest(request, wire), wire.size());
  }
  ContentReadResult perform(HalContentReadSession& session, bool authorized = true, bool busy = false,
                            const Identity* generation = nullptr, uint64_t revision = 1) {
    EXPECT_EQ(encodeContentReadRequest(request, wire), wire.size());
    const size_t length = session.reply(authorized, installation, generation ? *generation : request.generation,
                                        revision, busy, *paths, wire, scratch, output);
    ContentReadReplyView view;
    EXPECT_TRUE(decodeContentReadReply(std::span(output).first(length), request, view));
    return view.result;
  }
};
TEST_F(HalContentReadSessionTest, AuthenticatedChunksReuseHandleAndRevisionChangeReverifies) {
  HalContentReadSession session;
  ASSERT_EQ(perform(session), ContentReadResult::Ok);
  const auto reads = inventory_hal_test::state.reads, opens = inventory_hal_test::state.opens;
  request.offset = 961;
  ASSERT_EQ(perform(session), ContentReadResult::Ok);
  EXPECT_EQ(inventory_hal_test::state.reads, reads + 1);
  EXPECT_EQ(inventory_hal_test::state.opens, opens);
  InventoryIndexHeader header;
  ASSERT_TRUE(decodeInventoryPathsHeader(std::span(storage.bytes).first(INVENTORY_INDEX_HEADER_SIZE), header));
  header.revision = 2;
  ASSERT_EQ(encodeInventoryPathsHeader(header, storage.bytes), INVENTORY_INDEX_HEADER_SIZE);
  ASSERT_TRUE(paths->open(request.generation, 2));
  ASSERT_EQ(perform(session, true, false, nullptr, 2), ContentReadResult::Ok);
  EXPECT_EQ(inventory_hal_test::state.opens, opens + 1);
  request.offset = 1922;
  ASSERT_EQ(perform(session, true, false, nullptr, 2), ContentReadResult::Ok);
  EXPECT_EQ(output[63], uint8_t(1922 % 251));
}
TEST_F(HalContentReadSessionTest, UnauthorizedWrongCardBusyAndLowHeapPerformNoFileOpen) {
  HalContentReadSession session;
  EXPECT_EQ(perform(session, false), ContentReadResult::Unauthorized);
  Identity foreign{};
  foreign.fill(3);
  EXPECT_EQ(perform(session, true, false, &foreign), ContentReadResult::WrongStorage);
  EXPECT_EQ(perform(session, true, true), ContentReadResult::Busy);
  EXPECT_EQ(perform(session, true, false, nullptr, 0), ContentReadResult::Busy);
  EXPECT_EQ(perform(session, true, false, nullptr, 2), ContentReadResult::Corrupt);
  companion_memory_test::internal.freeBytes = 50 * 1024;
  EXPECT_EQ(perform(session), ContentReadResult::Busy);
  EXPECT_EQ(inventory_hal_test::state.opens, 0U);
}
TEST_F(HalContentReadSessionTest, MissingManifestConflictingManifestAndCorruptBytesNeverReturnFileData) {
  HalContentReadSession session;
  request.manifest.contentHash[0] ^= 1;
  EXPECT_EQ(perform(session), ContentReadResult::NotFound);
  request.manifest.contentHash[0] ^= 1;
  ++request.manifest.length;
  EXPECT_EQ(perform(session), ContentReadResult::Corrupt);
  --request.manifest.length;
  inventory_hal_test::state.files["/reader-file"][100] ^= 1;
  EXPECT_EQ(perform(session), ContentReadResult::Corrupt);
}
TEST_F(HalContentReadSessionTest, InvalidRequestAndSmallOutputDoNotOpenFileOrMutateOutput) {
  HalContentReadSession session;
  output.fill(0xa5);
  const auto before = output;
  EXPECT_EQ(session.reply(true, installation, request.generation, 1, false, *paths, std::span(wire).first(92), scratch,
                          output),
            0U);
  EXPECT_EQ(output, before);
  EXPECT_EQ(session.reply(true, installation, request.generation, 1, false, *paths, wire, scratch,
                          std::span(output).first(63)),
            0U);
  EXPECT_EQ(output, before);
  EXPECT_EQ(inventory_hal_test::state.opens, 0U);
}

TEST_F(HalContentReadSessionTest, NativeOwnerValidatesInventoryPairAndClosesReadersBeforeMutation) {
  auto& state = inventory_hal_test::state;
  state.files[InventoryPublication::PATHS] = storage.bytes;
  std::array<uint8_t, INVENTORY_INDEX_ENTRY_SIZE> entry{};
  ASSERT_EQ(encodeInventoryIndexEntry(request.manifest, entry), entry.size());
  auto& index = state.files[InventoryPublication::INDEX];
  index.resize(INVENTORY_INDEX_HEADER_SIZE + entry.size());
  InventoryIndexHeader header{request.generation, 1, 1, inventoryIndexCrc(entry)};
  ASSERT_EQ(encodeInventoryIndexHeader(header, index), INVENTORY_INDEX_HEADER_SIZE);
  std::copy(entry.begin(), entry.end(), index.begin() + INVENTORY_INDEX_HEADER_SIZE);
  HalContentReadNativeOwner owner(scratch, nullptr, nullptr);
  ASSERT_TRUE(owner.prepare());
  ASSERT_TRUE(owner.open(request.generation, 1));
  ASSERT_EQ(owner.reply(installation, request.generation, 1, wire, output), 1024U);
  const auto opens = state.opens;
  request.offset = 961;
  ASSERT_EQ(encodeContentReadRequest(request, wire), wire.size());
  ASSERT_TRUE(owner.open(request.generation, 1));
  ASSERT_EQ(owner.reply(installation, request.generation, 1, wire, output), 1024U);
  EXPECT_EQ(state.opens, opens);
  state.failClosePath = "/reader-file";
  EXPECT_FALSE(owner.closeReaders());
  state.failClosePath.clear();
  ASSERT_TRUE(owner.closeReaders());
  state.files[InventoryPublication::PATHS][INVENTORY_INDEX_HEADER_SIZE + 3] ^= 1;
  EXPECT_FALSE(owner.open(request.generation, 1));
}

#include "lib/hal/HalContentMetadataLookup.h"

TEST_F(HalContentReadSessionTest, MetadataReturnsValidatedBasenameWithoutOpeningContent) {
  const size_t record = encodeInventoryPath(request.manifest, "/books/読書.epub", scratch);
  storage.bytes.resize(INVENTORY_INDEX_HEADER_SIZE + record);
  std::copy_n(scratch.begin(), record, storage.bytes.begin() + INVENTORY_INDEX_HEADER_SIZE);
  InventoryIndexHeader header{request.generation, 1, 1, inventoryIndexCrc(std::span(scratch).first(record))};
  ASSERT_EQ(encodeInventoryPathsHeader(header, storage.bytes), INVENTORY_INDEX_HEADER_SIZE);
  ASSERT_TRUE(paths->open(request.generation, 1));
  ContentMetadataRequest metadata{request.generation, request.manifest};
  std::array<char, INVENTORY_PATH_LIMIT + 1> path{};
  const auto size = contentMetadataLookup(metadata, true, request.generation, 1, false, *paths, path, output);
  ASSERT_EQ(size, CONTENT_METADATA_REPLY_HEADER_SIZE + std::string_view("読書.epub").size());
  EXPECT_EQ(output[4], static_cast<uint8_t>(ContentReadResult::Ok));
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(output.data() + 85), size - 85), "読書.epub");
  EXPECT_EQ(inventory_hal_test::state.opens, 0U);
  EXPECT_EQ(contentMetadataLookup(metadata, false, request.generation, 1, false, *paths, path, output), 85U);
  EXPECT_EQ(output[4], static_cast<uint8_t>(ContentReadResult::Unauthorized));
  EXPECT_EQ(contentMetadataLookup(metadata, true, request.generation, 2, false, *paths, path, output), 85U);
  EXPECT_EQ(output[4], static_cast<uint8_t>(ContentReadResult::Corrupt));
  companion_memory_test::internal.freeBytes = 40 * 1024;
  EXPECT_EQ(contentMetadataLookup(metadata, true, request.generation, 1, false, *paths, path, output), 85U);
  EXPECT_EQ(output[4], static_cast<uint8_t>(ContentReadResult::Busy));
}

#include "lib/hal/HalContentExportAdmission.h"

TEST_F(HalContentReadSessionTest, ExportAdmissionHashesActualSourceBeforeBindingTransaction) {
  auto& bytes = inventory_hal_test::state.files["/reader-file"];
  bytes.resize(2 * 1024 * 1024, 17);
  request.manifest.length = bytes.size();
  ASSERT_EQ(EVP_Digest(bytes.data(), bytes.size(), request.manifest.contentHash.data(), nullptr, EVP_sha256(), nullptr),
            1);
  const auto record = encodeInventoryPath(request.manifest, "/reader-file", scratch);
  storage.bytes.resize(INVENTORY_INDEX_HEADER_SIZE + record);
  std::copy_n(scratch.begin(), record, storage.bytes.begin() + INVENTORY_INDEX_HEADER_SIZE);
  InventoryIndexHeader header{request.generation, 1, 1, inventoryIndexCrc(std::span(scratch).first(record))};
  ASSERT_EQ(encodeInventoryPathsHeader(header, storage.bytes), INVENTORY_INDEX_HEADER_SIZE);
  ASSERT_TRUE(paths->open(request.generation, 1));
  ContentHandoffRequest admission;
  admission.transaction.fill(7);
  admission.read = request;
  admission.read.offset = 17;
  admission.read.maximumBytes = 1;
  HalContentReadSession session;
  ContentExportBinding binding;
  ASSERT_EQ(admitContentExport(binding, admission, false, installation, request.generation, 1, false, session, *paths,
                               scratch, output),
            77U);
  EXPECT_EQ(output[4], static_cast<uint8_t>(ContentReadResult::Unauthorized));
  EXPECT_EQ(inventory_hal_test::state.opens, 0U);
  bytes[1234] ^= 1;
  ASSERT_EQ(admitContentExport(binding, admission, true, installation, request.generation, 1, false, session, *paths,
                               scratch, output),
            77U);
  EXPECT_EQ(output[4], static_cast<uint8_t>(ContentReadResult::Corrupt));
  EXPECT_FALSE(binding.boundTo(admission.transaction, installation, request.generation, 1));
  bytes[1234] ^= 1;
  ASSERT_EQ(admitContentExport(binding, admission, true, installation, request.generation, 1, false, session, *paths,
                               scratch, output),
            77U);
  EXPECT_EQ(output[4], static_cast<uint8_t>(ContentReadResult::Ok));
  EXPECT_TRUE(binding.boundTo(admission.transaction, installation, request.generation, 1));
  const auto reads = inventory_hal_test::state.reads;
  ASSERT_EQ(admitContentExport(binding, admission, true, installation, request.generation, 1, false, session, *paths,
                               scratch, output),
            77U);
  EXPECT_EQ(output[4], static_cast<uint8_t>(ContentReadResult::Ok));
  EXPECT_EQ(inventory_hal_test::state.reads, reads + 1);
  ASSERT_TRUE(session.reset());

  auto& state = inventory_hal_test::state;
  state.files[InventoryPublication::PATHS] = storage.bytes;
  std::array<uint8_t, INVENTORY_INDEX_ENTRY_SIZE> entry{};
  ASSERT_EQ(encodeInventoryIndexEntry(request.manifest, entry), entry.size());
  auto& index = state.files[InventoryPublication::INDEX];
  index.resize(INVENTORY_INDEX_HEADER_SIZE + entry.size());
  InventoryIndexHeader indexHeader{request.generation, 1, 1, inventoryIndexCrc(entry)};
  ASSERT_EQ(encodeInventoryIndexHeader(indexHeader, index), INVENTORY_INDEX_HEADER_SIZE);
  std::copy(entry.begin(), entry.end(), index.begin() + INVENTORY_INDEX_HEADER_SIZE);
  HalContentReadNativeOwner owner(scratch, nullptr, nullptr);
  ASSERT_TRUE(owner.prepare());
  ASSERT_TRUE(owner.open(request.generation, 1));
  ASSERT_EQ(owner.admitExport(admission, installation, request.generation, 1, output), 77U);
  ASSERT_EQ(output[4], static_cast<uint8_t>(ContentReadResult::Ok));
  ASSERT_TRUE(owner.resetSource(true));
  EXPECT_TRUE(owner.exportBinding().boundTo(admission.transaction, installation, request.generation, 1));
  auto read = admission.read;
  read.maximumBytes = MAX_CONTENT_READ_BYTES;
  std::array<uint8_t, WIFI_CONTENT_READ_REQUEST_SIZE> wifiWire{};
  std::copy(admission.transaction.begin(), admission.transaction.end(), wifiWire.begin());
  ASSERT_EQ(encodeContentReadRequest(read, std::span(wifiWire).subspan(16)), CONTENT_READ_REQUEST_SIZE);
  ASSERT_EQ(owner.wifiReply(admission.transaction, installation, request.generation, 1, wifiWire, output), 1024U);
  EXPECT_EQ(output[4], static_cast<uint8_t>(ContentReadResult::Ok));
  EXPECT_EQ(output[63], bytes[17]);
  ASSERT_TRUE(owner.resetSource(true));
  bytes[1234] ^= 1;
  ASSERT_EQ(owner.wifiReply(admission.transaction, installation, request.generation, 1, wifiWire, output), 63U);
  EXPECT_EQ(output[4], static_cast<uint8_t>(ContentReadResult::Corrupt));
  ASSERT_TRUE(owner.resetSource());
  EXPECT_FALSE(owner.exportBinding().boundTo(admission.transaction, installation, request.generation, 1));
  const auto opens = state.opens;
  EXPECT_EQ(owner.wifiReply(admission.transaction, installation, request.generation, 1, wifiWire, output), 0U);
  EXPECT_EQ(state.opens, opens);
}
