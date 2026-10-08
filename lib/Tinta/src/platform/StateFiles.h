#pragma once

// core::StateStore on the SD card, under /tinta/ (PLAN.md 8.3), through lila's
// HalStorage. The same folder as the standalone Tinta firmware, so a card
// moves between the two with its progress.
//
// - The last file used stays open, so a run of reads and appends to one file
//   costs one open.
// - Every write is flushed: it is on the card when the call returns.
// - replace() writes <name>.tmp, flushes and closes it, removes <name> and
//   renames the temporary file. SdFat's rename does not overwrite, hence the
//   remove; a power cut between the two leaves only <name>.tmp, which the next
//   access promotes.
// - Without a card, available() is false and the app runs as a guest.
// - An I/O error is never passed off as a missing file: when a call fails,
//   the /tinta directory is probed, and if the card no longer answers the
//   store turns unavailable until the app is opened again, so a caller that
//   saw "no file" cannot then overwrite the real one.

#include <HalStorage.h>

#include <span>

#include "core/StateStore.h"

namespace tinta::platform {

class StateFiles final : public core::StateStore {
 public:
  void begin();
  bool beginCourse(std::span<const uint8_t, 16> course);

  bool available() const override { return mounted_ && !failed_; }
  // True when storage or pending-file recovery prevents safe use.
  bool failed() const { return failed_; }

  int32_t size(const char* name) override;
  int32_t read(const char* name, uint32_t offset, void* buffer, uint32_t len) override;
  bool write(const char* name, uint32_t offset, const void* data, uint32_t len) override;
  bool append(const char* name, const void* data, uint32_t len) override;
  bool replace(const char* name, const void* data, uint32_t len) override;
  bool remove(const char* name) override;

  // Closes the cached file; the next call reopens what it needs.
  void release() { closeCached(); }

 private:
  static constexpr size_t kPathCap = 76;
  bool pathFor(const char* name, char (&out)[kPathCap], const char* suffix = "") const;
  void beginRoot();
  enum class Mode : uint8_t { None, Read, Write };

  // Opens `name` (cached) for reading or writing; false if it is missing or
  // the open failed (check failed_ to tell them apart).
  bool openCached(const char* name, Mode mode);
  void closeCached();
  // Called after any failed operation: decides whether the card is gone.
  void checkCard();
  // Promotes a leftover <name>.tmp from an interrupted replace().
  bool recover(const char* name);

  bool mounted_ = false;
  bool failed_ = false;
  HalFile file_;
  Mode mode_ = Mode::None;
  char cachedName_[24] = {};
  char root_[48] = "/tinta";
};

}  // namespace tinta::platform
