#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "lib/Companion/CompanionInventoryIndex.h"
using namespace companion;
namespace {
class Storage final : public InventoryIndexStorage {
 public:
  std::vector<uint8_t> bytes;
  bool fail = false;
  bool size(uint64_t& result) override {
    result = bytes.size();
    return !fail;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    if (fail || offset > bytes.size() || output.size() > bytes.size() - offset) return false;
    std::copy_n(bytes.begin() + offset, output.size(), output.begin());
    return true;
  }
};
std::vector<uint8_t> fixture() {
  std::ifstream file(std::string(COMPANION_FIXTURE_DIR) + "/InventoryIndex.json");
  const std::string json{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  const size_t start = json.find("\"binaryHex\": \"") + 14;
  const size_t end = json.find('"', start);
  std::vector<uint8_t> bytes;
  bytes.reserve((end - start) / 2);
  for (size_t i = start; i < end; i += 2)
    bytes.push_back(static_cast<uint8_t>(std::stoul(json.substr(i, 2), nullptr, 16)));
  return bytes;
}
}  // namespace
TEST(CompanionInventoryIndex, SharedFixturePagesAndReadCorruption) {
  Storage storage;
  storage.bytes = fixture();
  std::array<uint8_t, INVENTORY_INDEX_ENTRY_SIZE> scratch{};
  IndexedInventoryCatalog catalog(storage, scratch);
  Identity generation{};
  generation.fill(2);
  ASSERT_TRUE(catalog.open(generation));
  EXPECT_EQ(catalog.count(), 1);
  EXPECT_EQ(catalog.revision(), 1);
  ContentManifest manifest;
  ASSERT_TRUE(catalog.read(0, manifest));
  EXPECT_EQ(manifest.kind, ContentKind::Course);
  InventoryIndexHeader header;
  ASSERT_TRUE(decodeInventoryIndexHeader(std::span(storage.bytes).first(INVENTORY_INDEX_HEADER_SIZE), header));
  std::array<uint8_t, INVENTORY_INDEX_HEADER_SIZE> encodedHeader{};
  ASSERT_EQ(encodeInventoryIndexHeader(header, encodedHeader), encodedHeader.size());
  EXPECT_TRUE(std::equal(encodedHeader.begin(), encodedHeader.end(), storage.bytes.begin()));
  ASSERT_EQ(encodeInventoryIndexEntry(manifest, scratch), scratch.size());
  EXPECT_TRUE(std::equal(scratch.begin(), scratch.end(), storage.bytes.begin() + INVENTORY_INDEX_HEADER_SIZE));
  std::array<uint8_t, INVENTORY_REQUEST_SIZE> request{};
  ASSERT_EQ(encodeInventoryRequest({generation, 0, 0, 8}, request), request.size());
  std::array<uint8_t, MAX_INVENTORY_PAGE_SIZE + 1> response{};
  ASSERT_EQ(handleInventory(catalog, true, request, response), 1 + INVENTORY_PAGE_HEADER_SIZE + CONTENT_MANIFEST_SIZE);
  EXPECT_EQ(response[0], 0);
  storage.bytes[INVENTORY_INDEX_HEADER_SIZE + 4] ^= 1;
  EXPECT_FALSE(catalog.read(0, manifest));
  EXPECT_EQ(catalog.revision(), 0);
  EXPECT_EQ(catalog.count(), 0);
}
TEST(CompanionInventoryIndex, RejectsTornCorruptWrongCardAndUnsortedSnapshots) {
  const auto original = fixture();
  Storage storage;
  std::array<uint8_t, INVENTORY_INDEX_ENTRY_SIZE> scratch{};
  IndexedInventoryCatalog catalog(storage, scratch);
  Identity generation{};
  generation.fill(2);
  for (size_t size = 0; size < original.size(); ++size) {
    storage.bytes.assign(original.begin(), original.begin() + size);
    EXPECT_FALSE(catalog.open(generation));
    EXPECT_EQ(catalog.revision(), 0);
  }
  storage.bytes = original;
  Identity wrong{};
  wrong.fill(3);
  EXPECT_FALSE(catalog.open(wrong));
  storage.bytes[44] ^= 1;
  EXPECT_FALSE(catalog.open(generation));
  storage.bytes = original;
  InventoryIndexHeader header;
  ASSERT_TRUE(decodeInventoryIndexHeader(std::span(original).first(INVENTORY_INDEX_HEADER_SIZE), header));
  const auto entry = std::span(original).subspan(INVENTORY_INDEX_HEADER_SIZE);
  storage.bytes.insert(storage.bytes.end(), entry.begin(), entry.end());
  header.count = 2;
  header.entriesCrc = inventoryIndexCrc(std::span(storage.bytes).subspan(INVENTORY_INDEX_HEADER_SIZE));
  ASSERT_EQ(encodeInventoryIndexHeader(header, storage.bytes), INVENTORY_INDEX_HEADER_SIZE);
  EXPECT_FALSE(catalog.open(generation));
  storage.bytes = original;
  storage.fail = true;
  EXPECT_FALSE(catalog.open(generation));
}
TEST(CompanionInventoryIndex, AllContentKindsAndHeaderOverflow) {
  std::array<uint8_t, INVENTORY_INDEX_ENTRY_SIZE> encoded{};
  ContentManifest manifest;
  manifest.contentHash.fill(7);
  for (uint8_t kind = 1; kind <= 5; ++kind) {
    manifest.kind = static_cast<ContentKind>(kind);
    ASSERT_EQ(encodeInventoryIndexEntry(manifest, encoded), encoded.size());
    ContentManifest decoded;
    ASSERT_TRUE(decodeInventoryIndexEntry(encoded, decoded));
    EXPECT_EQ(decoded, manifest);
  }
  InventoryIndexHeader header;
  header.generation.fill(2);
  header.revision = 1;
  header.count = std::numeric_limits<uint64_t>::max();
  encoded.fill(0xa5);
  EXPECT_EQ(encodeInventoryIndexHeader(header, encoded), 0);
  EXPECT_TRUE(std::all_of(encoded.begin(), encoded.end(), [](uint8_t byte) { return byte == 0xa5; }));
}
