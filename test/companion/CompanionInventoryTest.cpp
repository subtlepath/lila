#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "lib/Companion/CompanionInventory.h"
using namespace companion;
namespace {
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
TEST(CompanionInventory, SharedPageFixtureAndBorrowedEntries) {
  const auto bytes = fixture("InventoryPage");
  InventoryPageView page;
  ASSERT_TRUE(decodeInventoryPage(bytes, page));
  EXPECT_EQ(page.header.revision, 1U);
  EXPECT_TRUE(page.header.complete);
  ASSERT_EQ(page.count(), 1U);
  EXPECT_EQ(page.entries.data(), bytes.data() + INVENTORY_PAGE_HEADER_SIZE);
  ContentManifest entry;
  ASSERT_TRUE(decodeRecord(page.entries, entry));
  EXPECT_EQ(entry.kind, ContentKind::Course);
  EXPECT_EQ(entry.length, 123456U);
  std::vector<uint8_t> output(MAX_INVENTORY_PAGE_SIZE);
  EXPECT_EQ(encodeInventoryPage(page.header, std::span(&entry, 1), output), bytes.size());
  EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), output.begin()));
  for (size_t size = 0; size < bytes.size(); ++size) {
    const auto previous = page.entries.data();
    EXPECT_FALSE(decodeInventoryPage(std::span(bytes).first(size), page));
    EXPECT_EQ(page.entries.data(), previous);
  }
}
TEST(CompanionInventory, RequestBoundsAndPagination) {
  InventoryRequest request;
  request.storageGeneration.fill(2);
  std::array<uint8_t, INVENTORY_REQUEST_SIZE> bytes{};
  ASSERT_EQ(encodeInventoryRequest(request, bytes), bytes.size());
  InventoryRequest decoded;
  ASSERT_TRUE(decodeInventoryRequest(bytes, decoded));
  EXPECT_EQ(decoded, request);
  bytes[33] = 9;
  EXPECT_FALSE(decodeInventoryRequest(bytes, decoded));
  EXPECT_EQ(decoded, request);
  request.cursor = 1;
  EXPECT_EQ(encodeInventoryRequest(request, bytes), 0U);
  InventoryPageHeader header;
  header.storageGeneration.fill(2);
  header.revision = 1;
  header.nextCursor = 1;
  ContentManifest entry;
  std::vector<uint8_t> output(MAX_INVENTORY_PAGE_SIZE);
  ASSERT_EQ(encodeInventoryPage(header, std::span(&entry, 1), output), 106U);
  InventoryPageView view;
  ASSERT_TRUE(decodeInventoryPage(std::span(output).first(106), view));
  header.cursor = UINT64_MAX;
  header.complete = true;
  header.nextCursor = 0;
  EXPECT_EQ(encodeInventoryPage(header, std::span(&entry, 1), output), 0U);
  std::array<ContentManifest, 2> duplicate{};
  header.cursor = 0;
  EXPECT_EQ(encodeInventoryPage(header, duplicate, output), 0U);
}

#include "lib/Companion/CompanionInventoryHandler.h"
namespace {
class Catalog final : public InventoryCatalog {
 public:
  Identity generation{};
  uint64_t version = 1;
  uint64_t total = 9;
  bool mutate = false;
  bool fail = false;
  size_t reads = 0;
  Catalog() { generation.fill(2); }
  const Identity& storageGeneration() const override { return generation; }
  uint64_t revision() const override { return version; }
  uint64_t count() const override { return total; }
  bool read(uint64_t index, ContentManifest& manifest) override {
    ++reads;
    if (fail) return false;
    manifest = {};
    manifest.contentHash.fill(static_cast<uint8_t>(index + 1));
    manifest.length = 3;
    if (mutate) ++version;
    return true;
  }
};
}  // namespace
TEST(CompanionInventory, HandlerBoundsPagesAndRejectsChangedCatalog) {
  Catalog catalog;
  InventoryRequest request;
  request.storageGeneration.fill(2);
  std::array<uint8_t, INVENTORY_REQUEST_SIZE> body{};
  std::vector<uint8_t> response(MAX_INVENTORY_PAGE_SIZE + 1);
  ASSERT_EQ(encodeInventoryRequest(request, body), body.size());
  EXPECT_EQ(handleInventory(catalog, false, body, response), 1U);
  EXPECT_EQ(catalog.reads, 0U);
  const auto first = handleInventory(catalog, true, body, response);
  ASSERT_EQ(first, MAX_INVENTORY_PAGE_SIZE + 1);
  InventoryPageView page;
  ASSERT_TRUE(decodeInventoryPage(std::span(response).subspan(1, first - 1), page));
  EXPECT_EQ(page.count(), 8U);
  EXPECT_FALSE(page.header.complete);
  request.cursor = page.header.nextCursor;
  request.revision = page.header.revision;
  ASSERT_EQ(encodeInventoryRequest(request, body), body.size());
  const auto last = handleInventory(catalog, true, body, response);
  ASSERT_TRUE(decodeInventoryPage(std::span(response).subspan(1, last - 1), page));
  EXPECT_EQ(page.count(), 1U);
  EXPECT_TRUE(page.header.complete);
  catalog.mutate = true;
  EXPECT_EQ(handleInventory(catalog, true, body, response), 1U);
  EXPECT_EQ(response[0], static_cast<uint8_t>(InventoryResult::Changed));
  catalog.mutate = false;
  request.revision = catalog.version;
  ASSERT_EQ(encodeInventoryRequest(request, body), body.size());
  catalog.fail = true;
  EXPECT_EQ(handleInventory(catalog, true, body, response), 1U);
  EXPECT_EQ(response[0], static_cast<uint8_t>(InventoryResult::IoError));
}
