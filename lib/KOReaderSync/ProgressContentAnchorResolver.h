#pragma once

#include <Epub.h>
#include <Logging.h>
#include <expat.h>

#include <array>
#include <string_view>

#include "ProgressXPathStream.h"

struct XPathContentAnchor {
  uint16_t spineIndex = 0;
  uint32_t visibleTextOffset = 0;
  bool operator==(const XPathContentAnchor&) const = default;
};
// Checked off-stack batch owner; reuse parser pools and bounded traversal state.
class ProgressContentAnchorResolver final : private Print {
 public:
  ProgressContentAnchorResolver() : parser(XML_ParserCreate(nullptr)) {
    if (!parser) {
      LOG_ERR("PM", "OOM: content anchor XML parser");
    }
  }
  ~ProgressContentAnchorResolver() override {
    if (parser) XML_ParserFree(parser);
  }
  ProgressContentAnchorResolver(const ProgressContentAnchorResolver&) = delete;
  ProgressContentAnchorResolver& operator=(const ProgressContentAnchorResolver&) = delete;
  bool resolve(Epub& epub, const std::string& xpath, XPathContentAnchor& output) {
    using namespace progress_xpath_detail;
    static constexpr std::string_view PREFIX = "/body/DocFragment[";
    if (!parser || !xpath.starts_with(PREFIX) || xpath.find('\0') != std::string::npos) return false;
    const int fragment = parseIndex(xpath, PREFIX.data());
    if (fragment <= 0 || fragment > epub.getSpineItemsCount() || fragment > 65536) return false;
    const int spine = fragment - 1;
    const auto bracket = xpath.find(']');
    const std::string_view suffix(xpath.data() + bracket + 1, xpath.size() - bracket - 1);
    if (suffix.empty() || suffix == "/body" || suffix == "/body/" || suffix == ".0") {
      output = {static_cast<uint16_t>(spine), 0};
      return true;
    }
    const auto dot = xpath.rfind('.');
    if (dot == std::string::npos || dot + 1 == xpath.size()) return false;
    for (size_t i = dot + 1; i < xpath.size(); ++i)
      if (xpath[i] < '0' || xpath[i] > '9') return false;
    const auto textPos = xpath.rfind("/text()");
    if (textPos != std::string::npos) {
      const auto after = textPos + 7;
      if (after != dot &&
          (xpath[after] != '[' || xpath.find(']', after) != dot - 1 || parseIndex(xpath, "/text()[", true) <= 0))
        return false;
    }
    character = parseCharOffset(xpath);
    textNode = parseTextNodeIndex(xpath);
    if (character < 0 || textNode <= 0) return false;
    stepCount = parseXPathSteps(xpath, steps.data());
    bodyText = isBodyTextXPath(xpath);
    elementStart = textPos == std::string::npos && character == 0;
    if (!stepCount && !bodyText) return false;
    if (!stream(epub, spine)) return false;
    if (!found || target > UINT32_MAX) return false;
    output = {static_cast<uint16_t>(spine), static_cast<uint32_t>(target)};
    return true;
  }

 private:
  static constexpr int MAX_DEPTH = 64;
  struct TextRun {
    uint32_t index = 0;
    uint64_t characters = 0;
    bool active = false;
  };
  bool stream(Epub& epub, int spine) {
    if (!XML_ParserReset(parser, nullptr)) return false;
    nodes.fill({});
    counters.fill(0);
    entered.fill(0);
    depth = bodyDepth = hiddenDepth = matched = 0;
    visible = target = 0;
    found = failed = false;
    targetMatches = 0;
    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &start, &end);
    XML_SetCharacterDataHandler(parser, &text);
    XML_SetCommentHandler(parser, &comment);
    XML_SetProcessingInstructionHandler(parser, &instruction);
    XML_SetCdataSectionHandler(parser, &boundary, &boundary);
    const auto& href = epub.getSpineItem(spine).href;
    return !href.empty() && epub.readItemContentsToStream(href, *this, 1024) && !failed &&
           XML_Parse(parser, nullptr, 0, XML_TRUE) == XML_STATUS_OK;
  }
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t size) override {
    if (failed || size > INT_MAX ||
        XML_Parse(parser, reinterpret_cast<const char*>(bytes), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      failed = true;
      return 0;
    }
    return size;
  }
  static const char* local(const char* name) {
    const auto colon = strrchr(name, ':');
    return colon ? colon + 1 : name;
  }
  static void XMLCALL start(void* opaque, const XML_Char* name, const XML_Char**) {
    auto& self = *static_cast<ProgressContentAnchorResolver*>(opaque);
    self.nodes[self.depth].active = false;
    if (self.depth == MAX_DEPTH) {
      self.failed = true;
      XML_StopParser(self.parser, XML_FALSE);
      return;
    }
    ++self.depth;
    self.nodes[self.depth] = {};
    name = local(name);
    if (!self.bodyDepth && strcasecmp(name, "body") == 0) {
      self.bodyDepth = self.depth;
      return;
    }
    if (!self.bodyDepth) return;
    if (self.hiddenDepth || VisibleTextUtils::isNonVisibleElement(name)) {
      ++self.hiddenDepth;
      return;
    }
    if (self.matched < self.stepCount) {
      const auto& step = self.steps[self.matched];
      const bool correctDepth =
          self.matched ? self.depth == self.entered[self.matched - 1] + 1 : self.depth == self.bodyDepth + 1;
      if (correctDepth && strcasecmp(name, step.tag) == 0) {
        if (self.counters[self.matched] == INT_MAX) {
          self.failed = true;
          XML_StopParser(self.parser, XML_FALSE);
          return;
        }
        ++self.counters[self.matched];
        if (!step.siblingIndex || self.counters[self.matched] == step.siblingIndex) {
          self.entered[self.matched++] = self.depth;
          if (self.matched == self.stepCount && ++self.targetMatches > 1) {
            self.failed = true;
            XML_StopParser(self.parser, XML_FALSE);
            return;
          }
          if (self.matched == self.stepCount && self.elementStart) {
            self.target = self.visible;
            self.found = true;
          }
        }
      }
    }
  }
  static void XMLCALL end(void* opaque, const XML_Char*) {
    auto& self = *static_cast<ProgressContentAnchorResolver*>(opaque);
    if (self.hiddenDepth) --self.hiddenDepth;
    if (self.matched && self.depth == self.entered[self.matched - 1]) {
      --self.matched;
      for (int i = self.matched + 1; i < self.stepCount; ++i) self.counters[i] = 0;
    }
    if (self.depth == self.bodyDepth) self.bodyDepth = 0;
    if (self.depth) --self.depth;
    self.nodes[self.depth].active = false;
  }
  static void XMLCALL text(void* opaque, const XML_Char* bytes, int size) {
    auto& self = *static_cast<ProgressContentAnchorResolver*>(opaque);
    if (!self.bodyDepth || self.hiddenDepth || size <= 0) return;
    auto& node = self.nodes[self.depth];
    if (!node.active) {
      ++node.index;
      node.characters = 0;
      node.active = true;
    }
    const bool selected = self.bodyText
                              ? self.depth == self.bodyDepth
                              : self.matched == self.stepCount && self.depth == self.entered[self.stepCount - 1];
    for (int i = 0; i < size; ++i) {
      if ((static_cast<uint8_t>(bytes[i]) & 0xc0) == 0x80) continue;
      if (!self.found && selected && node.index == static_cast<uint32_t>(self.textNode) &&
          node.characters == static_cast<uint32_t>(self.character)) {
        self.target = self.visible;
        self.found = true;
      }
      ++self.visible;
      ++node.characters;
      if (!self.found && selected && node.index == static_cast<uint32_t>(self.textNode) &&
          node.characters == static_cast<uint32_t>(self.character)) {
        self.target = self.visible;
        self.found = true;
      }
    }
  }
  static void XMLCALL boundary(void* opaque) {
    auto& self = *static_cast<ProgressContentAnchorResolver*>(opaque);
    self.nodes[self.depth].active = false;
  }
  static void XMLCALL comment(void* opaque, const XML_Char*) { boundary(opaque); }
  static void XMLCALL instruction(void* opaque, const XML_Char*, const XML_Char*) { boundary(opaque); }
  XML_Parser parser;
  std::array<progress_xpath_detail::XPathStep, progress_xpath_detail::MAX_XPATH_DEPTH> steps{};
  std::array<int, progress_xpath_detail::MAX_XPATH_DEPTH> counters{}, entered{};
  std::array<TextRun, MAX_DEPTH + 1> nodes{};
  int character = 0, textNode = 1, stepCount = 0, depth = 0, bodyDepth = 0, hiddenDepth = 0, matched = 0;
  uint64_t visible = 0, target = 0;
  uint8_t targetMatches = 0;
  bool bodyText = false, elementStart = false, found = false, failed = false;
};
