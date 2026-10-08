#pragma once

#include <Memory.h>

#include "HalTintaDerivedPublicationStorage.h"

namespace companion {
enum class TintaDerivedRecoveryResult { NoPending, Recovered, Invalid, IoError, OutOfMemory };

inline TintaDerivedRecoveryResult recoverTintaDerivedPublication(const TintaDerivedPublicationBindings& bindings,
                                                                 HalTintaDerivedRecordReader& reader,
                                                                 std::span<uint8_t> pendingBuffer,
                                                                 std::span<uint8_t> previousBuffer,
                                                                 std::span<uint8_t> scratch) {
  const auto overlap = [](std::span<const uint8_t> a, std::span<const uint8_t> b) {
    const auto first = reinterpret_cast<uintptr_t>(a.data()), second = reinterpret_cast<uintptr_t>(b.data());
    return first >= second ? first - second < b.size() : second - first < a.size();
  };
  if (!reader.matchesCourse(bindings.course) || pendingBuffer.size() < TINTA_DERIVED_MANIFEST_SIZE ||
      previousBuffer.size() < TINTA_DERIVED_MANIFEST_SIZE || scratch.size() < TINTA_DERIVED_MANIFEST_SIZE ||
      overlap(pendingBuffer, previousBuffer) || overlap(pendingBuffer, scratch) || overlap(previousBuffer, scratch)) {
    LOG_ERR("COMPANION", "Invalid Tinta derived recovery buffers or course");
    return TintaDerivedRecoveryResult::Invalid;
  }
  const auto pending = reader.load(TintaDerivedRecord::Intent, pendingBuffer);
  if (pending == TintaDerivedRecordLoad::Missing) return TintaDerivedRecoveryResult::NoPending;
  if (pending == TintaDerivedRecordLoad::Invalid) return TintaDerivedRecoveryResult::Invalid;
  if (pending == TintaDerivedRecordLoad::IoError) return TintaDerivedRecoveryResult::IoError;
  const auto receipt = reader.load(TintaDerivedRecord::Receipt, previousBuffer);
  if (receipt == TintaDerivedRecordLoad::Invalid) return TintaDerivedRecoveryResult::Invalid;
  if (receipt == TintaDerivedRecordLoad::IoError) return TintaDerivedRecoveryResult::IoError;
  const auto previous = receipt == TintaDerivedRecordLoad::Loaded
                            ? std::span<const uint8_t>(previousBuffer.first(TINTA_DERIVED_MANIFEST_SIZE))
                            : std::span<const uint8_t>{};
  // Retained HAL handles/path buffers exceed the small task-local budget.
  auto backend = makeUniqueNoThrow<HalTintaDerivedPublicationStorage>(
      bindings, pendingBuffer.first(TINTA_DERIVED_MANIFEST_SIZE), scratch, previous);
  if (!backend) {
    LOG_ERR("COMPANION", "OOM: Tinta derived publication recovery session");
    return TintaDerivedRecoveryResult::OutOfMemory;
  }
  TintaDerivedPublication publication(*backend);
  const auto result = publication.recover(pendingBuffer.first(TINTA_DERIVED_MANIFEST_SIZE));
  if (result == TintaPublicationResult::Ok) return TintaDerivedRecoveryResult::Recovered;
  LOG_ERR("COMPANION", "Tinta derived recovery rejected: %u", static_cast<unsigned>(result));
  return result == TintaPublicationResult::IoError ? TintaDerivedRecoveryResult::IoError
                                                   : TintaDerivedRecoveryResult::Invalid;
}
}  // namespace companion
