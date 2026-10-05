#pragma once

#include <cstdint>

namespace tinta::core {

// Learner state storage: a flat set of small named files (PLAN.md section 8.3).
// On the device this is /tinta/ on the SD card; host tests use a directory or
// memory. Calls are stateless — no handles — so an implementation may cache
// the last file it opened. Every successful write is durable when it returns.
class StateStore {
 public:
  virtual ~StateStore() = default;

  // False when there is nowhere to save (no SD card): the app runs as a guest.
  virtual bool available() const = 0;

  // Size in bytes, or -1 if the file does not exist.
  virtual int32_t size(const char* name) = 0;

  // Reads up to `len` bytes at `offset`. Returns bytes read, or -1 on error.
  virtual int32_t read(const char* name, uint32_t offset, void* buffer, uint32_t len) = 0;

  // Writes `len` bytes at `offset`, creating or extending the file.
  virtual bool write(const char* name, uint32_t offset, const void* data, uint32_t len) = 0;

  // Appends `len` bytes, creating the file if needed.
  virtual bool append(const char* name, const void* data, uint32_t len) = 0;

  // Replaces the whole file atomically (write a temporary file, then rename).
  virtual bool replace(const char* name, const void* data, uint32_t len) = 0;

  virtual bool remove(const char* name) = 0;
};

}  // namespace tinta::core
