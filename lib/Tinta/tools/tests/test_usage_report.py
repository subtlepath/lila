"""Tests for tools/usage-report.py.

    python3 -m unittest discover -s tools/tests

The fixture data/usage-sample/tinta/usage-0001.log is written by the firmware's
own encoder (test/tinta/usage_log_test.cpp checks it is byte for byte what
src/core/usage/UsageLog writes), so decoding it here ties the two together.
The other logs are built by a small encoder below, written from the layout in
src/core/usage/UsageLog.h.
"""

from __future__ import annotations

import importlib.util
import os
import struct
import tempfile
import unittest
import zlib

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(TOOLS)))
FIXTURE = os.path.join(REPO, "test", "tinta", "fixtures", "usage-sample")

_spec = importlib.util.spec_from_file_location("usage_report", os.path.join(TOOLS, "usage-report.py"))
ur = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ur)


def text(s: str) -> bytes:
    raw = s.encode()
    return bytes([len(raw)]) + raw


def record(type_: int, uptime: int, fields: bytes) -> bytes:
    return struct.pack("<BBI", type_, len(fields), uptime) + fields


def chunk(records: bytes, wall: int = 87000000, uptime: int = 0, flags: int = 1) -> bytes:
    return b"TU" + struct.pack("<HHBBII", len(records), zlib.crc32(records) & 0xFFFF, flags, 1, wall, uptime) + records


def boot(reason: int = 0, uptime: int = 0) -> bytes:
    fields = (bytes([reason]) + text("X3") + text("x3-x4") + text("0.7.0") +
              struct.pack("<IBBIIH", 1, 1, 1, 0x1234, 0, 1000))
    return record(1, uptime, fields)


def undo(uid: int, uptime: int = 0) -> bytes:
    return record(37, uptime, struct.pack("<I", uid))


def decode(*files: bytes, screens=None):
    with tempfile.TemporaryDirectory() as tmp:
        os.mkdir(os.path.join(tmp, "tinta"))
        for i, data in enumerate(files, start=1):
            with open(os.path.join(tmp, "tinta", f"usage-{i:04d}.log"), "wb") as fh:
                fh.write(data)
        return ur.decode_files(ur.log_files(tmp), screens or {})


class FixtureTest(unittest.TestCase):
    """The firmware's own sample session (test/tinta/usage_log_test.cpp, sampleSession)."""

    @classmethod
    def setUpClass(cls):
        cls.events, cls.stats = ur.decode_files(ur.log_files(FIXTURE), {1: "Home", 16: "Session"})

    def test_counts(self):
        self.assertEqual(self.stats["chunks"], 3)
        self.assertEqual(self.stats["skipped_bytes"], 0)
        self.assertEqual(len(self.events), 42)
        self.assertEqual(self.events[0]["type"], "boot")
        self.assertEqual(self.events[-1]["type"], "sleep")

    def test_boot(self):
        b = self.events[0]
        self.assertEqual((b["reason"], b["device"], b["build"], b["version"]),
                         ("wake-key", "X4CLASSIC", "x4-classic", "0.7.0-draft"))
        self.assertEqual((b["pack_edition"], b["pack_crc"], b["day"]), (1, "acd59ceb", 1007))
        # Day 1007 counted from 2024-01-01; the chunk's clock carried back by uptime.
        self.assertTrue(b["wall_iso"].startswith("2026-10-04T"))
        self.assertTrue(b["time_of_day"])

    def test_learning(self):
        shown = [e for e in self.events if e["type"] == "item_shown"]
        self.assertEqual(shown[0]["options"], ["house", "table", "dog", "cat"])
        self.assertEqual(shown[0]["format"], "choose-meaning")
        self.assertEqual(shown[1]["kind"], "gender")
        self.assertEqual(shown[2]["options"], [])
        answers = [e for e in self.events if e["type"] == "answer"]
        self.assertEqual([a["correct"] for a in answers], ["wrong", "right", "near"])
        self.assertEqual(answers[0]["chosen"], 2)
        self.assertEqual(answers[2]["typed"], "mésa")
        self.assertEqual(answers[2]["response_ms"], 6000)

    def test_interface(self):
        inputs = [(e["input"], e["outcome"], e["screen"]) for e in self.events if e["type"] == "input"]
        self.assertIn(("up", "ignored", "Session"), inputs)
        self.assertIn(("confirm", "queued", "Session"), inputs)
        frames = [e for e in self.events if e["type"] == "frame"]
        self.assertEqual(frames[0]["latency_ms"], 0xFFFF)
        self.assertEqual(frames[1]["refresh"], "half")

    def test_report(self):
        names = ur.Names({1: "vocab:casa:recognise"}, {})
        md = ur.report(self.events, self.stats, names)
        self.assertIn("| vocab:casa:recognise | house | dog | 1 |", md)  # wrong option chosen
        self.assertIn("| despertarse | 2 |", md)  # glossed twice
        self.assertIn("| spoon | english | 1 |", md)  # a search with no results
        self.assertIn("| Session | up | ignored | 1 |", md)
        self.assertIn("| finished | 1 | 3 |", md)


class DecoderTest(unittest.TestCase):
    def test_torn_chunk_and_garbage_are_skipped(self):
        good1 = chunk(boot() + undo(1))
        torn = chunk(undo(2) + undo(3))[:-5]  # cut during the append
        good2 = chunk(boot(1) + undo(4))
        events, stats = decode(good1 + torn + b"\x00\xa5TU\x01" + good2)
        self.assertEqual([e.get("uid") for e in events if e["type"] == "undo"], [1, 4])
        self.assertEqual(stats["chunks"], 2)
        self.assertGreater(stats["skipped_bytes"], 0)
        self.assertEqual([e["boot"] for e in events if e["type"] == "boot"], [1, 2])

    def test_unknown_types_and_extra_fields_are_skipped(self):
        newer = record(99, 5, b"\x01\x02\x03") + record(37, 6, struct.pack("<I", 7) + b"\xff\xff")
        events, stats = decode(chunk(boot() + newer + undo(8)))
        self.assertEqual([e["type"] for e in events], ["boot", "type-99", "undo", "undo"])
        self.assertEqual([e.get("uid") for e in events if e["type"] == "undo"], [7, 8])
        self.assertEqual(stats["unknown_types"][99], 1)

    def test_older_records_with_fewer_fields(self):
        # A battery record from a firmware that wrote only the percent.
        events, _ = decode(chunk(record(5, 0, b"\x42")))
        self.assertEqual(events[0]["percent"], 0x42)
        self.assertNotIn("flags", events[0])

    def test_files_in_order_and_file_start_keeps_the_boot(self):
        first = chunk(boot(2) + undo(1))
        second = chunk(boot(4) + undo(2))  # FileStart: the same boot
        third = chunk(boot(0) + undo(3))
        events, stats = decode(first, second, third)
        self.assertEqual([(e["file"], e["boot"]) for e in events if e["type"] == "undo"],
                         [("usage-0001.log", 1), ("usage-0002.log", 1), ("usage-0003.log", 2)])
        self.assertEqual(len(stats["files"]), 3)

    def test_a_resumed_session_counts_once(self):
        def start(resumed):
            return record(32, 0, struct.pack("<BHHHHB", 0, 0, 20, 10, 10, resumed))

        def end(how, done):
            return record(33, 0, struct.pack("<BHHI", how, done, done, 60))

        slept, finished = 2, 0
        events, stats = decode(chunk(boot() + start(0) + end(slept, 5)) + chunk(boot(1) + start(1) + end(finished, 20)))
        md = ur.report(events, stats, ur.Names({}, {}))
        self.assertIn("| today | 1 | 1 |", md)  # started once, resumed once
        self.assertIn("| finished | 1 | 20 |", md)
        self.assertNotIn("| slept |", md)

    def test_record_wall_time(self):
        # A record made 4.5 s before the flush is 4 s earlier on the wall.
        events, _ = decode(chunk(undo(1, uptime=500), wall=1000, uptime=5000))
        self.assertEqual(events[0]["wall"], 996)

    def test_screen_names_from_the_header(self):
        with tempfile.NamedTemporaryFile("w", suffix=".h", delete=False) as fh:
            fh.write("enum class ScreenId : uint8_t {\n  None,\n  Home,  // first\n  Settings,\n  Count,\n};\n")
        try:
            self.assertEqual(ur.load_screens(fh.name), {0: "None", 1: "Home", 2: "Settings", 3: "Count"})
        finally:
            os.unlink(fh.name)


class CommandLineTest(unittest.TestCase):
    def test_writes_report_jsonl_and_csv(self):
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(ur.main([FIXTURE, "--out", out, "--ids", "", "--dump", "", "--screens", ""]), 0)
            with open(os.path.join(out, "report.md"), encoding="utf-8") as fh:
                self.assertIn("# Tinta usage report", fh.read())
            with open(os.path.join(out, "events.jsonl"), encoding="utf-8") as fh:
                self.assertEqual(sum(1 for _ in fh), 42)
            self.assertTrue(os.path.exists(os.path.join(out, "csv", "answer.csv")))

    def test_no_logs(self):
        with tempfile.TemporaryDirectory() as empty:
            self.assertEqual(ur.main([empty, "--out", os.path.join(empty, "r")]), 1)


if __name__ == "__main__":
    unittest.main()
