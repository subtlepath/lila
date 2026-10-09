#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include "CompanionContentRead.h"
#include "HalInventoryFileHash.h"

namespace companion {
// Session-owned. The caller retains the borrowed file without reopening it while
// attached, excludes content mutations, and supplies the existing hash workspace.
class HalContentReadSource final {
 public:
  explicit HalContentReadSource(InventoryHashProgress progress = nullptr, void* context = nullptr)
      : progress(progress), context(context) {}
  void detach() { file = nullptr; }
  bool matches(const ContentReadRequest& request) const {
    return file && generation == request.generation && manifest == request.manifest;
  }
  ContentReadResult attach(const ContentReadRequest& request, HalFile& borrowed, std::span<uint8_t> scratch) {
    detach();
    if (!validContentReadRequest(request) || scratch.empty()) return failure(ContentReadResult::Invalid, "arguments");
    if (!borrowed || borrowed.isDirectory()) return failure(ContentReadResult::IoError, "file unavailable");
    if (!authorized()) return failure(ContentReadResult::Busy, "cancelled");
    const uint64_t size = borrowed.fileSize64();
    const uint32_t modified = borrowed.modificationTime();
    if (size != request.manifest.length) return failure(ContentReadResult::Corrupt, "length mismatch");
    uint64_t length = 0;
    Digest hash{};
    if (!hashInventoryFile(borrowed, scratch, length, hash, progress, context))
      return failure(authorized() ? ContentReadResult::IoError : ContentReadResult::Busy, "hash failed");
    if (!authorized()) return failure(ContentReadResult::Busy, "cancelled");
    if (!borrowed || borrowed.fileSize64() != size || borrowed.modificationTime() != modified || length != size ||
        hash != request.manifest.contentHash)
      return failure(ContentReadResult::Corrupt, "changed content");
    file = &borrowed;
    generation = request.generation;
    manifest = request.manifest;
    modification = modified;
    return ContentReadResult::Ok;
  }
  // Only Ok permits the caller to include output bytes in its encoded reply.
  ContentReadResult read(const ContentReadRequest& request, std::span<uint8_t> output) {
    if (!validContentReadRequest(request) || output.size() != contentReadByteCount(request))
      return failure(ContentReadResult::Invalid, "read bounds");
    if (!file || !*file) return failure(ContentReadResult::NotFound, "detached file");
    if (generation != request.generation) return failure(ContentReadResult::WrongStorage, "card changed");
    if (manifest != request.manifest) return failure(ContentReadResult::Invalid, "manifest changed");
    if (!authorized()) return failure(ContentReadResult::Busy, "cancelled");
    if (!unchanged()) return failure(ContentReadResult::Corrupt, "file changed");
    if (!file->seek64(request.offset) || file->read(output.data(), output.size()) != static_cast<int>(output.size()))
      return failure(ContentReadResult::IoError, "chunk read failed");
    if (!authorized()) return failure(ContentReadResult::Busy, "cancelled");
    if (!unchanged()) return failure(ContentReadResult::Corrupt, "file changed during read");
    return ContentReadResult::Ok;
  }

 private:
  HalFile* file = nullptr;
  Identity generation{};
  ContentManifest manifest{};
  uint32_t modification = 0;
  InventoryHashProgress progress;
  void* context;
  bool authorized() const { return !progress || progress(context); }
  bool unchanged() const {
    return file && *file && !file->isDirectory() && file->fileSize64() == manifest.length &&
           file->modificationTime() == modification;
  }
  ContentReadResult failure(ContentReadResult result, const char* reason) {
    detach();
    LOG_ERR("COMPANION", "Content export: %s", reason);
    return result;
  }
};
}  // namespace companion
