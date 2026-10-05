#include "core/pack/PackSource.h"

namespace tinta::core::pack {

void CachedSource::attach(const uint32_t size, uint8_t* blocks, const uint8_t blockCount) {
  detach();
  size_ = size;
  blocks_ = blocks;
  count_ = blocks == nullptr ? 0 : (blockCount < kMaxBlocks ? blockCount : kMaxBlocks);
}

void CachedSource::detach() {
  blocks_ = nullptr;
  size_ = 0;
  count_ = 0;
  tick_ = 0;
  std::memset(tag_, 0, sizeof tag_);
  std::memset(used_, 0, sizeof used_);
}

const uint8_t* CachedSource::block(const uint32_t index) {
  ++tick_;
  // An empty slot has tick 0, so the least recently used pick prefers it.
  uint8_t victim = 0;
  for (uint8_t i = 0; i < count_; ++i) {
    if (tag_[i] == index + 1) {
      used_[i] = tick_;
      return blocks_ + i * kBlockSize;
    }
    if (used_[i] < used_[victim]) victim = i;
  }
  uint8_t* slot = blocks_ + victim * kBlockSize;
  const uint32_t start = index * kBlockSize;
  const uint32_t len = size_ - start < kBlockSize ? size_ - start : kBlockSize;
  ++misses_;
  if (!readRaw(start, slot, len)) {
    ++ioErrors_;
    tag_[victim] = 0;
    used_[victim] = 0;
    return nullptr;
  }
  tag_[victim] = index + 1;
  used_[victim] = tick_;
  return slot;
}

bool CachedSource::read(uint32_t offset, void* out, uint32_t len) {
  if (offset > size_ || len > size_ - offset) return false;
  auto* dst = static_cast<uint8_t*>(out);
  if (count_ == 0) return len == 0 || readRaw(offset, dst, len);
  while (len > 0) {
    const uint32_t index = offset / kBlockSize;
    const uint32_t at = offset % kBlockSize;
    const uint32_t take = kBlockSize - at < len ? kBlockSize - at : len;
    const uint8_t* b = block(index);
    if (b == nullptr) return false;
    std::memcpy(dst, b + at, take);
    dst += take;
    offset += take;
    len -= take;
  }
  return true;
}

}  // namespace tinta::core::pack
