#if LILA_TINTA

#include "platform/StateFiles.h"

#include <stdio.h>
#include <string.h>

#include "platform/Log.h"

namespace tinta::platform {
namespace {

constexpr const char* kRoot = "/tinta";
constexpr size_t kPathCap = 48;

bool pathFor(const char* name, char (&out)[kPathCap], const char* suffix = "") {
  const int n = snprintf(out, kPathCap, "%s/%s%s", kRoot, name, suffix);
  return n > 0 && static_cast<size_t>(n) < kPathCap;
}

}  // namespace

void StateFiles::begin() {
  failed_ = false;
  mounted_ = Storage.exists(kRoot) || Storage.mkdir(kRoot);
  if (!mounted_) {
    log("storage: cannot use %s, guest mode", kRoot);
    return;
  }
  log("storage: %s ready", kRoot);
}

void StateFiles::closeCached() {
  if (file_) file_.close();
  mode_ = Mode::None;
  cachedName_[0] = '\0';
}

void StateFiles::checkCard() {
  if (failed_ || Storage.exists(kRoot)) return;
  failed_ = true;
  closeCached();
  log("storage: card stopped responding, guest mode");
}

void StateFiles::recover(const char* name) {
  char path[kPathCap];
  char tmp[kPathCap];
  if (!pathFor(name, path) || !pathFor(name, tmp, ".tmp")) return;
  if (Storage.exists(path) || !Storage.exists(tmp)) return;
  if (Storage.rename(tmp, path)) log("storage: recovered %s from an interrupted replace", name);
}

bool StateFiles::openCached(const char* name, const Mode mode) {
  if (mode_ != Mode::None && strcmp(cachedName_, name) == 0 && (mode_ == Mode::Write || mode == Mode::Read)) {
    return true;
  }
  closeCached();
  char path[kPathCap];
  if (strlen(name) >= sizeof cachedName_ || !pathFor(name, path)) return false;
  recover(name);
  if (mode == Mode::Read) {
    if (!Storage.exists(path)) {
      checkCard();
      return false;
    }
    file_ = Storage.open(path, O_RDONLY);
  } else {
    file_ = Storage.open(path, O_RDWR | O_CREAT);
  }
  if (!file_) {
    // The file exists (or was to be created) and still did not open.
    checkCard();
    if (!failed_) log("storage: cannot open %s", name);
    return false;
  }
  mode_ = mode;
  strcpy(cachedName_, name);
  return true;
}

int32_t StateFiles::size(const char* name) {
  if (!available() || !openCached(name, Mode::Read)) return -1;
  return static_cast<int32_t>(file_.size());
}

int32_t StateFiles::read(const char* name, const uint32_t offset, void* buffer, const uint32_t len) {
  if (!available() || !openCached(name, Mode::Read)) return -1;
  if (offset > file_.size()) return 0;
  if (!file_.seekSet(offset)) {
    checkCard();
    return -1;
  }
  const int n = file_.read(buffer, len);
  if (n < 0) {
    checkCard();
    return -1;
  }
  return n;
}

bool StateFiles::write(const char* name, const uint32_t offset, const void* data, const uint32_t len) {
  if (!available() || !openCached(name, Mode::Write)) return false;
  const size_t size = file_.size();
  bool ok = file_.seekSet(offset < size ? offset : size);
  // Extending past the end: fill the gap with zeros.
  static const uint8_t kZeros[32] = {};
  for (size_t at = size; ok && at < offset;) {
    const uint32_t n = offset - at < sizeof kZeros ? static_cast<uint32_t>(offset - at) : sizeof kZeros;
    ok = file_.write(kZeros, n) == n;
    at += n;
  }
  ok = ok && file_.write(static_cast<const uint8_t*>(data), len) == len;
  if (ok) file_.flush();
  if (!ok) checkCard();
  return ok;
}

bool StateFiles::append(const char* name, const void* data, const uint32_t len) {
  if (!available() || !openCached(name, Mode::Write)) return false;
  const bool ok = file_.seekSet(file_.size()) && file_.write(static_cast<const uint8_t*>(data), len) == len;
  if (ok) file_.flush();
  if (!ok) checkCard();
  return ok;
}

bool StateFiles::replace(const char* name, const void* data, const uint32_t len) {
  if (!available()) return false;
  closeCached();
  char path[kPathCap];
  char tmp[kPathCap];
  if (!pathFor(name, path) || !pathFor(name, tmp, ".tmp")) return false;
  bool ok = false;
  {
    HalFile out = Storage.open(tmp, O_RDWR | O_CREAT | O_TRUNC);
    ok = static_cast<bool>(out) && out.write(static_cast<const uint8_t*>(data), len) == len;
    if (ok) out.flush();
    // Closed before the rename.
  }
  // Only once the new copy is safely on the card does the old one go.
  ok = ok && (!Storage.exists(path) || Storage.remove(path)) && Storage.rename(tmp, path);
  if (!ok) {
    checkCard();
    if (!failed_) log("storage: replace %s failed", name);
  }
  return ok;
}

bool StateFiles::remove(const char* name) {
  if (!available()) return false;
  closeCached();
  char path[kPathCap];
  char tmp[kPathCap];
  if (!pathFor(name, path) || !pathFor(name, tmp, ".tmp")) return false;
  if (Storage.exists(tmp)) Storage.remove(tmp);
  const bool ok = !Storage.exists(path) || Storage.remove(path);
  if (!ok) checkCard();
  return ok;
}

}  // namespace tinta::platform

#endif  // LILA_TINTA
