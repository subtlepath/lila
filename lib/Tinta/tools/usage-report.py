#!/usr/bin/env python3
"""Decodes a device's usage log and writes a report (PLAN.md 8.6).

    python3 tools/usage-report.py /Volumes/CARD                 # report into ./usage-report/
    python3 tools/usage-report.py /Volumes/CARD --out ~/tinta-week1
    python3 tools/usage-report.py SD --ids content/ids.lock --dump build/course.dump.txt

SD is the card's root (the directory holding tinta/) or tinta/ itself. Every
usage-NNNN.log there is read in order. Written to --out:

    report.md        the analyses below
    events.jsonl     every record, one JSON object per line
    csv/<type>.csv   the same, one file per record type

Words are named from content/ids.lock (uid -> key) and, with --dump, from a
pack listing (python3 tools/packc --dump); screens from src/app/View.h. All
three default to this checkout's files when they exist.

The record layout is src/core/usage/UsageLog.h; docs/usage-log.md describes
each record. Standard library only.
"""

from __future__ import annotations

import argparse
import csv
import datetime
import json
import os
import re
import statistics
import struct
import sys
import zlib
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EPOCH = datetime.datetime(2024, 1, 1)  # Clock::nowSeconds() counts from here, local time
CHUNK_HEADER = 16
RECORD_HEADER = 6
FILE_RE = re.compile(r"^usage-(\d{4})\.log$")

# ---- The vocabulary (src/core/usage/UsageLog.h) -----------------------------

ENUMS = {
    "reason": ["power-on", "wake-key", "wake-timer", "restart", "file-start", "enabled"],
    "input": ["back", "confirm", "left", "right", "up", "down", "power", "home", "back-hold", "home-hold",
              "tap", "swipe"],
    "outcome": ["handled", "ignored", "queued"],
    "refresh": ["full", "half", "fast", "window"],
    "sleep_cause": ["key", "idle", "menu", "low-battery", "rotation"],
    "clock_kind": ["set", "day-confirmed", "time-zone", "day-rollover"],
    "error": {0: "card-failed", 1: "pack-error", 2: "progress-guest", 3: "session-lost", 255: "other"},
    "session_kind": {0: "today", 1: "lesson", 2: "phrases", 3: "starred", 255: "other"},
    "end_how": ["finished", "ended", "slept", "left", "dropped"],
    "correct": ["wrong", "right", "near", "self-graded"],
    "page_kind": {0: "cover", 1: "note", 2: "dialogue", 3: "word", 4: "practice", 255: "other"},
    "source": {0: "reader", 1: "dictionary", 2: "lesson", 3: "session", 4: "search", 255: "other"},
    "search_mode": ["spanish", "english", "letter"],
    "phrase_action": ["opened", "practised"],
    # ui::ExerciseFormat, the journal's format codes (append-only).
    "format": ["unknown", "flashcard-recognise", "flashcard-produce", "reveal-card", "choose-meaning",
               "choose-word", "choose-gap", "choose-article", "choose-form", "build-sentence", "type-word",
               "type-gap", "type-form"],
    # core::ItemKind.
    "item_kind": ["recognise", "produce", "cloze", "conjugation", "gender", "phrase", "word-order"],
    "how": ["push", "pop", "reset", "restored"],
}

# type: (name, [(field, kind)]). Kinds: u8 u16 u32 i32 text options, or an
# ENUMS key (a u8 shown by name).
TYPES = {
    1: ("boot", [("reason", "reason"), ("device", "text"), ("build", "text"), ("version", "text"),
                 ("pack_edition", "u32"), ("pack_major", "u8"), ("pack_minor", "u8"), ("pack_crc", "hex32"),
                 ("pack_build_time", "u32"), ("day", "u16")]),
    2: ("dropped", [("count", "u32")]),
    3: ("log_state", [("on", "u8")]),
    4: ("sleep", [("cause", "sleep_cause"), ("battery", "u8"), ("charging", "u8"), ("wake_after_s", "u32")]),
    5: ("battery", [("percent", "u8"), ("flags", "u8")]),
    6: ("clock_change", [("kind", "clock_kind"), ("before", "u32"), ("after", "u32"), ("day", "u16")]),
    7: ("error", [("code", "error"), ("detail", "u32")]),
    8: ("setting", [("value", "i32"), ("name", "text")]),
    16: ("screen", [("screen", "screen"), ("depth", "u8"), ("how", "how")]),
    17: ("input", [("input", "input"), ("outcome", "outcome"), ("screen", "screen"), ("x", "u16"), ("y", "u16")]),
    18: ("frame", [("refresh", "refresh"), ("latency_ms", "u16"), ("present_ms", "u16"), ("screen", "screen")]),
    32: ("session_start", [("kind", "session_kind"), ("tag", "u16"), ("planned", "u16"), ("due", "u16"),
                           ("fresh", "u16"), ("resumed", "u8")]),
    33: ("session_end", [("how", "end_how"), ("done", "u16"), ("correct", "u16"), ("seconds", "u32")]),
    34: ("item_shown", [("uid", "u32"), ("format", "format"), ("kind", "item_kind"), ("lesson", "u16"),
                        ("reps", "u8"), ("answer", "u8"), ("options", "options")]),
    35: ("answer", [("uid", "u32"), ("format", "format"), ("chosen", "u8"), ("correct", "correct"),
                    ("grade", "u8"), ("response_ms", "u32"), ("attempts", "u8"), ("typed", "text")]),
    36: ("reveal", [("uid", "u32")]),
    37: ("undo", [("uid", "u32")]),
    38: ("lesson_page", [("lesson", "u16"), ("page", "u16"), ("page_count", "u16"), ("page_kind", "page_kind")]),
    39: ("dialogue_english", [("lesson", "u16"), ("line", "u16"), ("shown", "u8")]),
    48: ("story_open", [("story", "hex32"), ("page_count", "u16")]),
    49: ("story_page", [("story", "hex32"), ("page", "u16"), ("page_count", "u16")]),
    50: ("story_done", [("story", "hex32"), ("right", "u8"), ("total", "u8")]),
    51: ("gloss", [("lemma", "u16"), ("source", "source"), ("word", "text")]),
    52: ("sentence_english", [("where", "hex32"), ("sentence", "u16"), ("source", "source")]),
    53: ("star", [("uid", "u32"), ("on", "u8"), ("word", "text")]),
    54: ("quiz_answer", [("story", "hex32"), ("question", "u8"), ("chosen", "u8"), ("right", "u8")]),
    55: ("search", [("mode", "search_mode"), ("results", "u16"), ("text", "text")]),
    56: ("entry", [("lemma", "u16"), ("via", "source"), ("headword", "text")]),
    57: ("verb_table", [("lemma", "u16"), ("tense", "u8")]),
    58: ("phrasebook", [("category", "u16"), ("action", "phrase_action")]),
}
TYPE_NAMES = {code: name for code, (name, _) in TYPES.items()}
BOOT_REASONS = {"power-on", "wake-key", "wake-timer", "restart"}


def name_of(table, value):
    names = ENUMS[table]
    if isinstance(names, dict):
        return names.get(value, value)
    return names[value] if 0 <= value < len(names) else value


# ---- Decoding ----------------------------------------------------------------

class Short(Exception):
    """A record ends before a field: an older firmware wrote fewer fields."""


def read_fields(spec, data, screens):
    out = {}
    at = 0

    def take(n):
        nonlocal at
        if at + n > len(data):
            raise Short
        chunk = data[at:at + n]
        at += n
        return chunk

    try:
        for field, kind in spec:
            if kind == "u8":
                out[field] = take(1)[0]
            elif kind == "u16":
                out[field] = struct.unpack("<H", take(2))[0]
            elif kind == "u32":
                out[field] = struct.unpack("<I", take(4))[0]
            elif kind == "hex32":
                out[field] = f"{struct.unpack('<I', take(4))[0]:08x}"
            elif kind == "i32":
                out[field] = struct.unpack("<i", take(4))[0]
            elif kind == "text":
                out[field] = take(take(1)[0]).decode("utf-8", "replace")
            elif kind == "options":
                count = take(1)[0]
                out[field] = [take(take(1)[0]).decode("utf-8", "replace") for _ in range(count)]
            elif kind == "screen":
                code = take(1)[0]
                out[field] = screens.get(code, code)
            else:
                out[field] = name_of(kind, take(1)[0])
    except Short:
        pass
    return out


def chunks(data):
    """(offset, flags, wall, uptime, body) per good chunk, and the bytes skipped."""
    found = []
    skipped = 0
    at = 0
    while at + CHUNK_HEADER <= len(data):
        if data[at:at + 2] != b"TU":
            nxt = data.find(b"TU", at + 1)
            nxt = len(data) if nxt < 0 else nxt
            skipped += nxt - at
            at = nxt
            continue
        length, check, flags, _layout, wall, uptime = struct.unpack_from("<HHBBII", data, at + 2)
        body = data[at + CHUNK_HEADER:at + CHUNK_HEADER + length]
        if len(body) != length or (zlib.crc32(body) & 0xFFFF) != check:
            skipped += 1
            at += 1
            continue
        found.append((at, flags, wall, uptime, body))
        at += CHUNK_HEADER + length
    skipped += len(data) - at if at < len(data) else 0
    return found, skipped


def wall_time(seconds):
    return (EPOCH + datetime.timedelta(seconds=seconds)).isoformat(timespec="seconds")


def decode_files(paths, screens):
    """Every record of every file, in order, as dicts; and decoding statistics."""
    events = []
    stats = {"files": [], "chunks": 0, "skipped_bytes": 0, "unknown_types": Counter()}
    boot = 0
    in_boot = False
    for path in paths:
        with open(path, "rb") as fh:
            data = fh.read()
        found, skipped = chunks(data)
        stats["files"].append({"file": os.path.basename(path), "bytes": len(data), "chunks": len(found),
                               "skipped_bytes": skipped})
        stats["chunks"] += len(found)
        stats["skipped_bytes"] += skipped
        for _offset, flags, wall, uptime, body in found:
            at = 0
            while at + RECORD_HEADER <= len(body):
                code, size, rec_uptime = struct.unpack_from("<BBI", body, at)
                fields_data = body[at + RECORD_HEADER:at + RECORD_HEADER + size]
                at += RECORD_HEADER + size
                if code in TYPES:
                    name, spec = TYPES[code]
                    fields = read_fields(spec, fields_data, screens)
                else:
                    name, fields = f"type-{code}", {}
                    stats["unknown_types"][code] += 1
                if name == "boot":
                    reason = fields.get("reason")
                    if reason in BOOT_REASONS or not in_boot:
                        boot += 1
                        in_boot = True
                # The chunk's wall clock, carried back to when the record was made.
                when = wall - (((uptime - rec_uptime) & 0xFFFFFFFF) // 1000)
                event = {"file": os.path.basename(path), "boot": boot, "type": name,
                         "uptime_ms": rec_uptime, "wall": when, "wall_iso": wall_time(when),
                         "time_of_day": bool(flags & 1)}
                event.update(fields)
                events.append(event)
    return events, stats


def log_files(sd):
    """The usage logs under SD (the card root or its tinta/), oldest first."""
    folder = os.path.join(sd, "tinta") if os.path.isdir(os.path.join(sd, "tinta")) else sd
    names = sorted((int(m.group(1)), n) for n in os.listdir(folder) if (m := FILE_RE.match(n)))
    # Numbers wrap after 9999: a gap means the newer run starts after it.
    numbers = [n for n, _ in names]
    if numbers and numbers[-1] - numbers[0] >= 9000:
        split = next(i for i in range(1, len(numbers)) if numbers[i] - numbers[i - 1] > 1000)
        names = names[split:] + names[:split]
    return [os.path.join(folder, n) for _, n in names]


# ---- Names for uids and screens ------------------------------------------------

def load_ids(path):
    words = {}
    if not path or not os.path.exists(path):
        return words
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("#") or not line.strip():
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 2 and parts[0].isdigit():
                words[int(parts[0])] = parts[1]
    return words


def load_dump(path):
    labels = {}
    if not path or not os.path.exists(path):
        return labels
    pattern = re.compile(r"^\s*\d+\s+uid\s+(\d+)\s+\S+\s+(.*)$")
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            m = pattern.match(line)
            if m:
                labels[int(m.group(1))] = m.group(2).strip()
    return labels


def load_screens(path):
    if not path or not os.path.exists(path):
        return {}
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    m = re.search(r"enum class ScreenId\s*:\s*uint8_t\s*\{(.*?)\};", text, re.S)
    if not m:
        return {}
    names = [re.sub(r"//.*", "", n).strip() for n in m.group(1).split(",")]
    names = [n.split("=")[0].strip() for n in names if n and not n.startswith("//")]
    return {i: n for i, n in enumerate(n for n in names if n)}


class Names:
    def __init__(self, ids, dump):
        self.ids = ids
        self.dump = dump

    def item(self, uid):
        if uid in self.dump:
            return self.dump[uid]
        if uid in self.ids:
            return self.ids[uid]
        return f"uid {uid}"


# ---- Report --------------------------------------------------------------------

def table(headers, rows):
    lines = ["| " + " | ".join(headers) + " |", "|" + "---|" * len(headers)]
    for row in rows:
        lines.append("| " + " | ".join(str(c).replace("|", "\\|") for c in row) + " |")
    return "\n".join(lines) if rows else "_none_"


def median(values):
    return round(statistics.median(values)) if values else ""


def p90(values):
    if not values:
        return ""
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(0.9 * len(ordered)))]


def pct(part, whole):
    return f"{100 * part / whole:.0f} %" if whole else ""


def report(events, stats, names):
    out = []
    w = out.append
    by_type = Counter(e["type"] for e in events)
    boots = [e for e in events if e["type"] == "boot" and e["reason"] in BOOT_REASONS]
    w("# Tinta usage report\n")
    span = (events[0]["wall_iso"], events[-1]["wall_iso"]) if events else ("", "")
    w(f"{len(events)} records from {len(stats['files'])} file(s), {span[0]} to {span[1]}; "
      f"{len(boots)} boot(s); {sum(e['count'] for e in events if e['type'] == 'dropped')} record(s) "
      f"reported dropped; {stats['skipped_bytes']} damaged byte(s) skipped.\n")
    devices = sorted({tuple(str(e.get(k, "")) for k in ("device", "build", "version", "pack_edition"))
                      for e in events if e["type"] == "boot"})
    w(table(["device", "build", "version", "pack edition"], devices))
    w("")

    # ---- Learning ----
    shown = {}  # uid -> the last item_shown
    answers = []
    for e in events:
        if e["type"] == "item_shown":
            shown[e["uid"]] = e
        elif e["type"] == "answer":
            e = dict(e)
            item = shown.get(e["uid"], {})
            e["lesson"] = item.get("lesson")
            e["options"] = item.get("options", [])
            e["right_option"] = item.get("answer")
            answers.append(e)
    w("## Learning\n")
    w("### By format\n")
    rows = []
    for fmt, group in sorted(_group(answers, "format").items(), key=lambda kv: str(kv[0])):
        right = sum(1 for a in group if a["correct"] in ("right", "near") or
                    (a["correct"] == "self-graded" and a["grade"] >= 3))
        rows.append([fmt, len(group), pct(right, len(group)), median([a["response_ms"] for a in group])])
    w(table(["format", "answers", "right", "median ms"], rows))
    w("\n### By lesson\n")
    rows = []
    for lesson, group in sorted(_group(answers, "lesson").items(), key=lambda kv: (kv[0] is None, kv[0] or 0)):
        right = sum(1 for a in group if a["correct"] in ("right", "near"))
        label = "outside lessons" if lesson in (None, 0xFFFF) else lesson
        rows.append([label, len(group), pct(right, len(group)), median([a["response_ms"] for a in group])])
    w(table(["lesson", "answers", "right", "median ms"], rows))
    w("\n### Hardest words (two answers or more)\n")
    rows = []
    for uid, group in _group(answers, "uid").items():
        if len(group) < 2:
            continue
        right = sum(1 for a in group if a["correct"] in ("right", "near"))
        rows.append((right / len(group), -len(group), uid, len(group), right))
    rows.sort()
    w(table(["item", "answers", "right"], [[names.item(uid), n, pct(r, n)] for _, _, uid, n, r in rows[:20]]))
    w("\n### Wrong options chosen most\n")
    wrong = Counter()
    for a in answers:
        if a["correct"] == "wrong" and a["chosen"] != 0xFF and a["chosen"] < len(a["options"]):
            right = a["options"][a["right_option"]] if a["right_option"] is not None and \
                a["right_option"] < len(a["options"]) else ""
            wrong[(a["uid"], right, a["options"][a["chosen"]])] += 1
    w(table(["item", "right answer", "chosen instead", "times"],
            [[names.item(uid), right, chosen, n] for (uid, right, chosen), n in wrong.most_common(20)]))
    w("\n### Typed answers not quite right\n")
    typed = Counter((names.item(a["uid"]), a["typed"], a["correct"]) for a in answers
                    if a.get("typed") and a["correct"] in ("wrong", "near"))
    w(table(["item", "typed", "result", "times"], [[i, t, c, n] for (i, t, c), n in typed.most_common(20)]))
    w("\n### Sessions\n")
    starts = Counter((e["kind"], bool(e.get("resumed"))) for e in events if e["type"] == "session_start")
    # A session set aside for sleep ends "slept" with its totals so far and
    # starts again, resumed, at the wake: its last end is the one that counts.
    ends = [e for e in events if e["type"] == "session_end" and not (e["how"] == "slept" and _resumes(events, e))]
    kinds = sorted({k for k, _ in starts}, key=str)
    w(table(["kind", "started", "resumed after a wake"], [[k, starts[(k, False)], starts[(k, True)]] for k in kinds]))
    w("")
    w(table(["ended", "sessions", "items done (median)", "minutes (median)"],
            [[how, len(g), median([e["done"] for e in g]), median([e["seconds"] / 60 for e in g])]
             for how, g in sorted(_group(ends, "how").items(), key=lambda kv: str(kv[0]))]))
    w("\n### Lessons: furthest page reached and time per page\n")
    pages = [e for e in events if e["type"] == "lesson_page"]
    dwell = _dwell(events, "lesson_page")
    rows = []
    for lesson, group in sorted(_group(pages, "lesson").items()):
        furthest = max(g["page"] for g in group)
        count = max(g["page_count"] for g in group)
        times = [dwell[id(g)] for g in group if id(g) in dwell]
        rows.append([lesson, f"{furthest}/{count}", len(group), median(times)])
    w(table(["lesson", "furthest page", "page views", "median ms per page"], rows))
    english = sum(1 for e in events if e["type"] == "dialogue_english" and e["shown"])
    w(f"\nDialogue English shown {english} time(s).\n")

    # ---- Reading and lookup ----
    w("## Reading and lookup\n")
    w("### Words glossed most\n")
    glossed = Counter(e["word"] for e in events if e["type"] == "gloss")
    w(table(["word", "times"], glossed.most_common(25)))
    w("\n### Searches\n")
    searches = [e for e in events if e["type"] == "search"]
    w(table(["text", "mode", "times", "results"],
            [[t, m, n, r] for (t, m, r), n in Counter((e["text"], e["mode"], e["results"])
                                                      for e in searches).most_common(25)]))
    w("\n### Searches that found nothing (candidate headwords)\n")
    empty = Counter((e["text"], e["mode"]) for e in searches if e["results"] == 0 and e["text"])
    w(table(["text", "mode", "times"], [[t, m, n] for (t, m), n in empty.most_common(25)]))
    w("\n### Entries opened and stars\n")
    w(table(["headword", "times"], Counter(e["headword"] for e in events if e["type"] == "entry").most_common(15)))
    w("")
    w(table(["starred", "times"], Counter(e["word"] for e in events if e["type"] == "star" and e["on"])
            .most_common(15)))
    opened = {e["story"] for e in events if e["type"] == "story_open"}
    done = {e["story"] for e in events if e["type"] == "story_done"}
    quiz = [e for e in events if e["type"] == "quiz_answer"]
    w(f"\nReadings opened {len(opened)}, finished {len(done & opened)}; quiz answers right "
      f"{pct(sum(e['right'] for e in quiz), len(quiz)) or 'n/a'} of {len(quiz)}.\n")

    # ---- Interface ----
    w("## Interface\n")
    w("### Time on each screen\n")
    screens = [e for e in events if e["type"] == "screen"]
    sdwell = _dwell(events, "screen")
    rows = []
    for screen, group in sorted(_group(screens, "screen").items(), key=lambda kv: str(kv[0])):
        times = [sdwell[id(g)] for g in group if id(g) in sdwell]
        rows.append([screen, len(group), median(times), round(sum(times) / 60000, 1) if times else ""])
    w(table(["screen", "visits", "median ms", "total min"], rows))
    w("\n### Paths (screen to screen)\n")
    paths = Counter()
    for boot, group in _group(screens, "boot").items():
        for a, b in zip(group, group[1:]):
            paths[(a["screen"], b["screen"])] += 1
    w(table(["from", "to", "times"], [[a, b, n] for (a, b), n in paths.most_common(20)]))
    w("\n### Presses that did nothing, and presses during a refresh\n")
    inputs = [e for e in events if e["type"] == "input"]
    rows = Counter((e["screen"], e["input"], e["outcome"]) for e in inputs if e["outcome"] != "handled")
    w(table(["screen", "input", "outcome", "times"], [[s, i, o, n] for (s, i, o), n in rows.most_common(25)]))
    total = len(inputs)
    w(f"\n{total} input(s): {pct(sum(1 for e in inputs if e['outcome'] == 'ignored'), total) or 'n/a'} did nothing, "
      f"{pct(sum(1 for e in inputs if e['outcome'] == 'queued'), total) or 'n/a'} waited for a refresh.\n")
    w("### Refreshes\n")
    frames = [e for e in events if e["type"] == "frame"]
    rows = []
    for kind, group in sorted(_group(frames, "refresh").items(), key=lambda kv: str(kv[0])):
        lat = [g["latency_ms"] for g in group if g["latency_ms"] != 0xFFFF]
        rows.append([kind, len(group), median(lat), p90(lat), median([g["present_ms"] for g in group])])
    w(table(["refresh", "frames", "press to frame, median ms", "p90 ms", "present median ms"], rows))

    # ---- Device ----
    w("\n## Device\n")
    w(table(["boot reason", "times"], Counter(e["reason"] for e in events if e["type"] == "boot").most_common()))
    w("")
    w(table(["sleep cause", "times"], Counter(e["cause"] for e in events if e["type"] == "sleep").most_common()))
    w("\n### Battery use (stretches of a quarter hour or more)\n")
    awake, asleep = _battery(events)
    w(table(["while", "hours", "percent used", "% per hour"],
            [[label, f"{h:.1f}", used, f"{used / h:.2f}" if h else ""] for label, (h, used) in
             (("awake", awake), ("asleep or off", asleep)) if h >= 0.25]))
    errors = Counter((e["code"], e["detail"]) for e in events if e["type"] == "error")
    w("\n### Errors and settings\n")
    w(table(["error", "detail", "times"], [[c, d, n] for (c, d), n in errors.most_common()]))
    w("")
    w(table(["setting", "value", "when"], [[e["name"], e["value"], e["wall_iso"]] for e in events
                                            if e["type"] == "setting"]))
    w("\n### Records\n")
    w(table(["type", "records"], sorted(by_type.items())))
    if stats["unknown_types"]:
        w(f"\nUnknown record types (newer firmware?), skipped: {dict(stats['unknown_types'])}")
    return "\n".join(out) + "\n"


def _resumes(events, end):
    """True when the session that `end` set aside for sleep was resumed later."""
    after = False
    for e in events:
        if e is end:
            after = True
        elif after and e["type"] == "session_start":
            return bool(e.get("resumed"))
        elif after and e["type"] == "session_end":
            return False
    return False


def _group(items, key):
    groups = defaultdict(list)
    for item in items:
        groups[item.get(key)].append(item)
    return groups


def _dwell(events, kind):
    """ms from each `kind` record to the next screen or page change in the same boot."""
    out = {}
    pending = None
    for e in events:
        if pending is not None and (e["type"] in ("screen", "lesson_page", "sleep", "boot") or
                                    e["boot"] != pending["boot"]):
            if e["boot"] == pending["boot"] and e["uptime_ms"] >= pending["uptime_ms"]:
                out[id(pending)] = e["uptime_ms"] - pending["uptime_ms"]
            pending = None
        if e["type"] == kind:
            pending = e
    return out


def _battery(events):
    """(hours, percent used) awake and asleep, between battery readings when not charging."""
    awake = [0.0, 0]
    asleep = [0.0, 0]
    last = None
    for e in events:
        if e["type"] not in ("battery", "sleep") or not e["time_of_day"]:
            continue
        percent = e.get("percent", e.get("battery"))
        charging = e.get("charging", 0) if e["type"] == "sleep" else (e["flags"] & 2)
        if percent is None or percent == 0xFF or charging:
            last = None
            continue
        if last is not None and e["wall"] > last["wall"]:
            hours = (e["wall"] - last["wall"]) / 3600
            used = max(0, last["percent"] - percent)
            bucket = awake if e["boot"] == last["boot"] else asleep
            bucket[0] += hours
            bucket[1] += used
        last = {"wall": e["wall"], "percent": percent, "boot": e["boot"]}
    return tuple(awake), tuple(asleep)


# ---- Output --------------------------------------------------------------------

def write_outputs(events, stats, names, out_dir):
    os.makedirs(os.path.join(out_dir, "csv"), exist_ok=True)
    with open(os.path.join(out_dir, "events.jsonl"), "w", encoding="utf-8") as fh:
        for e in events:
            fh.write(json.dumps(e, ensure_ascii=False) + "\n")
    for name, group in _group(events, "type").items():
        columns = []
        for e in group:
            for k in e:
                if k not in columns:
                    columns.append(k)
        if any("uid" in e for e in group):
            columns.append("item")
        with open(os.path.join(out_dir, "csv", f"{name}.csv"), "w", newline="", encoding="utf-8") as fh:
            writer = csv.writer(fh)
            writer.writerow(columns)
            for e in group:
                row = dict(e)
                if "uid" in row:
                    row["item"] = names.item(row["uid"])
                writer.writerow(["; ".join(row[c]) if isinstance(row.get(c), list) else row.get(c, "")
                                 for c in columns])
    with open(os.path.join(out_dir, "report.md"), "w", encoding="utf-8") as fh:
        fh.write(report(events, stats, names))


def main(argv=None):
    parser = argparse.ArgumentParser(description="Decode Tinta's usage log and write a report.")
    parser.add_argument("sd", help="the card's root, or its tinta/ folder")
    parser.add_argument("--out", default="usage-report", help="output folder (default: ./usage-report)")
    parser.add_argument("--ids", default=os.path.join(ROOT, "content", "ids.lock"),
                        help="uid -> key table (default: this checkout's content/ids.lock)")
    parser.add_argument("--dump", default=os.path.join(ROOT, "build", "course.dump.txt"),
                        help="a pack listing from packc --dump, for item labels")
    parser.add_argument("--screens", default=os.path.join(ROOT, "src", "app", "View.h"),
                        help="the header with enum class ScreenId (default: src/app/View.h)")
    args = parser.parse_args(argv)
    paths = log_files(args.sd)
    if not paths:
        print(f"usage-report: no usage-NNNN.log under {args.sd}", file=sys.stderr)
        return 1
    names = Names(load_ids(args.ids), load_dump(args.dump))
    events, stats = decode_files(paths, load_screens(args.screens))
    write_outputs(events, stats, names, args.out)
    print(f"usage-report: {len(events)} records from {len(paths)} file(s), "
          f"{stats['skipped_bytes']} damaged byte(s) skipped; wrote {args.out}/report.md, events.jsonl, csv/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
