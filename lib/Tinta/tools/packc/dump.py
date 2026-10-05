"""Human-readable listing of a pack, read back from its bytes."""

from __future__ import annotations

import os
import sys

from . import forms as F
from .items import KIND_NAMES
from .reader import NONE16, Pack
from .sources import POS_NAMES

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import charset  # noqa: E402

REG = ("", "formal", "informal", "vulgar")
GENDER = ("", "m", "f", "m/f")


def dump(pack: Pack) -> str:
    out: list[str] = []
    S = lambda off: charset.decode(pack.str(off))  # noqa: E731
    w = out.append
    w(f"Tinta pack format {pack.major}.{pack.minor}, content {pack.content_version}, locale {pack.locale}, "
      f"{len(pack.data):,} bytes, crc {'ok' if pack.crc_ok() else 'BAD'}, "
      f"{'release' if pack.flags & 1 else 'development'} build")

    units = pack.records("UNIT")
    lessons = pack.records("LESS")
    items = pack.records("ITEM")
    sentences = pack.records("SENT")
    lemmas = pack.records("LEMM")
    toks = pack.records("TOKS")
    dist = pack.array("DIST")

    def sentence_text(sid: int) -> str:
        sent = sentences[sid]
        raw = pack.str(sent["es"]).encode()
        parts, pos = [], 0
        for t in toks[sent["firstToken"]:sent["firstToken"] + sent["tokenCount"]]:
            parts.append(raw[pos:t["start"]].decode())
            word = raw[t["start"]:t["start"] + t["length"]].decode()
            parts.append(f"[{word}]" if t["flags"] & 1 else word)
            pos = t["start"] + t["length"]
        parts.append(raw[pos:].decode())
        return charset.decode("".join(parts))

    def item_text(item: dict) -> str:
        kind = item["kind"]
        cands = dist[item["firstCandidate"]:item["firstCandidate"] + item["candidateCount"]]
        if kind in (0, 1, 4):
            lem = lemmas[item["a"]]
            body = S(lem["es"]) + " = " + S(lem["en"])
            if kind == 0:
                opts = [S(lemmas[c]["en"]) for c in cands]
            elif kind == 1:
                opts = [S(lemmas[c]["es"]) for c in cands]
            else:
                opts = []
        elif kind in (2, 6):
            body = sentence_text(item["a"])
            if kind == 2:
                body += f"  (token {item['b']})"
            opts = [S(c) for c in cands] if kind == 2 else []
        elif kind == 3:
            body = f"{S(lemmas[item['a']]['es'])} {F.tag_name(item['b'])}"
            opts = [S(c) for c in cands]
        else:
            body = sentence_text(item["a"])
            opts = [S(sentences[c]["es"]) for c in cands]
        must = item["flags"] & 3
        pre = "" if item["prereq"] == NONE16 else f" after #{item['prereq']}"
        line = f"uid {item['uid']:<5} {KIND_NAMES[kind]:<11} {body}{pre}"
        if opts:
            line += "\n" + " " * 24 + "options: " + " | ".join(("*" if i < must else "") + o for i, o in enumerate(opts))
        return line

    w("\n== Course ==")
    for u_index, unit in enumerate(units):
        w(f"Unit {unit['number']}: {S(unit['title'])} ({S(unit['titleEn'])})")
        for l_index in range(unit["firstLesson"], unit["firstLesson"] + unit["lessonCount"]):
            les = lessons[l_index]
            w(f"  Lesson {unit['number']}.{les['number']}: {S(les['title'])} ({S(les['titleEn'])}) "
              f"{les['itemCount']} items, {les['newCount']} new, "
              f"{'reviewed' if les['flags'] & 1 else 'NOT REVIEWED'}")
            for i in range(les["firstItem"], les["firstItem"] + les["itemCount"]):
                w(f"    {i:>4} " + item_text(items[i]))
    rest = [i for i, it in enumerate(items) if it["lesson"] == NONE16]
    if rest:
        w("  Outside lessons:")
        for i in rest:
            w(f"    {i:>4} " + item_text(items[i]))

    w("\n== Lexicon ==")
    lexs = pack.array("LEXS")
    for lid, lem in enumerate(lemmas):
        tags = [POS_NAMES.get(lem["pos"], "?"), GENDER[lem["gender"]], f"A{lem['level']}" if lem["level"] <= 2
                else f"B{lem['level'] - 2}", REG[lem["reg"]], "MX" if lem["flags"] & 1 else ""]
        w(f"{lid:>5} {S(lem['es'])}  [{S(lem['pron'])}]  {' '.join(t for t in tags if t)}  = {S(lem['en'])}")
        extra = [S(lem[k]) for k in ("feminine", "plural") if lem[k]]
        if extra:
            w(f"      forms: {', '.join(extra)}")
        if lem["note"]:
            w(f"      note: {S(lem['note'])}")
        for sid in lexs[lem["firstExample"]:lem["firstExample"] + lem["exampleCount"]][:2]:
            w(f"      e.g. {sentence_text(sid)}")
        if lem["verbTable"] != NONE16:
            v = pack.verb(lem["verbTable"])
            for t, tense in enumerate(F.TENSES):
                cells = [S(o) for o in v["forms"][t]]
                w(f"      {tense:<7} " + ", ".join(c or "-" for c in cells))
            w(f"      gerund {S(v['gerund'])}, participle {S(v['participle'])}, flags {v['flags']:#x}")

    w("\n== Stories ==")
    lines = pack.records("SLIN")
    for st in pack.records("STOR"):
        w(f"{S(st['title'])} ({S(st['titleEn'])}) lesson {st['lesson'] if st['lesson'] != NONE16 else '-'}")
        for ln in lines[st["firstLine"]:st["firstLine"] + st["lineCount"]]:
            who = S(ln["speaker"])
            w(f"   {who + ': ' if who else ''}{sentence_text(ln['sentence'])}")
        for q in range(st["firstQuestion"], st["firstQuestion"] + st["questionCount"]):
            qq = pack.question(q)
            w(f"   ? {S(qq['text'])} -> " + " | ".join(
                ("*" if i == qq["answer"] else "") + S(o) for i, o in enumerate(qq["options"][:qq["optionCount"]])))

    w("\n== Phrasebook ==")
    entries = pack.records("PENT")
    for cat in pack.records("PHRS"):
        w(f"{S(cat['title'])} ({S(cat['titleEn'])})")
        for e in entries[cat["firstEntry"]:cat["firstEntry"] + cat["entryCount"]]:
            w(f"   {sentence_text(e['sentence'])}  [{S(e['pron'])}]  = {S(sentences[e['sentence']]['en'])}")

    w("\n== Confusables ==")
    for i in range(pack.count("CONF")):
        c = pack.confusable(i)
        names = [S(lemmas[m]["es"]) for m in c["members"][:c["memberCount"]]]
        w(f"{S(c['name'])}: {', '.join(names)} — {S(c['note'])}")
    return "\n".join(out) + "\n"


def size_report(pack: Pack) -> str:
    total = len(pack.data)
    rows = [f"{'section':<8}{'count':>8}{'bytes':>11}{'share':>8}"]
    for tag, (off, sz, n) in sorted(pack.sections.items(), key=lambda kv: -kv[1][1]):
        rows.append(f"{tag:<8}{n:>8,}{sz:>11,}{100 * sz / total:>7.1f}%")
    rows.append(f"{'total':<8}{'':>8}{total:>11,}")
    return "\n".join(rows)
