#pragma once

#include <algorithm>

#include "CompanionTransfer.h"

namespace companion {
inline constexpr size_t COURSE_BINDING_SIZE = 71;
inline constexpr char ACTIVE_COURSE_PATH[] = "/tinta/course.pack";
inline constexpr char COURSE_BINDING_PATH[] = "/.crosspoint/companion/course-binding";
inline constexpr char COURSE_BINDING_STAGE[] = "/.crosspoint/companion/course-binding.tmp";
inline constexpr char COURSE_BINDING_BACKUP[] = "/.crosspoint/companion/course-binding.bak";

enum class CourseBindingResult { Ok, Invalid, IoError, Corrupt, DifferentCourse, HashMismatch, UnboundHistory };

inline bool validCourseBinding(const ContentManifest& manifest) {
  return manifest.kind == ContentKind::Course && manifest.length > 0 && manifest.formatVersion > 0 &&
         std::any_of(manifest.logicalIdentity.begin(), manifest.logicalIdentity.end(),
                     [](uint8_t byte) { return byte != 0; });
}
inline uint32_t courseBindingCrc(std::span<const uint8_t> bytes) {
  uint32_t value = 0xffffffff;
  for (const auto byte : bytes) {
    value ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (0xedb88320U & (0U - (value & 1U)));
  }
  return ~value;
}
inline size_t encodeCourseBinding(const ContentManifest& manifest, std::span<uint8_t> output) {
  if (output.size() < COURSE_BINDING_SIZE || !validCourseBinding(manifest)) return 0;
  auto bytes = output.first(COURSE_BINDING_SIZE);
  bytes[0] = 'L';
  bytes[1] = 'C';
  bytes[2] = 'B';
  bytes[3] = 1;
  if (encodeRecord(manifest, bytes.subspan(4, CONTENT_MANIFEST_SIZE)) != CONTENT_MANIFEST_SIZE) return 0;
  const auto checksum = courseBindingCrc(bytes.first(67));
  for (size_t i = 0; i < 4; ++i) bytes[67 + i] = static_cast<uint8_t>(checksum >> (8 * i));
  return COURSE_BINDING_SIZE;
}
inline bool decodeCourseBinding(std::span<const uint8_t> bytes, ContentManifest& manifest) {
  if (bytes.size() != COURSE_BINDING_SIZE || bytes[0] != 'L' || bytes[1] != 'C' || bytes[2] != 'B' || bytes[3] != 1)
    return false;
  uint32_t checksum = 0;
  for (size_t i = 0; i < 4; ++i) checksum |= uint32_t(bytes[67 + i]) << (8 * i);
  ContentManifest parsed;
  if (checksum != courseBindingCrc(bytes.first(67)) || !decodeRecord(bytes.subspan(4, CONTENT_MANIFEST_SIZE), parsed) ||
      !validCourseBinding(parsed))
    return false;
  manifest = parsed;
  return true;
}
inline CourseBindingResult readCourseBinding(TransferStorage& storage, const char* path, std::span<uint8_t> scratch,
                                             ContentManifest& manifest, bool& present) {
  if (scratch.size() < COURSE_BINDING_SIZE) return CourseBindingResult::Invalid;
  uint64_t size = 0;
  const auto status = storage.stat(path, size);
  if (status == FileStatus::Error) return CourseBindingResult::IoError;
  present = status == FileStatus::Present;
  if (!present) return CourseBindingResult::Ok;
  if (size != COURSE_BINDING_SIZE) return CourseBindingResult::Corrupt;
  auto bytes = scratch.first(COURSE_BINDING_SIZE);
  if (!storage.read(path, 0, bytes)) return CourseBindingResult::IoError;
  return decodeCourseBinding(bytes, manifest) ? CourseBindingResult::Ok : CourseBindingResult::Corrupt;
}
inline bool removeCourseBindingFile(TransferStorage& storage, const char* path) {
  uint64_t size = 0;
  const auto status = storage.stat(path, size);
  return status == FileStatus::Missing || (status == FileStatus::Present && storage.remove(path));
}

// An unbound legacy pack must first be confirmed by transferring its exact bytes.
// Missing packs with retained learner state require migration before activation.
inline CourseBindingResult authorizeCourseReplacement(TransferStorage& storage, const ContentManifest& candidate,
                                                      bool hasLearnerState, std::span<uint8_t> scratch) {
  if (scratch.size() < COURSE_BINDING_SIZE || !validCourseBinding(candidate)) return CourseBindingResult::Invalid;
  ContentManifest current;
  bool present = false;
  const auto result = readCourseBinding(storage, COURSE_BINDING_PATH, scratch, current, present);
  if (result != CourseBindingResult::Ok) return result;
  uint64_t size = 0;
  for (const auto* path : {COURSE_BINDING_STAGE, COURSE_BINDING_BACKUP}) {
    const auto status = storage.stat(path, size);
    if (status == FileStatus::Error) return CourseBindingResult::IoError;
    if (status != FileStatus::Missing) return CourseBindingResult::Corrupt;
  }
  if (present) {
    if (current.logicalIdentity != candidate.logicalIdentity) return CourseBindingResult::DifferentCourse;
    return storage.verify(ACTIVE_COURSE_PATH, current.length, current.contentHash, scratch)
               ? CourseBindingResult::Ok
               : CourseBindingResult::HashMismatch;
  }
  const auto status = storage.stat(ACTIVE_COURSE_PATH, size);
  if (status == FileStatus::Error) return CourseBindingResult::IoError;
  if (status == FileStatus::Missing)
    return hasLearnerState ? CourseBindingResult::UnboundHistory : CourseBindingResult::Ok;
  return storage.verify(ACTIVE_COURSE_PATH, candidate.length, candidate.contentHash, scratch)
             ? CourseBindingResult::Ok
             : CourseBindingResult::UnboundHistory;
}

// Call only while a durable Installing transfer retains this exact manifest.
// Recovery repeats this operation before committing the parent transaction.
inline CourseBindingResult installCourseBinding(TransferStorage& storage, const ContentManifest& manifest,
                                                std::span<uint8_t> scratch) {
  if (scratch.size() < COURSE_BINDING_SIZE || !validCourseBinding(manifest)) return CourseBindingResult::Invalid;
  if (!storage.verify(ACTIVE_COURSE_PATH, manifest.length, manifest.contentHash, scratch))
    return CourseBindingResult::HashMismatch;
  ContentManifest active;
  bool hasActive = false, hasBackup = false;
  auto result = readCourseBinding(storage, COURSE_BINDING_PATH, scratch, active, hasActive);
  if (result != CourseBindingResult::Ok) return result;
  if (hasActive && active.logicalIdentity != manifest.logicalIdentity) return CourseBindingResult::DifferentCourse;
  const bool alreadyInstalled = hasActive && active == manifest;
  result = readCourseBinding(storage, COURSE_BINDING_BACKUP, scratch, active, hasBackup);
  if (result != CourseBindingResult::Ok) return result;
  if (hasBackup && active.logicalIdentity != manifest.logicalIdentity) return CourseBindingResult::DifferentCourse;
  if (alreadyInstalled) {
    return removeCourseBindingFile(storage, COURSE_BINDING_STAGE) &&
                   removeCourseBindingFile(storage, COURSE_BINDING_BACKUP)
               ? CourseBindingResult::Ok
               : CourseBindingResult::IoError;
  }
  if (hasActive && hasBackup) return CourseBindingResult::Corrupt;
  auto bytes = scratch.first(COURSE_BINDING_SIZE);
  if (encodeCourseBinding(manifest, bytes) != COURSE_BINDING_SIZE ||
      !storage.write(COURSE_BINDING_STAGE, 0, bytes, true))
    return CourseBindingResult::IoError;
  if (hasActive && !storage.rename(COURSE_BINDING_PATH, COURSE_BINDING_BACKUP)) return CourseBindingResult::IoError;
  if (!storage.rename(COURSE_BINDING_STAGE, COURSE_BINDING_PATH)) return CourseBindingResult::IoError;
  bool present = false;
  result = readCourseBinding(storage, COURSE_BINDING_PATH, scratch, active, present);
  if (result != CourseBindingResult::Ok) return result;
  if (!present || active != manifest) return CourseBindingResult::Corrupt;
  return removeCourseBindingFile(storage, COURSE_BINDING_BACKUP) ? CourseBindingResult::Ok
                                                                 : CourseBindingResult::IoError;
}
}  // namespace companion
