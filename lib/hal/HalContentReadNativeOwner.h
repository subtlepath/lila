#pragma once

#include "CompanionWifiContentRead.h"
#include "HalContentExportAdmission.h"
#include "HalContentReadSession.h"
#include "HalInventoryPublicationValidator.h"

namespace companion {
// Retained outside the task stack; all variable-size I/O uses the activity workspace.
class HalContentReadNativeOwner final {
 public:
  HalContentReadNativeOwner(std::span<uint8_t> scratch, InventoryHashProgress progress, void* context)
      : scratch(scratch), validator(scratch), paths(pathStorage, scratch), session(progress, context) {}
  bool prepare() { return session.prepare(); }
  bool resetSource(bool preserveExport = false) {
    if (!preserveExport) binding.reset();
    return session.reset();
  }
  const ContentExportBinding& exportBinding() const { return binding; }
  size_t admitExport(const ContentHandoffRequest& request, const Identity& installation, const Identity& generation,
                     uint64_t revision, std::span<uint8_t> output) {
    return admitContentExport(binding, request, true, installation, generation, revision, !opened, session, paths,
                              scratch, output);
  }
  size_t wifiReply(const Identity& transaction, const Identity& installation, const Identity& generation,
                   uint64_t revision, std::span<const uint8_t> input, std::span<uint8_t> output) {
    ContentReadRequest request;
    if (!decodeWifiContentRead(input, transaction, installation, revision, binding, request)) return 0;
    return session.reply(true, installation, generation, revision, !opened, paths, input.subspan(16), scratch, output);
  }
  bool closeReaders() {
    binding.reset();
    const bool sourceClosed = session.reset();
    opened = false;
    const bool pathsClosed = pathStorage.close();
    return sourceClosed && pathsClosed;
  }
  bool open(const Identity& generation, uint64_t revision) {
    if (opened && paths.belongsTo(generation, revision)) return true;
    if (!closeReaders() || !revision || !admitCompanionHeap()) return false;
    if (validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation, revision) !=
            InventoryValidation::Valid ||
        !pathStorage.open(InventoryPublication::PATHS) || !paths.open(generation, revision) || !admitCompanionHeap()) {
      closeReaders();
      LOG_ERR("COMPANION", "Content export inventory unavailable");
      return false;
    }
    opened = true;
    return true;
  }
  size_t metadataReply(const ContentMetadataRequest& request, const Identity& generation, uint64_t revision,
                       std::span<uint8_t> output) {
    return session.metadataReply(request, true, generation, revision, !opened, paths, output);
  }
  size_t reply(const Identity& installation, const Identity& generation, uint64_t revision,
               std::span<const uint8_t> input, std::span<uint8_t> output) {
    return session.reply(true, installation, generation, revision, !opened, paths, input, scratch, output);
  }

 private:
  std::span<uint8_t> scratch;
  HalInventoryPublicationValidator validator;
  HalInventoryIndexStorage pathStorage;
  InventoryPaths paths;
  HalContentReadSession session;
  ContentExportBinding binding;
  bool opened = false;
};
}  // namespace companion
