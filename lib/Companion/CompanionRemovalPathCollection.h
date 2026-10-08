#pragma once

#include "CompanionSingleFileRemovalPlan.h"

namespace companion {
enum class RemovalPathCollectionResult { Ok, Invalid, Missing, Conflict, IoError };
// Only finish() may publish a sealed plan. Append borrows path for that call.
// Discard touches only the request's owned, unpublished staging evidence.
class RemovalPathSink {
 public:
  virtual ~RemovalPathSink() = default;
  virtual bool begin(const ContentRemovalRequest& request, uint64_t inventoryRevision) = 0;
  virtual bool append(std::string_view path) = 0;
  virtual bool finish(uint64_t count) = 0;
  virtual bool discard() = 0;
};
// Session-owned outside the stack; streams arbitrarily many matching paths with
// one copied path. The caller validates the index/path pair and excludes writers.
class RemovalPathCollection final {
 public:
  RemovalPathCollection(InventoryPaths& paths, RemovalPathSink& sink) : paths(paths), sink(sink) {}
  RemovalPathCollectionResult collect(const ContentRemovalRequest& request, uint64_t revision) {
    if (!validContentRemovalRequest(request) || revision == 0 ||
        (request.manifest.kind != ContentKind::Epub && request.manifest.kind != ContentKind::Font))
      return RemovalPathCollectionResult::Invalid;
    expected = request;
    if (!paths.open(expected.generation, revision)) return RemovalPathCollectionResult::IoError;
    count = 0;
    if (!sink.begin(expected, revision)) return cancel(RemovalPathCollectionResult::IoError);
    for (;;) {
      const auto result = paths.nextPath(current, path);
      if (result == InventoryPathRecordResult::Error) return cancel(RemovalPathCollectionResult::IoError);
      if (result == InventoryPathRecordResult::End) break;
      if (current.contentHash != expected.manifest.contentHash) continue;
      if (current != expected.manifest || !validSingleFileRemovalPlan({expected, path.data()}))
        return cancel(RemovalPathCollectionResult::Conflict);
      if (!sink.append(path.data())) return cancel(RemovalPathCollectionResult::IoError);
      ++count;
    }
    if (!count) return cancel(RemovalPathCollectionResult::Missing);
    if (!sink.finish(count)) return cancel(RemovalPathCollectionResult::IoError);
    return RemovalPathCollectionResult::Ok;
  }

 private:
  InventoryPaths& paths;
  RemovalPathSink& sink;
  ContentRemovalRequest expected;
  ContentManifest current;
  std::array<char, INVENTORY_PATH_LIMIT + 1> path{};
  uint64_t count = 0;
  RemovalPathCollectionResult cancel(RemovalPathCollectionResult result) {
    return sink.discard() ? result : RemovalPathCollectionResult::IoError;
  }
};
}  // namespace companion
