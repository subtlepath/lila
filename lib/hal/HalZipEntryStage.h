#pragma once

#include "CompanionDictionaryCachePublication.h"
#include "CompanionZipEntryExtraction.h"
#include "HalVerifiedFileStage.h"

namespace companion {
inline constexpr const char* ZIP_MEMBER_CANDIDATE = DICTIONARY_MEMBER_CANDIDATE;
// Sealed members belong to the installer; unknown candidates are never replaced.
class HalZipEntryStage final : public ZipEntrySink {
 public:
  enum class Member { Definitions, Index, Info, Synonyms };
  static const char* memberPath(Member member) {
    switch (member) {
      case Member::Definitions:
        return "/.crosspoint/companion/dictionary-definitions-next";
      case Member::Index:
        return "/.crosspoint/companion/dictionary-index-next";
      case Member::Info:
        return "/.crosspoint/companion/dictionary-info-next";
      case Member::Synonyms:
        return "/.crosspoint/companion/dictionary-synonyms-next";
    }
    return nullptr;
  }
  using Progress = InventoryHashProgress;
  explicit HalZipEntryStage(std::span<uint8_t> scratch, Progress progress = nullptr, void* context = nullptr)
      : stage(scratch, progress, context) {}
  // Reuse retained handles across members; sealed files remain caller-owned.
  bool extractMember(ZipEntryExtraction& extractor, const ZipEntrySpan& entry, Member member) {
    sealed = false;
    const auto selected = memberPath(member);
    if (!selected) {
      LOG_ERR("COMPANION", "Invalid dictionary member stage");
      return false;
    }
    candidate = selected;
    const bool result = extractor.extract(entry, *this);
    candidate = ZIP_MEMBER_CANDIDATE;
    return result;
  }
  bool begin(uint64_t expectedBytes) override {
    sealed = false;
    expected = expectedBytes;
    if (expected > ZipEntryExtraction::MAX_EXPANDED_BYTES) {
      LOG_ERR("COMPANION", "ZIP member stage exceeds entry limit");
      return false;
    }
    return stage.begin(candidate, expected);
  }
  bool write(uint64_t offset, std::span<const uint8_t> bytes) override { return stage.write(offset, bytes); }
  bool seal(uint64_t bytes) override {
    if (bytes != expected) {
      LOG_ERR("COMPANION", "ZIP member stage length mismatch");
      stage.abort();
      return false;
    }
    sealed = stage.seal(bytes);
    return sealed;
  }
  void abort() override {
    sealed = false;
    stage.abort();
  }
  bool isSealed() const { return sealed && stage.isSealed(); }
  const Digest& contentHash() const { return stage.contentHash(); }

 private:
  HalVerifiedFileStage stage;
  uint64_t expected = 0;
  const char* candidate = ZIP_MEMBER_CANDIDATE;
  bool sealed = false;
};
}  // namespace companion
