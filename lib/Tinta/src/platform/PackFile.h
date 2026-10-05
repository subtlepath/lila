#pragma once

// The course pack, read from the SD card through lila's HalStorage and a
// block cache (core::pack::CachedSource). The caller lends the block memory.

#include <HalStorage.h>

#include "core/pack/PackSource.h"

namespace tinta::platform {

// Where lila looks for the course: next to the progress files.
inline constexpr const char* kPackPath = "/tinta/course.pack";

class PackFile final : public core::pack::CachedSource {
 public:
  // False when there is no such file.
  bool open(const char* path, uint8_t* blocks, uint8_t blockCount);
  void close();

 protected:
  bool readRaw(uint32_t offset, uint8_t* out, uint32_t len) override;

 private:
  HalFile file_;
};

}  // namespace tinta::platform
