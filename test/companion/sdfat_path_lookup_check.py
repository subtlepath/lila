"""Compile and test checked lookup against the installed real SdFat Unicode code."""
import argparse
import json
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--compiler", default="g++")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    with TemporaryDirectory(prefix="lila-sdfat-path-") as temporary:
        binary = Path(temporary) / "path-lookup"
        subprocess.run([
            args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DHEX=16",
            "-I" + str(repo / "test/companion/font_removal_stubs"),
            "-I" + str(repo / "test/companion/hal_transfer_stubs"),
            "-I" + str(repo / "lib/Companion"), "-I" + str(repo), "-I" + str(args.source),
            str(repo / "test/companion/sdfat_path_lookup_test.cpp"),
            str(args.source / "common/FsUtf.cpp"), str(args.source / "common/upcase.cpp"),
            "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([
            args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DHEX=16",
            "-I" + str(repo / "test/companion/font_removal_stubs"),
            "-I" + str(repo / "test/companion/hal_transfer_stubs"),
            "-I" + str(repo / "lib/Companion"), "-I" + str(repo), "-I" + str(args.source),
            "-I" + str(repo / "lib/Memory"),
            "-I" + str(args.source.parent.parent / "ArduinoJson/src"),
            str(repo / "test/companion/sdfat_font_removal_references_test.cpp"),
            str(repo / "lib/Companion/CompanionEpubReferenceJson.cpp"),
            str(repo / "lib/Companion/CompanionRecords.cpp"),
            str(repo / "lib/hal/HalInventoryFileHash.cpp"),
            str(args.source / "common/FsUtf.cpp"), str(args.source / "common/upcase.cpp"),
            "-lcrypto", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([
            args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DHEX=16",
            "-I" + str(repo / "test/companion/font_removal_stubs"),
            "-I" + str(repo / "test/companion/hal_transfer_stubs"),
            "-I" + str(repo / "lib/Companion"), "-I" + str(repo), "-I" + str(args.source),
            "-I" + str(repo / "lib/Memory"),
            "-I" + str(args.source.parent.parent / "ArduinoJson/src"),
            str(repo / "test/companion/sdfat_font_removal_cohort_test.cpp"),
            str(repo / "lib/Companion/CompanionEpubReferenceJson.cpp"),
            str(repo / "lib/Companion/CompanionRecords.cpp"),
            str(repo / "lib/hal/HalInventoryFileHash.cpp"),
            str(args.source / "common/FsUtf.cpp"), str(args.source / "common/upcase.cpp"),
            "-lcrypto", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([
            args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DHEX=16",
            "-I" + str(repo / "test/companion/font_removal_stubs"),
            "-I" + str(repo / "test/companion/hal_transfer_stubs"),
            "-I" + str(repo / "lib/Companion"), "-I" + str(repo), "-I" + str(args.source),
            str(repo / "test/companion/sdfat_epub_removal_test.cpp"),
            str(repo / "lib/Companion/CompanionRecords.cpp"),
            str(repo / "lib/hal/HalInventoryFileHash.cpp"),
            str(args.source / "common/FsUtf.cpp"), str(args.source / "common/upcase.cpp"),
            "-lcrypto", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([
            args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DHEX=16",
            "-I" + str(repo / "test/companion/font_removal_stubs"),
            "-I" + str(repo / "test/companion/hal_transfer_stubs"),
            "-I" + str(repo / "lib/Companion"), "-I" + str(repo), "-I" + str(args.source),
            str(repo / "test/companion/sdfat_removal_metadata_test.cpp"),
            str(repo / "lib/Companion/CompanionRecords.cpp"),
            str(repo / "lib/hal/HalInventoryFileHash.cpp"),
            str(args.source / "common/FsUtf.cpp"), str(args.source / "common/upcase.cpp"),
            "-lcrypto", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([
            args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DHEX=16",
            "-I" + str(repo / "test/companion/font_removal_stubs"),
            "-I" + str(repo / "test/companion/hal_transfer_stubs"),
            "-I" + str(repo / "lib/Companion"), "-I" + str(repo),
            "-I" + str(args.source.parent.parent / "ArduinoJson/src"),
            "-I" + str(args.source),
            str(repo / "test/companion/sdfat_removal_json_test.cpp"),
            str(repo / "lib/Companion/CompanionEpubReferenceJson.cpp"),
            str(args.source / "common/FsUtf.cpp"), str(args.source / "common/upcase.cpp"),
            str(repo / "lib/Companion/CompanionRecords.cpp"),
            "-lcrypto", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([
            args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DHEX=16",
            "-I" + str(repo / "test/companion/font_removal_stubs"),
            "-I" + str(repo / "test/companion/hal_transfer_stubs"),
            "-I" + str(repo / "lib/Companion"), "-I" + str(repo), "-I" + str(args.source),
            "-I" + str(args.source.parent.parent / "ArduinoJson/src"),
            str(repo / "test/companion/sdfat_reference_snapshot_test.cpp"),
            str(repo / "lib/Companion/CompanionEpubReferenceJson.cpp"),
            str(repo / "lib/Companion/CompanionRecords.cpp"),
            str(repo / "lib/hal/HalInventoryFileHash.cpp"),
            str(args.source / "common/FsUtf.cpp"), str(args.source / "common/upcase.cpp"),
            "-lcrypto", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([
            args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DHEX=16",
            "-I" + str(repo / "test/companion/font_removal_stubs"),
            "-I" + str(repo / "test/companion/hal_transfer_stubs"),
            "-I" + str(repo / "lib/Companion"), "-I" + str(repo), "-I" + str(args.source),
            "-I" + str(args.source.parent.parent / "ArduinoJson/src"),
            "-I" + str(repo / "lib/Memory"),
            str(repo / "test/companion/sdfat_epub_reference_provider_test.cpp"),
            str(repo / "lib/Companion/CompanionEpubReferenceJson.cpp"),
            str(repo / "lib/Companion/CompanionRecords.cpp"),
            str(repo / "lib/hal/HalInventoryFileHash.cpp"),
            str(args.source / "common/FsUtf.cpp"), str(args.source / "common/upcase.cpp"),
            "-lcrypto", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([
            args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DHEX=16",
            "-I" + str(repo / "test/companion/font_removal_stubs"),
            "-I" + str(repo / "test/companion/hal_transfer_stubs"),
            "-I" + str(repo / "lib/Companion"), "-I" + str(repo), "-I" + str(args.source),
            "-I" + str(repo / "lib/Memory"),
            "-I" + str(args.source.parent.parent / "ArduinoJson/src"),
            str(repo / "lib/Companion/CompanionEpubReferenceJson.cpp"),
            str(repo / "test/companion/sdfat_multi_path_removal_plan_test.cpp"),
            str(repo / "lib/hal/HalInventoryFileHash.cpp"),
            str(repo / "lib/Companion/CompanionRecords.cpp"),
            str(args.source / "common/FsUtf.cpp"), str(args.source / "common/upcase.cpp"),
            "-lcrypto", "-o", str(binary),
        ], check=True)
        fixture = json.loads((repo / "protocol/fixtures/MultiPathRemovalPlan.json").read_text())
        addresses = json.loads((repo / "protocol/fixtures/RemovalCohortAddress.json").read_text())
        subprocess.run([str(binary), fixture["binaryHex"],
                        *[row["address"] for row in addresses["addresses"]]], check=True)
        subprocess.run([
            args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DHEX=16",
            "-I" + str(repo / "test/companion/hal_transfer_stubs"),
            "-I" + str(repo / "lib/Companion"), "-I" + str(repo), "-I" + str(args.source),
            str(repo / "test/companion/sdfat_dictionary_removal_test.cpp"),
            str(repo / "lib/Companion/CompanionRecords.cpp"),
            str(repo / "lib/hal/HalInventoryFileHash.cpp"),
            str(args.source / "common/FsUtf.cpp"), str(args.source / "common/upcase.cpp"),
            "-lcrypto", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([
            args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DHEX=16",
            "-I" + str(repo / "test/companion/hal_transfer_stubs"),
            "-I" + str(repo / "lib/Companion"), "-I" + str(repo), "-I" + str(args.source),
            str(repo / "test/companion/sdfat_course_removal_test.cpp"),
            str(repo / "lib/Companion/CompanionRecords.cpp"),
            str(repo / "lib/hal/HalInventoryFileHash.cpp"),
            str(args.source / "common/FsUtf.cpp"), str(args.source / "common/upcase.cpp"),
            "-lcrypto", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([
            args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DHEX=16",
            "-I" + str(repo / "test/companion/hal_transfer_stubs"),
            "-I" + str(repo / "lib/Companion"), "-I" + str(repo), "-I" + str(args.source),
            str(repo / "test/companion/sdfat_course_state_isolation_test.cpp"),
            str(repo / "lib/Companion/CompanionRecords.cpp"),
            str(repo / "lib/hal/HalInventoryFileHash.cpp"),
            str(args.source / "common/FsUtf.cpp"), str(args.source / "common/upcase.cpp"),
            "-lcrypto", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([
            args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DHEX=16",
            '-DCOMPANION_FIXTURE_DIR="' + str(repo / "protocol/fixtures") + '"',
            "-I" + str(repo / "test/companion/hal_transfer_stubs"),
            "-I" + str(repo / "lib/Companion"), "-I" + str(repo), "-I" + str(args.source),
            str(repo / "test/companion/sdfat_dictionary_removal_references_test.cpp"),
            str(repo / "lib/Companion/CompanionRecords.cpp"),
            str(repo / "lib/hal/HalInventoryFileHash.cpp"),
            str(args.source / "common/FsUtf.cpp"), str(args.source / "common/upcase.cpp"),
            "-lcrypto", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([
            args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DHEX=16",
            "-I" + str(repo / "test/companion/hal_transfer_stubs"),
            "-I" + str(repo / "lib/Companion"), "-I" + str(repo), "-I" + str(args.source),
            str(repo / "test/companion/sdfat_dictionary_removal_cohort_plan_test.cpp"),
            str(repo / "lib/Companion/CompanionRecords.cpp"),
            str(args.source / "common/FsUtf.cpp"), str(args.source / "common/upcase.cpp"),
            "-lcrypto", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([
            args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DHEX=16",
            '-DCOMPANION_FIXTURE_DIR="' + str(repo / "protocol/fixtures") + '"',
            "-I" + str(repo / "test/companion/hal_transfer_stubs"),
            "-I" + str(repo / "lib/Companion"), "-I" + str(repo), "-I" + str(args.source),
            str(repo / "test/companion/sdfat_dictionary_removal_cohort_test.cpp"),
            str(repo / "lib/Companion/CompanionRecords.cpp"),
            str(repo / "lib/hal/HalInventoryFileHash.cpp"),
            str(args.source / "common/FsUtf.cpp"), str(args.source / "common/upcase.cpp"),
            "-lcrypto", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([
            args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DHEX=16",
            "-I" + str(repo / "test/companion/hal_transfer_stubs"),
            "-I" + str(repo / "lib/Companion"), "-I" + str(repo), "-I" + str(args.source),
            str(repo / "test/companion/sdfat_dictionary_removal_plan_writer_test.cpp"),
            str(repo / "lib/Companion/CompanionRecords.cpp"),
            str(args.source / "common/FsUtf.cpp"), str(args.source / "common/upcase.cpp"),
            "-lcrypto", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([
            args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DHEX=16",
            '-DCOMPANION_FIXTURE_DIR="' + str(repo / "protocol/fixtures") + '"',
            "-I" + str(repo / "test/companion/hal_transfer_stubs"),
            "-I" + str(repo / "lib/Companion"), "-I" + str(repo), "-I" + str(args.source),
            str(repo / "test/companion/sdfat_dictionary_removal_collection_test.cpp"),
            str(repo / "lib/Companion/CompanionRecords.cpp"),
            str(repo / "lib/hal/HalInventoryFileHash.cpp"),
            str(args.source / "common/FsUtf.cpp"), str(args.source / "common/upcase.cpp"),
            "-lcrypto", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([
            args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DHEX=16",
            '-DCOMPANION_FIXTURE_DIR="' + str(repo / "protocol/fixtures") + '"',
            "-I" + str(repo / "test/companion/font_removal_stubs"),
            "-I" + str(repo / "lib/Memory"),
            "-I" + str(args.source.parent.parent / "ArduinoJson/src"),
            "-I" + str(repo / "test/companion/hal_transfer_stubs"),
            "-I" + str(repo / "lib/Companion"), "-I" + str(repo), "-I" + str(args.source),
            str(repo / "test/companion/sdfat_dictionary_removal_session_test.cpp"),
            str(repo / "lib/Companion/CompanionEpubReferenceJson.cpp"),
            str(repo / "lib/hal/HalInventoryIndexStorage.cpp"),
            str(repo / "lib/hal/HalInventoryPublicationValidator.cpp"),
            str(repo / "lib/Companion/CompanionRecords.cpp"),
            str(repo / "lib/hal/HalInventoryFileHash.cpp"),
            str(args.source / "common/FsUtf.cpp"), str(args.source / "common/upcase.cpp"),
            "-lcrypto", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
    print("Installed SdFat/ArduinoJson removal and snapshot checks passed")


if __name__ == "__main__":
    main()
