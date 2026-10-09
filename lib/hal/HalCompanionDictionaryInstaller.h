#pragma once

#include <Memory.h>

#include <algorithm>

#include "CompanionWorkspace.h"
#include "HalCompanionHeapAdmission.h"
#include "HalDictionaryAbortCleanup.h"
#include "HalDictionaryFinalizedContentVerification.h"
#include "HalDictionaryInstallationPreparation.h"
#include "HalDictionaryInstallationReservation.h"
#include "HalDictionaryRecoveredInstallation.h"
#include "HalDictionaryRetirementOwner.h"
#include "HalDictionaryTransferInstaller.h"

namespace companion {
// Serialized transfer owner. Borrow the live transfer/card identity and session
// wire scratch; detach from HalTransferStorage before destruction.
class HalCompanionDictionaryInstaller final : public HalDictionaryTransferInstaller {
 public:
  HalCompanionDictionaryInstaller(const Transfer& transfer, const Identity& generation, std::span<uint8_t> scratch)
      : transfer(transfer), generation(generation), scratch(scratch) {}
  bool prepare(const char* destination, const char* candidate, const ContentManifest& manifest,
               const TransferState& state, std::span<uint8_t> wire) override {
    publication.reset();
    if (!context(destination, manifest, state, wire) || !candidate || std::strcmp(candidate, TRANSFER_STAGE) ||
        state.durableOffset != state.length ||
        (state.phase != TransferPhase::Receiving && state.phase != TransferPhase::Verified))
      return fail("preparation context");
    constexpr size_t peak = sizeof(DecodeWorkspace) + sizeof(HalDictionaryIncomingExtraction) +
                            sizeof(HalDictionaryInstallationPreparation) + sizeof(ReservationOwner);
    constexpr size_t largest = std::max({sizeof(DecodeWorkspace), sizeof(HalDictionaryIncomingExtraction),
                                         sizeof(HalDictionaryInstallationPreparation), sizeof(ReservationOwner)});
    if (!admit(peak, largest)) return false;
    // The decoder/NFC window and retained coordinators exceed the task stack.
    // They are released before publication, which requires no decompression.
    auto decoder = makeUniqueNoThrow<DecodeWorkspace>();
    if (!decoder || !decoder->ready) return fail("OOM: decoder workspace");
    if (!admit()) return false;
    HalFile file;
    if (!Storage.openFileForReadReusing("COMPANION", candidate, file)) return fail("incoming archive open");
    auto incoming = makeUniqueNoThrow<HalDictionaryIncomingExtraction>(
        file, state, manifest, generation, destination, archiveScratch(), comparisonScratch(), decoder->receipts,
        &decoder->decoder, decoder->window, decoder->names, progress);
    if (!incoming) return fail("OOM: extraction owner");
    if (!admit()) return false;
    if (!incoming->prepare()) return false;
    auto prepared = makeUniqueNoThrow<HalDictionaryInstallationPreparation>(
        *incoming, file, manifest, comparisonScratch(), &decoder->decoder, decoder->window, progress);
    if (!prepared) return fail("OOM: preparation owner");
    if (!admit()) return false;
    if (!prepared->prepare(destination)) return false;
    const auto receipt = incoming->receiptParent();
    const auto plan = prepared->current();
    if (!receipt || !plan) return fail("prepared ownership");
    auto reservation = makeUniqueNoThrow<ReservationOwner>(transfer, *receipt, planScratch(), workScratch());
    if (!reservation) return fail("OOM: reservation owner");
    if (!admit()) return false;
    return reservation->reservation.reserve(*plan) == DictionaryJournalResult::Ok;
  }
  bool install(const char* destination, const ContentManifest& manifest, const TransferState& state,
               std::span<uint8_t> wire) override {
    if (!context(destination, manifest, state, wire) || state.phase != TransferPhase::Installing ||
        state.durableOffset != state.length)
      return fail("installation context");
    return publicationOwner(state, manifest) && publication->owner.install() == DictionaryJournalResult::Ok;
  }
  bool metadata(const char* destination, const ContentManifest& manifest, const TransferState& state,
                std::span<uint8_t> wire) override {
    if (state.phase == TransferPhase::Installing) return install(destination, manifest, state, wire);
    return finalize(destination, manifest, state, wire);
  }
  bool finalize(const char* destination, const ContentManifest& manifest, const TransferState& state,
                std::span<uint8_t> wire) override {
    if (!context(destination, manifest, state, wire)) return fail("finalization context");
    if (state.phase == TransferPhase::Aborted) {
      publication.reset();
      if (!admit(sizeof(HalDictionaryAbortCleanup) + sizeof(HalDictionaryZipAudit),
                 std::max(sizeof(HalDictionaryAbortCleanup), sizeof(HalDictionaryZipAudit))))
        return false;
      // Retained journal codecs and HAL handles exceed the task-local budget.
      auto aborted = makeUniqueNoThrow<HalDictionaryAbortCleanup>(transfer, planScratch(), workScratch(), progress);
      if (!aborted) return fail("OOM: abort cleanup owner");
      if (!admit()) return false;
      return aborted->run();
    }
    if (state.phase != TransferPhase::Committed || state.durableOffset != state.length)
      return fail("finalization context");
    if (!admit(sizeof(RetirementOwner), sizeof(RetirementOwner))) return false;
    auto retired = makeUniqueNoThrow<RetirementOwner>(transfer, scratch);
    if (!retired) return fail("OOM: retirement owner");
    if (!admit()) return false;
    const auto resumed = retired->owner.resume();
    if (resumed == DictionaryJournalResult::Ok) {
      publication.reset();
      return true;
    }
    if (resumed != DictionaryJournalResult::Missing) return false;
    if (!publicationOwner(state, manifest)) return false;
    const auto recovered = publication->owner.recover();
    if (recovered == DictionaryJournalResult::Missing) {
      publication.reset();
      if (!admit(sizeof(HalDictionaryFinalizedContentVerification), sizeof(HalDictionaryFinalizedContentVerification)))
        return false;
      auto verified = makeUniqueNoThrow<HalDictionaryFinalizedContentVerification>(workScratch(), progress);
      if (!verified) return fail("OOM: finalized verification owner");
      if (!admit()) return false;
      return verified->verifyRetired(destination, manifest, state);
    }
    if (recovered != DictionaryJournalResult::Ok || publication->owner.finalize() != DictionaryJournalResult::Ok)
      return false;
    const auto parent = publication->owner.installationParent();
    if (!parent || retired->owner.begin(*parent) != DictionaryJournalResult::Ok ||
        retired->owner.resume() != DictionaryJournalResult::Ok)
      return false;
    publication.reset();
    return true;
  }

 private:
  struct DecodeWorkspace {
    std::array<uint32_t, ZipNameWorkspace::WINDOW_SCALARS> scalars{};
    tinfl_decompressor decoder{};
    std::array<uint8_t, DICTIONARY_EXTRACTION_RECEIPT_SIZE> receipts{};
    ZipNameWorkspace names;
    std::span<uint8_t> window;
    bool ready = ZipNameWorkspace::borrowDecoderWindow(scalars, names, window);
  };
  struct DestinationWorkspace {
    std::array<uint32_t, 256> decoded{};
    std::array<uint32_t, 1024> normalized{};
    std::array<uint8_t, 768> wanted{}, found{};
    HalDictionaryDestinationLookup lookup{decoded, normalized, wanted, found, progress};
  };
  struct ReservationOwner {
    DestinationWorkspace destination;
    HalDictionaryMemberVerification verification;
    HalDictionaryExtractionJournalStorage storage{HalDictionaryExtractionJournalStorage::Purpose::Installation};
    DictionaryInstallationJournal journal;
    DictionaryInstallationParent parent;
    HalDictionaryInstallationReservation reservation;
    ReservationOwner(const Transfer& transfer, const DictionaryExtractionParent& extraction, std::span<uint8_t> plan,
                     std::span<uint8_t> wire)
        : verification(wire, progress),
          journal(storage, plan),
          parent(journal, extraction, transfer),
          reservation(parent, destination.lookup, verification) {}
  };
  struct PublicationOwner {
    std::array<uint8_t, DICTIONARY_EXTRACTION_RECEIPT_SIZE> receipts{};
    DestinationWorkspace destination;
    HalDictionaryMemberVerification verification;
    HalDictionaryExtractionJournalStorage extraction;
    HalDictionaryExtractionJournalStorage installation{HalDictionaryExtractionJournalStorage::Purpose::Installation};
    HalDictionaryRecoveredInstallation owner;
    PublicationOwner(const Transfer& transfer, const TransferState& state, const ContentManifest& manifest,
                     const Identity& generation, std::span<uint8_t> plan, std::span<uint8_t> wire)
        : verification(wire, progress),
          owner(transfer, state, manifest, generation, extraction, installation, receipts, plan, destination.lookup,
                verification, wire, progress) {}
  };
  struct RetirementOwner {
    std::array<uint8_t, DICTIONARY_RETIREMENT_PROOF_SIZE> proof{};
    DestinationWorkspace destination;
    HalDictionaryMemberVerification verification;
    HalDictionaryExtractionJournalStorage extraction;
    HalDictionaryExtractionJournalStorage installation{HalDictionaryExtractionJournalStorage::Purpose::Installation};
    HalDictionaryExtractionJournalStorage proofs{HalDictionaryExtractionJournalStorage::Purpose::Retirement};
    HalCompanionFileLookup lookup{progress};
    HalDictionaryRetirementOwner owner;
    RetirementOwner(const Transfer& transfer, std::span<uint8_t> wire)
        : verification(wire, progress),
          owner(transfer, extraction, installation, proofs, lookup, destination.lookup, verification, proof, wire,
                progress) {}
  };
  const Transfer& transfer;
  const Identity& generation;
  std::span<uint8_t> scratch;
  std::unique_ptr<PublicationOwner> publication;
  static constexpr size_t ARCHIVE_BYTES = 512;
  static constexpr size_t COMPARISON_BYTES = 384;
  static constexpr size_t WORK_OFFSET = DICTIONARY_INSTALLATION_PLAN_SIZE;
  static constexpr size_t MINIMUM_SCRATCH_BYTES = ARCHIVE_BYTES + COMPARISON_BYTES;
  static_assert(MINIMUM_SCRATCH_BYTES >= WORK_OFFSET + DICTIONARY_BINDING_SIZE);
  static_assert(TRANSFER_SCRATCH_SIZE >= MINIMUM_SCRATCH_BYTES);
  static_assert(COMPARISON_BYTES >= DICTIONARY_ZIP_AUDIT_SIZE);
  // Preparation completes before journal serialization reuses these wire bytes.
  std::span<uint8_t> archiveScratch() const { return scratch.first(ARCHIVE_BYTES); }
  std::span<uint8_t> comparisonScratch() const { return scratch.subspan(ARCHIVE_BYTES, COMPARISON_BYTES); }
  std::span<uint8_t> planScratch() const { return scratch.first(DICTIONARY_INSTALLATION_PLAN_SIZE); }
  std::span<uint8_t> workScratch() const { return scratch.subspan(WORK_OFFSET); }
  bool context(const char* destination, const ContentManifest& manifest, const TransferState& state,
               std::span<uint8_t> wire) const {
    return scratch.size() >= MINIMUM_SCRATCH_BYTES && wire.data() == scratch.data() && wire.size() == scratch.size() &&
           destination && transfer.current() == &state && transfer.contentManifest() == &manifest &&
           transfer.destination() == std::string_view(destination) && state.storageGeneration == generation &&
           validDictionaryInstallationBase(destination) && validDictionaryBindingManifest(manifest) &&
           matchesTransferManifest(manifest, state) && state.durableOffset <= state.length &&
           inventory_detail::nonzero(state.owner) && inventory_detail::nonzero(state.transaction) &&
           inventory_detail::nonzero(generation);
  }
  bool publicationOwner(const TransferState& state, const ContentManifest& manifest) {
    if (!publication) {
      if (!admit(sizeof(PublicationOwner), sizeof(PublicationOwner))) return false;
      publication =
          makeUniqueNoThrow<PublicationOwner>(transfer, state, manifest, generation, planScratch(), workScratch());
    }
    if (!publication) return fail("OOM: publication owner");
    if (admit()) return true;
    publication.reset();
    return false;
  }
  static bool admit(size_t bytes = 0, size_t largest = 0) { return admitCompanionHeap(bytes, largest); }
  static bool progress(void*) {
    vTaskDelay(1);
    return admit();
  }
  static bool fail(const char* operation) {
    LOG_ERR("COMPANION", "Dictionary installer %s failed", operation);
    return false;
  }
};
}  // namespace companion
