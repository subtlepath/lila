#pragma once

#include <Logging.h>
#include <mbedtls/sha256.h>

#include "CompanionDictionaryBundleBuilder.h"

namespace companion {
// Hash the exact sequential member bytes emitted into the canonical archive.
// One retained SHA context is reused; proof arrays are fixed owner state.
class HalDictionaryRemovalMemberProofSource final : public DictionaryBundleSource {
 public:
  explicit HalDictionaryRemovalMemberProofSource(DictionaryBundleSource& source) : source(source) {
    mbedtls_sha256_init(&hash);
  }
  ~HalDictionaryRemovalMemberProofSource() override { mbedtls_sha256_free(&hash); }
  HalDictionaryRemovalMemberProofSource(const HalDictionaryRemovalMemberProofSource&) = delete;
  HalDictionaryRemovalMemberProofSource& operator=(const HalDictionaryRemovalMemberProofSource&) = delete;
  void reset(bool synonyms) {
    count = synonyms ? 4 : 3;
    lengths = {};
    consumed = {};
    hashes = {};
    known = sealed = 0;
    active = failed = false;
  }
  bool size(unsigned member, uint64_t& output) override {
    uint64_t length = 0;
    if (failed || member >= count || !source.size(member, length) ||
        ((known & (1u << member)) && lengths[member] != length))
      return fail("member size");
    lengths[member] = length;
    known |= 1u << member;
    output = length;
    return true;
  }
  bool read(unsigned member, uint64_t offset, std::span<uint8_t> bytes) override {
    if (failed || member >= count || !(known & (1u << member)) || (sealed & (1u << member)) || bytes.empty() ||
        offset != consumed[member] || offset > lengths[member] || bytes.size() > lengths[member] - offset ||
        !source.read(member, offset, bytes))
      return fail("member read order");
    if (!offset) {
      if (active || mbedtls_sha256_starts(&hash, 0)) return fail("member SHA start");
      active = true;
      activeMember = member;
    }
    if (!active || activeMember != member || mbedtls_sha256_update(&hash, bytes.data(), bytes.size()))
      return fail("member SHA update");
    consumed[member] += bytes.size();
    if (consumed[member] == lengths[member]) {
      if (mbedtls_sha256_finish(&hash, hashes[member].data())) return fail("member SHA finish");
      sealed |= 1u << member;
      active = false;
    }
    return true;
  }
  bool close() override { return source.close() || fail("member close"); }
  bool finish() {
    if (failed || active || known != (count == 4 ? 15 : 7)) return fail("incomplete proof");
    for (unsigned member = 0; member < count; ++member) {
      if (!lengths[member] && !(sealed & (1u << member))) {
        if (mbedtls_sha256_starts(&hash, 0) || mbedtls_sha256_finish(&hash, hashes[member].data()))
          return fail("empty member SHA");
        sealed |= 1u << member;
      }
    }
    return sealed == (count == 4 ? 15 : 7) || fail("unsealed proof");
  }
  const std::array<uint64_t, 4>& memberLengths() const { return lengths; }
  const std::array<Digest, 4>& memberHashes() const { return hashes; }

 private:
  DictionaryBundleSource& source;
  mbedtls_sha256_context hash;
  std::array<uint64_t, 4> lengths{}, consumed{};
  std::array<Digest, 4> hashes{};
  unsigned count = 0, activeMember = 0;
  uint8_t known = 0, sealed = 0;
  bool active = false, failed = false;
  bool fail(const char* reason) {
    failed = true;
    LOG_ERR("COMPANION", "Dictionary removal proof %s failed", reason);
    return false;
  }
};
}  // namespace companion
