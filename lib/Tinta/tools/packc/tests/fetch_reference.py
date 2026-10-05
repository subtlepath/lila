"""Download the external conjugation reference used by test_reference.py.

Source: Fred Jehle's Conjugated Spanish Verb Database, compiled by Brian
Ghidinelli, https://github.com/ghidinelli/fred-jehle-spanish-verbs
(jehle_verb_database.csv: 637 verbs, every mood and tense).
Licence: Creative Commons Attribution-NonCommercial-ShareAlike 3.0 Unported.
It is used for tests only: it is downloaded into build/reference/ (ignored by
git) and never copied into the repository or the pack.

    python3 tools/packc/tests/fetch_reference.py
"""

from __future__ import annotations

import os
import sys
import urllib.request

URL = ("https://raw.githubusercontent.com/ghidinelli/fred-jehle-spanish-verbs/master/"
       "jehle_verb_database.csv")
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
PATH = os.path.join(ROOT, "build", "reference", "jehle_verb_database.csv")


def main() -> int:
    os.makedirs(os.path.dirname(PATH), exist_ok=True)
    with urllib.request.urlopen(URL, timeout=60) as response:  # noqa: S310 (fixed https URL)
        data = response.read()
    with open(PATH, "wb") as fh:
        fh.write(data)
    print(f"wrote {PATH} ({len(data):,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
