#pragma once

#include <HalStorage.h>

#include "CompanionTintaJournal.h"
#include "CompanionTintaJournalPaths.h"
#include "HalCompanionFileLookup.h"

namespace companion {

class HalTintaJournalStorage final : public TintaJournalStorage {
 public:
  explicit HalTintaJournalStorage(TintaJournalLocation location = TintaJournalLocation::Active);
  ~HalTintaJournalStorage() override;
  bool close();
  bool size(uint32_t& bytes) override;
  bool read(uint32_t offset, std::span<uint8_t> bytes) override;
  bool write(uint32_t offset, std::span<const uint8_t> bytes) override;
  bool truncate(uint32_t bytes) override;
  bool readHeader(uint8_t slot, std::span<uint8_t> bytes, size_t& length) override;
  bool writeHeader(uint8_t slot, std::span<const uint8_t> bytes) override;
  bool digest(std::span<const uint8_t> bytes, Digest& output) override;

 private:
  bool prepare();
  const TintaJournalPaths* paths;
  HalFile events;
  HalCompanionFileLookup headerLookup;
  uint8_t readsSinceYield = 0;
};

}  // namespace companion
