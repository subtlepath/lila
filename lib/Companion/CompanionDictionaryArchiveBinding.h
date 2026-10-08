#pragma once

#include "CompanionInventoryIndex.h"

namespace companion {
enum class DictionaryBindingResult { Missing, Found, Error };
struct DictionaryArchiveBinding {
  ContentManifest members;
  ContentManifest original;
  bool operator==(const DictionaryArchiveBinding&) const = default;
};
class DictionaryArchiveBindings {
 public:
  virtual ~DictionaryArchiveBindings() = default;
  // Installation validates the original ZIP and extracted members before
  // publishing this association. Pending, corrupt or unreadable records fail.
  virtual DictionaryBindingResult read(const char* basePath, DictionaryArchiveBinding& output) = 0;
};
inline constexpr size_t DICTIONARY_BINDING_SIZE = 8 + 32 + 2 * CONTENT_MANIFEST_SIZE + 4;
inline constexpr std::array<uint8_t, 8> DICTIONARY_BINDING_PREFIX = {'D', 'B', 'N', 'D', 1, 0, 0, 0};
inline bool validDictionaryBindingManifest(const ContentManifest& manifest) {
  return manifest.kind == ContentKind::Dictionary && manifest.formatVersion == 1 && manifest.length >= 22 &&
         manifest.length <= UINT32_MAX && manifest.logicalIdentity == Identity{} &&
         std::any_of(manifest.contentHash.begin(), manifest.contentHash.end(), [](uint8_t byte) { return byte != 0; });
}
inline size_t encodeDictionaryBinding(const Digest& basePathHash, const DictionaryArchiveBinding& binding,
                                      std::span<uint8_t> output) {
  if (output.size() < DICTIONARY_BINDING_SIZE || !validDictionaryBindingManifest(binding.members) ||
      !validDictionaryBindingManifest(binding.original))
    return 0;
  auto bytes = output.first(DICTIONARY_BINDING_SIZE);
  std::copy(DICTIONARY_BINDING_PREFIX.begin(), DICTIONARY_BINDING_PREFIX.end(), bytes.begin());
  std::copy(basePathHash.begin(), basePathHash.end(), bytes.begin() + 8);
  if (encodeRecord(binding.members, bytes.subspan(40, CONTENT_MANIFEST_SIZE)) != CONTENT_MANIFEST_SIZE ||
      encodeRecord(binding.original, bytes.subspan(40 + CONTENT_MANIFEST_SIZE, CONTENT_MANIFEST_SIZE)) !=
          CONTENT_MANIFEST_SIZE)
    return 0;
  inventory_detail::write(bytes, DICTIONARY_BINDING_SIZE - 4,
                          inventoryIndexCrc(bytes.first(DICTIONARY_BINDING_SIZE - 4)), 4);
  return DICTIONARY_BINDING_SIZE;
}
inline bool decodeDictionaryBinding(const Digest& basePathHash, std::span<const uint8_t> bytes,
                                    DictionaryArchiveBinding& output) {
  if (bytes.size() != DICTIONARY_BINDING_SIZE ||
      !std::equal(DICTIONARY_BINDING_PREFIX.begin(), DICTIONARY_BINDING_PREFIX.end(), bytes.begin()) ||
      !std::equal(basePathHash.begin(), basePathHash.end(), bytes.begin() + 8) ||
      inventoryIndexCrc(bytes.first(DICTIONARY_BINDING_SIZE - 4)) !=
          inventory_detail::read(bytes, DICTIONARY_BINDING_SIZE - 4, 4))
    return false;
  DictionaryArchiveBinding parsed;
  if (!decodeRecord(bytes.subspan(40, CONTENT_MANIFEST_SIZE), parsed.members) ||
      !decodeRecord(bytes.subspan(40 + CONTENT_MANIFEST_SIZE, CONTENT_MANIFEST_SIZE), parsed.original) ||
      !validDictionaryBindingManifest(parsed.members) || !validDictionaryBindingManifest(parsed.original))
    return false;
  output = parsed;
  return true;
}
}  // namespace companion
