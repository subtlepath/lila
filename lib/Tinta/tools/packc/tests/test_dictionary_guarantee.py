"""Dictionary rows never change the course: real content compiled with and without
content/lexicon/dictionary*.tsv must link every token and offer every option alike."""

from __future__ import annotations

import glob
import os
import shutil
import tempfile
import unittest

from packc.build import compile_course
from packc.diag import Diagnostics

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
CONTENT = os.path.join(ROOT, "content")


def snapshot(build, root: str) -> tuple[dict, dict]:
    """Token links and item options, by content rather than by (shifting) ids or paths."""
    links = {}
    for sent in build.sentences:
        where = (os.path.relpath(sent.src.where.path, root), sent.src.where.line)
        links[(where, sent.es)] = [
            (t.surface, t.lemma.key if t.lemma else None, t.tag) for t in sent.tokens]
    options = {}
    for item in build.items:
        if item.kind in (0, 1):
            shown = [build.lemmas[c].key for c in item.candidates]
        elif item.kind == 5:
            shown = [build.sentences[c].es for c in item.candidates]
        else:
            shown = list(item.candidates)
        options[item.key] = (shown, item.must_show)
    return links, options


@unittest.skipUnless(glob.glob(os.path.join(CONTENT, "lexicon", "dictionary*.tsv")), "no dictionary rows yet")
class DictionaryNeverChangesTheCourse(unittest.TestCase):
    def test_links_and_options_are_identical(self):
        with tempfile.TemporaryDirectory() as tmp:
            without = os.path.join(tmp, "content")
            shutil.copytree(CONTENT, without)
            for path in glob.glob(os.path.join(without, "lexicon", "dictionary*.tsv")):
                os.remove(path)
            with_links, with_options = snapshot(compile_course(CONTENT, Diagnostics(), True), CONTENT)
            bare_links, bare_options = snapshot(compile_course(without, Diagnostics(), True), without)
        self.assertEqual(set(with_links), set(bare_links))
        changed = [k for k in bare_links if with_links[k] != bare_links[k]]
        self.assertEqual(changed[:10], [], "token links changed by dictionary rows")
        self.assertEqual(set(with_options), set(bare_options))
        changed = [k for k in bare_options if with_options[k] != bare_options[k]]
        self.assertEqual(changed[:10], [], "item options changed by dictionary rows")


if __name__ == "__main__":
    unittest.main()
