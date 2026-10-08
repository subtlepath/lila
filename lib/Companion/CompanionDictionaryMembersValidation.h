#pragma once

#include "CompanionDictionaryBundleBuilder.h"
#include "CompanionDictionaryIndexValidation.h"
#include "CompanionDictionaryInfoValidation.h"
#include "CompanionDictzipValidation.h"

namespace companion {
struct DictionaryMembersDetails {
  DictionaryInfoDetails info{};
  uint64_t definitionBytes = 0;
  bool compressed = false, synonyms = false;
  bool operator==(const DictionaryMembersDetails&) const = default;
};
// Session-owned; source keeps all members stable until validation/build finish.
// The index state exceeds the task-local budget. Buffers and decoder are borrowed.
class DictionaryMembersValidation final {
 public:
  using Progress = bool (*)(void*);
  DictionaryMembersValidation(DictionaryBundleSource& source, std::span<uint8_t> scratch,
                              tinfl_decompressor* decoder = nullptr, std::span<uint8_t> window = {},
                              Progress progress = nullptr, void* context = nullptr)
      : source(source),
        view(source),
        scratch(scratch),
        decoder(decoder),
        window(window),
        progress(progress),
        context(context) {}
  // Rebind only between synchronous validations; the caller retains both buffers.
  void setCompressedResources(tinfl_decompressor* value, std::span<uint8_t> bytes) {
    decoder = value;
    window = bytes;
  }
  bool validate(bool compressed, bool synonyms, DictionaryMembersDetails& output) {
    struct Close {
      DictionaryBundleSource& source;
      bool closed = false;
      ~Close() {
        if (!closed) source.close();
      }
    } close{source};
    if (scratch.size() < 12 || (compressed && (!decoder || window.size() != 32768))) return false;
    const unsigned count = synonyms ? 4 : 3;
    for (unsigned member = 0; member < count; ++member)
      if (!source.size(member, lengths[member]) || lengths[member] > UINT32_MAX) return false;
    DictionaryMembersDetails parsed;
    parsed.compressed = compressed;
    parsed.synonyms = synonyms;
    if (!info(parsed.info) || lengths[1] != parsed.info.indexBytes || (synonyms && !parsed.info.hasSynonyms) ||
        (!synonyms && parsed.info.synonyms != 0))
      return false;
    view.member = 0;
    if (compressed) {
      if (!definitions(parsed.definitionBytes)) return false;
    } else {
      parsed.definitionBytes = lengths[0];
      if (!readDefinitions()) return false;
    }
    view.member = 1;
    if (!index.validate(view, scratch, parsed.info.words, lengths[1], DictionaryIndexMode::Definitions,
                        parsed.definitionBytes, progress, context))
      return false;
    if (synonyms) {
      view.member = 3;
      if (!index.validate(view, scratch, parsed.info.synonyms, lengths[3], DictionaryIndexMode::Synonyms,
                          parsed.info.words, progress, context))
        return false;
    }
    for (unsigned member = 0; member < count; ++member) {
      uint64_t actual;
      if (!source.size(member, actual) || actual != lengths[member]) return false;
    }
    if (!source.close()) return false;
    close.closed = true;
    output = parsed;
    return true;
  }

 private:
  class View final : public InventoryIndexStorage {
   public:
    explicit View(DictionaryBundleSource& source) : source(source) {}
    unsigned member = 0;
    bool size(uint64_t& bytes) override { return source.size(member, bytes); }
    bool read(uint64_t at, std::span<uint8_t> bytes) override { return source.read(member, at, bytes); }

   private:
    DictionaryBundleSource& source;
  };
  DictionaryBundleSource& source;
  View view;
  DictionaryIndexValidation index;
  std::array<uint64_t, 4> lengths{};
  std::span<uint8_t> scratch;
  tinfl_decompressor* decoder;
  std::span<uint8_t> window;
  Progress progress;
  void* context;
  [[gnu::noinline]] bool info(DictionaryInfoDetails& result) {
    view.member = 2;
    DictionaryInfoValidation validation(view, scratch, progress, context);
    return validation.validate(result);
  }
  [[gnu::noinline]] bool definitions(uint64_t& bytes) {
    DictzipLayout layout;
    DictzipValidation validation(view, *decoder, window, scratch, progress, context);
    if (!validation.validate(layout)) return false;
    bytes = layout.expandedBytes;
    return true;
  }
  [[gnu::noinline]] bool readDefinitions() {
    for (uint64_t at = 0; at < lengths[0];) {
      const auto count = static_cast<size_t>(std::min<uint64_t>(scratch.size(), lengths[0] - at));
      if ((progress && !progress(context)) || !source.read(0, at, scratch.first(count))) return false;
      at += count;
    }
    return true;
  }
};
}  // namespace companion
