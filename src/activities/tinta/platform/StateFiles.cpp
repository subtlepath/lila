#if LILA_TINTA

#include "platform/StateFiles.h"

#include <stdio.h>
#include <string.h>

#include "CompanionCourseStatePaths.h"
#include "platform/Log.h"

namespace tinta::platform {
bool StateFiles::pathFor(const char* name, char (&out)[kPathCap], const char* suffix) const {
  if (!name || !name[0] || strlen(name) >= sizeof(cachedName_) || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    return false;
  for (const auto* at = name; *at; ++at) {
    const auto byte = static_cast<unsigned char>(*at);
    if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '.' ||
          byte == '-' || byte == '_'))
      return false;
  }
  const int n = snprintf(out, kPathCap, "%s/%s%s", root_, name, suffix);
  return n > 0 && static_cast<size_t>(n) < kPathCap;
}

void StateFiles::begin() {
  closeCached();
  strcpy(root_, "/tinta");
  beginRoot();
}
bool StateFiles::beginCourse(std::span<const uint8_t, 16> course) {
  closeCached();
  mounted_ = false;
  failed_ = false;
  companion::Identity identity{};
  std::copy(course.begin(), course.end(), identity.begin());
  if (!companion::courseStateDirectory(identity, root_)) {
    failed_ = true;
    log("storage: invalid course identity");
    return false;
  }
  if (!Storage.ensureDirectoryExists("/tinta") || !Storage.ensureDirectoryExists("/tinta/courses")) {
    log("storage: cannot create course state parent");
    return false;
  }
  beginRoot();
  if (!available()) return false;
  static constexpr const char* LEARNER_FILES[] = {"items.bin",   "reviews.log", "profile.bin", "days.bin",
                                                  "session.bin", "starred.bin", "read.bin"};
  for (const auto* name : LEARNER_FILES) {
    if (!recover(name)) return false;
  }
  return available();
}
void StateFiles::beginRoot() {
  failed_ = false;
  mounted_ = Storage.exists(root_) || Storage.mkdir(root_);
  if (!mounted_) {
    log("storage: cannot use %s, guest mode", root_);
    return;
  }
  log("storage: %s ready", root_);
}

void StateFiles::closeCached() {
  if (file_) file_.close();
  mode_ = Mode::None;
  cachedName_[0] = '\0';
}

void StateFiles::checkCard() {
  if (failed_ || Storage.exists(root_)) return;
  failed_ = true;
  closeCached();
  log("storage: card stopped responding, guest mode");
}

bool StateFiles::recover(const char* name) {
  char path[kPathCap];
  char tmp[kPathCap];
  if (!pathFor(name, path) || !pathFor(name, tmp, ".tmp")) return false;
  if (Storage.exists(path) || !Storage.exists(tmp)) return true;
  if (!Storage.rename(tmp, path)) {
    failed_ = true;
    closeCached();
    log("storage: recovery of %s failed", name);
    return false;
  }
  log("storage: recovered %s from an interrupted replace", name);
  return true;
}

bool StateFiles::openCached(const char* name, const Mode mode) {
  if (mode_ != Mode::None && strcmp(cachedName_, name) == 0 && (mode_ == Mode::Write || mode == Mode::Read)) {
    return true;
  }
  closeCached();
  char path[kPathCap];
  if (strlen(name) >= sizeof cachedName_ || !pathFor(name, path)) return false;
  if (!recover(name)) return false;
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
  if (ok) ok = file_.sync();
  if (!ok) {
    log("storage: write %s failed", name);
    checkCard();
  }
  return ok;
}

bool StateFiles::append(const char* name, const void* data, const uint32_t len) {
  if (!available() || !openCached(name, Mode::Write)) return false;
  const bool ok =
      file_.seekSet(file_.size()) && file_.write(static_cast<const uint8_t*>(data), len) == len && file_.sync();
  if (!ok) {
    log("storage: append %s failed", name);
    checkCard();
  }
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
    if (ok) ok = out.sync();
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
  if (Storage.exists(tmp) && !Storage.remove(tmp)) {
    log("storage: remove temporary %s failed", name);
    checkCard();
    return false;
  }
  const bool ok = !Storage.exists(path) || Storage.remove(path);
  if (!ok) {
    log("storage: remove %s failed", name);
    checkCard();
  }
  return ok;
}

}  // namespace tinta::platform

#endif  // LILA_TINTA
