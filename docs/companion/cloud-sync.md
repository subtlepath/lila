# Private cloud synchronization

`CloudJournalSync` implements the private CloudKit adapter using CKSyncEngine.
Settings creates it only when `LilaCloudKitContainer` contains a configured
`iCloud.` identifier, then requires account confirmation before enabling sync.
Local library and reader operations remain independent of cloud availability.
Pairing credentials stay exclusively in the installation's Keychain.

## Signing and schema setup

Choose your Apple development team and enable iCloud with CloudKit for both app
targets in Xcode. Select the same private container owned by that team and set
`LILA_CLOUDKIT_CONTAINER` to its exact identifier in both targets/configurations.
The generated Info.plist expands that setting into `LilaCloudKitContainer`.
Xcode must configure the account-owned container/service entitlements and
provisioning profiles. Preserve the Mac sandbox, Bluetooth, selected-file access,
and outbound networking entitlements. Empty container settings leave cloud
controls unavailable without preventing local work.

Use the container's development schema and private custom zone `LilaJournalV1`:

| Record type | Fields | Purpose |
| --- | --- | --- |
| `LilaJournalV1` | `envelope` Bytes, `body` Bytes | Immutable event identity and body |
| `LilaContentV1` | `descriptor` Bytes, `asset` Asset | Hash-addressed validated content |
| `LilaCourseV1` | `association` Bytes | Explicit content-to-course association |
| `LilaVisibilityV1` | `change` Bytes | Global library visibility change |

Do not promote a production schema until native integration checks pass.
Firmware remains in the separate update path. Filenames are display aliases;
content identity is its SHA-256 hash. Device-local paths and pairing credentials
are not cloud library metadata.

## Persistence and exchange

`CloudSyncStateStore` atomically persists account-bound engine serialization,
enablement, and retry deadlines. Serialization is bounded to 8 MiB and its JSON
file to 12 MiB. State-update delegate events persist serialization. Account
changes/sign-out stop the current engine and require confirmation; retained
library data is not automatically uploaded into another account. Persistence
failure stops cloud work while leaving local operations available.

Outgoing scans use bounded immutable pages and durable account-specific receipts
for events, content, associations, and visibility changes. Restarted scans can
find late-arriving entries behind an earlier cursor; deduplication and receipts
prevent repeated publication of acknowledged immutable data. Fetching precedes
outgoing publication. The delegate provides record batches without recursively
starting fetch/send operations from event handling.

Incoming assets are copied through the immutable vault and verified for declared
hash, length, and content format before publication. Metadata is derived from
validated bytes. Course association is explicit rather than inferred from title
or language. Incoming events validate hashes, typed bodies, course families, and
causal history. Conflicting immutable payloads are refused rather than silently
replaced. Global visibility changes have durable identities and conflict handling;
reader-specific content removal is a separate operation. Unexpected CloudKit
record deletion stops the adapter instead of erasing local causal history.

The adapter reports quota, account, network/service, conflict, and local
persistence failures. Transient retry deadlines persist across restart and honor
server delay guidance without shortening an existing deadline. Explicit account
confirmation and manual synchronization are available in Settings. Previously
enabled work resumes opportunistically during app startup/scene activation;
execution and connectivity are not guaranteed in the background.

## Verification

Run `swift test --package-path apple/CompanionKit`. The 556-test portable suite
passes on Swift 6.0.3/Linux, including cloud state/account isolation, retry,
receipts, asset validation, association, visibility, and restart cases. Linux
excludes CloudKit SDK code, so those results do not typecheck the delegate or
prove live account behavior. Native builds and UI commands are in
[apple/App/README.md](../../apple/App/README.md).

On signed iOS and Mac development installations using the same private account:

- Configure the development schema and confirm the account on each installation.
  Import content, associate a course, create bookmark/history events, synchronize
  both ways, and repeat without new changes. Check assets and scoped history.
- Exercise immutable-record conflicts, concurrent visibility changes,
  interrupted asset imports/state writes, quota exhaustion, unavailable network,
  persisted retry deadlines, and restart. Confirm errors preserve local data.
- Change/sign out of the cloud account. Confirm explicit reauthorization before
  using another account, separation of acknowledgements/checkpoints, and working
  offline reader operations. Inspect persisted state across restart.
- Verify opportunistic resume and cancellation in real app lifecycle transitions.
  Confirm pairing credentials never appear in CloudKit records or assets.

These native/signing/account checks remain unverified. Record them in
[hardware-verification.md](hardware-verification.md) before declaring full-plan
completion.
