"""Print every record of a pack, as read by tools/packc/reader.py, for the C++ cross-check.

    python3 tools/packc/tests/pack_expect.py mini.pack > mini.expect.txt   (test/tinta/make-fixtures.sh)

test/tinta/pack_test.cpp reads the output and checks every src/core/pack/Pack
accessor against it, record by record. One line per record:

    TAG index name=value name=value ...

with the field names of docs/pack-format.md and decimal values (string fields
are STRS offsets). Other lines:

    HEAD 0 formatMajor=.. formatMinor=.. contentVersion=.. buildTime=.. size=.. crc32=.. flags=..
    LOCL 0 <locale>
    SECT <tag> offset=.. size=.. count=..
    STRS <offset> <hex of the string's bytes>      (one per string in the heap)
    LEXS / DIST <index> value=..
"""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from packc import emit  # noqa: E402
from packc.reader import FIELDS, Pack  # noqa: E402


def line(tag: str, index: int, fields: list[tuple[str, int]]) -> str:
    return " ".join([tag, str(index)] + [f"{name}={value}" for name, value in fields])


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: pack_expect.py PACK", file=sys.stderr)
        return 2
    with open(argv[1], "rb") as fh:
        pack = Pack(fh.read())
    out: list[str] = []
    w = out.append

    w(line("HEAD", 0, [("formatMajor", pack.major), ("formatMinor", pack.minor),
                       ("contentVersion", pack.content_version), ("buildTime", pack.build_time),
                       ("size", len(pack.data)), ("crc32", pack.crc), ("flags", pack.flags)]))
    w(f"LOCL 0 {pack.locale}")
    for tag in emit.ORDER:
        off, size, count = pack.sections[tag]
        w(f"SECT {tag} offset={off} size={size} count={count}")

    # Every string of the heap, by offset; the heap starts with the empty string.
    offset = 1
    while offset < len(pack.heap):
        end = pack.heap.index(b"\0", offset)
        assert pack.str(offset).encode("utf-8") == pack.heap[offset:end]
        w(f"STRS {offset} {pack.heap[offset:end].hex()}")
        offset = end + 1

    for tag in emit.ORDER:
        if tag in FIELDS:
            names = FIELDS[tag].split()
            for i, rec in enumerate(pack.records(tag)):
                w(line(tag, i, [(n, rec[n]) for n in names]))
        elif tag in ("LEXS", "DIST"):
            for i, value in enumerate(pack.array(tag)):
                w(line(tag, i, [("value", value)]))
        elif tag == "VERB":
            for i in range(pack.count(tag)):
                v = pack.verb(i)
                cells = [(f"forms[{t}][{p}]", v["forms"][t][p]) for t in range(8) for p in range(5)]
                w(line(tag, i, cells + [(n, v[n]) for n in ("infinitive", "gerund", "participle", "lemma",
                                                           "flags")]))
        elif tag == "SQST":
            for i in range(pack.count(tag)):
                q = pack.question(i)
                opts = [(f"options[{k}]", q["options"][k]) for k in range(4)]
                w(line(tag, i, [("text", q["text"])] + opts +
                       [(n, q[n]) for n in ("answer", "optionCount", "flags")]))
        elif tag == "CONF":
            for i in range(pack.count(tag)):
                c = pack.confusable(i)
                members = [(f"members[{k}]", c["members"][k]) for k in range(6)]
                w(line(tag, i, [("name", c["name"]), ("note", c["note"])] + members +
                       [("memberCount", c["memberCount"])]))
        elif tag != "STRS":
            raise SystemExit(f"pack_expect: no expectation writer for section {tag}")
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
