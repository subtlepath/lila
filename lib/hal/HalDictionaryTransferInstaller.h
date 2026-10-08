#pragma once

#include <memory>

#include "CompanionTransfer.h"

namespace companion {
// Borrowed serialized owner; attach before recovery and detach before destruction.
// Implementations retain durable preparation/publication/retirement ownership.
class HalDictionaryTransferInstaller {
 public:
  virtual ~HalDictionaryTransferInstaller() = default;
  virtual bool prepare(const char*, const char*, const ContentManifest&, const TransferState&, std::span<uint8_t>) = 0;
  virtual bool install(const char*, const ContentManifest&, const TransferState&, std::span<uint8_t>) = 0;
  virtual bool metadata(const char*, const ContentManifest&, const TransferState&, std::span<uint8_t>) = 0;
  virtual bool finalize(const char*, const ContentManifest&, const TransferState&, std::span<uint8_t>) = 0;
};
// Controller and phase owners use checked allocation; all arguments are borrowed.
std::unique_ptr<HalDictionaryTransferInstaller> createHalDictionaryTransferInstaller(const Transfer&, const Identity&,
                                                                                     std::span<uint8_t>);
}  // namespace companion
