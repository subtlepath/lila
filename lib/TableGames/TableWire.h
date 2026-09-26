#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Wire format for the Games "table": a host-authoritative session that guests
// join over ESP-NOW. Every multi-byte field is little-endian and read through
// Reader/Writer byte by byte, so no packet buffer is ever cast to a wider type
// (RISC-V faults on unaligned loads).
namespace table {

constexpr size_t MAC_LEN = 6;
// Slot 0 is always the host; guests and CPU players take 1..MAX_SLOTS-1.
constexpr uint8_t MAX_SLOTS = 6;
constexpr uint8_t HOST_SLOT = 0;
constexpr uint8_t NO_SLOT = 0xFF;
// Player names travel as fixed 12-byte UTF-8 fields (zero padded).
constexpr size_t NAME_LEN = 12;
// ESP-NOW v1 payload limit. Staying under it keeps every IDF version and
// board interoperable; no game state comes close.
constexpr size_t MAX_PACKET = 250;
constexpr size_t HEADER_LEN = 10;
constexpr size_t MAX_GAME_STATE = 200;
constexpr size_t MAX_ACTION = 8;
constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr uint8_t MAGIC0 = 'C';
constexpr uint8_t MAGIC1 = 'G';

enum class MsgType : uint8_t {
  Beacon = 1,  // host -> broadcast, 1 Hz: table advert and host heartbeat
  Join,        // guest -> host: sit down (also used to rejoin after a dropout)
  Welcome,     // host -> guest: assigned slot
  Deny,        // host -> guest: no seat
  Roster,      // host -> guest: seats, current game, phase
  Leave,       // guest -> host: standing up
  Close,       // host -> guest: table closed
  Ping,        // guest -> host, 1 Hz: heartbeat carrying the versions it holds
  State,       // host -> guest: game state as that guest may see it
  Action,      // guest -> host: a move
};
constexpr uint8_t MSG_TYPE_MAX = static_cast<uint8_t>(MsgType::Action);

enum class DenyReason : uint8_t { Full = 1, Closed = 2 };

enum class Phase : uint8_t { Lobby = 0, Playing = 1 };

enum class GameId : uint8_t { None = 0, ConnectFour = 1, DotsAndBoxes = 2, LiarsDice = 3, MurderMystery = 4 };
constexpr uint8_t GAME_ID_MAX = static_cast<uint8_t>(GameId::MurderMystery);

enum class SeatStatus : uint8_t { Empty = 0, Present = 1, Away = 2, Bot = 3 };

// Length of the longest prefix of s that fits maxBytes without splitting a
// UTF-8 sequence.
inline size_t utf8Prefix(const char* s, const size_t maxBytes) {
  size_t n = strnlen(s, maxBytes);
  if (n == maxBytes && s[n] != '\0') {
    // Back off to the start of the character that straddles the limit.
    while (n > 0 && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80) n--;
  }
  return n;
}

// Wrapping 16-bit sequence comparison: true when a is newer than b.
constexpr bool seqNewer(const uint16_t a, const uint16_t b) { return static_cast<int16_t>(a - b) > 0; }

class Writer {
 public:
  Writer(uint8_t* buffer, const size_t capacity) : buf(buffer), cap(capacity) {}

  void u8(const uint8_t v) {
    if (!reserve(1)) return;
    buf[len++] = v;
  }
  void u16(const uint16_t v) {
    u8(static_cast<uint8_t>(v));
    u8(static_cast<uint8_t>(v >> 8));
  }
  void u32(const uint32_t v) {
    for (int i = 0; i < 4; i++) u8(static_cast<uint8_t>(v >> (i * 8)));
  }
  void bytes(const void* data, const size_t n) {
    if (!reserve(n)) return;
    if (n > 0) memcpy(buf + len, data, n);
    len += n;
  }
  // Fixed-width name field: truncated or zero padded to NAME_LEN bytes.
  void name(const char* s) {
    uint8_t field[NAME_LEN] = {};
    if (s) memcpy(field, s, utf8Prefix(s, NAME_LEN));
    bytes(field, NAME_LEN);
  }

  bool ok() const { return good; }
  size_t size() const { return len; }
  uint8_t* data() { return buf; }

 private:
  bool reserve(const size_t n) {
    if (!good || len + n > cap) {
      good = false;
      return false;
    }
    return true;
  }

  uint8_t* buf;
  size_t cap;
  size_t len = 0;
  bool good = true;
};

class Reader {
 public:
  Reader(const uint8_t* buffer, const size_t length) : buf(buffer), len(length) {}

  uint8_t u8() {
    if (!good || pos + 1 > len) {
      good = false;
      return 0;
    }
    return buf[pos++];
  }
  uint16_t u16() {
    const uint16_t lo = u8();
    return static_cast<uint16_t>(lo | (static_cast<uint16_t>(u8()) << 8));
  }
  uint32_t u32() {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(u8()) << (i * 8);
    return v;
  }
  bool bytes(void* out, const size_t n) {
    if (!good || pos + n > len) {
      good = false;
      return false;
    }
    if (n > 0) memcpy(out, buf + pos, n);
    pos += n;
    return true;
  }
  // Reads a NAME_LEN field into a NAME_LEN+1 buffer, always null-terminated.
  void name(char* out) {
    uint8_t field[NAME_LEN] = {};
    bytes(field, NAME_LEN);
    memcpy(out, field, NAME_LEN);
    out[NAME_LEN] = '\0';
  }
  const uint8_t* cursor() const { return buf + pos; }
  size_t remaining() const { return good ? len - pos : 0; }
  void skip(const size_t n) {
    if (!good || pos + n > len) {
      good = false;
      return;
    }
    pos += n;
  }
  bool ok() const { return good; }

 private:
  const uint8_t* buf;
  size_t len;
  size_t pos = 0;
  bool good = true;
};

struct Header {
  MsgType type = MsgType::Beacon;
  uint32_t tableId = 0;
  uint8_t srcSlot = NO_SLOT;
};

inline void writeHeader(Writer& w, const MsgType type, const uint32_t tableId, const uint8_t srcSlot) {
  w.u8(MAGIC0);
  w.u8(MAGIC1);
  w.u8(PROTOCOL_VERSION);
  w.u8(static_cast<uint8_t>(type));
  w.u32(tableId);
  w.u8(srcSlot);
  w.u8(0);  // reserved flags
}

// Rejects foreign ESP-NOW traffic, other protocol versions and unknown types.
inline bool readHeader(Reader& r, Header& out) {
  if (r.u8() != MAGIC0 || r.u8() != MAGIC1 || r.u8() != PROTOCOL_VERSION) return false;
  const uint8_t type = r.u8();
  out.tableId = r.u32();
  out.srcSlot = r.u8();
  r.u8();  // reserved flags
  if (!r.ok() || type < 1 || type > MSG_TYPE_MAX) return false;
  out.type = static_cast<MsgType>(type);
  return true;
}

}  // namespace table
