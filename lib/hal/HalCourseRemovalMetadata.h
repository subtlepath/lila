#pragma once

#include "HalCompanionFileLookup.h"

namespace companion {
// Read-only, retained off stack. Complete parent scans prove absence and reject
// duplicate names/aliases; caller excludes namespace and learner-state writers.
class HalCourseRemovalMetadata final : public TransferStorage {
 public:
  using Permission = bool (*)(void*);
  HalCourseRemovalMetadata(Permission permitted, void* context) : permitted(permitted), context(context) {}
  ~HalCourseRemovalMetadata() { closeReaders(); }
  bool prepare() override { return guard() && Storage.ready(); }
  FileStatus stat(const char* path, uint64_t& output) override {
    if (!closeReaders() || !guard() || !Storage.ready() || !path) return error("stat admission");
    const auto length = strnlen(path, INVENTORY_PATH_LIMIT + 1);
    const std::string_view view(path, length);
    if (!validInventoryPath(view) || !hal_filename::valid(view)) return error("stat path");
    const auto separator = view.find_last_of('/');
    const auto parentLength = separator ? separator : 1;
    std::copy_n(path, parentLength, parent.begin());
    parent[parentLength] = 0;
    const auto wanted = view.substr(separator + 1);
    if (!Storage.openFileForReadReusing("COMPANION", parent.data(), directory) || !guard() ||
        !directory.isDirectory() || !entry.prepareDirectoryEntry())
      return error("stat parent");
    bool found = false;
    uint64_t size = 0;
    unsigned steps = 0;
    for (;;) {
      if (!guard() || !closeEntry()) return error("stat permission/close");
      const auto result = directory.nextEntry(entry);
      if (!guard() || result == HalDirectoryResult::Error) return error("stat enumeration");
      if (result == HalDirectoryResult::End) {
        if (!closeReaders() || !guard()) return error("stat end close");
        if (!found) return FileStatus::Missing;
        output = size;
        return FileStatus::Present;
      }
      const auto count = entry.getName(name.data(), name.size());
      if (!guard() || !count || count >= name.size() || name[count] != 0) return error("stat name");
      const auto match = hal_filename::compare(wanted, std::string_view(name.data(), count), foldName);
      char alias[13]{};
      if (!entry.getShortName(alias, sizeof(alias)) || !guard()) return error("stat alias");
      const auto aliasLength = strnlen(alias, sizeof(alias));
      if (aliasLength == sizeof(alias)) return error("stat alias length");
      const auto aliasMatch = aliasLength
                                  ? hal_filename::compare(wanted, std::string_view(alias, aliasLength), foldName)
                                  : hal_filename::Comparison::Different;
      if (match == hal_filename::Comparison::Invalid || aliasMatch == hal_filename::Comparison::Invalid)
        return error("stat name encoding");
      if (match == hal_filename::Comparison::Equal || aliasMatch == hal_filename::Comparison::Equal) {
        if (found || entry.isDirectory() || !guard()) return error("stat collision");
        found = true;
        size = entry.fileSize64();
        if (!guard()) return error("stat size permission");
      }
      if (++steps == 32) {
        steps = 0;
        vTaskDelay(1);
      }
    }
  }
  bool read(const char* path, uint64_t offset, std::span<uint8_t> bytes) override {
    uint64_t size = 0;
    if (stat(path, size) != FileStatus::Present || offset > size || bytes.size() > size - offset || !guard() ||
        !Storage.openFileForReadReusing("COMPANION", path, file))
      return fail("read admission/open");
    const bool read = guard() && !file.isDirectory() && file.fileSize64() == size && file.seek64(offset) && guard() &&
                      file.read(bytes.data(), bytes.size()) == static_cast<int>(bytes.size()) && guard() && file.sync();
    const bool closed = closeReaders();
    return (read && closed && guard()) || fail("read/sync/close");
  }
  bool write(const char*, uint64_t, std::span<const uint8_t>, bool) override { return fail("write refused"); }
  bool resize(const char*, uint64_t) override { return fail("resize refused"); }
  bool rename(const char*, const char*) override { return fail("rename refused"); }
  bool remove(const char*) override { return fail("remove refused"); }
  bool verify(const char*, uint64_t, const Digest&, std::span<uint8_t>) override {
    return fail("hash request refused");
  }
  bool closeReaders() {
    const bool entryClosed = closeEntry();
    const bool directoryClosed = !directory.isOpen() || directory.close();
    const bool fileClosed = !file.isOpen() || file.close();
    return entryClosed && directoryClosed && fileClosed;
  }

 private:
  Permission permitted;
  void* context;
  HalFile directory, entry, file;
  std::array<char, INVENTORY_PATH_LIMIT + 1> parent{}, name{};
  bool guard() const { return permitted && permitted(context); }
  bool closeEntry() { return !entry.isOpen() || entry.close(); }
  static uint32_t foldName(uint32_t codepoint) { return Storage.foldFilenameCodepoint(codepoint); }
  bool fail(const char* operation) {
    LOG_ERR("COMPANION", "Course removal metadata %s failed", operation);
    closeReaders();
    return false;
  }
  FileStatus error(const char* operation) {
    fail(operation);
    return FileStatus::Error;
  }
};
}  // namespace companion
