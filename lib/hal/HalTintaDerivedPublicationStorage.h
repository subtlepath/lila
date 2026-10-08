#pragma once

#include "HalTintaDerivedFileOwnership.h"
#include "HalTintaDerivedGenerationValidation.h"
#include "HalTintaDerivedIntentWriter.h"
#include "HalTintaDerivedReceiptWriter.h"

namespace companion {
struct TintaDerivedPublicationBindings {
  Identity course{}, storage{};
  Digest pack{}, frontier{};
  void* context = nullptr;
  bool (*proveJournal)(void*, const TintaDerivedManifestView&) = nullptr;
};
// Large session object: integration must use checked allocation, never a local stack object.
class HalTintaDerivedPublicationStorage final : public TintaDerivedPublicationStorage {
 public:
  HalTintaDerivedPublicationStorage(const TintaDerivedPublicationBindings& bindings, std::span<const uint8_t> expected,
                                    std::span<uint8_t> scratch, std::span<const uint8_t> previousReceipt = {})
      : bindings(bindings),
        expected(expected),
        scratch(scratch),
        previous(previousReceipt),
        records(bindings.course, scratch),
        intentWriter(bindings.course, records),
        receiptWriter(bindings.course, records),
        ownership(bindings.course, expected, records, scratch) {}
  ~HalTintaDerivedPublicationStorage() override {
    if (file.isOpen() && !file.close()) failure("validation handle close");
  }
  bool validateBindings(const TintaDerivedManifestView& manifest) override {
    authorized = scratch.size() >= TINTA_DERIVED_MANIFEST_SIZE && manifest.matchesBytes(expected) &&
                 manifest.matches(bindings.course, bindings.storage, bindings.pack, bindings.frontier) &&
                 bindings.proveJournal && bindings.proveJournal(bindings.context, manifest);
    if (!authorized) candidatesValidated = false;
    return authorized || failure("bindings or journal proof");
  }
  bool validateCandidates(const TintaDerivedManifestView& manifest) override {
    candidatesValidated = false;
    HalTintaDerivedGenerationValidation validation;
    if (!validateBindings(manifest) ||
        !validation.begin(manifest, bindings.course, bindings.storage, bindings.pack, bindings.frontier))
      return false;
    for (unsigned i = 0; i < 5; ++i) {
      const auto kind = static_cast<TintaDerivedFile>(i);
      if ((file.isOpen() && !file.close()) ||
          !tintaDerivedFilePath(bindings.course, kind, TintaDerivedRole::Candidate, path) ||
          !Storage.openFileForReadReusing("COMPANION", path.data(), file))
        return failure("candidate open");
      const bool valid = validation.file(kind, file, scratch);
      const bool closed = file.close();
      if (!valid || !closed) return failure("candidate validation/close");
    }
    candidatesValidated = validation.complete();
    return candidatesValidated;
  }
  TintaPublicationState intent(std::span<const uint8_t> bytes) override {
    return records.inspect(TintaDerivedRecord::Intent, bytes);
  }
  TintaPublicationState committed(std::span<const uint8_t> bytes) override {
    return records.inspect(TintaDerivedRecord::Receipt, bytes);
  }
  TintaPublicationState state(TintaDerivedFile kind, TintaDerivedRole role,
                              const TintaDerivedManifestView& manifest) override {
    if (!manifest.matchesBytes(expected)) {
      failure("state manifest");
      return TintaPublicationState::Error;
    }
    return ownership.state(kind, role);
  }
  bool persistIntent(std::span<const uint8_t> bytes) override {
    if (!authorized || !candidatesValidated || !sameBytes(bytes)) return failure("intent admission");
    candidatesValidated = false;
    return intentWriter.persist(bytes);
  }
  bool rename(TintaDerivedFile kind, TintaDerivedRole from, TintaDerivedRole to) override {
    return (authorized && ownership.rename(kind, from, to)) || failure("rename admission");
  }
  bool remove(TintaDerivedFile kind, TintaDerivedRole role) override {
    return (authorized && ownership.remove(kind, role)) || failure("remove admission");
  }
  bool commit(std::span<const uint8_t> bytes) override {
    return (authorized && sameBytes(bytes) && receiptWriter.commit(bytes, previous)) || failure("commit admission");
  }
  bool clearIntent() override {
    if (!authorized) return failure("clear admission");
    for (unsigned i = 0; i < 5; ++i)
      if (ownership.state(static_cast<TintaDerivedFile>(i), TintaDerivedRole::Backup) != TintaPublicationState::Missing)
        return failure("backups remain");
    return receiptWriter.clearIntent(expected);
  }

 private:
  bool sameBytes(std::span<const uint8_t> bytes) const {
    return bytes.size() == expected.size() && std::equal(bytes.begin(), bytes.end(), expected.begin());
  }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Tinta publication backend %s failed", reason);
    return false;
  }
  const TintaDerivedPublicationBindings& bindings;
  std::span<const uint8_t> expected;
  std::span<uint8_t> scratch;
  std::span<const uint8_t> previous;
  HalTintaDerivedRecordReader records;
  HalTintaDerivedIntentWriter intentWriter;
  HalTintaDerivedReceiptWriter receiptWriter;
  HalTintaDerivedFileOwnership ownership;
  HalFile file;
  std::array<char, COURSE_STATE_PATH_SIZE> path{};
  bool authorized = false;
  bool candidatesValidated = false;
};
}  // namespace companion
