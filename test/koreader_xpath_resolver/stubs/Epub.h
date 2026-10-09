#pragma once

#include <Print.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

class Epub {
 public:
  struct SpineItem {
    std::string href;
  };

  explicit Epub(std::vector<std::string> spineContents) : spineContents(std::move(spineContents)) {
    companionIdentity.fill(7);
  }
  bool hasCompanionContentIdentity() const { return companionIdentityReady; }
  bool getCompanionContentIdentity(std::span<uint8_t>, std::array<uint8_t, 32>& output) const {
    if (!companionIdentityReady) return false;
    output = companionIdentity;
    return true;
  }
  std::array<uint8_t, 32> companionIdentity{};
  bool companionIdentityReady = true;
  void (*afterStream)(void*) = nullptr;
  void* streamContext = nullptr;

  int getSpineItemsCount() const { return static_cast<int>(spineContents.size()); }

  SpineItem getSpineItem(const int spineIndex) const {
    if (spineIndex < 0 || spineIndex >= getSpineItemsCount()) {
      return {};
    }
    return {"chapter" + std::to_string(spineIndex) + ".xhtml"};
  }

  bool readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize, bool = false) const {
    for (int spineIndex = 0; spineIndex < getSpineItemsCount(); spineIndex++) {
      if (itemHref != getSpineItem(spineIndex).href) {
        continue;
      }

      const auto& contents = spineContents[spineIndex];
      size_t offset = 0;
      while (offset < contents.size()) {
        const size_t size = std::min(chunkSize, contents.size() - offset);
        out.write(reinterpret_cast<const uint8_t*>(contents.data() + offset), size);
        offset += size;
      }
      if (afterStream) afterStream(streamContext);
      return true;
    }
    return false;
  }

 private:
  std::vector<std::string> spineContents;
};
