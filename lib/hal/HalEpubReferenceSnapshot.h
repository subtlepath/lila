#pragma once

#include "HalEpubReferenceJsonIo.h"
#include "HalInventoryFileHash.h"
#include "HalInventoryPathLookup.h"
#include "HalRemovalMetadataPaths.h"
#include "HalRemovalMetadataSnapshotStorage.h"
#include "HalRemovalReferencePath.h"

namespace companion {
// The caller excludes metadata writers and verifies the content plan/path.
// This session-owned object borrows the JSON workspace and IO scratch, which is
// separate from declaration storage scratch. Release the JSON arena after prep.
class HalEpubReferenceSnapshot final {
 public:
  HalEpubReferenceSnapshot(ContentRemovalJournal& journal, HalRemovalMetadataSnapshotStorage& declarations,
                           EpubReferenceJson& json, std::span<uint8_t> scratch)
      : journal(journal), declarations(declarations), json(json), scratch(scratch) {}
  ~HalEpubReferenceSnapshot() { close(); }
  bool prepare(const ContentRemovalRecord& owner, RemovalMetadataFile kind, std::string_view removedPath,
               RemovalMetadataSnapshot& output) {
    if (!validInventoryPath(removedPath)) return failure("path arguments");
    return prepareMatching(
        owner, kind,
        [](void* opaque, std::string_view path, bool& matched) {
          const auto& removed = *static_cast<std::string_view*>(opaque);
          matched = removalReferencePathEqual(nullptr, path, removed);
          return true;
        },
        &removedPath, output);
  }
  bool prepareMatching(const ContentRemovalRecord& owner, RemovalMetadataFile kind, EpubReferenceJson::PathMatch match,
                       void* context, RemovalMetadataSnapshot& output) {
    if (!validContentRemovalRecord(owner) || owner.phase != ContentRemovalPhase::Quarantined ||
        owner.request.manifest.kind != ContentKind::Epub || !match || scratch.empty() || !journal.current() ||
        *journal.current() != owner || !close())
      return failure("arguments/ownership");
    checkpoint = owner;
    const auto receipt = declarations.load(owner.planHash, kind, expected);
    if (!guard()) return failure("declaration ownership");
    if (receipt == RemovalMetadataStorageResult::Ok) {
      if (expected.request != owner.request || expected.planHash != owner.planHash || expected.file != kind)
        return failure("foreign declaration");
      output = expected;
      return true;
    }
    if (receipt != RemovalMetadataStorageResult::Missing) return failure("declaration lookup");
    expected.previousHash.fill(0);
    expected.nextHash.fill(0);
    expected.previousLength = expected.nextLength = 0;
    expected.request = owner.request;
    expected.planHash = owner.planHash;
    expected.file = kind;
    if (!removalMetadataPaths(owner.planHash, kind, candidate, backup)) return failure("paths");
    target = kind == RemovalMetadataFile::State ? "/.crosspoint/state.json" : "/.crosspoint/recent.json";
    uint64_t size = 0;
    if (lookup.stat(backup.data(), size) != FileStatus::Missing || !guard()) return failure("unowned backup");
    const auto original = lookup.stat(target, size);
    if (!guard() || original == FileStatus::Error) return failure("source lookup");
    if (original == FileStatus::Present) {
      if (!Storage.openFileForReadReusing("COMPANION", target, file) || file.fileSize64() != size ||
          !reader.begin(file, scratch, progress, this))
        return failure("source open");
      expected.previousLength = size;
      const bool parsed =
          json.loadMatching(reader, kind, match, context) && reader.contentHash(expected.previousHash) && file.sync();
      const bool closed = close();
      if (!parsed || !closed || !guard()) return failure("source parse/sync/close");
    } else {
      // No previous metadata exists; publish the native empty-store shape.
      empty.begin(kind == RemovalMetadataFile::State ? "{}" : "{\"books\":[]}");
      if (!json.loadMatching(empty, kind, match, context)) return failure("empty source");
    }
    const auto staged = lookup.stat(candidate.data(), size);
    if (!guard() || staged == FileStatus::Error || (staged == FileStatus::Present && size > REMOVAL_METADATA_MAX_BYTES))
      return failure("candidate collision");
    if (original == FileStatus::Present && !json.changed()) {
      if (staged != FileStatus::Missing) return failure("unchanged candidate collision");
      expected.nextLength = expected.previousLength;
      expected.nextHash = expected.previousHash;
    } else {
      expected.nextLength = json.encodedSize();
      if (!guard() || !Storage.openFileForWriteReusing("COMPANION", candidate.data(), file) ||
          !writer.begin(file, scratch, expected.nextLength, progress, this))
        return failure("candidate open");
      const bool written = json.write(writer) && writer.finish() && writer.contentHash();
      if (written) expected.nextHash = *writer.contentHash();
      const bool closed = close();
      if (!written || !closed || !guard() || !verify(candidate.data(), expected.nextLength, expected.nextHash))
        return failure("candidate write/verification");
    }
    // Confirm that publication still starts from the exact parsed generation.
    if (original == FileStatus::Present) {
      if (!verify(target, expected.previousLength, expected.previousHash)) return failure("source changed");
    } else if (lookup.stat(target, size) != FileStatus::Missing || !guard()) {
      return failure("source appeared");
    }
    if (lookup.stat(backup.data(), size) != FileStatus::Missing || !guard() ||
        declarations.persist(expected, journal) != RemovalMetadataStorageResult::Ok || !guard())
      return failure("declaration publication");
    output = expected;
    return true;
  }

 private:
  class Empty final : public EpubReferenceJsonReader {
   public:
    void begin(std::string_view value) {
      bytes = value;
      at = 0;
    }
    int read() override { return at < bytes.size() ? static_cast<unsigned char>(bytes[at++]) : -1; }
    bool healthy() const override { return true; }
    size_t readBytes(char* output, size_t count) override {
      count = std::min(count, bytes.size() - at);
      std::copy_n(bytes.data() + at, count, output);
      at += count;
      return count;
    }

   private:
    std::string_view bytes;
    size_t at = 0;
  } empty;
  ContentRemovalJournal& journal;
  HalRemovalMetadataSnapshotStorage& declarations;
  EpubReferenceJson& json;
  std::span<uint8_t> scratch;
  ContentRemovalRecord checkpoint;
  RemovalMetadataSnapshot expected;
  HalInventoryPathLookup lookup;
  HalFile file;
  HalEpubReferenceJsonReader reader;
  HalEpubReferenceJsonWriter writer;
  std::array<char, 112> candidate{}, backup{};
  const char* target = nullptr;
  Digest actual{};
  bool guard() const { return journal.current() && *journal.current() == checkpoint; }
  static bool progress(void* context) { return static_cast<HalEpubReferenceSnapshot*>(context)->guard(); }
  bool verify(const char* path, uint64_t requiredLength, const Digest& requiredHash) {
    if (!guard() || !close() || !Storage.openFileForReadReusing("COMPANION", path, file)) return failure("verify open");
    uint64_t length = 0;
    const bool valid = !file.isDirectory() && file.fileSize64() == requiredLength &&
                       hashInventoryFile(file, scratch, length, actual, progress, this) && length == requiredLength &&
                       actual == requiredHash && file.sync();
    const bool closed = close();
    return (valid && closed && guard()) || failure("verify bytes/sync/close");
  }
  bool close() { return !file.isOpen() || file.close() || failure("close"); }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "EPUB reference snapshot failed: %s", reason);
    return false;
  }
};
}  // namespace companion
