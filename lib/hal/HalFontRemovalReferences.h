#pragma once

#include <Logging.h>

#include <cstring>

#include "HalCompanionHeapAdmission.h"
#include "HalFileName.h"
#include "HalSingleFileRemovalParticipant.h"

namespace companion {
class FontRemovalSettings {
 public:
  virtual ~FontRemovalSettings() = default;
  virtual bool load() = 0;
  virtual bool selectedFamily(std::string_view& output) const = 0;
  virtual bool clearAndSave() = 0;
  virtual bool save() = 0;
};
// Store writers remain suspended until the removal journal finishes recovery.
class HalFontRemovalReferences final : public SingleFileRemovalReferences {
 public:
  HalFontRemovalReferences(ContentRemovalJournal& journal, FontRemovalSettings& settings)
      : journal(journal), settings(settings) {}
  bool verifyPlan(const ContentRemovalRecord& record, const char* path) override {
    bool selected = false;
    return (record.phase == ContentRemovalPhase::Prepared && select(record, path, true) && matches(selected) &&
            guard()) ||
           failure("plan settings");
  }
  bool publish(const ContentRemovalRecord& record, const char* path) override {
    bool selected = false;
    if (record.phase != ContentRemovalPhase::Quarantined || !select(record, path) || !matches(selected))
      return failure("publication context");
    if (settingsSaved && !selected) return guard();
    // Retry persistence even when a failed save already cleared the in-memory selection.
    const bool saved = selected ? settings.clearAndSave() : settings.save();
    settingsSaved = saved && guard();
    return settingsSaved || failure("settings persistence");
  }
  bool verify(const ContentRemovalRecord& record, const char* path) override {
    bool selected = false;
    return ((record.phase == ContentRemovalPhase::Quarantined || record.phase == ContentRemovalPhase::Committed) &&
            select(record, path) && matches(selected) && !selected && guard()) ||
           failure("published settings");
  }
  bool retire(const ContentRemovalRecord& record, const char* path) override {
    return record.phase == ContentRemovalPhase::Committed && verify(record, path);
  }
  bool verifyRetired(const ContentRemovalRecord& record, const char* path) override {
    bool selected = false;
    return (record.phase >= ContentRemovalPhase::Committed && select(record, path) && matches(selected) && !selected &&
            guard()) ||
           failure("retired settings");
  }

 private:
  ContentRemovalJournal& journal;
  FontRemovalSettings& settings;
  ContentRemovalRecord checkpoint;
  std::string_view family;
  bool preflight = false, settingsSaved = false;
  bool guard() const {
    return (preflight ? !journal.current() : journal.current() && *journal.current() == checkpoint) &&
           admitCompanionHeap();
  }
  bool select(const ContentRemovalRecord& record, const char* path, bool allowPreflight = false) {
    if (!path || record.request.manifest.kind != ContentKind::Font) return false;
    const size_t length = strnlen(path, INVENTORY_PATH_LIMIT + 1);
    const std::string_view view(path, length);
    if (!validSingleFileRemovalPlan({record.request, view})) return false;
    const auto relative = view.substr(view[1] == '.' ? 8 : 7);
    const auto slash = relative.find('/');
    if (slash == std::string_view::npos) {
      if (record.request.manifest.formatVersion != 1) return false;
      family = relative.substr(0, relative.size() - 4);
    } else {
      if (relative.find('/', slash + 1) != std::string_view::npos) return false;
      family = relative.substr(0, slash);
    }
    if (family.empty() || family.front() == '.' || family.front() == '_') return false;
    if (checkpoint != record) settingsSaved = false;
    checkpoint = record;
    preflight = allowPreflight && !journal.current();
    return guard();
  }
  bool matches(bool& selected) const {
    std::string_view name;
    if (!settings.selectedFamily(name)) return false;
    selected = false;
    if (name.empty()) return true;
    const auto comparison = HalFileName::compare(family, name);
    selected = comparison == FileNameComparison::Equal;
    return comparison != FileNameComparison::Invalid;
  }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Font reference removal failed: %s", reason);
    return false;
  }
};
}  // namespace companion
