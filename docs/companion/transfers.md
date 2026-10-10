# Reader transfer persistence

Publication-record interruption coverage checks all 183 truncated lengths of
Prepared and Published records, both canonical and staged (732 cases). Each
case preserves every file, performs no storage mutation or publication callback,
and exposes no completed-publication loan. All 1,916 host tests pass. This proves
safe refusal for torn records; automatic finish/rollback for those records still
requires implementation and physical power-cut acceptance.

The final default build including the BLE workspace lease passes the x4/chip-5
image validator: 6,546,208 bytes, SHA-256
`30acf6b1d67967d1ef7a3815774ccdafdd3b98ec491a481fd48e522546afb541`,
leaving 7,392 bytes in the OTA partition. The final sticky image also passes its
board/chip-9 validator: 5,789,648 bytes, SHA-256
`797b0504f888ef4e5107c6863b9673e305ac7bd485365de128a59c8683fae88d`,
with 763,952 bytes of OTA headroom. The same batch validated x4c (6,550,256 bytes,
3,344 bytes of headroom) and papermono (5,904,080 bytes, 649,520 bytes of headroom).
X4pro failed during framework package copying before compilation. These images
precede the later borrowed-source/native reviewed-journal audit changes; new
affected-target builds and physical acceptance remain pending.

BLE exposes a mutex-protected exclusive control-workspace lease for operations
that need more than the ordinary 940-byte transfer partition. Acquisition requires
the current authenticated transport token, an empty command queue, and an idle
fragment assembler. The consumer must copy its command and finish borrowed frame
views first. This lease does not authorize an Apple installation or fresh consent.

While leased, incoming authenticated writes are rejected before touching workspace
bytes and the peer is disconnected. Receive/send and new connections cannot reuse
the buffer. Disconnect revokes the permission check but leaves storage protected
until the original consumer releases it; a different or stale token cannot release
another consumer's lease. Release resets queue/assembler metadata for subsequent
frames. `HalCompanionControlWorkspaceLease` supplies scoped cleanup, including
early returns; Bluetooth and its workspace must outlive it, and it must be released
before reply encoding or radio lifecycle changes. No buffer or heap allocation is
introduced by the lease.

All 1,915 host tests pass, including queued/partial-frame preservation, disconnect
and token changes, and scratch reuse followed by fresh frame assembly. Enabled C3
compilation passes the 256-byte frame limit: ingress uses 96 bytes and lease methods
48 bytes. The disabled HAL branch also compiles with host stubs. These are software
checks; physical BLE concurrency, Wi-Fi leasing, and live baseline command
integration remain unfinished. Final affected firmware builds must include the
lease source changes before accepting this checkpoint.

`CompanionTransfer` implements a durable transaction controller with a
`HalTransferStorage` adapter. Connect & Sync provisions identity
and runs recovery before starting BLE. Failed recovery blocks ordinary navigation.
Startup also recovers before Home or reader routing. BLE transfer handlers now
use the authenticated installation identity. Initial
content installs accept `/Books/Companion/<sha256>.epub`, with destination hash
matching the content hash. Declared format-1 courses additionally use only
`/tinta/course.pack` on builds with Tinta, subject to validation and durable family
binding. Fonts now install through the same native recoverable transaction with
registry-compatible destination checks and full parser validation. Dictionary
activation and physical end-to-end validation remain pending.

`TransferRecoveryMode::InspectJournal` is a read-only bootstrap step for attaching
context-dependent installers. It parses the existing checksummed journal and
manifest, selects the current sequence, and checks the storage generation before
exposing `current()`, `contentManifest()`, and `destination()`. It does not prove
candidate integrity, validate installed metadata, resume installation, truncate
incoming data, or remove files. Begin, append, commit, and abort remain disabled
until normal recovery succeeds. Callers must establish the native reader/card
context, known persisted owner, exact consent, writer exclusion, and journal
readiness before attaching a baseline installer and invoking normal recovery.
This mode is not yet used by Startup or Connect; those production paths remain
to be wired.

The 1,908-test host run covers inspection in Receiving, Installing, and Committed
phases, mutation refusal, failed reloads, and candidate corruption requiring
subsequent normal verification. The native baseline fixture interrupts a rename,
releases the original import owner, inspects the journal without file changes,
creates a replacement owner, and completes normal recovery. A later duplicate
commit preserves newer live learner bytes. Forced C3 compilation passes the
256-byte frame limit (`recoverImpl`: 32 bytes, `recover`: 16 bytes). Physical
power-cut acceptance and boot/Connect lifecycle integration remain pending.

Dictionary commits use a separate member-publication storage hook. They never
enter the single-file rename/backup path: the installer must publish the member
set and its metadata before the controller writes a committed checkpoint.
Recovery verifies the original archive through a storage hook, allowing the
installer to retain it in its immutable cache rather than at the dictionary base
path. Deferred recovery checks that proof without advancing publication. The
default archive verifier checks the incoming stage or destination; a cache-backed
installer must override it. The native HAL override checks the incoming stage
when present and otherwise verifies the original manifest's immutable cache blob
by length and SHA-256. A bad incoming stage cannot be hidden by a valid cache copy.
It rejects a ZIP placed at the member base as a substitute for the retained cache.
The cache verifier's checked heap owner holds lookup/file handles and the cache
path outside the task stack, borrows transfer scratch, and yields during hashing.
An absent member installer cannot commit a dictionary.
After journal and storage-generation validation, recovery exposes the borrowed
destination to installation callbacks so dictionary parents can authorize their
durable plans. Transfer begin/append/commit/abort remain disabled until recovery
returns success. The scoped destination access ends on every return, including
failed publication or metadata validation. Tests exercise the production parent,
member publication, and binding coordinators inside automatic recovery, rejecting
reentrant commands and foreign storage generations, then retrying failed metadata
without moving members again.
Installation-plan recovery can read its expected proof from the durable slots
and bind it to the recovered transfer's transaction, SD generation, original
manifest, and destination. It then runs the existing two-slot revision checks;
the installation parent additionally requires the recovered extraction receipt
to match. This permits reboot recovery after members have moved, without retaining
the initial plan in RAM or reconstructing it from missing stage files. The journal
reuses its existing working plan and borrowed scratch. This structural proof does
not replace archive or installed-member verification.
`HalDictionaryArchiveSelection` composes the two-pass dictionary selector with
full ZIP payload/header validation, disk-backed range checks, and normalized-name
duplicate checks. It borrows disjoint archive/comparison scratch and decoder/name
workspace from its session owner; no archive-sized member table is allocated.
The owner must live outside the task stack because it retains lookup handles and
parser state. Successful selection removes its private indexes before exposing
member spans. Progress callbacks reach the range and duplicate audits as well as
archive reads; host tests cancel at every callback boundary and preserve caller
output on failure. This selects validated ZIP members; StarDict semantic validation
and durable installation still run in the subsequent stages.
`HalDictionaryIncomingExtraction` connects source length/SHA verification,
selection, receipt recovery, initial ownership persistence, and member extraction.
It operates only before installation, while the completed parent transfer is
Receiving or Verified. It borrows parser/hash/receipt buffers from a checked
session owner outside the task stack. Restart rechecks sealed members and discards
only receipt-owned partial stages. Failed preparation hides its selected members,
extraction proof, and parent accessors. Tests cover failed initial/first-member
receipt sync, partial-stage recovery, wrong source bytes, damaged sealed members,
and preparation feeding semantic validation, canonical cache publication,
original ZIP retention, and installation-plan construction for plain/dictzip
bundles. The concrete installer now composes this owner with publication and
retirement; the activity attaches it before recovery and advertises dictionary
transfers when the installer is present.
`HalDictionaryInstallationPreparation` consumes verified extraction, validates
StarDict semantics while constructing the canonical ZIP, publishes the canonical
cache blob, retains the hash-checked original ZIP, and builds the initial plan.
Its plan accessor remains hidden until every step succeeds and the extraction
parent still matches. The caller must complete inventory recovery and exclude
cache writers before this stage. It discards only the unpublished cache candidate;
valid retained blobs and member stages remain intact. Each owner performs one
attempt so a retry releases its handles before rebuilding disposable staging with
a fresh checked owner. Parser/hash buffers and the decoder window are borrowed;
the retained session state belongs outside the task stack. The
`HalZipNormalizedArchiveTest` host target verifies plain/dictzip preparation and
retries after destination, staged-member, and cache-sync failures.
Destination reservation first checks the prepared plan against its live transfer
and extraction parent. It can then create missing canonical root/dictionary-folder
directories through HAL, checking long names and FAT aliases before and after
creation. Occupied folders, ambiguous aliases, foreign plans, and mkdir errors
cannot publish an installation plan. Directory construction reuses the lookup's
retained path buffers and adds no heap allocation. A failure after mkdir may leave
an empty folder; retry must verify it again before persisting the plan. The
`CompanionDictionaryInstallationParentTest` host target checks new directories,
alias conflicts, mkdir failure, and foreign destinations.
Reservation retry recovers the exact saved Prepared plan before changing the
filesystem. It rechecks staged-member lengths/hashes and destination emptiness,
then returns success without rewriting matching journal slots. A missing plan
uses initial reservation; foreign/corrupt records and plans whose publication
already started require their recovery path rather than another reservation.
Staged-member checks precede directory creation so damaged members do not create
new destination folders. Host tests reopen the reservation, reject changed member
bytes and foreign destinations, and verify unchanged journal bytes on retry.
`HalDictionaryPublicationSession` recovers the durable plan before publication
or finalization. It resumes member moves under the live parent guard, skips those
moves for an already Bound plan, installs the hash-verified archive binding, and
finalizes only after the transfer is Committed. Installed verification checks the
Committed plan, member bytes, retained archives, and finalized binding. Lookup,
member verification, and wire scratch remain borrowed; retained HAL handles and
coordinator state live outside the task stack. The parent host target exercises
a rename that succeeds while reporting failure, binding-sync failure, unchanged
Bound retry, restart finalization, and corrupted cache/member rejection through
this session. The concrete installer composes it with retirement cleanup and is attached by
CompanionConnectActivity before recovery.
`HalDictionaryRetirementSession` orders proof recovery, verified repair of a
missing proof copy, member-journal cleanup, and retirement completion. Its retained
state lives outside the task stack while HAL providers, owner guard, and wire
scratch remain borrowed. It preserves a surviving proof when repair verification
or sync fails, and refuses completion while temporary staging remains. A proof
removal that succeeds but reports failure can resume from the remaining copy;
repair rechecks installed content before restoring redundancy. Missing proofs
are reported as Missing rather than being treated as evidence of completion.
The parent host target exercises damaged-member repair rejection, repair sync
failure, remaining-stage rejection, and interrupted proof removal through this
session. Native callback wiring and cleanup of other temporary stages remain pending.
Before retiring member journals, the session verifies installed content under
redundant proofs and checks the incoming ZIP against the original manifest's
length and SHA-256. It rejects unexpected backups and unknown/corrupt staging,
closes its retained HAL handle before removal, and checks absence afterward.
Wrong-sized files are rejected before hashing. Hashing borrows wire scratch and
yields through the existing HAL helper; no per-read allocation is added. A removal
that succeeds but reports failure preserves both proofs, allowing retry from an
absent incoming stage. Host tests cover changed bytes, read failure, an unexpected
backup, and removal failure after its effect without deleting journal evidence.
Host tests cover failed publication, restart, metadata phase, and committed
checkpoint writes failing before or after their effect. The concrete controller binds extraction, member publication, cache verification,
and cleanup; the activity attaches it before recovery and advertises support
when the installer is present.
`HalTransferStorage` now dispatches dictionary preparation, member installation,
metadata, and finalization through a borrowed `HalDictionaryTransferInstaller`.
Attach the owner before recovery and detach it before destruction. Dispatch checks
the canonical destination, matching manifest, nonzero owner/transaction/card
generation, and permitted durable phase; preparation and installation also require
the complete durable offset. The interface adds no allocation. Host coverage uses
a fake installer to prove callback order, preparation retry, rejection before
callbacks for mismatched context, and failure after detachment. This proves the
dispatch boundary. The concrete factory described below exercises real extraction
and installation, while activity attachment remains pending.
Extraction receipts can now be recovered without a caller-supplied seed receipt.
The journal discovers a valid slot matching the durable transaction, card
generation, and original archive hash, then checks both slots for matching member
lengths/flags and valid revision progression. It reuses its retained receipt and
borrowed wire scratch. The extraction parent rechecks the complete transfer
manifest, ownership, durable offset, and allowed phase before exposing recovered
state. Installing/Committed recovery therefore does not need to rerun incoming
selection against members that may already have moved. This restores receipt
ownership only; member/cache verification and publication remain separate steps.
`HalDictionaryRecoveredInstallation` retains the extraction and installation
journals/parents together with publication state. It restores extraction ownership
before discovering the plan, then resumes publication, finalization, or installed
verification. Transfer references must be the live durable state and manifest;
copied/stale context is rejected before journal reads. Recovery failures hide the
parent and invalidate both journals. Providers and disjoint scratch remain
borrowed; callers allocate this large owner with checked allocation outside the
task stack. The native member/binding test now uses this owner after reboot and
covers unreadable journal retry and copied-context rejection. The activity-level
installer, preparation ownership, and retirement wiring remain pending.
`HalDictionaryRetirementOwner` assembles the proof journal, cache/binding
verification, member verification, and retirement session under a guard tied to
the durable transfer. It verifies the committed installation before publishing
retirement proofs, and can recover cleanup after the member journals are gone.
Both borrowed scratch regions must be proof-sized and disjoint; invalid regions
are rejected before SD work. The owner belongs in checked session allocation
outside the task stack. Host coverage rejects damaged content before proof writes,
preserves unknown staging and failed repairs, and reconstructs the owner after
proof removal succeeds but reports failure. Missing proofs remain Missing, so
post-cleanup repeated Commit still requires independent finalized-content
verification. Activity-level construction remains pending.
`HalDictionaryFinalizedContentVerification` reads the finalized binding and
verifies both retained archive hashes. It discovers the installed members and
streams the existing canonical ZIP builder into SHA-256 rather than writing a
candidate archive; the resulting length/hash must match the bound canonical
manifest. Compressed definitions are copied as stored bytes, so verification
needs no decoder window. Its cache adapter never creates directories or mutates
files. Discovery buffers, retained handles, and hashing state live in a checked
short-lived heap owner because they exceed the task-local budget; wire scratch
remains borrowed and there is no per-chunk allocation. When no dictionary
installer is attached, native Committed metadata/finalization calls use this
verification only after every installation temporary and retirement proof is
absent. Installing still requires the concrete installer. Host tests check
changed members, damaged retained archives, incomplete/wrong-phase context,
read failures, every remaining temporary, and proof presence without changing
files. The concrete activity installer and physical acceptance remain pending.
The native transfer test uses a fake publication callback over an already-bound
fixture, removes incoming staging on commit, detaches the installer, then repeats
Commit and reopens the durable transfer through the real HAL metadata/finalization
fallback. This covers post-cleanup transfer recovery, while concrete extraction
and publication remain covered by their separate session tests.
`createHalDictionaryTransferInstaller` constructs the concrete
`HalCompanionDictionaryInstaller` with checked allocation. It borrows the live
transfer, card identity, and session scratch. Preparation allocates the 32-KiB
decoder/NFC window and extraction/preparation/reservation owners, verifies the
complete ZIP, seals members, publishes retained archives, and persists destination
reservation. Those owners are released before publication. A smaller publication
owner survives the Installing/metadata phases. Committed finalization resumes
retirement proofs first, otherwise finalizes the durable plan and publishes proofs
before removing staging and journals. Missing member journals after completed
retirement use read-only finalized-content verification. The installer fits the
activity's 940-byte transfer slice. Preparation borrows disjoint 512/384-byte
archive/comparison regions. After preparation finishes, plan serialization reuses
the first 520 bytes and verification borrows the remaining 420 bytes. Receipt
buffers live inside the checked preparation/publication/abort owners. The
retirement owner embeds its 532-byte proof buffer and borrows the full transfer
slice for cleanup, keeping those buffers disjoint without enlarging the session
allocation. A static assertion checks the minimum against the actual workspace
layout. No allocation occurs in chunk/hash loops.
The native HAL transfer test now installs plain and compressed real ZIP fixtures
through the factory, checks installed members and complete cleanup, repeats Commit,
and reopens the transfer with a fresh installer. It also recreates the installer
and recovers after a member move or retirement-proof sync succeeds but reports
failure. Factory compilation on C3 exposed Arduino's HIGH macro colliding with
the legacy ZIP name table; the table now uses CP437_HIGH_SCALARS, and the native
host include reproduces that macro context. CompanionConnectActivity attaches
the checked factory before transfer recovery and detaches/destroys it before
releasing Transfer and workspace. The real ZIP/abort tests use that exact transfer
slice and check canary bytes across queue/request/reply storage. New dictionary
Begin commands are admitted at their hash-scoped destination, and the activity
advertises the attached installer. Physical heap/stack measurement and device
acceptance remain pending, including power cuts during ZIP index creation and
cleanup. Private index recovery now has a durable owner. The full installer now fits the C3 OTA
partition with compact Unicode tables and link-time optimization. The default
image is 6,243,392 bytes against a 6,553,600-byte partition and passes board/chip,
length, checksum and SHA-trailer validation. Physical rendering/transfer timing
and task stack high-water measurements remain to be checked.
`DictionaryZipAuditJournal` defines a 302-byte, versioned CRC ownership witness
for private ZIP audit indexes. Both slots carry the complete transfer declaration
(owner, transaction, card generation, original archive length/hash and format)
and canonical destination. Publication syncs and reads back both copies before
reporting readiness. Recovery accepts one surviving matching copy for repair;
foreign valid copies, wholly corrupt evidence and I/O failures remain blocked.
Only complete Receiving/Verified parents may publish; a matching complete Aborted
parent may recover the proof for eventual cleanup. The retained codec/journal
state exceeds the task-local budget and must be embedded in a checked session
owner; it adds no per-write buffer allocation. Nine host tests cover interrupted writes,
foreign context, corruption, parent mutation and every single-byte wire mutation.
`HalDictionaryZipAudit` now checks index absence before initial publication,
consumes a matching witness before deleting indexes, and removes both slots only
after checked index absence. It checks all index types/limits before any removal,
rechecks the captured parent around progress callbacks and removal readback, and
rejects phase misuse. `HalDictionaryIncomingExtraction` hashes the entire incoming
archive before entering this coordinator, then retires the witness after both
selection passes and index cleanup. The owner is embedded in the existing checked
extraction allocation and borrows the 384-byte comparison slice. Abort uses one
short-lived checked owner borrowing the existing 420-byte work slice; it is freed
before receipt/member cleanup. Keeping the retained codecs and HAL handles on the
small task stack would exceed the 256-byte local budget.

Begin admission and completed-install verification also check the audit slots.
The native factory tests cover failed audit sync, selection cleanup reporting
failure after removal, and recreated owners with surviving indexes for both plain
and compressed ZIPs. Abort covers a surviving first audit copy with no extraction
receipt and owned indexes alongside extraction receipts. Coordinator tests check
all artifact types before deletion and exercise owner changes at 64 progress
callback boundaries. The factory cases use the live activity's 940-byte transfer slice
and preserve queue/request/reply canaries. Dictionary Begin remains gated.

Boot recovery also attaches the checked installer after provisioning card identity,
before recovering Transfer, and detaches it before destroying the owner. Its small
factory object has recovery lifetime; large decoder/publication owners remain
phase-scoped checked allocations rather than startup stack objects.

Unicode 15.1 tables retain every canonical mapping while using a shared NFD scalar
pool, 18-bit scalar bytes, four-byte decomposition/class records and eight-byte
composition records. Accessors assemble scalar bytes explicitly, without unaligned
wide loads. ELF symbols show 26,093 bytes of table data versus 52,544 previously,
a 26,451-byte flash-data reduction with no new runtime allocation. The complete
normalization fixture (more than 19,000 rows, five inputs each) and all 46 real
transfer tests pass. Firmware enables -flto for source compilation and uses
scripts/companion_lto.py to remove the framework's conflicting -fno-lto linker
flag and enable the compiler's link plugin. The C3 ELF retains the display ISR
and panic handlers in .iram0.text, and the ISR semaphore pointer in .dram0.data.
[Espressif's size guidance](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/performance/size.html)
explains LTO's stack implications: measure uxTaskGetStackHighWaterMark() during
rendering, parsing and transfers, plus peak free heap before/after activity exit.
The full host build and all 1,326 tests pass, as do 15 release-manifest and three
companion configuration tests and all 359 CompanionKit tests. Both default C3
and sticky S3 builds pass. Validated images are 6,243,392 and 5,605,376 bytes,
respectively, leaving 310,208 and 948,224 bytes of OTA headroom. Both ELFs retain
the checked display ISR/panic handlers in IRAM and the ISR semaphore in DRAM.
These are build/image checks; radio, SD interruption, peak heap and stack
acceptance still require devices.

The concrete installer now handles Aborted transfers through
`HalDictionaryAbortCleanup`. An incomplete upload with no receipts succeeds only
when private staging is absent after the parent removes its incoming ZIP. With
receipts, cleanup checks the transaction/card/archive identity, any Prepared plan,
and every remaining member before deleting anything. Sealed members require exact
length/SHA; unsealed partial members must fit their receipt length. Retirement
proofs, backups, unowned audit indexes, foreign/corrupt receipts, and plans
whose publication started are preserved and block cleanup. Receipt-owned private
member/cache candidates must be regular files. Member stages and candidates are
removed first, then Prepared plan slots, then extraction receipts; installed
members and immutable cache blobs are never removed. A retained snapshot rechecks
the exact transfer state, manifest, and destination before each mutation. The
checked heap owner keeps journal codecs, handles, and snapshot buffers outside
the task stack while borrowing wire scratch. Host tests cover incomplete upload,
failed plan sync, removal failure after its effect, damaged members, a foreign
receipt, unknown ZIP audit scratch, fresh-owner recovery, and repeated Abort.

## Storage and acknowledgements

The shared Transfer state machine borrows scratch within the session's single
8 KiB allocation. HAL phase owners use checked heap allocation as described above.
Keep state as an activity member rather than a large local
object. The scratch must remain alive and exclusively available throughout a
controller call. In BLE mode, its final 940 bytes are disjoint from the queue,
fragment assembler, request, and response buffers. Checkpoint encoding requires
308 bytes of scratch; SHA-256 verification streams through the available scratch. Startup
recovery may use the entire allocation because no radio is running. Incoming
chunk views remain in the separate request buffer until their SD write completes.

The adapter accesses SD only through `HalStorage` and `HalFile`. `HalFile::sync`
now returns the underlying SdFat status under the storage mutex, so an offset is
not acknowledged after a failed sync. Local file handles close through RAII
before a subsequent rename/remove operation. Their existing HAL implementation
allocates handle storage; actual heap fragmentation and session watermarks still
require measurement. There are no new payload or hash-buffer allocations.
SHA-256 uses a stack context and the borrowed workspace, yielding between chunks.

Files under `/.crosspoint/companion/` are:

- `incoming`: staged content, truncated to the recovered durable offset.
- `backup`: the original destination while installation is in progress.
- `transfer-a` and `transfer-b`: alternating checkpoints.

Legacy checkpoints remain 244 bytes: `LCT` and version 1 at offsets 0–3, little-endian
journal sequence at 4–11, the 99-byte `TransferState` record at 12–110, an
original-destination flag at 111, a 128-byte null-terminated target path at
112–239, and IEEE CRC-32 over bytes 0–239 at 240–243. CRC detects interrupted or
corrupt writes; it is not authentication.

Declared transfers use a 308-byte LCT version 2 checkpoint. Offsets 4–239 retain
the legacy fields. Offset 240 is the required declaration flag (1); offsets
241–303 hold the 63-byte content manifest; offsets 304–307 hold IEEE CRC-32 over
bytes 0–303. Recovery checks the size/version pair, manifest hash and length
against transfer state, and the course family/format rules. The contract survives
all later checkpoints, including commit and abort. Repeating begin must retain
the exact manifest; dropping or changing a declaration is rejected. Legacy begin
continues writing version 1 and recovery does not invent a manifest for it.

The controller stores the manifest as fixed member state and borrows its encoding
space from the existing session workspace. There are no new heap allocations.
The manifest validation helper keeps its local decode object separate from the
recovery scan frame. Host GCC `-Os -fstack-usage` reports 240 bytes for the scan
and 112 bytes for that helper; embedded stack and heap watermarks still require
measurement. The declared overload is not wired into the BLE begin handler;
course activation and its learner-state transaction remain pending.

Recovery selects the highest valid
sequence and rejects equal sequences, missing state with orphan files, and a
storage generation that differs from the supplied generation.

Begin journals intent before creating the empty stage. Each chunk is written and
synced before its offset is journaled and synced. A failed checkpoint blocks
further commands until recovery rereads the SD state; an error may occur after
an operation's effects reached storage. Clients must query the durable offset
rather than assume that a failed request had no effect.

Commit requires the full length and verified SHA-256, then journals installation
intent. It renames an existing target to the backup, renames the stage to the
target, verifies the installed file, journals committed state, and removes the
backup. Recovery completes this sequence after interruption. Repeating commit
for the retained transaction is harmless. Abort before installation journals
aborted state before deleting the stage; it preserves the original destination.

Only one transaction is retained. Retired transaction tombstones, multi-file
bundles (including dictionaries), and installation of derived learner state as a
single transaction still need implementation. SHA failures during recovery and
ambiguous filesystem states return errors and preserve recovery data; automatic
rollback for damaged media remains pending. The activity must prevent reading
from resuming while recovery returns an error.

Paths must be canonical absolute paths shorter than 128 bytes with no hidden,
empty, dot, or parent components. This excludes the private checkpoint directory.
A future authenticated handler must additionally enforce allowed content roots,
content kind/format compatibility, dependencies, and transaction ownership. The
controller checks paired installation ownership on append, commit, and abort.

## Verification

```sh
cmake -S test -B /tmp/lila-companion-tests
cmake --build /tmp/lila-companion-tests --target CompanionTransferTest
ctest --test-dir /tmp/lila-companion-tests -R CompanionTransfer --output-on-failure
```

Host tests inject errors both before and after every commit mutation, interrupted
begin/checkpoint writes, all 244 legacy and 308 declared torn-checkpoint prefixes, storage-generation
changes, authorization failures, corrupt staged data, insufficient storage,
unsafe paths, duplicate begin/commit/abort, and resumption from a durable offset.
These tests use a storage fake; they validate controller ordering and recovery,
not the physical SD filesystem or the HAL adapter's SHA implementation.

Hardware acceptance must cut power during checkpoint writes and both renames,
reconnect over each transport, compare the installed file's SHA-256, and confirm
that the original or complete new content is available before reading resumes.
Check reported offsets against the checkpoint record. Repeat sessions while
monitoring free/largest heap and task stack watermarks; require more than 50 KiB
free heap and no accumulating loss. Firmware builds and physical testing remain
required before this flow can be enabled.

## Startup gate

`HalCompanionRecovery::recoverAtStartup` checks for either journal, staging file,
or backup immediately after SD mount. With no artifacts it returns without
allocating a workspace or reserving an identity epoch. With artifacts it owns
one temporary recovery object containing the 8 KiB workspace, provisions the
storage generation, and runs the same controller recovery before activities
open. The object is released before fonts load.

Failure routes into Connect & Sync with navigation and global shortcuts blocked
until recovery succeeds. If that activity cannot be allocated, a latched loop
guard keeps Home and reading unavailable. Corrupt or foreign-generation artifacts
are preserved for diagnosis; repeated reboot does not silently discard them.

On hardware, interrupt an installation after each rename and reboot: the target
must recover to its verified contents before opening a book. Also boot with a
truncated journal and a replaced card containing copied journals; both must show
recovery failure and prevent reading. Compare free/largest heap before recovery
and after activity exit. These startup/HAL checks still require physical hardware.

## Finishing a Wi-Fi session

The encrypted HTTP transport accepts the existing version-1 Wi-Fi handoff Cancel
session command, bound to the offered transaction and session. It acknowledges
with a zero payload byte without aborting the durable transfer. The HTTP listener
drains the encrypted response before ending the message session; the activity then
releases Wi-Fi resources and restores BLE through its checked teardown path.
The Apple transfer action calls `WifiHandoffTransport.finish` after successful
completion. Lost or invalid finish acknowledgements close the Apple session;
the durable job still requires inventory reconciliation rather than retransmission
with the old cipher. The native Apple action now reconnects to the remembered
Bluetooth peripheral within a bounded retry window, authenticates a fresh session,
checks reader/card/installation identities, and refreshes inventory before
continuing queued jobs. Failure attempts Bluetooth recovery and retains pending
work; cancellation stops continuation. These native paths still require Apple SDK
builds and physical iPhone/Mac/reader testing; portable core tests do not execute
CoreBluetooth or the app model.

## BLE transfer responses

BeginTransfer, TransferChunk, TransferStatus, Commit, and Abort respond with one
TransferResult byte (values in `CompanionTransfer.h`). A successful response also
contains the current 99-byte TransferState. Failed requests expose no other
installation's state. The handler checks output capacity before storage mutation.
Unauthorized session access returns Error (11), byte 2 before invoking storage.

An I/O error triggers controller recovery using the disjoint scratch region. If
recovery fails, or an installation remains unresolved, the activity stops BLE and
blocks normal navigation until startup recovery succeeds. Transfer state and
durable offsets survive disconnect; authenticate again before querying status.
EPUB archive validation remains an Apple importer responsibility; the reader
verifies staged length and SHA-256 before recoverable installation.

## Lost chunk acknowledgements

A chunk wholly behind the durable offset is treated as a retry. The controller
reads its staged range through bounded scratch windows and compares every byte
with the request. Identical data returns success without SD writes or a new
checkpoint. Changed data, forward gaps, or a retry crossing the durable boundary
return Offset; read errors return IoError. The same rules apply after reboot.
Chunk input must be disjoint from checkpoint scratch, and aliasing is rejected
before storage mutation. Queries and repeated commits remain idempotent.

## Course binding preparation

`CompanionCourseBinding.h` defines the course-family metadata and replacement
policy described in `tinta-pack-transfer.md`. Binding metadata uses its own
stage/backup pair, with the retained typed transfer Installing journal as its
authoritative intent. Before enabling course installs, invoke replacement policy
and staged validation before renaming the active pack, then recover binding
metadata before committing the parent transfer. The helper is now connected to the controller through content validation and
metadata installation hooks. The actual HAL hooks validate course format and
identity, enforce learner-state policy and install/recover the binding before
Committed is persisted. The BLE begin handler now accepts declared courses on Tinta-enabled builds.

After integration, hardware acceptance must interrupt every binding write and
rename as well as the pack replacement. Before Tinta resumes, the pack hash,
course family and durable binding must match the parent transaction. Updating a
bound family must retain learner files; another family or unbound history must
stop replacement until the required migration is resolved.

## Declared begin payload

The declaration form starts with a ContentManifest record, followed by the
initial TransferState record (162 bytes total), a one-byte destination length,
and destination bytes. The legacy form starts with TransferState. The declared
reader decoder and Apple encoder share `DeclaredBeginTransfer.json`; live
capability advertisement and declared begin dispatch are connected. Do not send
the declaration form merely because a reader supports legacy protocol version 1.

### Encrypted handoff transfer runner

TransferRunner now exposes handoff run/abort overloads alongside its authenticated BLE entry points. They check the job against the authenticated reader/installation/card metadata and require the active handoff to match that exact transaction before invoking the same transfer loop or abort operation. Run retains the existing per-reader selection checks. The app must obtain the offer through authenticated BLE before stopping BLE and must close the handoff on ownership changes; these overloads do not negotiate keys or switch radios.

The course transfer test composes SQLite, ContentVault, TransferRunner, WifiHandoffTransport, AES-GCM and the existing reader transfer fixture. It completes a pack entirely over encrypted messages. A second path loses an encrypted reply after the reader stores a chunk, closes that handoff, reopens SQLite and resumes over the BLE transport abstraction using the reader's durable offset. Both paths deliver identical bytes exactly once, retain the same declared manifest/transaction and commit once. All 234 Swift tests pass. This is an in-memory encrypted provider, not a real HTTP socket or BLE radio test; public handoff overloads compile but still need authenticated-session/native integration coverage. Firmware endpoint, radio handoff and physical iPhone/Mac/C3/S3 recovery remain unfinished.

The live BLE activity now uses the shared `dispatchTransfer` helper for transfer handling and post-I/O-error recovery. The helper reports recovery blockage and the transition to Committed separately from the command's original result. Inventory invalidation follows that transition, including an installation completed by recovery; repeated Commit requests do not invalidate it again. Empty reply storage or unauthorized ownership does not mutate a transfer. The activity preserves its existing behavior of stopping BLE when recovery becomes blocked.

`CompanionConnectActivity::wifiTransferDispatch` supplies the same activity-level transfer/recovery/inventory path to the future HTTP listener. It verifies that the ephemeral session owner still matches the activity's authenticated installation before dispatch, uses a function pointer with borrowed context, and adds no heap allocation. The listener is not yet constructed or passed this callback by the radio owner. After a callback marks recovery blocked, the eventual Wi-Fi activity loop must stop the listener/radios and enter the existing recovery UI before another command is processed.

All 35 transfer host tests pass. New coverage injects interrupted Commit mutations before/after their effects, then recovers/retries through the shared dispatcher and checks exactly one inventory-change notification, correct installed bytes, and no mutations on repeated Commit. Persistent read failures block recovery without claiming inventory change; missing reply storage and foreign ownership cause no mutations. The shared handler passes the optimized host 256-byte frame check. Physical BLE behavior, integrated Wi-Fi dispatch, SD power cuts and target resource measurements remain pending.

The final default C3 build passes with the shared live-BLE dispatcher and Wi-Fi callback. Including the Wi-Fi header from the activity exposed Arduino Print.h's HEX macro collision with the discovery lookup table; the table is now named HEX_DIGITS, and the offer host test includes the header with HEX defined to cover that integration context. The offer and discovery tests pass. On hardware, verify BLE interrupted Commit/retry, inventory refresh after installation completed by recovery, and the recovery-blocked screen before enabling the Wi-Fi listener; successful compilation does not verify those UI/radio paths.

### Preparing a large transfer for Wi-Fi

`TransferRunner.prepareForHandoff` requires an authenticated reader session and verifies installation, reader/card bindings, protocol/content support, selection, abort intent and vault integrity through the existing transfer path. Jobs must exceed 1 MiB and cannot already be locally committing. It sends the normal Begin (including the confirmed course declaration for Tinta), validates the returned transfer state and checkpoints the reader's durable offset as paused. It sends no chunks or commit and opens no content stream for bulk transmission. The same transaction can then be negotiated for Wi-Fi or resumed over BLE.

A reader-reported committed transaction completes local reconciliation without resending bytes. A reader-reported Installing state is checkpointed as committing and refuses handoff; failure handling preserves that protection. Receiving/Verified staging rechecks cancellation, selection and abort intent after the reply. A durable offset learned after a lost BLE chunk acknowledgement is retained without retransmitting that chunk during preparation.

Four new runner tests cover a job above 1 MiB, no-chunk/no-commit staging, repeated Begin, SQLite reopen, small/unselected/commit-protected rejection before BLE, reader committed/installing reconciliation and reader-offset recovery after a lost chunk reply. All 260 CompanionKit host tests pass. These transport fixtures establish runner/persistence behavior, not SD installation or native Apple/BLE execution. The app still needs to compose preparation, negotiation, OS joining, encrypted HTTP confirmation and transfer continuation; Wi-Fi capability advertising and physical acceptance remain pending.

The iOS Devices action now composes staging, hotspot negotiation, temporary OS configuration, endpoint discovery and encrypted runner continuation when the user enables hotspot assistance. A separate handoff flag suppresses the ordinary BLE-disconnect cancellation only during that operation; explicit disconnect/pause still cancels. Unsupported/unavailable preparation falls back over the existing authenticated BLE session. The action closes HTTP/cipher and the OS lease on success/failure, and clears stale BLE authentication afterward. It currently completes one Wi-Fi job before requiring reconnection for remaining queued work. Mac/saved-network guidance is now wired through a timed manual prompt; automatic multi-job reconnect and native/physical validation remain unfinished. Only Swift source parsing and localization validation establish the app changes here; the existing 266 host tests do not prove this model lifecycle.


## Native font installation

Font declarations use zero logical identity and format 4 for bitmap fonts or
format 1 for vector fonts. Bitmap destinations must be a family directory under
/fonts or /.fonts with a <name>_<1..255>.cpfont filename; loose bitmap files are
rejected because the reader registry would ignore them. Vector destinations use
.ttf/.otf/.ttc and are accepted only on BOARD_HAS_PSRAM builds. Candidate length
and font structure are checked before replacing the installed file, and recovery
revalidates the installed font before completing transfer metadata.

Validation uses a short-lived checked heap workspace for retained HAL handles,
parser state, and the destination directory, borrowing transfer scratch. It
allocates once per validation call, outside the parser read loops, and frees on
all exits. Connect & Sync unloads SD-backed renderer fonts under its render lock
before transfers; registry rediscovery and loading the selected font occur when
the reader next needs fonts. This prevents replacement against stale open font
handles and cached parser state.

Host tests cover bitmap installation, invalid files/paths/format, interrupted
final rename and recovery, and vector capability policy on C3 and S3 configurations.
Physical acceptance must replace a currently selected font, leave Connect & Sync,
reopen reading, and verify the updated face, free/largest heap, and handle cleanup.
The Apple selected-content flow transfers bitmap and supported vector fonts over
the shared BLE/Wi-Fi runner. Bitmap filenames such as Fixture_14.cpfont map to
/fonts/Fixture/Fixture_14.cpfont; vector files map directly under /fonts. SQLite
schema 32 retains each job's destination alongside its declaration. Changing
content metadata to produce another destination rejects the existing job rather
than redirecting a resumed transfer. Tests cover database restart after a lost
chunk reply and capability rejection before traffic. Physical device acceptance
and a full Apple app build remain pending.

The Apple dictionary runner requires declared-transfer and dictionary capability bits
0 and 7 before retaining a declaration or sending Begin. It revalidates the vault ZIP
and uses `/dictionaries/<archive SHA-256>/dictionary`, independent of library titles
or original filenames. Plain and dictzip bundle tests exercise lost chunk and commit
replies across database restarts and a library rename. Firmware dictionary Begin
is admitted at the declared hash-scoped destination; physical device acceptance
remains pending. The reader inventory resolver verifies
the installed canonical members and retained original binding before reporting the
original ZIP manifest. Apple reconciliation tests then check that a selected dictionary
does not queue a duplicate install and that deselection targets that exact manifest.
Dictionary inventory matches require format 1 and a zero logical identity.

### Declared font and dictionary admission

The reader command handler now accepts bitmap-font declarations only at validated
font destinations and vector-font declarations only on PSRAM boards. Admission and
HAL installation share `CompanionFontDestination.h`, which rejects traversal,
additional path levels, invalid sizes and mismatched suffixes without allocation.
Dictionary declarations require format 1 and the exact
`/dictionaries/<declared SHA-256>/dictionary` destination used by the Apple plan.
The handler still checks the authenticated owner before starting the transaction.
Two command-handler regressions reproduce the former rejection and now pass;
45 transfer tests and the C3/S3 HAL font tests pass. The activity advertises
dictionary transfer when its concrete installer is attached. All 49 HAL installer
tests pass, including authenticated Begin/chunk/Commit at the hash-scoped
destination for plain and dictzip bundles and harmless repeated Commit.
Physical installation acceptance remains outstanding.

The changed handler also compiles with the actual C3 firmware flags and
`-Werror=frame-larger-than=256` after removing LTO for frame reporting. Compiler
stack-usage output reports 96 bytes for font destination validation, 64 bytes
for hash-scoped destination validation, 80 bytes for admission and 240 bytes
for declaration decoding. The new validators allocate no heap and borrow the
command payload as bounded views. These individual-frame checks do not measure
aggregate task stack, radio allocations, free/largest heap or repeated-session
loss; those remain physical acceptance requirements.

Dictionary Begin also uses `validDictionaryBindingManifest`, matching the native
installer and Apple plan: nonzero hash, length 22 through UINT32_MAX, format 1
and zero logical identity. Handler regressions reject short/oversized and zero-hash manifests
before any storage mutation. A fresh full host build and all 1,668 CTest cases
pass after this correction. Firmware verification must use the source after it.
