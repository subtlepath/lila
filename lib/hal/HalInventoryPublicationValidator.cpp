#include "HalInventoryPublicationValidator.h"

#include <Logging.h>

namespace companion {
namespace {
InventoryValidation report(InventoryValidation result) {
  if (result != InventoryValidation::Valid)
    LOG_ERR("COMPANION", "Inventory snapshot validation failed: %u", static_cast<unsigned>(result));
  return result;
}
[[gnu::noinline]] bool openCatalog(IndexedInventoryCatalog& catalog, const Identity& generation) {
  return catalog.open(generation);
}
[[gnu::noinline]] bool openPaths(InventoryPaths& paths, const Identity& generation, uint64_t revision) {
  return paths.open(generation, revision);
}
}  // namespace
bool HalInventoryPublicationValidator::close() {
  const bool pathsClosed = pathStorage.close();
  const bool indexClosed = indexStorage.close();
  return pathsClosed && indexClosed;
}
InventoryValidation HalInventoryPublicationValidator::open(HalInventoryIndexStorage& reader, const char* path) {
  if (!path || !path[0]) return InventoryValidation::Invalid;
  if (!Storage.ready()) return InventoryValidation::IoError;
  if (!Storage.exists(path)) return InventoryValidation::Invalid;
  return reader.open(path) ? InventoryValidation::Valid : InventoryValidation::IoError;
}
InventoryValidation HalInventoryPublicationValidator::finish(bool valid) {
  const bool ioError = indexStorage.hadReadError() || pathStorage.hadReadError();
  const bool closed = close();
  return report(ioError || !closed ? InventoryValidation::IoError
                                   : (valid ? InventoryValidation::Valid : InventoryValidation::Invalid));
}
bool HalInventoryPublicationValidator::validateIndex(const Identity& generation, uint64_t revision) {
  IndexedInventoryCatalog catalog(indexStorage, scratch);
  return openCatalog(catalog, generation) && (revision == 0 || catalog.revision() == revision);
}
bool HalInventoryPublicationValidator::validatePaths(const Identity& generation, uint64_t revision) {
  if (scratch.size() < INVENTORY_PATH_MAX_RECORD) return false;
  InventoryIndexHeader header;
  if (!indexStorage.read(0, scratch.first(INVENTORY_INDEX_HEADER_SIZE)) ||
      !decodeInventoryPathsHeader(scratch.first(INVENTORY_INDEX_HEADER_SIZE), header))
    return false;
  InventoryPaths paths(indexStorage, scratch);
  return openPaths(paths, generation, revision == 0 ? header.revision : revision);
}
InventoryValidation HalInventoryPublicationValidator::file(const char* path, bool paths, const Identity& generation,
                                                           uint64_t revision) {
  if (!close()) return report(InventoryValidation::IoError);
  const auto opened = open(indexStorage, path);
  if (opened != InventoryValidation::Valid) return report(opened);
  return finish(paths ? validatePaths(generation, revision) : validateIndex(generation, revision));
}
InventoryValidation HalInventoryPublicationValidator::pair(const char* index, const char* paths,
                                                           const Identity& generation, uint64_t revision,
                                                           uint64_t* actualRevision) {
  if (!close()) return report(InventoryValidation::IoError);
  auto opened = open(indexStorage, index);
  if (opened == InventoryValidation::Valid) opened = open(pathStorage, paths);
  if (opened != InventoryValidation::Valid) {
    if (!close()) return report(InventoryValidation::IoError);
    return report(opened);
  }
  const bool valid = validation.validate(generation, revision);
  const uint64_t actual = validation.revision();
  const auto result = finish(valid);
  if (result == InventoryValidation::Valid && actualRevision) *actualRevision = actual;
  return result;
}
}  // namespace companion
