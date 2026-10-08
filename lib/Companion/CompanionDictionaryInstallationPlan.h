#pragma once

#include "CompanionDictionaryArchiveBinding.h"
#include "CompanionDictionaryExtractionReceipt.h"
#include "CompanionInventoryPaths.h"
#include "CompanionZipPathValidation.h"

namespace companion {
enum class DictionaryInstallationPhase : uint8_t { Prepared = 1, Publishing = 2, Bound = 3, Committed = 4 };
struct DictionaryInstallationPlan {
  uint64_t revision = 0;
  DictionaryExtractionReceipt extraction;
  DictionaryArchiveBinding archives;
  std::array<char, 128> base{};
  DictionaryInstallationPhase phase = DictionaryInstallationPhase::Prepared;
  uint8_t published = 0;
  bool operator==(const DictionaryInstallationPlan&) const = default;
};
inline bool validDictionaryInstallationBase(std::string_view path) {
  if (path.size() >= 128 || !validInventoryPath(path)) return false;
  if (path.starts_with("/dictionaries/"))
    path.remove_prefix(14);
  else if (path.starts_with("/.dictionaries/"))
    path.remove_prefix(15);
  else
    return false;
  ZipPathValidation validation;
  ZipPathDetails details;
  validation.reset();
  if (!validation.consume(std::span(reinterpret_cast<const uint8_t*>(path.data()), path.size())) ||
      !validation.finish(false, details))
    return false;
  const auto slash = path.find('/');
  return slash != std::string_view::npos && slash != 0 && slash + 1 < path.size() && path.front() != '.' &&
         path[slash + 1] != '.' && path.find('/', slash + 1) == std::string_view::npos;
}
inline bool validDictionaryInstallationPlan(const DictionaryInstallationPlan& plan) {
  const auto size = strnlen(plan.base.data(), plan.base.size());
  if (!plan.revision || !validDictionaryExtractionReceipt(plan.extraction) ||
      plan.extraction.sealed != (plan.extraction.synonyms ? 15 : 7) ||
      !validDictionaryBindingManifest(plan.archives.members) ||
      !validDictionaryBindingManifest(plan.archives.original) ||
      plan.archives.original.contentHash != plan.extraction.archiveHash ||
      !validDictionaryInstallationBase(std::string_view(plan.base.data(), size)) ||
      !std::all_of(plan.base.begin() + size, plan.base.end(), [](char byte) { return byte == 0; }))
    return false;
  const uint8_t required = plan.extraction.synonyms ? 15 : 7;
  if (plan.published != 0 && plan.published != 4 && plan.published != 6 && plan.published != 7 &&
      !(required == 15 && plan.published == 15))
    return false;
  switch (plan.phase) {
    case DictionaryInstallationPhase::Prepared:
      return plan.published == 0;
    case DictionaryInstallationPhase::Publishing:
      return true;
    case DictionaryInstallationPhase::Bound:
    case DictionaryInstallationPhase::Committed:
      return plan.published == required;
  }
  return false;
}
inline constexpr size_t DICTIONARY_INSTALLATION_MANIFEST_OFFSET = 16 + DICTIONARY_EXTRACTION_RECEIPT_SIZE;
inline constexpr size_t DICTIONARY_INSTALLATION_PATH_OFFSET =
    DICTIONARY_INSTALLATION_MANIFEST_OFFSET + 2 * CONTENT_MANIFEST_SIZE + 2;
inline constexpr size_t DICTIONARY_INSTALLATION_PLAN_SIZE = DICTIONARY_INSTALLATION_PATH_OFFSET + 128 + 4;
// Session-owned working plan exceeds the task-local budget. Decode copies it
// to caller-owned output only after all fields and nested records validate.
class DictionaryInstallationPlanCodec final {
 public:
  static size_t encode(const DictionaryInstallationPlan& plan, std::span<uint8_t> output) {
    if (output.size() < DICTIONARY_INSTALLATION_PLAN_SIZE || !validDictionaryInstallationPlan(plan)) return 0;
    auto bytes = output.first(DICTIONARY_INSTALLATION_PLAN_SIZE);
    std::fill(bytes.begin(), bytes.end(), 0);
    static constexpr std::array<uint8_t, 5> PREFIX = {'D', 'I', 'N', 'S', 1};
    std::copy(PREFIX.begin(), PREFIX.end(), bytes.begin());
    bytes[5] = static_cast<uint8_t>(plan.phase);
    bytes[6] = plan.published;
    inventory_detail::write(bytes, 8, plan.revision, 8);
    encodeDictionaryExtractionReceipt(plan.extraction, bytes.subspan(16, DICTIONARY_EXTRACTION_RECEIPT_SIZE));
    encodeRecord(plan.archives.members, bytes.subspan(DICTIONARY_INSTALLATION_MANIFEST_OFFSET, CONTENT_MANIFEST_SIZE));
    encodeRecord(plan.archives.original,
                 bytes.subspan(DICTIONARY_INSTALLATION_MANIFEST_OFFSET + CONTENT_MANIFEST_SIZE, CONTENT_MANIFEST_SIZE));
    const auto length = strnlen(plan.base.data(), plan.base.size());
    inventory_detail::write(bytes, DICTIONARY_INSTALLATION_PATH_OFFSET - 2, length, 2);
    std::copy_n(plan.base.begin(), length, bytes.begin() + DICTIONARY_INSTALLATION_PATH_OFFSET);
    inventory_detail::write(bytes, bytes.size() - 4, inventoryIndexCrc(bytes.first(bytes.size() - 4)), 4);
    return bytes.size();
  }
  bool decode(std::span<const uint8_t> bytes, DictionaryInstallationPlan& output) {
    const auto parsed = inspect(bytes);
    if (!parsed) return false;
    output = *parsed;
    return true;
  }
  // Borrowed view expires on the next inspect/decode call.
  const DictionaryInstallationPlan* inspect(std::span<const uint8_t> bytes) {
    static constexpr std::array<uint8_t, 5> PREFIX = {'D', 'I', 'N', 'S', 1};
    if (bytes.size() != DICTIONARY_INSTALLATION_PLAN_SIZE || !std::equal(PREFIX.begin(), PREFIX.end(), bytes.begin()) ||
        bytes[7] != 0 ||
        inventoryIndexCrc(bytes.first(bytes.size() - 4)) != inventory_detail::read(bytes, bytes.size() - 4, 4))
      return nullptr;
    const auto length = inventory_detail::read(bytes, DICTIONARY_INSTALLATION_PATH_OFFSET - 2, 2);
    if (!length || length >= working.base.size() ||
        !std::all_of(bytes.begin() + DICTIONARY_INSTALLATION_PATH_OFFSET + length,
                     bytes.begin() + DICTIONARY_INSTALLATION_PATH_OFFSET + 128,
                     [](uint8_t byte) { return byte == 0; }) ||
        !decodeDictionaryExtractionReceipt(bytes.subspan(16, DICTIONARY_EXTRACTION_RECEIPT_SIZE), working.extraction) ||
        !decodeRecord(bytes.subspan(DICTIONARY_INSTALLATION_MANIFEST_OFFSET, CONTENT_MANIFEST_SIZE),
                      working.archives.members) ||
        !decodeRecord(
            bytes.subspan(DICTIONARY_INSTALLATION_MANIFEST_OFFSET + CONTENT_MANIFEST_SIZE, CONTENT_MANIFEST_SIZE),
            working.archives.original))
      return nullptr;
    working.revision = inventory_detail::read(bytes, 8, 8);
    working.phase = static_cast<DictionaryInstallationPhase>(bytes[5]);
    working.published = bytes[6];
    working.base.fill(0);
    std::copy_n(bytes.begin() + DICTIONARY_INSTALLATION_PATH_OFFSET, length, working.base.begin());
    if (strnlen(working.base.data(), working.base.size()) != length || !validDictionaryInstallationPlan(working))
      return nullptr;
    return &working;
  }

 private:
  DictionaryInstallationPlan working;
};
}  // namespace companion
