#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

#include "CompanionCoursePackArchive.h"
#include "HalCourseRemovalMetadata.h"
#include "HalInventoryFileHash.h"

namespace companion {
// Retain off stack after heap admission, with content/state writers excluded.
// File/lookup handles are reused; scratch belongs to the serialized transfer.
class HalCoursePackArchive final {
 public:
  using Permission = CoursePackArchive::Permission;
  HalCoursePackArchive(std::span<uint8_t> scratch, Permission permitted, void* context)
      : storage(permitted, context), archive(storage, scratch, permitted, context) {}
  ~HalCoursePackArchive() { closeReaders(); }
  CourseArchiveResult publish(const ContentManifest& manifest, std::string_view source) {
    archive.close();
    if (source.empty() || source.size() >= COURSE_ARCHIVE_PATH_SIZE || source.find('\0') != std::string_view::npos ||
        !validCourseBinding(manifest) || manifest.formatVersion != 1 || manifest.contentHash == Digest{} ||
        !courseStateDirectory(manifest.logicalIdentity, directory))
      return CourseArchiveResult::Invalid;
    if (!storage.prepare() || !storage.prepareCourse(directory.data())) return CourseArchiveResult::IoError;
    return finish(archive.publish(manifest, source));
  }
  CourseArchiveResult open(const Identity& course, const Digest& hash) { return finish(archive.open(course, hash)); }
  CourseArchiveResult inspectPrepared(const ContentManifest& manifest) {
    return finish(archive.inspectPrepared(manifest));
  }
  bool referenceIsPending() const { return archive.referenceIsPending(); }
  uint64_t referenceLength() const { return archive.referenceLength(); }
  const char* path() const { return archive.path(); }
  const ContentManifest* manifest() const { return archive.manifest(); }
  const char* referencePath() const { return archive.referencePath(); }
  bool closeReaders() {
    archive.close();
    return storage.closeReaders();
  }

 private:
  class ArchiveStorage final : public TransferStorage {
   public:
    ArchiveStorage(Permission permitted, void* context)
        : permitted(permitted), context(context), metadata(permitted, context) {}
    ~ArchiveStorage() override { closeReaders(); }
    bool prepare() override {
      return (guard() && Storage.ready() && Storage.ensureDirectoryExists(TRANSFER_DIRECTORY) && guard()) ||
             fail("prepare");
    }
    bool prepareCourse(const char* directory) {
      return (guard() && Storage.ensureDirectoryExists("/tinta") && guard() &&
              Storage.ensureDirectoryExists("/tinta/courses") && guard() && Storage.ensureDirectoryExists(directory) &&
              guard()) ||
             fail("course directory");
    }
    FileStatus stat(const char* path, uint64_t& size) override {
      if (!guard() || (writer.isOpen() && std::strcmp(path, writePath.data()) == 0 && !closeWriter()))
        return FileStatus::Error;
      return metadata.stat(path, size);
    }
    bool read(const char* path, uint64_t offset, std::span<uint8_t> bytes) override {
      const bool result = guard() && metadata.read(path, offset, bytes) && guard();
      vTaskDelay(1);
      return result || fail("read");
    }
    bool write(const char* path, uint64_t offset, std::span<const uint8_t> bytes, bool truncate) override {
      const auto length = strnlen(path, writePath.size());
      if (!guard() || length >= writePath.size()) return fail("write admission");
      if (!writer.isOpen() || std::strcmp(path, writePath.data()) != 0) {
        if (!closeWriter()) return false;
        if (offset == 0) {
          if (!Storage.openFileForWriteReusing("COMPANION", path, writer)) return fail("write open");
        } else {
          // A resumed prefix opens once; all remaining chunks reuse this handle.
          writer = Storage.open(path, O_WRONLY);
          if (!writer) return fail("resume open");
        }
        std::copy_n(path, length + 1, writePath.begin());
      }
      const bool written = guard() && !writer.isDirectory() && (!truncate || writer.truncate(0)) &&
                           writer.seek64(offset) && guard() &&
                           writer.write(bytes.data(), bytes.size()) == bytes.size() && guard() &&
                           writer.truncate(offset + bytes.size()) && writer.sync() && guard();
      vTaskDelay(1);
      return written || fail("write/sync");
    }
    bool resize(const char*, uint64_t) override { return fail("resize refused"); }
    bool remove(const char*) override { return fail("remove refused"); }
    bool rename(const char* from, const char* to) override {
      uint64_t size = 0;
      return (closeReaders() && guard() && metadata.stat(from, size) == FileStatus::Present && guard() &&
              metadata.stat(to, size) == FileStatus::Missing && guard() && Storage.rename(from, to) && guard()) ||
             fail("rename");
    }
    bool verify(const char* path, uint64_t length, const Digest& expected, std::span<uint8_t> scratch) override {
      uint64_t size = 0, actualLength = 0;
      Digest actual{};
      if (!closeReaders() || !guard() || metadata.stat(path, size) != FileStatus::Present || size != length ||
          !guard() || !Storage.openFileForReadReusing("COMPANION", path, reader))
        return fail("hash open");
      const bool hashed =
          !reader.isDirectory() && hashInventoryFile(reader, scratch, actualLength, actual, permitted, context);
      const bool synced = hashed && reader.sync();
      const bool closed = reader.close();
      return (hashed && synced && closed && guard() && actualLength == length && actual == expected) ||
             fail("hash/close");
    }
    bool closeReaders() {
      const bool writeClosed = closeWriter();
      const bool readClosed = !reader.isOpen() || reader.close();
      const bool metadataClosed = metadata.closeReaders();
      return writeClosed && readClosed && metadataClosed;
    }
    bool allowed() const { return guard(); }

   private:
    Permission permitted;
    void* context;
    HalCourseRemovalMetadata metadata;
    HalFile writer, reader;
    std::array<char, COURSE_ARCHIVE_PATH_SIZE> writePath{};
    bool guard() const { return permitted && permitted(context); }
    bool closeWriter() { return !writer.isOpen() || writer.close() || fail("write close"); }
    bool fail([[maybe_unused]] const char* operation) {
      LOG_ERR("COMPANION", "Course archive %s failed", operation);
      return false;
    }
  };
  ArchiveStorage storage;
  CoursePackArchive archive;
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> directory{};
  CourseArchiveResult finish(CourseArchiveResult result) {
    if (!storage.closeReaders() || !storage.allowed()) {
      archive.close();
      return CourseArchiveResult::IoError;
    }
    if (result != CourseArchiveResult::Ok) archive.close();
    return result;
  }
};
}  // namespace companion
