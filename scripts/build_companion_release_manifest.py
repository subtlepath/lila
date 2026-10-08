#!/usr/bin/env python3
"""Validate release assets and produce deterministic companion update metadata."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# Board tags match FirmwareBoardTag.cpp; chip IDs match esptool's C3/S3 targets.
BOARDS = {
    "x3-x4": ("x4", 5),
    "sticky": ("sticky", 9),
    "x4pro": ("x4pro", 9),
    "x4c": ("x4c", 9),
    "papermono": ("papermono", 9),
}
OTA_PARTITION_BYTES = 0x640000
BOARD_PREFIX = b"CROSSPOINT-BOARD-V1:"


def validate_firmware(path: Path, board: str, chip: int) -> dict:
    size = path.stat().st_size
    if not 24 <= size <= OTA_PARTITION_BYTES:
        raise ValueError(f"{path.name}: image does not fit OTA partition")
    data = path.read_bytes()
    if data[0] != 0xE9 or not 1 <= data[1] <= 16 or data[23] not in (0, 1):
        raise ValueError(f"{path.name}: invalid ESP image header")
    if struct.unpack_from("<H", data, 12)[0] != chip:
        raise ValueError(f"{path.name}: wrong chip")
    offset, checksum = 24, 0xEF
    tags = []
    for _ in range(data[1]):
        if offset + 8 > size:
            raise ValueError(f"{path.name}: truncated segment header")
        length = struct.unpack_from("<I", data, offset + 4)[0]
        offset += 8
        if length > size - offset:
            raise ValueError(f"{path.name}: truncated segment data")
        segment = data[offset:offset + length]
        for byte in segment:
            checksum ^= byte
        tags.extend(re.findall(rb"CROSSPOINT-BOARD-V1:([^;]{1,23});", segment))
        # Reject malformed tags rather than overlooking partial/misleading metadata.
        if segment.count(BOARD_PREFIX) != len(re.findall(rb"CROSSPOINT-BOARD-V1:([^;]{1,23});", segment)):
            raise ValueError(f"{path.name}: malformed board tag")
        offset += length
    if not tags or any(tag != board.encode("ascii") for tag in tags):
        raise ValueError(f"{path.name}: missing or wrong board tag")
    padded = (offset + 16) & ~15
    expected = padded + (32 if data[23] else 0)
    if size != expected:
        raise ValueError(f"{path.name}: incorrect image length")
    if data[padded - 1] != checksum:
        raise ValueError(f"{path.name}: invalid image checksum")
    if data[23] and hashlib.sha256(data[:padded]).digest() != data[padded:]:
        raise ValueError(f"{path.name}: invalid image SHA-256 trailer")
    return {
        "name": path.name,
        "kind": "firmware",
        "length": size,
        "sha256": hashlib.sha256(data).hexdigest(),
        "boardTags": [board],
        "chipId": chip,
        "otaPartitionBytes": OTA_PARTITION_BYTES,
    }


def validate_pack(path: Path) -> dict:
    # Reuse the authoritative compiler's reader and CRC check, avoiding a second
    # definition of the pack layout. This runs on the release host, not the MCU.
    sys.path.insert(0, str(ROOT / "lib/Tinta/tools"))
    from packc.reader import Pack

    data = path.read_bytes()
    pack = Pack(data)
    if not pack.crc_ok():
        raise ValueError(f"{path.name}: invalid course CRC")
    return {
        "name": path.name,
        "kind": "course",
        "length": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "packFormat": {"major": pack.major, "minor": pack.minor},
        "contentVersion": pack.content_version,
        "locale": pack.locale,
    }


def build_manifest(directory: Path, tag: str, revision: str, repository: str,
                   channel: str, protocol: int, state_min: int, state_max: int,
                   journal_formats: tuple[int, ...] = ()) -> dict:
    version = tag.removeprefix("v")
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:rc\d*)?", version):
        raise ValueError("release tag must contain a lila release version")
    if channel not in {"stable", "rc"} or (channel == "rc") != ("rc" in version):
        raise ValueError("release version and channel disagree")
    if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", revision):
        raise ValueError("revision must be a full Git commit hash")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("repository must be owner/name")
    if not 0 <= protocol <= 255 or not 0 <= state_min <= state_max <= 0xFFFFFFFF:
        raise ValueError("invalid protocol or state compatibility range")
    # Capabilities describe the released binary, never infer them from this script.
    if (any(type(value) is not int or value not in {1, 2, 3} for value in journal_formats)
            or len(set(journal_formats)) != len(journal_formats)
            or (journal_formats and protocol == 0)):
        raise ValueError("invalid journal format capabilities")
    expected = {f"lila-{version}-{device}.bin" for device in BOARDS}
    actual = {path.name for path in directory.glob("lila-*.bin")}
    if actual != expected:
        raise ValueError(f"firmware asset set mismatch: missing={sorted(expected-actual)}, extra={sorted(actual-expected)}")
    assets = []
    for device, (board, chip) in BOARDS.items():
        asset = validate_firmware(directory / f"lila-{version}-{device}.bin", board, chip)
        asset.update({
            "companionProtocol": {"minimum": protocol, "maximum": protocol} if protocol else None,
            "initialUpgradeRequired": protocol == 0,
            "stateSchema": {"minimum": state_min, "maximum": state_max},
            "minimumBatteryPercent": 30,
            "supportedJournalHeaderVersions": sorted(journal_formats),
            # The runtime checks formatMajor and record strides, not formatMinor.
            "supportedPackFormatMajors": [1] if board in {"x4", "x4pro", "x4c"} else [],
        })
        assets.append(asset)
    assets.append(validate_pack(directory / "course.pack"))
    for asset in assets:
        asset["url"] = f"https://github.com/{repository}/releases/download/{tag}/{asset['name']}"
    return {
        "schemaVersion": 1,
        "version": version,
        "tag": tag,
        "revision": revision,
        "channel": channel,
        "assets": sorted(assets, key=lambda asset: asset["name"]),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets-dir", type=Path, required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--channel", choices=("stable", "rc"), required=True)
    parser.add_argument("--companion-protocol", type=int, required=True,
                        help="0 for firmware without companion mode; otherwise implemented protocol version")
    parser.add_argument("--state-schema-min", type=int, required=True)
    parser.add_argument("--state-schema-max", type=int, required=True)
    parser.add_argument("--journal-header-versions", type=int, nargs="*", default=[],
                        help="explicit TJH versions readable by the released firmware; omitted means none")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        manifest = build_manifest(args.assets_dir, args.tag, args.revision, args.repository,
                                  args.channel, args.companion_protocol, args.state_schema_min,
                                  args.state_schema_max, tuple(args.journal_header_versions))
        encoded = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        temporary = args.output.with_name(args.output.name + ".tmp")
        temporary.write_text(encoded, encoding="utf-8")
        temporary.replace(args.output)
    except (OSError, ValueError, struct.error, UnicodeError) as error:
        print(f"companion manifest: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
