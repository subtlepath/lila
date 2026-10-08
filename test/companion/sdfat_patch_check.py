"""Check patched real SdFat sources without changing installed dependencies.

Run: python3 test/companion/sdfat_patch_check.py .pio/libdeps/default/SdFat/src
Accepts original or patched sources. Checks source drift, idempotence,
compilation and iterator behavior on host filesystem images.
"""

import argparse
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from companion_sdfat_patch import patch_exfat, patch_fat, patch_fs_short_name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--compiler", default="g++")
    args = parser.parse_args()
    with TemporaryDirectory(prefix="lila-sdfat-check-") as temporary:
        root = Path(temporary) / "src"
        shutil.copytree(args.source, root)
        header = root / "FsLib/FsFile.h"
        original = header.read_text()
        patched = patch_fs_short_name(original)
        assert patch_fs_short_name(patched) == patched, "short-name patch must be idempotent"
        try:
            patch_fs_short_name(original.replace("getWriteError() const", "changedWriteError() const"))
        except ValueError:
            pass
        else:
            raise AssertionError("short-name patch anchor drift must be rejected")
        header.write_text(patched)
        shim = Path(temporary) / "host.h"
        shim.write_text("class __FlashStringHelper;\ninline unsigned long millis() { return 0; }\n")
        for name, patch in [("FatLib/FatFile.cpp", patch_fat),
                            ("ExFatLib/ExFatFile.cpp", patch_exfat)]:
            path = root / name
            original = path.read_text()
            patched = patch(original)
            assert patch(patched) == patched, "patch must be idempotent"
            try:
                patch(original.replace("\nfail:", "\nchanged_fail:"))
            except ValueError:
                pass
            else:
                raise AssertionError("changed failure labels must be rejected")
            path.write_text(patched)
            subprocess.run([
                args.compiler, "-std=c++20", "-fno-exceptions", "-fno-rtti",
                "-DENABLE_ARDUINO_FEATURES=0", "-DENABLE_ARDUINO_SERIAL=0",
                "-DENABLE_ARDUINO_STRING=0", "-DSPI_DRIVER_SELECT=3",
                "-DUSE_UTF8_LONG_NAMES=1", "-DDESTRUCTOR_CLOSES_FILE=1",
                "-include", str(shim), "-I" + str(root), "-fsyntax-only", str(path)
            ], check=True)
        executable = Path(temporary) / "enumeration-test"
        sources = [str(path) for folder in ("common", "FatLib", "ExFatLib", "FsLib")
                   for path in (root / folder).glob("*.cpp")]
        subprocess.run([
            args.compiler, "-std=c++20", "-O1", "-fno-exceptions", "-fno-rtti",
            "-DENABLE_ARDUINO_FEATURES=0", "-DENABLE_ARDUINO_SERIAL=0",
            "-DENABLE_ARDUINO_STRING=0", "-DSPI_DRIVER_SELECT=3",
            "-DUSE_BLOCK_DEVICE_INTERFACE=1", "-DUSE_UTF8_LONG_NAMES=1",
            "-DDESTRUCTOR_CLOSES_FILE=1", "-include", str(shim), "-I" + str(root),
            str(Path(__file__).with_name("sdfat_enumeration_test.cpp")),
            *sources, "-o", str(executable)
        ], check=True)
        subprocess.run([str(executable)], check=True)
    print("SdFat patch matching, idempotence, drift rejection, host compilation and runtime checks passed")


if __name__ == "__main__":
    main()
