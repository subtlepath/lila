#pragma once

// Every chrome string, in English and Spanish (PLAN.md 4.1: chrome starts in
// English; a setting switches it). Strings are in the font charset: Latin-1
// letters as UTF-8, typographic punctuation through the TINTA_* slot macros
// (core/lang/Charset.h). Entries ending in "Fmt" are printf formats.
//
// To add a string: add an enumerator here and a row in Strings.cpp, in the
// same position (a static_assert checks the count).

#include <stddef.h>
#include <stdint.h>

#include "core/Clock.h"
#include "core/profile/Profile.h"

namespace tinta::ui {

enum class Str : uint16_t {
  AppName,
  Home,
  Settings,
  Back,
  Select,
  Open,
  Change,
  Resume,
  Done,
  Save,
  Cancel,
  On,
  Off,
  Paused,
  Light,
  // Storage
  GuestBanner,
  CardFailedBanner,
  // Settings groups
  Study,
  Display,
  DateAndTime,
  About,
  Diagnostics,
  // Study
  NewPerDay,
  ReviewCap,
  Retention,
  MaxInterval,
  SessionSize,
  ShowVulgar,
  ItemsFmt,
  DaysFmt,
  PercentFmt,
  // Display
  TextSize,
  TextSmall,
  TextMedium,
  TextLarge,
  UiLanguage,
  LangEnglish,
  LangSpanish,
  FullRefreshEvery,
  ScreensFmt,
  // Time
  TodaysDate,
  DayStartsAt,
  Year,
  Month,
  Day,
  SetDateTitle,
  // Date prompt
  WhatDayIsIt,
  SameDay,
  NextDay,
  OtherDate,
  LastTimeFmt,
  // First run
  Welcome,
  FirstRunDate,
  // About and diagnostics
  Version,
  Device,
  Panel,
  StorageInfo,
  StorageReady,
  StorageAbsent,
  StorageFailed,
  ClockSource,
  ClockRtc,
  ClockAsked,
  CourseLesson,
  CourseUnlocked,
  BoardProfile,
  // Course pack (About, Diagnostics, the error screen)
  Course,
  CourseEditionFmt,
  CourseItemsFmt,
  CheckCourse,
  CourseIntact,
  CourseDamaged,
  PackErrorTitle,
  PackErrorFmt,
  // Home: Today
  Today,
  TodayCountsFmt,
  TodayLeftFmt,
  NothingDue,
  StreakFmt,
  StreakOne,
  OneDay,
  Start,
  Continue,
  Dictionary,
  Progress,
  // Session
  ReviewFmt,
  Again,
  Hard,
  Good,
  Easy,
  ThisSession,
  DaysShortFmt,
  MonthsShortFmt,
  YearsShortFmt,
  EndSession,
  SessionDone,
  Reviewed,
  Correct,
  NewItems,
  TimeSpent,
  Streak,
  LeechTitle,
  LeechFmt,
  Suspend,
  Keep,
  PromptProduce,
  PromptGender,
  PromptCloze,
  PromptOrder,
  PromptPhrase,
  PromptConjugate,
  PromptMeaning,
  Right,
  TheAnswerFmt,
  RightAccents,
  RightTypo,
  RightAlternativeFmt,
  YouTypedFmt,
  GoOnKeys,
  GoOnTouch,
  OneMistake,
  MistakesFmt,
  Place,
  TypeHere,
  Check,
  TypedAnswers,
  // Parts of speech, PartOfSpeech order from Noun (1)
  PosNoun,
  PosVerb,
  PosAdjective,
  PosAdverb,
  PosPronoun,
  PosDeterminer,
  PosPreposition,
  PosConjunction,
  PosInterjection,
  PosNumeral,
  PosExpression,
  PosProperNoun,
  // Dictionary
  Letters,
  JumpToLetter,
  Conjugation,
  Gerund,
  Participle,
  PageFmt,
  // Progress
  NotStarted,
  Learning,
  Mature,
  Suspended,
  ReviewsLast14,
  DueNext14,
  TimeStudiedFmt,
  StudyDaysFmt,
  // Lessons
  LessonFmt,
  LessonRowFmt,
  LessonProgressFmt,
  CourseMap,
  NewWordFmt,
  English,
  Speaker,
  PracticeTitle,
  PracticeFmt,
  PracticeText,
  LessonDoneFmt,
  NextLessonFmt,
  CourseFinished,
  Locked,
  UnlockAll,
  UnitFmt,
  InThisLesson,
  NewWordsFmt,
  DialogueLabel,
  NoteGrammar,
  NoteCulture,
  NotePronunciation,
  NoteUsage,
  StartPractice,
  LessonNow,
  Next,
  NextLessonRow,
  // Key names
  KeyBack,
  // M6
  SleepRemember,
  SleepNewWord,
  SleepTomorrowFmt,
  Read,
  Phrases,
  Readings,
  PhrasesProgressFmt,
  PhrasesCountFmt,
  Practise,
  ReaderQuestions,
  ReaderQuestionsFmt,
  ReaderTheEnd,
  ReaderWrong,
  QuestionFmt,
  QuizScoreFmt,
  Gloss,
  Close,
  DeckAdd,
  DeckRemove,
  DeckShortAdd,
  DeckShortRemove,
  DeckShortIn,
  DeckIn,
  DeckLearnt,
  Options,
  Search,
  SearchHint,
  SearchNothing,
  SearchMore,
  DictionaryEnglish,
  JumpToLetterEnglish,
  DeleteShort,
  // M7 interface
  ShowAnswerKeys,
  ShowAnswerTouch,
  LangAutoEnglish,
  LangAutoSpanish,
  WelcomeChoose,
  KeysTitle,
  KeysFront,
  KeysSide,
  KeysHoldBackFmt,
  KeysPower,
  TouchTitle,
  TouchTap,
  TouchCard,
  TouchLight,
  TouchHome,
  VulgarTitle,
  VulgarText,
  Licences,
  LicTimesHelvetica,
  LicLargerType,
  LicSoftware,
  LicMit,
  LicCourseText,
  Leave,  // Home's row back to lila
  Count,
};

// The language the chrome is drawn in: English or Spanish (the app resolves
// the profile's Auto before it gets here).
void setLanguage(core::UiLanguage language);
core::UiLanguage language();

const char* tr(Str id);
// In a given language whatever the setting (the first-run page, which shows
// both).
const char* trIn(Str id, core::UiLanguage language);

// "Jan".."Dec" / "ene".."dic", and full names.
const char* monthShort(uint8_t month);
const char* monthName(uint8_t month);
// 0 = Monday.
const char* weekdayName(uint8_t weekday);

enum class DateStyle : uint8_t {
  Long,   // Monday 5 October 2026 / lunes 5 de octubre de 2026
  Short,  // Mon 5 Oct / lun 5 oct
};
void formatDate(core::DayNumber day, DateStyle style, char* out, size_t cap);

}  // namespace tinta::ui
