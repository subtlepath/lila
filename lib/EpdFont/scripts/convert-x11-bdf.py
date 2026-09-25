#!/usr/bin/env python3
"""Bundle X11 BDF strikes into multi-style CrossPoint .cpfont families."""

from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

import freetype

from fontconvert_sdcard import generate_cpfont_multistyle


STYLE_REGULAR = 0
STYLE_BOLD = 1
STYLE_ITALIC = 2
STYLE_BOLDITALIC = 3
DASH_FALLBACK_CODEPOINTS = (0x2010, 0x2011, 0x2012, 0x2013, 0x2014, 0x2015, 0x2212)


def read_bdf_metadata(path: Path) -> dict[str, str]:
    properties: dict[str, str] = {}
    in_properties = False

    with path.open(encoding="ascii", errors="replace") as source:
        for raw_line in source:
            line = raw_line.rstrip("\r\n")
            if line.startswith("FONT "):
                properties["FONT"] = line[5:]
                continue
            if line.startswith("STARTPROPERTIES "):
                in_properties = True
                continue
            if line == "ENDPROPERTIES":
                in_properties = False
                continue
            if in_properties and " " in line:
                key, value = line.split(" ", 1)
                properties[key] = value.strip().strip('"')
    required = ("FONT", "FAMILY_NAME", "WEIGHT_NAME", "SLANT", "POINT_SIZE")
    missing = [key for key in required if key not in properties]
    if missing:
        raise ValueError(f"{path}: missing BDF properties: {', '.join(missing)}")
    return properties


def encoded_codepoints(path: Path) -> list[int]:
    face = freetype.Face(str(path))
    codepoints = [codepoint for codepoint, glyph_index in face.get_chars() if glyph_index != 0]
    if face.get_char_index(0x0060) and face.get_char_index(0x0027):
        codepoints.extend((0x2018, 0x2019, 0x201C, 0x201D))
    return codepoints


def style_id(properties: dict[str, str]) -> int:
    weight = properties["WEIGHT_NAME"].lower()
    bold = "bold" in weight
    italic = properties["SLANT"].upper() in ("I", "O")
    if bold and italic:
        return STYLE_BOLDITALIC
    if bold:
        return STYLE_BOLD
    if italic:
        return STYLE_ITALIC
    return STYLE_REGULAR


def family_name(path: Path, properties: dict[str, str]) -> str:
    family = properties["FAMILY_NAME"]
    add_style = properties.get("ADD_STYLE_NAME", "")
    if family == "Lucida" and add_style == "Sans":
        family = "Lucida Sans"
    if family == "Terminal":
        family = "Bitstream Technical" if path.stem.startswith("tech") else "Bitstream Terminal"
    return re.sub(r"[^A-Za-z0-9_-]+", "-", family).strip("-")


def xlfd_resolution(properties: dict[str, str]) -> int:
    fields = properties["FONT"].split("-")
    if len(fields) < 11 or not fields[10].isdigit():
        raise ValueError(f"invalid XLFD FONT property: {properties['FONT']}")
    return int(fields[10])


def intervals_for(codepoints: set[int]) -> list[tuple[int, int]]:
    if not codepoints:
        return []
    result: list[tuple[int, int]] = []
    start = previous = min(codepoints)
    for codepoint in sorted(codepoints)[1:]:
        if codepoint != previous + 1:
            result.append((start, previous))
            start = codepoint
        previous = codepoint
    result.append((start, previous))
    return result


def reported_sizes(groups, group_resolutions, target_dpi: int) -> dict[tuple[str, int], int]:
    """Map native strikes to unique integer physical point sizes."""
    by_family: dict[str, list[int]] = defaultdict(list)
    for family, source_point_size in groups:
        by_family[family].append(source_point_size)

    result: dict[tuple[str, int], int] = {}
    for family, source_sizes in by_family.items():
        previous = 0
        for source_point_size in sorted(source_sizes):
            resolution = group_resolutions[(family, source_point_size)]
            # Round source_pt * source_dpi / target_dpi to nearest, then
            # preserve every strike when two adjacent values collide.
            numerator = source_point_size * resolution
            nearest = (numerator * 2 + target_dpi) // (target_dpi * 2)
            reported = max(1, nearest, previous + 1)
            result[(family, source_point_size)] = reported
            previous = reported
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Directory containing X11 BDF packages")
    parser.add_argument("--output-dir", type=Path, default=Path("x11/cpfont"))
    parser.add_argument(
        "--target-dpi", type=int, default=150,
        help="Rasterization DPI used to normalize reported point sizes (default: 150)")
    args = parser.parse_args()
    if args.target_dpi <= 0:
        parser.error("--target-dpi must be greater than zero")

    bdf_paths = sorted(args.source.rglob("*.bdf"))
    if not bdf_paths:
        parser.error(f"no .bdf files found under {args.source}")

    records = []
    source_fonts: dict[tuple[str, int, int, int], tuple[Path, list[int]]] = {}
    family_resolutions: dict[str, set[int]] = defaultdict(set)
    for path in bdf_paths:
        properties = read_bdf_metadata(path)
        codepoints = encoded_codepoints(path)
        family = family_name(path, properties)
        # Some legacy files carry stale RESOLUTION_Y properties. The XLFD name
        # is the package's canonical 75/100 DPI identity and matches its path.
        resolution = xlfd_resolution(properties)
        source_point_size = int(properties["POINT_SIZE"]) // 10
        source_fonts[(family, resolution, source_point_size, style_id(properties))] = (path, codepoints)
        family_resolutions[family].add(resolution)
        records.append((path, properties, codepoints, family, resolution))

    groups: dict[tuple[str, int], dict[int, tuple[Path, list[int]]]] = defaultdict(dict)
    group_resolutions: dict[tuple[str, int], int] = {}
    for path, properties, codepoints, family, resolution in records:
        if len(family_resolutions[family]) > 1:
            family = f"{family}-{resolution}dpi"
        point_size = int(properties["POINT_SIZE"]) // 10
        style = style_id(properties)
        key = (family, point_size)
        group_resolutions[key] = resolution
        if style in groups[key]:
            previous = groups[key][style][0]
            raise ValueError(f"duplicate {family} {point_size}pt style {style}: {previous} and {path}")
        groups[key][style] = (path, codepoints)

    physical_sizes = reported_sizes(groups, group_resolutions, args.target_dpi)
    total_bytes = 0
    generated_count = 0
    for (family, source_point_size), styles in sorted(groups.items()):
        style_fonts = {style: str(record[0]) for style, record in styles.items()}
        codepoints = {cp for _, cps in styles.values() for cp in cps}
        fallback_style_fonts = {}
        if family == "Charter":
            resolution = group_resolutions[(family, source_point_size)]
            for style in styles:
                fallback = source_fonts.get(("New-Century-Schoolbook", resolution, source_point_size, style))
                if fallback:
                    fallback_path, fallback_codepoints = fallback
                    fallback_style_fonts[style] = str(fallback_path)
                    codepoints.update(cp for cp in DASH_FALLBACK_CODEPOINTS if cp in fallback_codepoints)
        if not codepoints:
            print(
                f"Skipping {family} {source_point_size}pt: its BDF strikes have no encoded Unicode glyphs",
                file=sys.stderr,
            )
            continue
        # CrossPoint uses U+FFFD as its missing-glyph fallback when available.
        # X11 strikes generally do not contain it, so do not invent an empty
        # interval entry; the runtime can still fall back to another font.
        intervals = intervals_for(codepoints)
        point_size = physical_sizes[(family, source_point_size)]
        output = args.output_dir / family / f"{family}_{point_size}.cpfont"
        print(
            f"Generating {output} from {len(styles)} BDF style(s) "
            f"({source_point_size}pt at {group_resolutions[(family, source_point_size)]} DPI)...",
            file=sys.stderr,
        )
        total_bytes += generate_cpfont_multistyle(
            style_fonts, point_size, intervals, str(output),
            fallback_style_fonts=fallback_style_fonts)
        generated_count += 1

    print(
        f"Generated {generated_count} files from {len(bdf_paths)} BDF strikes "
        f"({total_bytes / 1024 / 1024:.2f} MB)",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
