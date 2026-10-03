#!/usr/bin/env python3
"""Render reader pages from EPUBs with TypesetPreview and score the typesetting.

  preview.py spine BOOK                       list spine documents with their text size
  preview.py render BOOK[:i,j] ... --out DIR [--pages N] -- TOOL_ARGS...
  preview.py score STATS.jsonl ...            print typesetting metrics

BOOK is an .epub file or an unpacked EPUB directory; i,j pick spine indices (default: the two longest documents).
TOOL_ARGS go to TypesetPreview (e.g. --family times --size 12 --set lineCompression=1.2). Build the tool with
  cmake -S test -B build/test && cmake --build build/test --target TypesetPreview
"""

import argparse
import json
import re
import statistics
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from xml.etree import ElementTree

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "build/test/typeset_preview/TypesetPreview"
NS = {"c": "urn:oasis:names:tc:opendocument:xmlns:container", "opf": "http://www.idpf.org/2007/opf",
      "dc": "http://purl.org/dc/elements/1.1/"}


class Book:
    def __init__(self, path: Path):
        if path.is_file():
            root = Path(tempfile.mkdtemp(prefix="epub-"))
            with zipfile.ZipFile(path) as archive:
                archive.extractall(root)
            path = root
        self.root = path
        container = ElementTree.parse(path / "META-INF/container.xml")
        opf_path = path / container.find(".//c:rootfile", NS).attrib["full-path"]
        opf = ElementTree.parse(opf_path)
        base = opf_path.parent
        manifest = {item.attrib["id"]: item.attrib for item in opf.iterfind(".//opf:manifest/opf:item", NS)}
        self.language = (opf.findtext(".//dc:language", default="en", namespaces=NS) or "en").strip()
        self.css = [base / item["href"] for item in manifest.values() if item.get("media-type") == "text/css"]
        self.spine = [base / manifest[ref.attrib["idref"]]["href"] for ref in opf.iterfind(".//opf:spine/opf:itemref", NS)
                      if ref.attrib.get("idref") in manifest]

    def text_length(self, index: int) -> int:
        html = self.spine[index].read_text(encoding="utf-8", errors="replace")
        return len(re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", html)))

    def longest(self, count: int) -> list:
        ranked = sorted(range(len(self.spine)), key=self.text_length, reverse=True)
        return sorted(ranked[:count])


def render(book_spec: str, out: Path, pages: int, tool_args: list) -> Path:
    path, _, picks = book_spec.partition(":")
    book = Book(Path(path).expanduser())
    indices = [int(i) for i in picks.split(",")] if picks else book.longest(2)
    out.mkdir(parents=True, exist_ok=True)
    stats = out / "lines.jsonl"
    stats.unlink(missing_ok=True)
    args = [str(TOOL), "--out", str(out), "--stats", str(stats), "--set", f"lang={book.language}",
            "--set", f"maxPages={pages}"]
    for css in book.css:
        args += ["--css", str(css)]
    args += tool_args + [str(book.spine[i]) for i in indices]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"{book_spec}: {result.stderr.strip()}")
    for pgm in sorted(out.glob("page-*.pgm")):
        to_png(pgm)
    (out / "summary.txt").write_text(result.stdout)
    return stats


def to_png(pgm: Path):
    from PIL import Image
    Image.open(pgm).save(pgm.with_suffix(".png"))
    pgm.unlink()


def score(paths: list) -> dict:
    """Justified-line looseness, hyphenation, measure and page-fill metrics over every laid-out line."""
    lines = []
    for source, path in enumerate(paths):
        for raw in Path(path).read_text().splitlines():
            if raw:
                line = json.loads(raw)
                line["page"] = (source, line["page"])
                lines.append(line)
    if not lines:
        return {}
    flush, loose, very_loose, tight = 0, 0, 0, 0
    stretch_ratios, chars, hyphen_ends = [], [], 0
    consecutive_hyphens, ladders, run = 0, 0, 0
    for line in lines:
        words = line["words"]
        if not words:
            continue
        text = "".join(w[0] for w in words)
        chars.append(len(text) + sum(1 for a, b in zip(words, words[1:]) if b[1] - (a[1] + a[2]) > 2))
        hyphen = words[-1][0].endswith(("-", "‐")) and len(words[-1][0]) > 1
        hyphen_ends += hyphen
        run = run + 1 if hyphen else 0
        consecutive_hyphens += run == 2
        ladders += run == 3
        right = words[-1][1] + words[-1][2]
        if line["align"] != 0 or right < line["avail"] - 1:
            continue  # ragged, centred, or a paragraph's last line
        flush += 1
        gaps = [b[1] - (a[1] + a[2]) for a, b in zip(words, words[1:])]
        spaces = [g for g in gaps if g > 1]
        if not spaces:
            continue
        ratio = max(spaces) / NATURAL_SPACE[0]
        stretch_ratios.append(ratio)
        loose += ratio > 1.5
        very_loose += ratio > 2.0
        tight += min(spaces) < NATURAL_SPACE[0] * 0.75
    pages = {}
    for line in lines:
        pages.setdefault(line["page"], []).append(line)
    per_page = [len(v) for v in pages.values()]
    # A paragraph's last line is one that stops short of the measure; widows are such lines alone at the top of a
    # page, orphans are a paragraph's first line alone at the bottom.
    def ends_paragraph(line):
        words = line["words"]
        return not words or words[-1][1] + words[-1][2] < line["avail"] - 1
    widows = orphans = 0
    ordered = [pages[key] for key in sorted(pages)]
    for prev_page, page in zip(ordered, ordered[1:]):
        if len(page) > 1 and ends_paragraph(page[0]) and not ends_paragraph(prev_page[-1]):
            widows += 1
        if len(prev_page) > 1 and not ends_paragraph(prev_page[-1]) and ends_paragraph(prev_page[-2]):
            orphans += 1
    return {
        "lines": len(lines),
        "pages": len(pages),
        "lines/page": round(statistics.median(per_page), 1),
        "chars/line": round(statistics.median(chars), 1),
        "flush": flush,
        "loose%": round(100 * loose / max(flush, 1), 1),
        "veryLoose%": round(100 * very_loose / max(flush, 1), 1),
        "tight%": round(100 * tight / max(flush, 1), 1),
        "maxGap/space p95": round(sorted(stretch_ratios)[int(0.95 * (len(stretch_ratios) - 1))], 2)
        if stretch_ratios else 0,
        "hyphen%": round(100 * hyphen_ends / len(lines), 1),
        "hyphenPairs": consecutive_hyphens,
        "ladders3": ladders,
        "widows": widows,
        "orphans": orphans,
    }


# Natural interword space in px for the run being scored; set from --space.
NATURAL_SPACE = [6]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    spine = sub.add_parser("spine")
    spine.add_argument("book")
    rend = sub.add_parser("render")
    rend.add_argument("books", nargs="+")
    rend.add_argument("--out", type=Path, required=True)
    rend.add_argument("--pages", type=int, default=40)
    sc = sub.add_parser("score")
    sc.add_argument("stats", nargs="+")
    sc.add_argument("--space", type=int, default=6, help="natural interword space in px")

    argv = sys.argv[1:]
    tool_args = []
    if "--" in argv:
        split = argv.index("--")
        argv, tool_args = argv[:split], argv[split + 1:]
    args = parser.parse_args(argv)

    if args.command == "spine":
        book = Book(Path(args.book).expanduser())
        print(f"language={book.language} css={[c.name for c in book.css]}")
        for i, doc in enumerate(book.spine):
            print(f"{i:3d} {book.text_length(i):8d} {doc.name}")
    elif args.command == "render":
        for spec in args.books:
            name = Path(spec.partition(":")[0]).stem.split(" ")[0]
            stats = render(spec, args.out / name, args.pages, tool_args)
            print(name, (args.out / name / "summary.txt").read_text().strip(), sep="\t")
    else:
        NATURAL_SPACE[0] = args.space
        print(json.dumps(score(args.stats), indent=1))


if __name__ == "__main__":
    main()
