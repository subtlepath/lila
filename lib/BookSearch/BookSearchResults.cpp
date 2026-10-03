#include "BookSearchResults.h"

#include <HalStorage.h>

#include <cstring>

namespace booksearch {

namespace {

// Little-endian field access through memcpy: the buffers are byte arrays (RISC-V faults on unaligned loads).
template <typename T>
void put(uint8_t* buffer, const size_t at, const T value) {
  memcpy(buffer + at, &value, sizeof(T));
}

template <typename T>
T get(const uint8_t* buffer, const size_t at) {
  T value;
  memcpy(&value, buffer + at, sizeof(T));
  return value;
}

constexpr size_t QUERY_FIELD = QUERY_MAX_BYTES + 1;
static_assert(17 + QUERY_FIELD <= ResultsHeader::SIZE, "header fields overrun");
static_assert(16 + ResultRecord::EXCERPT_BYTES <= ResultRecord::SIZE, "record fields overrun");

}  // namespace

void ResultsHeader::serialize(uint8_t (&out)[SIZE]) const {
  memset(out, 0, SIZE);
  put(out, 0, MAGIC);
  put(out, 4, VERSION);
  put(out, 5, static_cast<uint8_t>(state));
  put(out, 6, spineCount);
  put(out, 8, nextSpine);
  put(out, 10, spineFirstResult);
  put(out, 12, count);
  put(out, 14, tagAtNextSpine);
  const size_t length = strnlen(query, QUERY_MAX_BYTES);
  put(out, 16, static_cast<uint8_t>(length));
  memcpy(out + 17, query, length);
}

bool ResultsHeader::deserialize(const uint8_t (&in)[SIZE]) {
  if (get<uint32_t>(in, 0) != MAGIC || get<uint8_t>(in, 4) != VERSION) return false;
  const uint8_t rawState = get<uint8_t>(in, 5);
  if (rawState > static_cast<uint8_t>(SearchState::Full)) return false;
  state = static_cast<SearchState>(rawState);
  spineCount = get<uint16_t>(in, 6);
  nextSpine = get<uint16_t>(in, 8);
  spineFirstResult = get<uint16_t>(in, 10);
  count = get<uint16_t>(in, 12);
  tagAtNextSpine = get<int16_t>(in, 14);
  const uint8_t length = get<uint8_t>(in, 16);
  if (length > QUERY_MAX_BYTES || count > MAX_RESULTS || spineFirstResult > count) return false;
  memcpy(query, in + 17, length);
  query[length] = '\0';
  return true;
}

void ResultRecord::serialize(uint8_t (&out)[SIZE]) const {
  memset(out, 0, SIZE);
  put(out, 0, spineIndex);
  put(out, 2, tocIndex);
  put(out, 4, start);
  put(out, 8, end);
  put(out, 12, bookPercent);
  memcpy(out + 16, excerpt, strnlen(excerpt, EXCERPT_BYTES - 1));
}

void ResultRecord::deserialize(const uint8_t (&in)[SIZE]) {
  spineIndex = get<uint16_t>(in, 0);
  tocIndex = get<int16_t>(in, 2);
  start = get<uint32_t>(in, 4);
  end = get<uint32_t>(in, 8);
  bookPercent = get<uint8_t>(in, 12);
  memcpy(excerpt, in + 16, EXCERPT_BYTES - 1);
  excerpt[EXCERPT_BYTES - 1] = '\0';
}

std::string resultsPath(const std::string& bookCachePath) { return bookCachePath + "/search.bin"; }

bool readHeader(const std::string& path, ResultsHeader& header) {
  HalFile file;
  if (!Storage.exists(path.c_str()) || !Storage.openFileForRead("BSR", path, file)) return false;
  uint8_t bytes[ResultsHeader::SIZE];
  if (file.read(bytes, sizeof(bytes)) != static_cast<int>(sizeof(bytes))) return false;
  return header.deserialize(bytes);
}

size_t readResults(const std::string& path, const uint16_t first, const size_t count, ResultRecord* out) {
  HalFile file;
  if (count == 0 || !Storage.openFileForRead("BSR", path, file)) return 0;
  if (!file.seek(ResultsHeader::SIZE + static_cast<size_t>(first) * ResultRecord::SIZE)) return 0;
  size_t read = 0;
  uint8_t bytes[ResultRecord::SIZE];
  while (read < count && file.read(bytes, sizeof(bytes)) == static_cast<int>(sizeof(bytes))) {
    out[read++].deserialize(bytes);
  }
  return read;
}

}  // namespace booksearch
