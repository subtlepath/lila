#pragma once

#include "HalDictionaryCacheStorage.h"
#include "HalDictionaryIncomingExtraction.h"
#include "HalDictionaryInstallationPlanBuilder.h"
#include "HalDictionaryOriginalArchive.h"

namespace companion {
// One preparation attempt per checked session owner, outside the task stack.
// Caller completes inventory recovery, serializes cache access, and retains the
// source/extraction through use. Retry uses a fresh owner after handles release.
class HalDictionaryInstallationPreparation final {
 public:
  HalDictionaryInstallationPreparation(HalDictionaryIncomingExtraction& incoming, HalFile& file,
                                       const ContentManifest& manifest, std::span<uint8_t> scratch,
                                       tinfl_decompressor* decoder, std::span<uint8_t> window,
                                       InventoryHashProgress progress = nullptr, void* context = nullptr)
      : incoming(incoming),
        file(file),
        manifest(manifest),
        canonical(scratch, decoder, window, progress, context),
        cache(progress, context),
        publication(cache, scratch),
        original(cache, scratch, progress, context) {}
  bool prepare(std::string_view base) {
    ready = false;
    if (attempted) return fail("reused owner");
    attempted = true;
    const auto parent = incoming.receiptParent();
    const auto members = incoming.selectedMembers();
    const auto extracted = incoming.extractedMembers();
    if (!parent || !members || !extracted || !validDictionaryInstallationBase(base) ||
        !parent->matchesArchive(manifest) ||
        (!parent->isPhase(TransferPhase::Receiving) && !parent->isPhase(TransferPhase::Verified)))
      return fail("extraction ownership");
    if (publication.discardUnpublishedCandidate() != DictionaryCacheResult::Ok ||
        !canonical.build(*members, *extracted, canonicalManifest) ||
        publication.publish(canonicalManifest) != DictionaryCacheResult::Ok || !source.attach(file) ||
        !original.retain(source, manifest, *parent) || !builder.create(base, manifest, canonical, *parent, plan))
      return fail("archives or plan");
    ready = true;
    return true;
  }
  const DictionaryInstallationPlan* current() const {
    const auto parent = incoming.receiptParent();
    return ready && parent && parent->current() && *parent->current() == plan.extraction &&
                   parent->matchesArchive(plan.archives.original)
               ? &plan
               : nullptr;
  }

 private:
  HalDictionaryIncomingExtraction& incoming;
  HalFile& file;
  const ContentManifest& manifest;
  HalDictionaryStagedArchive canonical;
  HalDictionaryCacheStorage cache;
  DictionaryCachePublication publication;
  HalInventoryFileView source;
  HalDictionaryOriginalArchive original;
  HalDictionaryInstallationPlanBuilder builder;
  ContentManifest canonicalManifest;
  DictionaryInstallationPlan plan;
  bool attempted = false, ready = false;
  static bool fail(const char* operation) {
    LOG_ERR("COMPANION", "Dictionary installation preparation %s failed", operation);
    return false;
  }
};
}  // namespace companion
