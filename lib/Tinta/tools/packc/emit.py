"""Pack writer (docs/pack-format.md)."""

from __future__ import annotations

import os
import hashlib
import struct
import sys
import time
import zlib

from . import forms as F
from .build import NONE16, Build, english_keys
from .fold import fold
from .items import CLOZE, CONJUGATION, PHRASE, VOCAB_PRODUCE, VOCAB_RECOGNISE

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import charset  # noqa: E402

FORMAT_MAJOR, FORMAT_MINOR = 1, 1
HEADER_SIZE = 48
FLAG_RELEASE = 1

# Record layouts, little-endian, no implicit padding ("<").
LEMMA = struct.Struct("<8I4H4BHH")          # 48
LEMMA_KEY = struct.Struct("<IHH")           # 8
ENGLISH_KEY = struct.Struct("<IHBB")        # 8
FORM_KEY = struct.Struct("<IIHH")           # 12
VERB = struct.Struct("<40I3IHH")            # 176
SENTENCE = struct.Struct("<4IHBBBBBB")      # 24
TOKEN = struct.Struct("<HBBHH")             # 8
ITEM = struct.Struct("<IBBHHHHBBI")         # 20
ITEM_UID = struct.Struct("<IHH")            # 8
ITEM_IDENTITY = struct.Struct("<I32s")      # 36, optional IDEN section
UNIT = struct.Struct("<3I4H")               # 20
LESSON = struct.Struct("<3I9HBB")           # 32
NOTE = struct.Struct("<IIHHBBH")            # 16
NOTE_SPAN = struct.Struct("<IBBH")          # 8
STORY = struct.Struct("<3IHHHBBBBH")        # 24
STORY_LINE = struct.Struct("<IHH")          # 8
QUESTION = struct.Struct("<5IBBBB")         # 24
PHRASE_CAT = struct.Struct("<IIHH")         # 12
PHRASE_ENTRY = struct.Struct("<IHH")        # 8
CONFUSABLE = struct.Struct("<II6HHH")       # 24
HEADER = struct.Struct("<4sHHIIII8sIHHII")  # 48
DIRENT = struct.Struct("<4sIII")            # 16

SIZES = {"LEMM": LEMMA, "LKEY": LEMMA_KEY, "EKEY": ENGLISH_KEY, "FORM": FORM_KEY, "VERB": VERB,
         "SENT": SENTENCE, "TOKS": TOKEN, "ITEM": ITEM, "IUID": ITEM_UID, "UNIT": UNIT, "LESS": LESSON,
         "NOTE": NOTE, "NSPN": NOTE_SPAN, "STOR": STORY, "SLIN": STORY_LINE, "SQST": QUESTION,
         "PHRS": PHRASE_CAT, "PENT": PHRASE_ENTRY, "CONF": CONFUSABLE}
for _tag, _s in SIZES.items():
    assert _s.size % 4 == 0, _tag

ORDER = ("LEMM", "LKEY", "EKEY", "FORM", "VERB", "SENT", "TOKS", "ITEM", "IUID", "DIST", "UNIT", "LESS",
         "NOTE", "NSPN", "STOR", "SLIN", "SQST", "PHRS", "PENT", "CONF", "LEXS", "STRS")


class Strings:
    def __init__(self) -> None:
        self.heap = bytearray(b"\0")
        self.offsets: dict[str, int] = {"": 0}

    def __call__(self, text: str | None) -> int:
        if not text:
            return 0
        if text not in self.offsets:
            self.offsets[text] = len(self.heap)
            self.heap += charset.encode(text).encode("utf-8") + b"\0"
        return self.offsets[text]


def _encoded_len(text: str) -> int:
    return len(charset.encode(text).encode("utf-8"))


def emit(b: Build, release: bool = False, build_time: int | None = None) -> tuple[bytes, dict]:
    s = Strings()
    sec: dict[str, tuple[bytes, int]] = {}

    # LEMM / LEXS
    lexs: list[int] = []
    rows = []
    for lem in b.lemmas:
        first = len(lexs)
        lexs += lem.examples
        rows.append(LEMMA.pack(
            s(lem.es), s(lem.en), s(lem.pron), s(lem.entry.note), s(lem.entry.alt), s(lem.feminine),
            s(lem.plural), first, len(lem.examples), min(lem.entry.freq, 0xFFFF), lem.lesson, lem.verb_table,
            lem.pos, lem.gender, lem.level, lem.reg, lem.flags, 0))
    sec["LEMM"] = (b"".join(rows), len(rows))
    sec["LEXS"] = (struct.pack(f"<{len(lexs)}H", *lexs), len(lexs))

    # LKEY / EKEY / FORM
    lkey = sorted((charset.encode(lem.fold).encode(), lem.id) for lem in b.lemmas)
    sec["LKEY"] = (b"".join(LEMMA_KEY.pack(s(k.decode()), lid, 0) for k, lid in lkey), len(lkey))
    ekeys: dict[tuple[str, int], int] = {}
    for lem in b.lemmas:
        for key, rank in english_keys(lem.en):
            ekeys[(key, lem.id)] = min(rank, ekeys.get((key, lem.id), 9))
    ekey = sorted((k.encode(), rank, lid) for (k, lid), rank in ekeys.items())
    sec["EKEY"] = (b"".join(ENGLISH_KEY.pack(s(k.decode()), lid, rank, 0) for k, rank, lid in ekey), len(ekey))
    forms: set[tuple[str, int, int, str]] = set()
    from .build import _bare  # noqa: PLC0415
    for lem in b.lemmas:
        for form, tag in lem.forms:
            form = _bare(form)
            if form.lower() != lem.es.lower():
                forms.add((fold(form), lem.id, tag, form))
    for sent in b.sentences:
        for tok in sent.tokens:
            if tok.lemma is not None and tok.surface.lower() != tok.lemma.es.lower():
                surface = tok.surface if tok.flags & 8 else tok.surface.lower()
                forms.add((fold(surface), tok.lemma.id, tok.tag, surface))
    # One entry per spelling of a lemma, under its first tag: "hable" fills subj.1s, subj.3s,
    # imp.3s and impneg.3s, and the UI finds those cells in the verb table (pack-format.md 3.6).
    form_rows = sorted((k.encode(), lid, tag, form) for k, lid, tag, form in forms)
    dedup, seen = [], set()
    for k, lid, tag, form in form_rows:
        if (k, lid, form) not in seen:
            seen.add((k, lid, form))
            dedup.append(FORM_KEY.pack(s(k.decode()), s(form), lid, tag))
    sec["FORM"] = (b"".join(dedup), len(dedup))

    # VERB
    rows = []
    for lem in b.verbs:
        conj = lem.verb
        cells = []
        for tense in F.TENSES:
            for person in F.PERSONS:
                cells.append(s(conj.forms.get((tense, person), "")))
        flags = (1 if conj.irregular else 0) | (STEM_CLASSES.get(conj.stem, 0) << 1) \
            | (16 if conj.spelling else 0) | (32 if conj.reflexive else 0)
        rows.append(VERB.pack(*cells, s(conj.infinitive), s(conj.gerund), s(conj.participle), lem.id, flags))
    sec["VERB"] = (b"".join(rows), len(rows))

    # SENT / TOKS
    rows, toks = [], []
    for sent in b.sentences:
        first = len(toks)
        encoded = charset.encode(sent.es)
        for tok in sent.tokens:
            start = len(encoded[:tok.start].encode("utf-8"))
            length = len(encoded[tok.start:tok.end].encode("utf-8"))
            toks.append(TOKEN.pack(start, min(length, 255), tok.flags,
                                   tok.lemma.id if tok.lemma else NONE16, tok.tag))
        rows.append(SENTENCE.pack(s(sent.es), s(sent.en), s(sent.note), first, sent.lesson,
                                  len(sent.tokens), sent.level, sent.source, sent.flags, sent.reg, 0))
    sec["SENT"] = (b"".join(rows), len(rows))
    sec["TOKS"] = (b"".join(toks), len(toks))

    # ITEM / IUID / DIST
    rows, dist = [], []
    for item in b.items:
        first = len(dist)
        for cand in item.candidates:
            if item.kind in (VOCAB_RECOGNISE, VOCAB_PRODUCE, PHRASE):
                dist.append(int(cand))
            elif item.kind in (CLOZE, CONJUGATION):
                dist.append(s(cand))
        flags = (item.must_show & 3) | item.flags
        rows.append(ITEM.pack(item.uid, item.kind, len(item.candidates), item.lesson, item.a, item.b,
                              item.prereq, flags, 0, first))
    sec["ITEM"] = (b"".join(rows), len(rows))
    uids = sorted((item.uid, item.index) for item in b.items)
    sec["IUID"] = (b"".join(ITEM_UID.pack(u, i, 0) for u, i in uids), len(uids))
    sec["DIST"] = (struct.pack(f"<{len(dist)}I", *dist), len(dist))

    # UNIT / LESS / NOTE / NSPN
    rows = []
    for unit_index, unit in enumerate(b.course.units):
        lessons = [l for l in b.lessons if l.unit == unit_index]
        first = lessons[0].id if lessons else 0
        rows.append(UNIT.pack(s(unit.title), s(unit.title_en), s("\n".join(unit.goals)), unit.number, first,
                              len(lessons), 0))
    sec["UNIT"] = (b"".join(rows), len(rows))
    rows = []
    for l in b.lessons:
        first_sent = l.sentences[0] if l.sentences else 0
        first_note = l.notes[0] if l.notes else 0
        rows.append(LESSON.pack(s(l.src.title), s(l.src.title_en), l.first_item, l.item_count, l.unit,
                                l.src.number, first_note, len(l.notes), l.dialogue, first_sent,
                                len(l.sentences), len(l.new), l.level, 1 if l.reviewed else 0))
    sec["LESS"] = (b"".join(rows), len(rows))
    rows, spans = [], []
    for note in b.notes:
        first = len(spans)
        for style, text in note.spans:
            spans.append(NOTE_SPAN.pack(s(text), style, 0, 0))
        rows.append(NOTE.pack(s(note.title), first, len(note.spans), note.lesson, note.kind, 0, 0))
    sec["NOTE"] = (b"".join(rows), len(rows))
    sec["NSPN"] = (b"".join(spans), len(spans))

    # STOR / SLIN / SQST
    rows, lines, questions = [], [], []
    for story in b.stories:
        first_line, first_q = len(lines), len(questions)
        for speaker, sid, flags in story.lines:
            lines.append(STORY_LINE.pack(s(speaker), sid, flags))
        for q in story.questions:
            opts = [s(o) for o in q["options"][:4]] + [0] * (4 - min(4, len(q["options"])))
            questions.append(QUESTION.pack(s(q["text"]), *opts, q["answer"], min(4, len(q["options"])),
                                           1 if q["spanish"] else 0, 0))
        rows.append(STORY.pack(s(story.title), s(story.title_en), first_line, len(story.lines), story.lesson,
                               first_q, len(story.questions), story.level, story.kind, 0, 0))
    sec["STOR"] = (b"".join(rows), len(rows))
    sec["SLIN"] = (b"".join(lines), len(lines))
    sec["SQST"] = (b"".join(questions), len(questions))

    # PHRS / PENT / CONF
    sec["PHRS"] = (b"".join(PHRASE_CAT.pack(s(c["title"]), s(c["title_en"]), c["first"], c["count"])
                            for c in b.phrase_categories), len(b.phrase_categories))
    sec["PENT"] = (b"".join(PHRASE_ENTRY.pack(s(e.pron), e.sentence, e.category) for e in b.phrase_entries),
                   len(b.phrase_entries))
    rows = []
    for conf in b.confusables:
        ids = [m.id for m in conf["members"]][:6]
        rows.append(CONFUSABLE.pack(s(conf["name"]), s(conf["note"]), *(ids + [NONE16] * (6 - len(ids))),
                                    len(ids), 0))
    sec["CONF"] = (b"".join(rows), len(rows))

    sec["STRS"] = (bytes(s.heap), len(s.offsets))
    identities = sorted((uid, key) for key, uid in b.id_lock.by_key.items())
    sec["IDEN"] = (b"".join(ITEM_IDENTITY.pack(uid, hashlib.sha256(
        b"Tinta item identity v1\0" + key.encode("utf-8")).digest())
        for uid, key in identities), len(identities))

    # Assemble.
    order = ORDER + ("IDEN",)
    directory_size = DIRENT.size * len(order)
    offset = HEADER_SIZE + directory_size
    layout = []
    for tag in order:
        data, count = sec[tag]
        offset = (offset + 3) & ~3
        layout.append((tag, offset, data, count))
        offset += len(data)
    total = (offset + 3) & ~3
    out = bytearray(total)
    dirents = b"".join(DIRENT.pack(tag.encode(), off, len(data), count) for tag, off, data, count in layout)
    out[HEADER_SIZE:HEADER_SIZE + directory_size] = dirents
    for _tag, off, data, _count in layout:
        out[off:off + len(data)] = data
    if build_time is None:
        build_time = int(os.environ.get("SOURCE_DATE_EPOCH", time.time()))
    header = HEADER.pack(b"TNTA", FORMAT_MAJOR, FORMAT_MINOR, b.course.version, build_time, total, 0,
                         b.course.locale.encode()[:8].ljust(8, b"\0"), HEADER_SIZE, len(order), HEADER_SIZE,
                         FLAG_RELEASE if release else 0, 0)
    out[:HEADER_SIZE] = header
    crc = zlib.crc32(bytes(out))
    struct.pack_into("<I", out, 20, crc)
    report = {tag: (count, len(data)) for tag, _off, data, count in layout}
    return bytes(out), report


STEM_CLASSES = {"": 0, "e>ie": 1, "o>ue": 2, "e>i": 3, "u>ue": 4, "i>ie": 5}
