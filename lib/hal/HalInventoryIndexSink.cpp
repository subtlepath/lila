#include "HalInventoryIndexSink.h"

#include <Logging.h>

#include <climits>

namespace companion {
namespace {
bool inventoryFailure([[maybe_unused]] const char* operation) {
  LOG_ERR("COMPANION", "Inventory publication failed: %s", operation);
  return false;
}
}  // namespace
HalInventoryIndexSink::~HalInventoryIndexSink() { abort(); }

HalInventoryIndexSink::Validation HalInventoryIndexSink::validate(const char* path, uint64_t* revision) {
  if (!reader.open(path)) return Validation::IoError;
  IndexedInventoryCatalog catalog(reader, scratch);
  const bool result = catalog.open(generation);
  bool ioError = reader.hadReadError();
  if (result && !ioError && revision) *revision = catalog.revision();
  if (!reader.close()) ioError = true;
  return ioError ? Validation::IoError : (result ? Validation::Valid : Validation::Corrupt);
}

bool HalInventoryIndexSink::removeIfPresent(const char* path) {
  uint64_t bytes = 0;
  const auto status = metadata.stat(path, bytes);
  return status == FileStatus::Missing || (status == FileStatus::Present && metadata.remove(path));
}

bool HalInventoryIndexSink::closeCandidate() {
  const bool result = !candidate || candidate.close();
  candidate = HalFile();
  writing = false;
  return result;
}

bool HalInventoryIndexSink::recover() {
  if (writing || scratch.size() < INVENTORY_INDEX_ENTRY_SIZE || !inventory_detail::nonzero(generation) ||
      !metadata.prepare())
    return inventoryFailure("prepare recovery");
  uint64_t bytes = 0;
  const auto active = metadata.stat(ACTIVE, bytes);
  const auto backup = metadata.stat(BACKUP, bytes);
  if (active == FileStatus::Error || backup == FileStatus::Error) return inventoryFailure("inspect recovery");
  if (active == FileStatus::Present) {
    const auto validation = validate(ACTIVE);
    if (validation == Validation::IoError) return inventoryFailure("read active recovery");
    if (validation == Validation::Valid)
      return (removeIfPresent(BACKUP) && removeIfPresent(CANDIDATE)) || inventoryFailure("clean recovery");
  }
  if (backup == FileStatus::Present) {
    const auto validation = validate(BACKUP);
    if (validation == Validation::IoError) return inventoryFailure("read backup recovery");
    if (validation == Validation::Valid) {
      if (!removeIfPresent(ACTIVE) || !metadata.rename(BACKUP, ACTIVE) || !removeIfPresent(CANDIDATE))
        return inventoryFailure("restore backup");
      return true;
    }
  }
  if (active == FileStatus::Present || backup == FileStatus::Present) return inventoryFailure("no valid snapshot");
  return removeIfPresent(CANDIDATE) || inventoryFailure("discard candidate");
}

bool HalInventoryIndexSink::nextRevision(uint64_t& revision) {
  if (!recover()) return false;
  uint64_t bytes = 0, current = 0;
  const auto status = metadata.stat(ACTIVE, bytes);
  if (status == FileStatus::Error || (status == FileStatus::Present && validate(ACTIVE, &current) != Validation::Valid))
    return inventoryFailure("read revision");
  uint64_t next = 0;
  if (!revisions.reserve(current, next)) return false;
  reservedRevision = next;
  revision = next;
  return true;
}

bool HalInventoryIndexSink::buildAndPublish(SortedInventorySource& source, uint64_t& revision) {
  uint64_t next = 0;
  if (!nextRevision(next)) return false;
  InventoryIndexBuilder builder(*this, scratch);
  if (!builder.build(source, generation, next)) return inventoryFailure("build snapshot");
  revision = next;
  return true;
}

bool HalInventoryIndexSink::begin() {
  if (reservedRevision == 0) return inventoryFailure("missing revision reservation");
  if (!recover()) return false;
  candidate = Storage.open(CANDIDATE, O_WRONLY | O_CREAT | O_TRUNC);
  if (!candidate) return inventoryFailure("open candidate");
  writing = true;
  return true;
}

bool HalInventoryIndexSink::write(uint64_t offset, std::span<const uint8_t> bytes) {
  if (!writing || bytes.size() > INT_MAX || offset > UINT64_MAX - bytes.size() || !candidate.seek64(offset) ||
      candidate.write(bytes.data(), bytes.size()) != bytes.size())
    return inventoryFailure("write candidate");
  return true;
}

bool HalInventoryIndexSink::finish(uint64_t bytes) {
  if (!writing || candidate.fileSize64() != bytes || !candidate.sync()) return inventoryFailure("sync candidate");
  uint64_t candidateRevision = 0;
  if (!closeCandidate() || validate(CANDIDATE, &candidateRevision) != Validation::Valid)
    return inventoryFailure("validate candidate");
  if (candidateRevision != reservedRevision) return inventoryFailure("unreserved candidate revision");
  uint64_t oldBytes = 0;
  const auto status = metadata.stat(ACTIVE, oldBytes);
  if (status == FileStatus::Error) return inventoryFailure("inspect active");
  if (status == FileStatus::Present) {
    uint64_t activeRevision = 0;
    if (validate(ACTIVE, &activeRevision) != Validation::Valid || candidateRevision <= activeRevision)
      return inventoryFailure("non-increasing revision");
  }
  if (status == FileStatus::Present && !metadata.rename(ACTIVE, BACKUP)) return inventoryFailure("retain backup");
  if (!metadata.rename(CANDIDATE, ACTIVE)) {
    if (status == FileStatus::Present && !metadata.rename(BACKUP, ACTIVE))
      LOG_ERR("COMPANION", "Inventory backup needs recovery");
    return inventoryFailure("publish candidate");
  }
  reservedRevision = 0;
  // Backup cleanup is deferred until recovery; publication has already committed.
  return true;
}

void HalInventoryIndexSink::abort() {
  reservedRevision = 0;
  if (!closeCandidate()) LOG_ERR("COMPANION", "Inventory candidate close failed");
  if (!removeIfPresent(CANDIDATE)) LOG_ERR("COMPANION", "Inventory candidate cleanup failed");
}
}  // namespace companion
