#pragma once

#include "CompanionDictionaryMembersValidation.h"
#include "HalDictionaryArchiveStage.h"
#include "HalDictionaryStagedSource.h"

namespace companion {
// Session-owned. Parent retains exclusive ownership of member and canonical
// candidates through installation/recovery; all working buffers are borrowed.
class HalDictionaryStagedArchive final {
 public:
  HalDictionaryStagedArchive(std::span<uint8_t> scratch, tinfl_decompressor* decoder = nullptr,
                             std::span<uint8_t> window = {}, InventoryHashProgress progress = nullptr,
                             void* context = nullptr)
      : scratch(scratch),
        progress(progress),
        context(context),
        validation(source, scratch, decoder, window, progress, context),
        stage(scratch, progress, context),
        builder(stage, scratch) {}
  bool build(const DictionaryZipMembers& members, const HalDictionaryZipExtraction& extraction,
             ContentManifest& output) {
    ready = false;
    DictionaryMembersDetails details;
    if (!source.begin(members, extraction, scratch, progress, context) ||
        !validation.validate(members.compressed, members.hasSynonyms, details) ||
        !source.begin(members, extraction, scratch, progress, context))
      return fail("member validation");
    manifest = {};
    manifest.kind = ContentKind::Dictionary;
    manifest.formatVersion = 1;
    if (!builder.build(source, members.compressed, members.hasSynonyms, manifest.length) || !stage.isSealed())
      return fail("canonical archive construction");
    manifest.contentHash = stage.contentHash();
    lengths = {members.definitions.expandedBytes, members.index.expandedBytes, members.info.expandedBytes,
               members.hasSynonyms ? members.synonyms.expandedBytes : 0};
    compressed = members.compressed;
    synonyms = members.hasSynonyms;
    hashes.fill({});
    for (unsigned at = 0; at < (synonyms ? 4u : 3u); ++at) {
      const auto hash = extraction.contentHash(static_cast<HalZipEntryStage::Member>(at));
      if (!hash) return fail("missing member proof");
      hashes[at] = *hash;
    }
    output = manifest;
    ready = true;
    return true;
  }
  const ContentManifest* current() const { return ready ? &manifest : nullptr; }
  bool matchesReceipt(const DictionaryExtractionReceipt& receipt) const {
    return ready && receipt.lengths == lengths && receipt.hashes == hashes && receipt.compressed == compressed &&
           receipt.synonyms == synonyms && receipt.sealed == (synonyms ? 15 : 7);
  }

 private:
  std::span<uint8_t> scratch;
  InventoryHashProgress progress;
  void* context;
  HalDictionaryStagedSource source;
  DictionaryMembersValidation validation;
  HalDictionaryArchiveStage stage;
  DictionaryBundleBuilder builder;
  ContentManifest manifest;
  // Keep the proof independent of a reusable extraction coordinator's state.
  std::array<uint64_t, 4> lengths{};
  std::array<Digest, 4> hashes{};
  bool compressed = false, synonyms = false;
  bool ready = false;
  bool fail(const char* operation) {
    source.close();
    LOG_ERR("COMPANION", "Staged dictionary archive %s failed", operation);
    return false;
  }
};
}  // namespace companion
