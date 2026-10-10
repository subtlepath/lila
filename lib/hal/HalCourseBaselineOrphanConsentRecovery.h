#pragma once

#include <Memory.h>

#include "CompanionCourseBaselineOrphanConsent.h"
#include "HalCompanionHeapAdmission.h"
#include "HalCourseRemovalMetadata.h"

namespace companion {
// Temporary owner retained off stack. Caller closes activities and excludes all
// state/namespace writers; transfer inspection precedes every rollback scan.
class HalCourseBaselineOrphanConsentRecovery final {
 public:
  using Permission = bool (*)(void*);
  HalCourseBaselineOrphanConsentRecovery(Permission permitted, void* context)
      : permitted(permitted), context(context), io(*this), rollback(io, rollbackPermission, this) {}
  ~HalCourseBaselineOrphanConsentRecovery() { close(); }
  HalCourseBaselineOrphanConsentRecovery(const HalCourseBaselineOrphanConsentRecovery&) = delete;
  HalCourseBaselineOrphanConsentRecovery& operator=(const HalCourseBaselineOrphanConsentRecovery&) = delete;
  bool pending(bool& output) {
    output = false;
    if (operating) return failure("scan reentry");
    operating = true;
    const bool result = scan(output);
    operating = false;
    return result;
  }
  bool run(Transfer& parent, const Identity& generation) {
    if (operating) return failure("reentry");
    if (!guard() || parent.recover(generation, TransferRecoveryMode::InspectJournal) != TransferResult::Ok || !guard())
      return failure("parent inspection");
    transfer = &parent;
    hasParent = parent.current() != nullptr;
    if (hasParent) checkpoint = *parent.current();
    operating = true;
    bool result = true;
    for (;;) {
      bool found = false;
      if (!scan(found)) {
        result = false;
        break;
      }
      if (!found) break;
      if (!canRollback() || rollback.rollback(selected) != CourseBaselineConsentResult::Ok) {
        result = failure("rollback evidence");
        break;
      }
    }
    const bool closed = close();
    const bool owned = guard();
    operating = false;
    transfer = nullptr;
    return result && closed && owned;
  }

 private:
  class QuarantineStorage final : public TransferStorage {
   public:
    explicit QuarantineStorage(HalCourseBaselineOrphanConsentRecovery& owner)
        : owner(owner), metadata(basePermission, &owner) {}
    bool prepare() override { return owner.canRollback() && metadata.prepare(); }
    FileStatus stat(const char* path, uint64_t& size) override {
      return owner.canRollback() && valid(path) ? metadata.stat(path, size) : FileStatus::Error;
    }
    bool read(const char*, uint64_t, std::span<uint8_t>) override { return failure("read refused"); }
    bool write(const char*, uint64_t, std::span<const uint8_t>, bool) override { return failure("write refused"); }
    bool resize(const char*, uint64_t) override { return failure("resize refused"); }
    bool remove(const char*) override { return failure("remove refused"); }
    bool verify(const char*, uint64_t, const Digest&, std::span<uint8_t>) override { return failure("verify refused"); }
    bool rename(const char* from, const char* to) override {
      uint64_t size = 0;
      return (owner.canRollback() && valid(from, ".consent.tmp") && valid(to, ".consent.orphan") && close() &&
              metadata.stat(from, size) == FileStatus::Present && size <= COURSE_BASELINE_IMPORT_REQUEST_SIZE &&
              owner.canRollback() && metadata.stat(to, size) == FileStatus::Missing && close() && owner.canRollback() &&
              Storage.rename(from, to) && owner.canRollback()) ||
             failure("rename");
    }
    bool close() { return metadata.closeReaders(); }

   private:
    HalCourseBaselineOrphanConsentRecovery& owner;
    HalCourseRemovalMetadata metadata;
    bool valid(const char* path, std::string_view required = {}) const {
      if (!path || owner.selected == Identity{}) return false;
      const auto length = strnlen(path, 96);
      const std::string_view view(path, length);
      static constexpr std::string_view PREFIX = "/.crosspoint/companion/course-baseline-";
      static constexpr char DIGITS[] = "0123456789abcdef";
      if (length == 96 || !view.starts_with(PREFIX) || length < PREFIX.size() + 32) return false;
      for (size_t at = 0; at < owner.selected.size(); ++at)
        if (view[PREFIX.size() + 2 * at] != DIGITS[owner.selected[at] >> 4] ||
            view[PREFIX.size() + 2 * at + 1] != DIGITS[owner.selected[at] & 15])
          return false;
      const auto suffix = view.substr(PREFIX.size() + 32);
      static constexpr std::string_view NUMBERED = ".consent.orphan-";
      const auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
      const bool orphan =
          suffix == ".consent.orphan" || (suffix.size() == NUMBERED.size() + 2 && suffix.starts_with(NUMBERED) &&
                                          hex(suffix[NUMBERED.size()]) && hex(suffix[NUMBERED.size() + 1]));
      if (!required.empty()) return required == ".consent.orphan" ? orphan : suffix == required;
      return suffix == ".consent" || suffix == ".consent.tmp" || orphan || suffix == ".prepared" ||
             suffix == ".prepared.tmp" || suffix == ".published" || suffix == ".published.tmp";
    }
  };
  Permission permitted;
  void* context;
  Transfer* transfer = nullptr;
  TransferState checkpoint;
  Identity selected{};
  bool hasParent = false, operating = false;
  HalFile directory, entry;
  std::array<char, INVENTORY_PATH_LIMIT + 1> name{};
  QuarantineStorage io;
  CourseBaselineOrphanConsent rollback;
  bool guard() const {
    if (!permitted || !permitted(context) || !Storage.ready() || !admitCompanionHeap()) return false;
    if (!transfer) return true;
    const auto* current = transfer->current();
    return hasParent ? current && *current == checkpoint : current == nullptr;
  }
  bool canRollback() const {
    return transfer && operating && guard() && (!hasParent || checkpoint.transaction != selected);
  }
  static bool basePermission(void* context) {
    return static_cast<HalCourseBaselineOrphanConsentRecovery*>(context)->guard();
  }
  static bool rollbackPermission(void* context) {
    return static_cast<HalCourseBaselineOrphanConsentRecovery*>(context)->canRollback();
  }
  bool closeScan() {
    const bool entryClosed = !entry.isOpen() || entry.close();
    const bool directoryClosed = !directory.isOpen() || directory.close();
    return entryClosed && directoryClosed;
  }
  bool close() {
    const bool scanClosed = closeScan(), ioClosed = io.close();
    return scanClosed && ioClosed;
  }
  static bool folded(std::string_view value, std::string_view expected) {
    if (value.size() != expected.size()) return false;
    for (size_t at = 0; at < value.size(); ++at) {
      const char c = value[at] >= 'A' && value[at] <= 'Z' ? value[at] + ('a' - 'A') : value[at];
      if (c != expected[at]) return false;
    }
    return true;
  }
  // Return -1 for malformed or noncanonical names in the consent-stage scope.
  static int candidate(std::string_view value, Identity& transaction) {
    static constexpr std::string_view PREFIX = "course-baseline-", SUFFIX = ".consent.tmp";
    if (value.size() < PREFIX.size() + SUFFIX.size() || !folded(value.substr(0, PREFIX.size()), PREFIX) ||
        !folded(value.substr(value.size() - SUFFIX.size()), SUFFIX))
      return 0;
    if (value.size() != PREFIX.size() + 32 + SUFFIX.size() || !value.starts_with(PREFIX) || !value.ends_with(SUFFIX))
      return -1;
    const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t at = 0; at < transaction.size(); ++at) {
      const auto high = hex(value[PREFIX.size() + 2 * at]), low = hex(value[PREFIX.size() + 2 * at + 1]);
      if (high < 0 || low < 0) return -1;
      transaction[at] = static_cast<uint8_t>((high << 4) | low);
    }
    return transaction == Identity{} ? -1 : 1;
  }
  bool scan(bool& output) {
    output = false;
    if (!closeScan() || !guard() || !Storage.openFileForReadReusing("COMPANION", TRANSFER_DIRECTORY, directory) ||
        !directory.isDirectory() || !entry.prepareDirectoryEntry())
      return failure("scan admission");
    unsigned steps = 0;
    for (;;) {
      if (!guard() || (entry.isOpen() && !entry.close())) {
        closeScan();
        return failure("scan ownership/close");
      }
      const auto next = directory.nextEntry(entry);
      if (!guard() || next == HalDirectoryResult::Error) {
        closeScan();
        return failure("scan enumeration");
      }
      if (next == HalDirectoryResult::End) return (closeScan() && guard()) || failure("scan end");
      const auto count = entry.getName(name.data(), name.size());
      char alias[13]{};
      if (!guard() || !count || count >= name.size() || name[count] || !entry.getShortName(alias, sizeof(alias)) ||
          strnlen(alias, sizeof(alias)) == sizeof(alias)) {
        closeScan();
        return failure("scan name/alias");
      }
      Identity transaction{};
      const auto match = candidate(std::string_view(name.data(), count), transaction);
      if (match < 0 || (match && entry.isDirectory())) {
        closeScan();
        return failure("scan consent name");
      }
      if (match && !output) {
        selected = transaction;
        output = true;
      }
      if (++steps == 32) {
        steps = 0;
        vTaskDelay(1);
      }
    }
  }
  static bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Orphan baseline consent %s refused", operation);
    return false;
  }
};

inline bool hasHalCourseBaselineOrphanConsents(bool& output,
                                               HalCourseBaselineOrphanConsentRecovery::Permission permitted,
                                               void* context) {
  output = false;
  if (!permitted || !permitted(context) ||
      !admitCompanionHeap(sizeof(HalCourseBaselineOrphanConsentRecovery),
                          sizeof(HalCourseBaselineOrphanConsentRecovery)))
    return false;
  auto owner = makeUniqueNoThrow<HalCourseBaselineOrphanConsentRecovery>(permitted, context);
  if (!owner) {
    LOG_ERR("COMPANION", "OOM: orphan consent scan");
    return false;
  }
  return owner->pending(output);
}
inline bool recoverHalCourseBaselineOrphanConsents(Transfer& transfer, const Identity& generation,
                                                   HalCourseBaselineOrphanConsentRecovery::Permission permitted,
                                                   void* context) {
  if (!permitted || !permitted(context) ||
      !admitCompanionHeap(sizeof(HalCourseBaselineOrphanConsentRecovery),
                          sizeof(HalCourseBaselineOrphanConsentRecovery)))
    return false;
  auto owner = makeUniqueNoThrow<HalCourseBaselineOrphanConsentRecovery>(permitted, context);
  if (!owner) {
    LOG_ERR("COMPANION", "OOM: orphan consent recovery");
    return false;
  }
  return owner->run(transfer, generation);
}
}  // namespace companion
