#pragma once

#include <Logging.h>

#include <algorithm>

#include "CompanionIdentity.h"
#include "CompanionTintaJournal.h"

namespace companion {
enum class LegacyBookmarkDecision : uint8_t { Undecided, Associate, LeaveUnassociated };
inline constexpr bool validLegacyBookmarkDecision(LegacyBookmarkDecision decision) {
  return decision == LegacyBookmarkDecision::Undecided || decision == LegacyBookmarkDecision::Associate ||
         decision == LegacyBookmarkDecision::LeaveUnassociated;
}
struct ReaderBookmarkBinding {
  Digest edition{};
  Identity device{}, storageGeneration{};
  bool ready = false;
  Identity conflict{};
  bool legacyAssociationRequired = false;
  LegacyBookmarkDecision legacyDecision = LegacyBookmarkDecision::Undecided;
};

inline TintaJournalResult validateBookmarkAssociationBinding(IdentityStorage& identities, const Digest& edition,
                                                             ReaderBookmarkBinding& binding, IdentityState& state) {
  if (binding.ready || !binding.legacyAssociationRequired ||
      binding.legacyDecision != LegacyBookmarkDecision::Undecided || edition != binding.edition ||
      !tinta_body_detail::nonzero(edition) || tinta_body_detail::nonzero(binding.conflict)) {
    binding.ready = false;
    binding.legacyAssociationRequired = false;
    binding.legacyDecision = LegacyBookmarkDecision::Undecided;
    binding.conflict = {};
    LOG_ERR("COMPANION", "Bookmark legacy association restore required");
    return TintaJournalResult::Invalid;
  }
  const auto inspected = inspectIdentity(identities, state);
  if (inspected == IdentityInspectionResult::Ok && state.device == binding.device &&
      state.storageGeneration == binding.storageGeneration)
    return TintaJournalResult::Ok;
  binding.ready = false;
  binding.legacyAssociationRequired = false;
  binding.legacyDecision = LegacyBookmarkDecision::Undecided;
  binding.conflict = {};
  LOG_ERR("COMPANION", "Bookmark association card binding changed: %u", static_cast<unsigned>(inspected));
  return inspected == IdentityInspectionResult::IoError ? TintaJournalResult::IoError : TintaJournalResult::Invalid;
}

inline TintaJournalResult prepareBookmarkReaderIdentity(IdentityStorage& identities, const Digest& edition,
                                                        IdentityState& state) {
  if (!std::any_of(edition.begin(), edition.end(), [](uint8_t byte) { return byte != 0; })) {
    LOG_ERR("COMPANION", "Bookmark edition proof missing");
    return TintaJournalResult::Invalid;
  }
  const auto inspected = inspectIdentity(identities, state);
  if (inspected == IdentityInspectionResult::Ok) return TintaJournalResult::Ok;
  if (inspected != IdentityInspectionResult::Unavailable && inspected != IdentityInspectionResult::WrongStorage) {
    LOG_ERR("COMPANION", "Bookmark identity inspection refused: %u", static_cast<unsigned>(inspected));
    return inspected == IdentityInspectionResult::IoError ? TintaJournalResult::IoError : TintaJournalResult::Corrupt;
  }
  // Restore may initialize a new card; edit validation never provisions identities.
  const auto provisioned = provisionIdentity(identities, state);
  if (provisioned == IdentityResult::Ok) return TintaJournalResult::Ok;
  LOG_ERR("COMPANION", "Bookmark identity provisioning refused: %u", static_cast<unsigned>(provisioned));
  if (provisioned == IdentityResult::Exhausted) return TintaJournalResult::Exhausted;
  if (provisioned == IdentityResult::Corrupt || provisioned == IdentityResult::WrongHardware)
    return TintaJournalResult::Corrupt;
  return TintaJournalResult::IoError;
}

inline TintaJournalResult validateBookmarkReaderBinding(IdentityStorage& identities, const Digest& edition,
                                                        ReaderBookmarkBinding& binding, IdentityState& state) {
  if (!binding.ready || binding.legacyAssociationRequired || edition != binding.edition ||
      !std::any_of(edition.begin(), edition.end(), [](uint8_t byte) { return byte != 0; })) {
    binding.ready = false;
    if (edition != binding.edition) {
      binding.conflict = {};
      binding.legacyAssociationRequired = false;
      binding.legacyDecision = LegacyBookmarkDecision::Undecided;
    }
    LOG_ERR("COMPANION", "Bookmark reader restore required");
    return TintaJournalResult::Invalid;
  }
  const auto inspected = inspectIdentity(identities, state);
  if (inspected == IdentityInspectionResult::Ok && state.device == binding.device &&
      state.storageGeneration == binding.storageGeneration)
    return TintaJournalResult::Ok;
  binding.ready = false;
  binding.conflict = {};
  binding.legacyAssociationRequired = false;
  binding.legacyDecision = LegacyBookmarkDecision::Undecided;
  LOG_ERR("COMPANION", "Bookmark reader card binding changed: %u", static_cast<unsigned>(inspected));
  return inspected == IdentityInspectionResult::IoError ? TintaJournalResult::IoError : TintaJournalResult::Invalid;
}
inline TintaJournalResult validateBookmarkChoiceBinding(IdentityStorage& identities, const Digest& edition,
                                                        ReaderBookmarkBinding& binding, IdentityState& state) {
  if (binding.legacyAssociationRequired || !validLegacyBookmarkDecision(binding.legacyDecision) ||
      edition != binding.edition ||
      !std::any_of(edition.begin(), edition.end(), [](uint8_t byte) { return byte != 0; }) ||
      !std::any_of(binding.conflict.begin(), binding.conflict.end(), [](uint8_t byte) { return byte != 0; })) {
    binding.ready = false;
    binding.conflict = {};
    binding.legacyAssociationRequired = false;
    binding.legacyDecision = LegacyBookmarkDecision::Undecided;
    LOG_ERR("COMPANION", "Bookmark conflict restore required");
    return TintaJournalResult::Invalid;
  }
  const auto inspected = inspectIdentity(identities, state);
  if (inspected == IdentityInspectionResult::Ok && state.device == binding.device &&
      state.storageGeneration == binding.storageGeneration)
    return TintaJournalResult::Ok;
  binding.ready = false;
  binding.conflict = {};
  binding.legacyAssociationRequired = false;
  binding.legacyDecision = LegacyBookmarkDecision::Undecided;
  LOG_ERR("COMPANION", "Bookmark conflict card binding changed: %u", static_cast<unsigned>(inspected));
  return inspected == IdentityInspectionResult::IoError ? TintaJournalResult::IoError : TintaJournalResult::Invalid;
}
}  // namespace companion
