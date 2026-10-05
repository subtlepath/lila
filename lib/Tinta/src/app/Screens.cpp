// One instance of each screen, made when Tinta opens and freed when it closes
// (createScreens(), from App::open()), so a closed Tinta holds no RAM.

#include <new>

#include "app/App.h"
#include "ui/screens/DictionaryScreens.h"
#include "ui/screens/HomeScreen.h"
#include "ui/screens/InfoScreens.h"
#include "ui/screens/LessonScreens.h"
#include "ui/screens/PhraseScreens.h"
#include "ui/screens/ProgressScreen.h"
#include "ui/screens/ReaderScreens.h"
#include "ui/screens/SearchScreen.h"
#include "ui/screens/SessionScreen.h"
#include "ui/screens/SettingsScreens.h"
#include "ui/screens/Sheets.h"
#include "ui/screens/TimeScreens.h"
#include "ui/screens/WelcomeScreens.h"

namespace tinta::app {

struct Screens {
  using Page = ui::SettingsPage::Page;
  explicit Screens(App& a)
      : home(a),
        settings(a),
        study(a, Page::Study),
        display(a, Page::Display),
        time(a, Page::Time),
        about(a),
        diagnostics(a),
        pause(a),
        datePrompt(a),
        datePicker(a),
        setClock(a),
        session(a),
        summary(a),
        progress(a),
        dictionary(a),
        letters(a),
        entry(a),
        verbTable(a),
        packError(a),
        lesson(a),
        course(a),
        readings(a),
        reader(a),
        quiz(a),
        phrasebook(a),
        phrases(a),
        search(a),
        entryActions(a),
        welcome(a),
        keyGuide(a),
        vulgarChoice(a),
        licences(a) {}

  ui::HomeScreen home;
  ui::SettingsScreen settings;
  ui::SettingsPage study;
  ui::SettingsPage display;
  ui::SettingsPage time;
  ui::AboutScreen about;
  ui::DiagnosticsScreen diagnostics;
  ui::PauseSheet pause;
  ui::DatePromptScreen datePrompt;
  ui::DatePickerScreen datePicker;
  ui::SetClockScreen setClock;
  ui::SessionScreen session;
  ui::SummaryScreen summary;
  ui::ProgressScreen progress;
  ui::DictionaryScreen dictionary;
  ui::LetterSheet letters;
  ui::EntryScreen entry;
  ui::VerbTableScreen verbTable;
  ui::PackErrorScreen packError;
  ui::LessonScreen lesson;
  ui::CourseScreen course;
  ui::ReadingsScreen readings;
  ui::ReaderScreen reader;
  ui::QuizScreen quiz;
  ui::PhrasebookScreen phrasebook;
  ui::PhrasesScreen phrases;
  ui::SearchScreen search;
  ui::EntryActionsSheet entryActions;
  ui::WelcomeScreen welcome;
  ui::KeyGuideScreen keyGuide;
  ui::VulgarChoiceScreen vulgarChoice;
  ui::LicencesScreen licences;
};

Screens* createScreens(App& app) { return new (std::nothrow) Screens(app); }

void destroyScreens(Screens* screens) { delete screens; }

// Ids lila has no screen for (sleep settings, the light sheet, firmware
// update, USB transfer, the input test and type specimen) keep their numbers,
// which session.bin stores, and show Home.
View* App::view(const ScreenId id) {
  Screens& s = *screens_;
  switch (id) {
    case ScreenId::Settings:
      return &s.settings;
    case ScreenId::SettingsStudy:
      return &s.study;
    case ScreenId::SettingsDisplay:
      return &s.display;
    case ScreenId::SettingsTime:
      return &s.time;
    case ScreenId::About:
      return &s.about;
    case ScreenId::Diagnostics:
      return &s.diagnostics;
    case ScreenId::Pause:
      return &s.pause;
    case ScreenId::DatePrompt:
      return &s.datePrompt;
    case ScreenId::DatePicker:
      return &s.datePicker;
    case ScreenId::SetClock:
      return &s.setClock;
    case ScreenId::Session:
      return &s.session;
    case ScreenId::Summary:
      return &s.summary;
    case ScreenId::Progress:
      return &s.progress;
    case ScreenId::Dictionary:
      return &s.dictionary;
    case ScreenId::DictLetters:
      return &s.letters;
    case ScreenId::DictEntry:
      return &s.entry;
    case ScreenId::VerbTable:
      return &s.verbTable;
    case ScreenId::PackError:
      return &s.packError;
    case ScreenId::Lesson:
      return &s.lesson;
    case ScreenId::Course:
      return &s.course;
    case ScreenId::Readings:
      return &s.readings;
    case ScreenId::Reader:
      return &s.reader;
    case ScreenId::Quiz:
      return &s.quiz;
    case ScreenId::Phrasebook:
      return &s.phrasebook;
    case ScreenId::Phrases:
      return &s.phrases;
    case ScreenId::Search:
      return &s.search;
    case ScreenId::EntryActions:
      return &s.entryActions;
    case ScreenId::Welcome:
      return &s.welcome;
    case ScreenId::KeyGuide:
      return &s.keyGuide;
    case ScreenId::VulgarChoice:
      return &s.vulgarChoice;
    case ScreenId::Licences:
      return &s.licences;
    default:
      break;
  }
  return &s.home;
}

}  // namespace tinta::app
