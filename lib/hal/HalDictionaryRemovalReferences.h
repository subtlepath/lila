#pragma once

#include "CompanionDictionaryRemovalParticipant.h"
#include "HalCompanionHeapAdmission.h"
#include "HalDictionaryBindings.h"
#include "HalFileName.h"

namespace companion {
class DictionaryRemovalSettings {
 public:
  virtual ~DictionaryRemovalSettings() = default;
  virtual bool load() = 0;
  virtual bool selectedDirectory(std::string_view& output) const = 0;
  virtual bool clearAndSave() = 0;
  virtual bool save() = 0;
};
// Borrow exclusive settings/binding owners until journal recovery completes.
// Fixed state remains off stack; no allocations or cache deletion occur here.
class HalDictionaryRemovalReferences final : public DictionaryRemovalReferences {
 public:
  HalDictionaryRemovalReferences(ContentRemovalJournal& journal, DictionaryRemovalSettings& settings,
                                 HalDictionaryBindings& bindings)
      : journal(journal), settings(settings), bindings(bindings) {}
  bool verifyPlan(const ContentRemovalRecord& record, const DictionaryRemovalPlan& plan) override {
    bool selected = false;
    if (record.phase != ContentRemovalPhase::Prepared || !select(record, plan, true) || !matches(selected))
      return fail("plan settings");
    const auto presence = bindings.read(plan.installed.base.data(), binding);
    return (guard() && ((presence == DictionaryBindingResult::Found && binding == plan.installed.archives &&
                         bindings.verifyFinalized(plan.installed.base.data(), binding) && guard()) ||
                        (presence == DictionaryBindingResult::Missing && canonical(plan)))) ||
           fail("plan binding");
  }
  bool publish(const ContentRemovalRecord& record, const DictionaryRemovalPlan& plan) override {
    bool selected = false;
    if (record.phase != ContentRemovalPhase::Quarantined || !select(record, plan) || !matches(selected))
      return fail("publication settings");
    if (settingsSaved && !selected) return guard();
    const bool saved = selected ? settings.clearAndSave() : settings.save();
    settingsSaved = saved && guard();
    return settingsSaved || fail("settings persistence");
  }
  bool verify(const ContentRemovalRecord& record, const DictionaryRemovalPlan& plan) override {
    bool selected = false;
    if ((record.phase != ContentRemovalPhase::Quarantined && record.phase != ContentRemovalPhase::Committed) ||
        !select(record, plan) || !matches(selected) || selected)
      return fail("published settings");
    const auto presence = bindings.read(plan.installed.base.data(), binding);
    return (guard() && ((presence == DictionaryBindingResult::Found && binding == plan.installed.archives) ||
                        (presence == DictionaryBindingResult::Missing &&
                         (record.phase == ContentRemovalPhase::Committed || canonical(plan))))) ||
           fail("published binding");
  }
  bool retire(const ContentRemovalRecord& record, const DictionaryRemovalPlan& plan) override {
    return record.phase == ContentRemovalPhase::Committed && verify(record, plan) && guard() &&
           bindings.retireRemoval(journal, plan, record.planHash) && guard();
  }
  bool verifyRetired(const ContentRemovalRecord& record, const DictionaryRemovalPlan& plan) override {
    bool selected = false;
    return (record.phase >= ContentRemovalPhase::Committed && select(record, plan) && matches(selected) && !selected &&
            guard() && bindings.verifyRemovalRetired(journal, plan, record.planHash) && guard()) ||
           fail("retired references");
  }

 private:
  ContentRemovalJournal& journal;
  DictionaryRemovalSettings& settings;
  HalDictionaryBindings& bindings;
  ContentRemovalRecord checkpoint;
  DictionaryArchiveBinding binding;
  std::string_view directory;
  bool preflight = false, settingsSaved = false;
  static bool canonical(const DictionaryRemovalPlan& plan) {
    return plan.installed.archives.original == plan.installed.archives.members;
  }
  bool guard() const {
    return (preflight ? !journal.current() : journal.current() && *journal.current() == checkpoint) &&
           admitCompanionHeap();
  }
  bool select(const ContentRemovalRecord& record, const DictionaryRemovalPlan& plan, bool allowPreflight = false) {
    if (!validContentRemovalRecord(record) || !validDictionaryRemovalPlan(plan) || record.request != plan.request)
      return false;
    const std::string_view base(plan.installed.base.data());
    const auto relative = base.substr(base[1] == '.' ? 15 : 14);
    directory = relative.substr(0, relative.find('/'));
    if (checkpoint != record) settingsSaved = false;
    checkpoint = record;
    preflight = allowPreflight && !journal.current();
    return guard();
  }
  bool matches(bool& selected) const {
    std::string_view name;
    if (!settings.selectedDirectory(name)) return false;
    selected = false;
    if (name.empty()) return true;
    const auto comparison = HalFileName::compare(directory, name);
    selected = comparison == FileNameComparison::Equal;
    return comparison != FileNameComparison::Invalid;
  }
  static bool fail(const char* operation) {
    LOG_ERR("COMPANION", "Dictionary reference removal %s failed", operation);
    return false;
  }
};
}  // namespace companion
