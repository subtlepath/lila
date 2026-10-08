#pragma once

#include "CompanionContentRemovalJournal.h"
#include "HalCompanionFileLookup.h"

namespace companion {
enum class CompletedRemovalResult { Ok, Missing, Invalid, Conflict, Corrupt, IoError };
// A completed transaction ID is never reused. Receipts retain the original full
// request and plan even after reader state changes or the active journal rotates.
// This session-owned store borrows encoding scratch separate from journal bytes.
class HalCompletedContentRemovals final {
 public:
  explicit HalCompletedContentRemovals(std::span<uint8_t> scratch) : scratch(scratch) {}
  ~HalCompletedContentRemovals() { close(); }
  CompletedRemovalResult load(const ContentRemovalRequest& request, ContentRemovalRecord& output) {
    if (!validContentRemovalRequest(request) || scratch.size() < CONTENT_REMOVAL_RECORD_SIZE)
      return CompletedRemovalResult::Invalid;
    if (!prepare(request.transaction)) return error("load preparation");
    const auto result = read(target.data());
    if (result != CompletedRemovalResult::Ok) return result;
    if (decoded.request != request) return CompletedRemovalResult::Conflict;
    output = decoded;
    return CompletedRemovalResult::Ok;
  }
  CompletedRemovalResult persist(const ContentRemovalRecord& record, ContentRemovalJournal& journal) {
    if (!validContentRemovalRecord(record) || record.phase != ContentRemovalPhase::Retired ||
        scratch.size() < CONTENT_REMOVAL_RECORD_SIZE || !journal.current() || *journal.current() != record)
      return CompletedRemovalResult::Invalid;
    expected = record;
    owner = &journal;
    if (!prepare(expected.request.transaction) || !guard()) return error("persist preparation");
    auto result = read(target.data());
    if (!guard()) return CompletedRemovalResult::Conflict;
    if (result == CompletedRemovalResult::Ok) return decoded == expected ? result : CompletedRemovalResult::Conflict;
    if (result != CompletedRemovalResult::Missing) return result;
    result = read(stage.data());
    if (!guard()) return CompletedRemovalResult::Conflict;
    if (result == CompletedRemovalResult::Ok && decoded != expected) return CompletedRemovalResult::Conflict;
    if (result != CompletedRemovalResult::Missing && result != CompletedRemovalResult::Corrupt &&
        result != CompletedRemovalResult::Ok)
      return result;
    if (encodeContentRemovalRecord(expected, scratch) != CONTENT_REMOVAL_RECORD_SIZE || !guard() ||
        !Storage.openFileForWriteReusing("COMPANION", stage.data(), file))
      return error("stage open/encode");
    const bool written = !file.isDirectory() &&
                         file.write(scratch.data(), CONTENT_REMOVAL_RECORD_SIZE) == CONTENT_REMOVAL_RECORD_SIZE &&
                         file.truncate(CONTENT_REMOVAL_RECORD_SIZE) && file.sync();
    const bool closed = close();
    if (!written || !closed || !guard()) return error("stage write/sync");
    result = read(stage.data());
    if (result != CompletedRemovalResult::Ok || decoded != expected || !guard()) return error("stage readback");
    if (lookup.inspect(target.data()) != CompanionFilePresence::Missing || !guard() ||
        !Storage.rename(stage.data(), target.data()))
      return error("publication rename");
    result = read(target.data());
    if (result != CompletedRemovalResult::Ok || decoded != expected || !guard()) return error("publication readback");
    return CompletedRemovalResult::Ok;
  }

 private:
  std::span<uint8_t> scratch;
  ContentRemovalRecord expected, decoded;
  ContentRemovalJournal* owner = nullptr;
  HalCompanionFileLookup lookup;
  HalFile file;
  std::array<char, 112> target{}, stage{};
  bool guard() const { return owner && owner->current() && *owner->current() == expected; }
  bool close() { return !file.isOpen() || file.close() || failure("close"); }
  bool prepare(const Identity& transaction) {
    if (!close() || !Storage.ready() || !Storage.ensureDirectoryExists(TRANSFER_DIRECTORY)) return false;
    static constexpr char PREFIX[] = "/.crosspoint/companion/removal-done-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    static_assert(sizeof(PREFIX) + 32 + 4 <= 112);
    size_t at = sizeof(PREFIX) - 1;
    std::copy_n(PREFIX, at, target.begin());
    for (const auto byte : transaction) {
      target[at++] = HEX_DIGITS[byte >> 4];
      target[at++] = HEX_DIGITS[byte & 15];
    }
    target[at] = 0;
    std::copy_n(target.begin(), at, stage.begin());
    std::copy_n(".tmp", 5, stage.begin() + at);
    return true;
  }
  CompletedRemovalResult read(const char* path) {
    if (!close()) return error("read close");
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Missing) return CompletedRemovalResult::Missing;
    if (presence != CompanionFilePresence::Present || !Storage.openFileForReadReusing("COMPANION", path, file))
      return error("read open");
    if (file.isDirectory() || file.fileSize64() > CONTENT_REMOVAL_RECORD_SIZE) {
      close();
      return error("receipt collision");
    }
    const bool exact = file.fileSize64() == CONTENT_REMOVAL_RECORD_SIZE;
    if (exact &&
        file.read(scratch.data(), CONTENT_REMOVAL_RECORD_SIZE) != static_cast<int>(CONTENT_REMOVAL_RECORD_SIZE)) {
      close();
      return error("receipt read");
    }
    const bool valid = exact && decodeContentRemovalRecord(scratch.first(CONTENT_REMOVAL_RECORD_SIZE), decoded) &&
                       decoded.phase == ContentRemovalPhase::Retired;
    const bool synced = valid && file.sync();
    const bool closed = close();
    if (!closed || (valid && !synced)) return error("receipt sync/close");
    return valid ? CompletedRemovalResult::Ok : CompletedRemovalResult::Corrupt;
  }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Completed removal storage failed: %s", reason);
    return false;
  }
  static CompletedRemovalResult error(const char* reason) {
    failure(reason);
    return CompletedRemovalResult::IoError;
  }
};
}  // namespace companion
