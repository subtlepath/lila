"""Command line: python3 tools/packc [--content content] [--out build/course.pack] ..."""

from __future__ import annotations

import argparse
import os
import sys
import struct
import tempfile

from . import review
from .build import compile_course
from .diag import Diagnostics
from .dump import dump, size_report
from .emit import emit
from .reader import Pack, PackError


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="packc", description="Compile Tinta course content into a pack.")
    parser.add_argument("--content", default="content", help="content directory (default: content)")
    parser.add_argument("--out", help="pack to write (default: build/course.pack)")
    parser.add_argument("--check", action="store_true", help="validate only; write nothing")
    parser.add_argument("--baseline-from", metavar="PACK",
                        help="add identity history only if all course sections match this legacy pack")
    parser.add_argument("--dump", action="store_true", help="also write a human-readable listing next to the pack")
    parser.add_argument("--review", action="store_true", help="write printable review pages to build/review/")
    parser.add_argument("--review-dir", help="where --review writes (default: build/review/ for the default "
                             "content; otherwise review/ next to --out, or build/review-NAME/ for --content NAME)")
    parser.add_argument("--release", action="store_true",
                        help="fail unless every lesson, story and phrasebook file carries a current review hash")
    parser.add_argument("--allow-missing-examples", action="store_true",
                        help="development only: a lemma without an example sentence is a warning, "
                             "so a unit's lexicon rows can land before its lessons (not with --release)")
    parser.add_argument("--mark-reviewed", nargs="+", metavar="NAME",
                        help="record reviewed: {by, date, hash} in these lessons (u01.l02), stories or "
                             "phrasebook files (file name), or 'all'; needs --by")
    parser.add_argument("--by", help="reviewer name for --mark-reviewed")
    parser.add_argument("--quiet", action="store_true", help="print errors and warnings only")
    args = parser.parse_args(argv)
    args.review_dir = args.review_dir or _review_dir(args.content, args.out)
    args.out = args.out or "build/course.pack"

    if not os.path.isdir(args.content):
        print(f"packc: no content directory '{args.content}'", file=sys.stderr)
        return 2
    if args.release and args.allow_missing_examples:
        print("packc: --allow-missing-examples is for development; --release needs every example",
              file=sys.stderr)
        return 2
    if args.mark_reviewed:
        return _mark_reviewed(args)
    diag = Diagnostics()
    b = compile_course(args.content, diag, args.allow_missing_examples)

    unreviewed = [s for s in b.review_status if not s.ok and s.gated]
    if args.release:
        for s in unreviewed:
            diag.error(None, "review", f"{os.path.relpath(s.path)} ({s.kind} {s.name}): {s.reason}")
        if b.new_uids:
            diag.error(None, "ids", f"{b.new_uids} item(s) have no id in ids.lock yet; run a normal build "
                       "and commit content/ids.lock first")
    elif unreviewed:
        names = ", ".join(s.name for s in unreviewed[:8]) + (" ..." if len(unreviewed) > 8 else "")
        gated = sum(1 for s in b.review_status if s.gated)
        diag.warn(None, "review", f"{len(unreviewed)} of {gated} reviewable files are not "
                  f"reviewed or changed since review ({names}); --release would fail")

    for msg in diag.messages:
        if msg.level != "note" or not args.quiet:
            print(msg, file=sys.stderr)

    if args.review:
        pages = review.write_pages(b, args.review_dir)
        if not args.quiet:
            print(f"review: wrote {len(pages)} pages to {args.review_dir}/ (open index.html)")

    if diag.errors:
        print(f"packc: {len(diag.errors)} error(s), {len(diag.warnings)} warning(s)", file=sys.stderr)
        if args.release and any(m.code == "review" for m in diag.errors):
            print("packc: release refused: content must be reviewed first (python3 tools/packc --review)",
                  file=sys.stderr)
        return 1

    data, _report = emit(b, release=args.release)
    pack = Pack(data)
    if args.baseline_from:
        try:
            _validate_baseline(pack, args.baseline_from)
        except (OSError, ValueError, PackError, struct.error) as exc:
            print(f"packc: identity baseline refused: {exc}", file=sys.stderr)
            return 1
    summary = (f"{len(b.lemmas)} lemmas, {len(b.verbs)} verb tables, {len(b.sentences)} sentences, "
               f"{len(b.items)} items, {len(b.lessons)} lessons, {len(b.stories)} stories, "
               f"{len(b.phrase_entries)} phrases")
    if args.check:
        if not args.quiet:
            extra = f"; {b.new_uids} new item id(s) would be added to ids.lock" if b.new_uids else ""
            print(f"packc: check passed: {summary}{extra}; {len(diag.warnings)} warning(s)")
        return 0

    try:
        b.id_lock.write()
    except OSError as exc:
        print(f"packc: cannot persist item identities: {exc}", file=sys.stderr)
        return 1
    try:
        _write_pack(args.out, data)
    except OSError as exc:
        print(f"packc: cannot publish pack: {exc}", file=sys.stderr)
        return 1
    if args.dump:
        dump_path = os.path.splitext(args.out)[0] + ".dump.txt"
        with open(dump_path, "w", encoding="utf-8") as fh:
            fh.write(dump(pack))
    if not args.quiet:
        print(f"packc: wrote {args.out}: {summary}")
        if b.new_uids or b.retired_uids:
            print(f"ids.lock: {b.new_uids} new, {b.retired_uids} retired")
        print(size_report(pack))
        if args.dump:
            print(f"dump: {dump_path}")
    return 0


def _validate_baseline(candidate: Pack, path: str) -> None:
    with open(path, "rb") as fh:
        previous = Pack(fh.read())
    if not previous.crc_ok():
        raise ValueError("legacy pack checksum mismatch")
    if "IDEN" in previous.sections:
        raise ValueError("reference pack already has identity history")
    if previous.locale.lower() != candidate.locale.lower():
        raise ValueError("course language differs")
    tags = tuple(tag for tag in candidate.sections if tag != "IDEN")
    if tuple(previous.sections) != tags:
        raise ValueError("course section layout differs")
    for tag in tags:
        old_offset, old_size, old_count = previous.sections[tag]
        new_offset, new_size, new_count = candidate.sections[tag]
        if (old_size, old_count) != (new_size, new_count) or (
                previous.data[old_offset:old_offset + old_size] !=
                candidate.data[new_offset:new_offset + new_size]):
            raise ValueError(f"course section {tag} changed; use the original source and ids.lock")


def _write_pack(path: str, data: bytes) -> None:
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    staged = None
    try:
        with tempfile.NamedTemporaryFile(dir=directory, prefix=".packc-", delete=False) as fh:
            staged = fh.name
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(staged, path)
        staged = None
    finally:
        if staged is not None:
            os.unlink(staged)


def _mark_reviewed(args) -> int:
    if not args.by:
        print("packc: --mark-reviewed needs --by NAME", file=sys.stderr)
        return 2
    diag = Diagnostics()
    b = compile_course(args.content, diag, args.allow_missing_examples)
    errors = [m for m in diag.errors if m.code != "review"]
    if errors:
        for msg in errors:
            print(msg, file=sys.stderr)
        print("packc: fix the errors before marking content reviewed", file=sys.stderr)
        return 1
    try:
        marked = review.mark_reviewed(b, args.mark_reviewed, args.by)
    except KeyError as exc:
        names = ", ".join(s.name for s in b.review_status)
        print(f"packc: nothing named {exc} to mark (choose from: {names}, or all)", file=sys.stderr)
        return 2
    for s in marked:
        print(f"reviewed: {os.path.relpath(s.path)} ({s.kind} {s.name}) hash {s.hash} by {args.by}")
    check = compile_course(args.content, Diagnostics())
    stale = [s.name for s in check.review_status if s.name in {m.name for m in marked} and not s.ok]
    if stale:
        print(f"packc: internal error: hash still differs for {', '.join(stale)}", file=sys.stderr)
        return 1
    return 0


def _review_dir(content: str, out: str | None) -> str:
    """Review pages of a scratch --content must not overwrite the course's (parallel writers)."""
    if out:
        return os.path.join(os.path.dirname(os.path.abspath(out)) or ".", "review")
    if os.path.abspath(content) == os.path.abspath("content"):
        return os.path.join("build", "review")
    name = os.path.basename(os.path.normpath(os.path.abspath(content))) or "content"
    return os.path.join("build", f"review-{name}")
