#pragma once

#include "CompanionDictionaryRemovalCohortPlan.h"
#include "HalRemovalReferencePath.h"
#include "HalRemovalStageClaimStorage.h"

namespace companion {
// One serialized plan owner, outside the stack. Maximum bytes is the caller's
// admitted SD quota. Inventory supplies canonical long paths, not short aliases.
class HalDictionaryRemovalCohortPlanWriter final : private InventoryIndexStorage {
 public:
  explicit HalDictionaryRemovalCohortPlanWriter(uint64_t maximumBytes)
      : maximumBytes(maximumBytes), claims(claimBytes), reader(*this, buffer) {
    mbedtls_sha256_init(&intended);
    mbedtls_sha256_init(&actualRecords);
    mbedtls_sha256_init(&full);
  }
  ~HalDictionaryRemovalCohortPlanWriter() override {
    close();
    mbedtls_sha256_free(&intended);
    mbedtls_sha256_free(&actualRecords);
    mbedtls_sha256_free(&full);
  }
  const char* publishedPath() const { return published ? target.data() : nullptr; }
  const Digest* publishedDigest() const { return published ? &planHash : nullptr; }
  bool setMaximumBytes(uint64_t bytes) {
    if (active || owned || bytes < DICTIONARY_REMOVAL_COHORT_HEADER_SIZE) return failure("quota change");
    maximumBytes = bytes;
    return true;
  }
  bool begin(const ContentRemovalRequest& request, uint64_t revision) {
    if (active || owned || request.manifest.kind != ContentKind::Dictionary) return failure("active owner or kind");
    published = owned = failed = false;
    claim.request = request;
    claim.inventoryRevision = revision;
    if (maximumBytes < DICTIONARY_REMOVAL_COHORT_HEADER_SIZE || !close() ||
        claims.persist(claim) != RemovalStageClaimResult::Ok)
      return failure("claim or quota");
    std::copy_n(claims.planStagePath(), strlen(claims.planStagePath()) + 1, stage.begin());
    if (!inspectStage()) return false;
    owned = true;
    header = {};
    header.request = request;
    header.inventoryRevision = revision;
    payloadCrc = 0xffffffff;
    previous[0] = 0;
    if (mbedtls_sha256_starts(&intended, 0) || !Storage.openFileForWriteReusing("COMPANION", stage.data(), file))
      return failure("stage open");
    if (!writeInitialHeader()) return false;
    active = true;
    return true;
  }
  bool append(const DictionaryRemovalPlan& plan) {
    if (!active || failed || !validDictionaryRemovalPlan(plan) || plan.request != claim.request ||
        (header.count && std::strcmp(previous.data(), plan.installed.base.data()) >= 0))
      return failure("append context or order");
    for (uint64_t index = 0; index < header.count; ++index) {
      if (!file.seek64(DICTIONARY_REMOVAL_COHORT_HEADER_SIZE + index * DICTIONARY_REMOVAL_PLAN_SIZE) ||
          file.read(buffer.data(), buffer.size()) != static_cast<int>(buffer.size()) ||
          !codec.decode(buffer, compared) || compared.request != claim.request)
        return failure("staged record");
      if (removalReferencePathEqual(nullptr, plan.installed.base.data(), compared.installed.base.data()))
        return failure("duplicate canonical path");
      vTaskDelay(1);
    }
    if (header.count >= (maximumBytes - DICTIONARY_REMOVAL_COHORT_HEADER_SIZE) / DICTIONARY_REMOVAL_PLAN_SIZE ||
        DictionaryRemovalPlanCodec::encode(plan, buffer) != buffer.size() ||
        !file.seek64(DICTIONARY_REMOVAL_COHORT_HEADER_SIZE + header.count * DICTIONARY_REMOVAL_PLAN_SIZE) ||
        mbedtls_sha256_update(&intended, buffer.data(), buffer.size()) ||
        file.write(buffer.data(), buffer.size()) != buffer.size())
      return failure("record write or quota");
    payloadCrc = inventoryIndexCrcUpdate(payloadCrc, buffer);
    previous = plan.installed.base;
    ++header.count;
    vTaskDelay(1);
    return true;
  }
  bool finish(uint64_t count) {
    if (!active || failed || count != header.count || !count || claims.load(claim) != RemovalStageClaimResult::Ok)
      return failure("seal context");
    header.payloadCrc = ~payloadCrc;
    if (mbedtls_sha256_finish(&intended, intendedHash.data()) ||
        encodeDictionaryRemovalCohortHeader(header, buffer) != DICTIONARY_REMOVAL_COHORT_HEADER_SIZE ||
        !file.seek64(0) ||
        file.write(buffer.data(), DICTIONARY_REMOVAL_COHORT_HEADER_SIZE) != DICTIONARY_REMOVAL_COHORT_HEADER_SIZE ||
        !file.truncate(DICTIONARY_REMOVAL_COHORT_HEADER_SIZE + (header.count * DICTIONARY_REMOVAL_PLAN_SIZE)) ||
        !file.sync())
      return failure("header seal");
    if (!close() || !verify(stage.data(), planHash, false)) return failure("stage verification");
    static constexpr char PREFIX[] = "/.crosspoint/companion/removal-dictionary-plan-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    static_assert(sizeof(PREFIX) + 64 <= 128);
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
  // Stop IO after card/authentication ownership is lost. A fresh authorized
  // owner must verify the claim before resuming or discarding its stage.
  bool releaseHandles() {
    active = false;
    return close();
  }
  bool discard() {
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
  std::array<uint8_t, DICTIONARY_REMOVAL_PLAN_SIZE> buffer{};
  HalRemovalStageClaimStorage claims;
  HalFile file;
  HalCompanionFileLookup lookup;
  DictionaryRemovalCohortReader reader;
  RemovalStageClaim claim;
  DictionaryRemovalCohortHeader header, checkedHeader;
  ContentRemovalRequest checkedRequest;
  DictionaryRemovalPlanCodec codec;
  DictionaryRemovalPlan compared;
  std::array<char, 128> previous{};
  std::array<char, 112> stage{};
  std::array<char, 128> target{};
  mbedtls_sha256_context intended, actualRecords, full;
  Digest intendedHash{}, actualHash{}, planHash{}, checkedHash{};
  uint32_t payloadCrc = 0xffffffff;
  unsigned reads = 0;
  bool active = false, failed = false, owned = false, published = false;
  bool close() { return !file.isOpen() || file.close() || failure("close"); }
  __attribute__((noinline)) bool writeInitialHeader() {
    // An unsealed header carries ownership but is never a valid reader plan.
    std::fill_n(buffer.begin(), DICTIONARY_REMOVAL_COHORT_HEADER_SIZE, 0);
    static constexpr std::array<uint8_t, 8> PREFIX = {'L', 'D', 'R', 'M', 1, 0, 0, 0};
    std::copy(PREFIX.begin(), PREFIX.end(), buffer.begin());
    if (!encodeContentRemovalRequest(claim.request, std::span(buffer).subspan(8, CONTENT_REMOVAL_REQUEST_SIZE)))
      return failure("stage request");
    inventory_detail::write(buffer, 123, claim.inventoryRevision, 8);
    inventory_detail::write(buffer, 143, inventoryIndexCrc(std::span(buffer).first(143)), 4);
    if (file.isDirectory() ||
        file.write(buffer.data(), DICTIONARY_REMOVAL_COHORT_HEADER_SIZE) != DICTIONARY_REMOVAL_COHORT_HEADER_SIZE)
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
    if (length >= DICTIONARY_REMOVAL_COHORT_HEADER_SIZE) {
      if (file.read(buffer.data(), DICTIONARY_REMOVAL_COHORT_HEADER_SIZE) != DICTIONARY_REMOVAL_COHORT_HEADER_SIZE)
        return failure("stage header read");
      if (inventory_detail::read(buffer, 143, 4) == inventoryIndexCrc(std::span(buffer).first(143))) {
        static constexpr std::array<uint8_t, 8> PREFIX = {'L', 'D', 'R', 'M', 1, 0, 0, 0};
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
        file.fileSize64() != DICTIONARY_REMOVAL_COHORT_HEADER_SIZE + (header.count * DICTIONARY_REMOVAL_PLAN_SIZE) ||
        !reader.open(claim.request) || !reader.current() || *reader.current() != header || !file.seek64(0) ||
        file.read(buffer.data(), DICTIONARY_REMOVAL_COHORT_HEADER_SIZE) != DICTIONARY_REMOVAL_COHORT_HEADER_SIZE ||
        !decodeDictionaryRemovalCohortHeader(std::span(buffer).first(DICTIONARY_REMOVAL_COHORT_HEADER_SIZE),
                                             checkedHeader) ||
        checkedHeader != header || mbedtls_sha256_starts(&full, 0) || mbedtls_sha256_starts(&actualRecords, 0) ||
        mbedtls_sha256_update(&full, buffer.data(), DICTIONARY_REMOVAL_COHORT_HEADER_SIZE))
      return failure("plan verification context");
    uint64_t remaining = (header.count * DICTIONARY_REMOVAL_PLAN_SIZE);
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
        file.fileSize64() != DICTIONARY_REMOVAL_COHORT_HEADER_SIZE + (header.count * DICTIONARY_REMOVAL_PLAN_SIZE) ||
        !file.sync())
      return failure("plan hash or sync");
    if (!close()) return false;
    output = actualHash;
    return true;
  }
  bool failure(const char* operation) {
    failed = true;
    LOG_ERR("COMPANION", "Dictionary cohort removal plan %s failed", operation);
    return false;
  }
};
}  // namespace companion
