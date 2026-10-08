"""Apply checked SdFat directory-error transformations before compilation."""
from pathlib import Path
import sys

Import("env")

project = Path(env.subst("$PROJECT_DIR"))
sys.path.insert(0, str(project / "scripts"))
from companion_sdfat_patch import patch_exfat, patch_fat, patch_fs_short_name

guard = "#define LILA_SDFAT_DIRECTORY_ERRORS 1"
updates = []
for root in (project / ".pio" / "libdeps").glob("*/SdFat/src"):
    for name, patch in [("FatLib/FatFile.cpp", patch_fat),
                        ("ExFatLib/ExFatFile.cpp", patch_exfat)]:
        path = root / name
        original = path.read_text()
        updates.append((path, original, patch(original)))
    header = root / "FsLib" / "FsFile.h"
    original = header.read_text()
    if guard not in original:
        if original.count("#pragma once") != 1:
            raise RuntimeError(f"SdFat directory-error guard does not match {header}")
        patched = original.replace("#pragma once", "#pragma once\n" + guard, 1)
    else:
        patched = original
    updates.append((header, original, patch_fs_short_name(patched)))

# Validate every source before publishing any transformation.
for path, original, patched in updates:
    if original != patched:
        path.write_text(patched)
