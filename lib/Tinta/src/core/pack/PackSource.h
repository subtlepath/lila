#pragma once

#include <cstdint>
#include <cstring>

namespace tinta::core::pack {

// Where a pack's bytes come from: memory (host tests, PSRAM boards) or a file
// read through a block cache (the SD card on the C3 boards).
class PackSource {
 public:
  virtual ~PackSource() = default;
  virtual uint32_t size() const = 0;
  // Copies len bytes at offset; false (out unspecified) on an I/O error or a
  // range outside [0, size()).
  virtual bool read(uint32_t offset, void* out, uint32_t len) = 0;
  // The whole pack when it sits in addressable memory, else nullptr. Pack
  // then hands out strings in place instead of copying them.
  virtual const uint8_t* data() const { return nullptr; }
};

class MemorySource final : public PackSource {
 public:
  MemorySource() = default;
  MemorySource(const uint8_t* bytes, uint32_t size) : bytes_(bytes), size_(size) {}
  void reset(const uint8_t* bytes, uint32_t size) {
    bytes_ = bytes;
    size_ = size;
  }

  uint32_t size() const override { return size_; }
  bool read(uint32_t offset, void* out, uint32_t len) override {
    if (bytes_ == nullptr || offset > size_ || len > size_ - offset) return false;
    std::memcpy(out, bytes_ + offset, len);
    return true;
  }
  const uint8_t* data() const override { return bytes_; }

 private:
  const uint8_t* bytes_ = nullptr;
  uint32_t size_ = 0;
};

// A read-through LRU cache of fixed blocks over a slow medium. The caller
// owns the block memory (blockCount * kBlockSize bytes) so the cache costs
// nothing while the app is closed.
class CachedSource : public PackSource {
 public:
  static constexpr uint32_t kBlockSize = 1024;
  static constexpr uint8_t kMaxBlocks = 32;

  // Medium size in bytes.
  void attach(uint32_t size, uint8_t* blocks, uint8_t blockCount);
  void detach();

  uint32_t size() const override { return size_; }
  bool read(uint32_t offset, void* out, uint32_t len) override;

  uint32_t misses() const { return misses_; }
  uint32_t ioErrors() const { return ioErrors_; }

 protected:
  // Reads len bytes at offset from the medium.
  virtual bool readRaw(uint32_t offset, uint8_t* out, uint32_t len) = 0;

 private:
  const uint8_t* block(uint32_t index);

  uint8_t* blocks_ = nullptr;
  uint32_t size_ = 0;
  uint32_t tag_[kMaxBlocks] = {};   // block index + 1; 0 = empty
  uint32_t used_[kMaxBlocks] = {};  // tick of last use
  uint32_t tick_ = 0;
  uint32_t misses_ = 0;
  uint32_t ioErrors_ = 0;
  uint8_t count_ = 0;
};

}  // namespace tinta::core::pack
