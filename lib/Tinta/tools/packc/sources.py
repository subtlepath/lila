"""Loaders for the course sources (PLAN.md 7.1, docs/content-style.md)."""

from __future__ import annotations

import glob
import os
import re
from dataclasses import dataclass, field

from . import yamlio
from .diag import Diagnostics, Where

POS = {"noun": 1, "verb": 2, "adj": 3, "adv": 4, "pron": 5, "det": 6, "prep": 7, "conj": 8,
       "interj": 9, "num": 10, "expr": 11, "propn": 12}
POS_NAMES = {v: k for k, v in POS.items()}
GENDERS = {"": (0, False), "m": (1, False), "f": (2, False), "mf": (3, False),
           "mpl": (1, True), "fpl": (2, True)}
LEVELS = {"A1": 1, "A2": 2, "B1": 3, "B2": 4}
REGISTERS = {"neutral": 0, "formal": 1, "informal": 2, "vulgar": 3}
NOTE_KINDS = {"grammar": 0, "culture": 1, "pronunciation": 2, "usage": 3}
STORY_KINDS = {"dialogue": 0, "reading": 1}

LEXICON_COLUMNS = ("lemma", "key", "pos", "gender", "gloss", "level", "register", "mx", "topic",
                   "forms", "alt", "note", "pron", "freq", "syn", "link")
REQUIRED_COLUMNS = ("lemma", "pos", "gloss", "level")
# lexicon/dictionary*.tsv: headwords for lookup only (no items, no examples).
DICTIONARY_COLUMNS = ("lemma", "key", "pos", "gender", "gloss", "level", "register", "mx", "forms", "note", "pron")
FORM_OVERRIDES = ("pl", "f", "fpl", "mpl", "apoc")


@dataclass
class LexEntry:
    key: str
    lemma: str
    pos: str
    gender: str
    gloss: str
    level: str
    register: str
    mx: bool
    topic: str
    forms: dict[str, str]
    alt: str
    note: str
    pron: str
    freq: int
    deck: bool          # from lexicon/frequency*.tsv: gets frequency-deck items
    where: Where
    synonyms: list[str]  # `syn` column: lemma keys never used as each other's distractors
    row: dict[str, str]  # raw columns, for review hashes
    dictionary: bool = False  # from lexicon/dictionary*.tsv: lookup only
    marked_only: bool = False  # `link: marked`: sentences link to it only where marked


@dataclass
class SentenceSrc:
    es: str             # with markup
    en: str
    where: Where
    note: str = ""
    order: bool = False
    allow: list[str] = field(default_factory=list)
    id: str = ""
    speaker: str = ""
    paragraph: bool = False
    pron: str = ""
    reviewer_notes: list[str] = field(default_factory=list)  # for the review page only


@dataclass
class NoteSrc:
    kind: str
    title: str
    text: str
    where: Where
    allow: list[str] = field(default_factory=list)   # words exempt from the Peninsular lint


@dataclass
class StorySrc:
    name: str
    title: str
    title_en: str
    level: str
    kind: str
    lines: list[SentenceSrc]
    questions: list[dict]
    where: Where
    lesson_ref: str = ""
    reviewed: dict | None = None
    data: dict | None = None


@dataclass
class LessonSrc:
    unit_number: int
    number: int
    title: str
    title_en: str
    level: str
    new: list[tuple[str, Where]]
    conjugate: list[tuple[str, Where]]
    notes: list[NoteSrc]
    sentences: list[SentenceSrc]
    dialogue: StorySrc | None
    reviewed: dict | None
    where: Where
    data: dict

    @property
    def ref(self) -> str:
        return f"u{self.unit_number:02d}.l{self.number:02d}"


@dataclass
class UnitSrc:
    number: int
    title: str
    title_en: str
    goals: list[str]
    lessons: list[LessonSrc]
    where: Where
    data: dict


@dataclass
class PhraseCategorySrc:
    name: str
    order: int
    title: str
    title_en: str
    phrases: list[SentenceSrc]
    reviewed: dict | None
    where: Where
    data: dict


@dataclass
class DeckSrc:
    """content/deck/NAME.yaml: example sentences for frequency-deck words, in no lesson."""
    name: str
    title: str
    sentences: list[SentenceSrc]
    reviewed: dict | None
    where: Where
    data: dict


@dataclass
class ConfusableSrc:
    name: str
    lemmas: list[str]
    note: str
    where: Where


@dataclass
class CourseSrc:
    root: str
    title: str = "Tinta"
    version: int = 0
    locale: str = "es-MX"
    units: list[UnitSrc] = field(default_factory=list)
    lexicon: dict[str, LexEntry] = field(default_factory=dict)
    stories: list[StorySrc] = field(default_factory=list)
    phrasebook: list[PhraseCategorySrc] = field(default_factory=list)
    deck: list[DeckSrc] = field(default_factory=list)
    lexicon_reviews: dict = field(default_factory=dict)  # dictionary file path -> its `# reviewed:` record
    confusables: list[ConfusableSrc] = field(default_factory=list)
    irregular_path: str = ""
    pronunciation_path: str = ""
    # The shared file first, then per-unit fragments (verbs/irregular/*.yaml ...).
    irregular_paths: list[str] = field(default_factory=list)
    pronunciation_paths: list[str] = field(default_factory=list)
    ids_path: str = ""

    def lessons(self) -> list[LessonSrc]:
        return [lesson for unit in self.units for lesson in unit.lessons]


# --- helpers -----------------------------------------------------------------

def _check_keys(data, allowed: tuple[str, ...], where: Where, diag: Diagnostics,
                required: tuple[str, ...] = ()) -> bool:
    if not isinstance(data, dict):
        diag.error(where, "schema", f"expected a mapping, found {type(data).__name__}")
        return False
    for key in data:
        if key not in allowed:
            diag.error(where.at(where.what, getattr(data, "line", where.line)), "schema",
                       f"unknown field '{key}' (allowed: {', '.join(allowed)})")
    ok = True
    for key in required:
        if data.get(key) in (None, ""):
            diag.error(where.at(where.what, getattr(data, "line", where.line)), "schema",
                       f"missing required field '{key}'")
            ok = False
    return ok


def _text(data: dict, key: str) -> str:
    value = data.get(key)
    if value is None:
        return ""
    return str(value).strip() if not isinstance(value, str) else value.strip()


def _load_yaml(path: str, diag: Diagnostics):
    try:
        return yamlio.load(path)
    except yamlio.YamlError as exc:
        diag.error(Where(path, exc.line), "yaml", str(exc).split(": ", 1)[-1])
    except OSError as exc:
        diag.error(Where(path), "io", str(exc))
    return None


def _line(data, default: int) -> int:
    return getattr(data, "line", default) or default


# --- sentences, notes, stories -------------------------------------------------

SENTENCE_FIELDS = ("es", "en", "note", "order", "allow", "id", "reviewer_notes")
LINE_FIELDS = ("speaker", "es", "en", "note", "allow", "paragraph", "reviewer_notes")
PHRASE_FIELDS = ("es", "en", "note", "pron", "allow", "id", "reviewer_notes")


def _strings(value, where: Where, name: str, diag: Diagnostics) -> list[str]:
    """A string or a list of strings."""
    if value is None:
        return []
    if isinstance(value, (str, int)):
        return [str(value)]
    if isinstance(value, list) and all(isinstance(v, (str, int)) for v in value):
        return [str(v) for v in value]
    diag.error(where, "schema", f"'{name}' must be a string or a list of strings")
    return []


def _sentence(data, fields: tuple[str, ...], where: Where, diag: Diagnostics) -> SentenceSrc | None:
    where = where.at(where.what, _line(data, where.line))
    if not _check_keys(data, fields, where, diag, required=("es", "en")):
        return None
    return SentenceSrc(
        es=_text(data, "es"), en=_text(data, "en"), where=where, note=_text(data, "note"),
        order=bool(data.get("order", False)), allow=_strings(data.get("allow"), where, "allow", diag),
        id=_text(data, "id"), speaker=_text(data, "speaker"), paragraph=bool(data.get("paragraph", False)),
        pron=_text(data, "pron"), reviewer_notes=_strings(data.get("reviewer_notes"), where, "reviewer_notes", diag))


def _note(data, where: Where, diag: Diagnostics) -> NoteSrc | None:
    where = where.at(where.what, _line(data, where.line))
    if not _check_keys(data, ("kind", "title", "text", "allow"), where, diag, required=("title", "text")):
        return None
    kind = _text(data, "kind") or "grammar"
    if kind not in NOTE_KINDS:
        diag.error(where, "schema", f"note kind '{kind}' is not one of {', '.join(NOTE_KINDS)}")
    return NoteSrc(kind=kind, title=_text(data, "title"), text=str(data.get("text", "")), where=where,
                   allow=_strings(data.get("allow"), where, "allow", diag))


STORY_FIELDS = ("title", "title_en", "level", "kind", "lesson", "lines", "questions", "reviewed", "reviewer_notes")


def _story(data, name: str, where: Where, diag: Diagnostics, kind_default: str) -> StorySrc | None:
    if not _check_keys(data, STORY_FIELDS, where, diag, required=("title", "lines")):
        return None
    lines = []
    for n, line in enumerate(data.get("lines") or [], 1):
        sentence = _sentence(line, LINE_FIELDS, where.at(f"line {n}"), diag)
        if sentence:
            lines.append(sentence)
    questions = []
    for n, q in enumerate(data.get("questions") or [], 1):
        qwhere = where.at(f"question {n}", _line(q, where.line))
        if _check_keys(q, ("q", "options", "answer", "lang"), qwhere, diag,
                       required=("q", "options")):
            questions.append({"where": qwhere, **q})
    kind = _text(data, "kind") or kind_default
    if kind not in STORY_KINDS:
        diag.error(where, "schema", f"story kind '{kind}' is not one of {', '.join(STORY_KINDS)}")
    return StorySrc(name=name, title=_text(data, "title"), title_en=_text(data, "title_en"),
                    level=_text(data, "level"), kind=kind, lines=lines, questions=questions,
                    where=where, lesson_ref=_text(data, "lesson"), reviewed=data.get("reviewed"),
                    data=data)


# --- top level ---------------------------------------------------------------------

LESSON_FIELDS = ("number", "title", "title_en", "level", "new", "conjugate", "notes", "sentences",
                 "dialogue", "reviewed", "reviewer_notes")


def _lesson(path: str, unit_number: int, diag: Diagnostics) -> LessonSrc | None:
    data = _load_yaml(path, diag)
    where = Where(path, 1)
    if data is None or not _check_keys(data, LESSON_FIELDS, where, diag,
                                       required=("number", "title")):
        return None
    new = []
    for n, key in enumerate(data.get("new") or []):
        new.append((str(key), where.at(f"new[{n}]", _line(data.get("new"), 1))))
    conjugate = []
    for n, spec in enumerate(data.get("conjugate") or []):
        conjugate.append((str(spec), where.at(f"conjugate[{n}]", _line(data.get("conjugate"), 1))))
    notes = [note for n, raw in enumerate(data.get("notes") or [], 1)
             if (note := _note(raw, where.at(f"note {n}"), diag))]
    sentences = [s for n, raw in enumerate(data.get("sentences") or [], 1)
                 if (s := _sentence(raw, SENTENCE_FIELDS, where.at(f"sentence {n}"), diag))]
    dialogue = None
    if data.get("dialogue") is not None:
        raw = data["dialogue"]
        dialogue = _story(raw, "", where.at("dialogue", _line(raw, 1)), diag, "dialogue")
    try:
        number = int(data["number"])
    except (TypeError, ValueError):
        diag.error(where, "schema", "lesson 'number' must be an integer")
        number = 0
    return LessonSrc(unit_number=unit_number, number=number, title=_text(data, "title"),
                     title_en=_text(data, "title_en"), level=_text(data, "level") or "A1",
                     new=new, conjugate=conjugate, notes=notes, sentences=sentences,
                     dialogue=dialogue, reviewed=data.get("reviewed"), where=where, data=data)


def _unit(directory: str, diag: Diagnostics) -> UnitSrc | None:
    path = os.path.join(directory, "unit.yaml")
    where = Where(path, 1)
    data = _load_yaml(path, diag)
    if data is None or not _check_keys(data, ("number", "title", "title_en", "goals"), where, diag,
                                       required=("title",)):
        return None
    try:
        number = int(data.get("number", -1))
    except (TypeError, ValueError):
        number = -1
    if number < 0:
        diag.error(where, "schema", "unit 'number' must be a non-negative integer")
    match = re.match(r"(\d+)-", os.path.basename(directory))
    if match and int(match.group(1)) != number:
        diag.error(where, "schema",
                   f"unit number {number} does not match directory '{os.path.basename(directory)}'")
    lessons = []
    for lesson_path in sorted(glob.glob(os.path.join(directory, "lesson-*.yaml"))):
        lesson = _lesson(lesson_path, number, diag)
        if lesson:
            lessons.append(lesson)
    lessons.sort(key=lambda lesson: lesson.number)
    seen: dict[int, str] = {}
    for lesson in lessons:
        if lesson.number in seen:
            diag.error(lesson.where, "schema",
                       f"lesson number {lesson.number} also used by {seen[lesson.number]}")
        seen[lesson.number] = lesson.where.path
    goals = data.get("goals") or []
    return UnitSrc(number=number, title=_text(data, "title"), title_en=_text(data, "title_en"),
                   goals=[str(g).strip() for g in goals], lessons=lessons, where=where, data=data)


def _lexicon(path: str, course: CourseSrc, diag: Diagnostics) -> None:
    name = os.path.basename(path)
    deck = name.startswith("frequency")
    dictionary = name.startswith("dictionary")
    allowed = DICTIONARY_COLUMNS if dictionary else LEXICON_COLUMNS
    with open(path, encoding="utf-8") as fh:
        rows = fh.read().split("\n")
    header: list[str] | None = None
    for lineno, raw in enumerate(rows, 1):
        if dictionary and raw.startswith("# reviewed:"):
            try:
                course.lexicon_reviews[path] = yamlio.loads(raw[len("# reviewed:"):])
            except Exception:  # noqa: BLE001 (any parse problem is reported the same way)
                diag.error(Where(path, lineno), "schema", "unreadable '# reviewed:' line; use --mark-reviewed")
            continue
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        cells = raw.split("\t")
        where = Where(path, lineno)
        if header is None:
            header = [c.strip() for c in cells]
            for col in header:
                if col not in allowed:
                    diag.error(where, "schema",
                               f"unknown lexicon column '{col}' in {name} (allowed: {', '.join(allowed)})")
            for col in REQUIRED_COLUMNS:
                if col not in header:
                    diag.error(where, "schema", f"lexicon is missing column '{col}'")
            continue
        if len(cells) > len(header):
            diag.error(where, "schema", f"{len(cells)} cells but {len(header)} columns")
            continue
        row = {col: (cells[i].strip() if i < len(cells) else "") for i, col in enumerate(header)}
        lemma = row.get("lemma", "")
        key = row.get("key") or lemma
        where = where.at(f"'{key}'")
        if not lemma:
            diag.error(where, "schema", "empty lemma")
            continue
        if key in course.lexicon:
            diag.error(where, "duplicate",
                       f"lemma key '{key}' already defined at {course.lexicon[key].where}; "
                       "give homographs distinct keys such as 'papa#pope'")
            continue
        forms: dict[str, str] = {}
        for part in filter(None, (p.strip() for p in row.get("forms", "").split(";"))):
            tag, eq, form = part.partition("=")
            if not eq or tag.strip() not in FORM_OVERRIDES:
                diag.error(where, "schema",
                           f"bad forms entry '{part}' (use {', '.join(t + '=...' for t in FORM_OVERRIDES)})")
                continue
            forms[tag.strip()] = form.strip()
        link = row.get("link", "")
        if link not in ("", "marked"):
            diag.error(where, "schema", f"link '{link}' must be empty or 'marked'")
        freq = 0
        if row.get("freq"):
            try:
                freq = int(row["freq"])
            except ValueError:
                diag.error(where, "schema", f"freq '{row['freq']}' is not an integer")
        course.lexicon[key] = LexEntry(
            key=key, lemma=lemma, pos=row.get("pos", ""), gender=row.get("gender", ""),
            gloss=row.get("gloss", ""), level=row.get("level", ""),
            register=row.get("register", "") or "neutral",
            mx=row.get("mx", "").lower() in ("x", "yes", "y", "1", "mx", "true"),
            topic=row.get("topic", ""), forms=forms, alt=row.get("alt", ""),
            note=row.get("note", ""), pron=row.get("pron", ""), freq=freq, deck=deck,
            where=where, row={k: v for k, v in row.items() if v},
            synonyms=[k.strip() for k in row.get("syn", "").split(";") if k.strip()], dictionary=dictionary,
            marked_only=link == "marked")


def fragment_paths(root: str, single: str, directory: str) -> list[str]:
    """A shared data file plus its per-unit fragments, so authors working in
    parallel each write their own file (confusables/u05.yaml) instead of
    editing the shared one."""
    paths = [os.path.join(root, single)] if os.path.exists(os.path.join(root, single)) else []
    return paths + sorted(glob.glob(os.path.join(root, directory, "*.yaml")))


def load_course(root: str, diag: Diagnostics) -> CourseSrc:
    course = CourseSrc(root=root)
    course_path = os.path.join(root, "course.yaml")
    if os.path.exists(course_path):
        data = _load_yaml(course_path, diag)
        if data is not None and _check_keys(data, ("title", "version", "locale"), Where(course_path, 1), diag):
            course.title = _text(data, "title") or course.title
            course.version = int(data.get("version", 0) or 0)
            course.locale = _text(data, "locale") or course.locale
    else:
        diag.error(Where(course_path), "io", "missing course.yaml")

    for path in sorted(glob.glob(os.path.join(root, "lexicon", "*.tsv"))):
        _lexicon(path, course, diag)

    for directory in sorted(glob.glob(os.path.join(root, "units", "*"))):
        if os.path.isdir(directory):
            unit = _unit(directory, diag)
            if unit:
                course.units.append(unit)
    course.units.sort(key=lambda unit: unit.number)

    for path in sorted(glob.glob(os.path.join(root, "stories", "*.yaml"))):
        data = _load_yaml(path, diag)
        if data is not None:
            name = os.path.splitext(os.path.basename(path))[0]
            story = _story(data, name, Where(path, 1), diag, "reading")
            if story:
                course.stories.append(story)

    for path in sorted(glob.glob(os.path.join(root, "phrasebook", "*.yaml"))):
        data = _load_yaml(path, diag)
        where = Where(path, 1)
        if data is None or not _check_keys(data, ("title", "title_en", "order", "phrases", "reviewed",
                                                  "reviewer_notes"),
                                           where, diag, required=("title", "phrases")):
            continue
        phrases = [s for n, raw in enumerate(data.get("phrases") or [], 1)
                   if (s := _sentence(raw, PHRASE_FIELDS, where.at(f"phrase {n}"), diag))]
        course.phrasebook.append(PhraseCategorySrc(
            name=os.path.splitext(os.path.basename(path))[0], order=int(data.get("order", 0) or 0),
            title=_text(data, "title"), title_en=_text(data, "title_en"), phrases=phrases,
            reviewed=data.get("reviewed"), where=where, data=data))
    course.phrasebook.sort(key=lambda c: (c.order, c.name))

    for path in sorted(glob.glob(os.path.join(root, "deck", "*.yaml"))):
        data = _load_yaml(path, diag)
        where = Where(path, 1)
        if data is None or not _check_keys(data, ("title", "sentences", "reviewed", "reviewer_notes"), where, diag,
                                           required=("sentences",)):
            continue
        sentences = [s for n, raw in enumerate(data.get("sentences") or [], 1)
                     if (s := _sentence(raw, SENTENCE_FIELDS, where.at(f"sentence {n}"), diag))]
        course.deck.append(DeckSrc(name=os.path.splitext(os.path.basename(path))[0], title=_text(data, "title"),
                                   sentences=sentences, reviewed=data.get("reviewed"), where=where, data=data))

    seen_sets: dict[str, Where] = {}
    for conf_path in fragment_paths(root, "confusables.yaml", "confusables"):
        data = _load_yaml(conf_path, diag) or []
        if not isinstance(data, list):
            diag.error(Where(conf_path, 1), "schema", "expected a list of confusable sets")
            continue
        for n, raw in enumerate(data, 1):
            where = Where(conf_path, _line(raw, 1), f"set {n}")
            if _check_keys(raw, ("name", "lemmas", "note"), where, diag, required=("name", "lemmas")):
                name = _text(raw, "name")
                if name in seen_sets:
                    diag.error(where, "duplicate", f"confusable set '{name}' is also defined at {seen_sets[name]}")
                    continue
                seen_sets[name] = where
                course.confusables.append(ConfusableSrc(
                    name=name, lemmas=[str(x) for x in raw.get("lemmas") or []],
                    note=_text(raw, "note"), where=where))

    course.irregular_path = os.path.join(root, "verbs", "irregular.yaml")
    course.pronunciation_path = os.path.join(root, "pronunciation.yaml")
    course.irregular_paths = fragment_paths(root, os.path.join("verbs", "irregular.yaml"),
                                            os.path.join("verbs", "irregular"))
    course.pronunciation_paths = fragment_paths(root, "pronunciation.yaml", "pronunciation")
    course.ids_path = os.path.join(root, "ids.lock")
    return course
