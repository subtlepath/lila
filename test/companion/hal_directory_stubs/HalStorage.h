#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>
namespace directory_test {
struct Node {
  std::string name;
  bool directory = false;
};
struct State {
  std::map<std::string, std::vector<Node>> directories;
  unsigned preparations = 0, errors = 0, yields = 0;
  std::map<std::string, std::vector<uint8_t>> files;
  std::string failReadPath;
  bool failPrepare = false, failClose = false, failName = false;
  std::string errorDirectory;
};
inline State state;
}  // namespace directory_test
enum class HalDirectoryResult { Entry, End, Error };
class HalFile {
 public:
  std::string path, name;
  bool opened = false, directory = false, prepared = false;
  size_t cursor = 0;
  explicit operator bool() const { return opened; }
  uint64_t fileSize64() const { return directory_test::state.files.at(path).size(); }
  bool seek64(uint64_t offset) {
    cursor = offset;
    return opened;
  }
  int read(void* output, size_t count) {
    if (path == directory_test::state.failReadPath) return -1;
    const auto& bytes = directory_test::state.files.at(path);
    if (cursor > bytes.size()) return -1;
    count = std::min(count, bytes.size() - cursor);
    memcpy(output, bytes.data() + cursor, count);
    cursor += count;
    return count;
  }
  bool isOpen() const { return opened; }
  bool isDirectory() const { return directory; }
  bool close() {
    opened = false;
    return !directory_test::state.failClose;
  }
  bool prepareDirectoryEntry() {
    if (directory_test::state.failPrepare) return false;
    if (!prepared) {
      ++directory_test::state.preparations;
      prepared = true;
    }
    return true;
  }
  size_t getName(char* out, size_t capacity) {
    if (directory_test::state.failName || name.size() >= capacity) return 0;
    memcpy(out, name.c_str(), name.size() + 1);
    return name.size();
  }
  HalDirectoryResult nextEntry(HalFile& entry) {
    if (!opened || !directory || !entry.prepared || entry.opened || path == directory_test::state.errorDirectory) {
      return HalDirectoryResult::Error;
    }
    const auto& entries = directory_test::state.directories.at(path);
    if (cursor == entries.size()) return HalDirectoryResult::End;
    const auto& next = entries[cursor++];
    entry.name = next.name;
    entry.path = path + (path == "/" ? "" : "/") + next.name;
    entry.directory = next.directory;
    entry.opened = true;
    entry.cursor = 0;
    return HalDirectoryResult::Entry;
  }
};
struct TestStorage {
  bool openFileForReadReusing(const char*, const char* path, HalFile& file) {
    if (!file.prepareDirectoryEntry() || !file.close()) return false;
    const auto& state = directory_test::state;
    if (!state.files.contains(path) && !state.directories.contains(path)) return false;
    file.path = path;
    file.opened = true;
    file.directory = state.directories.contains(path);
    file.cursor = 0;
    return true;
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    if (!directory_test::state.directories.contains(path)) return false;
    file.path = path;
    file.opened = true;
    file.directory = true;
    file.cursor = 0;
    return true;
  }
};
inline TestStorage Storage;
