"""Review workflow (PLAN.md 7.5): content hashes and printable review pages.

A reviewable file (a lesson, a story or a phrasebook category) records
`reviewed: {by, date, hash}`. The hash covers the file without its `reviewed`
field, plus what the reviewer sees with it: a lesson's unit header and the
lexicon rows of the lemmas it introduces. `--release` refuses a file whose
current hash differs from its reviewed hash; other builds only warn.
"""

from __future__ import annotations

import datetime
import hashlib
import html
import json
import os
import re
from dataclasses import dataclass

from . import forms as F
from . import markup, yamlio
from .build import NONE16, TF_TARGET, Build
from .sources import POS_NAMES


@dataclass
class ReviewStatus:
    kind: str           # lesson | story | phrasebook
    name: str           # u01.l02, story file name, phrasebook file name
    title: str
    path: str
    hash: str
    reviewed: dict | None
    ok: bool
    reason: str
    page: str           # review page file name
    gated: bool = True  # False: shown for review, but --release does not require it (dictionary)


def _digest(payload: dict) -> str:
    text = json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:12]


def _without_reviewed(data: dict | None) -> dict:
    """The file as hashed: without `reviewed:` and without any `reviewer_notes:`, so
    answering the author's questions does not invalidate a sign-off."""
    def strip(value):
        if isinstance(value, dict):
            return {k: strip(v) for k, v in value.items() if k != "reviewer_notes"}
        if isinstance(value, list):
            return [strip(v) for v in value]
        return value
    data = strip(yamlio.plain(data or {}))
    data.pop("reviewed", None)
    return data


def _file_notes(data) -> list[str]:
    value = (data or {}).get("reviewer_notes") if isinstance(data, dict) else None
    if value is None:
        return []
    return [str(v) for v in value] if isinstance(value, list) else [str(value)]


def _status(kind, name, title, path, payload, reviewed, page) -> ReviewStatus:
    digest = _digest(payload)
    if not reviewed:
        ok, reason = False, "not reviewed"
    elif not isinstance(reviewed, dict) or not all(reviewed.get(k) for k in ("by", "date", "hash")):
        ok, reason = False, "reviewed: needs by, date and hash"
    elif str(reviewed["hash"]) != digest:
        ok, reason = False, f"changed since review (reviewed {reviewed['hash']}, now {digest})"
    else:
        ok, reason = True, f"reviewed by {reviewed['by']} on {yamlio.plain(reviewed['date'])}"
    return ReviewStatus(kind, name, title, path, digest, reviewed, ok, reason, page)


def check_hashes(b: Build) -> None:
    out: list[ReviewStatus] = []
    for lesson in b.lessons:
        src = lesson.src
        unit = b.course.units[lesson.unit]
        payload = {"lesson": _without_reviewed(src.data), "unit": yamlio.plain(unit.data),
                   "lexicon": [lemma.entry.row for lemma in lesson.new]}
        status = _status("lesson", src.ref, src.title, src.where.path, payload, src.reviewed,
                         f"{src.ref.replace('.', '-')}.html")
        lesson.hash = status.hash
        lesson.reviewed = status.ok
        out.append(status)
    for story in b.stories:
        if not story.src.name:
            continue  # a lesson dialogue is reviewed with its lesson
        src = story.src
        out.append(_status("story", src.name, src.title, src.where.path,
                           {"story": _without_reviewed(src.data)}, src.reviewed, f"story-{src.name}.html"))
    for cat in b.phrase_categories:
        src = cat["src"]
        out.append(_status("phrasebook", src.name, src.title, src.where.path,
                           {"phrasebook": _without_reviewed(src.data)}, src.reviewed,
                           f"phrasebook-{src.name}.html"))
    for deck in b.deck_files:
        src = deck["src"]
        payload = {"deck": _without_reviewed(src.data), "lexicon": [w.entry.row for w in deck_words(b, deck)]}
        out.append(_status("deck", src.name, src.title or src.name, src.where.path, payload, src.reviewed,
                           f"deck-{src.name}.html"))
    for path, lemmas in dictionary_files(b).items():
        name = os.path.splitext(os.path.basename(path))[0]
        status = _status("dictionary", name, f"Dictionary: {name}", path,
                         {"dictionary": [lem.entry.row for lem in lemmas]}, b.course.lexicon_reviews.get(path),
                         f"{name}.html")
        status.gated = False
        if not status.ok and not status.reviewed:
            status.reason = "not reviewed (not required for release)"
        out.append(status)
    seen: dict[str, ReviewStatus] = {}
    for status in out:  # --mark-reviewed addresses files by name
        if status.name in seen:
            b.diag.error(None, "duplicate", f"review name '{status.name}' is used by {seen[status.name].path} and "
                         f"{status.path}; rename one of the files")
        seen[status.name] = status
    b.review_status = out


def dictionary_files(b: Build) -> dict[str, list]:
    """lexicon/dictionary*.tsv path -> its lemmas, in file order."""
    files: dict[str, list] = {}
    for entry in b.course.lexicon.values():
        if entry.dictionary and entry.key in b.by_key:
            files.setdefault(entry.where.path, []).append(b.by_key[entry.key])
    return files


def _mark_tsv(status: ReviewStatus, by: str, date: str) -> None:
    """A dictionary file records its review in a comment line at the top."""
    with open(status.path, encoding="utf-8") as fh:
        lines = [line for line in fh.read().split("\n") if not line.startswith("# reviewed:")]
    by_text = json.dumps(by, ensure_ascii=False)
    lines.insert(0, f"# reviewed: {{by: {by_text}, date: {date}, hash: '{status.hash}'}}")
    with open(status.path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


def mark_reviewed(b: Build, names: list[str], by: str, date: str | None = None) -> list[ReviewStatus]:
    """Record `reviewed: {by, date, hash}` in each named file with its current content hash.

    names are lesson refs (u01.l02), story or phrasebook file names, or "all".
    Raises KeyError for a name that matches nothing.
    """
    date = date or datetime.date.today().isoformat()
    wanted = {s.name: s for s in b.review_status}
    chosen = list(wanted.values()) if names == ["all"] else []
    for name in names if names != ["all"] else []:
        if name not in wanted:
            raise KeyError(name)
        chosen.append(wanted[name])
    for status in chosen:
        if status.kind == "dictionary":
            _mark_tsv(status, by, date)
            continue
        with open(status.path, encoding="utf-8") as fh:
            lines = fh.read().split("\n")
        # Drop an existing top-level reviewed: entry (one line, or a block of indented lines).
        out, skipping = [], False
        for line in lines:
            if re.match(r"reviewed\s*:", line):
                skipping = True
                continue
            if skipping and (line.startswith((" ", "\t")) or not line.strip()):
                continue
            skipping = False
            out.append(line)
        while out and not out[-1].strip():
            out.pop()
        by_text = json.dumps(by, ensure_ascii=False)
        out.append(f"reviewed: {{by: {by_text}, date: {date}, hash: '{status.hash}'}}")
        with open(status.path, "w", encoding="utf-8") as fh:
            fh.write("\n".join(out) + "\n")
    return chosen


# --- pages -------------------------------------------------------------------------

CSS = """
:root { --ink: #1b1b1b; --muted: #666; --line: #bbb; --accent: #8a1c1c; --paper: #fff; }
* { box-sizing: border-box; }
body { margin: 0 auto; max-width: 60rem; padding: 1.5rem 1rem 4rem; color: var(--ink);
       background: var(--paper); font: 15px/1.45 Helvetica, Arial, sans-serif; }
h1 { font-size: 1.5rem; margin: 0 0 .2rem; } h2 { font-size: 1.1rem; margin: 2rem 0 .5rem;
     border-bottom: 2px solid var(--ink); padding-bottom: .2rem; }
h3 { font-size: 1rem; margin: 1rem 0 .3rem; }
.meta { color: var(--muted); font-size: .9rem; } .status-bad { color: var(--accent); font-weight: bold; }
.es { font-family: "Times New Roman", Times, serif; font-size: 1.15rem; }
.es b { text-decoration: underline; }
.pron { font-style: italic; color: #333; } .tag { font-size: .75rem; text-transform: uppercase;
     letter-spacing: .04em; border: 1px solid var(--line); padding: 0 .3em; margin-right: .2em; }
.links { color: var(--muted); font-size: .8rem; } .notmx { text-decoration: line-through; }
table { border-collapse: collapse; width: 100%; } th, td { border: 1px solid var(--line);
     padding: .35rem .45rem; vertical-align: top; text-align: left; }
th { background: #f2f2f2; font-size: .8rem; } td.fix { width: 22%; } td.n { width: 2rem; color: var(--muted); }
.note { border-left: 3px solid var(--ink); padding: .1rem .8rem; margin: .8rem 0; }
.note p { margin: .4rem 0; } .note ul { margin: .4rem 0; padding-left: 1.4rem; } .note li { margin: .2rem 0; }
.ask { background: #fff4c2; border: 2px solid #c9a400; padding: .5rem .9rem; margin: 1rem 0; }
.ask h2 { border: 0; margin: 0 0 .3rem; } .ask li { margin: .25rem 0; }
.ask-inline { background: #fff4c2; padding: .15rem .35rem; margin-top: .3rem; font-size: .85rem; }
table.compact td, table.compact th { padding: .15rem .35rem; font-size: .85rem; }
table.compact .es { font-size: 1rem; }
.sign { margin-top: 2.5rem; border-top: 1px solid var(--line); padding-top: .6rem; }
@media print { body { max-width: none; padding: 0; font-size: 11pt; } h2 { break-after: avoid; }
     tr { break-inside: avoid; } a { color: inherit; text-decoration: none; } }
"""


def article(lemma) -> str:
    if lemma.entry.pos != "noun":
        return ""
    if lemma.plural_only:
        return "los" if lemma.gender == 1 else "las"
    return {1: "el", 2: "el" if lemma.flags & 4 else "la", 3: "el/la"}.get(lemma.gender, "")


def headword(lemma) -> str:
    art = article(lemma)
    return f"{art} {lemma.es}" if art else lemma.es


def _e(text: str) -> str:
    return html.escape(text or "")


def _page(title: str, body: str) -> str:
    return (f"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
            f"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
            f"<title>{_e(title)}</title><style>{CSS}</style></head><body>{body}</body></html>\n")


def _spanish(b: Build, sent) -> str:
    out, pos = [], 0
    for tok in sent.tokens:
        out.append(_e(sent.es[pos:tok.start]))
        word = _e(sent.es[tok.start:tok.end])
        out.append(f"<b>{word}</b>" if tok.flags & TF_TARGET else word)
        pos = tok.end
    out.append(_e(sent.es[pos:]))
    return "".join(out)


def _links(sent) -> str:
    parts = []
    for tok in sent.tokens:
        if tok.lemma is None:
            continue
        same = tok.surface.lower() == tok.lemma.es.lower()
        if same and tok.tag in (F.BASE, F.NONFINITE["inf"]):
            continue
        tag = "" if tok.tag in (F.BASE,) else f" {F.tag_name(tok.tag)}"
        parts.append(f"{_e(tok.surface)} → {_e(tok.lemma.key)}{_e(tag)}")
    return f"<div class=\"links\">{' · '.join(parts)}</div>" if parts else ""


def _note_html(note) -> str:
    paras: list[list[str]] = [[]]
    kinds = ["p"]
    styles = {markup.TEXT: "{}", markup.STRONG: "<b>{}</b>", markup.SPANISH: "<i class=\"es\">{}</i>",
              markup.SPANISH_STRONG: "<b class=\"es\">{}</b>", markup.RESPELLING: "<span class=\"pron\">{}</span>",
              markup.NOT_MEXICAN: "<span class=\"es notmx\" title=\"not Mexican\">{}</span>"}
    for style, text in note.spans:
        if style in (markup.PARAGRAPH, markup.BULLET):
            paras.append([])
            kinds.append("li" if style == markup.BULLET else "p")
            continue
        paras[-1].append(styles[style].format(_e(text)))
    body, in_list = [], False
    for kind, para in zip(kinds, paras):
        if not para:
            continue
        if (kind == "li") != in_list:
            body.append("<ul>" if kind == "li" else "</ul>")
            in_list = kind == "li"
        body.append(f"<{kind}>{''.join(para)}</{kind}>")
    if in_list:
        body.append("</ul>")
    body = "".join(body)
    return f"<div class=\"note\"><h3>{_e(note.title)}</h3>{body}</div>"


def _verb_table(lemma, tenses) -> str:
    rows = []
    for tense in tenses:
        persons = F.IMPERATIVE_PERSONS if tense in ("imp", "impneg") else F.PERSONS
        cells = []
        for p in F.PERSONS:
            form = lemma.verb.forms.get((tense, p), "") if p in persons else ""
            if form and tense == "impneg":
                form = "no " + form
            cells.append(f"<td class=\"es\">{_e(form)}</td>")
        rows.append(f"<tr><th>{_e(F.TENSE_NAMES[tense])}</th>{''.join(cells)}</tr>")
    head = "".join(f"<th>{_e(F.PERSON_NAMES[p])}</th>" for p in F.PERSONS)
    return (f"<h3 class=\"es\">{_e(lemma.es)} <span class=\"meta\">gerund {_e(lemma.verb.gerund)}, "
            f"participle {_e(lemma.verb.participle)}</span></h3>"
            f"<table><tr><th></th>{head}</tr>{''.join(rows)}</table>")


def _sentence_rows(b: Build, ids, speaker=False) -> str:
    rows = []
    for n, sid in enumerate(ids, 1):
        sent = b.sentences[sid]
        note = f"<div class=\"meta\">{_e(sent.note)}</div>" if sent.note else ""
        who = f"<td>{_e(sent.src.speaker)}</td>" if speaker else ""
        rows.append(f"<tr><td class=\"n\">{n}</td>{who}<td><div class=\"es\">{_spanish(b, sent)}</div>"
                    f"{_links(sent)}{_ask_inline(sent)}</td><td>{_e(sent.en)}{note}</td><td class=\"fix\"></td></tr>")
    who = "<th>Speaker</th>" if speaker else ""
    return (f"<table><tr><th></th>{who}<th>Spanish</th><th>English</th><th>Corrections</th></tr>"
            f"{''.join(rows)}</table>")


def _ask_inline(sent) -> str:
    return "".join(f"<div class=\"ask-inline\"><b>Please confirm:</b> {_e(q)}</div>"
                   for q in sent.src.reviewer_notes)


def _questions(file_notes: list[str], sentences: list) -> str:
    """The author's reviewer_notes, first on the page: what the reviewer must decide."""
    items = [f"<li>{_e(q)}</li>" for q in file_notes]
    for sent in sentences:
        items += [f"<li><span class=\"es\">{_e(sent.es)}</span> — {_e(q)}</li>" for q in sent.src.reviewer_notes]
    if not items:
        return ""
    return ("<div class=\"ask\"><h2>Questions for the reviewer</h2><p class=\"meta\">The author asks you to "
            "confirm these. Answer next to them or in the corrections column.</p>"
            f"<ul>{''.join(items)}</ul></div>")


def question_count(b: Build, status: ReviewStatus) -> int:
    if status.kind == "dictionary":
        return 0
    if status.kind == "lesson":
        lesson = next(l for l in b.lessons if l.src.ref == status.name)
        data, ids = lesson.src.data, list(lesson.sentences)
        extra = _file_notes(data.get("dialogue"))
        if lesson.dialogue != NONE16:
            ids += [sid for _s, sid, _f in b.stories[lesson.dialogue].lines]
    elif status.kind == "story":
        story = next(st for st in b.stories if st.src.name == status.name)
        data, ids, extra = story.src.data, [sid for _s, sid, _f in story.lines], []
    elif status.kind == "deck":
        deck = next(f for f in b.deck_files if f["src"].name == status.name)
        data, ids, extra = deck["src"].data, deck["sentences"], []
    else:
        i = next(i for i, c in enumerate(b.phrase_categories) if c["src"].name == status.name)
        data, extra = b.phrase_categories[i]["src"].data, []
        ids = [e.sentence for e in b.phrase_entries if e.category == i]
    return len(_file_notes(data)) + len(extra) + sum(len(b.sentences[i].src.reviewer_notes) for i in ids)


def _header(b: Build, status: ReviewStatus, subtitle: str) -> str:
    cls = "" if status.ok else " class=\"status-bad\""
    return (f"<h1 class=\"es\">{_e(status.title)}</h1><div class=\"meta\">{_e(subtitle)}</div>"
            f"<div class=\"meta\">{_e(os.path.relpath(status.path))} · content hash <b>{status.hash}</b> · "
            f"<span{cls}>{_e(status.reason)}</span></div>")


def _signoff(status: ReviewStatus) -> str:
    return ("<div class=\"sign\">Reviewed by ____________________ on ____________.<ol>"
            "<li>Write corrections in the right-hand column and return the page.</li>"
            "<li>The author applies them and runs <code>python3 tools/packc --review</code> again; "
            "the new page shows the corrected text and a new content hash.</li>"
            "<li>The reviewer checks the corrected page. When it is right, the author records it with "
            f"<code>python3 tools/packc --mark-reviewed {_e(status.name)} --by \"Name\"</code>, which writes "
            "<code>reviewed: {by, date, hash}</code> with the hash of the corrected content.</li></ol>"
            f"This page shows content hash <b>{status.hash}</b>; any later edit changes it, and "
            "<code>--release</code> refuses the file until it is marked reviewed again.</div>")


def _words_table(heading: str, lemmas) -> str:
    rows = []
    for n, lemma in enumerate(lemmas, 1):
        tags = [POS_NAMES.get(lemma.pos, "?")]
        if lemma.reg:
            tags.append(("neutral", "formal", "informal", "vulgar")[lemma.reg])
        if lemma.flags & 1:
            tags.append("MX")
        forms = ", ".join(x for x in (lemma.feminine, lemma.plural) if x)
        extra = f"<div class=\"meta\">{_e(forms)}</div>" if forms else ""
        note = f"<div class=\"meta\">{_e(lemma.entry.note)}</div>" if lemma.entry.note else ""
        alt = f"<div class=\"meta\">also accepted: {_e(lemma.entry.alt)}</div>" if lemma.entry.alt else ""
        rows.append(f"<tr><td class=\"n\">{n}</td><td><span class=\"es\">{_e(headword(lemma))}</span>{extra}"
                    f"<div class=\"pron\">{_e(lemma.pron)}</div></td>"
                    f"<td>{''.join(f'<span class=tag>{_e(t)}</span>' for t in tags)}</td>"
                    f"<td>{_e(lemma.en)}{note}{alt}</td><td class=\"fix\"></td></tr>")
    return (f"<h2>{_e(heading)}</h2><table><tr><th></th><th>Spanish</th><th>Tags</th><th>English</th>"
            f"<th>Corrections</th></tr>{''.join(rows)}</table>")


def deck_words(b: Build, deck) -> list:
    """Frequency-deck words practised by a deck file's targets, in first-use order."""
    words = []
    for sid in deck["sentences"]:
        for tok in b.sentences[sid].tokens:
            if tok.flags & TF_TARGET and tok.lemma is not None and tok.lemma.flags & 16 \
                    and tok.lemma not in words:
                words.append(tok.lemma)
    return words


def _needs_confirming(lemma) -> bool:
    """A dictionary row a native speaker should look at first: a judgement call."""
    e = lemma.entry
    return bool(e.mx or lemma.reg or e.note or e.pron)


def _dictionary_forms(lemma) -> str:
    e = lemma.entry
    parts = [f"{tag} {form}" for tag, form in e.forms.items()]
    if lemma.pos == 2 and lemma.verb is None:
        parts.append("no verb table yet")
    elif lemma.verb is not None and (lemma.verb.irregular or lemma.verb.stem or lemma.verb.spelling):
        v = lemma.verb.forms
        parts.append(f"yo {v.get(('pres', '1s'), '')}, él {v.get(('pres', '3s'), '')}; "
                     f"pret. él {v.get(('pret', '3s'), '')}; part. {lemma.verb.participle}")
    return "; ".join(parts)


def _dictionary_rows(lemmas) -> str:
    rows = []
    for n, lemma in enumerate(lemmas, 1):
        tags = [POS_NAMES.get(lemma.pos, "?")]
        if lemma.gender:
            tags.append({1: "m", 2: "f", 3: "m/f"}[lemma.gender] + (" pl" if lemma.plural_only else ""))
        if lemma.reg:
            tags.append(("neutral", "formal", "informal", "vulgar")[lemma.reg])
        if lemma.flags & 1:
            tags.append("MX")
        note = f"<div class=\"meta\">{_e(lemma.entry.note)}</div>" if lemma.entry.note else ""
        own = " (by hand)" if lemma.entry.pron else ""
        rows.append(f"<tr><td class=\"n\">{n}</td><td><span class=\"es\">{_e(headword(lemma))}</span>"
                    f"<div class=\"pron\">{_e(lemma.pron)}{own}</div></td>"
                    f"<td>{''.join(f'<span class=tag>{_e(t)}</span>' for t in tags)}</td>"
                    f"<td>{_e(lemma.en)}{note}</td><td class=\"es\">{_e(_dictionary_forms(lemma))}</td>"
                    "<td class=\"fix\"></td></tr>")
    return ("<table class=\"compact\"><tr><th></th><th>Spanish</th><th>Tags</th><th>English</th><th>Forms</th>"
            f"<th>Corrections</th></tr>{''.join(rows)}</table>")


def dictionary_page(b: Build, lemmas, status: ReviewStatus) -> str:
    ordered = sorted(lemmas, key=lambda lem: (lem.fold, lem.es))
    confirm = [lem for lem in ordered if _needs_confirming(lem)]
    rest = [lem for lem in ordered if not _needs_confirming(lem)]
    parts = [_header(b, status, f"{len(lemmas)} dictionary entries · lookup only, no exercises"),
             "<div class=\"ask\"><b>Not required for release.</b> These entries only appear when a learner "
             "looks a word up, so a release does not wait for this page; corrections are still very welcome. "
             "Mark wrong glosses, tags, notes or forms in the right-hand column.</div>"]
    if confirm:
        parts.append(f"<h2>Please confirm ({len(confirm)})</h2><p class=\"meta\">Entries with a Mexico flag, "
                     "a register tag, a usage note or a respelling written by hand.</p>" + _dictionary_rows(confirm))
    if rest:
        parts.append(f"<h2>Other entries ({len(rest)})</h2>" + _dictionary_rows(rest))
    parts.append(_signoff(status))
    return _page(f"Review {status.name}", "".join(parts))


HOW_TO = """
<h1>How to review Tinta</h1>
<p>Tinta teaches the Spanish spoken in Mexico to English speakers. You are checking that what it teaches
is natural, correct Mexican Spanish. You do not need the computer or the device: print the pages, mark them
with a pen, and give them back.</p>
<h2>The pages</h2>
<ul>
<li><b>Lesson</b> (u01.l02 and so on): the notes the learner reads, the new words, the verb tables, every practice
sentence with its English, and the lesson's dialogue.</li>
<li><b>Story</b>: a short reading or dialogue with comprehension questions (the right answer is in bold).</li>
<li><b>Phrasebook</b>: ready-made phrases for travel, with their pronunciation.</li>
<li><b>Deck</b>: extra practice sentences for frequent words that the lessons do not teach.</li>
<li><b>Dictionary</b>: entries learners can look up. Not needed before release, but corrections help.</li>
</ul>
<h2>What to check</h2>
<ul>
<li>Would a Mexican say it this way? Is anything Spanish from Spain (vosotros, ordenador, coger...)?</li>
<li>Is the English translation right and natural?</li>
<li>Are accents and punctuation right (¿? ¡! á é í ó ú ñ ü)?</li>
<li>Is <i>tú</i> or <i>usted</i> used where a Mexican would use it?</li>
<li>Are the tags right (see below)?</li>
</ul>
<h2>How to mark corrections</h2>
<ul>
<li>Write the correction in the <b>Corrections</b> column of the same row. Cross out what is wrong and write the
right version; one short reason helps ("too formal", "nobody says this", "Spain").</li>
<li>A yellow <b>Questions for the reviewer</b> box lists things the author was unsure about: answer each one
next to it ("yes", "no, say ...").</li>
<li>If a whole row is fine, leave it blank. Sign and date the bottom of each page you finish.</li>
<li>Give the pages back to the author, who makes the changes and prints the page again for a last look.</li>
</ul>
<h2>What the tags mean</h2>
<table>
<tr><th>Tag</th><th>Meaning</th></tr>
<tr><td>noun, verb, adj, adv, pron, det, prep, conj, interj, expr, propn</td><td>part of speech: noun, verb,
adjective, adverb, pronoun, article/determiner, preposition, conjunction, interjection, expression, name</td></tr>
<tr><td>m, f, m/f, pl</td><td>gender (el / la / either) and plural-only nouns (los lentes)</td></tr>
<tr><td>formal, informal, vulgar</td><td>register: who you can say it to. Vulgar words are hidden unless the learner
turns them on.</td></tr>
<tr><td>MX</td><td>a word or meaning typical of Mexico (camión = bus, chamba)</td></tr>
<tr><td>CAPITALS in the pronunciation</td><td>the stressed syllable: CHAHM-bah. Spelled for English readers.</td></tr>
<tr><td><b>bold</b> word in a sentence</td><td>the word the learner fills in during practice</td></tr>
<tr><td>grey line under a sentence</td><td>which dictionary word each Spanish word is linked to</td></tr>
<tr><td><s>struck-through</s> Spanish</td><td>a form quoted as NOT Mexican, to warn the learner</td></tr>
</table>
"""


def deck_page(b: Build, deck, status: ReviewStatus) -> str:
    src = deck["src"]
    sents = [b.sentences[i] for i in deck["sentences"]]
    parts = [_header(b, status, f"Frequency deck · {src.title or src.name}"),
             _questions(_file_notes(src.data), sents)]
    words = deck_words(b, deck)
    if words:
        parts.append(_words_table("Deck words practised here", words))
    parts.append("<h2>Sentences</h2><p class=\"meta\">Bold words are practice targets.</p>"
                 + _sentence_rows(b, deck["sentences"]))
    parts.append(_signoff(status))
    return _page(f"Review deck {src.name}", "".join(parts))


def lesson_page(b: Build, lesson, status: ReviewStatus) -> str:
    src = lesson.src
    unit = b.course.units[lesson.unit]
    parts = [_header(b, status, f"Unit {unit.number} · {unit.title} — Lesson {src.number}: "
                                f"{src.title_en} · {src.level}")]
    sents = [b.sentences[i] for i in lesson.sentences]
    if lesson.dialogue != NONE16:
        sents += [b.sentences[sid] for _s, sid, _f in b.stories[lesson.dialogue].lines]
    parts.append(_questions(_file_notes(src.data) + _file_notes(src.data.get("dialogue")), sents))
    if lesson.notes:
        parts.append("<h2>Notes</h2>" + "".join(_note_html(b.notes[i]) for i in lesson.notes))
    if lesson.new:
        parts.append(_words_table("New words", lesson.new))
    verbs = {}
    for spec, _where in src.conjugate:
        key, _, tense = spec.partition(":")
        lemma = b.by_key.get(key.strip())
        if lemma is not None and lemma.verb is not None:
            verbs.setdefault(lemma.key, (lemma, []))[1].append(tense.split(".")[0])
    for lemma in lesson.new:
        if lemma.verb is not None:
            verbs.setdefault(lemma.key, (lemma, []))
    if verbs:
        tables = []
        for lemma, tenses in verbs.values():
            wanted = list(dict.fromkeys(["pres"] + tenses))
            tables.append(_verb_table(lemma, wanted))
        parts.append("<h2>Verb forms</h2>" + "".join(tables))
    if lesson.sentences:
        parts.append("<h2>Sentences</h2><p class=\"meta\">Bold words are practice targets. Grey lines show "
                     "how each word is linked to the dictionary; check them too.</p>"
                     + _sentence_rows(b, lesson.sentences))
    if lesson.dialogue != NONE16:
        story = b.stories[lesson.dialogue]
        parts.append(f"<h2>Dialogue: <span class=\"es\">{_e(story.title)}</span> "
                     f"<span class=\"meta\">{_e(story.title_en)}</span></h2>"
                     + _sentence_rows(b, [sid for _s, sid, _f in story.lines], speaker=True))
    parts.append(_signoff(status))
    return _page(f"Review {src.ref} {src.title}", "".join(parts))


def story_page(b: Build, story, status: ReviewStatus) -> str:
    parts = [_header(b, status, f"{story.title_en} · {('dialogue', 'reading')[story.kind]}")]
    parts.append(_questions(_file_notes(story.src.data), [b.sentences[sid] for _s, sid, _f in story.lines]))
    parts.append("<h2>Text</h2>" + _sentence_rows(b, [sid for _s, sid, _f in story.lines],
                                                  speaker=any(s for s, _i, _f in story.lines)))
    if story.questions:
        rows = []
        for n, q in enumerate(story.questions, 1):
            opts = "".join(f"<li{' style=font-weight:bold' if i == q['answer'] else ''}>{_e(o)}</li>"
                           for i, o in enumerate(q["options"]))
            cls = "es" if q["spanish"] else ""
            rows.append(f"<tr><td class=\"n\">{n}</td><td class=\"{cls}\">{_e(q['text'])}<ol>{opts}</ol></td>"
                        "<td class=\"fix\"></td></tr>")
        parts.append("<h2>Questions</h2><p class=\"meta\">The right answer is in bold.</p><table><tr><th></th>"
                     f"<th>Question</th><th>Corrections</th></tr>{''.join(rows)}</table>")
    parts.append(_signoff(status))
    return _page(f"Review story {story.src.name}", "".join(parts))


def phrasebook_page(b: Build, cat_index: int, status: ReviewStatus) -> str:
    cat = b.phrase_categories[cat_index]
    parts = [_header(b, status, f"Phrasebook · {cat['title_en']}")]
    parts.append(_questions(_file_notes(cat["src"].data),
                            [b.sentences[e.sentence] for e in b.phrase_entries if e.category == cat_index]))
    rows = []
    for n, entry in enumerate(e for e in b.phrase_entries if e.category == cat_index):
        sent = b.sentences[entry.sentence]
        note = f"<div class=\"meta\">{_e(sent.note)}</div>" if sent.note else ""
        rows.append(f"<tr><td class=\"n\">{n + 1}</td><td><div class=\"es\">{_spanish(b, sent)}</div>"
                    f"<div class=\"pron\">{_e(entry.pron)}</div>{_links(sent)}{_ask_inline(sent)}</td>"
                    f"<td>{_e(sent.en)}{note}</td>"
                    "<td class=\"fix\"></td></tr>")
    parts.append("<h2>Phrases</h2><table><tr><th></th><th>Spanish</th><th>English</th><th>Corrections</th></tr>"
                 + "".join(rows) + "</table>")
    parts.append(_signoff(status))
    return _page(f"Review phrasebook {cat['src'].name}", "".join(parts))


def write_pages(b: Build, out_dir: str) -> list[str]:
    os.makedirs(out_dir, exist_ok=True)
    written = []
    by_name = {(s.kind, s.name): s for s in b.review_status}
    for lesson in b.lessons:
        status = by_name[("lesson", lesson.src.ref)]
        written.append(_write(out_dir, status.page, lesson_page(b, lesson, status)))
    for story in b.stories:
        status = by_name.get(("story", story.src.name))
        if status:
            written.append(_write(out_dir, status.page, story_page(b, story, status)))
    for i, cat in enumerate(b.phrase_categories):
        status = by_name[("phrasebook", cat["src"].name)]
        written.append(_write(out_dir, status.page, phrasebook_page(b, i, status)))
    for deck in b.deck_files:
        status = by_name[("deck", deck["src"].name)]
        written.append(_write(out_dir, status.page, deck_page(b, deck, status)))
    for path, lemmas in dictionary_files(b).items():
        status = by_name[("dictionary", os.path.splitext(os.path.basename(path))[0])]
        written.append(_write(out_dir, status.page, dictionary_page(b, lemmas, status)))
    written.append(_write(out_dir, "how-to-review.html", _page("How to review Tinta", HOW_TO)))
    rows = "".join(
        f"<tr><td>{_e(s.kind)}</td><td><a href=\"{_e(s.page)}\">{_e(s.name)}</a></td>"
        f"<td class=\"es\">{_e(s.title)}</td><td>{question_count(b, s) or ''}</td><td>{s.hash}</td>"
        f"<td{'' if s.ok else ' class=status-bad'}>{_e(s.reason)}</td></tr>" for s in b.review_status)
    index = (f"<h1>{_e(b.course.title)} — content review</h1><p><b>Reviewer: start with "
             "<a href=\"how-to-review.html\">How to review Tinta</a>.</b></p><p class=\"meta\">One page per "
             "lesson, story, phrasebook category, deck file and dictionary file. Dictionary pages are not "
             "required for release.</p><h2>How a review works</h2><ol>"
             "<li><b>Reviewer:</b> print a page, write corrections in the right-hand column, sign it.</li>"
             "<li><b>Author:</b> apply the corrections and run <code>python3 tools/packc --review</code> again. "
             "The corrected page has a new content hash.</li>"
             "<li><b>Reviewer:</b> check the corrected page.</li>"
             "<li><b>Author:</b> record the review with <code>python3 tools/packc --mark-reviewed u01.l02 "
             "--by \"Name\"</code> (lesson, story or phrasebook name from the table, or <code>all</code>). "
             "It stores the hash of the content as it is now, so never copy a hash from a page by hand.</li>"
             "<li>Any later edit changes the hash: the page shows the file as changed since review and "
             "<code>--release</code> refuses to build until it is reviewed again.</li></ol>"
             f"<table><tr><th>Kind</th><th>Page</th><th>Title</th><th>Questions</th><th>Hash</th><th>Status</th></tr>{rows}</table>")
    written.append(_write(out_dir, "index.html", _page("Tinta content review", index)))
    return written


def _write(out_dir: str, name: str, text: str) -> str:
    path = os.path.join(out_dir, name)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)
    return path
