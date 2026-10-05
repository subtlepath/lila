#!/usr/bin/env python3
"""Check the frequency-deck batches and the deck sentences written so far.

    python3 content-plan/deck/check_deck.py             # batches + every written deck file
    python3 content-plan/deck/check_deck.py B           # batch B only (its files and words)
    python3 content-plan/deck/check_deck.py --content DIR   # check another content tree

Batch checks (content-plan/deck/batches.yaml against content/lexicon/frequency*.tsv):
  - every frequency row is in exactly one batch, and every batch key is a frequency row;
  - each batch's keys lie inside its declared freq range, so A < B < C by rank.

Deck file checks (content/deck/a-*.yaml, b-*.yaml, c-*.yaml; the prefix names the batch):
  - every sentence has exactly one {target}, a deck word of the file's own batch;
  - every word of a batch that has deck files is the target of one or two sentences;
  - a sentence uses only course words (lessons' lexicon and names) and deck words of
    its own or a more frequent batch: no later-batch deck word, no phrasebook-only or
    dictionary-only word;
  - verb forms: no future or conditional in batches A and B; a present subjunctive is
    listed for review (fixed formulas only). Commands are fine.

Runs the compiler in check mode (writes nothing). Exit status 1 on any problem; notes
(review items, batches without files yet) do not fail.
"""

from __future__ import annotations

import fnmatch
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from packc import forms as F  # noqa: E402
from packc import yamlio  # noqa: E402
from packc.build import TF_TARGET, compile_course  # noqa: E402
from packc.diag import Diagnostics  # noqa: E402

BATCHES = os.path.join(ROOT, "content-plan", "deck", "batches.yaml")
CONTENT = os.path.join(ROOT, "content")
ORDER = "ABC"
MAX_TARGETS = 2
LONG = 100  # characters; the compiler's hard limit is 132
SUBJ_TRIGGERS = frozenset({"que", "ojalá", "para", "cuando"})


def word_source(entry) -> str:
    """'deck', 'phrasebook', 'dictionary' or 'course' for a lexicon row."""
    name = os.path.basename(entry.where.path)
    if entry.deck:
        return "deck"
    if entry.dictionary:
        return "dictionary"
    if name.startswith("phrasebook"):
        return "phrasebook"
    return "course"


def main(argv: list[str]) -> int:
    content = argv[argv.index("--content") + 1] if "--content" in argv else CONTENT
    only = {a.upper() for a in argv if a.upper() in ORDER and len(a) == 1}
    problems: list[str] = []
    notes: list[str] = []

    diag = Diagnostics()
    build = compile_course(content, diag, allow_missing_examples=True)
    lexicon = build.course.lexicon
    deck_rows = {k: e for k, e in lexicon.items() if e.deck}

    # --- batches -----------------------------------------------------------------------
    batches = yamlio.load(BATCHES)["batches"]
    batch_of: dict[str, str] = {}
    for name in ORDER:
        spec = batches.get(name) or {}
        low, high = spec.get("freq") or [0, 0]
        for key in spec.get("keys") or []:
            key = str(key)
            if key in batch_of:
                problems.append(f"batch {name}: '{key}' is also in batch {batch_of[key]}")
                continue
            batch_of[key] = name
            if key not in deck_rows:
                problems.append(f"batch {name}: '{key}' is not a row of lexicon/frequency*.tsv")
            elif not low <= deck_rows[key].freq <= high:
                problems.append(f"batch {name}: '{key}' has freq {deck_rows[key].freq}, "
                                f"outside the batch range {low}-{high}")
    for key in deck_rows:
        if key not in batch_of:
            problems.append(f"frequency row '{key}' is in no batch")

    # --- deck files --------------------------------------------------------------------
    targets: dict[str, list[str]] = {}
    files_of: dict[str, list[str]] = {b: [] for b in ORDER}
    for deck_file in build.deck_files:
        src = deck_file["src"]
        rel = os.path.relpath(src.where.path, ROOT)
        batch = next((b for b in ORDER if fnmatch.fnmatch(os.path.basename(src.where.path),
                                                          f"{b.lower()}-*.yaml")), None)
        if batch is None:
            problems.append(f"{rel}: deck files are named a-*.yaml, b-*.yaml or c-*.yaml by batch")
            continue
        files_of[batch].append(rel)
        if only and batch not in only:
            continue
        rank = ORDER.index(batch)
        for sid in deck_file["sentences"]:
            sent = build.sentences[sid]
            where = f"{rel}:{sent.src.where.line}"
            marked = [t for t in sent.tokens if t.flags & TF_TARGET]
            if len(marked) != 1:
                problems.append(f"{where}: {len(marked)} targets; mark exactly one: {sent.es}")
            for tok in marked:
                if tok.lemma is None:
                    continue
                key = tok.lemma.key
                if batch_of.get(key) != batch:
                    owner = f"batch {batch_of[key]}" if key in batch_of else "not a deck word"
                    problems.append(f"{where}: target '{key}' is {owner}, not batch {batch}: {sent.es}")
                targets.setdefault(key, []).append(where)
            if len(sent.es) > LONG:
                notes.append(f"{where}: {len(sent.es)} characters; aim for {LONG} or fewer: {sent.es}")
            for i, tok in enumerate(sent.tokens):
                lemma = tok.lemma
                if lemma is None:
                    continue
                source = word_source(lemma.entry)
                if source == "deck":
                    used = batch_of.get(lemma.key)
                    if used and ORDER.index(used) > rank:
                        problems.append(f"{where}: '{tok.surface}' is deck word '{lemma.key}' of "
                                        f"batch {used}, later than {batch}: {sent.es}")
                elif source != "course":
                    problems.append(f"{where}: '{tok.surface}' is a {source}-only word "
                                    f"('{lemma.key}'); use a course or deck word: {sent.es}")
                tag = tok.tag & ~F.ENCLITIC
                if lemma.verb is not None and F.is_verb_tag(tag):
                    tense = F.verb_tag_parts(tag)[0]
                    # Commands share the subjunctive's forms: negative ones (no te preocupes) and
                    # usted/ustedes ones (pague en caja). After que, ojalá, para or cuando it is
                    # a real subjunctive.
                    # The linker reports the first cell a form fills (pague: subj.1s), so look
                    # at every cell it fills.
                    before = {t.surface.lower() for t in sent.tokens[:i]}
                    form = lemma.form_for(tag)
                    cells = {F.verb_tag_parts(t)[0] for f, t in lemma.forms if f == form and F.is_verb_tag(t)}
                    command = not before & SUBJ_TRIGGERS and (
                        "imp" in cells or ("impneg" in cells and "no" in before))
                    if tense in ("fut", "cond") and batch != "C":
                        problems.append(f"{where}: '{tok.surface}' is {F.TENSE_NAMES[tense]}; "
                                        f"only batch C may use it: {sent.es}")
                    elif tense in ("fut", "cond"):
                        notes.append(f"{where}: {F.TENSE_NAMES[tense]} '{tok.surface}' (use sparingly)")
                    elif tense == "subj" and not command:
                        notes.append(f"{where}: subjunctive '{tok.surface}': a fixed formula only? {sent.es}")

    for batch in ORDER:
        if only and batch not in only:
            continue
        if not files_of[batch]:
            notes.append(f"batch {batch}: no deck files yet")
            continue
        for key, owner in batch_of.items():
            if owner != batch:
                continue
            n = len(targets.get(key, []))
            if n == 0:
                problems.append(f"batch {batch}: '{key}' is the target of no sentence")
            elif n > MAX_TARGETS:
                problems.append(f"batch {batch}: '{key}' is the target of {n} sentences "
                                f"({', '.join(targets[key])}); write one or two")

    deck_errors = [m for m in diag.errors if m.where and "/deck/" in m.where.path]
    if deck_errors:
        problems.append(f"the compiler reports {len(deck_errors)} error(s) in deck files; "
                        "run python3 tools/packc --check --allow-missing-examples")

    for line in notes:
        print(f"note: {line}")
    for line in problems:
        print(line)
    counted = ", ".join(f"{b} {sum(1 for v in batch_of.values() if v == b)} words "
                        f"/ {len(files_of[b])} file(s)" for b in ORDER)
    print(f"check_deck: {len(deck_rows)} frequency rows; {counted}; {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
