# Companion hardware acceptance

No hardware results have been recorded. Passing host tests or linking firmware
is not evidence of passing this checklist. Source exists for reader inventory,
BLE/Wi-Fi transfer, synchronization, content/update services and native Apple app
targets, but native Apple execution and physical acceptance remain unverified.
Run only capabilities advertised by the tested firmware; source presence does
not establish availability on every board. Do not mark unsupported flows passed.

The descriptor in src/activities/network/CompanionConnectActivity.cpp advertises
dictionary transfers when its concrete installer is attached and EPUB removal.
Native Apple and physical acceptance remain part of full-plan completion. Declared Begin dispatch now admits bitmap fonts through
the same destination validator used during installation, restricts vector fonts to
PSRAM boards, and admits format-1 dictionaries only at their hash-scoped destination.
Command-handler regressions cover valid requests and invalid paths without mutation;
these host checks do not establish physical transfer acceptance.
The Sticky profile excludes Tinta because it lacks layouts (platformio.ini:291), so its successful build does not validate course
installation/switching or learner-state integration. These gaps must be resolved
and verified, rather than treated as hardware passes or waived requirements.

## Equipment and evidence

Use an X4/C3 without PSRAM and a Sticky/S3, a physical iPhone/iPad running iOS 18
or later, and a Mac running macOS 15 or later. Use one person's Apple account.
Keep a spare SD card, copies of original books/course progress, a known-good
firmware image, and deliberately corrupt/wrong-board images.

Record firmware commit and board tag, app build and OS version, SD identity and
storage generation, device identity, and test start/end times. Save serial logs,
transfer hashes/offsets, screenshots of conflicts, and outcomes per board and
Apple installation. Never record pairing secrets or handoff keys in evidence.

Build the final sources once per affected firmware profile: `default`, `sticky`,
`x4pro`, `x4c`, and `papermono`. Record each result and binary hash. Also record
Apple unit/UI tests, iOS simulator build, native Mac build, affected C++/Tinta
host tests, and `./bin/clang-format-fix -g`. Keep these as software checks separate
from the physical results below.

Use `python3 scripts/debugging_monitor.py` with a debug build. Existing logs
report internal free/largest heap at BLE start and activity exit, the main-loop
heap sample, and the NimBLE authentication callback's stack watermark. The
startup gate of 128 KiB is a conservative guard, not measured BLE consumption.

| Test | C3 result/evidence | S3 result/evidence |
| --- | --- | --- |
| Pairing and independent installations | pending | pending |
| Repeated sessions and memory | pending | pending |
| BLE transfer and power-loss recovery | pending | pending |
| Wi-Fi handoff and fallback | acceptance pending; check capabilities | acceptance pending; check capabilities |
| Offline/CloudKit reconciliation | acceptance pending; check capabilities | acceptance pending; check capabilities |
| Content and firmware installation | acceptance pending; check capabilities | acceptance pending; check capabilities |

## Pairing and resource lifecycle

- [ ] Enter Connect & Sync from an open EPUB and from an active Tinta session.
      Verify their current state is saved and their file/task resources close
      before BLE starts. Return to each and verify saved state.
- [ ] Pair the iPhone with the displayed six-digit code; reject an incorrect code.
      Repeat with the Mac as a separate installation and credential identity.
- [ ] Disconnect and reconnect each Apple installation. Authentication must
      succeed only with its own stored identity/secret and bonded peer.
- [ ] Present a wrong secret, another installation's identity, and an unbonded
      client. No private inventory/state or transfer mutation may be exposed.
- [ ] Connect a second companion while the first is connected. Only one may own
      the session. An old response must never arrive at a newly connected peer,
      even when the BLE connection handle is reused.
- [ ] Forget the connected peer on the reader. Its installation credentials and
      bond disappear; the other peer's credentials and bond remain usable.
      Repeat forgetting without a connected peer and verify no unrelated deletion.
- [ ] Rotate through all supported orientations. Pairing code, status, logical
      Back/Confirm controls, and bezel-safe layout remain usable.
- [ ] Repeat at least 30 enter/pair/transfer/exit cycles, including reconnects and
      failed starts. Record free/largest heap and task stack watermarks at the
      same lifecycle points. Require internal free heap strictly above 51,200
      bytes during each session, no accumulating loss after exit, no stack
      overflow/watchdog/assert, and no leftover radio task using freed activity
      storage. Warm caches before comparing the steady-state trend.

## BLE content and recovery

Use known SHA-256 EPUBs, including a payload larger than 1 MiB. Record expected
length/hash and the transaction/owner/storage-generation identities.

- [ ] Register or authenticate the Apple installation before transfer commands.
      Begin with `/Books/Companion/<lowercase-sha256>.epub`. Reject another owner's
      transaction and a destination whose filename does not match its hash.
- [ ] Send fragmented control frames across MTU boundaries. Exercise truncated,
      oversized, unknown-version, reserved-flag, trailing-byte, and wrong-offset
      requests. Fill the bounded queue; overload must disconnect safely.
- [ ] Transfer, lose an acknowledgement, resend the same durable chunk, and
      verify success without advancing the offset twice. Change one byte in the
      retry and verify rejection. Query status after app and reader restart.
- [ ] Exhaust SD space during staging/checkpoint persistence. No uncommitted
      offset may be acknowledged. Free space and recover without losing the
      original installed file.
- [ ] Interrupt power before/after each checkpoint write and installation rename.
      Reboot and verify recovery completes before opening a book. Compare the
      installed file's full length and SHA-256 against the source.
- [ ] Repeat Commit and Abort. Repeated Commit is harmless; Abort before install
      preserves original content and removes staging according to the journal.
- [ ] Boot with a truncated journal, missing stage, unexpected backup, or copied
      journals on another card. Ambiguous recovery blocks reading and preserves
      diagnostic artifacts. Restore the original card/artifacts and retest.
- [ ] Rename an identical EPUB and import it twice: the content identity remains
      one edition. Change edition bytes: the content identity differs. Open the
      installed book and verify rendering/reading resumption.

## Removal startup recovery (physical acceptance pending)

Boot guards can be checked now. Exercise removal phase interruptions once the
reader removal command/session flow is available; it is not advertised yet.
Record hashes of the EPUB, `/.crosspoint/state.json`, `recent.json`, settings,
and retained history before and after each attempt.

- [ ] Boot a card without `/.crosspoint/companion/`. Its private directory is
      prepared and normal reading still starts. Repeat a clean boot.
- [ ] With original files safely retained, introduce a truncated
      `/.crosspoint/companion/removal-a`. Boot must display the translated
      recovery message before loading mutable stores, remain blocked, and leave
      the journal, settings, recent list, book, and history unchanged. Restore
      the original evidence and reboot to verify normal store loading resumes.
- [ ] Interrupt each Prepared/Quarantined/Committed/Retired transition, metadata
      publication, and backup deletion. Reboot without opening an app: verify
      the persisted plan finishes removal, live references are cleared, unrelated
      recent entries and history remain, and the completion receipt survives.
- [ ] Interrupt completion-receipt write/sync/rename and either journal-slot
      deletion. Reboot repeatedly: an independent terminal receipt must allow
      cleanup even when only an older Committed slot survives. Preserve a
      foreign or corrupt surviving slot and block reading instead of deleting it.
- [ ] Copy a pending journal to a different card, or corrupt/remove its active
      persisted plan. Verify failed recovery preserves all evidence and does not
      load or save default settings over the existing user settings.
- [ ] After successful removal, open another book and change its progress. Reboot
      and repeat the old removal request after removing a second EPUB. The old
      completion must not change the new reading state or the next transaction.
- [ ] Measure internal free/largest heap and stack watermark during boot recovery,
      including JSON snapshot preparation. Repeat interruptions and successful
      boots on C3 and S3; verify the plan's resource acceptance limits and attach
      serial measurements. Firmware size and host allocation tests do not prove
      these physical limits.

## Wi-Fi assistance (native/physical acceptance pending)

- [ ] Queue more than 1 MiB, exercise both saved-network and password-protected
      hotspot assistance, including iOS joining and Mac system Wi-Fi guidance.
- [ ] Exchange a fresh key over authenticated BLE, stop BLE before Wi-Fi starts,
      and retain the same transaction ID and durable offset across transports.
- [ ] Reject altered encrypted messages, invalid authentication tags, replayed
      messages, and an unrelated LAN client. Never expose plaintext credentials
      or content through companion HTTP.
- [ ] Fail network join, lose Wi-Fi mid-chunk, and suspend the Apple app. Recover
      BLE and continue from the durable offset without duplicate installation.
- [ ] Repeat handoffs in the memory-cycle test and verify the heap threshold.

## Distributed state and CloudKit (native/physical acceptance pending)

- [ ] Change the same book position on both readers offline. One-sided changes
      propagate; concurrent positions retain both anchors until user resolution.
- [ ] Add/delete bookmarks and edit the same bookmark concurrently. Explicit
      deletions propagate; conflicts retain the relevant versions.
- [ ] Apply allowed typography/dictionary/Tinta preferences after installing their
      dependent content. Controls, orientation, refresh, lighting, clocks, and
      credentials remain device-specific.
- [ ] Review the same course offline on both readers, including suspension,
      lesson completion, stars, completed readings, and undo of an exact review.
      Merge reversed arrival orders and duplicates through iPhone, Mac, and
      iCloud. Verify deterministic schedules, totals, and rebuilt sessions with
      inaccurate clocks as well as trustworthy time.
- [ ] Save a pending Tinta queue before its first local review, then reconcile a
      changed authoritative snapshot whose local reviews.log is also empty.
      Reopen and confirm the queue rebuilds from current schedules. An unchanged
      verified snapshot must resume normally. Repeat with date confirmation
      pending and power-cycle twice before confirming the date; the stale queue
      must still rebuild. Check session.bin version 3 carries the manifest hash,
      legacy metadata triggers rebuilding under canonical authority, and a torn
      session file cannot restore unchecked state.
- [ ] Migrate confirmed shared legacy history without double-counting. Preserve
      backups and present ambiguous overlap as an import conflict. Diagnostic
      usage logs remain per-device.
- [ ] Query active journal state over BLE and encrypted Wi-Fi with backups in
      different journal formats. Compare reported count/frontier with a fresh
      complete export and check the record size against the active header.
      Append a local event between connections, repeat, and verify the new
      count/frontier. Reject foreign-card and interrupted-SD requests before
      queueing migration; monitor heap/stack use across repeated queries.
- [ ] Before first Tinta migration, edit a native learner file after backup review
      and verify admission refuses it without publishing a baseline. Restore the
      reviewed files, then interrupt the first canonical installation at each
      file rename and receipt write. Reboot into recovery before opening Tinta;
      verify one consistent revision-1 snapshot, correct frontier/study day, and
      unchanged immutable backup hashes. Repeat with preference-only history,
      then repeat reconciliation and check for no further state changes.
- [ ] Terminate the Apple app after reader admission, during event append, and
      after commit but before its reply. Reopen the same library and resume the
      saved migration job over BLE and encrypted Wi-Fi. Confirm the reader's
      durable count selects the remaining events, started candidates are not
      re-admitted, commit recovery publishes one baseline, and completed retries
      send no further commands. Confirm reviewed backups remain accessible.
- [ ] Repeat an unchanged sync: no additional events, transfers, or state writes.
- [ ] Test offline operation, account sign-out/change, CloudKit conflicts and
      quota errors, app termination, and later resumption. Pairing credentials
      never appear in private CloudKit records or assets.
- [ ] Remove a reader without deleting library content. Verify a global library
      deletion requires its separate explicit action.

## Other content and firmware (native/physical acceptance pending)

- [ ] Import the Spanish course pack, supported fonts, and StarDict bundles.
      Reject incompatible/corrupt packs and invalid archives. Preserve stable
      course item identities across updates.
- [ ] Interrupt multi-file dictionary/derived-state installation after every
      mutation; recovery yields one consistent version, not a mixture.
- [ ] Test stable-release and manually imported firmware, wrong chip/board,
      oversized partition, corrupt integrity data, insufficient storage, and
      incompatible migrated-state downgrade. None may reach flashing.
- [ ] Report less than 30% battery and refuse flashing. At 30% or more, require
      an explicit installation action and release radios before flash writes.
- [ ] Reconnect after reboot and verify the expected running build before
      reporting success. A disconnect alone is not success. Interrupted flashing
      must preserve an available recovery/previous boot path.
- [ ] Connect older firmware: show existing OTA/SD initial-upgrade instructions.

## Apple interaction (native/physical acceptance pending)

- [ ] Open Settings → Portable preferences with concurrent reader changes.
      Verify both values are shown, cancel preserves both, and confirming a
      choice creates a resolution that survives app restart. Receive a newer
      change while the confirmation is open: the stale choice must fail and
      refresh without discarding history. Check VoiceOver and Dynamic Type.
- [ ] Verify Files import, iOS sharing, and Mac drag-and-drop with valid and
      invalid archives. Interrupted durable jobs survive app restart.
- [ ] Check Devices, Library, Updates, Settings, device details, pending changes,
      and last successful sync on iPhone/iPad and native Mac.
- [ ] Check localized strings, VoiceOver labels, Dynamic Type, keyboard shortcuts,
      menus, and Mac window behavior. Background suspension pauses work safely;
      firmware installation never starts as an automatic background side effect.

Acceptance requires completed evidence for every relevant row and checkbox.
Signing, CloudKit provisioning, distribution, and remote Git operations are
separate from recording physical test results.

## Existing reader content import

Native reader-only controls, authenticated filename metadata, BLE reads, and
reverse Wi-Fi admission/dispatch are wired. Record the complete user flow on C3
and S3; command tests and source parsing do not satisfy this acceptance section.

- [ ] Select an EPUB already on a reader but absent from the companion library;
  verify parsed metadata, exact SHA/length, source-reader selection and retained
  per-book reading history. Other readers must remain unselected.
- [ ] Import an existing Tinta pack and verify its stable logical identity,
  locale/edition and existing learner history remain intact.
- [ ] Import bitmap/vector/collection fonts on supported boards and verify their
  authenticated original names/families survive library/cloud/transfer use.
- [ ] Import both plain and dictzip dictionaries; compare the retained original
  ZIP bytes and validate members, index/synonyms and compressed definitions.
- [ ] Interrupt a middle chunk, lose a reply, terminate/relaunch the Apple app,
  reconnect and verify the acknowledged offset resumes without duplicate bytes.
- [ ] Lose the last reply and interrupt verification/publication; retry must
  preserve one immutable object and one completed job without reselecting an
  item that was subsequently deselected.
- [ ] Delete library content locally or through cloud visibility while a read or
  verification is in flight; it must not publish/reselect after cancellation.
- [ ] Change/remove the card, alter source bytes, disconnect/unpair, force low or
  fragmented heap, and attempt concurrent transfer/removal/journal mutations.
  Record explicit refusal, safe handle cleanup, retry and unchanged learner data.
- [ ] Verify large-file Wi-Fi assistance, lease expiry, cancellation, reconnect, and
  BLE fallback. Host encrypted transport tests do not prove physical handoff.
- [ ] Monitor existing serial heap logs before/after radio start, every failure
  and repeated sessions; record free/largest blocks and stack watermarks. Static
  link RAM and compiler frames are not substitutes for the >50 KiB heap gate.

- [ ] Complete a Wi-Fi reader import and verify automatic Bluetooth reconnect,
  installation authentication, and refreshed inventory for the same reader and
  card generation. The imported object and reader selection must remain present.
- [ ] Lose the Wi-Fi finish response after library publication. Verify the job
  remains Completed and the object is retained; a failed reconnect must report
  successful import with a reconnect instruction rather than a paused download.
- [ ] Replace the SD card or reader before post-handoff authentication. Reject
  the changed identity/generation and do not publish its inventory as the old
  reader's state. The completed companion object must remain usable.
- [ ] Disconnect explicitly while post-handoff reconnect is running. Verify the
  cancelled connection operation cannot later replace the current device state.

- [ ] Delete the selected local font with settings-save failure injected. Verify
  neither font root is removed until fallback persistence succeeds, including a
  retry after the in-memory selection was cleared.
- [ ] Fail deletion before and after effect in each root. Verify remaining files
  are retained on a failed save and caches/registry reflect any partial deletion.
  Check that a different selected family and unrelated directories survive.

- [ ] Fail local font deletion, then retry with Confirm and touch separately.
  Both must repeat deletion of the same family, with no download request. Back
  must clear the deletion retry target. Check translated deletion errors and
  registry refresh after partial failure in both local and web interfaces.

- [ ] With CloudKit offline or stalled, reactivate a connected app with retained
  local transfer/import jobs and a complete matching inventory. Local work must
  resume independently of cloud completion. Deliver a global deletion while an
  import is active and verify it cannot publish or reselect the deleted content.

## Current portable software evidence

The full CompanionKit Linux suite passes 504 tests. The complete configured CTest
suite passes 1,706 entries. These cover the selected portable implementations,
including inventory, persistence, migration, synchronization, transfers, removal,
export codecs and shared fixtures, source verification, encrypted imports,
restart/fallback, in-flight deletion, immutable filenames, cancellation, and
bounded terminal staging cleanup. Native-only Apple SDK branches, SwiftUI
interaction, CloudKit execution, and physical devices are outside these checks.

Reproduce the Swift checks with `swift test --package-path apple/CompanionKit`.
Configure/build `test/companion` with CMake and run its CTest suite; the verification
run uses `/tmp/lila-companion-tests`. Native UI targets and Xcode commands are
listed in `apple/App/README.md`. Swift source syntax parsing and the localization
catalog check pass on Linux, but native compilation and UI execution remain
unverified.

`FontInstallerFaultCheck` is included in the passing CTest suite. It can also be
run directly with `python3 test/companion/font_installer_fault_check.py`.
It compiles the native installer against fake settings/storage to exercise failed
saves and partial two-root deletion. It does not execute native settings
serialization or physical SD failure behavior.

### Current firmware images

The routed font cohort implementation passes all five firmware builds and image
validation. These images include capability bit 13, complete-path admission,
checked settings publication, single-file/cohort startup recovery and registry
refresh, alongside the earlier reader export and font-deletion UI changes.
Board/chip identity, segment bounds, checksum, SHA trailer and OTA-size validation
pass through `scripts/build_companion_release_manifest.py`. Saved proof records
and images are under `/tmp/lila-font-cohort-images`.

The dictionary-removal plan codec is host-tested but is not referenced by firmware
yet; these images do not enable dictionary removal. Earlier stopped build batches
are checkpoints only; this table contains the final routed-font results.

| Profile | Static link RAM (bytes) | Image (bytes) | OTA headroom (bytes) | SHA-256 |
| --- | ---: | ---: | ---: | --- |
| default | 64984 | 6439040 | 114560 | `8485f123ae87bbb9e838dde7ad9bbc14bbc98d453382b04d1b9c512d662ad7d6` |
| sticky | 75220 | 5749296 | 804304 | `27720bf7a082cad83699180ca48312d4ba2d6553053e33881e28251dc36dc793` |
| x4pro | 108900 | 6478672 | 74928 | `6a48e20086082fa6638d6d9cde98b64b56f61a5afdd3e70b3f9c067955402889` |
| x4c | 108732 | 6449712 | 103888 | `d9bd4eb270377aabc4064c366daeadbe82e5ac23d18ac7675e51364ef5d6bc77` |
| papermono | 125076 | 5863696 | 689904 | `f42903c5fbdb3167e6e3c84a2a891a767ef8686410d43fbb0ba704e4460fa8d1` |

Static link RAM is not runtime free heap. Firmware-image acceptance does not prove
radio operation, learner-history preservation, SD power-loss recovery, or the
required repeated-session memory acceptance. Sticky excludes Tinta, so its build
is not evidence of course installation or learner-state behavior.

### Compiler frame checks

The font cohort/reference probe passes with C3 project flags, LTO disabled and
frames above 256 bytes promoted to errors. It reports 192 bytes for font reference
path selection, 64 bytes for selected-family comparison and cohort walking,
32 bytes for publication, and 32/16 bytes for the plan/cohort probe entry points.
The probe uses the matching cached 5.5.5 SDK headers because PlatformIO temporarily
replaces the shared package while switching profiles; Arduino headers are 3.3.11.
Evidence is `/tmp/lila-font-cohort-frame-probe.log` and
`/tmp/lila-font-cohort-frame-probe.su`. These are individual compiler frames,
not whole-call stack usage or physical task watermarks.

The updated native `FontInstaller.cpp` passes an ESP32-C3 compile using actual
project flags with LTO disabled and frames above 256 bytes promoted to errors.
`deleteFamily` has a 224-byte static frame. The probe log and stack report are
`/tmp/lila-font-delete-frame.log` and `/tmp/lila-font-delete-frame.su`. This is an
individual compiler frame, not a task watermark or whole-call-chain bound.

A focused ESP32-C3 probe uses actual project flags, disables LTO for reporting,
and promotes frames larger than 256 bytes to errors. It passes with 256 bytes for
source admission, 240 bytes for the Wi-Fi read wrapper, and 80 bytes for the shared
session reply. Earlier codec/source probes report a largest codec frame of 160
bytes and source attach/read frames of 96/32 bytes. These are individual generated
frames, not whole-call stack usage or task high-water marks.

The retained-owner test closes a source handle while preserving the admitted
binding, reopens and hashes it for Wi-Fi, rejects changed bytes, and refuses reads
after normal cleanup clears the binding. Other host fault tests cover low or
fragmented heap refusal before source/mutation admission, failed close propagation,
malformed wire bindings, and retry. Measure free/largest heap, task watermarks,
and repeated radio transitions on actual C3/S3 hardware; no physical results have
been recorded.

### Font removal acceptance

- [ ] On C3 and S3, remove an in-use bitmap font using the Apple confirmation;
      verify built-in fallback persists across reboot and the library copy remains.
- [ ] On a vector-capable S3, repeat for vector fonts and loose-root font files.
- [ ] Install identical font bytes in both font roots, remove their content ID,
      and verify both copies leave inventory while unrelated styles remain.
- [ ] Interrupt quarantine, settings save, completion publication and reply;
      verify boot recovery/retry retains the same transaction and preserves other
      preferences. A failed settings load must block recovery without overwriting
      the saved settings file.
- [ ] Check native VoiceOver/keyboard/touch confirmation and ensure EPUB-only
      firmware receives no font-removal request.
- [ ] Record free/largest internal heap, stack watermarks and repeated-session
      memory during font removal, including refused low/fragmented-heap admission.
