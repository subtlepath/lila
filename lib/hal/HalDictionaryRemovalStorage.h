#pragma once

#include "CompanionDictionaryRemovalParticipant.h"
#include "HalDictionaryRemovalCohortPlanStorage.h"
#include "HalInventoryFileHash.h"
#include "HalInventoryPathLookup.h"
#include "HalRemovalCohortAddress.h"

namespace companion {
// Retain outside the task stack. Plan bytes and disjoint IO scratch are borrowed,
// immutable for this owner, and outlive it. The parent excludes namespace edits.
class HalDictionaryRemovalStorage final : public DictionaryRemovalStorage {
 public:
  HalDictionaryRemovalStorage(ContentRemovalJournal& journal, std::span<const uint8_t> bytes,
                              std::span<uint8_t> scratch)
      : journal(journal), bytes(bytes), scratch(scratch), lookup(progress, this) {
    mbedtls_sha256_init(&hash);
  }
  ~HalDictionaryRemovalStorage() override {
    close();
    mbedtls_sha256_free(&hash);
  }
  HalDictionaryRemovalStorage(const HalDictionaryRemovalStorage&) = delete;
  HalDictionaryRemovalStorage& operator=(const HalDictionaryRemovalStorage&) = delete;
  bool configureCohort(HalDictionaryRemovalCohortPlanStorage& plans, uint64_t ordinal) {
    if (bound || cohort || !plans.current() || ordinal >= plans.current()->count || ordinal > (UINT64_MAX - 3) / 4)
      return fail("cohort arguments");
    cohort = &plans;
    memberBase = ordinal * 4;
    return true;
  }
  bool unbind() {
    bound = preflight = false;
    cohort = nullptr;
    memberBase = 0;
    return close();
  }
  bool verifyPlan(const DictionaryRemovalPlan& input, const Digest& expected) override {
    if (bound || scratch.empty() || !close() ||
        (journal.current() && (journal.current()->request != input.request || journal.current()->planHash != expected)))
      return fail("plan arguments");
    preflight = !journal.current();
    if (!preflight) checkpoint = *journal.current();
    if (cohort) {
      if (!cohort->current() || !cohort->verifiedDigest() || *cohort->verifiedDigest() != expected ||
          cohort->current()->request != input.request || !cohort->rewind() || !guard())
        return fail("cohort proof");
      for (uint64_t at = 0; at <= memberBase / 4; ++at)
        if (cohort->next(decoded) != InventoryPathRecordResult::Entry || !guard()) return fail("cohort record");
    } else {
      if (!codec.decode(bytes, decoded) || mbedtls_sha256_starts(&hash, 0) ||
          mbedtls_sha256_update(&hash, bytes.data(), bytes.size()) || mbedtls_sha256_finish(&hash, actual.data()) ||
          actual != expected)
        return fail("plan SHA");
    }
    if (decoded != input || !guard()) return fail("plan value");
    planHash = expected;
    bound = true;
    return true;
  }
  FileStatus stat(unsigned member, bool backup) override {
    if (!select(member, backup)) return FileStatus::Error;
    uint64_t length = 0;
    const auto result = lookup.stat(path.data(), length);
    return guard() ? result : FileStatus::Error;
  }
  bool verify(unsigned member, bool backup, uint64_t length, const Digest& expected) override {
    if (!select(member, backup) || length != decoded.installed.extraction.lengths[member] ||
        expected != decoded.installed.extraction.hashes[member] ||
        !Storage.openFileForReadReusing("COMPANION", path.data(), file))
      return fail("member open or proof");
    uint64_t measured = 0;
    const bool hashed = hashInventoryFile(file, scratch, measured, actual, progress, this);
    const bool synced = hashed && file.sync();
    const bool closed = close();
    return (hashed && synced && closed && guard() && measured == length && actual == expected) || fail("member SHA");
  }
  bool quarantine(unsigned member) override {
    if (!select(member, false) || !journal.current() || checkpoint.phase != ContentRemovalPhase::Prepared ||
        !removalCohortAddress(hash, planHash, memberBase + member, backupPath) || !guard())
      return fail("quarantine context");
    uint64_t length = 0;
    if (lookup.stat(path.data(), length) != FileStatus::Present || !guard() ||
        lookup.stat(backupPath.data(), length) != FileStatus::Missing || !guard() ||
        !Storage.rename(path.data(), backupPath.data()) || !guard())
      return fail("quarantine rename");
    return true;
  }
  bool remove(unsigned member, bool backup) override {
    if (!select(member, backup) || !journal.current() ||
        checkpoint.phase != (backup ? ContentRemovalPhase::Committed : ContentRemovalPhase::Prepared))
      return fail("removal context");
    uint64_t length = 0;
    return (lookup.stat(path.data(), length) == FileStatus::Present && guard() && Storage.remove(path.data()) &&
            guard()) ||
           fail("member removal");
  }

 private:
  ContentRemovalJournal& journal;
  std::span<const uint8_t> bytes;
  std::span<uint8_t> scratch;
  DictionaryRemovalPlanCodec codec;
  DictionaryRemovalPlan decoded;
  HalInventoryPathLookup lookup;
  HalFile file;
  mbedtls_sha256_context hash;
  Digest actual{}, planHash{};
  ContentRemovalRecord checkpoint;
  std::array<char, 144> path{};
  std::array<char, 112> backupPath{};
  HalDictionaryRemovalCohortPlanStorage* cohort = nullptr;
  uint64_t memberBase = 0;
  bool bound = false, preflight = false;
  bool select(unsigned member, bool backup) {
    if (!bound || member >= (decoded.installed.extraction.synonyms ? 4U : 3U) || !close())
      return fail("member context");
    preflight = !journal.current();
    if (!preflight) {
      checkpoint = *journal.current();
      if (checkpoint.request != decoded.request || checkpoint.planHash != planHash) return fail("journal ownership");
    }
    return (backup ? removalCohortAddress(hash, planHash, memberBase + member, path)
                   : dictionaryRemovalMemberPath(decoded, member, path)) &&
           guard();
  }
  bool guard() const { return preflight ? !journal.current() : journal.current() && *journal.current() == checkpoint; }
  static bool progress(void* context) { return static_cast<HalDictionaryRemovalStorage*>(context)->guard(); }
  bool close() { return !file.isOpen() || file.close() || fail("close"); }
  static bool fail(const char* operation) {
    LOG_ERR("COMPANION", "Dictionary removal %s failed", operation);
    return false;
  }
};
}  // namespace companion
