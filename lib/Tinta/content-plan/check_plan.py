#!/usr/bin/env python3
"""Check the course plan against the lexicon and the lessons written so far.

    python3 content-plan/check_plan.py            # plan consistency + vocabulary order
    python3 content-plan/check_plan.py u05 u06    # order check for these units only
    python3 content-plan/check_plan.py --counts   # also print per-unit counts
    python3 content-plan/check_plan.py --content DIR   # check another content tree

Plan checks (content-plan/units/*.yaml against content/lexicon/*.tsv):
  - every planned `new` key is a lexicon row, not a name, introduced only once in the
    whole course (Units 0-1 included), 8-12 per lesson;
  - `rows_in_core` lists exactly the new keys whose row is in core.tsv; the others
    live in the unit's own uNN.tsv;
  - every row of uNN.tsv is a planned new word of that unit, a `supporting` entry,
    or a name;
  - `conjugate` entries name lexicon verbs and valid tenses, and no verb:tense.person
    is drilled twice in the course;
  - titles fit 30 characters.

Order check (lessons that exist under content/units/): a lesson sentence or dialogue
line may only use words introduced in that lesson or earlier. A word counts as
introduced where a written lesson's `new` says so, otherwise where the plan puts it.
Words no lesson introduces (supporting rows, core function words) are always allowed.
Units 0-1 predate the plan and are only reported with --all. ORDER_EXCEPTIONS lists the
few deliberate early uses (Unit 0 teaches sounds before any verb); they are not reported.

Runs the compiler in check mode (writes nothing). Exit status 1 on any problem.
"""

from __future__ import annotations

import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from packc import forms as F  # noqa: E402
from packc import yamlio  # noqa: E402
from packc.build import NONE16, compile_course  # noqa: E402
from packc.diag import Diagnostics  # noqa: E402

PLAN_DIR = os.path.join(ROOT, "content-plan", "units")
CONTENT = os.path.join(ROOT, "content")


# Deliberate early uses of a word, not reported by the order check: (lemma key, lesson or
# unit where the use is allowed, why). Keep this short; every entry needs a reason.
ORDER_EXCEPTIONS = [
    ("ser", "u00", "Unit 0 teaches sounds and has no verbs; es/son/soy frame its example words "
                   "(ser is taught in u01.l02)."),
    ("por favor", "u00", "Orders like 'Un café, por favor' show the sounds in use; taught in u01.l03."),
    ("gracias", "u00", "Courtesy in the Unit 0 dialogues; taught in u01.l01."),
    ("naranja", "u00.l02", "'jugo de naranja' shows the j sound twice; naranja is taught in u07.l01."),
]


def excepted(key: str, ref: str) -> bool:
    return any(key == k and (ref == where or ref.startswith(where + ".")) for k, where, _why in ORDER_EXCEPTIONS)


def order_key(ref: str) -> tuple[int, int]:
    """'u05.l03' -> (5, 3)."""
    unit, lesson = ref.split(".")
    return int(unit[1:]), int(lesson[1:])


def load_plan() -> list[dict]:
    return [yamlio.load(p) for p in sorted(glob.glob(os.path.join(PLAN_DIR, "*.yaml")))]


def conjugate_items(entry: str) -> list[str]:
    """'ser:pres' -> ['ser:pres.1s', ...]; 'ser:pres.3s' -> ['ser:pres.3s']."""
    verb, _, form = entry.partition(":")
    tense, _, person = form.partition(".")
    if person:
        return [f"{verb}:{tense}.{person}"]
    persons = F.IMPERATIVE_PERSONS if tense in ("imp", "impneg") else F.PERSONS
    return [f"{verb}:{tense}.{p}" for p in persons]


def main(argv: list[str]) -> int:
    show_counts = "--counts" in argv
    include_old = "--all" in argv
    only = {a for a in argv if a.startswith("u")}
    content = argv[argv.index("--content") + 1] if "--content" in argv else CONTENT
    problems: list[str] = []

    diag = Diagnostics()
    build = compile_course(content, diag, allow_missing_examples=True)
    lexicon = build.course.lexicon                       # key -> LexEntry, all files
    by_key = build.by_key
    file_of = {k: os.path.basename(e.where.path) for k, e in lexicon.items()}
    plan = load_plan()

    # --- introductions: written lessons first, then the plan for the rest ------------
    written = {les.src.ref: les for les in build.lessons}
    introduced: dict[str, str] = {}
    for les in build.lessons:
        for lem in les.new:
            introduced[lem.key] = les.src.ref
    planned_new: dict[str, str] = {}
    for unit in plan:
        for les in unit["lessons"]:
            for key in les["new"]:
                if key in planned_new:
                    problems.append(f"{les['id']}: '{key}' is also planned as new in {planned_new[key]}")
                planned_new[key] = les["id"]
                if key in introduced and introduced[key] != les["id"]:
                    problems.append(f"{les['id']}: '{key}' is already introduced in {introduced[key]}")
                introduced.setdefault(key, les["id"])

    # --- plan consistency --------------------------------------------------------------
    drilled: dict[str, str] = {}
    for les in build.lessons:
        for entry, _where in les.src.conjugate:
            for item in conjugate_items(str(entry)):
                drilled[item] = les.src.ref
    counts = []
    for unit in plan:
        n = unit["unit"]
        tsv = f"u{n:02d}.tsv"
        unit_new: set[str] = set()
        for les in unit["lessons"]:
            ref = les["id"]
            new = les["new"]
            for what in ("title",):
                if len(les[what]) > 30:
                    problems.append(f"{ref}: title '{les[what]}' is over 30 characters")
            if not 8 <= len(new) <= 12:
                problems.append(f"{ref}: {len(new)} new lemmas (8-12)")
            core_rows = []
            for key in new:
                if key not in lexicon:
                    problems.append(f"{ref}: new '{key}' has no lexicon row")
                    continue
                if lexicon[key].pos == "propn":
                    problems.append(f"{ref}: new '{key}' is a name")
                if file_of[key] == "core.tsv":
                    core_rows.append(key)
                elif file_of[key] != tsv:
                    problems.append(f"{ref}: row for '{key}' is in {file_of[key]}, expected {tsv}")
            if sorted(core_rows) != sorted(les.get("rows_in_core") or []):
                problems.append(f"{ref}: rows_in_core should be {core_rows}")
            unit_new.update(new)
            for entry in les.get("conjugate") or []:
                verb, _, form = str(entry).partition(":")
                tense = form.partition(".")[0]
                if verb not in by_key or by_key[verb].verb is None:
                    problems.append(f"{ref}: conjugate '{entry}': '{verb}' is not a lexicon verb")
                if tense not in F.TENSES:
                    problems.append(f"{ref}: conjugate '{entry}': unknown tense")
                for item in conjugate_items(str(entry)):
                    if item in drilled and drilled[item] != ref:
                        problems.append(f"{ref}: conjugate '{entry}' repeats {item} from {drilled[item]}")
                    drilled.setdefault(item, ref)
        if len(unit["title"]) > 30:
            problems.append(f"unit {n}: title over 30 characters")
        supporting = set(unit.get("supporting") or [])
        rows = [k for k, f in file_of.items() if f == tsv]
        names = [k for k in rows if lexicon[k].pos == "propn"]
        for key in rows:
            if key in names or key in unit_new or key in supporting:
                continue
            problems.append(f"{tsv}: row '{key}' is neither planned new in unit {n} nor supporting")
        for key in supporting:
            if file_of.get(key) != tsv:
                problems.append(f"unit {n}: supporting '{key}' has no row in {tsv}")
            if key in introduced:
                problems.append(f"unit {n}: supporting '{key}' is introduced in {introduced[key]}")
        counts.append((n, len(unit["lessons"]), len(unit_new), sum(1 for k in unit_new if file_of.get(k) == "core.tsv"),
                       len(rows) - len(names), len(supporting), len(names)))

    # --- vocabulary order in written lessons ---------------------------------------------
    for sent in build.sentences:
        if sent.lesson == NONE16:
            continue
        ref = build.lessons[sent.lesson].src.ref
        unit_no = order_key(ref)[0]
        if unit_no < 2 and not include_old:
            continue
        if only and ref.split(".")[0] not in only:
            continue
        for tok in sent.tokens:
            lem = tok.lemma
            if lem is None or lem.key not in introduced:
                continue
            first = introduced[lem.key]
            if order_key(first) > order_key(ref) and not excepted(lem.key, ref):
                where = sent.src.where
                problems.append(f"{where}: '{tok.surface}' ({lem.key}) is introduced later, in {first}")
    for st in build.stories:
        ref = st.src.lesson_ref
        if not ref or (order_key(ref)[0] < 2 and not include_old):
            continue
        for _speaker, sid, _flags in st.lines:
            for tok in build.sentences[sid].tokens:
                lem = tok.lemma
                if lem is not None and lem.key in introduced and order_key(introduced[lem.key]) > order_key(ref) \
                        and not excepted(lem.key, ref):
                    problems.append(f"{st.src.where}: '{tok.surface}' ({lem.key}) is introduced later, in "
                                    f"{introduced[lem.key]}, but the story unlocks after {ref}")

    if show_counts or not problems:
        total_new = sum(c[2] for c in counts)
        total_rows = sum(c[4] for c in counts)
        print("unit lessons new (in core) rows supporting names")
        for c in counts:
            print(f"u{c[0]:02d}  {c[1]:>7} {c[2]:>3} {c[3]:>10} {c[4]:>4} {c[5]:>10} {c[6]:>5}")
        print(f"total {sum(c[1] for c in counts):>6} {total_new:>3} {sum(c[3] for c in counts):>10} "
              f"{total_rows:>4} {sum(c[5] for c in counts):>10} {sum(c[6] for c in counts):>5}")
        print(f"written lessons checked for order: {len([r for r in written if order_key(r)[0] >= 2])}")
    for p in problems:
        print(p)
    if diag.errors:
        print("note: the compiler reports errors too; run python3 tools/packc --check --allow-missing-examples")
    print(f"check_plan: {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
