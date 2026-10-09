#include "BookmarkFile.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>

#include <cmath>
#include <cstring>

#include "BookmarkUtil.h"

bool BookmarkFile::load(const std::string& bookPath, std::vector<BookmarkEntry>& bookmarks) {
  const std::string path = BookmarkUtil::getBookmarkPath(bookPath);
  if (!loadFromPath(path.c_str(), bookmarks, false)) return false;
  for (auto& bookmark : bookmarks) {
    if (bookmark.name.size() > BookmarkEntry::MAX_NAME_LENGTH) bookmark.name.resize(BookmarkEntry::MAX_NAME_LENGTH);
  }
  return true;
}

bool BookmarkFile::loadFromPath(const char* path, std::vector<BookmarkEntry>& bookmarks, bool strict) {
  bookmarks.clear();
  if (!path || !*path) {
    LOG_ERR("BKM", "Missing bookmark file path");
    return false;
  }
  // Keep the JSON parser instantiated in PersistableStore.cpp.
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(path, doc)) return false;

  if (!doc["bookmarks"].is<JsonArray>()) {
    LOG_ERR("BKM", "Invalid bookmark array");
    return false;
  }
  JsonArray arr = doc["bookmarks"].as<JsonArray>();
  bookmarks.reserve(arr.size());
  for (JsonVariant value : arr) {
    if (!value.is<JsonObject>()) {
      LOG_ERR("BKM", "Invalid bookmark entry");
      bookmarks.clear();
      return false;
    }
    JsonObject obj = value.as<JsonObject>();
    if (strict) {
      static constexpr const char* TEXT_FIELDS[] = {"xpath", "name", "summary"};
      static constexpr const char* SHORT_FIELDS[] = {"si", "pc", "pp"};
      bool valid = true;
      for (const char* key : TEXT_FIELDS) {
        const JsonVariantConst field = obj[key];
        if (field.isUnbound()) continue;
        if (!field.is<const char*>()) {
          valid = false;
          break;
        }
        const JsonString text = field.as<JsonString>();
        if (memchr(text.c_str(), 0, text.size())) {
          valid = false;
          break;
        }
      }
      for (const char* key : SHORT_FIELDS) {
        const JsonVariantConst field = obj[key];
        if (!field.isUnbound() && !field.is<uint16_t>()) valid = false;
      }
      const JsonVariantConst offset = obj["vo"];
      if (!offset.isUnbound() && (!offset.is<uint32_t>() || !obj["si"].is<uint16_t>())) valid = false;
      const JsonVariantConst percentage = obj["percentage"];
      if (!percentage.isUnbound() && (!percentage.is<float>() || !std::isfinite(percentage.as<float>()))) valid = false;
      if (!valid) {
        LOG_ERR("BKM", "Invalid legacy bookmark fields");
        bookmarks.clear();
        return false;
      }
    }
    bookmarks.emplace_back();
    auto& bookmark = bookmarks.back();
    if (obj["id"].is<const char*>()) {
      const JsonString text = obj["id"].as<JsonString>();
      if (!BookmarkIdentity::decode(std::string_view(text.c_str(), text.size()), bookmark.identity)) {
        LOG_ERR("BKM", "Invalid bookmark identity");
        bookmarks.clear();
        return false;
      }
    } else if (!obj["id"].as<JsonVariantConst>().isUnbound()) {
      LOG_ERR("BKM", "Invalid bookmark identity type");
      bookmarks.clear();
      return false;
    }
    if (BookmarkIdentity::valid(bookmark.identity)) {
      for (size_t i = 0; i + 1 < bookmarks.size(); ++i) {
        if (bookmarks[i].identity == bookmark.identity) {
          LOG_ERR("BKM", "Duplicate bookmark identity");
          bookmarks.clear();
          return false;
        }
      }
    }
    bookmark.xpath = obj["xpath"] | "";
    bookmark.percentage = obj["percentage"] | static_cast<float>(0);
    bookmark.summary = obj["summary"] | "";
    bookmark.name = obj["name"] | "";
    bookmark.computedSpineIndex = obj["si"] | static_cast<uint16_t>(0);
    bookmark.computedChapterPageCount = obj["pc"] | static_cast<uint16_t>(0);
    bookmark.computedChapterProgress = obj["pp"] | static_cast<uint16_t>(0);
    if (!obj["vo"].isNull()) {
      bookmark.visibleTextOffset = obj["vo"] | static_cast<uint32_t>(0);
      bookmark.hasVisibleTextOffset = true;
    }
  }

  LOG_DBG("BKM", "Loaded %zu bookmarks from file", bookmarks.size());
  return true;
}

bool BookmarkFile::save(const std::string& bookPath, const std::vector<BookmarkEntry>& bookmarks) {
  for (size_t i = 0; i < bookmarks.size(); ++i) {
    if (!BookmarkIdentity::valid(bookmarks[i].identity)) continue;
    for (size_t j = 0; j < i; ++j) {
      if (bookmarks[j].identity == bookmarks[i].identity) {
        LOG_ERR("BKM", "Duplicate bookmark identity");
        return false;
      }
    }
  }
  for (const auto& bookmark : bookmarks) {
    if (bookmark.name.size() > BookmarkEntry::MAX_NAME_LENGTH) {
      LOG_ERR("BKM", "Bookmark name exceeds %zu bytes", BookmarkEntry::MAX_NAME_LENGTH);
      return false;
    }
  }

  JsonDocument doc;
  JsonArray arr = doc["bookmarks"].to<JsonArray>();
  LOG_DBG("BKM", "Saving %zu bookmarks to file", bookmarks.size());
  for (const auto& bookmark : bookmarks) {
    JsonObject obj = arr.add<JsonObject>();
    if (BookmarkIdentity::valid(bookmark.identity)) {
      std::array<char, 33> identityText{};
      if (!BookmarkIdentity::encode(bookmark.identity, identityText)) {
        LOG_ERR("BKM", "Cannot encode bookmark identity");
        return false;
      }
      obj["id"] = identityText.data();
    }
    obj["xpath"] = bookmark.xpath;
    obj["percentage"] = bookmark.percentage;
    obj["summary"] = bookmark.summary;
    if (!bookmark.name.empty()) {
      obj["name"] = bookmark.name;
    }
    obj["si"] = bookmark.computedSpineIndex;
    obj["pc"] = bookmark.computedChapterPageCount;
    obj["pp"] = bookmark.computedChapterProgress;
    if (bookmark.hasVisibleTextOffset) {
      obj["vo"] = bookmark.visibleTextOffset;
    }
  }

  // writeDocToFile ensures /.crosspoint; the bookmarks subdirectory is ours.
  Storage.mkdir(BookmarkUtil::getBookmarksDir().c_str());
  const std::string path = BookmarkUtil::getBookmarkPath(bookPath);
  return PersistableStoreBase::writeDocToFile(path.c_str(), doc);
}
