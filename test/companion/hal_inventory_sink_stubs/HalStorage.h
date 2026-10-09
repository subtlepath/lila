#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>
inline constexpr int O_WRONLY = 1, O_CREAT = 2, O_TRUNC = 4;
namespace inventory_hal_test {
struct State {
  std::map<std::string, std::vector<uint8_t>> files;
  bool failSync = false, failWrite = false, failClose = false;
  std::string statErrorPath;
  std::string readErrorPath;
  std::vector<uint8_t> revisionBytes;
  bool failNvsOpen = false, failNvsWrite = false, failNvsRead = false;
  unsigned failRename = 0, failRenameAgain = 0, renames = 0, yields = 0, errors = 0;
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
    return *this;
  }
  explicit operator bool() const { return opened; }
  bool isOpen() const { return opened; }
  // This fixture exercises inventory publication, not transfer lookup.
  bool prepareDirectoryEntry() { return false; }
  HalDirectoryResult nextEntry(HalFile&) { return HalDirectoryResult::Error; }
  size_t getName(char*, size_t) { return 0; }
  bool getShortName(char*, size_t) { return false; }
  bool close() {
    opened = false;
    return !inventory_hal_test::state.failClose;
  }
  bool isDirectory() const { return false; }
  uint64_t fileSize64() const { return inventory_hal_test::state.files.at(path).size(); }
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
    if (inventory_hal_test::state.failWrite) return 0;
    auto& bytes = inventory_hal_test::state.files.at(path);
    if (position + count > bytes.size()) bytes.resize(position + count);
    std::copy_n(static_cast<const uint8_t*>(input), count, bytes.begin() + position);
    position += count;
    return count;
  }
  bool sync() { return !inventory_hal_test::state.failSync; }

 private:
  std::string path;
  bool opened = false;
  uint64_t position = 0;
};
class TestHalStorage {
 public:
  static uint32_t foldFilenameCodepoint(uint32_t value) {
    return value >= 'a' && value <= 'z' ? value - ('a' - 'A') : value;
  }
  bool ready() const { return true; }
  bool openFileForReadReusing(const char* module, const char* path, HalFile& file) {
    return openFileForRead(module, path, file);
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    if (!inventory_hal_test::state.files.contains(path)) return false;
    file = HalFile(path);
    return true;
  }
  HalFile open(const char* path, int flags) {
    if (flags & O_TRUNC) inventory_hal_test::state.files[path].clear();
    return HalFile(path);
  }
};
inline TestHalStorage Storage;
