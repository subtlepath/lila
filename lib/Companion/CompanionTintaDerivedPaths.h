#pragma once

#include "CompanionCourseStatePaths.h"
#include "CompanionTintaDerivedPublication.h"

namespace companion {
enum class TintaDerivedRecord { Intent, IntentStage, Receipt, ReceiptStage };
inline constexpr std::string_view TINTA_DERIVED_FILE_NAMES[5][3] = {
    {"items.bin", "items.sync", "items.sync-old"},
    {"reviews.log", "reviews.sync", "reviews.sync-old"},
    {"lessons.bin", "lessons.sync", "lessons.sync-old"},
    {"readings.bin", "readings.sync", "readings.sync-old"},
    {"days.bin", "days.sync", "days.sync-old"}};
inline constexpr std::string_view TINTA_DERIVED_RECORD_NAMES[] = {"sync-intent", "sync-intent-stage", "sync-receipt",
                                                                  "sync-receipt-stage"};

inline bool tintaDerivedFilePath(const Identity& course, TintaDerivedFile file, TintaDerivedRole role,
                                 std::span<char> output) {
  if (!output.empty()) output[0] = '\0';
  const auto index = static_cast<size_t>(file), location = static_cast<size_t>(role);
  if (index >= 5 || location >= 3) return false;
  return courseStatePath(course, TINTA_DERIVED_FILE_NAMES[index][location], output);
}
inline bool tintaDerivedRecordPath(const Identity& course, TintaDerivedRecord record, std::span<char> output) {
  if (!output.empty()) output[0] = '\0';
  const auto index = static_cast<size_t>(record);
  if (index >= 4) return false;
  return courseStatePath(course, TINTA_DERIVED_RECORD_NAMES[index], output);
}
}  // namespace companion
