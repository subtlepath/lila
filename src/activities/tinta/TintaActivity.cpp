#if LILA_TINTA

#include "TintaActivity.h"

#include "CompanionReaderPreferences.h"

// Before lila's I18n.h, whose tr() macro would rewrite Tinta's own ui::tr().
#include <GfxRenderer.h>
#include <HalCourseStateMigration.h>
#include <HalGPIO.h>
#include <HalJournalCausalAuditSession.h>
#include <HalJournalMergeStartupRecovery.h>
#include <HalJournalMigrationPublicationStorage.h>
#include <HalTintaApplicationAcknowledge.h>
#include <HalTintaDerivedStartupRecovery.h>
#include <HalTintaLearnerPreparation.h>
#include <HalTintaMergedJournalReconciliation.h>
#include <HalTintaWriterSession.h>
#include <HalTransferStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "TintaMutationBindings.h"
#include "activities/util/FrontlightPanelActivity.h"
#include "app/App.h"
#include "components/UITheme.h"

namespace {
struct CourseStartup {
  companion::HalTransferStorage storage;
  companion::HalCompanionFileLookup journalLookup;
  std::array<uint8_t, 512> scratch{};
};

uint32_t freeInternal() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
uint32_t largestInternal() { return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

}  // namespace

struct TintaCompanionSession {
  TintaCompanionSession(std::unique_ptr<companion::HalTintaLearnerPreparation> preparation, tinta::app::App& learner,
                        const companion::Identity& course)
      : preparation(std::move(preparation)),
        learner(learner),
        course(course),
        receipts(writer.proofStorage()),
        acknowledgement(writer.authority(), writer.proofStorage(), learner.progress(), receipts, course,
                        this->preparation->storageGeneration(), this->preparation->resource()),
        lessons(learner.pack(), false) {}
  ~TintaCompanionSession() {
    bindings.reset();
    writer.close();
  }
  bool start() {
    companion::EventIdentity undo{};
    if (!preparation->sessionSnapshot() || writer.start(identities) != companion::TintaJournalResult::Ok ||
        !acknowledgement.recoverUndo(undo))
      return false;
    bindings = makeUniqueNoThrow<companion::HalTintaMutationBindings>(
        learner, writer.mutations(), learner.pack(), preparation->subjects(), lessons, course, preparation->resource(),
        undo, this, &recover, &acknowledge, &error);
    if (!bindings) {
      LOG_ERR("TNT", "OOM: companion mutation bindings");
      return false;
    }
    ready = bindings->bind();
    return ready;
  }
  static bool recover(void* context) {
    // Startup proof applies only to the stores already opened before binding.
    error(context, companion::TintaJournalResult::Unavailable);
    return false;
  }
  static bool acknowledge(void* context, const companion::EventIdentity& event, const tinta::core::JournalEntry& entry,
                          const tinta::core::ItemState& before, const tinta::core::ItemState& after,
                          uint32_t milliseconds) {
    auto& owner = *static_cast<TintaCompanionSession*>(context);
    return owner.ready && owner.acknowledgement.acknowledge(event, entry, before, after, milliseconds);
  }
  static void error(void* context, companion::TintaJournalResult result) {
    auto& owner = *static_cast<TintaCompanionSession*>(context);
    owner.ready = false;
    owner.writer.mutations().stop();
    LOG_ERR("TNT", "Companion learner mutation failed: %u", static_cast<unsigned>(result));
  }
  std::unique_ptr<companion::HalTintaLearnerPreparation> preparation;
  tinta::app::App& learner;
  companion::Identity course;
  companion::HalTintaWriterSession writer;
  companion::HalTintaApplicationReceipts receipts;
  companion::HalTintaApplicationAcknowledge acknowledgement;
  companion::TintaPackSubjectKeys lessons;
  companion::HalIdentityStorage identities;
  std::unique_ptr<companion::HalTintaMutationBindings> bindings;
  bool ready = false;
};

TintaActivity::TintaActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Tinta", renderer, mappedInput) {}

TintaActivity::~TintaActivity() = default;

void TintaActivity::onEnter() {
#if LILA_COMPANION
  companion::suspendReaderPreferenceCapture();
#endif
  Activity::onEnter();
  LOG_DBG("TNT", "Before open: free %lu, largest %lu", static_cast<unsigned long>(freeInternal()),
          static_cast<unsigned long>(largestInternal()));
  // Opened before render() can see it: a render may still be on its way.
  companion::Identity course{};
  bool bound = false;
  bool stateReady = false;
  {
    // The storage handles and scratch exceed the task stack budget; release before UI allocation.
    auto startup = makeUniqueNoThrow<CourseStartup>();
    if (startup) {
      stateReady = companion::selectActiveCourseState(startup->storage, startup->scratch, course, bound) &&
                   companion::recoverExistingJournalMigration(startup->journalLookup) &&
                   companion::recoverExistingJournalMerge(startup->journalLookup) &&
                   (!bound || companion::recoverBoundTintaDerivedPublication(course)) &&
                   (!bound || companion::reconcileBoundTintaJournalMerge(course, startup->journalLookup)) &&
                   companion::auditExistingCompanionJournal(startup->journalLookup);
    } else
      LOG_ERR("TNT", "OOM: course startup workspace");
  }
  auto opened = stateReady ? makeUniqueNoThrow<tinta::app::App>(renderer) : nullptr;
  auto preparation = opened && bound ? makeUniqueNoThrow<companion::HalTintaLearnerPreparation>(course) : nullptr;
  const bool prepared =
      !bound || (preparation && opened &&
                 opened->setLearnerPreparation({preparation.get(), [](void* context, tinta::app::App& learner) {
                                                  auto& owner =
                                                      *static_cast<companion::HalTintaLearnerPreparation*>(context);
                                                  if (!owner.run(learner.storage(), learner.profile(), learner.pack()))
                                                    return false;
                                                  const auto* snapshot = owner.sessionSnapshot();
                                                  return !snapshot || learner.setVerifiedLearnerSnapshot(*snapshot);
                                                }}));
  if (!prepared || !opened || !opened->open(bound ? course.data() : nullptr)) {
    LOG_ERR("TNT", "Cannot open tinta app (free %lu, largest %lu)", static_cast<unsigned long>(freeInternal()),
            static_cast<unsigned long>(largestInternal()));
    opened.reset();
  }
  std::unique_ptr<TintaCompanionSession> mutations;
  if (opened && preparation && preparation->sessionSnapshot()) {
    mutations = makeUniqueNoThrow<TintaCompanionSession>(std::move(preparation), *opened, course);
    if (!mutations || !mutations->start()) {
      LOG_ERR("TNT", "Cannot bind companion learner mutations");
      opened->close();
      mutations.reset();
      opened.reset();
    }
  }
  preparation.reset();
  if (opened)
    LOG_DBG("TNT", "Opened: free %lu, largest %lu", static_cast<unsigned long>(freeInternal()),
            static_cast<unsigned long>(largestInternal()));
  {
    RenderLock lock;
    app = std::move(opened);
    companionSession = std::move(mutations);
    failed = !app;
  }
  requestFrame();
}

void TintaActivity::onExit() {
  // lila goes to sleep or elsewhere through here, with the render lock held:
  // Tinta writes what it holds.
  if (app) {
    app->close();
    companionSession.reset();
    app.reset();
  }
#if LILA_COMPANION
  companion::resumeReaderPreferenceCapture();
#endif
  LOG_DBG("TNT", "Closed: free %lu, largest %lu, render stack low water %u", static_cast<unsigned long>(freeInternal()),
          static_cast<unsigned long>(largestInternal()), renderStackLow);
  Activity::onExit();
}

bool TintaActivity::prepareForBackground(const RenderLock&) {
  if (!app) return true;
  if ((companionSession && !companionSession->ready) || !app->flush()) {
    LOG_ERR("TNT", "Cannot leave Tinta: learner persistence failed");
    saveError = true;
    return false;
  }
  return true;
}

void TintaActivity::requestFrame() {
  frameRequested.store(true);
  requestUpdate();
}

void TintaActivity::loop() {
  if (failed) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasBackGesture()) {
      onGoHome();
    }
    return;
  }
  app->board().pollInput();

  RenderLock lock(RenderLock::Mode::Try);
  // The panel is still refreshing: input stays queued for the next pass and
  // is routed against the frame that is going up.
  if (!lock.ownsLock()) return;
  app->update();
  const bool exit = app->exitRequested();
  const tinta::app::App::HostRequest request = app->takeHostRequest();
  const bool frame = app->frameWanted();
  lock.unlock();

  if (exit) {
    onGoHome();
    return;
  }
  if (request == tinta::app::App::HostRequest::Light) {
    auto panel = makeUniqueNoThrow<FrontlightPanelActivity>(renderer, mappedInput);
    if (panel) {
      startActivityForResult(std::move(panel), [this](const ActivityResult&) { requestUpdate(); });
      return;
    }
    LOG_ERR("TNT", "OOM: light panel");
  }
  if (frame) requestFrame();
}

void TintaActivity::render(RenderLock&&) {
  if (!app) {
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_TINTA_OPEN_FAILED));
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    return;
  }
  app->renderFrame(!frameRequested.exchange(false));
  if (saveError) {
    saveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
    renderer.displayBuffer();
  }
  const unsigned low = uxTaskGetStackHighWaterMark(nullptr);
  if (low < renderStackLow) renderStackLow = low;
}

bool TintaActivity::drawSleepFrame() { return app && app->drawSleepCard(); }

bool TintaActivity::handleHomeGesture() {
  if (!app || !gpio.hasHomeKey()) return false;
  tinta::platform::RawInput event{};
  event.kind = tinta::platform::RawInput::Kind::HomeTap;
  app->board().send(event);
  return true;
}

#endif  // LILA_TINTA
