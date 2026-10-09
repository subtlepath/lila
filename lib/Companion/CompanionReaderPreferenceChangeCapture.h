#pragma once

#include "CompanionReaderPreferenceEncoding.h"
#include "CompanionTintaWriter.h"

namespace companion {
// Retain off-stack. Baseline may contain unavailable legacy selections so users
// can repair them. Preflight every changed key before the first durable event.
class ReaderPreferenceChangeCapture final {
 public:
  using Encoder = size_t (*)(void*, const ReaderPreferenceValues&, uint8_t, std::span<uint8_t>);
  explicit ReaderPreferenceChangeCapture(TintaWriter& writer, PreferenceKnowledgeHeads* initialHeads = nullptr)
      : writer(writer), initialHeads(initialHeads) {}
  bool setInitialKnowledge(PreferenceKnowledgeHeads& heads) {
    if (recorded || (initialized && !ready)) return false;
    initialHeads = &heads;
    return true;
  }
  const ReaderPreferenceValues& baselineValues() const { return baseline; }
  bool initialize(const ReaderPreferenceValues& values) {
    if (initialized) return false;
    baseline = values;
    ready = initialized = true;
    return true;
  }
  TintaJournalResult prepare(const ReaderPreferenceValues& values, Encoder encoder, void* context,
                             uint16_t contentChanges = 0) {
    prepared = false;
    if (!ready) return TintaJournalResult::Unavailable;
    if (!encoder || (contentChanges & ~uint16_t{0x401})) return TintaJournalResult::Invalid;
    return prepareSelected(values, encoder, context,
                           static_cast<uint16_t>(changedKeys(baseline, values) | contentChanges));
  }
  // Import callers select only keys proved absent from audited journal authority.
  TintaJournalResult prepareSelected(const ReaderPreferenceValues& values, Encoder encoder, void* context,
                                     uint16_t mask) {
    prepared = false;
    if (!ready) return TintaJournalResult::Unavailable;
    if (!encoder || (mask & ~uint16_t{0x3fff})) return TintaJournalResult::Invalid;
    for (unsigned slot = 0; slot < bodies.size(); ++slot) {
      if (!(mask & (1U << slot))) continue;
      const auto size = encoder(context, values, static_cast<uint8_t>(slot + 1), bodies[slot]);
      PreferenceBodyView decoded;
      if (!size || size > bodies[slot].size() || !decodePreferenceBody(std::span(bodies[slot]).first(size), decoded) ||
          decoded.key != slot + 1)
        return TintaJournalResult::Invalid;
      lengths[slot] = static_cast<uint8_t>(size);
    }
    preparedValues = values;
    preparedMask = mask;
    prepared = true;
    return TintaJournalResult::Ok;
  }
  bool preparedChanges() const { return prepared && preparedMask; }
  std::span<const uint8_t> preparedBody(uint8_t key) const {
    if (!prepared || key < 1 || key > 14 || !(preparedMask & (uint16_t{1} << (key - 1)))) return {};
    return std::span(bodies[key - 1]).first(lengths[key - 1]);
  }
  bool omitPreparedKey(uint8_t key) {
    if (!prepared || key < 1 || key > 14) return false;
    preparedMask &= ~(uint16_t{1} << (key - 1));
    return true;
  }
  TintaJournalResult persistPrepared(uint32_t day, uint64_t timestamp, ClockQuality quality) {
    if (!ready || !writer.available() || !prepared) return TintaJournalResult::Unavailable;
    prepared = false;
    for (unsigned slot = 0; slot < bodies.size(); ++slot) {
      if (!(preparedMask & (1U << slot))) continue;
      const auto encoded = std::span(bodies[slot]).first(lengths[slot]);
      const auto result = initialHeads
                              ? writer.recordPreferenceResolving(encoded, *initialHeads, day, timestamp, quality)
                              : writer.recordPreference(encoded, day, timestamp, quality);
      if (result != TintaJournalResult::Ok && result != TintaJournalResult::Duplicate) {
        ready = false;
        writer.stop();
        return result;
      }
      recorded = true;
      initialHeads = nullptr;
    }
    baseline = preparedValues;
    return TintaJournalResult::Ok;
  }
  TintaJournalResult persist(const ReaderPreferenceValues& values, Encoder encoder, void* context, uint32_t day,
                             uint64_t timestamp, ClockQuality quality, uint16_t contentChanges = 0) {
    if (!ready || !writer.available()) return TintaJournalResult::Unavailable;
    const auto result = prepare(values, encoder, context, contentChanges);
    return result == TintaJournalResult::Ok ? persistPrepared(day, timestamp, quality) : result;
  }
  static uint16_t changedKeys(const ReaderPreferenceValues& a, const ReaderPreferenceValues& b) {
    uint16_t mask = 0;
    const auto mark = [&](unsigned key, bool changed) {
      if (changed) mask |= uint16_t{1} << (key - 1);
    };
    mark(1, !sameName(a.sdFontFamilyName, b.sdFontFamilyName) ||
                (b.sdFontFamilyName.front() == 0 && a.fontFamily != b.fontFamily) ||
                (b.sdFontFamilyName.front() != 0 && a.fontPointSize != b.fontPointSize));
    mark(2, a.fontPointSize != b.fontPointSize);
    mark(3, a.lineSpacing != b.lineSpacing);
    mark(4, a.paragraphAlignment != b.paragraphAlignment);
    mark(5, a.extraParagraphSpacing != b.extraParagraphSpacing);
    mark(6, a.wordSpacing != b.wordSpacing);
    mark(7, a.characterSpacing != b.characterSpacing);
    mark(8, a.screenMargin != b.screenMargin);
    mark(9, a.hyphenationEnabled != b.hyphenationEnabled);
    mark(10, a.language != b.language);
    mark(11, !sameName(a.dictionaryName, b.dictionaryName));
    mark(12, a.textAntiAliasing != b.textAntiAliasing);
    mark(13, a.embeddedStyle != b.embeddedStyle);
    mark(14, a.focusReadingEnabled != b.focusReadingEnabled);
    return mask;
  }

 private:
  static bool sameName(const std::array<char, 32>& a, const std::array<char, 32>& b) {
    const auto aEnd = std::find(a.begin(), a.end(), '\0');
    const auto bEnd = std::find(b.begin(), b.end(), '\0');
    return aEnd - a.begin() == bEnd - b.begin() && std::equal(a.begin(), aEnd, b.begin());
  }
  TintaWriter& writer;
  PreferenceKnowledgeHeads* initialHeads;
  ReaderPreferenceValues baseline, preparedValues;
  std::array<std::array<uint8_t, 69>, 14> bodies{};
  std::array<uint8_t, 14> lengths{};
  uint16_t preparedMask = 0;
  bool ready = false, initialized = false, prepared = false, recorded = false;
};
}  // namespace companion
