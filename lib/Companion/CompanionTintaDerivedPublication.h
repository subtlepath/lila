#pragma once

#include "CompanionTintaDerivedManifest.h"

namespace companion {
enum class TintaDerivedRole { Active, Candidate, Backup };
enum class TintaPublicationState { Missing, Matches, Other, Error };
enum class TintaPublicationResult { Ok, Invalid, Corrupt, IoError };

class TintaDerivedPublicationStorage {
 public:
  virtual ~TintaDerivedPublicationStorage() = default;
  virtual bool validateBindings(const TintaDerivedManifestView&) = 0;
  virtual bool validateCandidates(const TintaDerivedManifestView&) = 0;
  virtual TintaPublicationState intent(std::span<const uint8_t> manifest) = 0;
  virtual TintaPublicationState committed(std::span<const uint8_t> manifest) = 0;
  virtual TintaPublicationState state(TintaDerivedFile, TintaDerivedRole, const TintaDerivedManifestView&) = 0;
  // Mutations must log errors, preserve ownership, and be durable before returning true.
  virtual bool persistIntent(std::span<const uint8_t>) = 0;
  virtual bool rename(TintaDerivedFile, TintaDerivedRole from, TintaDerivedRole to) = 0;
  virtual bool remove(TintaDerivedFile, TintaDerivedRole) = 0;
  // Idempotent manifest/frontier receipt publication, with checked readback.
  virtual bool commit(std::span<const uint8_t>) = 0;
  virtual bool clearIntent() = 0;
};

class TintaDerivedPublication {
 public:
  explicit TintaDerivedPublication(TintaDerivedPublicationStorage& storage) : storage(storage) {}
  TintaPublicationResult publish(std::span<const uint8_t> bytes) {
    TintaDerivedManifestView manifest;
    if (!manifest.decode(bytes) || !storage.validateBindings(manifest)) return TintaPublicationResult::Invalid;
    const auto intent = storage.intent(bytes);
    if (intent == TintaPublicationState::Matches) return recover(bytes);
    if (intent == TintaPublicationState::Error) return TintaPublicationResult::IoError;
    if (intent != TintaPublicationState::Missing) return TintaPublicationResult::Corrupt;
    const auto committed = storage.committed(bytes);
    if (committed == TintaPublicationState::Error) return TintaPublicationResult::IoError;
    if (committed == TintaPublicationState::Matches) return TintaPublicationResult::Ok;
    if (!storage.validateCandidates(manifest)) return TintaPublicationResult::Invalid;
    for (unsigned i = 0; i < 5; ++i) {
      const auto file = static_cast<TintaDerivedFile>(i);
      const auto backup = storage.state(file, TintaDerivedRole::Backup, manifest);
      if (backup == TintaPublicationState::Error) return TintaPublicationResult::IoError;
      if (backup != TintaPublicationState::Missing) return TintaPublicationResult::Corrupt;
    }
    if (!storage.persistIntent(bytes)) return TintaPublicationResult::IoError;
    return recover(bytes);
  }
  TintaPublicationResult recover(std::span<const uint8_t> bytes) {
    TintaDerivedManifestView manifest;
    if (!manifest.decode(bytes) || !storage.validateBindings(manifest)) return TintaPublicationResult::Invalid;
    const auto intent = storage.intent(bytes);
    if (intent == TintaPublicationState::Error) return TintaPublicationResult::IoError;
    if (intent == TintaPublicationState::Missing) {
      const auto committed = storage.committed(bytes);
      if (committed == TintaPublicationState::Error) return TintaPublicationResult::IoError;
      if (committed == TintaPublicationState::Matches) return TintaPublicationResult::Ok;
    }
    if (intent != TintaPublicationState::Matches) return TintaPublicationResult::Corrupt;
    for (unsigned i = 0; i < 5; ++i) {
      const auto file = static_cast<TintaDerivedFile>(i);
      const auto candidate = storage.state(file, TintaDerivedRole::Candidate, manifest);
      const auto active = storage.state(file, TintaDerivedRole::Active, manifest);
      const auto backup = storage.state(file, TintaDerivedRole::Backup, manifest);
      if (candidate == TintaPublicationState::Error || active == TintaPublicationState::Error ||
          backup == TintaPublicationState::Error)
        return TintaPublicationResult::IoError;
      if (candidate == TintaPublicationState::Other) return TintaPublicationResult::Corrupt;
      if (active == TintaPublicationState::Matches) {
        if (candidate == TintaPublicationState::Matches && !storage.remove(file, TintaDerivedRole::Candidate))
          return TintaPublicationResult::IoError;
        continue;
      }
      if (candidate != TintaPublicationState::Matches) return TintaPublicationResult::Corrupt;
      if (active == TintaPublicationState::Other) {
        if (backup != TintaPublicationState::Missing) return TintaPublicationResult::Corrupt;
        if (!storage.rename(file, TintaDerivedRole::Active, TintaDerivedRole::Backup))
          return TintaPublicationResult::IoError;
      }
      if (!storage.rename(file, TintaDerivedRole::Candidate, TintaDerivedRole::Active))
        return TintaPublicationResult::IoError;
    }
    for (unsigned i = 0; i < 5; ++i) {
      const auto state = storage.state(static_cast<TintaDerivedFile>(i), TintaDerivedRole::Active, manifest);
      if (state == TintaPublicationState::Error) return TintaPublicationResult::IoError;
      if (state != TintaPublicationState::Matches) return TintaPublicationResult::Corrupt;
    }
    if (!storage.commit(bytes)) return TintaPublicationResult::IoError;
    for (unsigned i = 0; i < 5; ++i) {
      const auto file = static_cast<TintaDerivedFile>(i);
      const auto backup = storage.state(file, TintaDerivedRole::Backup, manifest);
      if (backup == TintaPublicationState::Error) return TintaPublicationResult::IoError;
      if (backup != TintaPublicationState::Missing && !storage.remove(file, TintaDerivedRole::Backup))
        return TintaPublicationResult::IoError;
    }
    return storage.clearIntent() ? TintaPublicationResult::Ok : TintaPublicationResult::IoError;
  }

 private:
  TintaDerivedPublicationStorage& storage;
};
}  // namespace companion
