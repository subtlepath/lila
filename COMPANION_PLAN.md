# lila companion for iOS and macOS

## Summary

Build native SwiftUI apps for iPhone/iPad and Mac, alongside the firmware support they require.

This early alpha targets one current protocol and state format. All relevant devices can be reflashed, and existing alpha data may be reset. Backward compatibility and migration of older alpha state are out of scope. Future compatible course updates within the new format still preserve learning history.

The agreed experience:

- Pair readers over Bluetooth.
- Synchronize content, reading progress, bookmarks, Tinta learning history, and portable preferences.
- Use Wi-Fi assistance for large transfers, with Bluetooth fallback.
- Share the companion library through private iCloud.
- Connect readers through a dedicated **Connect & Sync** mode.
- Merge Tinta histories automatically; ask when reading positions conflict.

## Apple apps

- Add an Apple project with iOS/iPadOS 18+ and native macOS 15+ targets sharing UI, transport, persistence, and synchronization code.
- Provide four main destinations: **Devices**, **Library**, **Updates**, and **Settings**. Device details show connection state, battery, installed content, pending changes, and last successful sync.
- Support Files import, iOS sharing, and Mac drag-and-drop. Include EPUBs, existing Tinta course packs, supported fonts, and StarDict dictionary bundles. Extract EPUB metadata and validate archives on the Apple device using pinned [ZIPFoundation](https://github.com/weichsel/ZIPFoundation).
- Store metadata and durable transfer jobs in SQLite, with content files addressed by SHA-256. Keep interrupted work available after app restart.
- Use private CloudKit records and assets through [CKSyncEngine](https://developer.apple.com/documentation/CloudKit/CKSyncEngine-5sie5). Persist its state and handle account changes, quota errors, and conflicts explicitly. Local reader operations remain available offline.
- Pair each Apple installation separately; keep pairing credentials out of CloudKit. Provide localized string catalogs, accessibility labels, Dynamic Type, and native Mac keyboard/menu behavior.
- Treat background work as opportunistic. Paused transfers resume when execution becomes available; firmware installation requires an explicit action.

## Reader connection and transfer protocol

The existing transfer activity already releases font caches before networking and restarts after Wi-Fi use ([CrossPointWebServerActivity.cpp:64](src/activities/network/CrossPointWebServerActivity.cpp#L64)). Follow that resource lifecycle.

- Add a dedicated activity that saves and closes the current reader/Tinta activity before enabling radios. Use HAL wrappers, translated firmware strings, and existing theme components.
- Add a NimBLE peripheral behind a new HAL interface. Support one connected companion at a time, bonded authenticated pairing with a displayed code, and reader-side unpairing.
- Introduce a versioned companion protocol with device/capability discovery, paginated inventories, change exchange, resumable transfers, transaction commit/abort, Wi-Fi handoff, and firmware installation. Define shared fixtures for `DeviceDescriptor`, `ContentManifest`, `SyncEvent`, `SyncCheckpoint`, and `TransferState`.
- Keep hardware identity independent of filenames, Bluetooth addresses exposed by Apple, and SD contents. Track storage generations separately so card replacement or cloning cannot silently reuse synchronization counters.
- Carry control messages and small transfers over BLE. Offer Wi-Fi assistance for queued payloads above 1 MiB: use a saved network or a temporary password-protected reader hotspot. Guide hotspot joining through iOS APIs and the Mac's system Wi-Fi controls.
- Exchange a fresh session key over authenticated BLE before handoff. Use authenticated encryption for companion HTTP messages and chunks. Stop BLE before starting Wi-Fi; restore BLE after a failed handoff. Share transaction IDs and durable offsets across transports.
- Stream through one reusable 8 KiB session workspace allocated with `makeUniqueNoThrow`. Heap allocation avoids oversized stack frames and releases the workspace outside sync mode. Bound protocol queues and inventory pages; include NimBLE task/pool allocations in measured memory usage.
- Stage transfers on SD through `HalStorage`, verify length and SHA-256, then install through a recoverable transaction with backups. Recovery must finish or roll back before reading resumes; repeated commits must be harmless.

## Synchronization, content, and updates

**Reading and preferences**

- Identify EPUB editions by content hash and map them to device-local paths. Synchronize spine/text anchors instead of rendered page numbers; the reader already persists visible-text offsets ([EpubReaderUtils.h:15](src/activities/reader/EpubReaderUtils.h#L15)).
- Use recorded synchronization ancestry to distinguish one-sided changes from concurrent changes. Show both reading positions when concurrent; retain both until resolved.
- Merge bookmark additions and track deletions explicitly. Ask about concurrent edits to the same bookmark.
- Synchronize an explicit preference allowlist: typography, spacing, alignment, margins, hyphenation, language, dictionary selection, and Tinta learning/display preferences. Keep controls, orientation, refresh behavior, lighting, clocks, and credentials device-specific. Install required content before applying dependent preferences.
- Let users select content per reader. Import existing reader content into the companion library when selected. Device removal does not delete the library copy; global deletion is a separate explicit action.

**Tinta history**

The current journal has local review/undo records without distributed identities ([ProgressStore.h:14](lib/Tinta/src/core/srs/ProgressStore.h#L14)). Extend persistence before enabling history merging.

- Add a versioned authoritative event journal with durable origin/epoch/sequence identity, causal ancestry, course/item identity, study day, clock quality, and scheduler configuration. Undo events reference the exact review they undo.
- Journal mutations before updating derived state, with recovery for interrupted writes. Include reviews, suspension changes, lesson completion, stars, and completed readings.
- Merge and deduplicate events on the Apple apps. Reuse the portable C++ scheduling/replay code through a narrow bridge, preserving per-origin and causal ordering. Resolve concurrent ordering deterministically using study day, trustworthy time when available, and event identity.
- Recompute schedules and study totals from merged history; install consistent derived state as one recoverable transaction. Rebuild pending study sessions after reconciliation.
- Start with fresh learner state when upgrading older alpha installations. Preserve diagnostic usage logs per device rather than merging them into learner state.

**Content and firmware**

- Support transferring Tinta language-learning packs through the companion: import a pack into the library, select a reader, and queue installation over the shared resumable BLE/Wi-Fi transfer protocol. Start with the existing Spanish `course.pack`; additional languages require compatible pack content and reader support.
- Include pack language, stable course identity, pack-format version, required reader capabilities, length, and content hash in library metadata and compatibility checks. Validate the complete staged pack before a recoverable installation; interrupted or incompatible transfers must leave the installed pack and learning progress usable.
- Course updates retain stable item identities and learning history. A different course requires an explicit switch and separate learner state; do not overwrite the current course's progress. The current single-active-pack reader needs course switching and state isolation before the companion can offer that operation.
- Supported fonts/dictionaries and EPUBs use the same content-transfer foundation. Runtime UI translation packs require a separately defined format and firmware loader; transferring Tinta learning content does not by itself add runtime UI translation support.
- Add a release manifest containing asset hashes, board compatibility, protocol requirements, and pack-format compatibility.
- Default to stable lila releases and support manually imported firmware files. Download on the Apple device, stage on SD, then reuse the existing validator/flasher ([FirmwareFlasher.h:50](src/network/FirmwareFlasher.h#L50)).
- Require at least 30% reported battery before flashing. Validate chip, board, partition size, and image integrity; release radio resources before writing flash. Report success only after reconnecting and verifying the expected running build.
- Require the supported alpha firmware, protocol, and state format on every participating reader. Reflash and reset older alpha installations as needed; no legacy migration or downgrade compatibility is required.

## Validation and delivery

- **Protocol tests:** malformed frames, authorization, bounded queues, duplicate commands, reconnects, transport handoff, offset recovery, insufficient storage, and interrupted commits.
- **Sync tests:** two readers changing offline; reversed arrival order; duplicate delivery through iPhone/Mac/iCloud; inaccurate clocks; undo after concurrent reviews; clean alpha initialization; bookmark deletion; preference conflicts; and repeated sync producing no further changes.
- **Content/update tests:** renamed identical EPUBs, different editions, Tinta pack import and transfer, compatible course updates preserving progress, explicit course switches with isolated learner state, incompatible packs, interrupted pack/dictionary installation, wrong-board/corrupt firmware, and reboot verification.
- **Build checks:** Apple unit/UI tests and iOS simulator/native Mac builds; affected C++/Tinta host tests; repository formatting wrapper; final firmware builds for Xteink X3 (`default`, the combined X3/X4 image), X4 Pro (`x4pro`), and X4 Classic (`x4c`). Other reader boards are outside this project's build and acceptance scope.
- **Hardware acceptance:** test Xteink X3/C3, X4 Pro/S3, and X4 Classic/S3, plus a physical iPhone and Mac. Use at least two readers for distributed synchronization checks. Exercise BLE and Wi-Fi recovery, firmware updates, and reading resumption. Monitor free/largest heap and stack watermarks; require over 50 KiB free heap and no accumulating loss across repeated sessions.
- Implement in dependency order: transport/resource validation, safe persistence, synchronization, then complete app/cloud/update flows. Preserve existing local work.
- Deliver source, tests, signing/CloudKit setup instructions, and a hardware verification checklist. Assume one person's devices and Apple account. Store submission, publishing, and remote Git operations remain separate actions.
