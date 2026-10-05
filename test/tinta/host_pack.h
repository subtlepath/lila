#pragma once

// Course packs for host tests: in memory, or (TINTA_PACK_SOURCE=file) read
// from the file through CachedSource and string copies, as the SD card reads
// them on the device.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/pack/Pack.h"
#include "core/pack/PackSource.h"

namespace tinta_test {

class HostFileSource final : public tinta::core::pack::CachedSource {
 public:
  ~HostFileSource() override { close(); }

  bool open(const char* path, uint8_t blocks) {
    close();
    file_ = std::fopen(path, "rb");
    if (!file_) return false;
    std::fseek(file_, 0, SEEK_END);
    const long size = std::ftell(file_);
    memory_.assign(static_cast<size_t>(blocks) * kBlockSize, 0);
    attach(static_cast<uint32_t>(size), memory_.data(), blocks);
    return true;
  }
  void close() {
    detach();
    if (file_) std::fclose(file_);
    file_ = nullptr;
  }
  uint32_t rawReads() const { return rawReads_; }

 protected:
  bool readRaw(uint32_t offset, uint8_t* out, uint32_t len) override {
    ++rawReads_;
    return std::fseek(file_, static_cast<long>(offset), SEEK_SET) == 0 && std::fread(out, 1, len, file_) == len;
  }

 private:
  std::FILE* file_ = nullptr;
  std::vector<uint8_t> memory_;
  uint32_t rawReads_ = 0;
};

inline bool fileSourceMode() {
  const char* mode = std::getenv("TINTA_PACK_SOURCE");
  return mode && std::strcmp(mode, "file") == 0;
}

struct LoadedPack {
  std::vector<uint8_t> image;
  HostFileSource file;
  // Tests read many strings without passes in between: room for all of them.
  std::vector<char> arena;
  tinta::core::pack::Pack pack;

  bool load(const char* path) {
    if (fileSourceMode()) {
      arena.assign(8u << 20, 0);
      pack.setArena(arena.data(), static_cast<uint32_t>(arena.size()));
      return file.open(path, 8) && pack.open(file) == tinta::core::pack::PackStatus::Ok;
    }
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    uint8_t chunk[4096];
    size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) image.insert(image.end(), chunk, chunk + n);
    std::fclose(f);
    return pack.open(image.data(), static_cast<uint32_t>(image.size())) == tinta::core::pack::PackStatus::Ok;
  }
};

}  // namespace tinta_test
