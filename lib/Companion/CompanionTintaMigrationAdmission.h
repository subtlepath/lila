#pragma once

#include "CompanionJournalMergeIntent.h"

namespace companion {
inline constexpr size_t TINTA_MIGRATION_ADMISSION_SIZE = 256;
// Retain off-stack; admission also requires authenticated ownership and verified
// immutable backup/native-source hashes before publishing any journal merge.
struct TintaMigrationAdmission {
  JournalMergeIntent merge;
  Identity course{}, backupTransaction{}, reader{};
  Digest resource{}, backupManifest{};
  bool operator==(const TintaMigrationAdmission&) const = default;
};
inline bool validTintaMigrationAdmission(const TintaMigrationAdmission& value) {
  return validJournalMergeIntent(value.merge) && value.merge.merged.count > value.merge.previous.count &&
         tinta_body_detail::nonzero(value.course) && tinta_body_detail::nonzero(value.backupTransaction) &&
         tinta_body_detail::nonzero(value.reader) && tinta_body_detail::nonzero(value.resource) &&
         tinta_body_detail::nonzero(value.backupManifest);
}
inline bool encodeTintaMigrationAdmission(const TintaMigrationAdmission& value, std::span<uint8_t> output) {
  if (output.size() != TINTA_MIGRATION_ADMISSION_SIZE || !validTintaMigrationAdmission(value)) return false;
  const auto begin = reinterpret_cast<uintptr_t>(output.data());
  const auto object = reinterpret_cast<uintptr_t>(&value);
  if (begin < object + sizeof(value) && object < begin + output.size()) return false;
  output[0] = 'T';
  output[1] = 'M';
  output[2] = 'A';
  output[3] = 1;
  if (!encodeJournalMergeIntent(value.merge, output.subspan(4, JOURNAL_MERGE_INTENT_SIZE))) return false;
  std::copy(value.course.begin(), value.course.end(), output.begin() + 140);
  std::copy(value.resource.begin(), value.resource.end(), output.begin() + 156);
  std::copy(value.backupTransaction.begin(), value.backupTransaction.end(), output.begin() + 188);
  std::copy(value.reader.begin(), value.reader.end(), output.begin() + 204);
  std::copy(value.backupManifest.begin(), value.backupManifest.end(), output.begin() + 220);
  tinta_body_detail::write(output, 252, binary_record::crc32(output.data(), 252), 4);
  return true;
}
// Decode into a retained owner; validation finishes before changing output.
inline bool decodeTintaMigrationAdmission(std::span<const uint8_t> input, TintaMigrationAdmission& output) {
  if (input.size() != TINTA_MIGRATION_ADMISSION_SIZE || input[0] != 'T' || input[1] != 'M' || input[2] != 'A' ||
      input[3] != 1 || tinta_body_detail::read(input, 252, 4) != binary_record::crc32(input.data(), 252))
    return false;
  const auto begin = reinterpret_cast<uintptr_t>(input.data());
  const auto object = reinterpret_cast<uintptr_t>(&output);
  if (begin < object + sizeof(output) && object < begin + input.size()) return false;
  JournalMergeIntent merge;
  if (!decodeJournalMergeIntent(input.subspan(4, JOURNAL_MERGE_INTENT_SIZE), merge) ||
      merge.merged.count <= merge.previous.count)
    return false;
  for (const auto bytes : {input.subspan(140, 16), input.subspan(156, 32), input.subspan(188, 16),
                           input.subspan(204, 16), input.subspan(220, 32)})
    if (!tinta_body_detail::nonzero(bytes)) return false;
  output.merge = merge;
  std::copy_n(input.begin() + 140, 16, output.course.begin());
  std::copy_n(input.begin() + 156, 32, output.resource.begin());
  std::copy_n(input.begin() + 188, 16, output.backupTransaction.begin());
  std::copy_n(input.begin() + 204, 16, output.reader.begin());
  std::copy_n(input.begin() + 220, 32, output.backupManifest.begin());
  return true;
}
}  // namespace companion
