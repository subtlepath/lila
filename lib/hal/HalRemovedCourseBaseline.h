#pragma once

#include "CompanionCourseContext.h"
#include "CompanionTransferDeclaration.h"
#include "HalCompletedContentRemovals.h"
#include "HalContentRemovalJournalStorage.h"
#include "HalCourseRemovalBaseline.h"
#include "HalCourseRemovalMetadata.h"
#include "HalCourseRemovalPlanStorage.h"
#include "HalCourseRemovalProofStorage.h"
#include "HalCourseStateIsolation.h"

namespace companion {
// Retain off stack while all namespace/state writers remain excluded. The loan
// ends on close, another open, permission loss or destruction. Opening is read-only;
// retirement removes only verified obsolete proof metadata.
class HalRemovedCourseBaseline final {
 public:
  using Permission = bool (*)(void*);
  HalRemovedCourseBaseline(const Identity& generation, std::span<uint8_t> io, Permission permitted, void* context)
      : generation(generation),
        permitted(permitted),
        context(context),
        metadata(permitted, context),
        journal(journalStorage, journalBytes),
        proofs(proofBytes, permitted, context),
        completions(completionBytes),
        plans(comparisonBytes),
        isolation(metadata, io, permitted, context),
        baseline(journal, io, permitted, context),
        io(io) {}
  bool open(const ContentManifest& candidate) {
    ready = false;
    if (!closeReaders() || !guard() || generation == Identity{} || io.size() < COURSE_BINDING_SIZE ||
        !validCourseBinding(candidate) || candidate.formatVersion != 1 || candidate.contentHash == Digest{})
      return fail("admission");
    bool present = false;
    if (readCourseBinding(metadata, COURSE_BINDING_PATH, io, binding, present) != CourseBindingResult::Ok || !guard() ||
        !present || binding.logicalIdentity != candidate.logicalIdentity || binding.formatVersion != 1)
      return fail("retained binding");
    uint64_t length = 0;
    for (const auto* path : {COURSE_BINDING_STAGE, COURSE_BINDING_BACKUP, COURSE_REMOVAL_PROOF_STAGE})
      if (!guard() || metadata.stat(path, length) != FileStatus::Missing || !guard())
        return fail("unfinished publication");
    if (proofs.load(generation, proof) != CourseRemovalProofStorageResult::Ok || !guard() ||
        completions.load(proof.request, completed) != CompletedRemovalResult::Ok || !guard() ||
        !completedCourseRemovalProof(proof, completed, binding, generation) ||
        plans.load(proof.planHash, planBytes, plan) != CourseRemovalPlanStorageResult::Ok || !guard() ||
        plan.request != proof.request || !isolation.verify(binding.logicalIdentity) || !guard() ||
        !baseline.verifyCompleted(proof, completed, binding, generation) || !guard())
      return fail("completed baseline proof");
    ready = true;
    return true;
  }
  CourseContextResult inspectCurrentCourse(ContentManifest& output, CourseContextSource& source) {
    ready = false;
    if (!closeReaders() || !guard() || generation == Identity{} || io.size() < COURSE_BINDING_SIZE)
      return CourseContextResult::IoError;
    bool present = false;
    const auto result = readCourseBinding(metadata, COURSE_BINDING_PATH, io, binding, present);
    if (!guard() || result == CourseBindingResult::IoError) return CourseContextResult::IoError;
    if (result != CourseBindingResult::Ok) return CourseContextResult::Corrupt;
    uint64_t size = 0;
    for (const auto* path : {COURSE_BINDING_STAGE, COURSE_BINDING_BACKUP, COURSE_REMOVAL_PROOF_STAGE}) {
      const auto status = metadata.stat(path, size);
      if (!guard() || status == FileStatus::Error) return CourseContextResult::IoError;
      if (status != FileStatus::Missing) return CourseContextResult::Busy;
    }
    const auto recovered = journal.recover(generation);
    if (!guard()) return CourseContextResult::IoError;
    if (recovered == ContentRemovalJournalResult::Ok) return CourseContextResult::Busy;
    if (recovered != ContentRemovalJournalResult::Missing) return CourseContextResult::Corrupt;
    const auto active = metadata.stat(ACTIVE_COURSE_PATH, size);
    if (!guard() || active == FileStatus::Error) return CourseContextResult::IoError;
    if (!present) {
      if (active == FileStatus::Present) return CourseContextResult::Unsupported;
      if (metadata.stat(COURSE_REMOVAL_PROOF_PATH, size) != FileStatus::Missing || !guard())
        return CourseContextResult::Corrupt;
      return closeReaders() && guard() ? CourseContextResult::Missing : CourseContextResult::IoError;
    }
    const ContentManifest candidate = binding;
    if (!validCourseBinding(candidate) || candidate.formatVersion != 1 || candidate.contentHash == Digest{})
      return CourseContextResult::Corrupt;
    CourseContextSource selected = CourseContextSource::Live;
    if (active == FileStatus::Present) {
      if (metadata.stat(COURSE_REMOVAL_PROOF_PATH, size) != FileStatus::Missing || !guard())
        return CourseContextResult::Busy;
      if (!verifyInstalled(candidate)) return CourseContextResult::Corrupt;
    } else {
      if (!open(candidate)) return CourseContextResult::Corrupt;
      const auto* proven = manifest();
      if (!proven || *proven != candidate) {
        closeReaders();
        return CourseContextResult::Corrupt;
      }
      selected = CourseContextSource::Removed;
    }
    if (!closeReaders() || !guard()) return CourseContextResult::IoError;
    output = candidate;
    source = selected;
    return CourseContextResult::Ok;
  }
  bool retireInstalled(const ContentManifest& installed, const TransferState& state) {
    ready = false;
    if (!closeReaders() || !guard() || generation == Identity{} || state.storageGeneration != generation ||
        !validCourseBinding(installed) || installed.formatVersion != 1 || !matchesTransferManifest(installed, state) ||
        state.owner == Identity{} || state.transaction == Identity{} || state.phase != TransferPhase::Committed ||
        state.durableOffset != state.length || io.size() < COURSE_BINDING_SIZE)
      return fail("retirement admission");
    bool present = false;
    if (readCourseBinding(metadata, COURSE_BINDING_PATH, io, binding, present) != CourseBindingResult::Ok || !present ||
        binding != installed || !guard())
      return fail("installed binding");
    uint64_t size = 0;
    for (const auto* path : {COURSE_BINDING_STAGE, COURSE_BINDING_BACKUP, COURSE_REMOVAL_PROOF_STAGE})
      if (metadata.stat(path, size) != FileStatus::Missing || !guard()) return fail("retirement publication");
    if (journal.recover(generation) != ContentRemovalJournalResult::Missing || !guard())
      return fail("retirement journal");
    if (!verifyInstalled(installed)) return fail("installed bytes");
    const auto result = proofs.load(generation, proof);
    if (!guard()) return fail("retirement permission");
    if (result == CourseRemovalProofStorageResult::Missing) return closeReaders() && guard();
    if (result != CourseRemovalProofStorageResult::Ok ||
        proof.request.manifest.logicalIdentity != installed.logicalIdentity ||
        completions.load(proof.request, completed) != CompletedRemovalResult::Ok || !guard() ||
        plans.load(proof.planHash, planBytes, plan) != CourseRemovalPlanStorageResult::Ok ||
        plan.request != proof.request || !guard() || !isolation.verify(installed.logicalIdentity) || !guard() ||
        !baseline.verifyReinstalled(proof, completed, installed, generation) || !guard() || !closeReaders() || !guard())
      return fail("retirement evidence");
    // The serialized owner excludes proof writers until checked deletion completes.
    if (!Storage.remove(COURSE_REMOVAL_PROOF_PATH) || !guard() ||
        metadata.stat(COURSE_REMOVAL_PROOF_PATH, size) != FileStatus::Missing || !guard())
      return fail("proof retirement");
    return closeReaders() && guard();
  }
  const char* path() const {
    if (!ready) return nullptr;
    if (!guard()) {
      ready = false;
      return nullptr;
    }
    const auto* result = baseline.path();
    if (!result) ready = false;
    return result;
  }
  const ContentManifest* manifest() const { return path() ? &binding : nullptr; }
  bool closeReaders() {
    ready = false;
    const bool metadataClosed = metadata.closeReaders();
    const bool proofClosed = proofs.closeReaders();
    const bool completionClosed = completions.closeReaders();
    const bool planClosed = plans.closeReaders();
    const bool stateClosed = isolation.closeReaders();
    const bool baselineClosed = baseline.closeReaders();
    const bool installedClosed = !installedFile.isOpen() || installedFile.close();
    return metadataClosed && proofClosed && completionClosed && planClosed && stateClosed && baselineClosed &&
           installedClosed;
  }

 private:
  HalFile installedFile;
  Identity generation;
  Permission permitted;
  void* context;
  HalCourseRemovalMetadata metadata;
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalBytes{}, proofBytes{}, completionBytes{};
  std::array<uint8_t, REMOVAL_STAGE_CLAIM_SIZE> comparisonBytes{};
  std::array<uint8_t, COURSE_REMOVAL_PLAN_SIZE> planBytes{};
  HalContentRemovalJournalStorage journalStorage;
  ContentRemovalJournal journal;
  HalCourseRemovalProofStorage proofs;
  HalCompletedContentRemovals completions;
  HalCourseRemovalPlanStorage plans;
  HalCourseStateIsolation isolation;
  HalCourseRemovalBaseline baseline;
  std::span<uint8_t> io;
  ContentManifest binding;
  ContentRemovalRecord proof, completed;
  CourseRemovalPlan plan;
  mutable bool ready = false;
  bool verifyInstalled(const ContentManifest& installed) {
    uint64_t size = 0, length = 0;
    Digest actual{};
    if (!guard() || metadata.stat(ACTIVE_COURSE_PATH, size) != FileStatus::Present || size != installed.length ||
        !guard() || !Storage.openFileForReadReusing("COMPANION", ACTIVE_COURSE_PATH, installedFile))
      return false;
    const bool hashed = !installedFile.isDirectory() &&
                        hashInventoryFile(
                            installedFile, io, length, actual,
                            [](void* ctx) { return static_cast<HalRemovedCourseBaseline*>(ctx)->guard(); }, this);
    const bool synced = hashed && installedFile.sync();
    const bool closed = installedFile.close();
    return hashed && synced && closed && guard() && length == installed.length && actual == installed.contentHash;
  }
  bool guard() const { return permitted && permitted(context); }
  bool fail(const char* operation) {
    LOG_ERR("COMPANION", "Removed course baseline %s failed", operation);
    closeReaders();
    return false;
  }
};
}  // namespace companion
