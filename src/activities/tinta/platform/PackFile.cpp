#if LILA_TINTA

#include "platform/PackFile.h"

namespace tinta::platform {

bool PackFile::open(const char* path, uint8_t* blocks, const uint8_t blockCount) {
  close();
  if (!Storage.exists(path)) return false;
  file_ = Storage.open(path, O_RDONLY);
  if (!file_) return false;
  attach(static_cast<uint32_t>(file_.size()), blocks, blockCount);
  return true;
}

void PackFile::close() {
  detach();
  if (file_) file_.close();
}

bool PackFile::readRaw(const uint32_t offset, uint8_t* out, const uint32_t len) {
  return file_.seekSet(offset) && file_.read(out, len) == static_cast<int>(len);
}

}  // namespace tinta::platform

#endif  // LILA_TINTA
