#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <array>
#include <cstring>

// Session-owned: fixed buffers must not be placed on an embedded task stack.
class HalInventoryDirectoryWalker {
 public:
  static constexpr size_t MAX_DEPTH = 32;
  static constexpr size_t PATH_CAPACITY = 512;
  static constexpr size_t NAME_CAPACITY = 256;
  enum class Result { Entry, End, Error };

  ~HalInventoryDirectoryWalker() { close(); }
  HalInventoryDirectoryWalker() = default;
  HalInventoryDirectoryWalker(const HalInventoryDirectoryWalker&) = delete;
  HalInventoryDirectoryWalker& operator=(const HalInventoryDirectoryWalker&) = delete;

  bool begin(const char* root = "/") {
    if (!close()) return fail("Closing previous scan failed");
    failed = true;
    complete = false;
    pendingDirectory = false;
    depth = 0;
    if (!root || root[0] != '/') return fail("Invalid inventory root");
    size_t length = 0;
    while (length < path.size() && root[length] != 0) ++length;
    if (length == path.size()) return fail("Inventory root too long");
    memcpy(path.data(), root, length + 1);
    size_t normalized = length;
    while (normalized > 1 && path[normalized - 1] == '/') --normalized;
    path[normalized] = 0;
    prefixLengths[0] = normalized;
    if (!Storage.openFileForRead("COMPANION", path.data(), handles[0]) || !handles[0].isDirectory()) {
      return fail("Inventory root unavailable");
    }
    failed = false;
    return true;
  }

  // Path and file are borrowed until next(), begin(), or close().
  const char* entryPath() const { return path.data(); }
  HalFile& entryFile() { return handles[depth + 1]; }
  void skipDirectory() { pendingDirectory = false; }

  Result next() {
    if (failed) return Result::Error;
    if (complete) return Result::End;
    if (pendingDirectory) {
      if (depth + 1 >= MAX_DEPTH) return error("Inventory directory depth exceeded");
      ++depth;
      prefixLengths[depth] = strlen(path.data());
      pendingDirectory = false;
    } else if (!closeHandle(depth + 1)) {
      return error("Closing inventory entry failed");
    }
    for (;;) {
      path[prefixLengths[depth]] = 0;
      auto& entry = handles[depth + 1];
      if (!entry.prepareDirectoryEntry()) return error("Inventory entry allocation failed");
      const auto result = handles[depth].nextEntry(entry);
      if (result == HalDirectoryResult::Error) return error("Inventory directory read failed");
      if (result == HalDirectoryResult::End) {
        if (!closeHandle(depth + 1) || !closeHandle(depth)) return error("Closing inventory directory failed");
        if (depth == 0) {
          complete = true;
          return Result::End;
        }
        --depth;
        continue;
      }
      const size_t length = entry.getName(name.data(), name.size());
      if (length == 0 || length >= name.size() || name[length] != 0) return error("Inventory name unavailable");
      if (strcmp(name.data(), ".") == 0 || strcmp(name.data(), "..") == 0) {
        if (!closeHandle(depth + 1)) return error("Closing dot entry failed");
        continue;
      }
      for (size_t i = 0; i < length; ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c < 32 || c == 127 || c == '/' || c == '\\' || c == ':') return error("Invalid inventory name");
      }
      const size_t prefix = prefixLengths[depth];
      const size_t separator = prefix == 1 ? 0 : 1;
      if (prefix + separator + length >= path.size()) return error("Inventory path too long");
      if (separator) path[prefix] = '/';
      memcpy(path.data() + prefix + separator, name.data(), length + 1);
      pendingDirectory = entry.isDirectory();
      return Result::Entry;
    }
  }

  bool close() {
    bool ok = true;
    for (size_t i = handles.size(); i != 0; --i) {
      if (!closeHandle(i - 1)) ok = false;
    }
    pendingDirectory = false;
    complete = true;
    if (!ok) fail("Closing inventory scan failed");
    return ok;
  }

 private:
  std::array<HalFile, MAX_DEPTH + 1> handles;
  std::array<char, PATH_CAPACITY> path{};
  std::array<char, NAME_CAPACITY> name{};
  std::array<uint16_t, MAX_DEPTH> prefixLengths{};
  size_t depth = 0;
  bool failed = true;
  bool complete = false;
  bool pendingDirectory = false;

  bool closeHandle(size_t index) { return !handles[index].isOpen() || handles[index].close(); }
  bool fail(const char* reason) {
    LOG_ERR("COMPANION", "%s", reason);
    failed = true;
    return false;
  }
  Result error(const char* reason) {
    fail(reason);
    return Result::Error;
  }
};
