#pragma once

#include "CompanionBookmarkSaveSession.h"

namespace companion {
// Checked off-stack owner. Caller freezes edition/card and serializes journal
// writers. Retain only one fixed page while the reader asks for a choice.
class NativeBookmarkChoicePage final {
 public:
  static constexpr uint32_t CAPACITY = 4;
  TintaJournalResult load(const Digest& verifiedEdition, const Identity& identity, uint32_t spineCount,
                          uint32_t start = 0) {
    ready = false;
    recovery = false;
    size = total = 0;
    offset = start;
    edition = verifiedEdition;
    bookmark = identity;
    spines = spineCount;
    if (!tinta_body_detail::nonzero(edition) || !tinta_body_detail::nonzero(bookmark) || !spines || spines > 65536)
      return TintaJournalResult::Invalid;
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit || !audit->run(&frontier)) return failure("choices audit");
    const auto result = audit->visitBookmarkHeads(edition, bookmark, collect, this);
    if (result != TintaJournalResult::Ok) return result;
    if (!audit->run(&rechecked)) return failure("choices recheck");
    if (frontier != rechecked) return TintaJournalResult::Conflict;
    if (!size) return TintaJournalResult::Unavailable;
    ready = true;
    return TintaJournalResult::Ok;
  }
  uint32_t count() const { return ready ? size : 0; }
  void invalidate() { ready = false; }
  bool matches(const Digest& verifiedEdition, const Identity& identity) const {
    return ready && edition == verifiedEdition && bookmark == identity;
  }
  uint32_t totalCount() const { return ready ? total : 0; }
  bool hasNext() const { return ready && total - offset > size; }
  bool choice(uint32_t at, BookmarkBodyView& output) const {
    return ready && at < size && decodeBookmarkBody(std::span(bodies[at]).first(lengths[at]), output);
  }
  const EventIdentity* source(uint32_t at) const { return ready && at < size ? &sources[at] : nullptr; }
  TintaJournalResult resolve(uint32_t at, IdentityStorage& identities) {
    BookmarkBodyView selected;
    if (!choice(at, selected)) return TintaJournalResult::Invalid;
    // Canonical bodies and writer ownership exceed the local stack budget.
    auto save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
    if (!save) return failure("choice save allocation");
    const auto result = save->resolve(selected, edition, identities, frontier);
    recovery = save->requiresRecovery();
    ready = false;
    return result;
  }
  bool requiresRecovery() const { return recovery; }

 private:
  static bool collect(void* context, const EventIdentity& event, std::span<const uint8_t> body) {
    auto& self = *static_cast<NativeBookmarkChoicePage*>(context);
    BookmarkBodyView decoded;
    if (!decodeBookmarkBody(body, decoded) || (!decoded.deleted && decoded.anchor.spine >= self.spines)) return false;
    const auto ordinal = self.total++;
    if (ordinal < self.offset || self.size == CAPACITY) return true;
    const auto at = self.size++;
    self.lengths[at] = body.size();
    self.sources[at] = event;
    std::copy(body.begin(), body.end(), self.bodies[at].begin());
    return true;
  }
  static TintaJournalResult failure(const char* operation) {
    LOG_ERR("COMPANION", "Bookmark choice failed: %s", operation);
    return TintaJournalResult::IoError;
  }
  std::array<std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE>, CAPACITY> bodies{};
  std::array<size_t, CAPACITY> lengths{};
  std::array<EventIdentity, CAPACITY> sources{};
  Digest edition{}, frontier{}, rechecked{};
  Identity bookmark{};
  uint32_t spines = 0, offset = 0, size = 0, total = 0;
  bool ready = false, recovery = false;
};
static_assert(sizeof(NativeBookmarkChoicePage) <= 3072);
}  // namespace companion
