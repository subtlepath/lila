#!/usr/bin/env python3
"""Prints a learner's progress files from an SD card directory (PLAN.md 8.3).

    python3 tools/progress-dump.py build/sim/run/X3/sd            # every file, as text
    python3 tools/progress-dump.py SD --json                      # the same as JSON
    python3 tools/progress-dump.py SD --get journal.count         # one value (flows)

SD is the card's root (the directory holding tinta/) or tinta/ itself. The
layouts are the firmware's: items.bin and reviews.log in
src/core/srs/ProgressStore.h, ItemState in src/core/srs/ItemState.h, days.bin
in src/core/stats/DayLog.h, profile.bin in src/core/profile/Profile.h,
session.bin in src/app/App.cpp, src/app/SessionController.cpp and
src/core/srs/DayQueue.cpp. Every CRC is checked and reported.

--get takes a dotted path into the JSON, e.g. items.header.journalCount,
journal.count, journal.last.grade, days.totals.reviews, session.queue.count,
session.queue.uids.0, profile.newPerDay. A missing file or key prints "none".
"""

import argparse
import json
import os
import struct
import sys
import zlib

DAY0 = 19723  # 2024-01-01 as days since 1970-01-01


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def iso(day):
    import datetime
    return (datetime.date(1970, 1, 1) + datetime.timedelta(days=DAY0 + day)).isoformat()


def read(root, name):
    path = os.path.join(root, name)
    try:
        with open(path, "rb") as fh:
            return fh.read()
    except OSError:
        return None


PHASES = ["new", "learning", "review", "relearning"]


def item_state(b):
    uid, due, last, stab, diff, phase, reps, lapses, flags = struct.unpack_from("<IHHHBBBBH", b)
    return {
        "uid": uid,
        "due": due,
        "last": last,
        "stability": round(stab / 16.0, 4),
        "difficulty": round(diff / 25.0, 2),
        "phase": PHASES[phase & 3],
        "step": (phase >> 2) & 7,
        "reps": reps,
        "lapses": lapses,
        "flags": flags,
    }


def header(b):
    if len(b) < 80 or b[:4] != b"TIS1":
        return None
    out = {"ok": struct.unpack_from("<I", b, 76)[0] == crc32(b[:76])}
    (out["version"], _, out["seq"], out["recordCount"], out["journalCount"], out["statDay"], out["statNew"],
     out["statReviews"], flags, out["pendingSlot"]) = struct.unpack_from("<HHIIIHHHHI", b, 4)
    out["pending"] = item_state(b[32:48]) if flags & 1 else None
    out["undo"] = bool(flags & 2)
    return out


def dump_items(root):
    data = read(root, "items.bin")
    if data is None:
        return None
    heads = [h for h in (header(data[0:80]), header(data[512:592])) if h]
    valid = [h for h in heads if h["ok"]]
    best = max(valid, key=lambda h: h["seq"]) if valid else None
    records = []
    count = best["recordCount"] if best else max(0, (len(data) - 1024) // 16)
    for n in range(count):
        at = 1024 + 16 * n
        if at + 16 > len(data):
            break
        records.append(item_state(data[at:at + 16]))
    return {"bytes": len(data), "header": best, "headers": heads, "count": len(records), "records": records}


def dump_journal(root):
    data = read(root, "reviews.log")
    if data is None:
        return None
    entries = []
    for at in range(0, len(data) - len(data) % 12, 12):
        uid, time, day, op, arg = struct.unpack_from("<IIHBB", data, at)
        grade = op & 7
        if 1 <= grade <= 4:
            entries.append({"uid": uid, "time": time, "day": day, "grade": grade, "format": op >> 3,
                            "ms": arg * 250})
        else:
            code = op >> 3
            entries.append({"uid": uid, "time": time, "day": day, "control": {1: "undo", 2: "flags"}.get(code, code),
                            "arg": arg})
    reviews = [e for e in entries if "grade" in e]
    return {"bytes": len(data), "count": len(entries), "reviews": len(reviews), "tail": len(data) % 12,
            "last": entries[-1] if entries else None, "entries": entries}


def dump_days(root):
    data = read(root, "days.bin")
    if data is None:
        return None
    out = {"bytes": len(data), "magic": data[:4] == b"TDL1", "records": [], "bad": 0}
    days = {}
    for at in range(4, len(data) - (len(data) - 4) % 12, 12):
        day, reviews, correct, new, seconds, check = struct.unpack_from("<HHHHHH", data, at)
        if check != crc32(data[at:at + 10]) & 0xFFFF:
            out["bad"] += 1
            continue
        out["records"].append({"day": day, "reviews": reviews, "correct": correct, "new": new, "seconds": seconds})
        d = days.setdefault(day, {"reviews": 0, "correct": 0, "new": 0, "seconds": 0})
        for k, v in (("reviews", reviews), ("correct", correct), ("new", new), ("seconds", seconds)):
            d[k] += v
    out["days"] = {str(k): v for k, v in sorted(days.items())}
    out["totals"] = {k: sum(d[k] for d in days.values()) for k in ("reviews", "correct", "new", "seconds")}
    return out


PROFILE_FIELDS = [
    ("newPerDay", "H", 0), ("reviewCap", "H", 2), ("retentionPermille", "H", 4), ("maxInterval", "H", 6),
    ("sessionSize", "H", 8), ("textSize", "B", 10), ("uiLanguage", "B", 11), ("showVulgar", "B", 12),
    ("fullRefreshEvery", "B", 13), ("sleepTimeoutSeconds", "H", 14), ("sleepScreen", "B", 16),
    ("rolloverHour", "B", 17), ("utcOffsetMinutes", "h", 18), ("lastConfirmedDay", "H", 20),
    ("currentLesson", "H", 22), ("unlockedThrough", "H", 24), ("frontlightBrightness", "B", 26),
    ("frontlightWarmth", "B", 27), ("typedAnswers", "B", 28),
    ("sleepCount", "H", 29),
]


def dump_profile(root):
    data = read(root, "profile.bin")
    if data is None:
        return None
    if len(data) < 12 or data[:4] != b"TPRF":
        return {"ok": False}
    version, payload = struct.unpack_from("<HH", data, 4)
    out = {"ok": len(data) >= 12 + payload and struct.unpack_from("<I", data, 8 + payload)[0] == crc32(data[:8 + payload]),
           "version": version}
    for name, fmt, offset in PROFILE_FIELDS:
        if offset + struct.calcsize(fmt) <= payload:
            out[name] = struct.unpack_from("<" + fmt, data, 8 + offset)[0]
    return out


def dump_queue(blob):
    if len(blob) < 28 or blob[:4] != b"TSQ1":
        return {"ok": False}
    version, count, day, done, reviews, correct, new, kind, _, ms, tag = struct.unpack_from("<HHHHHHHBBIH", blob, 4)
    head = 28 if version >= 2 else 24
    size = head + 5 * count + 4
    ok = size <= len(blob) and struct.unpack_from("<I", blob, size - 4)[0] == crc32(blob[:size - 4])
    uids, flags = [], []
    for n in range(count):
        at = head + 5 * n
        if at + 5 > len(blob):
            break
        uids.append(struct.unpack_from("<I", blob, at)[0])
        flags.append(blob[at + 4])
    return {"ok": ok, "version": version, "count": count, "day": day, "done": done, "reviews": reviews,
            "correct": correct, "new": new, "ms": ms, "kind": kind, "tag": tag, "uids": uids, "flags": flags}


def dump_session(root):
    data = read(root, "session.bin")
    if data is None:
        return None
    if len(data) < 11 or data[:4] != b"TSES":
        return {"ok": False}
    version, depth = struct.unpack_from("<HB", data, 4)
    screens = list(data[7:7 + depth])
    covered = 7 + depth
    out = {"version": version, "screens": screens, "session": None}
    if version >= 2:
        length = struct.unpack_from("<H", data, covered)[0]
        part = data[covered + 2:covered + 2 + length]
        covered += 2 + length
        if length >= 20:
            journal, flags = struct.unpack_from("<IB", part, 0)
            reviews, correct, new, ms, qlen = struct.unpack_from("<HHHIH", part, 6)
            out["session"] = {"journalCount": journal, "back": bool(flags & 1),
                              "flushed": {"reviews": reviews, "correct": correct, "new": new, "ms": ms}}
            out["queue"] = dump_queue(part[20:20 + qlen])
    out["ok"] = covered + 4 <= len(data) and struct.unpack_from("<I", data, covered)[0] == crc32(data[:covered])
    out["bytes"] = len(data)
    return out


def dump(root):
    if os.path.isdir(os.path.join(root, "tinta")):
        root = os.path.join(root, "tinta")
    return {
        "items": dump_items(root),
        "journal": dump_journal(root),
        "days": dump_days(root),
        "profile": dump_profile(root),
        "session": dump_session(root),
    }


def lookup(tree, path):
    node = tree
    for part in path.split("."):
        if isinstance(node, dict) and part in node:
            node = node[part]
        elif isinstance(node, list) and part.lstrip("-").isdigit() and -len(node) <= int(part) < len(node):
            node = node[int(part)]
        else:
            return None
    return node


def text(tree):
    lines = []
    items = tree["items"]
    if items is None:
        lines.append("items.bin    none")
    else:
        h = items["header"]
        lines.append(f"items.bin    {items['bytes']} bytes, {items['count']} records" +
                     (f", seq {h['seq']}, journal {h['journalCount']}, day {h['statDay']} new {h['statNew']} "
                      f"reviews {h['statReviews']}{', pending' if h['pending'] else ''}{', undo' if h['undo'] else ''}"
                      if h else ", NO VALID HEADER"))
        for r in items["records"]:
            lines.append(f"  uid {r['uid']:<6} {r['phase']:<10} step {r['step']} due {r['due']} ({iso(r['due'])}) "
                         f"last {r['last']} S {r['stability']:<8} D {r['difficulty']:<5} reps {r['reps']} "
                         f"lapses {r['lapses']} flags {r['flags']}")
    j = tree["journal"]
    if j is None:
        lines.append("reviews.log  none")
    else:
        lines.append(f"reviews.log  {j['count']} records ({j['reviews']} grades){', TORN TAIL' if j['tail'] else ''}")
        for e in j["entries"]:
            if "grade" in e:
                lines.append(f"  day {e['day']} uid {e['uid']:<6} grade {e['grade']} format {e['format']} {e['ms']} ms")
            else:
                lines.append(f"  day {e['day']} uid {e['uid']:<6} {e['control']} {e['arg']}")
    d = tree["days"]
    if d is None:
        lines.append("days.bin     none")
    else:
        lines.append(f"days.bin     {len(d['records'])} records{', BAD MAGIC' if not d['magic'] else ''}"
                     f"{', %d bad' % d['bad'] if d['bad'] else ''}; totals {d['totals']}")
        for day, v in d["days"].items():
            lines.append(f"  day {day} ({iso(int(day))}) {v}")
    p = tree["profile"]
    lines.append("profile.bin  none" if p is None else
                 "profile.bin  " + ("" if p["ok"] else "CORRUPT ") + ", ".join(f"{k} {v}" for k, v in p.items()
                                                                            if k not in ("ok",)))
    s = tree["session"]
    if s is None:
        lines.append("session.bin  none")
    else:
        lines.append(f"session.bin  version {s.get('version')}, screens {s.get('screens')}"
                     f"{'' if s.get('ok') else ', BAD CRC'}")
        if s.get("session"):
            q = s.get("queue", {})
            lines.append(f"  session: {s['session']}")
            lines.append(f"  queue: day {q.get('day')} done {q.get('done')} count {q.get('count')} "
                         f"uids {q.get('uids')}{'' if q.get('ok') else ' BAD CRC'}")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("sd", help="the SD card directory (or its tinta/)")
    parser.add_argument("--json", action="store_true", help="print JSON")
    parser.add_argument("--get", metavar="PATH", help="print one value, e.g. journal.count")
    args = parser.parse_args()
    tree = dump(args.sd)
    if args.get:
        value = lookup(tree, args.get)
        print("none" if value is None else json.dumps(value) if isinstance(value, (dict, list)) else value)
    elif args.json:
        print(json.dumps(tree, indent=1))
    else:
        print(text(tree))
    return 0


if __name__ == "__main__":
    sys.exit(main())
