"""Python pack reader, for --dump and round-trip tests. Mirrors src/core/pack/Pack."""

from __future__ import annotations

import struct
import zlib

from . import emit

NONE16 = 0xFFFF

FIELDS = {
    "LEMM": ("es en pron note alt feminine plural firstExample exampleCount freqRank lesson verbTable "
             "pos gender level reg flags reserved"),
    "LKEY": "key lemma reserved",
    "EKEY": "key lemma rank reserved",
    "FORM": "key form lemma tag",
    "SENT": "es en note firstToken lesson tokenCount level source flags reg reserved",
    "TOKS": "start length flags lemma tag",
    "ITEM": "uid kind candidateCount lesson a b prereq flags reserved firstCandidate",
    "IUID": "uid index reserved",
    "UNIT": "title titleEn goals number firstLesson lessonCount reserved",
    "LESS": ("title titleEn firstItem itemCount unit number firstNote noteCount dialogue firstSentence "
             "sentenceCount newCount level flags"),
    "NOTE": "title firstSpan spanCount lesson kind flags reserved",
    "NSPN": "text style flags reserved",
    "STOR": "title titleEn firstLine lineCount lesson firstQuestion questionCount level kind flags reserved",
    "SLIN": "speaker sentence flags",
    "PHRS": "title titleEn firstEntry entryCount",
    "PENT": "pron sentence category",
}


class PackError(ValueError):
    pass


class Pack:
    def __init__(self, data: bytes):
        self.data = data
        if len(data) < emit.HEADER_SIZE:
            raise PackError("too small")
        (magic, self.major, self.minor, self.content_version, self.build_time, size, self.crc, locale,
         dir_off, count, header_size, self.flags, _r) = emit.HEADER.unpack_from(data, 0)
        if magic != b"TNTA":
            raise PackError("bad magic")
        if self.major != emit.FORMAT_MAJOR:
            raise PackError(f"format {self.major}.{self.minor} not supported")
        if size != len(data):
            raise PackError(f"size field {size} != {len(data)}")
        self.locale = locale.rstrip(b"\0").decode()
        self.sections: dict[str, tuple[int, int, int]] = {}
        for i in range(count):
            tag, off, sz, n = emit.DIRENT.unpack_from(data, dir_off + i * emit.DIRENT.size)
            if off % 4 or off + sz > len(data):
                raise PackError(f"section {tag!r} out of bounds")
            if tag.decode() in self.sections:
                raise PackError(f"duplicate section {tag!r}")
            self.sections[tag.decode()] = (off, sz, n)
        for tag in emit.ORDER:
            if tag not in self.sections:
                raise PackError(f"missing section {tag}")
        off, sz, _n = self.sections["STRS"]
        self.heap = data[off:off + sz]
        if "IDEN" in self.sections:
            off, size, count = self.sections["IDEN"]
            if count >= 0xFFFFFFFF or size != count * 36:
                raise PackError("invalid identity history size")
            for index in range(count):
                uid, digest = emit.ITEM_IDENTITY.unpack_from(data, off + index * 36)
                if uid != index + 1 or not any(digest):
                    raise PackError("invalid identity history record")
            for item in self.records("ITEM"):
                if not 0 < item["uid"] <= count:
                    raise PackError("identity history misses active item")

    def crc_ok(self) -> bool:
        copy = bytearray(self.data)
        copy[20:24] = b"\0\0\0\0"
        return zlib.crc32(bytes(copy)) == self.crc

    def str(self, offset: int) -> str:
        end = self.heap.index(b"\0", offset)
        return self.heap[offset:end].decode("utf-8")

    def count(self, tag: str) -> int:
        return self.sections[tag][2]

    def record(self, tag: str, index: int) -> dict:
        off, sz, n = self.sections[tag]
        if not 0 <= index < n:
            raise IndexError(f"{tag}[{index}]")
        layout = emit.SIZES[tag]
        stride = sz // n
        values = layout.unpack_from(self.data, off + index * stride)
        return dict(zip(FIELDS[tag].split(), values))

    def records(self, tag: str) -> list[dict]:
        return [self.record(tag, i) for i in range(self.count(tag))]

    def array(self, tag: str) -> list[int]:
        off, sz, n = self.sections[tag]
        fmt = {"LEXS": "H", "DIST": "I"}[tag]
        return list(struct.unpack_from(f"<{n}{fmt}", self.data, off))

    def verb(self, index: int) -> dict:
        off, sz, n = self.sections["VERB"]
        values = emit.VERB.unpack_from(self.data, off + index * (sz // n))
        return {"forms": [list(values[t * 5:(t + 1) * 5]) for t in range(8)], "infinitive": values[40],
                "gerund": values[41], "participle": values[42], "lemma": values[43], "flags": values[44]}

    def question(self, index: int) -> dict:
        off, sz, n = self.sections["SQST"]
        v = emit.QUESTION.unpack_from(self.data, off + index * (sz // n))
        return {"text": v[0], "options": list(v[1:5]), "answer": v[5], "optionCount": v[6], "flags": v[7]}

    def confusable(self, index: int) -> dict:
        off, sz, n = self.sections["CONF"]
        v = emit.CONFUSABLE.unpack_from(self.data, off + index * (sz // n))
        return {"name": v[0], "note": v[1], "members": list(v[2:8]), "memberCount": v[8]}
