#pragma once

#include "CompanionCourseBinding.h"
#include "CompanionCourseStatePaths.h"

namespace companion {
inline constexpr size_t COURSE_ARCHIVE_PATH_SIZE = 128;
inline constexpr std::string_view COURSE_ARCHIVE_PREFIX = "/.crosspoint/companion/course-pack-";
enum class CourseArchiveResult { Ok, Missing, Invalid, Busy, Conflict, Corrupt, IoError };

// Retain off stack. The caller excludes namespace writers, prepares parent
// directories, validates the complete source pack, and lends disjoint scratch
// and a checked storage implementation.
// References are append-only; the consumer checks every version in a course scope.
class CoursePackArchive final {
 public:
  using Permission = bool (*)(void*);
  CoursePackArchive(TransferStorage& storage, std::span<uint8_t> scratch, Permission permitted, void* context)
      : storage(storage), scratch(scratch), permitted(permitted), context(context) {}
  CourseArchiveResult publish(const ContentManifest& manifest, std::string_view source) {
    ready = false;
    if (!valid(manifest) || source.empty() || source.size() >= sourcePath.size() ||
        source.find('\0') != std::string_view::npos || scratch.size() < 2 * COURSE_BINDING_SIZE)
      return CourseArchiveResult::Invalid;
    if (!guard()) return CourseArchiveResult::Busy;
    current = manifest;
    std::copy(source.begin(), source.end(), sourcePath.begin());
    sourcePath[source.size()] = 0;
    if (!paths(current.logicalIdentity, current.contentHash)) return CourseArchiveResult::Invalid;
    if (!verify(sourcePath.data())) return CourseArchiveResult::Corrupt;
    auto result = retain();
    if (result != CourseArchiveResult::Ok) return result;
    bool present = false;
    result = record(reference.data(), observed, present);
    if (result != CourseArchiveResult::Ok) return result;
    uint64_t size = 0;
    const auto staged = status(referenceStage.data(), size);
    if (staged == FileStatus::Error) return CourseArchiveResult::IoError;
    if (present) {
      if (observed != current) return CourseArchiveResult::Conflict;
      if (staged != FileStatus::Missing) return CourseArchiveResult::Conflict;
    } else {
      const auto expected = scratch.first(COURSE_BINDING_SIZE);
      if (encodeCourseBinding(current, expected) != expected.size()) return CourseArchiveResult::Invalid;
      if (staged == FileStatus::Present) {
        if (size > expected.size()) return CourseArchiveResult::Corrupt;
        const auto saved = scratch.subspan(COURSE_BINDING_SIZE, static_cast<size_t>(size));
        if (size && (!storage.read(referenceStage.data(), 0, saved) || !guard())) return CourseArchiveResult::IoError;
        if (!std::equal(saved.begin(), saved.end(), expected.begin())) return CourseArchiveResult::Conflict;
      }
      if (staged == FileStatus::Missing || size != COURSE_BINDING_SIZE) {
        if (!storage.write(referenceStage.data(), 0, expected, true) || !guard()) return CourseArchiveResult::IoError;
      }
      result = record(referenceStage.data(), observed, present);
      if (result != CourseArchiveResult::Ok) return result;
      if (!present || observed != current) return CourseArchiveResult::Conflict;
      if (status(reference.data(), size) != FileStatus::Missing || !guard()) return CourseArchiveResult::Conflict;
      if (!storage.rename(referenceStage.data(), reference.data()) || !guard()) return CourseArchiveResult::IoError;
    }
    return open(current.logicalIdentity, current.contentHash);
  }
  CourseArchiveResult open(const Identity& course, const Digest& hash) {
    ready = false;
    pendingReference = false;
    referenceSize = COURSE_BINDING_SIZE;
    if (scratch.size() < 2 * COURSE_BINDING_SIZE || course == Identity{} || hash == Digest{} || !paths(course, hash))
      return CourseArchiveResult::Invalid;
    if (!guard()) return CourseArchiveResult::Busy;
    uint64_t size = 0;
    const auto staged = status(referenceStage.data(), size);
    if (staged == FileStatus::Error) return CourseArchiveResult::IoError;
    if (staged != FileStatus::Missing) return CourseArchiveResult::Busy;
    bool present = false;
    auto result = record(reference.data(), observed, present);
    if (result != CourseArchiveResult::Ok) return result;
    if (!present) return CourseArchiveResult::Missing;
    if (observed.logicalIdentity != course || observed.contentHash != hash) return CourseArchiveResult::Conflict;
    current = observed;
    result = record(ownerPath.data(), owner, present);
    if (result != CourseArchiveResult::Ok) return result;
    if (!present || owner != current) return CourseArchiveResult::Conflict;
    const auto copying = status(cacheStage.data(), size);
    if (copying == FileStatus::Error) return CourseArchiveResult::IoError;
    if (copying != FileStatus::Missing) return CourseArchiveResult::Busy;
    if (!verify(cache.data())) return CourseArchiveResult::Corrupt;
    ready = guard();
    return ready ? CourseArchiveResult::Ok : CourseArchiveResult::Busy;
  }
  // Read-only Prepared evidence: a matching reference prefix is meaningful only
  // with the exact owner and a fully verified, immutable cache. Caller proves
  // native Prepared intent/consent and live state before resuming publication.
  CourseArchiveResult inspectPrepared(const ContentManifest& manifest) {
    ready = false;
    pendingReference = false;
    referenceSize = COURSE_BINDING_SIZE;
    if (!valid(manifest) || scratch.size() < 2 * COURSE_BINDING_SIZE) return CourseArchiveResult::Invalid;
    current = manifest;
    if (!paths(current.logicalIdentity, current.contentHash) || !guard()) return CourseArchiveResult::Busy;
    uint64_t size = 0;
    const auto canonical = status(reference.data(), size);
    const auto staged = status(referenceStage.data(), referenceSize);
    if (canonical == FileStatus::Error || staged == FileStatus::Error) return CourseArchiveResult::IoError;
    if (canonical == FileStatus::Present && staged != FileStatus::Missing) return CourseArchiveResult::Conflict;
    bool present = false;
    if (canonical == FileStatus::Present) {
      auto result = record(reference.data(), observed, present);
      if (result != CourseArchiveResult::Ok) return result;
      if (!present || observed != current) return CourseArchiveResult::Conflict;
      referenceSize = COURSE_BINDING_SIZE;
    } else {
      if (staged == FileStatus::Missing) return CourseArchiveResult::Missing;
      if (referenceSize > COURSE_BINDING_SIZE) return CourseArchiveResult::Corrupt;
      const auto expected = scratch.first(COURSE_BINDING_SIZE);
      if (encodeCourseBinding(current, expected) != expected.size()) return CourseArchiveResult::Invalid;
      const auto saved = scratch.subspan(COURSE_BINDING_SIZE, static_cast<size_t>(referenceSize));
      if (referenceSize && (!storage.read(referenceStage.data(), 0, saved) || !guard()))
        return CourseArchiveResult::IoError;
      if (!std::equal(saved.begin(), saved.end(), expected.begin())) return CourseArchiveResult::Conflict;
      pendingReference = true;
    }
    const auto result = record(ownerPath.data(), owner, present);
    if (result != CourseArchiveResult::Ok) return result;
    if (!present || owner != current) return CourseArchiveResult::Conflict;
    const auto copying = status(cacheStage.data(), size);
    if (copying == FileStatus::Error) return CourseArchiveResult::IoError;
    if (copying != FileStatus::Missing) return CourseArchiveResult::Busy;
    if (!verify(cache.data())) return CourseArchiveResult::Corrupt;
    ready = guard();
    return ready ? CourseArchiveResult::Ok : CourseArchiveResult::Busy;
  }
  void close() { ready = false; }
  const char* path() const {
    if (!ready) return nullptr;
    if (!guard()) {
      ready = false;
      return nullptr;
    }
    return cache.data();
  }
  const ContentManifest* manifest() const { return path() ? &current : nullptr; }
  const char* referencePath() const {
    return path() ? (pendingReference ? referenceStage.data() : reference.data()) : nullptr;
  }
  bool referenceIsPending() const { return path() && pendingReference; }
  uint64_t referenceLength() const { return path() ? referenceSize : 0; }

 private:
  TransferStorage& storage;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  std::array<char, COURSE_ARCHIVE_PATH_SIZE> cache{}, cacheStage{}, ownerPath{}, reference{}, referenceStage{},
      sourcePath{};
  ContentManifest current{}, observed{}, owner{};
  uint64_t referenceSize = COURSE_BINDING_SIZE;
  bool pendingReference = false;
  mutable bool ready = false;
  static bool valid(const ContentManifest& manifest) {
    return validCourseBinding(manifest) && manifest.formatVersion == 1 && manifest.contentHash != Digest{};
  }
  bool guard() const { return permitted && permitted(context); }
  FileStatus status(const char* path, uint64_t& size) {
    if (!guard()) return FileStatus::Error;
    const auto found = storage.stat(path, size);
    return guard() ? found : FileStatus::Error;
  }
  bool verify(const char* path) {
    uint64_t size = 0;
    return status(path, size) == FileStatus::Present && size == current.length &&
           storage.verify(path, current.length, current.contentHash, scratch) && guard();
  }
  CourseArchiveResult record(const char* path, ContentManifest& output, bool& present) {
    if (!guard()) return CourseArchiveResult::Busy;
    const auto result = readCourseBinding(storage, path, scratch, output, present);
    if (!guard()) return CourseArchiveResult::Busy;
    if (result == CourseBindingResult::IoError) return CourseArchiveResult::IoError;
    if (result != CourseBindingResult::Ok || (present && !valid(output))) return CourseArchiveResult::Corrupt;
    return CourseArchiveResult::Ok;
  }
  bool paths(const Identity& course, const Digest& hash) {
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    static_assert(COURSE_ARCHIVE_PREFIX.size() + 64 + 7 < COURSE_ARCHIVE_PATH_SIZE);
    static_assert(COURSE_STATE_DIRECTORY_SIZE + 5 + 64 + 4 < COURSE_ARCHIVE_PATH_SIZE);
    if (!courseStateDirectory(course, reference)) return false;
    size_t at = COURSE_ARCHIVE_PREFIX.size();
    std::copy(COURSE_ARCHIVE_PREFIX.begin(), COURSE_ARCHIVE_PREFIX.end(), cache.begin());
    for (auto byte : hash) {
      cache[at++] = HEX_DIGITS[byte >> 4];
      cache[at++] = HEX_DIGITS[byte & 15];
    }
    cache[at] = 0;
    std::copy_n(cache.begin(), at, cacheStage.begin());
    std::copy_n(cache.begin(), at, ownerPath.begin());
    std::copy_n(".tmp", 5, cacheStage.begin() + at);
    std::copy_n(".owner", 7, ownerPath.begin() + at);
    at = COURSE_STATE_DIRECTORY_SIZE - 1;
    std::copy_n("/pack-", 6, reference.begin() + at);
    at += 6;
    for (auto byte : hash) {
      reference[at++] = HEX_DIGITS[byte >> 4];
      reference[at++] = HEX_DIGITS[byte & 15];
    }
    std::copy_n(reference.begin(), at, referenceStage.begin());
    std::copy_n(".ref", 5, reference.begin() + at);
    std::copy_n(".tmp", 5, referenceStage.begin() + at);
    return true;
  }
  CourseArchiveResult retain() {
    bool present = false;
    auto result = record(ownerPath.data(), owner, present);
    if (result != CourseArchiveResult::Ok) return result;
    uint64_t size = 0;
    const auto cached = status(cache.data(), size);
    if (cached == FileStatus::Error) return CourseArchiveResult::IoError;
    const auto staged = status(cacheStage.data(), size);
    if (staged == FileStatus::Error) return CourseArchiveResult::IoError;
    if (!present) {
      if (cached != FileStatus::Missing || staged != FileStatus::Missing) return CourseArchiveResult::Conflict;
      const auto bytes = scratch.first(COURSE_BINDING_SIZE);
      if (encodeCourseBinding(current, bytes) != bytes.size()) return CourseArchiveResult::Invalid;
      if (!storage.write(ownerPath.data(), 0, bytes, true) || !guard()) return CourseArchiveResult::IoError;
      result = record(ownerPath.data(), owner, present);
      if (result != CourseArchiveResult::Ok) return result;
    }
    if (!present || owner != current) return CourseArchiveResult::Conflict;
    if (cached == FileStatus::Present) {
      if (staged != FileStatus::Missing) return CourseArchiveResult::Conflict;
      return verify(cache.data()) ? CourseArchiveResult::Ok : CourseArchiveResult::Corrupt;
    }
    const size_t half = scratch.size() / 2;
    uint64_t offset = staged == FileStatus::Present ? size : 0;
    if (offset > current.length) return CourseArchiveResult::Corrupt;
    for (uint64_t at = 0; at < offset;) {
      const size_t count = static_cast<size_t>(std::min<uint64_t>(half, offset - at));
      const auto saved = scratch.first(count), source = scratch.subspan(half, count);
      if (!guard() || !storage.read(cacheStage.data(), at, saved) || !guard() ||
          !storage.read(sourcePath.data(), at, source) || !guard())
        return CourseArchiveResult::IoError;
      if (!std::equal(saved.begin(), saved.end(), source.begin())) return CourseArchiveResult::Conflict;
      at += count;
    }
    while (offset < current.length) {
      const size_t count = static_cast<size_t>(std::min<uint64_t>(scratch.size(), current.length - offset));
      const auto bytes = scratch.first(count);
      if (!guard() || !storage.read(sourcePath.data(), offset, bytes) || !guard() ||
          !storage.write(cacheStage.data(), offset, bytes, offset == 0) || !guard())
        return CourseArchiveResult::IoError;
      offset += count;
    }
    if (!verify(sourcePath.data()) || !verify(cacheStage.data())) return CourseArchiveResult::Corrupt;
    if (status(cache.data(), size) != FileStatus::Missing || !guard()) return CourseArchiveResult::Conflict;
    if (!storage.rename(cacheStage.data(), cache.data()) || !guard()) return CourseArchiveResult::IoError;
    return verify(cache.data()) ? CourseArchiveResult::Ok : CourseArchiveResult::Corrupt;
  }
};
}  // namespace companion
