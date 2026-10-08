"""Release manifest validation, using corrupt and wrong-board ESP images."""

import hashlib
import importlib.util
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("release_manifest", ROOT / "scripts/build_companion_release_manifest.py")
manifest = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(manifest)


def image(board="x4", chip=5, append_hash=True):
    header = bytearray(24)
    header[0] = 0xE9
    header[1] = 1
    struct.pack_into("<H", header, 12, chip)
    header[23] = int(append_hash)
    payload = b"CROSSPOINT-BOARD-V1:" + board.encode() + b";\x00payload"
    data = header + struct.pack("<II", 0x3C000020, len(payload)) + payload
    checksum = 0xEF
    for byte in payload:
        checksum ^= byte
    data += bytes(((len(data) + 16) & ~15) - len(data) - 1) + bytes([checksum])
    if append_hash:
        data += hashlib.sha256(data).digest()
    return data


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        for device, (board, chip) in manifest.BOARDS.items():
            (self.directory / f"lila-0.1.0-{device}.bin").write_bytes(image(board, chip))
        shutil.copyfile(ROOT / "test/tinta/fixtures/mini.pack", self.directory / "course.pack")

    def build(self, **overrides):
        arguments = dict(directory=self.directory, tag="v0.1.0", revision="a" * 40,
                         repository="subtlepath/lila", channel="stable", protocol=0,
                         state_min=0, state_max=0)
        arguments.update(overrides)
        return manifest.build_manifest(**arguments)

    def test_complete_manifest_hashes_and_compatibility(self):
        result = self.build()
        self.assertEqual(result, self.build())
        self.assertEqual(len(result["assets"]), 6)
        for asset in result["assets"]:
            data = (self.directory / asset["name"]).read_bytes()
            self.assertEqual(asset["length"], len(data))
            self.assertEqual(asset["sha256"], hashlib.sha256(data).hexdigest())
            self.assertTrue(asset["url"].startswith("https://github.com/subtlepath/lila/releases/download/v0.1.0/"))
            if asset["kind"] == "firmware":
                self.assertIsNone(asset["companionProtocol"])
                self.assertTrue(asset["initialUpgradeRequired"])
                self.assertEqual(asset["minimumBatteryPercent"], 30)
                self.assertEqual(asset["supportedJournalHeaderVersions"], [])
                board = asset["boardTags"][0]
                self.assertEqual(asset["supportedPackFormatMajors"], [1] if board in {"x4", "x4pro", "x4c"} else [])

    def test_explicit_protocol_and_migrated_state_compatibility(self):
        result = self.build(protocol=1, state_min=2, state_max=3)
        for asset in result["assets"]:
            if asset["kind"] == "firmware":
                self.assertEqual(asset["companionProtocol"], {"minimum": 1, "maximum": 1})
                self.assertFalse(asset["initialUpgradeRequired"])
                self.assertEqual(asset["stateSchema"], {"minimum": 2, "maximum": 3})

    def test_journal_capabilities_are_explicit_sorted_and_strict(self):
        result = self.build(protocol=1, journal_formats=(3, 1, 2))
        for asset in result["assets"]:
            if asset["kind"] == "firmware":
                self.assertEqual(asset["supportedJournalHeaderVersions"], [1, 2, 3])
        for formats in [(0,), (4,), (1, 1), (True,), ("3",)]:
            with self.subTest(formats=formats):
                with self.assertRaisesRegex(ValueError, "journal format"):
                    self.build(protocol=1, journal_formats=formats)
        with self.assertRaisesRegex(ValueError, "journal format"):
            self.build(protocol=0, journal_formats=(3,))

    def test_release_candidate_uses_its_exact_tag_and_assets(self):
        for path in self.directory.glob("lila-*.bin"):
            path.rename(path.with_name(path.name.replace("0.1.0", "0.1.0rc")))
        result = self.build(tag="v0.1.0rc", channel="rc")
        self.assertEqual(result["channel"], "rc")
        for asset in result["assets"]:
            self.assertIn("/v0.1.0rc/", asset["url"])

    def test_rejects_missing_or_extra_firmware(self):
        path = self.directory / "lila-0.1.0-x3-x4.bin"
        path.unlink()
        with self.assertRaisesRegex(ValueError, "asset set mismatch"):
            self.build()
        path.write_bytes(image())
        (self.directory / "lila-0.0.1-x3-x4.bin").write_bytes(image())
        with self.assertRaisesRegex(ValueError, "asset set mismatch"):
            self.build()

    def test_rejects_wrong_board_and_wrong_chip(self):
        path = self.directory / "lila-0.1.0-x3-x4.bin"
        path.write_bytes(image("sticky"))
        with self.assertRaisesRegex(ValueError, "board tag"):
            self.build()
        path.write_bytes(image("x4", 9))
        with self.assertRaisesRegex(ValueError, "wrong chip"):
            self.build()

    def test_rejects_corruption_truncation_and_extra_bytes(self):
        path = self.directory / "lila-0.1.0-x3-x4.bin"
        original = image()
        for data in [original[:23], original[:-1], original + b"x"]:
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                self.build()
        data = bytearray(original)
        data[-33] ^= 1
        path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "checksum"):
            self.build()
        data = bytearray(original)
        data[-1] ^= 1
        path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            self.build()

    def test_rejects_segment_overflow_zero_segments_and_malformed_hash_flag(self):
        path = self.directory / "lila-0.1.0-x3-x4.bin"
        for offset, value in [(1, 0), (23, 2)]:
            data = image()
            data[offset] = value
            path.write_bytes(data)
            with self.assertRaisesRegex(ValueError, "header"):
                self.build()
        data = image()
        struct.pack_into("<I", data, 28, 0xFFFFFFFF)
        path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "segment data"):
            self.build()

    def test_accepts_checksum_image_without_sha_trailer(self):
        path = self.directory / "lila-0.1.0-x3-x4.bin"
        path.write_bytes(image(append_hash=False))
        self.build()

    def test_rejects_missing_board_tag_even_with_valid_image_integrity(self):
        path = self.directory / "lila-0.1.0-x3-x4.bin"
        data = image()
        data[32] = ord("Z")
        checksum = 0xEF
        length = struct.unpack_from("<I", data, 28)[0]
        for byte in data[32:32 + length]:
            checksum ^= byte
        data[-33] = checksum
        data[-32:] = hashlib.sha256(data[:-32]).digest()
        path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "board tag"):
            self.build()

    def test_rejects_oversize_image(self):
        path = self.directory / "lila-0.1.0-x3-x4.bin"
        with path.open("wb") as file:
            file.truncate(manifest.OTA_PARTITION_BYTES + 1)
        with self.assertRaisesRegex(ValueError, "partition"):
            self.build()

    def test_rejects_corrupt_course(self):
        path = self.directory / "course.pack"
        data = bytearray(path.read_bytes())
        data[-1] ^= 1
        path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "CRC"):
            self.build()

    def test_rejects_invalid_release_metadata(self):
        for changes in [dict(channel="preview"), dict(channel="rc"), dict(tag="../../v0.1.0"), dict(revision="abcd"),
                        dict(repository="foo/../bar"), dict(protocol=256), dict(state_min=2, state_max=1)]:
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.build(**changes)

    def test_partition_bound_matches_firmware_configuration(self):
        for line in (ROOT / "partitions.csv").read_text().splitlines():
            if line.startswith(("app0,", "app1,")):
                self.assertEqual(int(line.split(",")[4].strip(), 0), manifest.OTA_PARTITION_BYTES)

    def test_cli_keeps_existing_manifest_on_validation_failure(self):
        output = self.directory / "companion-release.json"
        command = [sys.executable, str(ROOT / "scripts/build_companion_release_manifest.py"),
                   "--assets-dir", str(self.directory), "--tag", "v0.1.0", "--revision", "a" * 40,
                   "--repository", "subtlepath/lila", "--channel", "stable", "--companion-protocol", "0",
                   "--state-schema-min", "0", "--state-schema-max", "0", "--output", str(output)]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        previous = output.read_bytes()
        self.assertEqual(json.loads(previous)["schemaVersion"], 1)
        (self.directory / "lila-0.1.0-x3-x4.bin").unlink()
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("asset set mismatch", result.stderr)
        self.assertEqual(output.read_bytes(), previous)


if __name__ == "__main__":
    unittest.main()
