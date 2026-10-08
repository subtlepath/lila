#include "CompanionEpubReferenceJson.h"

#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "CompanionInventoryPaths.h"

namespace companion {
void* EpubReferenceJson::Arena::allocate(size_t bytes) {
  static constexpr size_t ALIGNMENT = alignof(std::max_align_t);
  if (!bytes || bytes > storage.size() || used > storage.size() - sizeof(size_t)) return nullptr;
  const size_t start = (used + sizeof(size_t) + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
  if (start > storage.size() || bytes > storage.size() - start) return nullptr;
  std::memcpy(storage.data() + start - sizeof(size_t), &bytes, sizeof(bytes));
  used = start + bytes;
  return storage.data() + start;
}
void* EpubReferenceJson::Arena::reallocate(void* pointer, size_t bytes) {
  if (!pointer) return allocate(bytes);
  if (!bytes) return nullptr;
  const auto address = reinterpret_cast<uintptr_t>(pointer);
  const auto base = reinterpret_cast<uintptr_t>(storage.data());
  if (address < base + sizeof(size_t) || address - base >= used) return nullptr;
  const size_t start = address - base;
  size_t previous = 0;
  std::memcpy(&previous, storage.data() + start - sizeof(size_t), sizeof(previous));
  if (previous > used - start || bytes > storage.size() - start) return nullptr;
  if (bytes <= previous || start + previous == used) {
    if (start + previous == used) used = start + bytes;
    std::memcpy(storage.data() + start - sizeof(size_t), &bytes, sizeof(bytes));
    return pointer;
  }
  void* replacement = allocate(bytes);
  if (replacement) std::memcpy(replacement, pointer, previous);
  return replacement;
}
EpubReferenceJson::EpubReferenceJson() : document(&arena) {}
bool EpubReferenceJson::failure(const char* reason) {
  LOG_ERR("COMPANION", "EPUB reference JSON failed: %s", reason);
  return false;
}
bool EpubReferenceJson::load(EpubReferenceJsonReader& source, RemovalMetadataFile kind, std::string_view removedPath,
                             PathEqual equal, void* context) {
  if (!equal || removedPath.empty() || removedPath.size() > INVENTORY_PATH_LIMIT || removedPath.front() != '/') {
    available = modified = false;
    return failure("arguments");
  }
  struct SinglePath {
    std::string_view path;
    PathEqual equal;
    void* context;
  } single{removedPath, equal, context};
  return loadMatching(
      source, kind,
      [](void* opaque, std::string_view path, bool& matched) {
        const auto& single = *static_cast<SinglePath*>(opaque);
        matched = single.equal(single.context, path, single.path);
        return true;
      },
      &single);
}
bool EpubReferenceJson::loadMatching(EpubReferenceJsonReader& source, RemovalMetadataFile kind, PathMatch match,
                                     void* context) {
  available = modified = false;
  document.clear();
  arena.reset();
  if (!match || (kind != RemovalMetadataFile::State && kind != RemovalMetadataFile::Recent))
    return failure("arguments");
  const auto error =
      deserializeJson(document, source, DeserializationOption::NestingLimit(ARDUINOJSON_DEFAULT_NESTING_LIMIT));
  if (error || document.overflowed() || !document.is<JsonObject>()) return failure("parse or arena capacity");
  // ArduinoJson accepts a complete value without consuming trailing bytes.
  for (int byte = source.read(); byte >= 0; byte = source.read()) {
    if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n') return failure("trailing bytes");
  }
  if (!source.healthy()) return failure("source I/O");
  const auto matches = [&](JsonVariantConst value, bool& valid) {
    if (value.isNull()) return false;
    if (!value.is<const char*>()) {
      valid = false;
      return false;
    }
    const auto string = value.as<JsonString>();
    const std::string_view path(string.c_str(), string.size());
    if (path.find('\0') != std::string_view::npos) {
      valid = false;
      return false;
    }
    bool matched = false;
    if (!match(context, path, matched)) {
      valid = false;
      return false;
    }
    return matched;
  };
  bool valid = true;
  if (kind == RemovalMetadataFile::State) {
    if (matches(document["openEpubPath"], valid)) {
      if (!document["openEpubPath"].set("")) return failure("state update capacity");
      modified = true;
    }
  } else {
    const auto books = document["books"];
    if (!books.isNull() && !books.is<JsonArray>()) return failure("books type");
    auto entries = books.as<JsonArray>();
    for (size_t at = 0; at < entries.size();) {
      const auto entry = entries[at];
      if (!entry.is<JsonObject>()) return failure("book entry type");
      if (matches(entry["path"], valid)) {
        entries.remove(at);
        modified = true;
      } else {
        ++at;
      }
      if (!valid) return failure("book path type");
    }
  }
  if (!valid || document.overflowed()) return failure("reference type or capacity");
  available = true;
  return true;
}
size_t EpubReferenceJson::encodedSize() const { return available ? measureJson(document) : 0; }
bool EpubReferenceJson::write(EpubReferenceJsonWriter& destination) const {
  if (!available) return failure("unprepared document");
  const size_t required = measureJson(document);
  if (!required || required > REMOVAL_METADATA_MAX_BYTES || serializeJson(document, destination) != required)
    return failure("serialization length or write");
  return true;
}
}  // namespace companion
