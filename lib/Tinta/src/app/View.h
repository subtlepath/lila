#pragma once

// One screen of the app. App owns the stack, the chrome (status bar, key-hint
// footer or choice bar) and the refresh policy; a View draws its body,
// answers actions and may take raw input first.
//
// To add a screen: add a ScreenId, derive from View, and return the instance
// from App::view() in app/Screens.cpp.

#include <FreeInkApp.h>
#include <stdint.h>

#include "ui/KeyMap.h"

namespace tinta::app {

class App;

inline constexpr size_t kMaxInteractions = 40;
// Actions are dispatched to the active View, not through FreeInkApp handlers.
using UiApp = freeink::ui::FreeInkApp<kMaxInteractions, 1>;
using UiScreen = UiApp::ScreenType;
using freeink::ui::ActionEvent;
using freeink::ui::ActionId;

// Actions every screen shares; a view numbers its own from kFirstViewAction.
enum : ActionId {
  kActionBack = 1,    // the status bar's back arrow (touch)
  kActionChoice = 2,  // a choice-bar answer; value = cell index
  kFirstViewAction = 16,
};

// session.bin and the usage log store these numbers, so an id is never
// removed or reordered. Reserved ids are the standalone firmware's screens,
// which lila has no use for; App::view() shows Home for them.
enum class ScreenId : uint8_t {
  None,
  Home,
  Settings,
  SettingsStudy,
  SettingsDisplay,
  SettingsTime,
  SettingsSleep,  // reserved
  About,
  Diagnostics,
  Specimen,  // reserved
  BringUp,   // reserved
  Pause,
  Light,
  DatePrompt,
  DatePicker,
  SetClock,  // reserved
  Session,
  Summary,
  Progress,
  Dictionary,
  DictLetters,
  DictEntry,
  VerbTable,
  PackError,
  Lesson,
  Course,
  Update,       // reserved
  UsbTransfer,  // reserved
  Readings,
  Reader,
  Quiz,
  Phrasebook,
  Phrases,
  Search,
  EntryActions,
  Welcome,
  KeyGuide,
  VulgarChoice,
  Licences,
  Count,
};

class View {
 public:
  enum class Kind : uint8_t {
    Normal,      // status bar, body, footer
    Overlay,     // a sheet drawn over the screen below it
    FullScreen,  // draws everything itself
  };

  explicit View(App& app) : app_(app) {}
  virtual ~View() = default;

  // Logged as "[tinta] screen <name>" on entry; flows wait for it.
  virtual const char* name() const = 0;
  // The status bar's title.
  virtual const char* title() const { return nullptr; }
  virtual Kind kind() const { return Kind::Normal; }

  // Pushed (returning = false) or uncovered by a pop (returning = true).
  virtual void enter(bool returning) { (void)returning; }
  virtual void leave() {}

  // Draws the body into what the chrome left of the screen.
  virtual void build(UiScreen& screen) = 0;
  virtual void onAction(const ActionEvent& event) { (void)event; }
  // Raw input before any mapping; true when consumed.
  virtual bool onInput(const ui::InputEvent& event) {
    (void)event;
    return false;
  }
  // Back, from the key or the status bar arrow. Default: pop.
  virtual void onBack();

  // Non-null while the front keys answer rather than navigate.
  virtual const ui::ChoiceBar* choiceBar() const { return nullptr; }
  // Key devices: the labels over the front keys when there is no choice
  // bar. False for no footer.
  virtual bool keyHints(ui::ChoiceBar& out) const;

  // Key devices: the focus stop to start on when the screen appears (0 = the
  // first focusable element, -1 = none). Views that remember their last
  // focus return it here, so Back lands where the learner left.
  virtual int8_t focusOrdinal() const { return 0; }

  // Whether the pause sheet, Home and the light sheet may open over it.
  virtual bool allowsGlobalGestures() const { return true; }
  // While true the device stays on: no idle sleep, and Power does nothing
  // (a firmware update writing, the card handed to a computer).
  virtual bool keepAwake() const { return false; }
  // Called from every pass of the loop while the view is on top (after
  // input): for views that poll something. Keep it cheap.
  virtual void tick() {}
  // Whether waking from sleep may return to this screen.
  virtual bool restorable() const { return true; }

 protected:
  App& app_;
};

}  // namespace tinta::app
