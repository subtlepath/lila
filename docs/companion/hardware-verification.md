# Companion hardware acceptance

No hardware results have been recorded. Passing host tests or linking firmware
is not evidence of passing this checklist. The current implementation provides
BLE installation binding and EPUB transfers; inventory, Wi-Fi assistance,
synchronization, other content installs, firmware installation, and Apple apps
remain pending. Run their sections when the corresponding implementation exists.
Do not mark unsupported flows passed.

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
| Wi-Fi handoff and fallback | pending implementation | pending implementation |
| Offline/CloudKit reconciliation | pending implementation | pending implementation |
| Content and firmware installation | pending implementation | pending implementation |

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

## Wi-Fi assistance (implementation pending)

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

## Distributed state and CloudKit (implementation pending)

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

## Other content and firmware (implementation pending)

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

## Apple interaction (implementation pending)

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
