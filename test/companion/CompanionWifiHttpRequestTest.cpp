#include <gtest/gtest.h>

#include <array>
#include <string>

#include "lib/Companion/CompanionWifiHttpRequest.h"

using namespace companion;
namespace {
std::span<const uint8_t> bytes(const std::string& text) {
  return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}
std::string request(size_t count = 48, std::string extra = "") {
  return "POST /companion/v1/messages HTTP/1.1\r\nHost: 192.168.4.1:8080\r\nContent-Type: "
         "application/octet-stream\r\nContent-Length: " +
         std::to_string(count) + "\r\n" + extra + "\r\n" + std::string(count, '\x03');
}
}  // namespace
TEST(CompanionWifiHttpRequestTest, EverySplitAndSingleByteInputPreserveBinaryBody) {
  auto text = request();
  text[text.size() - 48] = '\0';
  text.back() = '\xff';
  std::array<uint8_t, WifiHttpRequest::MAX_BODY_SIZE + 2> body{};
  WifiHttpRequest parser;
  for (size_t split = 0; split <= text.size(); ++split) {
    body.fill(9);
    parser.reset(std::span(body).subspan(1, WifiHttpRequest::MAX_BODY_SIZE));
    ASSERT_TRUE(parser.feed(bytes(text).first(split)));
    ASSERT_TRUE(parser.feed(bytes(text).subspan(split)));
    ASSERT_TRUE(parser.endOfInput());
    EXPECT_EQ(parser.state(), WifiHttpRequestState::Complete);
    EXPECT_EQ(parser.length(), 48u);
    EXPECT_EQ(body.front(), 9);
    EXPECT_EQ(body.back(), 9);
    EXPECT_EQ(body[1], 0);
    EXPECT_EQ(body[48], 255);
  }
  parser.reset(std::span(body).subspan(1, WifiHttpRequest::MAX_BODY_SIZE));
  for (size_t at = 0; at < text.size(); ++at) ASSERT_TRUE(parser.feed(bytes(text).subspan(at, 1)));
  EXPECT_TRUE(parser.endOfInput());
}
TEST(CompanionWifiHttpRequestTest, BodyAndStorageBoundsRejectBeforeWriting) {
  std::array<uint8_t, WifiHttpRequest::MAX_BODY_SIZE> body{};
  WifiHttpRequest parser;
  for (const size_t size : {size_t{0}, size_t{47}, WifiHttpRequest::MAX_BODY_SIZE + 1, size_t{9999}}) {
    body.fill(7);
    parser.reset(body);
    EXPECT_FALSE(parser.feed(bytes(request(size))));
    EXPECT_EQ(parser.state(), WifiHttpRequestState::Failed);
    EXPECT_EQ(parser.length(), 0u);
    EXPECT_TRUE(std::all_of(body.begin(), body.end(), [](uint8_t byte) { return byte == 7; }));
  }
  parser.reset(std::span(body).first(body.size() - 1));
  EXPECT_FALSE(parser.feed(bytes(request())));
  parser.reset(body);
  ASSERT_TRUE(parser.feed(bytes(request(WifiHttpRequest::MAX_BODY_SIZE))));
  EXPECT_EQ(parser.length(), WifiHttpRequest::MAX_BODY_SIZE);
  EXPECT_EQ(body.back(), 3);
}
TEST(CompanionWifiHttpRequestTest, FramingAmbiguityAndUnsupportedBodyEncodingsAreRejected) {
  std::array<uint8_t, WifiHttpRequest::MAX_BODY_SIZE> body{};
  WifiHttpRequest parser;
  for (const auto& extra :
       {"Content-Length: 48\r\n", "content-length: 48\r\n", "Content-Length: 49\r\n", "Transfer-Encoding: chunked\r\n",
        "Transfer-Encoding: identity\r\n", "Content-Encoding: gzip\r\n", "Expect: 100-continue\r\n",
        "Host: another\r\n", "Content-Type: application/octet-stream\r\n", " folded-header\r\n",
        "Content-Length : 48\r\n", "Broken header\r\n"}) {
    parser.reset(body);
    EXPECT_FALSE(parser.feed(bytes(request(48, extra)))) << extra;
  }
  for (const auto& length : {"-1", "+48", "4 8", "48x", "18446744073709551616000"}) {
    auto text = request();
    const auto at = text.find("Content-Length: 48");
    text.replace(at, 18, std::string("Content-Length: ") + length);
    parser.reset(body);
    EXPECT_FALSE(parser.feed(bytes(text))) << length;
  }
  for (const auto& type : {"text/plain", "application/octet-stream; charset=utf-8"}) {
    auto text = request();
    const auto at = text.find("application/octet-stream");
    text.replace(at, 24, type);
    parser.reset(body);
    EXPECT_FALSE(parser.feed(bytes(text)));
  }
}
TEST(CompanionWifiHttpRequestTest, ExactRouteStrictLinesAndRequiredHeadersAreChecked) {
  std::array<uint8_t, WifiHttpRequest::MAX_BODY_SIZE> body{};
  WifiHttpRequest parser;
  for (const auto& line :
       {"GET /companion/v1/messages HTTP/1.1", "POST /other HTTP/1.1", "POST /companion/v1/messages?x=1 HTTP/1.1",
        "POST http://reader/companion/v1/messages HTTP/1.1", "POST /companion/v1/messages HTTP/2",
        "POST /companion/v1/messages HTTP/1.1 "}) {
    auto text = request();
    text.replace(0, text.find('\r'), line);
    parser.reset(body);
    EXPECT_FALSE(parser.feed(bytes(text)));
  }
  for (const auto& name : {"Host", "Content-Length", "Content-Type"}) {
    auto text = request();
    const auto at = text.find(std::string(name) + ":");
    text.erase(at, text.find("\r\n", at) + 2 - at);
    parser.reset(body);
    EXPECT_FALSE(parser.feed(bytes(text)));
  }
  for (const char bad : {'\0', '\n', '\x01', '\x7f'}) {
    auto text = request();
    text[40] = bad;
    parser.reset(body);
    EXPECT_FALSE(parser.feed(bytes(text)));
  }
  auto bareLf = request();
  for (size_t at = 0; (at = bareLf.find("\r\n", at)) != std::string::npos;) bareLf.erase(at, 1);
  parser.reset(body);
  EXPECT_FALSE(parser.feed(bytes(bareLf)));
}
TEST(CompanionWifiHttpRequestTest, HeaderLimitsTruncationTrailingBytesAndReset) {
  std::array<uint8_t, WifiHttpRequest::MAX_BODY_SIZE> body{};
  WifiHttpRequest parser;
  parser.reset(body);
  EXPECT_FALSE(parser.feed(bytes(request(48, "X-Long: " + std::string(256, 'a') + "\r\n"))));
  std::string extra;
  extra.reserve(WifiHttpRequest::MAX_HEADER_BYTES + 256);
  for (size_t at = 0; at < 30; ++at) extra += "X-Header: " + std::string(70, 'a') + "\r\n";
  parser.reset(body);
  EXPECT_FALSE(parser.feed(bytes(request(48, extra))));
  const auto text = request();
  for (size_t length = 0; length < text.size(); ++length) {
    parser.reset(body);
    ASSERT_TRUE(parser.feed(bytes(text).first(length)));
    EXPECT_FALSE(parser.endOfInput());
    EXPECT_EQ(parser.length(), 0u);
  }
  parser.reset(body);
  EXPECT_FALSE(parser.feed(bytes(text + "x")));
  parser.reset(body);
  ASSERT_TRUE(parser.feed(bytes(text)));
  EXPECT_FALSE(parser.feed(bytes(text)));
  parser.reset(body);
  ASSERT_TRUE(parser.feed(bytes(text)));
  EXPECT_TRUE(parser.endOfInput());
}
TEST(CompanionWifiHttpRequestTest, CaseInsensitiveHeadersWhitespaceAndHttp10) {
  std::array<uint8_t, WifiHttpRequest::MAX_BODY_SIZE> body{};
  WifiHttpRequest parser;
  const std::string text =
      "POST /companion/v1/messages HTTP/1.0\r\ncOnTeNt-LeNgTh:\t0048 \t\r\n"
      "CONTENT-TYPE: APPLICATION/OCTET-STREAM\r\nUser-Agent: lila\r\n\r\n" +
      std::string(48, '\x03');
  parser.reset(body);
  ASSERT_TRUE(parser.feed(bytes(text)));
  EXPECT_TRUE(parser.endOfInput());
  EXPECT_EQ(parser.length(), 48u);
}

TEST(CompanionWifiHttpRequestTest, ExactHeaderAndLineLimitsAreAccepted) {
  std::array<uint8_t, WifiHttpRequest::MAX_BODY_SIZE> body{};
  WifiHttpRequest parser;
  auto exactLine = request(48, "X: " + std::string(WifiHttpRequest::MAX_LINE_BYTES - 3, 'a') + "\r\n");
  parser.reset(body);
  ASSERT_TRUE(parser.feed(bytes(exactLine)));
  EXPECT_TRUE(parser.endOfInput());
  const auto base = request();
  size_t needed = WifiHttpRequest::MAX_HEADER_BYTES - (base.find("\r\n\r\n") + 4);
  std::string extra;
  extra.reserve(needed);
  while (needed > 210) {
    extra += "X: " + std::string(200, 'a') + "\r\n";
    needed -= 205;
  }
  ASSERT_GE(needed, 5u);
  extra += "X: " + std::string(needed - 5, 'a') + "\r\n";
  const auto exactHeaders = request(48, extra);
  ASSERT_EQ(exactHeaders.find("\r\n\r\n") + 4, WifiHttpRequest::MAX_HEADER_BYTES);
  parser.reset(body);
  ASSERT_TRUE(parser.feed(bytes(exactHeaders)));
  EXPECT_TRUE(parser.endOfInput());
  extra.insert(3, 1, 'a');
  parser.reset(body);
  EXPECT_FALSE(parser.feed(bytes(request(48, extra))));
}
