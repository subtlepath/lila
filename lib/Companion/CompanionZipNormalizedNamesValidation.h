#pragma once

#include "CompanionZipNameAudit.h"
#include "CompanionZipNameDuplicateValidation.h"
#include "CompanionZipNameNormalization.h"

namespace companion {
struct ZipNameWorkspace {
  std::span<uint8_t> raw, decoded, output;
  std::span<uint32_t> inputScalars, normalizedScalars;
  static constexpr size_t WINDOW_SCALARS = 8192;
  // Scalar objects retain their lifetimes while the decoder accesses their
  // byte representations. Call only with aligned, caller-owned scalar storage.
  static bool borrowDecoderWindow(std::span<uint32_t> storage, ZipNameWorkspace& output,
                                  std::span<uint8_t>& decoderWindow) {
    if (storage.size() < WINDOW_SCALARS || reinterpret_cast<uintptr_t>(storage.data()) % alignof(uint32_t))
      return false;
    auto bytes = std::span(reinterpret_cast<uint8_t*>(storage.data()), WINDOW_SCALARS * sizeof(uint32_t));
    output = {bytes.first(1024), bytes.subspan(1024, 3072), bytes.subspan(4096, 3072), storage.subspan(1792, 1024),
              storage.subspan(2816, 4096)};
    decoderWindow = bytes;
    return true;
  }
};
// Session-owned. Workspace spans must be disjoint, aligned and outside task
// stacks; they may reuse a decoder window between name and extraction phases.
class ZipNormalizedNamesValidation final : public ZipNameAudit {
 public:
  using Progress = bool (*)(void*);
  // Name views expire on return. Visitors must not publish results before the
  // enclosing archive validation succeeds.
  using Visitor = bool (*)(void*, std::span<const uint8_t>, const ZipEntryMetadata&);
  ZipNormalizedNamesValidation(InventoryIndexStorage& source, ZipNameDuplicateValidation& duplicates,
                               ZipNameWorkspace workspace, Progress progress = nullptr, void* context = nullptr,
                               Visitor visitor = nullptr, void* visitorContext = nullptr)
      : source(source),
        duplicates(duplicates),
        workspace(workspace),
        progress(progress),
        context(context),
        visitor(visitor),
        visitorContext(visitorContext) {}
  bool begin() override {
    ready = false;
    duplicates.abort();
    if (!tick() || !duplicates.begin()) return false;
    ready = true;
    return true;
  }
  bool add(const ZipEntryMetadata& entry) override {
    if (!ready || !entry.nameBytes || entry.nameBytes > workspace.raw.size() || !tick() ||
        !source.read(entry.nameOffset, workspace.raw.first(entry.nameBytes)))
      return fail();
    size_t bytes = 0;
    if (!ZipNameNormalization::normalize(workspace.raw.first(entry.nameBytes), entry.payload.flags & 0x800,
                                         entry.directory, workspace.decoded, workspace.inputScalars,
                                         workspace.normalizedScalars, workspace.output, bytes) ||
        !tick() || !duplicates.add(workspace.output.first(bytes)) ||
        (visitor && !visitor(visitorContext, workspace.output.first(bytes), entry)))
      return fail();
    return true;
  }
  bool finish() override {
    if (!ready) return false;
    ready = false;
    const bool ok = tick() && duplicates.finish();
    if (!ok) duplicates.abort();
    return ok;
  }
  void abort() override {
    ready = false;
    duplicates.abort();
  }

 private:
  InventoryIndexStorage& source;
  ZipNameDuplicateValidation& duplicates;
  ZipNameWorkspace workspace;
  Progress progress;
  void* context;
  Visitor visitor;
  void* visitorContext;
  bool ready = false;
  bool tick() const { return !progress || progress(context); }
  bool fail() {
    abort();
    return false;
  }
};
}  // namespace companion
