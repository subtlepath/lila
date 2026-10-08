#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "CompanionDictionaryCachePublication.h"
#include "CompanionInventoryPathSink.h"
#include "CompanionInventoryPaths.h"
#include "CompanionInventoryScan.h"
#include "HalInventoryDirectoryWalker.h"
#include "HalInventoryFileHash.h"

namespace companion {
enum class InventoryFileDecision { Skip, Include, Error };
class InventoryFileResolver {
 public:
  virtual ~InventoryFileResolver() = default;
  // Directory Include descends; Skip prunes. File Include supplies validated
  // kind/version/family; hash and length are always computed from the open file.
  // Unsupported transferable content must be Error, not Skip.
  virtual InventoryFileDecision resolve(const char* path, HalFile& file, ContentManifest& metadata) = 0;
  // Called only for included directories. Include supplies a fully validated,
  // immutable dictionary archive and its exact manifest; path is borrowed until
  // the next resolver call. Skip keeps normal directory traversal.
  virtual InventoryFileDecision resolveBundle(const char*, ContentManifest&, const char*&) {
    return InventoryFileDecision::Skip;
  }
  // Check bindings against actual file bytes before recording or emitting them.
  virtual bool verifyHashed(const char*, const ContentManifest&) { return true; }
};
// Session-owned; hash scratch must be disjoint from the sorter's scratch.
class HalInventoryFileSource final : public InventoryScan {
 public:
  HalInventoryFileSource(HalInventoryDirectoryWalker& walker, InventoryFileResolver& resolver, InventoryPathSink& paths,
                         std::span<uint8_t> hashScratch)
      : walker(walker), resolver(resolver), paths(paths), hashScratch(hashScratch) {}
  ~HalInventoryFileSource() override {
    if (!bundleFile.close()) LOG_ERR("COMPANION", "Inventory bundle close failed");
  }
  bool beginScan() override { return begin("/"); }
  bool closeScan() override {
    failed = true;
    const bool bundleClosed = bundleFile.close();
    const bool walkerClosed = walker.close();
    return bundleClosed && walkerClosed;
  }
  bool begin(const char* root = "/") {
    failed = true;
    complete = false;
    if (hashScratch.empty() || !bundleFile.close() || !walker.begin(root)) {
      LOG_ERR("COMPANION", "Cannot begin inventory file scan");
      return false;
    }
    failed = false;
    return true;
  }
  InventorySourceResult next(ContentManifest& manifest) override {
    if (failed) return InventorySourceResult::Error;
    if (complete) return InventorySourceResult::End;
    for (;;) {
      const auto result = walker.next();
      if (result == HalInventoryDirectoryWalker::Result::Error) return error("Inventory traversal failed");
      if (result == HalInventoryDirectoryWalker::Result::End) {
        complete = true;
        return InventorySourceResult::End;
      }
      auto& file = walker.entryFile();
      ContentManifest candidate;
      const auto decision = resolver.resolve(walker.entryPath(), file, candidate);
      vTaskDelay(1);
      if (decision == InventoryFileDecision::Error) return error("Inventory metadata resolution failed");
      if (decision == InventoryFileDecision::Skip) {
        if (file.isDirectory()) walker.skipDirectory();
        continue;
      }
      if (decision != InventoryFileDecision::Include) return error("Invalid inventory file decision");
      if (file.isDirectory()) {
        const char* archivePath = nullptr;
        const auto bundle = resolver.resolveBundle(walker.entryPath(), candidate, archivePath);
        if (bundle == InventoryFileDecision::Skip) continue;
        if (bundle != InventoryFileDecision::Include) return error("Inventory bundle resolution failed");
        walker.skipDirectory();
        return emitBundle(archivePath, candidate, manifest);
      }
      switch (candidate.kind) {
        case ContentKind::Epub:
        case ContentKind::Course:
        case ContentKind::Font:
        case ContentKind::Dictionary:
        case ContentKind::Firmware:
          break;
        default:
          return error("Invalid inventory content kind");
      }
      if (!hashInventoryFile(file, hashScratch, candidate.length, candidate.contentHash)) {
        return error("Inventory content hashing failed");
      }
      if (!resolver.verifyHashed(walker.entryPath(), candidate)) return error("Inventory content binding mismatch");
      if (!paths.record(candidate, walker.entryPath())) return error("Inventory path staging failed");
      manifest = candidate;
      return InventorySourceResult::Entry;
    }
  }

 private:
  HalInventoryDirectoryWalker& walker;
  InventoryFileResolver& resolver;
  InventoryPathSink& paths;
  std::span<uint8_t> hashScratch;
  HalFile bundleFile;  // Opaque HAL wrapper is allocated once and reused.
  bool failed = true, complete = false;
  [[gnu::noinline]] InventorySourceResult emitBundle(const char* archivePath, const ContentManifest& candidate,
                                                     ContentManifest& manifest) {
    if (!archivePath || !validInventoryPath(archivePath) || std::strcmp(archivePath, DICTIONARY_CACHE_CANDIDATE) == 0 ||
        std::strcmp(archivePath, DICTIONARY_MEMBER_CANDIDATE) == 0 || candidate.kind != ContentKind::Dictionary ||
        candidate.formatVersion != 1 || candidate.logicalIdentity != Identity{} || candidate.length < 22 ||
        candidate.length > UINT32_MAX || !Storage.openFileForReadReusing("COMPANION", archivePath, bundleFile))
      return error("Invalid inventory dictionary archive");
    if (bundleFile.isDirectory()) {
      bundleFile.close();
      return error("Inventory dictionary archive is a directory");
    }
    Digest actual{};
    uint64_t bytes = 0;
    const bool hashed = hashInventoryFile(bundleFile, hashScratch, bytes, actual);
    const bool closed = bundleFile.close();
    if (!hashed || !closed || bytes != candidate.length || actual != candidate.contentHash ||
        !resolver.verifyHashed(archivePath, candidate))
      return error("Inventory dictionary archive verification failed");
    if (!paths.record(candidate, archivePath)) return error("Inventory bundle path staging failed");
    manifest = candidate;
    return InventorySourceResult::Entry;
  }
  InventorySourceResult error(const char* reason) {
    LOG_ERR("COMPANION", "%s", reason);
    failed = true;
    return InventorySourceResult::Error;
  }
};
}  // namespace companion
