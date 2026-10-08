#pragma once

#include "CompanionInventoryIndex.h"
#include "CompanionTransfer.h"

namespace companion {
inline constexpr char DICTIONARY_MEMBER_CANDIDATE[] = "/.crosspoint/companion/dictionary-member-next";
inline constexpr char DICTIONARY_CACHE_CANDIDATE[] = "/.crosspoint/companion/dictionary-next";
enum class DictionaryCacheCheck { Missing, Valid, Corrupt, Collision, IoError };
enum class DictionaryCacheResult { Ok, Missing, Invalid, Corrupt, IoError };
class DictionaryCacheStorage {
 public:
  virtual ~DictionaryCacheStorage() = default;
  virtual bool prepare() = 0;
  // Corrupt means successfully read regular-file bytes/length mismatch.
  // I/O errors and directory collisions must never be classified as Corrupt.
  virtual DictionaryCacheCheck inspect(const char* path, const ContentManifest& manifest,
                                       std::span<uint8_t> scratch) = 0;
  virtual bool rename(const char* from, const char* to) = 0;
  virtual bool remove(const char* path) = 0;  // Missing is success; directories are preserved.
};
// Immutable cache additions precede inventory publication. The candidate path
// is never recorded in inventory; an interrupted build may discard/rebuild it.
class DictionaryCachePublication final {
 public:
  DictionaryCachePublication(DictionaryCacheStorage& storage, std::span<uint8_t> scratch)
      : storage(storage), scratch(scratch) {}
  const char* publishedPath() const { return ready ? target.data() : nullptr; }
  DictionaryCacheResult find(const ContentManifest& manifest) {
    ready = false;
    if (!arguments(manifest)) return DictionaryCacheResult::Invalid;
    if (!storage.prepare()) return DictionaryCacheResult::IoError;
    return checked(storage.inspect(target.data(), manifest, scratch));
  }
  DictionaryCacheResult publish(const ContentManifest& manifest) {
    ready = false;
    if (!arguments(manifest)) return DictionaryCacheResult::Invalid;
    if (!storage.prepare()) return DictionaryCacheResult::IoError;
    const auto candidate = storage.inspect(DICTIONARY_CACHE_CANDIDATE, manifest, scratch);
    if (candidate == DictionaryCacheCheck::IoError) return DictionaryCacheResult::IoError;
    if (candidate == DictionaryCacheCheck::Corrupt || candidate == DictionaryCacheCheck::Collision)
      return DictionaryCacheResult::Corrupt;
    const auto existing = storage.inspect(target.data(), manifest, scratch);
    if (existing == DictionaryCacheCheck::IoError) return DictionaryCacheResult::IoError;
    if (candidate == DictionaryCacheCheck::Missing) return checked(existing);
    if (candidate != DictionaryCacheCheck::Valid) return DictionaryCacheResult::Invalid;
    if (existing == DictionaryCacheCheck::Collision) return DictionaryCacheResult::Corrupt;
    if (existing == DictionaryCacheCheck::Valid) {
      if (!storage.remove(DICTIONARY_CACHE_CANDIDATE)) return DictionaryCacheResult::IoError;
    } else {
      // A valid candidate is required before repairing a known-bad cache copy.
      // The original installed member files are never changed by this class.
      if (existing == DictionaryCacheCheck::Corrupt && !storage.remove(target.data()))
        return DictionaryCacheResult::IoError;
      if (existing != DictionaryCacheCheck::Missing && existing != DictionaryCacheCheck::Corrupt)
        return DictionaryCacheResult::Invalid;
      if (!storage.rename(DICTIONARY_CACHE_CANDIDATE, target.data())) return DictionaryCacheResult::IoError;
    }
    return checked(storage.inspect(target.data(), manifest, scratch));
  }
  // Call only after inventory recovery, with the serialized cache owner.
  // Candidate files are private, unpublished scratch, and are rebuilt from
  // validated members. This must not clean retained original archive blobs.
  DictionaryCacheResult discardUnpublishedCandidate() {
    ready = false;
    return storage.prepare() && storage.remove(DICTIONARY_CACHE_CANDIDATE) ? DictionaryCacheResult::Ok
                                                                           : DictionaryCacheResult::IoError;
  }

 private:
  DictionaryCacheStorage& storage;
  std::span<uint8_t> scratch;
  std::array<char, 112> target{};
  bool ready = false;
  static constexpr char PREFIX[] = "/.crosspoint/companion/dictionary-";
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  static_assert(sizeof(PREFIX) + Digest{}.size() * 2 + 4 <= 112);
  bool arguments(const ContentManifest& manifest) {
    if (scratch.size() < 64 || manifest.kind != ContentKind::Dictionary || manifest.formatVersion != 1 ||
        manifest.length < 22 || manifest.length > UINT32_MAX || inventory_detail::nonzero(manifest.logicalIdentity) ||
        !std::any_of(manifest.contentHash.begin(), manifest.contentHash.end(), [](uint8_t byte) { return byte != 0; }))
      return false;
    std::copy_n(PREFIX, sizeof(PREFIX) - 1, target.begin());
    size_t at = sizeof(PREFIX) - 1;
    for (const auto byte : manifest.contentHash) {
      target[at++] = HEX_DIGITS[byte >> 4];
      target[at++] = HEX_DIGITS[byte & 15];
    }
    std::copy_n(".zip", 5, target.begin() + at);
    return true;
  }
  DictionaryCacheResult checked(DictionaryCacheCheck check) {
    if (check == DictionaryCacheCheck::Valid) {
      ready = true;
      return DictionaryCacheResult::Ok;
    }
    if (check == DictionaryCacheCheck::Missing) return DictionaryCacheResult::Missing;
    if (check == DictionaryCacheCheck::IoError) return DictionaryCacheResult::IoError;
    return DictionaryCacheResult::Corrupt;
  }
};
}  // namespace companion
