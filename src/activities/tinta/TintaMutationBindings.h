#pragma once

#include <Logging.h>

#include "CompanionTintaLessonJournal.h"
#include "HalTintaMarkMutationContext.h"
#include "HalTintaProfileMutationContext.h"
#include "HalTintaProgressMutationContext.h"
#include "app/App.h"

namespace companion {
// Checked off-stack owner; App, writer, validated pack/catalog/keys outlive it.
// Keep bound through App::close(), then unbind before destroying App.
class HalTintaMutationBindings {
 public:
  using ApplicationAcknowledge = bool (*)(void*, const EventIdentity&, const tinta::core::JournalEntry&,
                                          const tinta::core::ItemState&, const tinta::core::ItemState&, uint32_t);
  HalTintaMutationBindings(tinta::app::App& app, TintaWriter& writer, const tinta::core::pack::Pack& pack,
                           TintaSubjectCatalog& catalog, IdentityKeys& lessons, const Identity& course,
                           const Digest& resource, const EventIdentity& recoveredUndo, void* recoveryContext,
                           bool (*recover)(void*), ApplicationAcknowledge acknowledge,
                           void (*reportError)(void*, TintaJournalResult))
      : app(app),
        writer(writer),
        recoveryContext(recoveryContext),
        recover(recover),
        acknowledge(acknowledge),
        reportError(reportError),
        progress(writer, course, resource, recoveredUndo),
        progressContext(app.profile(), app.clock()),
        profileContext(writer, app.clock(), app.progress().lastStudyDay(), this, &errorCallback),
        starContext(pack, catalog, app.clock(), EventKind::Star, this, &recoverCallback, &errorCallback),
        readingContext(pack, catalog, app.clock(), EventKind::ReadingComplete, this, &recoverCallback, &errorCallback),
        stars(writer, course, resource, EventKind::Star, starContext.callbacks()),
        readings(writer, course, resource, EventKind::ReadingComplete, readingContext.callbacks()),
        lessonJournal(writer, lessons, catalog, course, resource,
                      {this, &clockCallback, &recoverCallback, &errorCallback}) {}
  ~HalTintaMutationBindings() { unbind(); }
  HalTintaMutationBindings(const HalTintaMutationBindings&) = delete;
  HalTintaMutationBindings& operator=(const HalTintaMutationBindings&) = delete;

  bool bind() {
    if (bound || !writer.available() || !recover || !acknowledge || !reportError) {
      LOG_ERR("COMPANION", "Tinta mutation bindings unavailable");
      return false;
    }
    if (!profileContext.initialize(app.profile()) ||
        !app.setProfileMutationJournal({&profileContext, HalTintaProfileMutationContext::callback})) {
      LOG_ERR("COMPANION", "Tinta profile mutation binding unavailable");
      return false;
    }
    progressBinding = progressContext.binding(progress, this, &errorCallback);
    app.progress().setMutationJournal({this, &progressCallback, &recoverCallback, &committedCallback});
    app.starred().setMutationJournal(stars.binding());
    app.readLog().setMutationJournal(readings.binding());
    app.setLessonMutationJournal(lessonJournal.binding());
    bound = true;
    return true;
  }
  void unbind() {
    if (!bound) return;
    app.progress().setMutationJournal({});
    app.starred().setMutationJournal({});
    app.readLog().setMutationJournal({});
    app.setLessonMutationJournal({});
    app.setProfileMutationJournal({});
    progressBinding = {};
    bound = false;
  }

 private:
  static bool progressCallback(void* context, const tinta::core::JournalEntry& entry,
                               const tinta::core::ItemState& before, const tinta::core::ItemState& after,
                               uint32_t milliseconds) {
    auto& owner = *static_cast<HalTintaMutationBindings*>(context);
    return owner.bound && owner.progressBinding.persist &&
           owner.progressBinding.persist(owner.progressBinding.context, entry, before, after, milliseconds);
  }
  static bool committedCallback(void* context, const tinta::core::JournalEntry& entry,
                                const tinta::core::ItemState& before, const tinta::core::ItemState& after,
                                uint32_t milliseconds) {
    auto& owner = *static_cast<HalTintaMutationBindings*>(context);
    if (!owner.bound) return false;
    if (!owner.progress.pendingApplication()) return true;
    if (!owner.app.progress().verifyCommittedMutation(entry, before, after) || !owner.acknowledge ||
        !owner.acknowledge(owner.recoveryContext, owner.progress.applicationIdentity(), entry, before, after,
                           milliseconds)) {
      owner.writer.stop();
      errorCallback(context, TintaJournalResult::IoError);
      return false;
    }
    owner.progress.didApply();
    return true;
  }
  static bool clockCallback(void* context, uint32_t& day, uint64_t& timestamp, ClockQuality& quality) {
    return readTintaMutationClock(static_cast<HalTintaMutationBindings*>(context)->app.clock(), day, timestamp,
                                  quality);
  }
  static bool recoverCallback(void* context) {
    auto& owner = *static_cast<HalTintaMutationBindings*>(context);
    if (owner.recover && owner.recover(owner.recoveryContext)) return true;
    errorCallback(context, TintaJournalResult::IoError);
    return false;
  }
  static void errorCallback(void* context, TintaJournalResult result) {
    auto& owner = *static_cast<HalTintaMutationBindings*>(context);
    LOG_ERR("COMPANION", "Tinta authoritative mutation failed: %u", static_cast<unsigned>(result));
    if (owner.reportError) owner.reportError(owner.recoveryContext, result);
  }
  tinta::app::App& app;
  TintaWriter& writer;
  void* recoveryContext;
  bool (*recover)(void*);
  ApplicationAcknowledge acknowledge;
  void (*reportError)(void*, TintaJournalResult);
  TintaProgressJournal progress;
  HalTintaProgressMutationContext progressContext;
  HalTintaProfileMutationContext profileContext;
  HalTintaMarkMutationContext starContext, readingContext;
  TintaMarkJournal stars, readings;
  TintaLessonJournal lessonJournal;
  tinta::core::ProgressStore::MutationJournal progressBinding;
  bool bound = false;
};
}  // namespace companion
