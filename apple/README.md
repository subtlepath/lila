# lila Apple companion

The shared `CompanionKit` package targets iOS/iPadOS 18 and macOS 15, using Swift
6. It currently implements bounded control frames, BLE notification assembly,
validation of the five reader record envelopes, and typed discovery/transfer
records. It also provides an actor-owned SQLite library/job store with WAL,
full synchronous commits, prepared statements, and transaction identity checks.
Its XCTest suite reads the repository's existing `protocol/fixtures`
directly; there is no second copy of the wire fixtures.

ZIPFoundation is pinned exactly to 0.9.20 for the upcoming importer. Its
[package manifest](https://github.com/weichsel/ZIPFoundation/blob/0.9.20/Package.swift)
provides the ZIPFoundation product used by CompanionKit.

On a Mac with Xcode 16 or later selected:

```sh
swift test --package-path apple/CompanionKit
```

The CI Apple-core job runs this command on macOS. The shared package compiles and
its sixty-six protocol/SQLite/content-vault/archive/EPUB/course/font/dictionary/gzip/credential/transfer tests pass with an isolated Swift 6.0.3 Debian/aarch64
toolchain. Native Apple builds and Apple-only APIs remain unverified here. Native application
targets/UI, native BLE/Keychain verification, native importer UI integration, CloudKit,
synchronization, update flows, signing setup, and Apple UI tests remain pending.
Firmware transport support does not imply the Apple apps are complete.

Physical acceptance is tracked in
[`hardware-verification.md`](../docs/companion/hardware-verification.md).

The job store persists reader, storage-generation, and installation identities
with transaction UUIDs. Re-enqueueing the same transaction preserves its existing
offset; reusing it for another identity/content is rejected. Remote status may
move a local checkpoint backward during recovery. Completed and aborted jobs
are immutable. Credentials are not part of the library/job schema. Physical
content objects must be installed durably before calling `LibraryStore.put`;
the vault now provides that durable object publication. `ContentImporter.importEPUB`
validates the staged copy before publication and commits the library entry only
after the object is durable. Invalid EPUBs leave no published object or staging
file. A database failure after publication can leave an unreferenced valid object;
retrying the import reuses it. Extracted authors, identifiers, and languages are
returned with the import result and persisted as JSON arrays in SQLite schema 2.
Version-1 libraries migrate in one transaction while retaining content and
interrupted transfer jobs. Unsupported future schemas fail closed; a failed
migration rolls back its schema changes.

The pinned ZIPFoundation 0.9.20 manifest produces an upstream warning about its
unhandled privacy resource during SwiftPM testing. That warning is separate
from CompanionKit compilation and needs review when native app resources are
assembled. `Package.resolved` records the exact downloaded dependency revision.

`ContentVault` streams source bytes through 64 KiB reads, hashes the stored bytes,
and publishes objects at `objects/<first-two-hex>/<sha256>`. Files are synchronized
before publication; hard-link publication never replaces an existing object.
Existing objects are rechecked for hash and length before deduplication. A normal
error or cancellation removes its staging file. Startup and explicit recovery
reclaim regular staging files named `<UUID>.partial`. Writers hold a shared
filesystem lock throughout import; recovery requires an exclusive lock and skips
cleanup while any writer is active. Unknown files and published objects are
preserved. Verify forced-termination recovery on Apple devices by interrupting a
large import, reopening the app, and checking that its owned partial file is
removed while the existing library remains readable.
Use `verifiedObject` before exporting a stored object. Archive/content validation
belongs to the importer and is not implied by storage success.

`ArchiveValidator` streams ZIPFoundation entries without extracting to disk,
checks expanded size and CRC, and rejects traversal paths, symlinks, duplicate
normalized names, and declared-entry count mismatches. ZIP and ZIP64 end records
are checked independently because the dependency iterator can stop on malformed
entries without throwing. Defaults bound archives to 20,000 entries, 256 MiB per
entry, and 1 GiB expanded total. Successful ZIP validation alone does not establish
a valid EPUB.

`EpubInspector` checks the EPUB mimetype, follows the container's package path,
and extracts title, creators, identifiers, and languages using namespace-aware
XML. Container/package XML are capped at 1 MiB/4 MiB, nesting at 32 levels, and
node count at 20,000. DTD/entity declarations are rejected before parsing,
including unused declarations. Package checks require unique manifest IDs and
local resource paths, existing declared files, valid acyclic fallback chains,
and a nonempty spine resolving to local XHTML/SVG content. Relative paths and
percent encoding are resolved within the archive root. Remote HTTP(S) resources
are accepted in the manifest without fetching them; a spine item needs a local
readable resource or fallback. Links inside XHTML/CSS are not yet checked.
Files/share/drop UI integration and the other supported content formats remain
pending.

`ContentImporter.importCoursePack` validates the staged pack before publication
and persists a course library entry. `CoursePackInspector` follows the portable
reader's format-major, directory, required-section, record-stride, and string-heap
boundary checks, then streams the whole-file CRC with the CRC field zeroed.
It returns format version, content version, and locale. Tests consume the existing
Tinta `mini.pack` fixture directly. Item identities must be unique and nonreserved;
the sorted UID index must map every item consistently. Item kind, lesson,
prerequisite, and candidate-range bounds are checked. Mutation tests recompute
the pack CRC so structural checks are exercised independently of checksum failure.
Other semantic record references and stable item identity across course updates
still require validation before reader installation.

`ContentImporter.importBitmapFont` supports the reader's version-4 `.cpfont`
container. Validation checks unique style IDs, reader count limits, table bounds,
ordered Unicode intervals covering the glyph table, and every glyph bitmap range.
Glyph payload lengths must match dimensions and bit depth. Kerning classes and
ligature pairs must be ordered, reference covered codepoints, and use valid class
IDs. Tests cover durable import and malformed/truncated tables and bitmaps.

`ContentImporter.importVectorFont` supports TTF/OTF/TTC directory inspection and
durable import. Every collection face is checked for bounded, ordered tables,
required table presence, outline format, and streamed table checksums (with the
`head` adjustment field zeroed). Collection faces are capped at 256 and face
directories at 4096 tables. The layout follows the
[OpenType specification](https://learn.microsoft.com/en-us/typography/opentype/spec/otff).
Tests include repository Noto Serif, Noto Sans Arabic, and Ubuntu source fonts,
plus synthetic TrueType/CFF/collection directory mutations. Inner glyph/layout
table semantics and native font-engine acceptance remain unverified; directory
inspection alone does not prove a font renders successfully.

`DictionaryInfo` parses bounded UTF-8 StarDict `.ifo` metadata, including CRLF
headers, names, version, index size, word/synonym counts, and definition type.
It rejects duplicate fields, numeric overflow, 64-bit offsets, and critical
facts outside the firmware's 2 KiB scan or obscured by repeated key text.
`DictionaryIndexInspector` streams `.idx` and `.syn` files through a bounded
record parser. It checks UTF-8 headwords, the reader's 255-byte word limit and
ASCII-folded ordering, declared entry/byte counts, definition ranges, and synonym
ordinals. Tests exercise every fragment boundary and malformed records.

`DictionaryBundleInspector` accepts a ZIP containing one `.ifo` and matching
`.idx` plus `.dict` or `.dict.dz`, with an optional declared `.syn`. It validates
the archive and streams indexes. Compressed definition members use exclusively
created UUID staging files for random-access verification; vault imports keep
these under the writer lock and interrupted-import recovery. `ContentImporter.importDictionaryBundle`
publishes the validated archive and persists its dictionary name/kind. Tests cover
restart and missing files, ambiguous headers, and invalid definition/synonym
ranges, compressed imports, and inner corruption with a valid outer ZIP. Temporary
members are removed on success and error. Recoverable reader-side multi-file
installation remains pending.

`GzipInspector` streams gzip through system zlib, discarding expanded bytes in
one reusable 64 KiB buffer. It verifies the gzip trailer and rejects truncation,
trailing bytes/streams, and expansion above its configured limit (1 GiB by
default). A deterministic fixture contains `abcdef` repeated 20,000 times;
tests exercise every compressed fragment boundary and output larger than one
buffer. `DictzipInspector` additionally validates one version-1 random-access
table, the reader's 8192-chunk limit, optional header fields, compressed bounds,
flush markers, independently expanded chunk sizes, and an empty final DEFLATE
terminator. Tests change random-access metadata while preserving valid gzip
integrity. Dictionary ZIP imports use these checks before publication.

Apple targets use CryptoKit. Linux tests use Swift Crypto pinned to 3.10.0; the
resolved package file also locks its transitive revision. Linux verification
does not establish Apple security-scoped URL or filesystem behavior.

`PairingVault` persists one installation identity and separate reader secrets
before registration. Atomic insert/readback preserves credentials across retries
and concurrent callers; corrupt records fail closed. Credentials have no Codable
or library/cloud representation. Apple random generation uses `SecRandomCopyBytes`.
`KeychainCredentialStorage` stores nonsynchronizable, device-only generic passwords
and uses the data-protection keychain on macOS. Its Apple-only code still needs a
signed native build and runtime verification. Linux tests use an injected storage
and deterministic random source; they verify lifecycle behavior, not Keychain APIs.

`TransferCommands` emits canonical hash-addressed EPUB begin requests, bounded
1000-byte chunks, and transaction status/commit/abort requests. Replies are bound
to request IDs and expected transaction/installation/storage/content identities.
Malformed responses and inconsistent successful commit/abort phases are rejected.
Recovery may report a lower durable offset. Swift state encoding is checked
against the shared firmware fixture.

`TransferRunner` transfers EPUB objects through an injected `CompanionTransport`.
It verifies local object integrity, reader/storage/protocol compatibility, and
uses idempotent BeginTransfer replies to resume at the reader's durable offset.
SQLite checkpoints follow validated chunk replies; committing is persisted before
Commit, and completion requires a committed reply or reconciled committed state.
Tests cover lost chunk/commit replies, restart, wrong storage, bad acknowledgments,
and cancellation while installation is unresolved. Transport adapters must authenticate
the installation and bind exchanges to the current reader connection.

`BluetoothTransport` provides the Apple-only CoreBluetooth adapter: bounded
service discovery, one selected reader, required write/notify characteristics,
acknowledged writes capped to the reader's 244-byte characteristic, and one
outstanding command. Unique wire request IDs map back to caller IDs; reply and
final write acknowledgment must both arrive before completing an exchange.
Cancellation, timeout, and disconnect terminate pending continuations. This code
is excluded on Linux and remains unverified against an Apple SDK/runtime. Native
builds and physical pairing/fragmentation/reconnect tests are still required.
GATT-ready state does not indicate installation authentication; the app session
must discover hardware identity and present its Keychain credential before transfer.

The shared `ReaderSession` discovers the hardware identity before accessing credentials.
Automatic authentication requires an existing credential and never registers or rotates
one. Explicit pairing persists the credential before sending it, authenticates first,
and registers only after the reader explicitly rejects authentication. This recovers
a lost registration acknowledgement without replacing the original secret. Discovery
and authorization replies must match the request ID, command, and exact result shape.
The public `TransferRunner.run(_:session:)` accepts only a session returned by this
handshake and checks the job's installation identity. Reader-side authorization remains
authoritative after a disconnect; a reconnect requires a new handshake.

The Linux shared-core suite currently passes 72 tests, including six handshake tests.
Apple SDK compilation, Keychain behavior, and Bluetooth pairing on physical readers
remain unverified. The native application project/UI and the remaining synchronization,
cloud, Wi-Fi, and update work in `COMPANION_PLAN.md` are still pending.

Authenticated sessions capture a transport connection token. The transport must check
that token and enqueue each request on the same executor without allowing a reconnect
between the two operations. The Bluetooth adapter uses its existing connection token
on the main actor. The handshake binds discovery and authorization to one connection,
checks it again before returning, and subsequent session requests reject stale tokens.
Tests cover reconnects both after pairing and immediately after authorization. To verify
on hardware, reconnect the same reader and confirm the old session refuses a request;
a fresh authentication must succeed before resuming the durable transfer job.

`SyncEvent` now decodes the distributed envelope into typed identities and ancestry.
`SyncHistory.merged` deduplicates identical deliveries, rejects identity equivocation,
and orders complete journals causally with implicit per-origin sequence dependencies.
Missing ancestors, sequence gaps, and cycles prevent replay. Concurrent ready events
sort by study day, then trusted timestamp (untrusted timestamps contribute zero), then
origin/epoch/sequence. A binary heap bounds ordering work to O(n log n); event maps,
edges, and output require O(n) Apple-side memory. Partial inventory pages must be
assembled into complete history before this method is used. Bodies remain separately
addressed by their hash; this merge does not validate body contents or replay schedules.
The firmware authoritative journal, migration, exact-review undo bodies, persistent
Apple event storage, and C++ scheduler bridge still need implementation before history
synchronization can be enabled. The shared-core suite currently passes 76 tests.

SQLite schema 3 adds an immutable sync-event journal with a binary 32-byte identity
key, resource index, envelope, and SHA-256-verified body (maximum 64 KiB per event).
`LibraryStore.importEvents` commits batches with `BEGIN IMMEDIATE`, returning the
number of newly inserted events. Identical deliveries do not rewrite rows; conflicting
identities roll back the entire batch. This serializes competing handles as well as
actor-local calls. Reads validate both indexed keys against the decoded envelope and
rehash bodies. Missing causal dependencies are allowed during ingestion, but complete
history validation is required before replay. Event bodies still need typed mutation
schemas and semantic validation. Schema upgrades remain atomic and preserve existing
content and transfer jobs. The shared-core suite now passes 80 tests, including restart,
out-of-order ingestion, batch rollback, hash/bounds checks, and concurrent-handle dedup.

`CTintaScheduler` is a C bridge to the original firmware `Fsrs.cpp`, `Review.cpp`, and
`ItemState.cpp`. Its thin translation units include those repository sources rather
than copying the implementation. The private `engine/core` relative symlink provides
header lookup; `engine` is excluded from SwiftPM source discovery so unrelated UI and
storage code is not compiled. Keep the package in this repository when building it.
The C functions use caller-owned 16-byte state buffers and allocate no heap memory.
Swift `ScheduledItem` validates packed state and applies reviews through that bridge.
`SchedulerConfiguration` version one fixes the existing FSRS default weights and
learning rules, with retention in basis points and the maximum interval encoded
explicitly. Other scheduler versions or custom weights must not be replayed with this
configuration. Full event-body decoding, configuration hash binding, exact-review undo,
merged totals, and recoverable installation of derived state remain pending.

The shared suite now passes 83 tests. Three bridge tests cover firmware packed first
review states, learning/relearning, lapse counters, due-day saturation, interval limits,
and invalid inputs. The existing Tinta FSRS and progress-store host checks also pass
in both memory and SD-backed modes (four CTest checks). Native Apple SDK compilation
of the package remains unverified on this Linux host.

Typed Tinta review, exact-review undo, suspension, star, lesson completion, and reading
completion bodies are now implemented with strict version/size/identity checks. Review
bodies bind the six-byte scheduler configuration to the envelope digest. Apple replay
uses the C++ scheduler, preserves other origins' concurrent reviews when undoing one
review, and recomputes per-course/day totals. It preserves unrelated flag bits and
tracks completion identities separately. `importTintaEvents` validates before SQLite
writes; `replayTinta` validates durable bodies and complete global causal history before
rebuilding learner state. See [the event body specification](../docs/companion/tinta-events.md).
The suite now passes 88 tests, including an integrated SQLite reopen/replay test.
Firmware event emission, stable lesson/reading identity adapters, legacy migration,
recoverable derived-state installation, and pending study-session reconciliation are
still unfinished; distributed history synchronization remains disabled.

The reader's portable Tinta body codec now matches Swift via shared review/undo
fixtures. All 60 companion C++ tests and 89 Swift tests pass. Codec host compilation
also passes the 256-byte frame warning gate with exceptions/RTTI disabled. Live
firmware journaling and migration remain pending; no history capability is advertised.

The portable reader Tinta journal core now supports durable record-before-header
commit, alternating committed-count headers, interrupted-tail recovery, and idempotent
append. Its six host tests include every byte cut in record/header writes and lost
commit acknowledgement. All 66 companion C++ checks pass; Swift remains at 89 passing
tests. The [journal format and integration contract](../docs/companion/tinta-journal.md)
describe the borrowed 512-byte scratch area and mandatory HAL durability guarantees.
HAL storage integration, durable event identity reservation, live mutation ordering,
and migration are still pending, so history synchronization remains disabled.

`HalTintaJournalStorage` now supplies mutex-wrapped SD event/header storage, durable
sync/truncate operations, and SHA-256 for the portable journal. It retains one event
handle across scans and exposes `close()` for the owning activity's resource lifecycle.
The adapter passes an ESP32-C3 cross-compile with the 256-byte frame warning treated
as an error. Full firmware builds and physical durability tests are separate gates;
live Tinta mutation integration and migration remain unfinished.

The portable reader event writer now reserves a durable epoch, assigns sequences only
within that lifecycle, hashes canonical Tinta bodies/configurations, and advances its
frontier only after journal commit. Interrupted writes require recovery and a fresh
epoch before new events. All 68 companion C++ tests pass, and writer operations pass
the host 256-byte frame warning gate. Firmware builds remain underway; live mutation
and derived-state recovery integration are still pending.

Reading positions now use spine/text anchors and causal reconciliation, retaining
concurrent differing positions for user resolution rather than choosing by timestamp.
Equal anchors keep all causal heads. SQLite-backed conflict candidates survive restart.
The suite now passes 94 tests. [Reading event details](../docs/companion/reading-events.md)
cover the body schema, resolution ancestry, and future hardware verification. Reader
emission/application, conflict UI, bookmark reconciliation, and preference sync remain
pending; the five-target firmware build is still running.

Bookmark synchronization core now uses stable identities, bounded text-anchor bodies,
explicit deletion tombstones, and causal conflict retention. Concurrent edit/delete
conflicts survive SQLite reopen; later resolution acknowledges both heads. The suite
passes 98 tests. Reader bookmark migration/emission/application and conflict UI remain
pending; the ongoing five-target firmware build has not finished.

Portable preferences now have an explicit typed allowlist, causal conflict retention,
SQLite reconstruction, and font/dictionary dependency reporting. The suite passes
102 tests. [Preference event details](../docs/companion/preferences.md) define ranges,
excluded device settings, and application gates; reader application remains pending.
The final C3 firmware build with the HAL journal adapter passed (65,184 bytes static
RAM, 6,258,275 bytes flash). The adapter object was compiled after the latest journal
header change. Static build size does not prove runtime heap acceptance. Sticky,
X4 Pro, X4 Classic, and Paper Mono builds remain running in the same process.

SQLite schema 4 now atomically creates local reading/bookmark/preference events with
installation-local counters. Epoch/generation are random for a fresh database and
persist across restart; counters and events commit together. Required ancestry is
validated without blocking on unrelated incomplete remote pages. The shared suite
passes 107 tests. All five firmware targets passed their builds with the HAL journal
adapter: default, sticky, x4pro, x4c, and papermono. Native Apple builds, cloud flows,
reader synchronization integration, migration, and physical acceptance remain pending.

Reading/bookmark/preference conflict decisions now have transactional resolution APIs.
They verify the displayed frontier is current and create bounded causal joins when
more than four heads exist. Every join and counter update commits atomically; failure
rolls the whole decision back. The suite passes 111 tests, including nine-head
resolution and a simulated failure during the second join. Native conflict UI and
reader synchronization integration remain pending.

Legacy Tinta migration now has a strict read-only journal preview and overlap report.
It preserves source bytes/hash, recognizes recovered zero padding, identifies exact
undo targets, and requires confirmation even for identical histories. The suite passes
116 tests. [Migration preview details](../docs/companion/legacy-tinta.md) describe the
remaining backup, configuration, state-comparison, and reader installation work; no
migration writes or history synchronization are enabled yet.

Legacy migration now produces a typed conversion plan and verifies C++ scheduler
replay against recovered packed item states. Exact undo references and representable
flags migrate; mismatched state/configuration or leech controls fail explicitly.
Pure undo now retains fresh item state consistently with the reader. The suite passes
120 tests. Durable backups, migration identity reservation, overlap confirmation UI,
and recoverable reader installation are still pending; this remains a read-only plan.

Legacy migration originals can now be preserved as immutable vault objects with a reader/generation/course-bound manifest. The manifest is published only after all expected file hashes and lengths match; restoration verifies every original. SQLite schema 5 now registers verified manifest IDs and recovers them by reader, SD generation, and course; the migration coordinator must use that registered receipt before installing events. The Linux Swift suite passes 122 tests; native Apple builds and reader migration remain pending.

SQLite schema 6 durably binds each registered legacy backup to one migration epoch and confirmed scheduler configuration. Reservation retries recover the original identity after restart and reject changed inputs. All 122 Swift tests pass; atomic migration installation remains pending.

SQLite schema 7 atomically installs a reserved legacy migration and its completion digest. Backup/event binding, rollback on completion-write failure, restart retry, and changed-retry rejection are covered by the now 123 passing Swift tests. Reader migration and shared-history confirmation remain pending.

A strict legacy `items.bin` decoder now reads TIS1 snapshots and validates packed records through the shared C++ bridge. Damaged headers, pending recovery, truncation, duplicates, and retired slots are tested. All 126 Swift tests pass; verified-backup coordinator integration and profile decoding remain pending.

Migration planning now reads verified backup objects directly, checks SD generation and journal/header counts, and verifies replay against exported packed item state. The atomic installation regression exercises this path; all 126 Swift tests pass. Profile and completion-state conversion remain pending.

Current legacy profiles now receive strict CRC, version, and field-range validation before backup-based migration planning. The decoder exposes scheduler and learner settings while preserving local clock/navigation data without merging it. All 128 Swift tests pass; preference/completion conversion and native confirmation UI remain pending.

Validated legacy learner settings now map to nine portable preferences for explicit adoption. Batch event creation is atomic and tested for partial-write rollback, counter rollback, and restart recovery. All 129 Swift tests pass; native confirmation UI and reader application remain pending.

Legacy TMK1 reading/star sets now have a strict decoder preserving original bytes and reader insertion order. Torn records and capacity conflicts require recovery. All 131 Swift tests pass; verified course-pack key mapping and completion-event conversion remain pending.

Verified backup snapshots now include optional reading and starred mark sets, with an explicit star-file backup role. Missing files remain distinct from empty sets; malformed supplied sets block planning while original bytes remain preserved. All 131 Swift tests pass. Completion/star event conversion remains pending.

Legacy lesson completion preview now follows reader progression exactly and checks bounds against the course lesson count exposed by the validated pack inspector. Independent unlocks do not become completions. All 132 Swift tests pass; stable lesson mapping and event emission remain pending.

Validated course metadata now exposes lesson keys derived from authored unit/lesson numbers, with collision and reference checks. Legacy completion preview maps through these keys. All 132 Swift tests pass; course binding and event emission remain pending.

Course metadata now exposes reader-compatible story hashes and rejects ambiguous title keys. Legacy completion mapping reports keys missing from the inspected pack. All 132 Swift tests pass; course binding and completion-event emission remain pending.

Confirmed logical course association plus matching immutable pack SHA now enables lesson/reading completion events in migration plans. Events extend review history causally and validate completion keys against inspected content. All 133 Swift tests pass; confirmation UI, star-set conversion, and reader application remain pending.

Course-bound legacy migration now converts the preserved star set after validating item membership, recognition kind, and fresh state. Unknown or stale entries become conflicts. The integration test verifies the replayed star flag; all 133 Swift tests pass. Native confirmation and reader application remain pending.

The registered-backup migration preparation API now preflights conversion before reserving its durable identity. The complete review/undo/completion/star plan is tested through installation, SQLite restart, identical-plan reconstruction, duplicate installation, and replay. All 133 Swift tests pass; shared-history confirmation, native UI, and reader application remain pending.

SQLite schema 8 now persists per-reader desired content selections separately from library metadata. Hardware identity namespaces choices; deselection remains a durable removal request and leaves the shared library copy intact. Repeated choices suppress redundant writes, and firmware cannot enter this content-selection path. The 134 passing Swift tests cover independent readers, restart recovery, retained library metadata, duplicate choices, and invalid identities. Inventory reconciliation, reader install/removal execution, and selection UI remain pending.

Typed content manifests and complete reader/SD-generation-bound inventories now support read-only reconciliation of desired selections into install/removal proposals. Partial or mismatched inventories are rejected; unchanged content produces no action. All 134 Swift tests pass. Authenticated inventory collection and job-aware execution remain pending.

Content work planning now accounts for durable transfer jobs: resume paused work, abort deselected pre-commit work, recover ambiguous commits, and isolate changed-card or foreign-installation jobs. All 134 Swift tests pass. Live-session execution and reader inventory/removal commands remain pending.

Atomic selected-content enqueue now rechecks desired state, deduplicates pending jobs across concurrent SQLite handles, preserves paused offsets, and rejects foreign ownership or stale deselection plans. All 135 Swift tests pass; authenticated inventory execution and removal support remain pending.

Authenticated transfer execution now rechecks desired selections before streaming and between chunks, with an atomic selection/full-offset commit barrier. Already committing jobs remain recoverable after deselection. All 136 Swift tests pass, including no-request rejection and lost-commit recovery after deselection. Abort execution and reader removal remain pending.

Durable transfer-abort intent and authenticated abort recovery now survive lost replies and SQLite restart. Resumption and commit are blocked while abort is pending; reselection does not silently cancel an in-flight abort. All 137 Swift tests pass. Reader content removal and physical verification remain pending.

SQLite schema 10 adds explicit global library deletion markers, separate from reader deselection. Deletion hides active library content and atomically deselects known readers/requests pre-commit aborts while retaining recovery data. Explicit restore does not reselect readers. All 139 Swift tests pass; native UI, CloudKit propagation, reader removal, and physical reclamation remain pending.

Authenticated inventory collection now drives bounded pagination through connection-bound sessions and returns only complete consistent scans. Wrong replies, interrupted scans, changed revisions, and reconnects are tested. All 146 Swift tests pass; the reader catalog/handler remains pending.
