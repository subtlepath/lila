#pragma once

#include "CompanionFrame.h"
#include "CompanionJournalMergeIntent.h"
#include "CompanionTintaJournal.h"

namespace companion {
enum class JournalMergeOperation : uint8_t { Begin = 1, Append = 2, Commit = 3, Abort = 4 };
inline constexpr size_t JOURNAL_MERGE_REQUEST_HEADER_SIZE = 28;
static_assert(JOURNAL_MERGE_REQUEST_HEADER_SIZE + MAX_RECORD_SIZE + TintaJournal::MAX_BODY_SIZE <= MAX_CONTROL_PAYLOAD);

// Views borrow the authenticated request buffer. Decode the envelope into session-owned storage before append.
struct JournalMergeRequestView {
  JournalMergeOperation operation = JournalMergeOperation::Begin;
  Identity transaction{};
  std::span<const uint8_t> declaration, envelope, body;
};

inline bool decodeJournalMergeRequest(std::span<const uint8_t> bytes, JournalMergeRequestView& output) {
  if (bytes.size() < JOURNAL_MERGE_REQUEST_HEADER_SIZE || bytes.size() > MAX_CONTROL_PAYLOAD || bytes[0] != 'J' ||
      bytes[1] != 'M' || bytes[2] != 'R' || bytes[3] != 1 || bytes[4] < 1 || bytes[4] > 4 || bytes[5] != 0 ||
      bytes[6] != 0 || bytes[7] != 0)
    return false;
  const size_t envelopeSize = tinta_body_detail::read(bytes, 24, 2);
  const size_t bodySize = tinta_body_detail::read(bytes, 26, 2);
  if (bytes.size() != JOURNAL_MERGE_REQUEST_HEADER_SIZE + envelopeSize + bodySize) return false;
  JournalMergeRequestView parsed;
  parsed.operation = static_cast<JournalMergeOperation>(bytes[4]);
  std::copy_n(bytes.begin() + 8, 16, parsed.transaction.begin());
  if (!tinta_body_detail::nonzero(parsed.transaction)) return false;
  if (parsed.operation == JournalMergeOperation::Append) {
    if (envelopeSize < SYNC_EVENT_BASE_SIZE || envelopeSize > MAX_RECORD_SIZE || bodySize == 0 ||
        bodySize > TintaJournal::MAX_BODY_SIZE)
      return false;
    parsed.envelope = bytes.subspan(JOURNAL_MERGE_REQUEST_HEADER_SIZE, envelopeSize);
    parsed.body = bytes.subspan(JOURNAL_MERGE_REQUEST_HEADER_SIZE + envelopeSize, bodySize);
  } else {
    if (envelopeSize != 0 || bodySize != JOURNAL_MERGE_INTENT_SIZE) return false;
    parsed.declaration = bytes.subspan(JOURNAL_MERGE_REQUEST_HEADER_SIZE);
    JournalMergeIntent declaration;
    if (!decodeJournalMergeIntent(parsed.declaration, declaration) || declaration.transaction != parsed.transaction)
      return false;
  }
  output = parsed;
  return true;
}

// Payload spans must not overlap output. Validation failures preserve output.
inline size_t encodeJournalMergeRequest(const JournalMergeRequestView& request, std::span<uint8_t> output) {
  const bool append = request.operation == JournalMergeOperation::Append;
  const size_t envelopeSize = append ? request.envelope.size() : 0;
  const auto body = append ? request.body : request.declaration;
  const size_t length = JOURNAL_MERGE_REQUEST_HEADER_SIZE + envelopeSize + body.size();
  if (static_cast<uint8_t>(request.operation) < 1 || static_cast<uint8_t>(request.operation) > 4 ||
      !tinta_body_detail::nonzero(request.transaction) || length > MAX_CONTROL_PAYLOAD || output.size() < length)
    return 0;
  if (append) {
    if (!request.declaration.empty() || envelopeSize < SYNC_EVENT_BASE_SIZE || envelopeSize > MAX_RECORD_SIZE ||
        body.empty() || body.size() > TintaJournal::MAX_BODY_SIZE)
      return 0;
  } else {
    JournalMergeIntent declaration;
    if (!request.envelope.empty() || !request.body.empty() ||
        !decodeJournalMergeIntent(request.declaration, declaration) || declaration.transaction != request.transaction)
      return 0;
  }
  std::fill_n(output.begin(), JOURNAL_MERGE_REQUEST_HEADER_SIZE, uint8_t{0});
  output[0] = 'J';
  output[1] = 'M';
  output[2] = 'R';
  output[3] = 1;
  output[4] = static_cast<uint8_t>(request.operation);
  std::copy(request.transaction.begin(), request.transaction.end(), output.begin() + 8);
  tinta_body_detail::write(output, 24, envelopeSize, 2);
  tinta_body_detail::write(output, 26, body.size(), 2);
  std::copy(request.envelope.begin(), request.envelope.end(), output.begin() + JOURNAL_MERGE_REQUEST_HEADER_SIZE);
  std::copy(body.begin(), body.end(), output.begin() + JOURNAL_MERGE_REQUEST_HEADER_SIZE + envelopeSize);
  return length;
}
}  // namespace companion
