#pragma once

#include <ArduinoJson.h>

#include <array>
#include <cstddef>
#include <string_view>

#include "CompanionRemovalMetadataSnapshot.h"

namespace companion {
class EpubReferenceJsonReader {
 public:
  virtual ~EpubReferenceJsonReader() = default;
  virtual int read() = 0;
  virtual bool healthy() const = 0;
  virtual size_t readBytes(char* bytes, size_t length) = 0;
};
class EpubReferenceJsonWriter {
 public:
  virtual ~EpubReferenceJsonWriter() = default;
  virtual size_t write(uint8_t byte) = 0;
  virtual size_t write(const uint8_t* bytes, size_t length) = 0;
};
// This workspace must be session-owned, allocated with makeUniqueNoThrow by the
// caller. Its fixed arena bounds parser memory and is reused between snapshots.
class EpubReferenceJson final {
 public:
  static constexpr size_t ARENA_BYTES = 16384;
  using PathMatch = bool (*)(void*, std::string_view, bool& matched);
  using PathEqual = bool (*)(void*, std::string_view, std::string_view);
  EpubReferenceJson();
  EpubReferenceJson(const EpubReferenceJson&) = delete;
  EpubReferenceJson& operator=(const EpubReferenceJson&) = delete;
  // Readers must report I/O failures separately from EOF to their HAL owner.
  // PathEqual supplies the native filesystem's component comparison rules.
  bool load(EpubReferenceJsonReader& source, RemovalMetadataFile kind, std::string_view removedPath, PathEqual equal,
            void* context);
  // A failed matcher invalidates the entire candidate, including prior matches.
  bool loadMatching(EpubReferenceJsonReader& source, RemovalMetadataFile kind, PathMatch match, void* context);
  bool write(EpubReferenceJsonWriter& destination) const;
  bool changed() const { return available && modified; }
  size_t encodedSize() const;

 private:
  class Arena final : public ArduinoJson::Allocator {
   public:
    void* allocate(size_t bytes) override;
    void deallocate(void*) override {}
    void* reallocate(void* pointer, size_t bytes) override;
    void reset() { used = 0; }

   private:
    alignas(std::max_align_t) std::array<uint8_t, ARENA_BYTES> storage{};
    size_t used = 0;
  } arena;
  JsonDocument document;
  bool available = false, modified = false;
  static bool failure(const char* reason);
};
}  // namespace companion
