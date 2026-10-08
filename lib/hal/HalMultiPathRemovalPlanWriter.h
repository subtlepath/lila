#pragma once

#include "CompanionRemovalPathCollection.h"
#include "HalRemovalReferencePath.h"
#include "HalRemovalStageClaimStorage.h"

namespace companion {
// One serialized plan owner, outside the stack. Maximum bytes is the caller's
// admitted SD quota. Inventory supplies canonical long paths, not short aliases.
class HalMultiPathRemovalPlanWriter final : public RemovalPathSink, private InventoryIndexStorage {
 public:
  explicit HalMultiPathRemovalPlanWriter(uint64_t maximumBytes)
      : maximumBytes(maximumBytes), claims(claimBytes), reader(*this, buffer) {
    mbedtls_sha256_init(&intended);
    mbedtls_sha256_init(&actualRecords);
    mbedtls_sha256_init(&full);
  }
  ~HalMultiPathRemovalPlanWriter() override {
    close();
    mbedtls_sha256_free(&intended);
    mbedtls_sha256_free(&actualRecords);
    mbedtls_sha256_free(&full);
  }
  const char* publishedPath() const { return published ? target.data() : nullptr; }
  const Digest* publishedDigest() const { return published ? &planHash : nullptr; }
  bool setMaximumBytes(uint64_t bytes) {
    if (active || owned || bytes < MULTI_PATH_REMOVAL_HEADER_SIZE) return failure("quota change");
    maximumBytes = bytes;
    return true;
  }
  bool begin(const ContentRemovalRequest& request, uint64_t revision) override {
    if (active || owned) return failure("active owner");
    published = owned = failed = false;
    claim.request = request;
    claim.inventoryRevision = revision;
    if (maximumBytes < MULTI_PATH_REMOVAL_HEADER_SIZE || !close() ||
        claims.persist(claim) != RemovalStageClaimResult::Ok)
      return failure("claim or quota");
    std::copy_n(claims.planStagePath(), strlen(claims.planStagePath()) + 1, stage.begin());
    if (!inspectStage()) return false;
    owned = true;
    header = {};
    header.request = request;
    header.inventoryRevision = revision;
    payloadCrc = 0xffffffff;
    if (mbedtls_sha256_starts(&intended, 0) || !Storage.openFileForWriteReusing("COMPANION", stage.data(), file))
      return failure("stage open");
    if (!writeInitialHeader()) return false;
    active = true;
    return true;
  }
  bool append(std::string_view path) override {
    if (!active || failed || !validSingleFileRemovalPlan({claim.request, path})) return failure("append context");
    uint64_t at = MULTI_PATH_REMOVAL_HEADER_SIZE;
    for (uint64_t index = 0; index < header.count; ++index) {
      if (!file.seek64(at) || file.read(buffer.data(), 2) != 2) return failure("duplicate lookup");
      const auto length = inventory_detail::read(buffer, 0, 2);
      if (length < 2 || length > INVENTORY_PATH_LIMIT) return failure("staged record length");
      const size_t size = static_cast<size_t>(length) + 6;
      if (!file.seek64(at) || file.read(buffer.data(), size) != static_cast<int>(size) ||
          !decodeMultiPathRemovalRecord(claim.request, std::span(buffer).first(size), compared))
        return failure("staged record");
      if (removalReferencePathEqual(nullptr, path, compared)) return failure("duplicate canonical path");
      at += size;
      if ((index & 31) == 31) vTaskDelay(1);
    }
    const auto size = encodeMultiPathRemovalRecord(claim.request, path, buffer);
    if (!size || header.recordsLength > maximumBytes - MULTI_PATH_REMOVAL_HEADER_SIZE ||
        size > maximumBytes - MULTI_PATH_REMOVAL_HEADER_SIZE - header.recordsLength ||
        !file.seek64(MULTI_PATH_REMOVAL_HEADER_SIZE + header.recordsLength) ||
        mbedtls_sha256_update(&intended, buffer.data(), size) || file.write(buffer.data(), size) != size)
      return failure("record write or quota");
    payloadCrc = inventoryIndexCrcUpdate(payloadCrc, std::span(buffer).first(size - 4));
    header.recordsLength += size;
    ++header.count;
    if ((header.count & 31) == 0) vTaskDelay(1);
    return true;
  }
  bool finish(uint64_t count) override {
    if (!active || failed || count != header.count || !count || claims.load(claim) != RemovalStageClaimResult::Ok)
      return failure("seal context");
    header.payloadCrc = ~payloadCrc;
    if (mbedtls_sha256_finish(&intended, intendedHash.data()) ||
        encodeMultiPathRemovalPlanHeader(header, buffer) != MULTI_PATH_REMOVAL_HEADER_SIZE || !file.seek64(0) ||
        file.write(buffer.data(), MULTI_PATH_REMOVAL_HEADER_SIZE) != MULTI_PATH_REMOVAL_HEADER_SIZE ||
        !file.truncate(MULTI_PATH_REMOVAL_HEADER_SIZE + header.recordsLength) || !file.sync())
      return failure("header seal");
    if (!close() || !verify(stage.data(), planHash, false)) return failure("stage verification");
    static constexpr char PREFIX[] = "/.crosspoint/companion/removal-plan-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    static_assert(sizeof(PREFIX) + 64 <= 112);
    size_t at = sizeof(PREFIX) - 1;
    std::copy_n(PREFIX, at, target.begin());
    for (uint8_t byte : planHash) {
      target[at++] = HEX_DIGITS[byte >> 4];
      target[at++] = HEX_DIGITS[byte & 15];
    }
    target[at] = 0;
    const auto presence = lookup.inspect(target.data());
    if (presence == CompanionFilePresence::Error) return failure("target lookup");
    if (claims.load(claim) != RemovalStageClaimResult::Ok) return failure("publication ownership");
    if (presence == CompanionFilePresence::Present) {
      if (!verify(target.data(), checkedHash, true) || !verify(stage.data(), checkedHash, true) ||
          !Storage.remove(stage.data()) || lookup.inspect(stage.data()) != CompanionFilePresence::Missing)
        return failure("matching publication or stage cleanup");
    } else if (!Storage.rename(stage.data(), target.data())) {
      return failure("publication rename");
    }
    if (!verify(target.data(), checkedHash, true)) return failure("published verification");
    published = true;
    owned = active = false;
    return true;
  }
  bool discard() override {
    active = false;
    if (!close()) return false;
    if (!owned) return true;
    if (claims.load(claim) != RemovalStageClaimResult::Ok || !inspectStage()) return failure("discard ownership");
    const auto presence = lookup.inspect(stage.data());
    if (presence == CompanionFilePresence::Error) return failure("discard lookup");
    if (presence == CompanionFilePresence::Present &&
        (!Storage.remove(stage.data()) || lookup.inspect(stage.data()) != CompanionFilePresence::Missing))
      return failure("discard removal");
    owned = false;
    return true;
  }

 private:
  uint64_t maximumBytes;
  std::array<uint8_t, REMOVAL_STAGE_CLAIM_SIZE> claimBytes{};
  std::array<uint8_t, MULTI_PATH_REMOVAL_RECORD_MAX> buffer{};
  HalRemovalStageClaimStorage claims;
  HalFile file;
  HalCompanionFileLookup lookup;
  MultiPathRemovalPlanReader reader;
  RemovalStageClaim claim;
  MultiPathRemovalPlanHeader header, checkedHeader;
  ContentRemovalRequest checkedRequest;
  std::string_view compared;
  std::array<char, 112> stage{}, target{};
  mbedtls_sha256_context intended, actualRecords, full;
  Digest intendedHash{}, actualHash{}, planHash{}, checkedHash{};
  uint32_t payloadCrc = 0xffffffff;
  unsigned reads = 0;
  bool active = false, failed = false, owned = false, published = false;
  bool close() { return !file.isOpen() || file.close() || failure("close"); }
  __attribute__((noinline)) bool writeInitialHeader() {
    // An unsealed header carries ownership but is never a valid reader plan.
    std::fill_n(buffer.begin(), MULTI_PATH_REMOVAL_HEADER_SIZE, 0);
    static constexpr std::array<uint8_t, 8> PREFIX = {'L', 'R', 'M', 'P', 1, 0, 0, 0};
    std::copy(PREFIX.begin(), PREFIX.end(), buffer.begin());
    if (!encodeContentRemovalRequest(claim.request, std::span(buffer).subspan(8, CONTENT_REMOVAL_REQUEST_SIZE)))
      return failure("stage request");
    inventory_detail::write(buffer, 123, claim.inventoryRevision, 8);
    inventory_detail::write(buffer, 151, inventoryIndexCrc(std::span(buffer).first(151)), 4);
    if (file.isDirectory() ||
        file.write(buffer.data(), MULTI_PATH_REMOVAL_HEADER_SIZE) != MULTI_PATH_REMOVAL_HEADER_SIZE)
      return failure("initial header");
    return true;
  }
  bool size(uint64_t& output) override {
    if (!file.isOpen() || file.isDirectory()) return false;
    output = file.fileSize64();
    return true;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    if (!file.isOpen() || !file.seek64(offset) ||
        file.read(output.data(), output.size()) != static_cast<int>(output.size()))
      return false;
    if (++reads == 32) {
      reads = 0;
      vTaskDelay(1);
    }
    return true;
  }
  bool inspectStage() {
    if (!close()) return false;
    const auto presence = lookup.inspect(stage.data());
    if (presence == CompanionFilePresence::Missing) return true;
    if (presence != CompanionFilePresence::Present ||
        !Storage.openFileForReadReusing("COMPANION", stage.data(), file) || file.isDirectory())
      return failure("stage collision");
    const auto length = file.fileSize64();
    if (length > maximumBytes) return failure("oversized stage");
    if (length >= MULTI_PATH_REMOVAL_HEADER_SIZE) {
      if (file.read(buffer.data(), MULTI_PATH_REMOVAL_HEADER_SIZE) != MULTI_PATH_REMOVAL_HEADER_SIZE)
        return failure("stage header read");
      if (inventory_detail::read(buffer, 151, 4) == inventoryIndexCrc(std::span(buffer).first(151))) {
        static constexpr std::array<uint8_t, 8> PREFIX = {'L', 'R', 'M', 'P', 1, 0, 0, 0};
        if (!std::equal(PREFIX.begin(), PREFIX.end(), buffer.begin()) ||
            !multi_path_removal_detail::decodeRequest(std::span(buffer).subspan(8, CONTENT_REMOVAL_REQUEST_SIZE),
                                                      checkedRequest) ||
            checkedRequest != claim.request || inventory_detail::read(buffer, 123, 8) != claim.inventoryRevision)
          return failure("foreign stage header");
      }
    }
    return close();
  }
  bool verify(const char* path, Digest& output, bool comparePlan) {
    if (!close() || !Storage.openFileForReadReusing("COMPANION", path, file) || file.isDirectory() ||
        file.fileSize64() != MULTI_PATH_REMOVAL_HEADER_SIZE + header.recordsLength || !reader.open(claim.request) ||
        !reader.current() || *reader.current() != header || !file.seek64(0) ||
        file.read(buffer.data(), MULTI_PATH_REMOVAL_HEADER_SIZE) != MULTI_PATH_REMOVAL_HEADER_SIZE ||
        !decodeMultiPathRemovalPlanHeader(std::span(buffer).first(MULTI_PATH_REMOVAL_HEADER_SIZE), checkedHeader) ||
        checkedHeader != header || mbedtls_sha256_starts(&full, 0) || mbedtls_sha256_starts(&actualRecords, 0) ||
        mbedtls_sha256_update(&full, buffer.data(), MULTI_PATH_REMOVAL_HEADER_SIZE))
      return failure("plan verification context");
    uint64_t remaining = header.recordsLength;
    while (remaining) {
      const size_t count = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
      if (file.read(buffer.data(), count) != static_cast<int>(count) ||
          mbedtls_sha256_update(&full, buffer.data(), count) ||
          mbedtls_sha256_update(&actualRecords, buffer.data(), count))
        return failure("plan hash read");
      remaining -= count;
      vTaskDelay(1);
    }
    if (mbedtls_sha256_finish(&actualRecords, actualHash.data()) || actualHash != intendedHash ||
        mbedtls_sha256_finish(&full, actualHash.data()) || (comparePlan && actualHash != planHash) ||
        file.fileSize64() != MULTI_PATH_REMOVAL_HEADER_SIZE + header.recordsLength || !file.sync())
      return failure("plan hash or sync");
    if (!close()) return false;
    output = actualHash;
    return true;
  }
  bool failure(const char* operation) {
    failed = true;
    LOG_ERR("COMPANION", "Multi-path removal plan %s failed", operation);
    return false;
  }
};
}  // namespace companion
