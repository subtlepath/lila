#include <CrossPointSettings.h>
#include <openssl/evp.h>

#include <cassert>
#include <memory>

#include "lib/hal/HalContentRemovalStartupRecovery.h"
#include "lib/hal/HalEpubRemovalSession.h"
using namespace companion;
struct PathsMemory final : InventoryIndexStorage {
  std::vector<uint8_t> bytes;
  bool size(uint64_t& output) override {
    output = bytes.size();
    return true;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    if (offset > bytes.size() || output.size() > bytes.size() - offset) return false;
    std::copy_n(bytes.begin() + offset, output.size(), output.begin());
    return true;
  }
};
int main() {
  for (uint32_t format : {1U, 4U}) {
    for (unsigned fault = 0; fault < 5; ++fault) {
      auto& state = inventory_hal_test::state;
      state = {};
      state.enumerateFileMap = true;
      state.falseExists = true;
      state.directories["/"] = {};
      state.directories["/fonts/Family"] = {};
      state.directories["/.fonts/Family"] = {};
      const char* first = format == 4 ? "/fonts/Family/Family_14.cpfont" : "/fonts/Family/Regular.ttf";
      const char* second = format == 4 ? "/.fonts/Family/Family_14.cpfont" : "/.fonts/Family/Regular.ttf";
      const std::vector<uint8_t> content{1, 2, 3, 4, 5};
      state.files[first] = content;
      state.files[second] = content;
      state.files["/fonts/Family/Unrelated.ttf"] = {9, 8};
      CrossPointSettings settings;
      ContentRemovalRequest request;
      request.transaction.fill(1);
      request.owner.fill(2);
      request.generation.fill(3);
      request.manifest.kind = ContentKind::Font;
      request.manifest.formatVersion = format;
      request.manifest.length = content.size();
      EVP_Digest(content.data(), content.size(), request.manifest.contentHash.data(), nullptr, EVP_sha256(), nullptr);
      PathsMemory memory;
      memory.bytes.resize(INVENTORY_INDEX_HEADER_SIZE + 2 * INVENTORY_PATH_MAX_RECORD);
      size_t length = INVENTORY_INDEX_HEADER_SIZE;
      for (const char* path : {first, second})
        length += encodeInventoryPath(request.manifest, path, std::span(memory.bytes).subspan(length));
      memory.bytes.resize(length);
      InventoryIndexHeader header{request.generation, 7, 2,
                                  inventoryIndexCrc(std::span(memory.bytes).subspan(INVENTORY_INDEX_HEADER_SIZE))};
      assert(encodeInventoryPathsHeader(header, memory.bytes) == INVENTORY_INDEX_HEADER_SIZE);
      std::array<uint8_t, INVENTORY_PATH_MAX_RECORD> scratch{};
      InventoryPaths paths(memory, scratch);
      const uint64_t revision = 7;
      unsigned refreshes = 0;
      auto session = std::make_unique<HalEpubRemovalSession>(
          request.generation, paths, revision, 4096, [](void*) { return true; },
          [](void* ctx) {
            ++*static_cast<unsigned*>(ctx);
            return true;
          },
          &refreshes, nullptr, &settings);
      assert(session->prepare());
      std::array<uint8_t, CONTENT_REMOVAL_REQUEST_SIZE> body{};
      assert(encodeContentRemovalRequest(request, body) == body.size());
      std::array<uint8_t, CONTENT_REMOVAL_REPLY_SIZE> reply{};
      if (fault == 1) state.failRenameAfter = state.renames + 2;
      if (fault == 2) settings.saveSucceeds = false;
      if (fault == 3) std::memset(settings.sdFontFamilyName, 'x', sizeof(settings.sdFontFamilyName));
      if (fault == 4) state.files[second][0] ^= 1;
      assert(session->handle(true, request.owner, body, reply) == reply.size());
      if (!fault)
        assert(reply[0] == static_cast<uint8_t>(ContentRemovalResult::Ok));
      else
        assert(reply[0] != static_cast<uint8_t>(ContentRemovalResult::Ok));
      if (fault == 3 || fault == 4) {
        assert(state.files.contains(first) && state.files.contains(second));
        assert(settings.saves == 0);
        assert(!state.files.contains(CONTENT_REMOVAL_JOURNALS[0]));
        continue;
      }
      state.failRenameAfter = 0;
      settings.saveSucceeds = true;
      assert(session->closeReaders());
      session.reset();
      if (fault) {
        auto startup = std::make_unique<HalContentRemovalStartupRecovery>(&settings);
        assert(startup->run(request.generation));
        bool pending = true;
        assert(startup->pending(pending) && !pending);
      }
      auto retry = std::make_unique<HalEpubRemovalSession>(
          request.generation, paths, revision, 4096, [](void*) { return true; },
          [](void* ctx) {
            ++*static_cast<unsigned*>(ctx);
            return true;
          },
          &refreshes, nullptr, &settings);
      assert(retry->prepare());
      assert(retry->handle(true, request.owner, body, reply) == reply.size());
      assert(reply[0] == static_cast<uint8_t>(ContentRemovalResult::Ok));
      assert(!state.files.contains(first) && !state.files.contains(second));
      assert(state.files.at("/fonts/Family/Unrelated.ttf") == std::vector<uint8_t>({9, 8}));
      assert(settings.persisted[0] == 0);
      if (!fault) assert(settings.saves == 1);
      assert(refreshes > 0);
    }
  }
}
