#pragma once

#include "../Serialization/BinaryRecordBytes.h"
#include "CompanionJournalMigrationPublication.h"
#include "CompanionTintaBody.h"

namespace companion {
inline constexpr size_t JOURNAL_MIGRATION_INTENT_SIZE = 64;
inline bool validJournalMigrationIntent(const JournalMigrationIntent& intent) {
  return intent.count <= UINT32_MAX / 1024 && tinta_body_detail::nonzero(intent.frontier);
}
inline bool encodeJournalMigrationIntent(const JournalMigrationIntent& intent, std::span<uint8_t> output) {
  if (output.size() != JOURNAL_MIGRATION_INTENT_SIZE || !validJournalMigrationIntent(intent)) return false;
  std::fill(output.begin(), output.end(), 0);
  output[0] = 'J';
  output[1] = 'M';
  output[2] = 'G';
  output[3] = 1;
  tinta_body_detail::write(output, 4, intent.count, 4);
  std::copy(intent.frontier.begin(), intent.frontier.end(), output.begin() + 8);
  output[40] = 9;
  output[41] = 10;
  tinta_body_detail::write(output, 60, binary_record::crc32(output.data(), 60), 4);
  return true;
}
inline bool decodeJournalMigrationIntent(std::span<const uint8_t> bytes, JournalMigrationIntent& output) {
  if (bytes.size() != JOURNAL_MIGRATION_INTENT_SIZE || bytes[0] != 'J' || bytes[1] != 'M' || bytes[2] != 'G' ||
      bytes[3] != 1 || bytes[40] != 9 || bytes[41] != 10 ||
      std::any_of(bytes.begin() + 42, bytes.begin() + 60, [](uint8_t byte) { return byte != 0; }) ||
      tinta_body_detail::read(bytes, 60, 4) != binary_record::crc32(bytes.data(), 60))
    return false;
  JournalMigrationIntent intent;
  intent.count = static_cast<uint32_t>(tinta_body_detail::read(bytes, 4, 4));
  std::copy_n(bytes.begin() + 8, 32, intent.frontier.begin());
  if (!validJournalMigrationIntent(intent)) return false;
  output = intent;
  return true;
}
}  // namespace companion
