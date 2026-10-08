#pragma once

#include "CompanionFrame.h"
#include "CompanionTintaJournal.h"

namespace companion {
inline constexpr size_t JOURNAL_EXPORT_HEADER_SIZE = 48;
static_assert(JOURNAL_EXPORT_HEADER_SIZE + MAX_RECORD_SIZE + TintaJournal::MAX_BODY_SIZE <= MAX_CONTROL_PAYLOAD);

// Caller audits the frontier and excludes writers for this session. Buffers must not overlap.
class JournalExportPage {
 public:
  explicit JournalExportPage(TintaJournal& journal) : journal(journal) {}
  bool begin(const Digest& auditedFrontier) {
    ready = false;
    if (!journal.available() || !tinta_body_detail::nonzero(auditedFrontier)) return false;
    frontier = auditedFrontier;
    count = journal.count();
    ready = true;
    return true;
  }
  void reset() { ready = false; }
  // Zero frontier/count/index starts an export; later requests echo frontier/count/next.
  // Zero return preserves output. Repeated cursors return identical pages.
  size_t page(std::span<const uint8_t> request, std::span<uint8_t> output) {
    if (!ready || !journal.available() || journal.count() != count) {
      ready = false;
      return 0;
    }
    if (request.size() != JOURNAL_EXPORT_REQUEST_SIZE || output.size() < MAX_CONTROL_PAYLOAD) return 0;
    const uint32_t requestedCount = tinta_body_detail::read(request, 32, 4);
    const uint32_t record = tinta_body_detail::read(request, 36, 4);
    const bool start = requestedCount == 0 && record == 0 &&
                       std::all_of(request.begin(), request.begin() + 32, [](uint8_t byte) { return byte == 0; });
    if (!start &&
        (requestedCount != count || record > count || !std::equal(frontier.begin(), frontier.end(), request.begin())))
      return 0;
    if (record < count && journal.read(record) != TintaJournalResult::Ok) {
      ready = false;
      return 0;
    }
    const size_t envelope =
        record < count ? encodeRecord(journal.event(), output.subspan(JOURNAL_EXPORT_HEADER_SIZE)) : 0;
    const auto body = record < count ? journal.body() : std::span<const uint8_t>{};
    std::fill_n(output.begin(), JOURNAL_EXPORT_HEADER_SIZE, uint8_t{0});
    output[0] = 1;
    output[1] = record == count ? 1 : 0;
    tinta_body_detail::write(output, 4, count, 4);
    tinta_body_detail::write(output, 8, record < count ? record + 1 : count, 4);
    std::copy(frontier.begin(), frontier.end(), output.begin() + 12);
    tinta_body_detail::write(output, 44, envelope, 2);
    tinta_body_detail::write(output, 46, body.size(), 2);
    std::copy(body.begin(), body.end(), output.begin() + JOURNAL_EXPORT_HEADER_SIZE + envelope);
    return JOURNAL_EXPORT_HEADER_SIZE + envelope + body.size();
  }

 private:
  TintaJournal& journal;
  Digest frontier{};
  uint32_t count = 0;
  bool ready = false;
};
}  // namespace companion
