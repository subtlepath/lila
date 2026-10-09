#include <type_traits>
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>
inline constexpr int O_RDONLY = 0, O_WRONLY = 1, O_CREAT = 2, O_TRUNC = 4, O_RDWR = 8;
namespace inventory_hal_test {
struct DirectoryEntry {
  std::string name;
  bool directory = false;
};
struct State {
  std::map<std::string, std::vector<DirectoryEntry>> directories;
  std::map<std::string, std::string> aliases;
  bool failShortName = false;
  bool enumerateFileMap = false;
  std::string directoryErrorPath;
  unsigned preparations = 0;
  std::map<std::string, std::vector<uint8_t>> files;
  std::map<std::string, uint32_t> modificationTimes;
  unsigned readOpens = 0;
  bool failSync = false, failWrite = false, failClose = false, corruptWrite = false;
  bool failOpen = false, failTruncate = false, failDirectory = false, failRemove = false;
  unsigned opens = 0, closes = 0;
  std::string statErrorPath;
  std::string failSyncPath;
  std::string failWritePath;
  unsigned matchingWrites = 0, failMatchingWrite = 0;
  std::string failClosePath;
  std::string corruptWritePath;
  std::string readErrorPath;
  std::string failRenameAfterSource;
  std::vector<uint8_t> revisionBytes;
  bool failNvsOpen = false, failNvsWrite = false, failNvsRead = false;
  unsigned directoryRenames = 0, failDirectoryRenameBefore = 0, failDirectoryRenameAfter = 0;
  unsigned failRename = 0, failRenameAgain = 0, failRenameAfter = 0, renames = 0, yields = 0, errors = 0;
  bool failRemoveAfter = false, falseExists = false;
  unsigned reads = 0, failRead = 0, shortRead = 0;
};
inline State state;
}  // namespace inventory_hal_test
enum class HalDirectoryResult { Entry, End, Error };
class HalFile {
 public:
  HalFile() = default;
  explicit HalFile(std::string path) : path(std::move(path)), opened(true) {}
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  HalFile(HalFile&& other) { *this = std::move(other); }
  HalFile& operator=(HalFile&& other) {
    path = std::move(other.path);
    opened = std::exchange(other.opened, false);
    position = other.position;
    prepared = other.prepared;
    return *this;
  }
  explicit operator bool() const { return opened; }
  bool close() {
    if (opened) ++inventory_hal_test::state.closes;
    opened = false;
    return !inventory_hal_test::state.failClose &&
           (inventory_hal_test::state.failClosePath.empty() || path != inventory_hal_test::state.failClosePath);
  }
  bool isOpen() const { return opened; }
  bool isDirectory() const { return inventory_hal_test::state.directories.contains(path); }
  void rewindDirectory() { position = 0; }
  bool prepareDirectoryEntry() {
    if (!prepared) {
      prepared = true;
      ++inventory_hal_test::state.preparations;
    }
    return true;
  }
  bool openReadReusing(const char* filename) {
    if (!prepareDirectoryEntry()) return false;
    if (opened && !close()) return false;
    auto& state = inventory_hal_test::state;
    ++state.opens;
    if (state.failOpen || filename == state.statErrorPath ||
        (!state.files.contains(filename) && !state.directories.contains(filename)))
      return false;
    path = filename;
    opened = true;
    position = 0;
    return true;
  }
  bool openWriteReusing(const char* filename) {
    if (!prepareDirectoryEntry()) return false;
    if (opened && !close()) return false;
    auto& state = inventory_hal_test::state;
    ++state.opens;
    if (state.failOpen) return false;
    state.files[filename].clear();
    path = filename;
    opened = true;
    position = 0;
    return true;
  }
  size_t getName(char* output, size_t capacity) {
    const auto start = path.find_last_of('/') + 1;
    const auto length = path.size() - start;
    if (length >= capacity) return 0;
    std::memcpy(output, path.c_str() + start, length + 1);
    return length;
  }
  bool getShortName(char* output, size_t capacity) {
    auto& state = inventory_hal_test::state;
    if (!opened || !output || capacity < 13 || state.failShortName) return false;
    const auto alias = state.aliases.contains(path) ? state.aliases.at(path) : std::string{};
    if (alias.size() >= capacity) return false;
    std::memcpy(output, alias.c_str(), alias.size() + 1);
    return true;
  }
  HalDirectoryResult nextEntry(HalFile& entry) {
    auto& state = inventory_hal_test::state;
    if (!opened || !isDirectory() || !entry.prepared || entry.opened || path == state.directoryErrorPath)
      return HalDirectoryResult::Error;
    if (path == "/.crosspoint/companion" || state.enumerateFileMap) {
      const std::string prefix = path == "/" ? "/" : path + "/";
      unsigned index = 0;
      for (const auto& [filename, bytes] : state.files) {
        if (!filename.starts_with(prefix) || filename.find('/', prefix.size()) != std::string::npos) continue;
        if (index++ != position) continue;
        ++position;
        entry.path = filename;
        entry.position = 0;
        entry.opened = true;
        return HalDirectoryResult::Entry;
      }
      for (const auto& [filename, children] : state.directories) {
        if (filename == path || !filename.starts_with(prefix) ||
            filename.find('/', prefix.size()) != std::string::npos || state.files.contains(filename))
          continue;
        if (index++ != position) continue;
        ++position;
        entry.path = filename;
        entry.position = 0;
        entry.opened = true;
        return HalDirectoryResult::Entry;
      }
      return HalDirectoryResult::End;
    }
    const auto& entries = state.directories.at(path);
    if (position == entries.size()) return HalDirectoryResult::End;
    const auto& next = entries[position++];
    entry.path = path + (path == "/" ? "" : "/") + next.name;
    entry.position = 0;
    entry.opened = true;
    return HalDirectoryResult::Entry;
  }
  HalFile openNextFile() {
    HalFile entry;
    entry.prepareDirectoryEntry();
    if (nextEntry(entry) != HalDirectoryResult::Entry) return {};
    return entry;
  }
  uint64_t fileSize64() const { return inventory_hal_test::state.files.at(path).size(); }
  size_t size() const { return fileSize64(); }
  uint32_t modificationTime() const {
    const auto& times = inventory_hal_test::state.modificationTimes;
    const auto found = times.find(path);
    return found == times.end() ? 0 : found->second;
  }
  bool seekSet(uint32_t offset) { return seek64(offset); }
  void flush() { sync(); }
  bool seek64(uint64_t offset) {
    position = offset;
    return opened;
  }
  int read(void* output, size_t count) {
    auto& state = inventory_hal_test::state;
    ++state.reads;
    if (state.reads == state.failRead) return -1;
    if (path == inventory_hal_test::state.readErrorPath) return -1;
    auto& bytes = inventory_hal_test::state.files.at(path);
    if (position > bytes.size()) return -1;
    count = std::min(count, bytes.size() - static_cast<size_t>(position));
    if (state.reads == state.shortRead && count > 0) --count;
    std::copy_n(bytes.begin() + position, count, static_cast<uint8_t*>(output));
    position += count;
    return count;
  }
  size_t write(const void* input, size_t count) {
    auto& state = inventory_hal_test::state;
    if (state.failWrite) return 0;
    if (!state.failWritePath.empty() && path == state.failWritePath &&
        ++state.matchingWrites == state.failMatchingWrite)
      return 0;
    auto& bytes = inventory_hal_test::state.files.at(path);
    if (position + count > bytes.size()) bytes.resize(position + count);
    std::copy_n(static_cast<const uint8_t*>(input), count, bytes.begin() + position);
    if ((inventory_hal_test::state.corruptWrite ||
         (!inventory_hal_test::state.corruptWritePath.empty() && path == inventory_hal_test::state.corruptWritePath)) &&
        count != 0)
      bytes[position + count - 1] ^= 1;
    position += count;
    return count;
  }
  bool truncate(uint64_t size) {
    if (inventory_hal_test::state.failTruncate) return false;
    inventory_hal_test::state.files.at(path).resize(size);
    return true;
  }
  bool sync() { return !inventory_hal_test::state.failSync && path != inventory_hal_test::state.failSyncPath; }

 private:
  std::string path;
  bool opened = false, prepared = false;
  uint64_t position = 0;
};
class TestHalStorage {
 public:
  // Selected fixture mappings; real-table behavior is checked by SdFat image tests.
  static uint32_t foldFilenameCodepoint(uint32_t codepoint) {
    if (codepoint >= 'a' && codepoint <= 'z') return codepoint - ('a' - 'A');
    if ((codepoint >= 0xe0 && codepoint <= 0xf6) || (codepoint >= 0xf8 && codepoint <= 0xfe)) return codepoint - 0x20;
    return codepoint;
  }
  bool mkdir(const char* path) {
    if (!ensureDirectoryExists(path)) return false;
    inventory_hal_test::state.directories.try_emplace(path);
    return true;
  }
  bool openFileForReadReusing(const char*, const char* path, HalFile& file) { return file.openReadReusing(path); }
  bool openFileForWriteReusing(const char*, const char* path, HalFile& file) { return file.openWriteReusing(path); }
  bool ready() const { return true; }
  bool exists(const char* path) const {
    return !inventory_hal_test::state.falseExists &&
           (inventory_hal_test::state.files.contains(path) || inventory_hal_test::state.directories.contains(path));
  }
  bool ensureDirectoryExists(const char* path) {
    if (inventory_hal_test::state.failDirectory) return false;
    inventory_hal_test::state.directories.try_emplace(path);
    return true;
  }
  bool rename(const char* from, const char* to) {
    auto& state = inventory_hal_test::state;
    ++state.renames;
    if (state.renames == state.failRename || state.renames == state.failRenameAgain || state.files.contains(to) ||
        state.directories.contains(to))
      return false;
    if (state.directories.contains(from)) {
      ++state.directoryRenames;
      if (state.directoryRenames == state.failDirectoryRenameBefore) return false;
      const std::string oldRoot(from), newRoot(to);
      const auto moveChildren = [&](auto& entries) {
        std::remove_reference_t<decltype(entries)> moved;
        for (auto at = entries.begin(); at != entries.end();) {
          if (at->first == oldRoot || at->first.starts_with(oldRoot + "/")) {
            auto node = entries.extract(at++);
            node.key() = newRoot + node.key().substr(oldRoot.size());
            moved.insert(std::move(node));
          } else
            ++at;
        }
        entries.merge(moved);
      };
      moveChildren(state.files);
      moveChildren(state.directories);
      if (state.directoryRenames == state.failDirectoryRenameAfter) return false;
    } else {
      if (!state.files.contains(from)) return false;
      state.files[to] = std::move(state.files.at(from));
      state.files.erase(from);
    }
    return state.renames != state.failRenameAfter && state.failRenameAfterSource != from;
  }
  bool remove(const char* path) {
    auto& state = inventory_hal_test::state;
    if (state.failRemove || state.files.erase(path) == 0) return false;
    return !state.failRemoveAfter;
  }
  bool rmdir(const char* path) {
    auto& state = inventory_hal_test::state;
    if (state.failRemove || !state.directories.contains(path)) return false;
    const std::string prefix = std::string(path) + "/";
    for (const auto& [name, bytes] : state.files)
      if (name.starts_with(prefix)) return false;
    for (const auto& [name, children] : state.directories)
      if (name.starts_with(prefix)) return false;
    state.directories.erase(path);
    return !state.failRemoveAfter;
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    if (path == inventory_hal_test::state.statErrorPath ||
        (!inventory_hal_test::state.files.contains(path) && !inventory_hal_test::state.directories.contains(path)))
      return false;
    file = HalFile(path);
    ++inventory_hal_test::state.readOpens;
    return true;
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    file = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    return static_cast<bool>(file);
  }
  HalFile open(const char* path, int flags) {
    if (inventory_hal_test::state.failOpen) return {};
    ++inventory_hal_test::state.opens;
    if (flags & O_CREAT) inventory_hal_test::state.files.try_emplace(path);
    if (flags & O_TRUNC) inventory_hal_test::state.files[path].clear();
    return HalFile(path);
  }
  HalFile open(const char* path) {
    HalFile file;
    if (!openFileForRead("TEST", path, file)) return {};
    return file;
  }
};
inline TestHalStorage Storage;
