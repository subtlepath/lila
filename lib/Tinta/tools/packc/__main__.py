"""Entry point for `python3 tools/packc` (and `python3 -m packc` from tools/)."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from packc.cli import main  # noqa: E402

sys.exit(main())
