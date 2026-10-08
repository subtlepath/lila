#pragma once

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

#include "CompanionFrame.h"

namespace companion {
enum class WifiHttpRequestState { RequestLine, Headers, Body, Complete, Failed };
// Session-owned; the ciphertext body borrows the message processor's request buffer.
class WifiHttpRequest final {
 public:
  static constexpr size_t MIN_BODY_SIZE = 31 + 1 + 16;
  static constexpr size_t MAX_BODY_SIZE = FRAME_HEADER_SIZE + MAX_CONTROL_PAYLOAD + 31 + 16;
  static constexpr size_t MAX_HEADER_BYTES = 2048;
  static constexpr size_t MAX_LINE_BYTES = 255;
  void reset(std::span<uint8_t> body) {
    this->body = body;
    phase = body.size() >= MAX_BODY_SIZE ? WifiHttpRequestState::RequestLine : WifiHttpRequestState::Failed;
    line.fill(0);
    lineLength = headerBytes = contentLength = bodyLength = 0;
    waitingLf = hasLength = hasType = hasHost = requiresHost = false;
  }
  WifiHttpRequestState state() const { return phase; }
  size_t length() const { return phase == WifiHttpRequestState::Complete ? bodyLength : 0; }
  // Input bytes must not overlap body storage.
  bool feed(std::span<const uint8_t> bytes) {
    if (phase == WifiHttpRequestState::Failed) return false;
    for (const auto byte : bytes) {
      if (phase == WifiHttpRequestState::Complete) return fail();
      if (phase == WifiHttpRequestState::Body) {
        body[bodyLength++] = byte;
        if (bodyLength == contentLength) phase = WifiHttpRequestState::Complete;
        continue;
      }
      if (++headerBytes > MAX_HEADER_BYTES) return fail();
      if (waitingLf) {
        waitingLf = false;
        if (byte != '\n' || !parseLine()) return fail();
        lineLength = 0;
      } else if (byte == '\r') {
        waitingLf = true;
      } else {
        if (byte == '\n' || byte == 0 || byte == 127 || (byte < 32 && byte != '\t') || lineLength == MAX_LINE_BYTES)
          return fail();
        line[lineLength++] = static_cast<char>(byte);
      }
    }
    return true;
  }
  bool endOfInput() {
    if (phase == WifiHttpRequestState::Complete) return true;
    return fail();
  }

 private:
  static bool equalAscii(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (size_t at = 0; at < left.size(); ++at) {
      const char byte = left[at];
      if ((byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte) != right[at]) return false;
    }
    return true;
  }
  static std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
  }
  static bool headerName(std::string_view value) {
    static constexpr std::string_view PUNCTUATION = "!#$%&'*+-.^_`|~";
    if (value.empty()) return false;
    for (const unsigned char byte : value) {
      if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
          PUNCTUATION.find(static_cast<char>(byte)) != std::string_view::npos)
        continue;
      return false;
    }
    return true;
  }
  bool parseLine() {
    const std::string_view value(line.data(), lineLength);
    if (phase == WifiHttpRequestState::RequestLine) {
      static constexpr std::string_view HTTP11 = "POST /companion/v1/messages HTTP/1.1";
      static constexpr std::string_view HTTP10 = "POST /companion/v1/messages HTTP/1.0";
      if (value != HTTP11 && value != HTTP10) return false;
      requiresHost = value == HTTP11;
      phase = WifiHttpRequestState::Headers;
      return true;
    }
    if (value.empty()) {
      if (!hasLength || !hasType || (requiresHost && !hasHost)) return false;
      phase = WifiHttpRequestState::Body;
      return true;
    }
    const auto colon = value.find(':');
    if (colon == std::string_view::npos) return false;
    const auto name = value.substr(0, colon);
    if (!headerName(name)) return false;
    const auto content = trim(value.substr(colon + 1));
    if (equalAscii(name, "content-length")) {
      if (hasLength || content.empty()) return false;
      size_t parsed = 0;
      for (const auto byte : content) {
        if (byte < '0' || byte > '9' || parsed > MAX_BODY_SIZE / 10) return false;
        parsed = parsed * 10 + static_cast<size_t>(byte - '0');
        if (parsed > MAX_BODY_SIZE) return false;
      }
      if (parsed < MIN_BODY_SIZE) return false;
      contentLength = parsed;
      hasLength = true;
    } else if (equalAscii(name, "content-type")) {
      if (hasType || !equalAscii(content, "application/octet-stream")) return false;
      hasType = true;
    } else if (equalAscii(name, "host")) {
      if (hasHost || content.empty()) return false;
      hasHost = true;
    } else if (equalAscii(name, "transfer-encoding") || equalAscii(name, "content-encoding") ||
               equalAscii(name, "expect")) {
      return false;
    }
    return true;
  }
  bool fail() {
    phase = WifiHttpRequestState::Failed;
    return false;
  }
  std::span<uint8_t> body;
  std::array<char, MAX_LINE_BYTES + 1> line{};
  size_t lineLength = 0, headerBytes = 0, contentLength = 0, bodyLength = 0;
  bool waitingLf = false, hasLength = false, hasType = false, hasHost = false, requiresHost = false;
  WifiHttpRequestState phase = WifiHttpRequestState::Failed;
};
}  // namespace companion
