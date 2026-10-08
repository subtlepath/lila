#pragma once

#include "../Serialization/BinaryRecordBytes.h"
#include "CompanionTintaBody.h"

namespace companion {
enum class LegacyTintaBackupRole : uint8_t {
  Reviews,
  Items,
  Profile,
  Lessons,
  Readings,
  Starred,
  Usage,
  Days,
  Session
};
inline constexpr size_t LEGACY_TINTA_BACKUP_ROLES = 9;
inline constexpr size_t LEGACY_TINTA_BACKUP_MANIFEST_SIZE = 436;
struct LegacyTintaBackupFile {
  uint64_t length = 0;
  Digest hash{};
  bool present = false;
};
// Owned input exceeds the task stack budget; retain it in a checked heap workspace.
struct LegacyTintaBackupManifest {
  Identity reader{}, generation{}, course{}, transaction{};
  std::array<LegacyTintaBackupFile, LEGACY_TINTA_BACKUP_ROLES> files{};
};
class LegacyTintaBackupManifestView {
 public:
  bool decode(std::span<const uint8_t> input) {
    bytes = {};
    if (input.size() != LEGACY_TINTA_BACKUP_MANIFEST_SIZE || input[0] != 'T' || input[1] != 'L' || input[2] != 'B' ||
        input[3] != '1' || input[70] || input[71] ||
        tinta_body_detail::read(input, 432, 4) != binary_record::crc32(input.data(), 432))
      return false;
    const auto mask = tinta_body_detail::read(input, 68, 2);
    if ((mask & ~0x1ffU) || (mask & 7) != 7) return false;
    for (size_t at = 4; at < 68; at += 16)
      if (!tinta_body_detail::nonzero(input.subspan(at, 16))) return false;
    for (size_t at = 0; at < LEGACY_TINTA_BACKUP_ROLES; ++at) {
      const auto entry = input.subspan(72 + at * 40, 40);
      if (!(mask & (1U << at))) {
        if (tinta_body_detail::nonzero(entry)) return false;
      } else if (tinta_body_detail::read(entry, 0, 8) > UINT32_MAX ||
                 (at == 0 && tinta_body_detail::read(entry, 0, 8) > 16 * 1024 * 1024) ||
                 !tinta_body_detail::nonzero(entry.subspan(8)))
        return false;
    }
    bytes = input;
    return true;
  }
  bool present(LegacyTintaBackupRole role) const {
    return !bytes.empty() && static_cast<size_t>(role) < LEGACY_TINTA_BACKUP_ROLES &&
           (tinta_body_detail::read(bytes, 68, 2) & (1U << static_cast<unsigned>(role)));
  }
  uint64_t length(LegacyTintaBackupRole role) const {
    return present(role) ? tinta_body_detail::read(bytes, 72 + static_cast<size_t>(role) * 40, 8) : 0;
  }
  std::span<const uint8_t> hash(LegacyTintaBackupRole role) const {
    return present(role) ? bytes.subspan(80 + static_cast<size_t>(role) * 40, 32) : std::span<const uint8_t>{};
  }
  std::span<const uint8_t> reader() const { return bytes.empty() ? bytes : bytes.subspan(4, 16); }
  std::span<const uint8_t> generation() const { return bytes.empty() ? bytes : bytes.subspan(20, 16); }
  std::span<const uint8_t> course() const { return bytes.empty() ? bytes : bytes.subspan(36, 16); }
  std::span<const uint8_t> transaction() const { return bytes.empty() ? bytes : bytes.subspan(52, 16); }

 private:
  std::span<const uint8_t> bytes;
};

// Output and input must not overlap; invalid arguments preserve output.
inline bool encodeLegacyTintaBackupManifest(const LegacyTintaBackupManifest& manifest, std::span<uint8_t> output) {
  if (output.size() != LEGACY_TINTA_BACKUP_MANIFEST_SIZE || !tinta_body_detail::nonzero(manifest.reader) ||
      !tinta_body_detail::nonzero(manifest.generation) || !tinta_body_detail::nonzero(manifest.course) ||
      !tinta_body_detail::nonzero(manifest.transaction))
    return false;
  uint16_t mask = 0;
  for (size_t at = 0; at < manifest.files.size(); ++at) {
    const auto& file = manifest.files[at];
    if (!file.present) {
      if (file.length || tinta_body_detail::nonzero(file.hash)) return false;
    } else {
      if (file.length > UINT32_MAX || (at == 0 && file.length > 16 * 1024 * 1024) ||
          !tinta_body_detail::nonzero(file.hash))
        return false;
      mask |= 1U << at;
    }
  }
  if ((mask & 7) != 7) return false;
  std::fill(output.begin(), output.end(), 0);
  output[0] = 'T';
  output[1] = 'L';
  output[2] = 'B';
  output[3] = '1';
  std::copy(manifest.reader.begin(), manifest.reader.end(), output.begin() + 4);
  std::copy(manifest.generation.begin(), manifest.generation.end(), output.begin() + 20);
  std::copy(manifest.course.begin(), manifest.course.end(), output.begin() + 36);
  std::copy(manifest.transaction.begin(), manifest.transaction.end(), output.begin() + 52);
  tinta_body_detail::write(output, 68, mask, 2);
  for (size_t at = 0; at < manifest.files.size(); ++at) {
    tinta_body_detail::write(output, 72 + at * 40, manifest.files[at].length, 8);
    std::copy(manifest.files[at].hash.begin(), manifest.files[at].hash.end(), output.begin() + 80 + at * 40);
  }
  tinta_body_detail::write(output, 432, binary_record::crc32(output.data(), 432), 4);
  return true;
}
}  // namespace companion
