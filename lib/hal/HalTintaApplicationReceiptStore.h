#pragma once

#include "CompanionTintaApplicationReceipt.h"
#include "HalCompanionFileLookup.h"

namespace companion {
enum class TintaApplicationReceiptResult { Ok, Missing, Invalid, Conflict, Corrupt, IoError };
// Borrowed companion-child paths must outlive this off-stack owner. The caller
// excludes mutation writers and verifies authority before acknowledging application.
class HalTintaApplicationReceiptStore final {
 public:
  HalTintaApplicationReceiptStore(const char* path, const char* stage) : path(path), stage(stage) {}
  ~HalTintaApplicationReceiptStore() {
    if (file.isOpen() && !file.close()) error("destructor close");
  }
  TintaApplicationReceiptResult load(TintaApplicationReceipt& output) {
    const auto result = read(path);
    if (result == TintaApplicationReceiptResult::Ok) output = decoded;
    return result;
  }
  TintaApplicationReceiptResult persist(const TintaApplicationReceipt& value) {
    if (!path || !stage || strcmp(path, stage) == 0 ||
        encodeTintaApplicationReceipt(value, expected) != expected.size())
      return TintaApplicationReceiptResult::Invalid;
    auto result = read(path);
    if (result == TintaApplicationReceiptResult::Ok)
      return bytes == expected ? result : TintaApplicationReceiptResult::Conflict;
    if (result != TintaApplicationReceiptResult::Missing) return result;
    result = read(stage);
    if (result == TintaApplicationReceiptResult::Ok && bytes != expected)
      return TintaApplicationReceiptResult::Conflict;
    if (result != TintaApplicationReceiptResult::Missing && result != TintaApplicationReceiptResult::Ok) return result;
    if (result == TintaApplicationReceiptResult::Missing) {
      if (!Storage.openFileForWriteReusing("COMPANION", stage, file)) return error("stage open");
      const bool written = !file.isDirectory() && file.seek64(0) &&
                           file.write(expected.data(), expected.size()) == expected.size() &&
                           file.truncate(expected.size()) && file.sync();
      const bool closed = file.close();
      if (!written || !closed) return error("stage write/sync/close");
    }
    if (!Storage.openFileForReadReusing("COMPANION", stage, file)) return error("stage durability open");
    const bool synced = !file.isDirectory() && file.sync();
    const bool closed = file.close();
    if (!synced || !closed) return error("stage durability sync/close");
    result = read(stage);
    if (result != TintaApplicationReceiptResult::Ok) return result;
    if (bytes != expected) return TintaApplicationReceiptResult::Conflict;
    result = read(path);
    if (result == TintaApplicationReceiptResult::Ok)
      return bytes == expected ? result : TintaApplicationReceiptResult::Conflict;
    if (result != TintaApplicationReceiptResult::Missing) return result;
    // A failed rename can have published the record; readback resolves that reply.
    const bool renamed = Storage.rename(stage, path);
    result = read(path);
    if (result == TintaApplicationReceiptResult::Ok)
      return bytes == expected ? result : TintaApplicationReceiptResult::Conflict;
    return renamed ? result : error("publication rename");
  }

 private:
  TintaApplicationReceiptResult read(const char* source) {
    if (!source || (file.isOpen() && !file.close()) || !Storage.ensureDirectoryExists(TRANSFER_DIRECTORY))
      return error("read preparation");
    const auto presence = lookup.inspect(source);
    if (presence == CompanionFilePresence::Missing) return TintaApplicationReceiptResult::Missing;
    if (presence == CompanionFilePresence::Error || !Storage.openFileForReadReusing("COMPANION", source, file))
      return error("lookup/open");
    const bool shape = !file.isDirectory() && file.fileSize64() == bytes.size();
    const bool loaded = shape && file.read(bytes.data(), bytes.size()) == static_cast<int>(bytes.size()) &&
                        file.fileSize64() == bytes.size();
    const bool closed = file.close();
    if (!closed || (shape && !loaded)) return error("read/close");
    if (!loaded || !decodeTintaApplicationReceipt(bytes, decoded)) return TintaApplicationReceiptResult::Corrupt;
    return TintaApplicationReceiptResult::Ok;
  }
  static TintaApplicationReceiptResult error(const char* reason) {
    LOG_ERR("COMPANION", "Tinta application receipt failed: %s", reason);
    return TintaApplicationReceiptResult::IoError;
  }
  const char* path;
  const char* stage;
  HalCompanionFileLookup lookup;
  HalFile file;
  std::array<uint8_t, TINTA_APPLICATION_RECEIPT_SIZE> bytes{}, expected{};
  TintaApplicationReceipt decoded;
};
}  // namespace companion
