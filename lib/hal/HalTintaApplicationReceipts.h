#pragma once

#include "CompanionTintaApplicationReceiptPaths.h"
#include "CompanionTintaJournal.h"
#include "HalTintaApplicationReceiptStore.h"

namespace companion {
// Allocate off-stack and retain for the learner session; digest storage outlives
// this owner. Caller excludes receipt writers and validates application authority.
class HalTintaApplicationReceipts final {
 public:
  explicit HalTintaApplicationReceipts(TintaJournalStorage& storage)
      : storage(storage), records(target.data(), stage.data()) {}
  TintaApplicationReceiptResult persist(const TintaApplicationReceipt& value) {
    if (!validTintaApplicationReceipt(value)) return TintaApplicationReceiptResult::Invalid;
    const auto result = prepare(value.event, value.course, value.generation, value.resource);
    return result == TintaApplicationReceiptResult::Ok ? records.persist(value) : result;
  }
  TintaApplicationReceiptResult load(const EventIdentity& event, const Identity& course, const Identity& generation,
                                     const Digest& resource, TintaApplicationReceipt& output) {
    auto result = prepare(event, course, generation, resource);
    if (result != TintaApplicationReceiptResult::Ok) return result;
    result = records.load(loaded);
    if (result != TintaApplicationReceiptResult::Ok) return result;
    if (loaded.event != event || loaded.course != course || loaded.generation != generation ||
        loaded.resource != resource)
      return TintaApplicationReceiptResult::Conflict;
    output = loaded;
    return result;
  }

 private:
  TintaApplicationReceiptResult prepare(const EventIdentity& event, const Identity& course, const Identity& generation,
                                        const Digest& resource) {
    if (!encodeTintaApplicationAddress(event, course, generation, resource, address))
      return TintaApplicationReceiptResult::Invalid;
    if (!storage.digest(address, digest)) {
      LOG_ERR("COMPANION", "Tinta receipt address digest failed");
      return TintaApplicationReceiptResult::IoError;
    }
    if (!tintaApplicationReceiptPath(digest, false, target) || !tintaApplicationReceiptPath(digest, true, stage))
      return TintaApplicationReceiptResult::Invalid;
    return TintaApplicationReceiptResult::Ok;
  }
  TintaJournalStorage& storage;
  std::array<uint8_t, TINTA_APPLICATION_ADDRESS_SIZE> address{};
  Digest digest{};
  std::array<char, TINTA_APPLICATION_PATH_SIZE> target{}, stage{};
  HalTintaApplicationReceiptStore records;
  TintaApplicationReceipt loaded;
};
}  // namespace companion
