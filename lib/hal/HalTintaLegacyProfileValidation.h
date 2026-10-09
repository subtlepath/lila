#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "../Serialization/BinaryRecordBytes.h"
#include "core/profile/Profile.h"

namespace companion {
// Borrows a hash-verified immutable reviewed file. Callers exclude writers and
// decide whether an Upgraded profile is acceptable; this never saves defaults.
inline bool inspectTintaLegacyProfile(HalFile& file, tinta::core::Profile& output,
                                      tinta::core::Profile::LoadResult& status, bool (*permitted)(void*) = nullptr,
                                      void* context = nullptr) {
  const auto failure = []([[maybe_unused]] const char* reason) {
    LOG_ERR("COMPANION", "Legacy profile inspection failed: %s", reason);
    return false;
  };
  if (!file.isOpen() || file.isDirectory() || (permitted && !permitted(context))) return failure("arguments");
  const auto length = file.fileSize64();
  uint8_t header[8];
  if (length < 12 || length > 65547 || !file.seek64(0) || file.read(header, sizeof header) != sizeof header ||
      length != 12u + binary_record::getU16(header + 6))
    return failure("extent or header");
  class ReadOnlyStore final : public tinta::core::StateStore {
   public:
    ReadOnlyStore(HalFile& file, uint32_t length, bool (*permitted)(void*), void* context)
        : file(file), length(length), permitted(permitted), context(context) {}
    bool available() const override {
      if (failed || !file.isOpen() || file.fileSize64() != length || (permitted && !permitted(context))) {
        failed = true;
        return false;
      }
      return true;
    }
    int32_t size(const char* name) override { return validName(name) && available() ? length : -1; }
    int32_t read(const char* name, uint32_t offset, void* buffer, uint32_t count) override {
      if (!validName(name) || !available() || offset > length || count > length - offset || !file.seek64(offset) ||
          file.read(buffer, count) != static_cast<int>(count) || !available()) {
        failed = true;
        return -1;
      }
      return count;
    }
    bool write(const char*, uint32_t, const void*, uint32_t) override { return mutation(); }
    bool append(const char*, const void*, uint32_t) override { return mutation(); }
    bool replace(const char*, const void*, uint32_t) override { return mutation(); }
    bool remove(const char*) override { return mutation(); }

   private:
    HalFile& file;
    uint32_t length;
    bool (*permitted)(void*);
    void* context;
    mutable bool failed = false;
    bool validName(const char* name) const { return name && std::strcmp(name, tinta::core::Profile::kFile) == 0; }
    bool mutation() {
      failed = true;
      LOG_ERR("COMPANION", "Mutation refused during legacy profile inspection");
      return false;
    }
  } store(file, length, permitted, context);
  tinta::core::Profile decoded;
  const auto result = decoded.load(store);
  if (!store.available() ||
      (result != tinta::core::Profile::LoadResult::Loaded && result != tinta::core::Profile::LoadResult::Upgraded))
    return failure("decode");
  output = decoded;
  status = result;
  return true;
}
}  // namespace companion
