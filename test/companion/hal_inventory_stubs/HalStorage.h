#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace inventory_hal_test {
struct State {
  std::vector<uint8_t> bytes;
  bool failOpen = false;
  bool directory = false;
  bool failSeek = false;
  bool shortRead = false;
  unsigned opens = 0;
  unsigned closes = 0;
  unsigned reads = 0;
  unsigned yields = 0;
  unsigned errors = 0;
};
inline State state;
}  // namespace inventory_hal_test

class HalFile {
 public:
  HalFile() = default;
  explicit HalFile(bool opened) : opened(opened) {
    if (opened) ++inventory_hal_test::state.opens;
  }
  ~HalFile() { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  HalFile(HalFile&& other) : opened(std::exchange(other.opened, false)), position(other.position) {}
  HalFile& operator=(HalFile&& other) {
    close();
    opened = std::exchange(other.opened, false);
    position = other.position;
    return *this;
  }
  explicit operator bool() const { return opened; }
  bool close() {
    if (opened) ++inventory_hal_test::state.closes;
    opened = false;
    return true;
  }
  bool isDirectory() const { return inventory_hal_test::state.directory; }
  uint64_t fileSize64() const { return inventory_hal_test::state.bytes.size(); }
  bool seek64(uint64_t offset) {
    if (!opened || inventory_hal_test::state.failSeek || offset > fileSize64()) return false;
    position = offset;
    return true;
  }
  int read(void* output, size_t count) {
    auto& state = inventory_hal_test::state;
    ++state.reads;
    if (!opened || position > state.bytes.size()) return -1;
    count = std::min(count, state.bytes.size() - static_cast<size_t>(position));
    if (state.shortRead && count > 0) --count;
    std::copy_n(state.bytes.begin() + position, count, static_cast<uint8_t*>(output));
    position += count;
    return static_cast<int>(count);
  }

 private:
  bool opened = false;
  uint64_t position = 0;
};
class TestHalStorage {
 public:
  bool openFileForRead(const char*, const char*, HalFile& out) {
    out = HalFile(!inventory_hal_test::state.failOpen);
    return static_cast<bool>(out);
  }
};
inline TestHalStorage Storage;
