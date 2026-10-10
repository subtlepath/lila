# Native app targets

Open `apple/LilaCompanion.xcodeproj` in Xcode. Shared schemes are
`LilaCompanion-iOS` (iOS/iPadOS 18+) and `LilaCompanion-macOS` (macOS 15+).
Both link the local CompanionKit package and share the SwiftUI source and
localization catalogs. CompanionKit pins ZIPFoundation and includes SQLite
persistence and the portable C++ Tinta replay bridge.

## Setup

Set your development team and app bundle identifiers before running signed
builds. For iOS reader hotspots, enable Hotspot Configuration for the app and its
provisioning profile; `App/iOS.entitlements` declares the local entitlement.
Preserve the Mac sandbox, Bluetooth, user-selected file access, and outbound
networking entitlements in `App/macOS.entitlements`.

Cloud synchronization is opt-in. Configure the same private CloudKit container
for both targets using `LILA_CLOUDKIT_CONTAINER`, then configure signing,
entitlements, and the development schema as described in
[cloud-sync.md](../../docs/companion/cloud-sync.md). The account owns these
identifiers and provisioning profiles. Local library and reader operations are
available without a configured cloud container. Pairing credentials remain in
the local installation's Keychain, outside CloudKit.

Use the current alpha firmware/protocol/state format on all readers. Older alpha
installations may be reflashed and reset; no old-alpha migration is required.
Current-format compatible course updates preserve authoritative learning history.

## Current source behavior

The four destinations are Devices, Library, Updates, and Settings.

- Devices discovers and authenticates readers, offers explicit pairing,
  persists reader descriptors, and displays connection/battery, complete
  installed-content inventories, durable pending jobs, and successful sync times.
  A saved reader is not a live authenticated session. Reader operations honor
  advertised capabilities and storage-generation bindings.
- Library imports EPUBs, Tinta packs, supported fonts, and StarDict bundles through
  validators and immutable SHA-256 storage. Files/Open In, file pickers, Mac
  Command-O, and file URL drops share import routing and security-scoped access.
  Course identities require explicit association; a different course requires
  confirmation and isolated reader state. Per-reader selection/removal is
  separate from global library visibility. Interrupted jobs remain durable.
- Synchronization exchanges authoritative history and portable preferences,
  presents concurrent reading/bookmark/preference choices, and replays Tinta
  history through the shared scheduling bridge. Content dependencies precede
  dependent preferences. Course removal retains learning history and the library
  copy; compatible updates validate that retained subjects remain available.
- Large transfers can use encrypted Wi-Fi after an authenticated BLE handoff.
  iOS reader hotspots use temporary NetworkExtension configuration. Mac hotspots
  and saved reader networks use system Wi-Fi guidance. Reconnection verifies
  reader identity, card generation, and installation before continuing jobs.
- Updates handles release manifests and manually imported firmware. Installation
  requires an explicit action and reader validation; completion is verified
  against the running build after reconnecting.
- Settings exposes private CloudKit account confirmation, status, synchronization,
  and disable controls. Enabled work resumes opportunistically with app activity;
  unavailable execution/networking leaves durable work for a later session.

These statements describe source wiring. They do not establish Apple SDK
compilation, OS permissions, CloudKit execution, or physical reader acceptance.
Protocol and resource details are in [protocol.md](../../docs/companion/protocol.md)
and [hardware-verification.md](../../docs/companion/hardware-verification.md).

## Build and verification

Portable CompanionKit tests:

```sh
swift test --package-path apple/CompanionKit
```

On a Mac with Xcode, build both native apps:

```sh
xcodebuild -project apple/LilaCompanion.xcodeproj -scheme LilaCompanion-iOS -destination 'generic/platform=iOS Simulator' CODE_SIGNING_ALLOWED=NO build
xcodebuild -project apple/LilaCompanion.xcodeproj -scheme LilaCompanion-macOS -destination 'platform=macOS' CODE_SIGNING_ALLOWED=NO build
```

Run UI tests using an available iOS 18+ simulator and the native Mac:

```sh
xcrun simctl list devices available
# Set the UUID of an available iOS 18+ simulator.
export LILA_IOS_SIMULATOR_ID='<simulator UUID>'
xcodebuild -project apple/LilaCompanion.xcodeproj -scheme LilaCompanion-iOS -destination "platform=iOS Simulator,id=$LILA_IOS_SIMULATOR_ID" CODE_SIGNING_ALLOWED=NO test
xcodebuild -project apple/LilaCompanion.xcodeproj -scheme LilaCompanion-macOS -destination 'platform=macOS' test
```

Set the Mac app/UI runner development team as needed. UI tests use normal app
startup and retain library data and credentials; previously enabled cloud work
may resume. Use a development library/account. Simulator runs use their own app
container. The shared UI tests cover four-destination navigation, library startup
and relaunch, picker cancellation, and Mac Command-O. They do not select imported
files or exercise pairing, transfers, installation, or CloudKit.

The macOS CI job runs CompanionKit tests, both native builds, and both UI schemes;
it chooses an available iPhone simulator dynamically. CI configuration is not a
recorded successful native run.

Current local evidence: all 556 CompanionKit tests pass on Swift 6.0.3/Linux;
Swift source parsing and localization-catalog checks pass. Linux excludes Apple
SDK branches and cannot typecheck or execute them. Native build/UI commands and
physical iPhone/Mac/reader acceptance remain unverified. Follow the hardware
checklist for imports, accessibility/Dynamic Type, permissions, cloud account and
quota handling, two-reader synchronization, BLE/Wi-Fi recovery, firmware updates,
and repeated-session heap/stack measurements. Record results before declaring
`COMPANION_PLAN.md` complete.
