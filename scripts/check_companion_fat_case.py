#!/usr/bin/env python3
"""Compare companion filename casing against the installed SdFat mapping."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdfat", type=Path, default=repo / ".pio/libdeps/default/SdFat/src")
    parser.add_argument("--compiler", default="c++")
    args = parser.parse_args()
    sdfat = args.sdfat.resolve()
    with tempfile.TemporaryDirectory(prefix="lila-fat-case-") as directory:
        work = Path(directory)
        source = work / "parity.cpp"
        source.write_text(
            '#include "lib/Companion/CompanionDictionaryFatCase.h"\n'
            '#include "common/upcase.h"\n'
            '#include <cstdio>\n'
            'int main() {\n'
            '  for (unsigned value = 0; value <= 0xffff; ++value) {\n'
            '    if (companion::dictionaryFatUpcase(value) != toUpcase(value)) {\n'
            '      std::printf("Mismatch: %04x\\n", value);\n'
            '      return 1;\n'
            '    }\n'
            '  }\n'
            '  std::puts("All 65536 BMP values match installed SdFat upcase mapping");\n'
            '}\n'
        )
        executable = work / "parity"
        subprocess.run(
            [args.compiler, "-std=c++20", "-Os", f"-I{repo}", f"-I{sdfat}",
             str(source), str(sdfat / "common/upcase.cpp"), "-o", str(executable)],
            check=True,
        )
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
