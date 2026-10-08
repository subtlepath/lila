#pragma once

#include "CompanionDictionaryZipSelection.h"
#include "HalDictionaryExtractionRecovery.h"
#include "HalZipEntryStage.h"

namespace companion {
// Selection requires a validated, exclusively owned archive. Sealed private
// files remain installer-owned even when later extraction or journaling fails.
class HalDictionaryZipExtraction final {
 public:
  using Member = HalZipEntryStage::Member;
  using Sealed = bool (*)(void*, Member, const Digest&);
  HalDictionaryZipExtraction(ZipEntryExtraction& extractor, HalZipEntryStage& stage, Sealed sealed = nullptr,
                             void* context = nullptr)
      : extractor(extractor), stage(stage), sealed(sealed), context(context) {}
  bool extract(const DictionaryZipMembers& members) {
    complete = false;
    mask = 0;
    hashes.fill({});
    return run(members, nullptr);
  }
  bool resume(const DictionaryZipMembers& members, DictionaryExtractionParent& parent,
              HalDictionaryExtractionRecovery& recovery) {
    complete = false;
    mask = 0;
    hashes.fill({});
    const auto receipt = parent.current();
    if (!receipt || receipt->compressed != members.compressed || receipt->synonyms != members.hasSynonyms ||
        receipt->lengths[0] != members.definitions.expandedBytes ||
        receipt->lengths[1] != members.index.expandedBytes || receipt->lengths[2] != members.info.expandedBytes ||
        receipt->lengths[3] != (members.hasSynonyms ? members.synonyms.expandedBytes : 0) ||
        !recovery.verifyAndDiscardPartial(parent)) {
      LOG_ERR("COMPANION", "Dictionary extraction resume validation failed");
      return false;
    }
    hashes = receipt->hashes;
    mask = receipt->sealed;
    return run(members, &parent);
  }
  bool isComplete() const { return complete; }
  // This mask includes sealed files whose persistence callback failed.
  uint8_t sealedMembers() const { return mask; }
  const Digest* contentHash(Member role) const {
    const auto at = static_cast<unsigned>(role);
    return at < hashes.size() && (mask & (1u << at)) ? &hashes[at] : nullptr;
  }

 private:
  ZipEntryExtraction& extractor;
  HalZipEntryStage& stage;
  Sealed sealed;
  void* context;
  std::array<Digest, 4> hashes{};
  uint8_t mask = 0;
  bool complete = false;
  bool run(const DictionaryZipMembers& members, DictionaryExtractionParent* parent) {
    if (!member(members.info, Member::Info, parent) || !member(members.index, Member::Index, parent) ||
        !member(members.definitions, Member::Definitions, parent) ||
        (members.hasSynonyms && !member(members.synonyms, Member::Synonyms, parent)))
      return false;
    complete = true;
    return true;
  }
  bool member(const ZipEntrySpan& payload, Member role, DictionaryExtractionParent* parent) {
    const auto at = static_cast<unsigned>(role);
    if (mask & (1u << at)) return true;
    if (!stage.extractMember(extractor, payload, role)) {
      LOG_ERR("COMPANION", "Dictionary member extraction failed: %u", static_cast<unsigned>(role));
      return false;
    }
    hashes[at] = stage.contentHash();
    mask |= 1u << at;
    if (parent ? parent->recordSealed(at, hashes[at]) != DictionaryJournalResult::Ok
               : sealed && !sealed(context, role, hashes[at])) {
      LOG_ERR("COMPANION", "Dictionary member receipt persistence failed: %u", at);
      return false;
    }
    return true;
  }
};
}  // namespace companion
