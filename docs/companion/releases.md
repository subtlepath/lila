# Companion release metadata

The published-release workflow builds `companion-release.json` after collecting
all five firmware binaries and compiling the Spanish course. The manifest is
uploaded alongside those assets. No release or remote operation is performed by
running the generator locally.

The generator validates each ESP image's chip, embedded board tag, segment
lengths, XOR checksum, optional SHA-256 trailer, and OTA partition size before
recording the whole-file SHA-256. It uses the existing Tinta compiler reader and
CRC check for `course.pack`. It refuses missing or unexpected firmware assets.
Board classes match `FirmwareBoardTag.cpp`: the combined X3/X4 asset is tagged
`x4`; Sticky, X4 Pro, X4 Classic, and Paper Mono each use their own tag. C3 chip
ID is 5; S3 chip ID is 9. Both app partitions currently have size `0x640000` in
`partitions.csv`; a test checks this bound against the generator.

## Schema version 1

Top-level fields are `schemaVersion`, `version`, the exact Git release `tag`, full
source `revision`, `channel` (`stable` or `rc`), and `assets`. The generator is
deterministic and does not insert wall-clock timestamps or local filesystem
paths. Each asset has `name`, `kind`, byte `length`, lowercase hex `sha256`, and a
GitHub release download `url`.

Firmware assets additionally declare:

- `boardTags` and ESP `chipId` for compatibility.
- `otaPartitionBytes`, and `minimumBatteryPercent` of 30.
- `companionProtocol`: an implemented protocol range with `minimum` and
  `maximum`, or `null` when companion mode is unavailable.
- `initialUpgradeRequired`: true when companion mode is unavailable.
- `stateSchema`: the minimum and maximum migrated-state schema the firmware can
  read. A companion must block installing a release whose range excludes the
  reader's current schema.
- `supportedPackFormatMajors`: currently `[1]` for X3/X4, X4 Pro, and X4 Classic;
  empty for Sticky and Paper Mono, whose profiles omit Tinta.

The course declares `packFormat` (major and minor), `contentVersion`, and `locale`.
Firmware's pack compatibility uses the major version because the runtime checks
that version and validates record strides for compatible minor extensions.

Current workflow values are protocol 0 and state schema 0. The pairing/discovery
prototype has no installation or synchronized-state handlers, so these releases must offer the existing
OTA/SD upgrade instructions instead of enabling companion-driven installation.
When radio and migrated-state support is integrated, update the workflow's
explicit protocol and state range to match the release's implemented behavior.

This metadata is not a signature. Downloads must come from the configured lila
release source over HTTPS and be checked against the manifest. The reader must
still run `firmware_flash::validateImageFile`, check battery and current state
compatibility, release radios, and verify the running build after reconnecting.
Those companion installation flows remain to be implemented.

## Local generation and verification

Install the course compiler's dependency (`pyyaml>=6.0.2`). In `dist/`, place the
five `lila-<version>-<device>.bin` files and `course.pack`, then run:

```sh
python3 scripts/build_companion_release_manifest.py \
  --assets-dir dist --tag v0.1.0 --revision "$(git rev-parse HEAD)" \
  --repository subtlepath/lila --channel stable \
  --companion-protocol 0 --state-schema-min 0 --state-schema-max 0 \
  --output dist/companion-release.json
python3 -m unittest discover -s test/release_manifest -v
```

The output replaces an existing manifest only after all assets validate. Tests
exercise stable/RC metadata, wrong board/chip, missing tags, corruption,
truncation, segment overflow, oversize firmware, corrupt packs, asset set
mismatches, protocol/state compatibility, and preservation of existing output on
failure. CI runs these tests. Publishing still requires the existing release
workflow and separate user authorization.

Reader discovery now fills `DeviceDescriptor.runningBuild` with SHA-256 over
the complete running ESP image, including its optional SHA trailer. The image
extent is computed from the bounded segment table and checksum padding; unused
OTA partition bytes are excluded. Hashing borrows the companion's transfer
scratch region and allocates no heap. Flash read failures leave the descriptor's
build hash zero, so a reconnect cannot verify an update from that response.
Companion staging, flashing, and authenticated reconnect verification are now
implemented; hardware acceptance remains required. On hardware, compare the discovered hash with `sha256sum`
of the exact flashed `.bin` and repeat discovery while BLE commands are queued.

Authenticated BLE `InstallFirmware` now supports a read-only information query:
`FWQ` plus binary version 1 and the current 16-byte SD generation. The 76-byte
`FWI1` response has board/battery/protocol bytes at 4..7, SD generation at 8,
running image SHA-256 at 24, chip ID at 56, two zero reserved bytes at 58, next
OTA partition size (u64 LE) at 60, state schema (u32 LE) at 68, and validated
journal-version bits (u32 LE) at 72. Bits 0..2 represent TJH1..TJH3. Schema 1
is reported conservatively when companion course-binding/migration records or
distributed journals exist; otherwise schema 0 denotes legacy state. Unknown
partition, hash, or journal inventory yields an error response.

Apple's authenticated session checks the reply request identity and generation,
then matches board, running build, and protocol with discovery. Firmware
admission can consume this fresh information, including battery and an explicit
chip match, while retaining complete image validation. Querying does not stage
or flash an image; install requests, radio teardown, and reboot verification
remain pending. The release workflow now declares protocol 1, schemas 0–1, and TJH1–3 support.

Declared firmware content can now stage at `/Companion/firmware.bin` through the
shared resumable transfer protocol. Only staging format 1, a zero logical
identity, and images of at least 64 KiB are admitted at this fixed destination.
Live and startup entry points inject the existing flasher's validation-only
callback, which checks against the actual next OTA partition. Missing callbacks
block commit/recovery. Image validation runs before installation and again for
staged-file recovery. The callback does not erase flash or switch boot slots.
Apple command encoding and its firmware staging runner support this destination;
the separate BLE installation command is implemented below. A Committed
content transfer means the image is staged, not that firmware was installed.

Apple `TransferRunner.stageFirmware` now queries fresh authenticated reader
information, checks release metadata and the complete local image, and runs the
resumable declared transfer. Ordinary content runs still reject firmware.
`FirmwareStagingReceipt` is reconstructed from the durable completed transfer,
retained declaration, and firmware metadata after database restart. It records
only SD staging, never flash or boot success. Lost chunk replies pause staging;
lost commit replies retain commit intent and can resume to the same receipt.
`prepareFirmwareHandoff` captures authenticated compatibility information and
begins the declared transaction before BLE yields to Wi-Fi. The returned
preparation binds the job and image; Wi-Fi staging additionally checks the
authenticated session and lease against reader, storage generation, installation,
and transaction. Compatibility and the local image are validated again before
sending bytes. A lost encrypted Wi-Fi reply can resume over BLE using the same
declaration. This snapshot authorizes staging only; flashing requires fresh
reader checks. Flash command, radio teardown for flashing, and post-reconnect update success
remain pending.

The Apple Updates view now offers staging for imported/downloaded firmware on an
authenticated connected reader. It uses the shared Wi-Fi preference and BLE
fallback, reports SD staging separately from flashing, and supports pausing.
`enqueueFirmware` atomically reuses unfinished jobs for the same image, reader,
storage generation, and installation; another installation cannot claim them.
The release compatibility metadata must exist before queuing, and the runner
checks the actual reader before transfer. A completed job is a past staging
receipt; staging again creates a new transaction to verify the current SD file.
App syntax was checked on Linux; full SwiftUI type checking and native iOS/macOS
execution still require Xcode.

The installation request codec is `FWF` plus binary version 1, exactly 104 bytes:

| Offset | Field |
| --- | --- |
| 4 | Storage generation, 16 bytes |
| 20 | Staging transaction, 16 bytes |
| 36 | Complete image SHA-256, 32 bytes |
| 68 | Image length, little-endian uint64 |
| 76, 80 | Minimum/maximum reader state schema, uint32 each |
| 84 | Supported journal-format mask, uint32 (bits 0–2 for TJH1–3) |
| 88, 89 | Minimum/maximum companion protocol, byte each |
| 90, 91 | Minimum battery percent and board, byte each |
| 92 | Chip ID, uint16 |
| 94 | Two reserved zero bytes |
| 96 | Required OTA partition capacity, uint64 |

The native admission helpers require a matching committed declared firmware
transfer at the fixed SD destination and fresh reader compatibility information.
The Apple encoder requires the staging receipt, matching release metadata, and
fresh authenticated reader information. The shared fixture tests both codecs.
The BLE installation handler now uses these helpers to rehash the current SD
file, record durable intent, recheck compatibility, release radio resources, and
invoke the existing flasher. Apple durable update tracking, final confirmation, and authenticated reconnect
verification are implemented below; native Apple and hardware validation remain
required. A past staging receipt alone cannot
prove that the current staged file still matches.

`FirmwareInstallIntent` persists the exact request plus the authenticated
installation owner (16 bytes), followed by little-endian CRC32 over those 120
bytes. Its 124-byte record is published from
`/.crosspoint/companion/firmware-install-next` to
`/.crosspoint/companion/firmware-install`, with synced writes and readback.
Exact retries reuse intent; different owners, transactions, metadata, malformed
records, and ambiguous simultaneous canonical/staging records are preserved and
rejected. Complete staged records can resume after interrupted publication.
Torn staging records are preserved for recovery inspection, not overwritten.

`admitFirmwareInstallation` checks fresh compatibility and the committed transfer,
then hashes the current fixed SD file before persisting authorization. The
storage backend's existing `verify` implementation supplies the complete hash;
the host fake tests admission ordering and publication faults. No heap buffer is
added; callers borrow at least 124 bytes of serialized session scratch.
`retireVerified` removes intent only after the running-image digest, board, chip,
and storage generation match the request, with no competing staging record.
Host tests cover interrupted write/rename/remove before and after effect, exact
retry, conflicting ownership, tampering, and rejection of an old running image.
The live BLE activity/flash path now uses these helpers.

Startup now includes canonical/staged firmware intent in companion recovery.
After transfer and inventory recovery, it hashes the complete running image
through an injected flasher callback using the existing recovery workspace.
Only an exact image hash and storage generation can retire canonical intent;
the image hash covers the board/chip bytes previously validated at staging.
Missing hash support, conflicting stage records, corrupt intent, and I/O errors
retain evidence and route startup into companion recovery. A valid intent with an
old running image is retained while normal startup permits an authenticated retry;
new content transfers remain blocked until intent is resolved.
Startup does not automatically flash a pending image. The recovery object adds
fixed request/owner/digest fields to its existing checked heap allocation; no
second hashing buffer or per-chunk allocation is introduced.

For device verification, stage/install a known compatible image once the live
handler is connected, then check serial output for
`Firmware boot verified; installation intent retired` after reboot. An old image
must instead log `Firmware installation has not booted; authenticated retry available` and
preserve `/.crosspoint/companion/firmware-install`. Power-cut and heap/stack checks
on C3/S3 remain required; the host tests do not establish those results.

The authenticated BLE `InstallFirmware` handler now accepts the 104-byte request
and replies with a result byte (`FirmwareInstallIntentResult`, 0–5) plus the
16-byte staging transaction. Result zero means durable authorization, not boot
success. It excludes journal/backup operations and Wi-Fi sessions, requires the
matching committed declared firmware transfer, gathers fresh reader information,
rehashes the current SD file, and publishes intent. It then stops command
processing, gives the acknowledgement a short delivery window, and rechecks
admission before radio teardown. A lost acknowledgement does not cancel durable
intent; the Apple update flow must reconnect and resolve it.

The activity closes radio/journal/inventory resources, checks battery and the
current SD hash again, and calls the shared validator/flasher. With an expected
image hash, the flasher reads back and hashes the written OTA partition using its
existing 4 KiB buffer before switching boot slots. A mismatched image never
reaches boot-slot selection. Failure preserves intent; success restarts for
startup verification. Wi-Fi transfers remain supported for staging, while the
installation command is sent over BLE after handoff completion.

Apple `AuthenticatedReaderSession.installFirmware` validates the acceptance
frame's command, request ID, transaction, payload size, and result. It is not yet
connected to the Updates view through an explicit installation confirmation.
Authenticated reconnect checks durable installation records against the running
image. Physical
radio teardown, readback, power-cut recovery, C3 heap/stack behavior, and native
Apple execution still need hardware/Xcode verification.

SQLite schema 31 adds `firmware_installations`, keyed by the completed staging job,
with the immutable 104-byte request and a boot-verification flag. Preparing an
installation binds the receipt, release metadata, current reader information,
and installation owner atomically; another unresolved installation on the same
reader is rejected. Lost acceptance replies and database restarts preserve the
same request. Neither transfer completion nor acceptance sets boot verification.

`TransferRunner.installFirmware` checks authenticated session ownership, reads
fresh reader information, revalidates the local image and metadata, persists
installation tracking before sending the command, and validates acceptance.
`verifyFirmwareInstallation` takes discovery from the newly authenticated reader
and requires matching reader identity, generation, owner, board, and exact image
hash before setting the durable flag. A matching boot can be verified even below
the flashing battery threshold; a previously verified historical installation
cannot be reused to flash after the running image changes. Stage a new transaction
for an intentional reinstall. Tests cover a lost acceptance, restart/retry,
old-image rejection, wrong-owner rejection, low-battery boot confirmation, and
retention of the final verification across database restart.

The Updates screen now offers “Install staged firmware” for a completed staging
job owned by the authenticated reader session. Confirmation captures that exact
job; the runner rechecks ownership, compatibility, image bytes, and battery before
persisting/sending installation. Accepted installation disconnects the app and
asks the user to reconnect after the reader restarts. A lost reply leaves durable
tracking for the next connection instead of declaring failure or success.

On authentication, pairing, or Bluetooth restoration after Wi-Fi handoff, the app
loads staged jobs and installation history scoped to the reader and installation.
It verifies unresolved installations for the current storage generation against
fresh authenticated discovery, then displays “Awaiting boot verification” or
“Boot previously verified”. Disconnect removes per-connection action choices;
durable database records remain. Syntax/localization checks passed on Linux;
Xcode SwiftUI type checking, native app execution, and physical end-to-end update
acceptance remain required.

The release manifest workflow now declares companion protocol 1, reader state
schemas 0–1, and supported journal header versions 1, 2, and 3. These describe the
current source: unbound legacy state is schema 0, while course bindings,
migration records, or distributed journals require schema 1. Generated metadata
continues to require exact board/chip identity, 30% battery, and the actual
0x640000-byte OTA capacity from `partitions.csv`. Older manually imported release
metadata with protocol 0 remains an initial-upgrade requirement; it is not
silently promoted. This change edits the workflow only and publishes nothing.

Companion persistence uses the header-only byte/CRC helpers in
`lib/Serialization/BinaryRecordBytes.h`, independently of Tinta's application
library. `core/srs/Bytes.h` re-exports the same functions for existing Tinta callers.
Companion and HAL persistence headers include shared serialization explicitly,
so the Sticky/no-Tinta build does not require a Tinta include path. The algorithms,
record layouts, and CRC values are unchanged; no allocation is introduced.
The affected journal/publication/day-log/progress/storage checks pass (99 native
checks). Sticky S3 builds and its actual firmware artifact passes release-image
validation for board `sticky`, chip 9, and the 0x640000-byte OTA partition.
The final default C3 build also passes, and its actual firmware artifact passes
release-image validation for board `x4`, chip 5, and the same OTA capacity.
These build results do not replace physical BLE/Wi-Fi/update or heap acceptance.

Complete-image hashing yields after crossing each 16 KiB boundary, for both
running-image discovery and written-partition verification. It still borrows the
existing scratch buffer and adds no heap allocation. This gives other tasks and
the idle task scheduling opportunities during large-image scans; physical
watchdog/latency verification remains required. A pending installation also
honors the existing one-second Wi-Fi cleanup retry deadline before repeating
admission and teardown, avoiding repeated scans while cleanup is waiting.
On hardware, repeat discovery/install with the largest supported image, monitor
serial output for watchdog resets and heap loss, and exercise failed Wi-Fi
teardown before permitting flash writes.
