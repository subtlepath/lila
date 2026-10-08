#include <openssl/evp.h>

#include <cassert>
#include <string>

#include "lib/Companion/CompanionEpubReferenceJson.h"
#include "lib/hal/HalEpubReferenceJsonIo.h"
#include "lib/hal/HalRemovalReferencePath.h"
using namespace companion;
struct Reader final : EpubReferenceJsonReader {
  std::string_view bytes;
  size_t at = 0;
  bool ok = true;
  explicit Reader(std::string_view bytes) : bytes(bytes) {}
  int read() override { return at < bytes.size() ? static_cast<unsigned char>(bytes[at++]) : -1; }
  bool healthy() const override { return ok; }
  size_t readBytes(char* output, size_t count) override {
    count = std::min(count, bytes.size() - at);
    std::copy_n(bytes.begin() + at, count, output);
    at += count;
    return count;
  }
};
struct Writer final : EpubReferenceJsonWriter {
  std::string output;
  size_t limit = SIZE_MAX;
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t count) override {
    if (count > limit - output.size()) return 0;
    output.append(reinterpret_cast<const char*>(bytes), count);
    return count;
  }
};
static bool permitted(void* context) { return *static_cast<bool*>(context); }
static void halAdapters() {
  EpubReferenceJson json;
  const std::string input = "{\"openEpubPath\":\"/gone.epub\",\"unknown\":\"" + std::string(3000, 'x') + "\"}";
  for (unsigned fault = 0; fault < 5; ++fault) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.files["/source"] = {input.begin(), input.end()};
    HalFile source;
    assert(Storage.openFileForReadReusing("TEST", "/source", source));
    std::array<uint8_t, 128> scratch{};
    bool allowed = true;
    HalEpubReferenceJsonReader reader;
    assert(reader.begin(source, scratch, permitted, &allowed));
    if (fault == 1) state.failRead = 1;
    if (fault == 2) state.shortRead = 1;
    if (fault == 3) allowed = false;
    if (fault == 4) state.files["/source"].push_back(' ');
    const bool loaded = json.load(reader, RemovalMetadataFile::State, "/gone.epub", removalReferencePathEqual, nullptr);
    assert(loaded == !fault);
    assert(reader.healthy() == !fault);
    Digest expectedHash{}, actualHash{};
    assert(EVP_Digest(input.data(), input.size(), expectedHash.data(), nullptr, EVP_sha256(), nullptr) == 1);
    assert(reader.contentHash(actualHash) == !fault);
    if (!fault) assert(actualHash == expectedHash);
    assert(source.close());
    if (!fault) assert(state.yields > 0);
  }
  Reader inputReader(input);
  assert(json.load(inputReader, RemovalMetadataFile::State, "/gone.epub", removalReferencePathEqual, nullptr));
  Writer expected;
  assert(json.write(expected));
  for (unsigned fault = 0; fault < 7; ++fault) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.files["/candidate"] = std::vector<uint8_t>(5000, 'z');
    HalFile destination;
    assert(Storage.openFileForWriteReusing("TEST", "/candidate", destination));
    std::array<uint8_t, 128> scratch{};
    bool allowed = true;
    HalEpubReferenceJsonWriter writer;
    assert(writer.begin(destination, scratch, json.encodedSize(), permitted, &allowed));
    if (fault == 1) state.failWrite = true;
    if (fault == 2) state.failSync = true;
    if (fault == 3) state.failTruncate = true;
    if (fault == 4) allowed = false;
    if (fault == 6) state.corruptWrite = true;
    const bool written = json.write(writer);
    if (fault == 5) allowed = false;
    const bool sealed = written && writer.finish();
    assert(sealed == (!fault || fault == 6));
    assert(destination.close());
    if (!fault || fault == 6) {
      Digest expectedHash{}, readbackHash{};
      assert(EVP_Digest(expected.output.data(), expected.output.size(), expectedHash.data(), nullptr, EVP_sha256(),
                        nullptr) == 1);
      assert(writer.contentHash() && *writer.contentHash() == expectedHash);
      const auto& bytes = state.files.at("/candidate");
      assert(EVP_Digest(bytes.data(), bytes.size(), readbackHash.data(), nullptr, EVP_sha256(), nullptr) == 1);
      assert((readbackHash == expectedHash) == !fault);
    } else {
      assert(!writer.contentHash());
    }
    if (!fault) {
      assert(state.files.at("/candidate") == std::vector<uint8_t>(expected.output.begin(), expected.output.end()));
      assert(state.yields > 0);
      assert(!writer.finish());
    }
  }
}
static void multiplePaths() {
  EpubReferenceJson json;
  auto matcher = [](void* context, std::string_view path, bool& matched) {
    if (context && path == "/second.epub") return false;
    matched = removalReferencePathEqual(nullptr, path, "/first.epub") ||
              removalReferencePathEqual(nullptr, path, "/second.epub");
    return true;
  };
  static constexpr char INPUT[] =
      "{\"books\":[{\"path\":\"/FIRST.epub\"},{\"path\":\"/second.epub\"},"
      "{\"path\":\"/keep.epub\",\"position\":23}],\"history\":[1,2]}";
  Reader input(INPUT);
  assert(json.loadMatching(input, RemovalMetadataFile::Recent, matcher, nullptr));
  Writer output;
  assert(json.write(output));
  assert(output.output == "{\"books\":[{\"path\":\"/keep.epub\",\"position\":23}],\"history\":[1,2]}");
  Reader failed(INPUT);
  bool fail = true;
  assert(!json.loadMatching(failed, RemovalMetadataFile::Recent, matcher, &fail));
  assert(!json.changed());
  assert(json.encodedSize() == 0);
  Writer rejected;
  assert(!json.write(rejected));
  assert(rejected.output.empty());
}
int main() {
  multiplePaths();
  halAdapters();
  assert(removalReferencePathEqual(nullptr, "/books/CAF\xc3\x89.EPUB", "/Books/caf\xc3\xa9.epub"));
  assert(!removalReferencePathEqual(nullptr, "/books/a.epub", "/books/nested/a.epub"));
  assert(!removalReferencePathEqual(nullptr, "/books/a.epub", "/books/../a.epub"));
  assert(!removalReferencePathEqual(nullptr, "/books/a.epub", "/books/BOOK~1.EPU"));
  EpubReferenceJson json;
  Reader state(R"({"openEpubPath":"/books/CAF\u00c9.EPUB","lastSleepFromReader":true,"unknown":{"x":[1,2,3]}})");
  assert(json.load(state, RemovalMetadataFile::State, "/Books/caf\xc3\xa9.epub", removalReferencePathEqual, nullptr));
  assert(json.changed());
  Writer stateOut;
  assert(json.write(stateOut));
  assert(stateOut.output == R"({"openEpubPath":"","lastSleepFromReader":true,"unknown":{"x":[1,2,3]}})");
  Reader recent(
      R"({"books":[{"path":"/gone.epub","progress":55},{"path":"/kept.epub","title":"keep","progress":33,"unknown":true},{"path":"/gone.epub"}],"other":[4]})");
  assert(json.load(recent, RemovalMetadataFile::Recent, "/gone.epub", removalReferencePathEqual, nullptr));
  assert(json.changed());
  Writer recentOut;
  assert(json.write(recentOut));
  assert(recentOut.output ==
         R"({"books":[{"path":"/kept.epub","title":"keep","progress":33,"unknown":true}],"other":[4]})");
  Reader unchanged(recentOut.output);
  assert(json.load(unchanged, RemovalMetadataFile::Recent, "/gone.epub", removalReferencePathEqual, nullptr));
  assert(!json.changed());
  Writer failed;
  failed.limit = 2;
  assert(!json.write(failed));
  for (const auto input : {"{", "[]", "{} trailing", "{}x", "{}{}", "{\"books\":4}", "{\"books\":[4]}",
                           "{\"books\":[{\"path\":4}]}", "{\"books\":[{\"path\":\"/gone.epub\\u0000x\"}]}"}) {
    Reader source(input);
    assert(!json.load(source, RemovalMetadataFile::Recent, "/gone.epub", removalReferencePathEqual, nullptr));
    assert(!json.changed());
    assert(!json.encodedSize());
    Writer destination;
    assert(!json.write(destination));
    assert(destination.output.empty());
  }
  Reader badState(R"({"openEpubPath":17})");
  assert(!json.load(badState, RemovalMetadataFile::State, "/gone.epub", removalReferencePathEqual, nullptr));
  const std::string nested = "{\"other\":" + std::string(ARDUINOJSON_DEFAULT_NESTING_LIMIT - 1, '[') + "0" +
                             std::string(ARDUINOJSON_DEFAULT_NESTING_LIMIT - 1, ']') + "}";
  Reader nesting(nested);
  assert(json.load(nesting, RemovalMetadataFile::State, "/gone.epub", removalReferencePathEqual, nullptr));
  const std::string excessive = "{\"other\":" + std::string(ARDUINOJSON_DEFAULT_NESTING_LIMIT, '[') + "0" +
                                std::string(ARDUINOJSON_DEFAULT_NESTING_LIMIT, ']') + "}";
  Reader overNested(excessive);
  assert(!json.load(overNested, RemovalMetadataFile::State, "/gone.epub", removalReferencePathEqual, nullptr));
  Reader io("{}");
  io.ok = false;
  assert(!json.load(io, RemovalMetadataFile::State, "/gone.epub", removalReferencePathEqual, nullptr));
  const std::string huge = "{\"other\":\"" + std::string(30000, 'a') + "\"}";
  Reader exhausted(huge);
  assert(!json.load(exhausted, RemovalMetadataFile::State, "/gone.epub", removalReferencePathEqual, nullptr));
  // Reset the arena only after clearing every document-owned resource.
  for (unsigned at = 0; at < 200; ++at) {
    Reader reusable(R"({"openEpubPath":"/gone.epub","other":123})");
    assert(json.load(reusable, RemovalMetadataFile::State, "/gone.epub", removalReferencePathEqual, nullptr));
    Writer destination;
    assert(json.write(destination));
    assert(destination.output == R"({"openEpubPath":"","other":123})");
  }
}
