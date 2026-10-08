#pragma once

#include "CompanionLegacyTintaBackupManifest.h"

namespace companion {
inline constexpr std::string_view LEGACY_TINTA_BACKUP_ROOT = "/.crosspoint/companion/legacy-tinta/";
inline constexpr size_t LEGACY_TINTA_BACKUP_DIRECTORY_SIZE = LEGACY_TINTA_BACKUP_ROOT.size() + 4 * 32 + 3 + 1;
inline constexpr size_t LEGACY_TINTA_BACKUP_PATH_SIZE = LEGACY_TINTA_BACKUP_DIRECTORY_SIZE + 24;
inline bool legacyTintaBackupDirectory(const Identity& reader, const Identity& generation, const Identity& course,
                                       const Identity& transaction, std::span<char> output) {
  if (!output.empty()) output[0] = '\0';
  if (output.size() < LEGACY_TINTA_BACKUP_DIRECTORY_SIZE || !tinta_body_detail::nonzero(reader) ||
      !tinta_body_detail::nonzero(generation) || !tinta_body_detail::nonzero(course) ||
      !tinta_body_detail::nonzero(transaction))
    return false;
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  const Identity* identities[] = {&reader, &generation, &course, &transaction};
  std::copy(LEGACY_TINTA_BACKUP_ROOT.begin(), LEGACY_TINTA_BACKUP_ROOT.end(), output.begin());
  size_t at = LEGACY_TINTA_BACKUP_ROOT.size();
  for (size_t index = 0; index < 4; ++index) {
    if (index) output[at++] = '/';
    for (const auto byte : *identities[index]) {
      output[at++] = HEX_DIGITS[byte >> 4];
      output[at++] = HEX_DIGITS[byte & 15];
    }
  }
  output[at] = '\0';
  return true;
}
enum class LegacyTintaBackupMetadata : uint8_t { Manifest, ManifestStage, Intent, IntentStage };
inline bool legacyTintaBackupNamedPath(const Identity& reader, const Identity& generation, const Identity& course,
                                       const Identity& transaction, std::string_view name, bool candidate,
                                       std::span<char> output) {
  if (!output.empty()) output[0] = '\0';
  constexpr std::string_view SUFFIX = "-next";
  const auto length = LEGACY_TINTA_BACKUP_DIRECTORY_SIZE + name.size() + (candidate ? SUFFIX.size() : 0) + 1;
  if (name.empty() || name.size() > 16 ||
      !std::all_of(name.begin(), name.end(),
                   [](char byte) { return (byte >= 'a' && byte <= 'z') || byte == '.' || byte == '-'; }) ||
      name.front() == '.' || name.back() == '.' || output.size() < length ||
      !legacyTintaBackupDirectory(reader, generation, course, transaction, output))
    return false;
  size_t at = LEGACY_TINTA_BACKUP_DIRECTORY_SIZE - 1;
  output[at++] = '/';
  std::copy(name.begin(), name.end(), output.begin() + at);
  at += name.size();
  if (candidate) {
    std::copy(SUFFIX.begin(), SUFFIX.end(), output.begin() + at);
    at += SUFFIX.size();
  }
  output[at] = '\0';
  return true;
}
inline bool legacyTintaBackupFilePath(const Identity& reader, const Identity& generation, const Identity& course,
                                      const Identity& transaction, LegacyTintaBackupRole role, bool candidate,
                                      std::span<char> output) {
  static constexpr std::string_view NAMES[] = {"reviews.log", "items.bin", "profile.bin", "lessons.bin", "readings.bin",
                                               "starred.bin", "usage.bin", "days.bin",    "session.bin"};
  if (!output.empty()) output[0] = '\0';
  const auto index = static_cast<size_t>(role);
  return index < LEGACY_TINTA_BACKUP_ROLES &&
         legacyTintaBackupNamedPath(reader, generation, course, transaction, NAMES[index], candidate, output);
}
inline bool legacyTintaBackupMetadataPath(const Identity& reader, const Identity& generation, const Identity& course,
                                          const Identity& transaction, LegacyTintaBackupMetadata kind,
                                          std::span<char> output) {
  if (!output.empty()) output[0] = '\0';
  const auto index = static_cast<unsigned>(kind);
  return index < 4 && legacyTintaBackupNamedPath(reader, generation, course, transaction,
                                                 index < 2 ? "manifest" : "intent", (index & 1) != 0, output);
}
}  // namespace companion
