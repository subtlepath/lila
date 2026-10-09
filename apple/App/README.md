# Native app targets

Open `apple/LilaCompanion.xcodeproj` in Xcode. Shared schemes are `LilaCompanion-iOS` (iOS/iPadOS 18+) and `LilaCompanion-macOS` (macOS 15+). Both link the local CompanionKit package and compile the same SwiftUI source and English string catalog. Set your signing team and bundle identifier in Xcode before running on a device.

The app provides Devices, Library, Updates and Settings navigation with Bluetooth
pairing, installed-content inventories, resumable transfers and reader imports,
course-switch confirmation, synchronization, CloudKit controls, and firmware
staging/install actions. Library imports EPUBs, Tinta packs, supported fonts, and
StarDict bundles through CompanionKit validators. Mac supports Command-O and file
URL drops; security-scoped access remains active through each import batch.
These services are wired in source. Linux checks do not establish native Apple
compilation, UI behavior, OS permissions, or physical reader acceptance. The
implementation notes below include earlier checkpoints and their limitations.


On a Mac with Xcode, verify both targets:

```sh
xcodebuild -project apple/LilaCompanion.xcodeproj -scheme LilaCompanion-iOS -destination 'generic/platform=iOS Simulator' CODE_SIGNING_ALLOWED=NO build
xcodebuild -project apple/LilaCompanion.xcodeproj -scheme LilaCompanion-macOS -destination 'platform=macOS' CODE_SIGNING_ALLOWED=NO build
```

Linux checks passed for Swift syntax parsing, Foundation OpenStep property-list parsing, project references, schemes, deployment targets and string-catalog JSON. These do not typecheck SwiftUI or prove native builds. Neither native build command has been run on this host; native launch, accessibility and import behavior require verification on Apple platforms.

All 150 CompanionKit host tests pass. Import routing tests cover uppercase course extensions, persisted course details, bitmap-font dispatch, remote/unsupported URL rejection and mismatched file content. These prove backend routing and validation; the native picker, drag-and-drop, sandbox scope lifetime and overlap gate still require Apple UI tests.

Devices now has source wiring for on-request BLE discovery, authentication, explicit per-installation pairing, cancellation/disconnection, and connected-reader identity/battery. It uses CompanionKit's connection-bound ReaderSession and local Keychain storage. Failed authentication does not automatically register new credentials; the Pair Reader action invokes the separate pairing operation. Connection operation tokens prevent canceled tasks from restoring old state. Disconnect clears the authenticated session. The app does not yet persist a reader list or show installed content, pending changes or last successful sync.

Both targets include a Bluetooth usage description and its English InfoPlist string catalog. The Mac target uses a sandbox entitlement for Bluetooth and read-only user-selected files. Swift syntax, project property-list/reference checks, entitlement plist and catalog JSON checks passed. Native typechecking and permission/pairing behavior remain unverified. On Apple hardware, verify first-use permission, discovery in Connect & Sync, reader passkey confirmation, explicit app pairing, reconnect authentication, cancellation, disconnect and Bluetooth power-off; then verify that restarting the app reuses the Keychain installation credentials.

Schema 13 persists authenticated reader descriptors and connection timestamps by physical reader identity. A new SD generation updates that reader's descriptor rather than creating another reader. The Devices source shows saved reader identities and last connection time across restarts. This cache is not proof of a live connection and is not used as the authenticated transfer session. Last successful sync remains unset; authentication alone does not count as synchronization. Credentials are still outside SQLite in the per-installation Keychain vault.

All 152 CompanionKit host tests pass, including descriptor encoding against the shared fixture, SQLite restart/card-change updates, no fabricated sync time, and invalid time/zero-identity rejection. App syntax and catalog checks pass. Native list rendering, connect/pair persistence and physical BLE behavior remain unverified.

Devices now wires Refresh Installed Content to the authenticated InventoryCollector. It publishes only a complete inventory matching the current connection's reader and SD generation, clears results/tasks on disconnect, and displays failures separately from a valid empty inventory. A 100,000-entry app-side bound rejects oversized scans rather than silently truncating them. Library titles use a lookup built when library metadata loads. Reading inventory does not update last successful sync. Firmware catalog/activity dispatch is still incomplete, so this control has not yet obtained real inventory from a reader.

The six inventory contract tests pass; app Swift syntax, localization coverage and workflow YAML checks pass. The existing Mac CI job now includes both native xcodebuild commands after CompanionKit tests. Those jobs have not run in this session and do not yet provide native build evidence. Native/UI tests, installed content retrieval on hardware, pending-change display and full synchronization remain pending.

The native targets now merge `App/Documents.plist` into their generated Info.plists. They declare alternate, read-only document handling for EPUB, ZIP bundles, fonts, Tinta `.pack` and CrossPoint `.cpfont` files. `onOpenURL` accepts local files through the same security-scoped, immutable-copy validator used by Files and drag-and-drop; non-file URLs and imports received while busy show localized errors. This implements a document-opening entry point, not a share extension or a durable incoming import queue.

Swift frontend parsing, Foundation parsing of the Xcode project, all four target configuration references, document plist and localization JSON checks pass on Linux. On Apple hardware, build both targets and verify the merged application Info.plist, Files/Open In for valid EPUB/pack/font/ZIP files, unsupported or malformed files, repeated imports, and a second incoming file during an active import. Native SDK compilation and system document-routing behavior remain unverified.

Tinta entries now open a course identity confirmation screen. Verified, unassociated packs offer a new random course identity or an explicitly selected existing course's identity, followed by confirmation of the permanent association. Language and title never automatically bind courses. The screen uses the existing transactional `associateCourse` validation, displays persisted confirmation after reload, and leaves learner history untouched. Older packs lacking verified details require reimport before confirmation. The UI does not establish stable-item compatibility, activate a reader course, or transfer history.

Verify on Apple hardware: import two distinct courses and a revised edition, confirm separate identities for distinct courses, explicitly associate the revision with its original course, restart and inspect persistence, cancel confirmation, and retry after a failed save. Run the `CourseAssociationTests` and `CoursePackTests` package tests for store immutability, restart persistence, pack validation and manifest encoding. Swift frontend parsing and localization catalog checks pass; native typechecking and UI behavior remain pending.

Connected and saved reader entries now offer a content selection screen. Choices persist through `setReaderSelection`, independently for each physical reader, and reload from SQLite after saving or reopening. The screen lists all imported non-firmware content, distinguishes desired selections from installed inventory, and explains that choosing content does not immediately install or remove it. Local selection remains available while the reader is disconnected. Transfer/reconciliation dispatch and reader removal remain pending.

Verify on Apple hardware: choose different content for two saved readers, restart the app, inspect each selection, toggle content while disconnected, and verify a failed save reports an error without presenting a successful change. `ReaderSelectionTests` cover persistence, independent readers, deselection and retention of library content; they do not validate SwiftUI behavior. Native syntax and localization catalog checks pass on Linux.

The connected reader now shows durable pending transfer jobs and an explicit transfer action after a complete inventory refresh. The action uses `reconcileContentWork`, persists all new install jobs before transferring, and runs existing authenticated `TransferRunner` operations sequentially. Disconnect cancels the task; partial work stays in SQLite. Inventory is invalidated after execution or failure so subsequent work requires a fresh scan. Supported EPUB jobs can transfer/resume; unsupported course/font/dictionary installation, removal, stale-generation and inspection work remains visibly incomplete, with pending jobs retained. This does not mark a full content/history sync successful or update last-successful-sync timestamps. Live firmware inventory population/dispatch remains pending, so end-to-end operation is not yet available.

Verify on Apple hardware after reader inventory integration: queued jobs survive app restart, interruption resumes after fresh inventory, deselection aborts a precommit job, commit recovery completes correctly, unsupported work remains pending, and connection loss never routes a queued job to another reader or SD-card generation. Linux checks cover Swift syntax and existing runner/reconciliation/selection tests; native typechecking and live transport behavior remain unverified.

Transfer orchestration now consumes `prepareContentWork` from CompanionKit. Preparation converts installation actions into persisted transfer jobs once, retains exact job IDs for execution, and preserves abort/removal/inspection/stale-generation actions. Partial preparation can leave reusable queued jobs but sends no reader data. Tests cover all four non-firmware content kinds, repeated preparation after reopening SQLite without duplicate jobs, deselection becoming abort work, and incomplete inventory rejection before queuing. Reader-side support for those content kinds is still incomplete; prepared jobs do not prove installation support.

Saved reader details now display the persisted last-successful-sync timestamp, or explicitly show that no sync has completed. Pending transfer rows display localized queue phases and flag jobs belonging to a different SD-card generation. Connection, inventory refresh and EPUB transfer do not set last-successful-sync: the full synchronization completion path remains unfinished. Syntax and catalog checks pass; verify native layout, accessibility reading order, paused/committing jobs and card-change indicators on Apple hardware.

Active content transfers now offer Pause. The action cancels the execution task, keeps its durable job rather than creating an abort intent, and waits for runner cleanup before enabling further work. Cancellation invalidates cached inventory, requiring a fresh scan before resuming. A runner test cancels after the reader durably accepts the first 1000-byte chunk, reopens SQLite, verifies a paused job with no abort intent, then resumes the same transaction to completion. The existing committing-cancellation test remains relevant: an in-progress install is preserved for recovery, not incorrectly rewritten as paused. Native pause interaction and live BLE cancellation still require Apple/reader testing.

Unexpected Bluetooth loss now invalidates the connection operation token when an authenticated or pairing session existed, cancels any connection task, and clears its busy state before cancelling inventory/transfer work. Completion callbacks from the lost session cannot publish inventory, pending jobs or errors into a later UI operation. Native syntax checks pass; verify Bluetooth power-off and peripheral disconnect during inventory collection, transfer and pairing on Apple hardware. Transport-level connection binding remains the authority for reader identity.

Library rows now offer a confirmed Remove action through their context menu, backed by the existing transactional library deletion method. Removed items appear in a separate section with an explicit Restore action. Removal deselects all readers and preserves recoverable files/metadata; restoration does not reselect readers or erase abort intents. Reader-side deletion and CloudKit propagation remain unfinished and the confirmation states that limitation. The removed-ID query is tested across SQLite reopening and explicit restoration. Verify native context menus, confirmation cancellation, restoration, accessibility and removal during paused/committing transfers on Apple hardware.

EPUB library entries now open saved reading positions. Concurrent unequal causal heads offer an explicit confirmed choice; the app submits every observed head to the existing transactional resolution method and uses its Keychain-backed installation identity as the event origin. A stale-head conflict reports an error and reloads candidates. Empty history and failed loading are distinct states. Positions currently show section and visible-text location because excerpt/location presentation is not integrated. The screen describes saved book history and compatible-reader exchange. Firmware now captures known text anchors and replays conflict-free positions; native Apple UI and physical reader acceptance remain pending. Native syntax and localization checks pass; verify layout/accessibility, cancellation, stale heads and restart persistence on Apple hardware.

Saved reading-position details now link to bookmarks. The screen displays saved names, summaries and locations; fully deleted bookmarks remain hidden while edit/delete conflicts retain the removal candidate. Users explicitly confirm which version to keep. Resolution uses the stable bookmark identity, every observed causal head and the Keychain-backed app origin through the transactional store API. Stale resolutions fail and reload. The UI distinguishes empty history from a read failure and states that reader/cloud history exchange is still unavailable. Native syntax and localization checks pass; verify concurrent edit/delete choice, cancellation, stale heads, accessibility and restart behavior on Apple hardware.

Confirmed course details now link to Saved learning history. The view uses the persisted course association and course-scoped replay to show saved item, suspension, star, completion and daily study counts across associated editions. It distinguishes loading, read/replay failure and absent study totals, offers refresh, and explicitly identifies the data as local app history. It does not exchange reader/cloud history or install derived learner state.

On a Mac with Xcode, build both native schemes and verify this view with an empty course, saved review/control history, two editions of one course, another course with independent history, and a journal with missing causal predecessors. Check refresh and failure messages, VoiceOver labels, Dynamic Type and the Mac window layout. Linux verification covers Swift syntax, localization keys and the scoped replay/store tests; native SDK typechecking and UI behavior remain unverified.

Settings now wires explicit private CloudKit journal synchronization when `LILA_CLOUDKIT_CONTAINER` is configured in the build. It offers account checking/confirmation, manual sync, status/failure and disable, while leaving local operations independent. See `docs/companion/cloud-sync.md` for signing/container/schema setup and native acceptance checks. Syntax, localization and plist validation pass; native SDK/CloudKit/UI execution remains unverified. Content assets and metadata are not synchronized yet.

### Temporary iOS reader hotspot configuration

The iOS Debug/Release configurations now reference `App/iOS.entitlements`, containing `com.apple.developer.networking.HotspotConfiguration`. Enable **Hotspot Configuration** in the iOS target's Signing & Capabilities and ensure the selected App ID/provisioning profile grants it; setting the local entitlement file alone does not prove the signing service has granted the capability. See [Apple's entitlement documentation](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.networking.hotspotconfiguration).

`WifiHotspotJoiner.apply` accepts the authenticated negotiation result and applies its exact SSID/WPA passphrase with `joinOnce = true`. It returns a cleanup lease for the app's temporary configuration. Retain it through the encrypted transfer and call `close` on completion, failure or cancellation; destruction schedules cleanup as a fallback. Only one pending/applied join is allowed. Original offer expiry, OS errors and cancellation remove the temporary configuration, including another removal if the OS reports success late. Saved-network offers are rejected by this API because their password remains on the reader; saved-network and Mac joining need system Wi-Fi guidance.

A configuration callback (including `alreadyAssociated`) is only a candidate for connecting. It does not establish IP readiness or reader identity: discover the offered endpoint within the original deadline, then require an authenticated encrypted HTTP response. Apple explicitly distinguishes [configuration application from connectivity](https://developer.apple.com/documentation/networkextension/nehotspotconfigurationmanager/apply%28_%3Acompletionhandler%3A%29). The app still needs to wire its job preparation/negotiation/join/HTTP/runner UI sequence.

Six host tests validate callback ownership, cleanup, cancellation, expiry, invalid mode, failures and overlap through an injected driver. They do not run NetworkExtension. Native Apple SDK typechecking, signing and physical iPhone verification remain required: accept/deny the prompt, cancel while it is displayed, let the original offer expire, background/sleep the app, check late callbacks cannot retain a stale configuration, and verify authenticated HTTP before transferring. [Apple's joinOnce documentation](https://developer.apple.com/documentation/networkextension/nehotspotconfiguration/joinonce) describes its foreground-dependent lifetime. Confirm repeated completion/cancellation leaves no app-created reader configuration behind.

### iOS transfer action with hotspot assistance

The Devices transfer section now has an opt-in **Use reader hotspot for large transfers** toggle. Eligible EPUB/Tinta jobs above 1 MiB are staged over authenticated BLE, negotiated with a fresh hotspot offer, joined through the temporary iOS configuration, and continued through the encrypted HTTP transport and the existing runner. Fully staged/committing jobs finish over BLE. A reader that reports unsupported/unavailable preparation continues over BLE. BLE disconnection during the intentional handoff preserves the task and connection-operation identity instead of cancelling it; explicit disconnect/pause still cancels the operation.

The first runner Begin reply over Wi-Fi is authenticated and bound before any chunk advances. Failure closes the cipher/HTTP transport and temporary hotspot lease, retaining durable work. After a Wi-Fi job, the current source reconnects Bluetooth, authenticates a fresh session, verifies the same physical reader/card generation/app installation, and collects complete inventory before continuing the remaining queue. Failed reconnect or binding verification stops continuation and retains pending work. Mac and saved-network joining use the manual guidance described below. This is source behavior; physical radio/reconnect/multi-job acceptance remains unverified.

Swift syntax parsing and the three new English catalog entries pass validation. CompanionKit's last full host run passed 266 tests, but those tests do not exercise this SwiftUI model or NetworkExtension. Native Xcode builds and physical acceptance remain required: intentional BLE loss must preserve the running Wi-Fi task; genuine BLE loss on ordinary transfers must cancel; explicit disconnect/pause must release the lease; rejected prepare must fall back safely; old-reader jobs must remain usable; and a failed/lost HTTP reply must resume with a newly negotiated key and the durable reader offset after reconnect. Test course association/compatibility, deselection, permissions, background/sleep and queued work on a physical iPhone and C3/S3 readers before enabling automatic assistance.

### Mac and saved-network Wi-Fi guidance

The assistance controls are now shared by iOS and Mac, with **Reader hotspot** and **Saved reader network** choices. iOS hotspots use NetworkExtension; Mac hotspots and saved networks use a transient **Join Wi-Fi** section with the exact network name, an ephemeral hotspot password when applicable, Continue and Cancel. Saved-network passwords remain on the reader and are not displayed or transmitted. Use system Wi-Fi controls to join, then confirm; the app still requires discovery and an authenticated encrypted Begin response before transmitting chunks.

`WifiManualJoinRequest` retains only UI strings and the original offer deadline, not the AES key. Unrenderable saved SSIDs, backward clocks, expiry, overlapping waits and cancellation are rejected. A pending prompt expires without user interaction. Repeated confirmation cannot resume twice, and cancellation immediately after confirmation still prevents continuation. Prompt fields disappear on completion/cancellation/disconnect and are not persisted or cloud-synced. Each completed Wi-Fi content job reconnects and refreshes authenticated inventory before remaining jobs proceed. Explicit cancellation or failed identity/generation checks prevent continuation; physical multi-job acceptance remains unverified.

The manual prompt has five host lifecycle tests. Swift source parsing and the new English catalog entries pass validation; native SwiftUI/NetworkExtension/CoreBluetooth builds and physical Mac/iPhone testing remain unverified. Test system-network selection, wrong-network confirmation, deadline expiry while in system controls, user cancellation, rejected permission, delayed BLE acknowledgement, encrypted-message failure and recovery through a new authenticated connection. Confirm saved passwords never enter UI/library/cloud records, temporary hotspot text disappears after exit, and remaining jobs retain their reader/card bindings.

### Native navigation UI tests

Both shared app schemes include a platform-specific UI-test target, compiling
`apple/UITests/CompanionNavigationUITests.swift`. The test launches the actual
app, visits Devices, Library, Updates and Settings, checks that library import
is enabled after persistent-store initialization, and repeats the library check
after termination/relaunch. Additional cases open and cancel the content and
release-manifest pickers, require no error alert and keep the import controls
enabled; Mac also checks that Command-O opens import from Devices. Tests use
normal startup and stable accessibility identifiers. They do not select a file
or invoke pairing, installation or CloudKit controls. Native launch uses Apple's
[XCUIAutomation application proxy](https://developer.apple.com/documentation/xcuiautomation/xcuiapplication).

On a Mac with Xcode and an available iOS 18+ simulator, run:

```sh
xcrun simctl list devices available
# Set this to the UUID of an available iOS 18+ simulator from the output above.
export LILA_IOS_SIMULATOR_ID='<simulator UUID>'
xcodebuild -project apple/LilaCompanion.xcodeproj -scheme LilaCompanion-iOS -destination "platform=iOS Simulator,id=$LILA_IOS_SIMULATOR_ID" CODE_SIGNING_ALLOWED=NO test
xcodebuild -project apple/LilaCompanion.xcodeproj -scheme LilaCompanion-macOS -destination 'platform=macOS' test
swift test --package-path apple/CompanionKit
```

Set the development team for the Mac app and its UI-test runner as needed.
Use a development library/account: normal app startup can resume previously
enabled cloud synchronization. The test does not reset saved library data or
credentials. Simulator runs use their own application container.

Linux verification parses the Swift sources and OpenStep project, checks every
object reference, both UI-test dependencies/configurations/source phases and
scheme Test Actions, and parses the string catalogs. These checks pass, but do
not typecheck Apple SDK APIs or execute UI automation. Neither native test
command has run here. Navigation selectors, native launch/signing, Mac/iPad
sidebar behavior and persistent-store initialization still require execution on
Apple platforms. Import/share/drop, accessibility audits, Dynamic Type,
keyboard/menu behavior, pairing, CloudKit and reader transfer UI tests remain
required beyond this smoke test.

The picker cases verify actual system UI rather than synthesizing callback
results. SwiftUI documents that user cancellation dismisses the importer without
calling its completion handler; see [fileImporter cancellation](https://developer.apple.com/documentation/swiftui/view/fileimporter%28ispresented%3Aallowedcontenttypes%3Aallowsmultipleselection%3Aoncompletion%3Aoncancellation%3A%29).
Source syntax and project checks pass on Linux; system-picker hierarchy,
Command-O routing and cancellation execution remain unverified until the native
schemes run. No cancellation error workaround was added without runtime evidence.

Course-transfer rejection now distinguishes reader capability. A switch-capable
reader directs the user to refresh installed content, open the pack in Library,
and confirm a course switch; refreshing is required because the failed transfer
clears cached inventory. A reader without the complete switch capability set
instead explains that compatible firmware is required. Both retain the current
course and pending work. Native Swift syntax and English string-catalog checks
pass; these error paths remain unverified in native UI/hardware execution.

Bluetooth-only course jobs now also collect fresh authenticated inventory after
successful transfer, before admitting the next queued job. Wi-Fi jobs already
refresh during Bluetooth restoration. This makes subsequent compatibility checks
use the newly installed course's item history. Collection or cancellation errors
stop the queue while keeping durable completed/pending job state.

The portable history regression demonstrates why this is required: the original
pack is compatible with stale inventory naming itself, but is rejected as removing
history when inventory names an installed extension. All 104 related portable
Swift course/transfer/inventory/handoff/import tests pass; native app syntax and
scoped diff checks pass. Native app typechecking and a physical queue with two
successive course versions remain unverified. On hardware, install an extended
course over Bluetooth, then try an earlier version in the same queue; verify the
second job is rejected before transmission and the extended course/history remain
installed. Repeat with compatible extensions and with interrupted inventory refresh.

Commit-boundary cancellation has an additional portable regression: the reader
applies commit, then cancellation prevents delivery of its acknowledgement.
SQLite restart must retain the same job as committing at full durable length;
retry recovers completed status without sending content bytes or commit again.
All 105 selected portable transfer/handoff/inventory/course tests pass. This
covers the runner's durable recovery boundary, not native scene scheduling or
physical OS suspension. Explicit Pause still cancels the current task; it is not
silently turned into permission to start fresh transfers or flash firmware.

## Opportunistic foreground content resume

When the scene becomes active, the app starts enabled CloudKit work and reader
resume checks independently, so local work does not wait for a cloud connection. With an authenticated Bluetooth connection
and a complete matching inventory, it resumes selected non-firmware content
work for the same reader, SD generation, and Apple installation. Failed jobs are
left for inspection. Retained EPUB removal jobs also resume when the connected
reader advertises removal support and reader/card/installation match. Recovery
uses the saved request and transaction ID before refreshing inventory; it does
not create a new removal intent merely because the app entered the foreground.
It rechecks connection ownership, busy states and the exact
inventory snapshot after asynchronous store reads. The content-transfer button
and action share an availability guard, including pending selection/library
writes and a complete matching reader/card inventory. Firmware staging/installation
still uses its explicit actions. If no upload work is eligible, the app resumes a retained reader import whose
reader/card/installation and manifest match the current inventory. Missing
filename metadata is fetched through the authenticated reader metadata command
before downloading. It resumes the saved job ID after
checking the full immutable job again; cancellation/deselection cannot implicitly
create a replacement job. An import can resume even if another app has already
provided its library metadata. Disconnected readers retain their existing
reconnect/resume controls. A cancelled metadata task sends no request, leaves the
saved job and filename binding unchanged, and permits a later retry.

All 504 CompanionKit tests pass, including cancellation before metadata exchange; native
source syntax also passes. Native foreground execution still needs Xcode/UI and
physical verification: background/reactivate a paused content job; change cards
or disconnect while store reads run; keep failed and firmware jobs from automatic
execution; verify no overlapping transfer starts during library changes; and
cancel/deselect an import during foreground checks to confirm no replacement
job or new selection is created; and keep CloudKit offline while confirming
local retained jobs can resume.

## Font removal compatibility

The installed-content removal action supports fonts only when the reader explicitly
advertises font-removal capability bit 13. EPUB-only readers keep their existing
EPUB controls and receive no font-removal command. The action, reconciliation and
foreground recovery use the same capability check as the runner, retain saved
request identities, and preserve the library copy. Font confirmation explains
that removing an in-use font selects the built-in fallback. Current firmware
source advertises this capability and routes multi-path removal and startup
recovery; all five target builds/image checks pass, with physical acceptance
still pending. Syntax/catalog checks and all 504 Kit tests pass, but Apple native
build/UI and physical confirmation/recovery checks remain required.
