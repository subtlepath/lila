#pragma once

#include <Memory.h>

#include "HalTintaAuthorityRetention.h"
#include "HalTintaDerivedJournalProof.h"
#include "HalTintaDerivedPublicationStorage.h"

namespace companion {
// Caller freezes journal, pack/catalog, learner writers and publication until return.
inline TintaPublicationResult publishProvenTintaDerived(const Identity& course, const Identity& storage,
                                                        const Digest& pack, TintaSubjectCatalog& catalog,
                                                        std::span<const uint8_t> manifestBytes,
                                                        std::span<uint8_t> scratch,
                                                        std::span<const uint8_t> previousReceipt = {}) {
  TintaDerivedManifestView manifest;
  if (!manifest.decode(manifestBytes) || scratch.size() < TINTA_DERIVED_MANIFEST_SIZE ||
      (!previousReceipt.empty() && previousReceipt.size() != TINTA_DERIVED_MANIFEST_SIZE)) {
    LOG_ERR("COMPANION", "Invalid proven Tinta publication arguments");
    return TintaPublicationResult::Invalid;
  }
  const auto overlap = [](std::span<const uint8_t> a, std::span<const uint8_t> b) {
    if (a.empty() || b.empty()) return false;
    const auto first = reinterpret_cast<uintptr_t>(a.data()), second = reinterpret_cast<uintptr_t>(b.data());
    return first >= second ? first - second < b.size() : second - first < a.size();
  };
  if (overlap(manifestBytes, scratch) || overlap(previousReceipt, scratch)) {
    LOG_ERR("COMPANION", "Overlapping proven Tinta publication buffers");
    return TintaPublicationResult::Invalid;
  }
  TintaDerivedPublicationBindings bindings;
  bindings.course = course;
  bindings.storage = storage;
  bindings.pack = pack;
  std::copy_n(manifestBytes.begin() + 52, bindings.frontier.size(), bindings.frontier.begin());
  if (!manifest.matches(course, storage, pack, bindings.frontier)) {
    LOG_ERR("COMPANION", "Proven Tinta publication binding differs");
    return TintaPublicationResult::Invalid;
  }
  // Release checkpoint audit/storage before allocating publication proof owners.
  {
    auto retention = makeUniqueNoThrow<HalTintaAuthorityRetention>();
    if (!retention) {
      LOG_ERR("COMPANION", "OOM: Tinta authority checkpoint owner");
      return TintaPublicationResult::IoError;
    }
    if (!retention->establish(manifestBytes, course, storage, pack, catalog)) return TintaPublicationResult::IoError;
  }
  // Proof and retained publication paths/handles exceed the task-local budget.
  auto proof = makeUniqueNoThrow<HalTintaDerivedJournalProof>(course, storage, pack, catalog, scratch);
  if (!proof) {
    LOG_ERR("COMPANION", "OOM: Tinta publication journal proof");
    return TintaPublicationResult::IoError;
  }
  bindings.context = proof.get();
  bindings.proveJournal = HalTintaDerivedJournalProof::callback;
  auto backend =
      makeUniqueNoThrow<HalTintaDerivedPublicationStorage>(bindings, manifestBytes, scratch, previousReceipt);
  if (!backend) {
    LOG_ERR("COMPANION", "OOM: proven Tinta publication backend");
    return TintaPublicationResult::IoError;
  }
  TintaDerivedPublication publication(*backend);
  return publication.publish(manifestBytes);
}
}  // namespace companion
