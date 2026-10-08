#pragma once

#include <cstddef>
#include <cstdint>

namespace binary_record {

// Little-endian field access for on-disk records. Every file
// layout is written byte by byte so it does not depend on struct padding or
// host endianness.

inline void putU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

inline void putU32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

inline uint16_t getU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

inline uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// CRC-32 (IEEE, as zlib), nibble-table form: 64 bytes of table, fast enough
// for the few dozen bytes per write it protects.
inline uint32_t crc32Update(uint32_t crc, const void* data, size_t len) {
  static constexpr uint32_t kTable[16] = {0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
                                          0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
                                          0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};
  const uint8_t* p = static_cast<const uint8_t*>(data);
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= p[i];
    crc = (crc >> 4) ^ kTable[crc & 0x0F];
    crc = (crc >> 4) ^ kTable[crc & 0x0F];
  }
  return ~crc;
}

inline uint32_t crc32(const void* data, size_t len) { return crc32Update(0, data, len); }

}  // namespace binary_record
