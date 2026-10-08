#pragma once

#include "CompanionCourseStatePaths.h"
#include "CompanionTintaReceiveJournal.h"
#include "HalCompanionFileLookup.h"

namespace companion {
// Session-owned retained handles/path buffers exceed the task-local budget.
// Caller excludes concurrent journal writers and prepares the course directory.
class HalTintaReceiveJournalStorage final : public TintaReceiveJournalStorage {
 public:
  explicit HalTintaReceiveJournalStorage(const Identity& course)
      : course(course), lookup(nullptr, nullptr, root.data()) {
    pathsValid = courseStateDirectory(course, root) && courseStatePath(course, "receive-a", paths[0]) &&
                 courseStatePath(course, "receive-b", paths[1]);
  }
  ~HalTintaReceiveJournalStorage() override {
    if (file.isOpen() && !file.close()) failure("destructor close");
  }
  bool read(uint8_t slot, std::span<uint8_t> bytes, size_t& length) override {
    length = 0;
    if (!pathsValid || slot > 1 || bytes.size() < TINTA_RECEIVE_CHECKPOINT_SIZE || (file.isOpen() && !file.close()))
      return failure("read arguments or close");
    const auto presence = lookup.inspect(paths[slot].data());
    if (presence == CompanionFilePresence::Missing) return true;
    if (presence != CompanionFilePresence::Present ||
        !Storage.openFileForReadReusing("COMPANION", paths[slot].data(), file) || file.isDirectory())
      return failure("read open");
    const auto extent = file.fileSize64();
    if (extent != TINTA_RECEIVE_CHECKPOINT_SIZE) {
      length = TINTA_RECEIVE_CHECKPOINT_SIZE + 1;
      return file.close() || failure("invalid extent close");
    }
    const bool read = file.seek64(0) &&
                      file.read(bytes.data(), TINTA_RECEIVE_CHECKPOINT_SIZE) == TINTA_RECEIVE_CHECKPOINT_SIZE &&
                      file.fileSize64() == extent;
    const bool closed = file.close();
    if (!read || !closed) return failure("read or close");
    TintaReceiveCheckpoint value;
    if (decodeTintaReceiveCheckpoint(bytes.first(TINTA_RECEIVE_CHECKPOINT_SIZE), value) && value.course != course)
      return failure("read course binding");
    length = TINTA_RECEIVE_CHECKPOINT_SIZE;
    return true;
  }
  bool write(uint8_t slot, std::span<const uint8_t> bytes) override {
    TintaReceiveCheckpoint value;
    if (!pathsValid || slot > 1 || !decodeTintaReceiveCheckpoint(bytes, value) || value.course != course ||
        (value.sequence & 1) != slot || (file.isOpen() && !file.close()))
      return failure("write binding or close");
    const auto presence = lookup.inspect(paths[slot].data());
    if (presence == CompanionFilePresence::Error) return failure("write lookup");
    if (!Storage.openFileForWriteReusing("COMPANION", paths[slot].data(), file) || file.isDirectory())
      return failure("write open");
    const bool written = file.seek64(0) && file.write(bytes.data(), bytes.size()) == bytes.size() &&
                         file.truncate(bytes.size()) && file.sync();
    const bool closed = file.close();
    return (written && closed) || failure("write/sync/close");
  }

 private:
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Tinta receive journal %s failed", reason);
    return false;
  }
  Identity course;
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  std::array<std::array<char, COURSE_STATE_PATH_SIZE>, 2> paths{};
  HalCompanionFileLookup lookup;
  HalFile file;
  bool pathsValid = false;
};
}  // namespace companion
