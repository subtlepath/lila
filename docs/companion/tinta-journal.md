# Portable reader Tinta journal

`CompanionTintaJournal.h` implements an append-only distributed-event journal core.
The activity binds live mutations after canonical startup recovery. Legacy migration
and physical recovery acceptance remain incomplete; keep the history capability disabled.

`ProgressStore::setMutationJournal` provides the live mutation boundary for reviews,
undo and item flags. Its borrowed context/function-pointer callback receives the
local entry, exact review response milliseconds, and before/after item state after planning, before any local journal or
derived-state write. Failure marks the progress store failed without local writes;
reopening is required before another mutation. Replay/rebuild paths do not invoke
the callback. The callback adds no heap allocation. Canonical activity sessions
retain the writer and validated catalog, reserve an epoch, recover exact undo
authority, and bind this callback after startup recovery. An event committed before a
later local write fails must be recovered from authoritative history, not discarded
or reissued with a new identity.

`CompanionTintaProgressMutation.h` maps the callback's entry and before/after state
to caller-owned distributed bodies. Reviews retain exact milliseconds and explicit
scheduler configuration; undo requires the referenced distributed review identity.
Suspension and star changes map separately, in that order. Unsupported flag changes,
invalid bindings and invalid body fields leave output unchanged. The mapper performs
no persistence and allocates no heap memory. Its two-body output requires recoverable
batch persistence before binding it to live mutations; app-level stars, completions,
clock quality, identity reservation and replay integration remain pending.

`LegacyTintaJournalDecoder` validates original 12-byte review records one at a
time, with a 16 MiB bound including zero-tail bytes. Its fixed counters and last
review UID/index support undo validation without retaining the whole history.
It rejects invalid UIDs, grades, formats, control codes, flags, nonzero partial
records and undo without an immediately available matching review. Invalid
records preserve output and do not consume decoder state; after a zero tail,
nonzero records are rejected. It preserves original local timestamps and
quarter-second response fields rather than labeling them trusted or claiming
millisecond precision. No buffers or heap allocations are introduced.
`LegacyTintaJournal-v1.fixture` is read by both Apple and native tests. Native
tests also exercise malformed records and the complete 16 MiB input boundary.
This decoder is a migration component; SD backup capture, distributed identity
assignment, overlap decisions and live migration activation remain unfinished.

`HalLegacyTintaJournalReader` borrows a `HalFile` and caller-owned scratch of
at least 12 bytes. It reads at most 32 records per chunk, freezes and checks
file extent, checks cancellation between chunks, and yields after 32 chunk
reads. It allocates nothing and leaves ownership/closing with the caller.
Read failures, extent changes and malformed records disarm further iteration;
their output entry stays unchanged. Zero tail bytes are validated through
the frozen end, including a partial final record. A returned record validates
only that prefix: migration must reach checked `End` before publishing any
result. The caller excludes writers and must separately bind the immutable
backup's hash/provenance; unchanged extent alone cannot prove unchanged bytes.
HAL tests cover the shared fixture with a 24-byte buffer, borrowed-handle
lifetime, cancellation, changed extent, short/failed reads and nonzero tails.
This reader is not yet activated in the native migration workflow.

`attachVerified` additionally requires the immutable backup's expected length
and SHA-256. It checks length and the 16 MiB bound before hashing, verifies the
initial bytes, and rehashes once at the end of validated iteration before
returning checked `End`. Same-length changes during a scan therefore disarm
the reader rather than accepting the validated prefix as a complete backup.
Both passes reuse caller scratch and the existing HAL hash helper; no heap
allocation is added. Tests reject wrong hashes/lengths, same-extent byte changes
and subsequent reads after failure, then accept an unchanged source. Backup
manifest capture and native migration activation remain unfinished.

`HalLegacyTintaBackupCopy` stages one immutable backup file through
`HalVerifiedFileStage`, using caller scratch for source copying and SHA readback.
The caller prepares a private destination parent and supplies canonical ASCII
paths, expected length/hash, and exclusive source access. The source is opened
for reading, checked before copying and again after seal, and never renamed or
written. Distinct source/candidate/backup paths are enforced with ASCII case
folding and path-component validation. A sealed matching candidate can resume
publication; a matching final backup makes retry a no-op. Mismatched existing
files are preserved. Only newly created staging files are owned by this copy
workspace, and publication uses rename after checked seal/readback. Tests
exercise alias rejection, unknown candidates, lost rename acknowledgment,
idempotent retry and corrupt backup preservation. Allocate this retained
handle/hash workspace with `makeUniqueNoThrow`; its buffers exceed the task
stack budget, and copying introduces no per-chunk allocation. The multi-file
manifest, power-loss recovery for partial candidates, backup export and native
migration entry flow remain unfinished. Physical SD durability is unverified.

Write or seal failures now immediately abort owned staging and attempt a
checked source-handle close, allowing retry with the same copier after the
storage fault clears. Sealed candidates retain their immutable ownership
status and pre-existing mismatches remain untouched. Fault tests inject write
and sync failures, verify original bytes stay unchanged and no final backup
appears, then successfully retry. A real power interruption still requires the
planned durable backup transaction/manifest before partial candidates can be
claimed or resumed across boot.

`CompanionLegacyTintaBackupManifest.h` defines a 436-byte `TLB1` reader backup
manifest. Reader, SD generation, logical course and transaction occupy bytes
4/20/36/52. A little-endian nine-role presence mask occupies 68–69; bytes 70–71
are reserved zero. Nine 40-byte entries start at 72, each containing an 8-byte
length and 32-byte SHA-256. CRC32 over bytes 0–431 occupies 432–435. Role order
is reviews, items, profile, lessons, readings, starred, usage, days, session.
The first three are mandatory. Absent roles have zero entries; present hashes
and all identities must be nonzero. Lengths fit 32 bits, with reviews bounded
to 16 MiB. The borrowed view exposes no metadata after failed decoding.
The owned input exceeds the embedded stack budget and belongs in a checked
heap workspace; encoding/decoding themselves allocate nothing.

Apple's `LegacyReaderBackupManifest` round-trips that wire format into its
existing verified JSON backup model and retains the transaction binding.
`days` and `session` are new optional JSON roles, so existing seven-role
backups remain valid. Native role selection/display recognizes both and uses
localized labels. Shared independently assembled fixtures and semantic
corruption tests with recomputed CRC verify cross-language behavior. The wire
manifest only describes expected bytes; actual immutable-file verification,
durable manifest publication, backup export and migration activation remain
required. Native Apple source was syntax checked; Xcode builds and UI execution
remain unverified in this environment.

`HalLegacyTintaBackupManifestStore` publishes immutable `TLB1` metadata after
the caller verifies every described backup file and prepares the private parent.
It validates the expected manifest and distinct canonical paths, stages bytes
with sync/close/SHA readback, compares complete metadata, rechecks the final
destination, then renames and reads back. A matching final record is a no-op;
different or malformed existing records are preserved. A complete matching
stage is resumed and sealed again before publication. Unknown partial stages
cannot be claimed by this store. Fault tests cover failure before rename, lost
rename acknowledgment, repeat publication and conflicts. Its 436-byte scratch,
hashing state and retained lookup handles require a checked heap workspace;
publication adds no per-loop allocations. Durable backup-transaction authority
for partial stages, automatic capture at migration entry and export remain unfinished.

`TintaProgressJournal` connects mapped bodies to the reserved-identity writer. It
records the exact committed review identity for undo and clears that target after
flags or undo, matching ProgressStore's local undo rules. Combined flags use the
atomic pair path. The adapter borrows its writer and keeps fixed mapping state;
it performs no allocation. Restart may supply an undo identity only from a verified
recovery checkpoint. The app callback must log failures and must not enable this
adapter until legacy migration and authoritative-to-derived recovery are complete.

For a live app binding, the adapter accepts a function-pointer configuration
provider and borrowed context. It refreshes scheduler configuration and clock
quality immediately before each mutation, so settings changes during an open
activity do not leave later reviews with the values captured at initial binding.
Provider failure rejects the mutation and reports an error before event bytes
or headers are written. Invalid scheduler values also fail body encoding before
writing. Fixed-configuration bindings remain available for immutable callers;
rebinding to one disables the dynamic provider. This adds fixed session-owned
pointers/state and no per-mutation allocation. Tests verify configuration
changes across reviews, unchanged journal files on provider/validation failure,
and successful rebinding. Native app activation remains pending; this
configuration provider does not itself turn local clock seconds into UTC.

Tinta's local clock counts seconds from 2024-01-01, while trusted distributed
timestamps use Unix UTC seconds. `tintaLocalSecondsToUnixUtc` adds the epoch
offset and subtracts the supplied local-minus-UTC offset at the event's instant,
using signed 64-bit arithmetic. Invalid offsets outside ±24 hours preserve
output. The caller must verify the RTC and know the correct offset, including
daylight saving; this helper does not infer clock trust. Unknown/device-quality
events may retain local timestamps because replay does not use them as trusted
ordering evidence.

The progress adapter now rejects trusted-quality persistence unless the caller
supplies an explicit timestamp. Dynamic bindings can provide a function-pointer
timestamp provider using the same borrowed app context. Provider failure
rejects the mutation before journal writes. Tests cover epoch and offset
conversion, crossing the epoch boundary, full 32-bit local seconds, preserved
output on invalid offsets, and actual trusted event persistence. These helpers
use fixed scalars and pointers without heap allocation. Native RTC/offset
integration still needs implementation and hardware verification.

The composition test links the production ProgressStore, scheduler, journal and
adapter. It verifies unchanged learner files when the authoritative header fails,
and a retained distributed event when the subsequent local write fails. In either
failure case ProgressStore refuses another callback until reopened. This verifies
mutation ordering and retry blocking; it does not yet replay authoritative events
into derived files or enable the app's journal binding.

`CompanionTintaItemReplay.h` provides a caller-owned item replay step using the
original C++ scheduler. It applies each review's configuration, reports new/review
counter increments using the existing study-day rules, and updates suspension/star
bits without replacing derived leech flags. The caller must validate envelopes,
deduplicate and order events, filter undone reviews, and handle completions separately.
The step allocates no memory and is not yet a reader replay/install controller.

The storage adapter owns an event file and two independent header files. A successful
write or truncate must be durable. Header slots must reside in separate sectors/files,
so a torn write to the next header cannot damage the current committed header. Only
one journal owner may mutate these files; SD access must use `HalStorage` serialization.
`HalTintaJournalStorage` implements the firmware adapter at
`/.crosspoint/companion/tinta-events/`: `events.bin`, `header-a.bin`, and `header-b.bin`.
It keeps one mutex-wrapped event handle open during journal scans and closes it on
adapter destruction or explicit `close()`. Header handles are local RAII handles.
Each event/header write and recovery truncate calls `HalFile::sync()` before success;
SHA-256 uses the SDK's verified mbedTLS API. Live mutation integration is still pending.

The core borrows 512 bytes from caller-owned workspace and allocates no buffers. Its
large decoded event/body members must be kept in session-owned storage, allocated with
`makeUniqueNoThrow` and checked before use when integrated; placing the journal object
on the task stack is unsafe. The fixed 512-byte record supports the largest distributed
envelope (293 bytes) and all version-one Tinta bodies (54 bytes maximum), with zero
padding and a CRC32. It is specifically a Tinta journal; larger reading/bookmark bodies
need their own persistence path.

| Event record offset | Field |
| --- | --- |
| 0 | `TJE` magic, version byte 1 |
| 4 | Envelope length u16 |
| 6 | Body length u16 |
| 8 | Four reserved zero bytes |
| 12 | Encoded SyncEvent, then body, then zero padding |
| 508 | CRC32 of bytes 0–507 |

| Header offset | Field |
| --- | --- |
| 0 | `TJH` magic, version byte 2; version 1 remains readable |
| 4 | Commit sequence u64; starts at 1 and advances once per commit |
| 12 | Committed record count u32 |
| 16 | Reserved zero bytes through offset 59 |
| 60 | CRC32 of bytes 0–59 |

All integers are little-endian and read bytewise. Header slot is sequence modulo two.
Version-1 headers require sequence equal to count + 1. Version-2 headers permit
one or two records per commit. If both headers validate, commit sequences must be
consecutive and counts must advance by one or two. A new empty journal durably creates
its initial zero-count header before the first event. No valid header alongside data
or damaged header files fails closed rather than inventing an empty history.

Append validates the body's SHA-256, scheduler configuration digest, envelope binding,
and undo ancestry. It scans committed identities: matching events return `Duplicate`
without writes; conflicting content returns `Conflict`. New appends durably write the
512-byte record first, then commit the next alternating header. A failed storage write
invalidates the open journal until recovery. The caller must update derived state only
after `Ok` or a verified `Duplicate`, and must reserve origin/epoch/sequence through the
identity protocol before constructing the event. That identity integration is pending.

`appendPair` validates two independent caller-owned events, writes both records,
and publishes their combined count through one alternating header. Recovery drops
both uncommitted records after a failed write, or retains both after a committed
header whose acknowledgement was lost. Retrying an identical pair returns Duplicate
without writes; a mixture of already committed and fresh identities returns Conflict.
Tests start from version-1 headers and cover every byte cut in either 512-byte record
and the 64-byte header, reopening and retrying with the same identities. Version-2
headers are incompatible with older firmware; downgrade gating must account for this
before live history is enabled. Writer identity reservation and live batch integration
remain pending.

`TintaWriter::recordFlags` assigns two sequences within its reserved epoch, binds
the suspension/star bodies to the same course and item, and makes the star depend
on the suspension. The suspension depends on the previous frontier. Both events
commit through `appendPair`; the writer advances its frontier and sequence only
after success and disables itself after an uncertain write. Fixed event/body members
reuse session-owned memory rather than allocating in mutation loops. The app still
needs migration and derived-state recovery before binding this writer to mutations.

Open validates all committed records and their cryptographic/body bindings. Any data
past the committed count is an interrupted append and is truncated durably. A fully
written header whose acknowledgement was lost still commits the event, so retry returns
`Duplicate`. Corrupt committed data, a file shorter than its committed count, absent
headers alongside data, and failed truncation are preserved and prevent further writes.
Borrowed decoded event/body views cannot be appended directly because scanning would
invalidate them; callers must supply independent session-owned input.

Host tests exercise every record-write cut (0–512), every header-write cut (0–64), lost
commit acknowledgement, duplicate/conflicting delivery, committed-data corruption,
missing headers, truncation failures, undersized workspace, and borrowed-view misuse.
The host compiler checks public journal operations with no exceptions/RTTI and a
256-byte stack-frame warning gate. Hardware acceptance must repeat interruption tests
through the HAL adapter, inspect journal/header files, and verify recovery precedes
reading or derived-state updates. Initial-header damage intentionally requires repair
rather than silently discarding ambiguous state.

The adapter uses only HAL storage calls. The retained `HalFile` uses the HAL's existing
handle allocation once per journal lifetime; keeping it avoids allocation/free for
every scanned record. It adds no explicit heap allocation or new scratch buffer.
Call `close()` before unmounting the card, entering USB Drive, or releasing the owning
activity. Physical validation should monitor free/largest heap before opening the
journal and after closing it, inspect all three files after interrupted writes, and
confirm no derived-state change is acknowledged before the committed header sync.

`CompanionTintaWriter.h` now binds journal appends to the existing durable identity
protocol. `start()` recovers the journal, remembers the latest committed identity,
and reserves a fresh NVS epoch through `provisionIdentity`. Local sequences start at
one and advance only after a durable `Ok`/verified `Duplicate`. New events depend on
the last committed event; undo adds its exact review target when it is a different
dependency. The writer hashes the canonical body and review configuration itself.
Ambiguous writes disable the writer until another recovery and fresh epoch reservation.
This prevents restarting an uncertain sequence while retaining a commit whose reply
was lost. Callers must rebuild derived state from the recovered authoritative journal
before issuing a new mutation; this recovery/replay integration is still pending.
The writer is session-owned and adds no allocation or large stack buffer.

Two host tests cover epoch changes, sequences, chained ancestry, exact undo targets,
failed epoch persistence, and lost journal acknowledgement. The full companion host
suite currently passes 68 tests. Live review/control call sites are not yet wired.

Duplicate comparison uses only serialized event fields and the active ancestor prefix.
Unused in-memory ancestor slots do not affect identity equality or trigger writes.
A regression test covers stale inactive slots and confirms that changed persisted
metadata still returns `Conflict`. The full companion C++ suite now passes 69 tests.

### Checked header absence

HalTintaJournalStorage now uses checked directory enumeration for header presence instead of Storage.exists. A missing header requires successful end-of-directory and handle closes; enumeration/open/name/close errors return failure to TintaJournal recovery. The shared lookup accepts a borrowed null-terminated parent constant and still restricts targets to direct children of that parent. The default transfer-directory callers retain their scope. The journal storage owns the lookup's 256-byte name buffer and two reused HAL handles rather than putting them on the task stack or allocating per entry; HAL handle backing allocation occurs on first use and is retained for reuse. The 29 selected lookup/course/journal tests and default firmware build pass. Host tests cover the lookup and journal separately; a composed test through the real HAL journal provider remains needed, as does device fault testing. Verify SD read errors stop journal recovery without header replacement or learner-history reset before enabling live mutation recording.

### Composed HAL header recovery tests

HalTintaJournalStorageTest compiles the production provider and journal together against file-backed HAL and OpenSSL SHA-256 shims. Enumeration failure blocks initial header creation, and after a committed star event it leaves every file unchanged. A false exists result does not hide headers; restoring enumeration recovers the original event. Malformed header bytes produce Corrupt without replacement. Both composed tests pass. These tests establish the provider/journal boundary under simulated storage faults; live mutation integration and physical SD failures remain unverified. Production source is unchanged by this test addition, so its previous successful default firmware build remains applicable.

### Reader undo target proof

TintaWriter now requires an undo target to exist in its committed journal as a Review with the same course and item UID. Unknown identities, non-review targets, different courses and different items return Invalid without writing, changing the causal frontier or consuming a sequence. Read/validation failures disable the writer through its existing failure path. The backward scan reuses journal scratch and keeps decoded target body state in the session-owned writer; no heap buffer is added. It can read up to the full journal for an unknown/old target, so live integration still needs bounded/yielding storage scans and hardware latency checks. All ten journal tests pass, including the four invalid-target cases followed by a successful exact undo. This writer is still not wired to live Tinta mutations; firmware compilation of the unused header would not establish that integration. Verify live integration eventually rejects an unrelated review identity while allowing exact undo and restoring derived state after restart.

### Cooperative HAL journal scans

The HAL event reader yields one FreeRTOS tick after every 32 successful read calls, following HalInventoryIndexStorage's existing policy. Its one-byte counter resets on close; the event handle remains reused during scans. Three composed HAL tests pass, including exact yield boundaries, reuse and reset. This bounds read calls between explicit yields, not wall-clock duration: slow SD operations and complete history scan latency still require device measurement. No heap allocation is introduced. Verify long recovery/undo scans on C3 with serial logs and stack/heap measurements before enabling live history recording.

### Constant-read local appends

Public TintaJournal.append retains full duplicate/conflict scanning for imported events. TintaWriter.start reserves a durable epoch and scans once to prove no committed event already uses that origin/epoch. Its private friend-only append path requires the journal count to match the writer's last committed count, then writes the validated event and header without rereading history. Sequence advancement remains tied to durable success; write errors disable the writer. A competing append or a restored identity binding that reuses an occupied epoch prevents the shortcut. This adds a four-byte writer count and no heap allocation. Twelve journal tests pass, including 64 local records with zero history reads, competing-owner rejection, epoch reuse rejection, restart and existing duplicate/power-cut cases. Startup/undo scans remain linear; live Tinta integration and actual SD timing remain pending. Verify per-review journal reads stay constant in a long device session and interrupted writes recover before derived learner state changes.

Host optimized wrappers for writer start/record also compile with `-fno-exceptions -fno-rtti -Werror=frame-larger-than=256`. This checks those host-generated frames, not the final RISC-V stack watermark or the unintegrated writer's device behavior.

`HalLegacyTintaBackupCapture` composes file copying and manifest publication for
the complete nine-role set. It rejects case-insensitive path aliases across all
sources, candidates, backups and manifest paths before any copy. Each present
role must verify before the final manifest is published; retries verify existing
backups and preserve conflicts. The caller retains a separate immutable encoded
manifest, disjoint copy scratch and path table, excludes writers and prepares the
private destination parent. Allocate the capture workspace with checked
`makeUniqueNoThrow` because its retained file owners and manifest scratch exceed
the task-local budget. This helper is not yet invoked automatically by migration.
Host coverage includes cross-role aliases, a changed source withholding the
manifest, successful retry, repeated capture and corrupt completed backup refusal.
On hardware, interrupt copying between roles and verify originals remain intact,
then retry and check every manifest length/hash against the backup bytes. Unknown
partial candidates remain preserved pending durable transaction ownership.

Capture also accepts reader/generation/course/transaction identities with a
source path table through `captureSources`. It hashes every supplied source
through HAL into the retained manifest workspace, rejects missing mandatory
reviews/items/profile roles, and then runs complete-set capture. Optional absent
roles have no paths. Retained manifest input and encoded bytes increase this
checked heap workspace by fixed storage; there are no per-file allocations or
whole-file buffers. Source writers must remain excluded throughout hashing and
copying. Durable transaction identity/path selection and automatic startup
invocation remain to be implemented.

The backup manifest store provides read-only `load` into caller-owned 436-byte
storage. Missing files return Missing; lookup/read/extent/format failures return
Error and log the failure. Output changes only after a full valid read. This
checks the manifest structure and CRC, not the described backup file content;
migration must still verify every file hash and reader/generation/course binding
before consuming it. Loading reuses the store scratch and allocates no heap.

`verifySaved` loads the published manifest, matches all four expected identities,
rejects absent-role paths and case-insensitive backup aliases, then hashes every
present backup file against its recorded length/SHA-256. It uses retained
workspace and never publishes, repairs or removes files. Original sources need
not remain present. Callers must exclude backup writers for verification and
subsequent consumption, and select transaction-scoped paths themselves. Tests
cover wrong generation, alias rejection, successful verification without an
original source and corrupt backup refusal while preserving all SD bytes.

Capture can receive distinct intent and intent-stage paths. After validating all
paths, it publishes the immutable expected manifest as an intent before copying
any file. A conflicting intent stops capture before file changes; successful
capture retains the intent as a witness. The additional retained manifest store
uses fixed scratch in the checked heap workspace, with no loop allocation.
Omitting both intent paths preserves the non-transactional capture API; native
migration must supply canonical transaction-scoped intent paths. The intent
does not yet authorize deletion/resumption of partial candidates: recovery must
first bind canonical paths and verify the current reader/card identities.
Tests verify intent survival when a later source fails, conflicting transaction
refusal, successful retry and final manifest publication after the complete set.

`recover` reads the durable intent and requires exact reader, generation, course
and transaction identities before continuing capture with its original receipts.
It does not rehash sources into a replacement intent. Matching completed backups
are reused; changed sources and conflicting files stop recovery. Callers retain
the original path mapping and exclude writers. No additional heap allocation is
introduced. Tests recreate the capture owner after an incomplete set, reject a
foreign generation, preserve changed sources, then complete and repeat recovery.
Partial candidates still require canonical path ownership recovery before they
can be resumed or discarded; automatic native invocation remains pending.

`HalLegacyTintaBackupSession` owns canonical destination paths for all four
identities, reusable 512-byte scratch and one checked capture owner. Prepare
retains borrowed source paths without SD changes; run prepares the directory
chain and captures, while recover/verify use the same canonical destinations.
Allocate the session on the checked heap because fixed path storage exceeds
the task-local limit; capture is allocated once per preparation, never per file.
Host fault coverage recreates the session after both failed manifest rename and
lost rename acknowledgement, removes original sources after complete copies,
then verifies successful manifest recovery and repeated recovery without writes.
This is host evidence; physical SD power-cut testing and startup migration
integration remain required.

Canonical session recovery now permits rebuilding a shorter candidate only
after loading the matching durable intent, verifying the entire original file
SHA-256 and comparing every candidate byte with the source prefix. It rechecks
the original and candidate extent, closes both before removal, then performs
the normal copy/seal/publication. Mismatched prefixes or changed original bytes
remain untouched. Only the canonical session has access to this authority; raw
capture continues to preserve unknown partials. Prefix comparison splits the
existing scratch into two halves and yields every 32 reads, adding no heap
allocation. Tests cover an 8193-byte prefix and an original changed beyond it.
On device, cut power mid-copy, retry with the same card/transaction and check
original hashes, complete backup hashes and final manifest before migration.

Authenticated BLE ExchangeChanges now routes 64-byte TLR version-1 backup
requests to a retained checked HAL exchange workspace. Capture requires the
actual installed course binding and bound course state; unbound legacy capture
remains unavailable. Requests must use the current SD generation. Unfinished
content installation, active journal export and journal reception exclude backup
exchange; a backup session excludes content transfer and journal exchange.
Close, disconnect, authorization reset and activity exit release handles and
workspace through the existing session lifecycle. Saved backup export uses
canonical reader/generation/course/transaction paths and immutable receipts.
Wi-Fi validation/routing support, Apple collection/import, usage-log preservation
and migration activation remain unfinished. Verify on hardware by authenticating,
capturing the installed course, dropping BLE during chunk export, reconnecting
and reopening at the saved offset, then comparing all assembled file hashes.

Encrypted Wi-Fi ExchangeChanges validation now admits a transaction prefix plus
the 64-byte backup request only when inner transaction and card generation match
the authenticated handoff. The shared callback routes it through the same backup
handler as BLE. Apple Wi-Fi transport validates those bindings before sending
and decodes the reply against the unprefixed request. Host encrypted transport
coverage verifies a valid reply and rejects a foreign generation without extra
traffic. Independent backup-only Wi-Fi handoff initiation remains unavailable:
the current handoff lease still originates from a content transaction, and
backup exchange is allowed only after content commit/abort releases its guard.

The native preserved-backups view now offers an explicit connected-reader
capture/export action for the selected course. Its model uses the resumable
collector and application-support staging; transfer controls remain busy during
the operation. A reader/generation/course-scoped transaction is retained across
failed attempts and removed after successful registration so a later action can
create a new snapshot. Capture retries first inspect their immutable intent and
recover existing copies, preserving the original snapshot even if learner files
have changed after it was copied. Host tests recreate the exchange owner, change
an original and verify retry exports the original backup bytes. Native app
syntax parsing is available here; Xcode type checking and iPhone/Mac execution
still require an Apple host. SQLite-backed durable backup job tracking and
collector progress/cancellation UI remain to be integrated.

`ProgressStore::MutationJournal` includes an optional authoritative recovery
callback alongside its persist callback. The owner installs the binding before
opening progress. With disk storage available, recovery runs before any local
items/review file is read or created; failure leaves the progress store failed
and prevents mutation. With no disk available, guest opening skips recovery.
The owner must log callback failures and outlive the store. The callback adds one
function pointer to the existing binding and introduces no heap allocation.

Progress-store host tests restore a reviewed item before local reads, check that
recovery failure performs no local reads/writes and blocks reviews, and check that
guest opening does not invoke disk recovery. The live activity still needs an
owner that imports/backups legacy history, publishes an authoritative baseline,
replays pending authoritative mutations, and binds progress before opening it.
Lesson completion currently updates the profile's contiguous completion position;
reading/lesson completion integration and native journal activation remain
unfinished. Attaching only the persist callback would leave a power-cut gap
between authoritative append and local progress publication.

`core::library::MarkLog` (completed readings and starred words) exposes a
`MutationJournal` binding with persistence and recovery callbacks. Persistence
runs before changing the in-memory set or appending its local record. A failed
authoritative callback leaves marks unchanged and blocks further mutations until
reopen. Duplicate additions and removals of absent keys do not emit events.
Recovery runs before local reads when disk storage is available; failure blocks
mutation, while no-disk guest opening skips disk recovery. Default bindings keep
the existing local/guest behavior. The owner logs callback errors and maps each
log's keys to the appropriate stable course subject; raw reading/word keys are
not automatically distributed item identities. The fixed callback binding adds
no heap allocation.

Library host tests cover add/remove callback ordering, rejection without RAM/SD
changes, failed-state blocking, reopen, duplicate suppression, and recovery/guest
ordering. Activation still requires legacy baseline capture, native recovery
ownership, stable subject mapping, lesson completion journaling, and binding the
callbacks before the app opens its logs. On device, cut power after authoritative
append and verify that reopen restores the mark before the reader UI displays it;
also inject a failed authoritative append and verify that no mark is shown.

Lesson completion uses `core::LessonCompletion`, owned by the native App. Its
optional persistence callback receives the inclusive zero-based range newly
completed by advancing `profile.currentLesson`; completing a later unlocked
lesson therefore includes every preceding unfinished lesson. The owner must map
these indices to the installed pack's stable lesson identities and publish the
range recoverably. Callback failure leaves both completion and unlock fields
unchanged and blocks later attempts until recovery. Repeating an already
completed lesson emits no new event. The binding is fixed callback/context data
and introduces no heap allocation.

The optional lesson recovery callback runs before loading profile.bin when disk
storage is available; a failed callback stops App opening. No-disk guest opening
skips disk recovery. `App::lessonCompleted` reports failure, and SessionController
clears its completed-lesson summary claim on that failure. Unbound/default local
behavior keeps the existing profile save. Profile tests cover range binding,
unchanged profile on failure, failed-state blocking, recovery, duplicates, invalid
lesson indices, and the legacy contiguous completion/unlock behavior. Native
journal ownership, stable lesson mapping, baseline migration, and actual recovery
publication are still required before activating the callbacks.

`CompanionTintaMarkJournal` adapts native MarkLog persistence to durable Star or
ReadingComplete events. Its owner supplies stable subject resolution, study-day
and clock metadata, authoritative recovery, and error logging. Invalid or
unresolved keys do not append events; a durable writer failure rejects the local
mutation. The adapter stores fixed callback fields and uses no heap allocation.
Journal tests cover stable completion identities, add/remove bodies, metadata,
resolution rejection before writes, recovery failure, and writer failure.

`resolveTintaLegacyStoryKey` streams titles from the immutable validated pack to
match native title-hash keys without using the string arena. It rejects missing
or ambiguous keys and preserves its output on failed reads. The fixture includes
repeated titles, so tests explicitly select a unique key before introducing an
additional collision. This resolver and the mark adapter remain unbound in the
native activity until baseline migration and recovery publication are ready.

`restoreTintaNativeMarks` builds a complete TMK1 snapshot in caller-owned scratch
and replaces the local mark file only after every stable identity has resolved.
The maximum scratch requirement is 772 bytes, so native owners must retain it as
a member or borrow an existing workspace rather than placing it on the task
stack. Failed reads, missing mappings, duplicate native keys, and completion sets
above MarkLog's 96-key capacity reject publication without replacing local state.
The caller must bind and verify the completion source against its course and
journal frontier before invoking this helper.

The native journal test target now links the production MarkLog implementation.
It cuts each local append after a durable completion event, leaving no bytes,
a partial record, zeros, or garbage. Reopening through recovery and the production
snapshot encoder restores the mark; a repeated add creates no new event, and a
subsequent remove survives reopening. Failed recovery prevents local reads and
mutations. These are host fault-injection checks, not proof of physical SD
power-cut behavior or complete baseline migration. Live activation still needs
verified source ownership, reverse story mapping, and a policy for histories
larger than the current native mark capacity.

Reverse reading projection now uses `resolveTintaStableStoryIdentity`: it finds
the stable story in the validated immutable pack, computes the native title key,
and verifies that the forward resolver returns exactly that story. Shared title
keys and stable identity collisions reject projection. Failed source reads,
missing identities, and invalid inputs preserve output. Both directions stream
titles without allocating heap memory or touching the string arena; reverse
projection scans the pack twice rather than retaining an in-memory title index.

`HalTintaCompletionSetView` exposes validated entry counts and indexed identities
for snapshot recovery. Indexed access reads four bytes through HalFile, preserves
output on errors, and invalidates the view until begin() succeeds again. Its
borrowed file must remain immutable and be bound to the intended course/frontier
by the owner; size checks do not substitute for receipt/hash verification.

`restoreTintaNativeReadings` connects the SD completion view, reverse story
resolver, and native snapshot encoder. It requires an open pack and a validated
Readings-kind set; a Lessons-kind set cannot be used for reading projection. It
borrows all inputs and the caller's snapshot scratch, introduces no heap
allocation, and logs failed projection before returning. Native owners must
verify the source receipt and immutable pack against the same course/frontier
before calling it; the adapter does not establish that ownership itself.

HAL host tests now link Pack and MarkLog and exercise a stable story from the
real fixture through TCS1 on the fake SD into TMK1 and MarkLog. Source-read
failure, an unknown stable identity, and a wrong completion kind leave the
existing native file unchanged. This joins the projection components but does
not yet bind authoritative recovery in TintaActivity or create a legacy baseline.

Native App startup now stops before constructing the learner UI when progress,
starred-word, or completed-reading authoritative recovery fails. ProgressStore
exposes `authoritativeRecoveryFailed()` independently of ordinary local-storage
failure, so an unbound local failure retains its existing fallback. The flag
resets on each open, and guest opening does not report disk recovery failure.
App releases the course pack/cache/slot allocations and cached state handle
before returning false; TintaActivity then follows its existing open-error path.
Recovery callbacks still have to be bound before App opening for this gate to
apply. Progress-store tests cover rejected recovery and flag reset on guest
reopen; journal integration tests cover mark rejection before local reads.

`restoreVerifiedTintaNativeReadings` checks manifest binding to the expected
course, storage generation, pack digest, and journal frontier, then verifies the
actual Readings file length and SHA-256 before validating its TCS1 format and
projecting native marks. It borrows the same 772-byte-or-larger workspace for
hashing, completion validation, and snapshot construction. The owner must still
verify the actual installed pack digest, obtain an authoritative frontier, keep
manifest bytes outside scratch, and exclude source writers throughout recovery.
The entry point is not yet invoked by the native activity.

HAL host tests reject each mismatched binding, corrupted file content, and a
failed hash read without replacing read.bin; matching receipt/file/pack subject
projection produces the expected native mark. These tests verify the reading
receipt gate, not full baseline migration or authoritative journal ownership.

When a derived receipt already names the merged frontier but the per-course
merge-applied receipt is absent, reconciliation now checks all five active file
lengths/hashes before finalizing the applied receipt. This covers interruption
between derived publication and merge acknowledgement. An already-applied merge
still permits subsequent native local study mutations. Course-transfer host tests
remove the applied receipt, corrupt published items, verify rejection without SD
changes, restore the file, and finish reconciliation successfully.

`LessonCompletion::project` derives native profile navigation from authoritative
per-index completion lookups. It finds the first unfinished lesson, retains
bounded existing unlocks, and unlocks the successor of each completed lesson.
Only after all lookups succeed does it update currentLesson/unlockedThrough;
other profile fields remain untouched. Empty courses project both fields to zero.
The function uses fixed scalar locals without heap allocation. Profile tests
exercise all 32 five-lesson patterns and failure at every lookup position.

This projection is not yet called by native recovery. Individual completion
badges must consult the authoritative completion set when it contains gaps;
the contiguous profile position alone cannot represent sparse completion history.
Stable lesson-index mapping, receipt verification, and profile replacement remain
part of the pending native recovery integration.

`projectTintaNativeLessons` joins stable lesson-index mapping, a validated Lessons
completion set, and navigation projection. It scans current pack lessons and
queries the set by stable identity; the number matched must equal the set count,
so unknown completion subjects cannot silently disappear. Pack lesson identities
must have been validated as unique by the owner. Projection uses a temporary
Profile plus scalar/reference fields without heap allocation and changes only
navigation fields after all checks pass.

`projectVerifiedTintaNativeLessons` additionally verifies course/generation/pack/
frontier binding and the actual Lessons file hash/length before projection. HAL
host tests cover a real pack lesson, absent subjects, failed reads, wrong set
kind, mismatched receipt binding, corrupted file content, and successful receipt-
verified projection, preserving navigation fields on failures. Neither entry
point persists profile.bin or is bound in native startup yet; sparse completion
badges and authoritative recovery ownership remain required.

`persistTintaNativeLessonProjection` projects into a temporary profile and uses
Profile::save's StateStore replacement only when navigation fields changed.
Unavailable storage or failed replacement leaves the caller's navigation fields
unchanged and logs an error. Once replacement succeeds, only currentLesson and
unlockedThrough are copied back; unchanged projection avoids redundant SD writes.
The owner must load the profile and verify completion ownership first.

HAL tests link the production profile encoder/loader and interrupt replacement
with missing, partial, zeroed, and garbage writes. They verify the previous file
and RAM navigation survive failure, retry restores navigation while retaining
study settings, repeated projection causes no additional write, and unavailable
storage rejects persistence. Physical SD replacement recovery and native startup
binding remain unverified and unfinished.

Lesson recovery accepts the existing InventoryHashProgress callback through
receipt hashing, completion validation, projection, and profile persistence.
Projection checks it before scanning, before each lesson lookup, and before
publishing navigation fields; persistence checks it again before replacement.
The owner can yield or cancel through this fixed function/context binding, with
no allocation. Cancellation leaves caller navigation unchanged and does not
write a profile. HAL tests cancel at every projection boundary and at the final
replacement boundary. Native startup must supply an appropriate yielding
callback when these recovery entry points are activated.

`mapLegacyTintaMutation` maps decoded native legacy entries to the same body
semantics as Apple migration: reviews retain quarter-second response precision,
undo uses an owner-supplied exact event identity, and every flags record emits
both suspension and star bodies. It rejects leech-bit changes that cannot be
represented as those mutations, invalid subjects, and invalid review/undo bodies.
Rejected mapping preserves output. Fixed two-body storage adds no heap allocation.
Native journal tests cover response timing, configuration, exact undo targets,
all prior/new three-bit flag combinations, and unchanged output on rejection.

The owner still must verify source backup hashes, replay legacy state, resolve
undoRecord against durable migration identities, and compare rebuilt learner
state before publication. This body mapper does not reserve migration provenance,
append events, establish history overlap decisions, or activate native migration.

Apple migration epoch reservation now excludes origin/epoch pairs reserved by
other legacy backups, in addition to local origins and installed sync events.
The SQLite immediate transaction covers collision checking and insertion, so
pending migrations own their identities before installation. A reopened store
reuses the original backup's reservation; collision rejection does not change it.
Different origins may use the same numeric epoch. Swift tests cover pending
collision rejection across database reopening, original retry, and independent
origin reservation; all 350 package tests pass.

Apple installation now compares TintaHistory replay of a migration plan's events
with its supplied snapshot before beginning the installation transaction. An
inconsistent snapshot is rejected rather than ignored. The migration test adds a
lesson completion absent from the event stream, checks rejection, and continues
through valid installation and retry. All 350 Swift package tests pass. This
checks event/snapshot consistency; verified backup reconstruction and overlap
confirmation still belong to migration preparation.

`HalTintaWriterSession` now composes active HAL journal storage, a fixed 1 KiB record workspace, the journal reader and the authoritative writer. Retain it through checked `makeUniqueNoThrow` outside the small activity stack; it makes no per-mutation allocations. Shutdown invalidates the writer before closing its retained HAL file handle. Failed startup/close poisons the owner, so recovery requires a new owner rather than continued writes through uncertain state. Startup requires the caller to finish legacy migration and derived-state recovery first. This owner is not yet allocated or bound by `TintaActivity`; progress/mark/lesson hook wiring and native recovery integration remain unfinished.

A portable lifecycle test checks that repeated stop prevents persistence, then explicit restart reserves a fresh epoch and retains prior events. The native owner start/close entry points pass the C3 compiler's 256-byte frame check. Physical heap/stack and lifecycle acceptance remain pending.

`tintaConfigurationFromProfile` converts the live native profile's retention permille to exact journal basis points and preserves maximum interval. It accepts the native profile ranges (700–970 permille, 1–36500 days), rejects invalid settings before assigning output, and allocates no memory. A dynamic progress-journal provider can call it after profile changes have applied to FSRS. Boundary tests verify accepted configurations encode successfully and invalid values preserve the prior output. This conversion is not yet called by the activity; the complete recovery and hook owner must provide it for every review.

The native Tinta clock now exposes `unixUtc` through `HalClock::unixTime`, using the same cached UTC snapshot that produces local wall time. It requires Tinta's RTC/date plausibility checks and returns false without changing output when only a learner-confirmed day is available. UTC availability does not assert distributed Trusted clock quality. No buffer allocation is added; the HAL uses its existing cache and a small local calendar structure. The native clock host test exercises UTC forwarding with different local wall time, unavailable UTC, old RTC dates, absent RTC and confirmed-date-only scheduling. The mutation owner still needs to consume this API and assign clock quality during hook binding. Physical DST changes, RTC read failures and reboot testing remain required.

`readTintaMutationClock` supplies the native mutation callback contract without allocating: a plausible RTC yields its UTC snapshot and Device quality; a learner-confirmed date yields its study day, timestamp zero and Unknown quality. An unknown date or failed UTC read rejects the mutation before changing any caller outputs. It never asserts Trusted quality solely from RTC availability or a confirmed date. The actual native clock test covers these outcomes and unchanged outputs after failed sampling; the provider passes the C3 256-byte frame check. The activity mutation owner still needs to bind this provider.

`HalTintaProgressMutationContext` now provides the dynamic configuration and timestamp callbacks for `TintaProgressJournal::binding`. It borrows the activity's profile and clock, captures exact scheduler settings and clock evidence together, and supplies that UTC sample once only when the local journal entry's study day and quality match. A failed capture clears the sample. This adds no heap allocation and rejects rollover/mismatched callback reuse before authoritative persistence. The native clock test checks UTC forwarding, single-use sampling, study-day mismatch and settings changes. The binding entry point and its referenced callback chain pass the C3 256-byte frame check. The activity must still provide recovered journal ownership and error handling before installing this binding on ProgressStore.

`HalTintaMarkMutationContext` now supplies mark-journal callbacks with borrowed immutable pack/catalog and native clock, plus caller-owned recovery/error delegates. Stars retain validated item UIDs; completed-reading title keys resolve through the full unique legacy-key scan before course membership is accepted. Missing, ambiguous, invalid or unreadable subjects leave the output unchanged and reject persistence. Recovery and error delegates are mandatory before callbacks are supplied. No heap or title arena is added. Tests exercise actual-pack reading-key resolution, star UID preservation, missing/IO membership and unsupported mutation kinds; existing resolver tests cover ambiguous title keys and failed reads. The native callback entry point passes the C3 frame-size check. Binding these callbacks to the activity's mark logs still requires verified startup recovery and mutation-owner lifetime management.

`TintaLessonJournal` supplies the native lesson completion mutation binding using validated immutable lesson identities and course membership. It preflights the entire inclusive range before any event is written, samples clock evidence once, and durably records each LessonComplete before allowing the profile to advance. Missing recovery/error/clock delegates reject admission. Mid-range I/O failure leaves the profile unchanged and a durable prefix for startup reconciliation; this is not an atomic range transaction. The owner must replay that prefix before reopening the profile. Tests reject a missing last subject without any journal write, persist the complete range before advancement, keep repeated completed advancement write-free, and inject second-event failure to preserve the first event while refusing profile advancement. No per-lesson allocation is added; the adapter retains its body off stack with the mutation owner. The binding callback chain passes the C3 256-byte frame check. Native activity recovery/binding remains unfinished.

`HalTintaMutationBindings` composes progress, star, reading and lesson adapters against the real App store APIs. Retain it off stack via checked `makeUniqueNoThrow`: its callback contexts and mutation bodies exceed the local stack budget and must be released when the learner activity exits. Borrowed App/writer/validated pack/catalog/lesson identities must outlive the binding owner. It requires an available writer and explicit recovery/error delegates, installs authoritative recovery before each store reads local derived state, logs persistence/recovery failures, and detaches every hook on unbind/destruction. Stop local mutation execution before unbinding and destroy the binding owner before destroying App. The native C3 bind/unbind and referenced callback chain compile against the real App APIs within the 256-byte frame limit. This is compile evidence, not native lifecycle execution; Activity allocation, verified recovery implementation, memory gating and physical acceptance remain unfinished.

`persistVerifiedTintaNativeLessons` combines exact course/card/pack/frontier binding and completion-file verification with profile replacement. It operates on an already loaded profile, retains all portable/device preferences, saves a candidate before assigning lesson fields and skips replacement when the projection is unchanged. Fault tests now exercise both bare and verified persistence with no-write, prefix, zero and garbage replacement failures; they reopen the profile to check retained preferences and verify repeated recovery causes no further writes. Wrong-course admission preserves profile and stored bytes without a write. The new combined helper passes the native C3 256-byte frame check. The recovery owner still needs to establish a loaded profile and invoke this helper before learner UI admission.

`restoreVerifiedTintaNativeStars` rebuilds native `starred.bin` from canonical, receipt-verified item flags. It requires matching course/card/pack/frontier, validates both item headers and records, checks starred UID membership and collects at most 96 keys in a reserved tail of caller scratch before encoding the existing native mark format. The workspace is `TINTA_NATIVE_STAR_WORKSPACE_SIZE`; receipt bytes remain separate and source writers are excluded. This uses no new heap allocation or per-item allocation and does not rescan items for each key. Excess star count, corrupt input, wrong card, failed membership or replacement leave native marks intact. Tests open the recovered file through real MarkLog, reject 97 stars explicitly, reject changed source hash/wrong card and simulate a torn profile-store replacement. The helper passes the C3 256-byte frame check. Activity recovery wiring and a broader native capacity policy remain unfinished.

Native mark restoration now compares an existing same-length file against the canonical candidate in 64-byte stack chunks and skips replacement when all bytes match. Comparison read failure returns IO error without replacing the file. This adds no heap allocation and avoids redundant SD serialization/replacement during repeated startup recovery. A star-recovery test verifies unchanged write count on repeated recovery and preserved bytes/write count after card loss during comparison. The existing journal/mark tests pass; the native star recovery callback chain retains its C3 256-byte frame check.

Guest learner changes remain RAM-only even when an authoritative writer is available through another HAL path. ProgressStore invokes mutation persistence only in disk mode; MarkLog bypasses its authoritative callback when local storage is unavailable; App passes current storage availability to lesson completion so guest profile advancement skips persistence. Default durable lesson behavior remains unchanged for ordinary callers. A regression test keeps an available authoritative writer while exercising guest review/undo/flags, marks and lesson advancement, verifying RAM effects without local or authoritative writes or persistence-error callbacks. Native Activity hook binding still remains unfinished.

ProgressStore's optional `MutationJournal::committed` callback runs only after the local journal/header/item commit succeeds in disk mode. It lets the mutation owner durably acknowledge derived application after the earlier authoritative persist callback. Failed acknowledgement marks the store Failed and blocks further mutations until recovery; it does not undo the already durable local mutation. Existing bindings omit the callback and retain their behavior. A test verifies the persist/commit order, failed acknowledgement, blocked subsequent writes and reopening the two committed local reviews. The recovery owner and native binding still need to supply and verify a durable acknowledgement protocol; this hook alone is not proof of reconciliation completeness.

The progress adapter now retains the exact identity of a mutation awaiting local application, separately from the undo target. `HalTintaMutationBindings` requires an acknowledgement delegate and installs the ProgressStore committed hook. It passes that retained identity and the local entry/before/after state to the delegate after disk commit, clears pending application only on success, and stops the authoritative writer on failed acknowledgement. A no-op flags change creates no event and does not acknowledge an unrelated prior frontier. Tests check pending identity/clearing and no-op behavior; the native callback chain passes the C3 frame check. This adds fixed owner state, no heap allocation. The delegate's durable receipt format, recovery validation and native activity integration remain unfinished.

The native application receipt codec is a fixed 152-byte `TAP` version-1 record: four-byte magic/version, event origin (16) and epoch/sequence (8 each), course (16), storage generation (16), resource digest (32), native local journal entry (12), before/after item states (16 each), full response milliseconds (4), and CRC-32 (4). Multibyte fields use little endian. It validates nonzero ownership, matching item UIDs, native state structure, review format/timing quantization and resulting review day, or supported undo/flag controls. Unknown versions, lengths, invalid bindings, corrupt CRC and overlapping input/output are rejected before output publication. The decoder keeps a bounded temporary candidate; no heap allocation is introduced, and encode/decode pass the native C3 256-byte frame check.

`protocol/fixtures/TintaApplicationReceipt-v1.fixture` is independently encoded with Python struct/zlib and compared byte-for-byte by native tests. Tests also exercise every-byte corruption, truncation, unaligned buffers, a valid-CRC zero generation and valid-wire destination overlap. The receipt is a candidate acknowledgement contract, not journal authority: durable storage/recovery must still verify the referenced event/body and actual derived files before trusting it. HAL receipt storage and native recovery integration remain unfinished.

`HalTintaApplicationReceiptStore` publishes immutable application receipts through
borrowed companion-child destination and stage paths. The owner retains fixed
encoding/readback buffers and must be allocated off the task stack. Publication
preserves conflicting or corrupt destination/stage records, re-syncs a matching
stage before rename, and resolves a lost rename reply through destination readback.
An identical published receipt requires no write or rename. Callers must exclude
other mutation writers and choose paths bound to the active learner/card context.
The store does not establish authority or verify that native derived files match
an event; those recovery checks remain required before activity integration.
Host fault coverage is in `HalTintaJournalStorageTest`; physical SD power-loss
verification remains required.

`verifyTintaReviewApplicationReceipt` binds a review receipt to its authoritative
SyncEvent and decoded body using caller-computed body/configuration digests. It
checks event identity, course, generation, resource, UID, study day, grade, format,
and unquantized response duration, then applies the shared native review scheduler
to the receipt's before-state and requires exact equality with its after-state.
This check does not prove the before-state's ancestry or current native file
contents. Undo and flag receipts use their dedicated authority validators.

`verifyTintaFlagsApplicationReceipt` requires the complete ordered authority batch
for the receipt's changed flags. It validates every envelope with independently
computed body digests, binds ownership/day/UID and enabled values, and ensures all
non-flag item fields are unchanged. Two-event changes require consecutive identities
from the same origin/epoch, the second event's sole ancestor to be the first, and
matching timestamp/clock quality. The receipt identifies the final batch event.
This still requires a proven before-state and native-file verification; undo
application validation uses its dedicated target-review check.


`verifyTintaUndoApplicationReceipt` validates the undo envelope and its target
review receipt against independently computed body/configuration digests. It
requires matching course/generation/resource/UID, explicit target ancestry, the
undo before-state to equal the verified review after-state, and the restored state
to equal that review's before-state. Recovery must separately prove target
availability and undo eligibility through intervening frozen journal history,
and verify local item/header/statistics files before trusting acknowledgement.

`ProgressStore::verifyCommittedMutation` is a read-only check of the latest disk
mutation after normal local recovery. It uses the production header decoder and
selection rules, requires the selected header/counters to match the open store,
checks exact item and journal extents, compares the last native journal entry,
and checks both the pending image and actual item record against the acknowledged
after-state. Reviews additionally require the saved undo before-image. The native
mutation binding runs this check before invoking its acknowledgement delegate;
failure stops the authoritative writer. This check does not prove prior authority,
intervening undo eligibility, or other learner files. Host tests cover altered and
torn headers/items/journal bytes, trailing journal bytes, review/undo/flag changes,
and read-time card removal without mutating files.

Application receipt addresses hash a canonical 100-byte tuple: `TAA` plus byte-1
version, event origin (16 bytes), little-endian epoch/sequence (8 bytes each),
course (16), storage generation (16), and resource digest (32). Published files
are companion-directory children named `tinta-applied-` plus the lowercase hex
SHA-256; their stages append `.tmp`. Mutable receipt state is excluded from the
address so different acknowledgements for the same event conflict.
`HalTintaApplicationReceipts` retains path/encoding/readback buffers off-stack,
uses the journal storage's checked digest provider, and validates full requested
ownership after reading an addressed receipt. Separate events and ownership
contexts retain separate receipts. Garbage collection/retention and full recovery
integration remain required; callers must not treat path lookup as authority proof.

`HalTintaApplicationAcknowledge` composes live native disk verification, frozen
journal record validation, review/flag/undo authority checks, and addressed durable
receipt publication. It requires the acknowledged identity to be the current
journal frontier. For undo it finds the earlier target event and loads that
review's ownership-bound receipt before verifying the exact restored transition.
All event/body/receipt buffers remain in the checked off-stack owner. Its callback
matches the native binding's acknowledgement delegate. The driver must keep all
borrowed stores alive, exclude concurrent writers, establish authoritative history
before binding, and stop further mutations when acknowledgement returns false.
This owner does not perform startup recovery or prove the legacy baseline.

`App::setLearnerPreparation` installs a borrowed startup callback before opening
the app; attempts to install it after the app is open fail. For durable storage,
App invokes the callback after loading the profile and opening the pack, before
allocating progress slots, opening progress/mark stores, or starting the learner
UI. Callback failure aborts startup through the existing course cleanup path.
Scheduler configuration is refreshed from the prepared profile before progress
opens. Guest startup skips the callback. This hook provides the lifecycle point
for native lesson/profile restoration and journal binding; the activity still
needs the concrete recovery driver and retained checked owners.
On hardware, verify that a preparation failure shows the activity's startup error
without entering learner screens, and that restored profile preferences configure
the first review. A strict whole-App frame probe currently reports an existing
416-byte SDK `layoutText` frame; the modified App methods produce no frame warning.

`HalTintaNativeDerivedPreparation` is a retained off-stack startup workspace for
an already published canonical generation. It loads the course receipt, requires
matching card/pack/frontier and a journal-proof callback (compatible with
`HalTintaDerivedJournalProof::callback`), then verifies all five source files'
hashes and formats before native restoration. It restores reading/star mark
snapshots and finally persists lesson projection into the already loaded profile.
The canonical item, empty local-review, and day files are already installed in
native-compatible form and are validated before progress opens. A missing receipt
returns `NoReceipt` for explicit legacy migration; it does not authorize a new
baseline. Repeated successful preparation is write-free. The caller keeps proof
context and validated pack/catalog alive and excludes writers throughout startup.
This driver does not handle native mutations beyond the canonical receipt;
incremental authority/application recovery and activity wiring remain required.

Native clock startup includes `ProgressStore::lastStudyDay()` when determining the
latest known study day. Canonical companion snapshots retain that day in the
selected item header while installing an empty `reviews.log`; an absent/empty or
unreadable local log no longer discards the snapshot day. If a local final journal
entry exists, startup uses the later of its day and the selected header day. On
hardware, install a snapshot dated later than the RTC and verify Tinta asks for a
valid date before scheduling or journaling a review. Host coverage checks the
Apple-produced snapshot day and rejection of an RTC older than the known day.

`HalTintaIncrementalRecovery` audits the current journal against the validated
course catalog and compares its frontier with an existing ownership-bound baseline.
It refuses pending publication intents and requires durable checkpoint retention proof
before replacing a changed baseline; current causal closure alone cannot prove
that older authority was retained. With that proof, it replays authority, exports
all five candidate files, advances the snapshot revision using a fresh snapshot ID,
and publishes through the existing journal-proof transaction. Export failure leaves
installed state intact. The export day includes the latest course event day, even
when the caller's known day is older. `Unchanged` means only that authority has not
advanced; callers still require native preparation and file verification. Missing
receipts require explicit legacy migration. The retained driver uses checked
phase-specific audit/replay/export allocations, releasing them before publication.
Session-long mutation binding and legacy migration remain unfinished.

`streamJournalPrefixFrontier` filters the audited current identity index to records
below a durable baseline count, retaining canonical identity order even when new
origins sort ahead of older events. It streams the same frontier encoding used by
complete journals, with bounded caller-owned scratch and no event vector.
`HalJournalCausalAuditSession::prefixFrontier` reopens the audited index, hashes
that prefix, closes both readers, and changes its output only after successful
hashing and closes. This is the retention primitive used by the durable manifest-bound checkpoint
proof.

`TintaAuthorityCheckpoint` defines an 80-byte CRC-protected checkpoint binding
the complete baseline manifest SHA-256, baseline frontier, physical record count,
and record size. Verification requires an independently computed manifest digest
and audited prefix frontier; it rejects missing records and record-size downgrades.
The codec uses bounded caller-owned storage and preserves output on invalid input.
An independent binary fixture and corruption tests cover its wire format. Its storage and publication integration are described below.

`HalTintaAuthorityCheckpointStore` publishes immutable checkpoint records through
checked staging, sync, close, and readback. A lost rename reply is resolved by
reading the destination; conflicting or corrupt records are preserved. Identical
retries leave the published file unchanged. Its fixed buffers belong to a retained
off-stack owner, with no allocation during publication. Host SD-fault coverage and
an ESP32-C3 compile probe verify publication behavior and the 256-byte frame limit.
The manifest-addressed owner and retention helper bind this store to publication
and incremental recovery.

`HalTintaAuthorityCheckpoints` addresses each immutable checkpoint by the complete
baseline manifest digest, using fixed ASCII paths under the companion directory.
Load verifies that the decoded manifest digest matches the requested address and
preserves caller output on missing, invalid, or misaddressed records. Distinct
manifest checkpoints coexist; neither path selection nor publication allocates.
Host tests cover separate manifests and valid records stored at the wrong address.
The retained owner passes the ESP32-C3 256-byte frame compile check.

`HalTintaAuthorityRetention` establishes a manifest-bound checkpoint only when
a fresh catalog-aware journal audit exactly matches the baseline frontier. It
proves retention by loading that immutable checkpoint, auditing current authority
against the expected current frontier, and hashing the checkpoint-sized physical
prefix in canonical identity order. Missing checkpoints, changed baseline
authority, stale expected frontiers, and read errors fail closed. The audit owner
uses a checked phase allocation because its retained scratch exceeds the stack
budget, and is released before checkpoint publication. Host coverage exercises
a real replay baseline before and after an appended event; the native compile
probe checks the 256-byte frame limit. Canonical activity startup wiring is described below.

`publishProvenTintaDerived` durably establishes the manifest checkpoint before
creating the publication intent, then releases the checkpoint/audit owners before
allocating journal proof and publication owners. `HalTintaIncrementalRecovery`
requires the concrete retained-prefix proof before rebuilding changed authority;
an optional policy callback can veto but cannot bypass that proof. For unchanged
authority it establishes or verifies the matching checkpoint, permitting old
receipts to adopt the proof only while their frontier still matches. Missing
checkpoints after authority advances refuse replacement. Rebuilt snapshots receive
their own checkpoint through the same publication entry point. Host coverage
checks that an approving callback cannot bypass a missing checkpoint.

`TintaActivity` now installs a startup-only `HalTintaLearnerPreparation` for bound
courses. With an authoritative receipt, it verifies the installed course binding
and full pack/catalog, provisions the card identity, runs concrete incremental
recovery, and prepares verified native readings/stars/lesson profile before App
opens progress or marks. Checked phase owners are released before the learner UI
runs. App consumes and clears the borrowed preparation callback at course-open entry,
including pack-open failure paths, so releasing the context leaves no dangling callback. Missing receipts retain
the existing separate legacy path; legacy migration and session-long mutation
journal binding are still unfinished. Guest startup skips durable preparation.
The helper passes the ESP32-C3 256-byte frame compile probe; physical startup heap
and failure-lifecycle acceptance still require device verification.

Retention regression coverage also replaces the journal with a valid empty
history and with a different, independently auditable event at the baseline
identity. Both fail checkpoint proof even when the replacement journal passes
causal and catalog validation. Restoring retained authority succeeds. The native
activity preparation implementation builds successfully for the standard C3
target; this is compile/link evidence, not physical heap or power-cut acceptance.

Pending-session reconciliation uses both the authoritative snapshot binding and
the local review-log count. Full physical acceptance remains required for changed
snapshots, unchanged resume, and sleep during date confirmation.

Native session format version 3 now appends the complete verified baseline
manifest SHA-256 before the outer CRC. The learner startup helper reloads and
hashes the prepared receipt, then binds App before resume metadata is read.
SessionController rebuilds the pending queue if that binding changes even when
both local review logs are empty. An unchanged binding preserves normal resume;
legacy version 1/2 metadata under canonical authority triggers rebuilding. An
unbound legacy learner continues writing version 2. Version 3 without verified
canonical authority is not resumed. While awaiting date confirmation, an old
unreconciled queue is saved without a binding until it has actually been rebuilt,
so a second power cycle cannot incorrectly label it current.

The shared production session decoder checks versions, screen bounds, lengths,
nonzero snapshot identity, and CRC while allowing trailing bytes from in-place
rewrites. Host coverage verifies changed/unchanged hashes with the same zero local
journal count, legacy adoption, truncation, every-byte corruption, and unchanged
output on failure. App, SessionController, and activity compile on C3; full firmware
and physical date-confirmation/reconciliation acceptance remain pending. The
fixed App session buffer grows by 32 bytes and retains a 32-byte snapshot digest
plus flags; no per-save allocation or extra framebuffer is introduced.

Session serialization explicitly reserves its 32-byte hash trailer. Startup
rechecks the reloaded receipt against course, storage generation, pack hash, and
audited frontier before hashing it as the session binding. Both affected native
translation units compile; the hardware checklist includes unchanged resume,
changed authority at zero local review count, and two power cycles while date
confirmation is pending.

A valid saved queue whose items were all retired by a content update now reaches
the snapshot rebuild check instead of returning early as empty. Corrupt or
expired queue blobs remain rejected. The host DayQueue regression retires every
saved item, verifies successful restoration into an empty queue, and builds from
the current catalog; the affected native SessionController compiles on C3.

`HalTintaMutationBindings` now initializes portable preference capture from the recovered native profile and attaches the App profile-save callback before the other mutation hooks. It shares the authoritative writer with reviews and marks. Its fixed capture buffers remain in the retained binding owner; no extra allocation is introduced. Keep the owner bound through `App::close()` so final dirty settings are journaled, then unbind before destroying App. Binding failure leaves the other hooks detached. The native profile-context runtime test and an ESP32-C3 compile probe of actual bind/unbind pass, including the 256-byte frame check. The activity still does not allocate this owner, so this does not establish live preference synchronization. Verify that integration by changing a portable preference, closing Tinta, and checking its durable preference event before reopening; device-only settings must emit none.

`ProgressStore::loadUndoReview` exposes the exact latest review and before/after item states only after checking the native header, journal extent/entry, and item bytes. `HalTintaApplicationAcknowledge::recoverUndo` scans the retained journal and accepts exactly one valid application receipt matching that native proof, course, storage generation, and pack hash. Missing proof, changed bytes, and ambiguous equal receipts preserve the caller output and refuse recovery. An absent native undo produces an empty identity. Both operations reuse retained state and fixed small local buffers without allocation. Host checks cover reopening and native corruption; the C3 recovery compile probe passes the 256-byte frame limit. Activity integration must invoke this before constructing the mutation bindings rather than guessing the last review identity.

Canonical `TintaActivity` sessions now retain `TintaCompanionSession`: the validated preparation/catalog, authoritative writer, receipt stores, acknowledgement owner, lesson keys, and mutation bindings. Reviews, undo, flags, marks, lessons, and portable profile saves use the shared writer. Checked session/binding allocations hold buffers and handles off the small task stack and add no per-mutation allocation. App closes while callbacks remain alive; bindings detach and journal handles close before App is destroyed. Startup allocation/proof failures refuse the learner; mutation errors stop the writer. Reopening a store within this live owner refuses recovery and requires exiting/re-entering Tinta so full canonical startup recovery runs. Legacy sessions without a canonical receipt retain the existing path until migration is implemented. The activity heap log now runs after binding, so hardware acceptance must measure the actual retained workspace. Verify exact undo across restart, a dirty preference on exit, SD failure after authoritative append, repeated enter/exit heap recovery, and at least 50 KiB free internal heap. These physical checks remain pending and the history capability stays disabled.

Canonical learner preparation now audits and causally resolves Tinta portable preferences before publishing the session snapshot. Native preparation workspaces are released before allocating the temporary audit/resolver owners. Conflicting or unreadable history refuses startup; resolved values apply as one checked profile replacement, preserving device-only fields, and unchanged values cause no writes. App configures its scheduler/UI from this prepared profile, then live preference capture initializes from the same values without re-emitting remote edits. The combined actual HAL journal resolution/native profile test passes persistence failure, retry, reopen, unchanged retry, and device-only preservation. The startup C3 compile probe passes the 256-byte frame check. Conflict presentation in the companion and physical two-reader preference acceptance remain required.

Connect startup now completes pending canonical publication, reconciles received merges, then reconciles locally appended Tinta events before enabling radios. `HalTintaJournalMergeCommitContext::reconcileLocalHistory` validates course binding, installed pack and subject catalog, generates a fresh snapshot identity, and invokes retained-authority incremental recovery. Its checked temporary owner avoids large stack buffers. This updates the baseline to the same journal frontier later required by merge commit admission, so offline native edits do not leave a stale baseline blocking synchronization. The actual course-transfer integration test appends an offline event, verifies revision advancement and subsequent merge admission, and rejects a foreign course without writes; all 46 course-transfer tests pass. Physical interrupted publication and offline edit → Connect → merge checks remain pending.

Missing canonical receipts no longer automatically admit legacy startup when the authoritative journal contains learning events for that course. `allowLegacyTintaWithoutReceipt` checks journal-directory absence without creating files, then audits any existing journal and queries exact course membership. Both learner startup and Connect local-history reconciliation use this gate. Matching learning history, invalid journals, or lookup/read/close failures refuse admission; empty history and unrelated courses remain eligible for the separate legacy migration path. Lookup/audit workspaces use checked temporary owners and allocate nothing per record. The native compile probe passes the 256-byte frame limit; 49 HAL journal tests cover admission and preserved query output. This guard cannot replace the still-pending initial legacy baseline workflow or physical SD-loss acceptance.

### Initial migration admission record

`TintaMigrationAdmission-v1.fixture` independently defines the 256-byte `TMA1`
record shared by C++ and Swift. It binds a reviewed immutable reader backup to
one complete journal merge declaration. The record alone does not authorize a
migration: authenticated ownership, verified backup/native-source hashes,
course compatibility, journal retention, and recoverable publication remain
required before reader installation. No capability is enabled by this codec.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | `TMA` and version 1 |
| 4 | 136 | Existing version-1 journal merge declaration, including its CRC |
| 140 | 16 | Stable course identity |
| 156 | 32 | Installed pack SHA-256 |
| 188 | 16 | Immutable backup transaction identity |
| 204 | 16 | Backup reader identity |
| 220 | 32 | Complete backup manifest SHA-256 |
| 252 | 4 | Little-endian CRC32 over preceding bytes |

All identities/hashes must be nonzero and the merge must add history. Decoders
require exact framing and both checksums. C++ preserves outputs on failure and
rejects input/output overlap without allocating memory; its native compile probe
passes the 256-byte frame limit. The reader controller, shared command dispatch
and initial baseline publisher consume this record; Apple orchestration remains pending.

`HalTintaMigrationAdmissionStore` durably publishes the migration record through a sealed stage, sync/close, rename, and exact readback. Valid staged decisions survive a failed sync or lost rename acknowledgement; unchanged published retries perform no writes. Conflicting/corrupt published or staged records remain untouched, and failed loads preserve the caller output. Its retained buffers and decoder state exceed the task stack budget; use a checked off-stack owner with borrowed companion-child paths. Publication itself allocates no memory. All 50 HAL journal tests pass, including store recovery/preservation cases, and the C3 compile probe passes the 256-byte frame limit. The controller combines this store with native backup/source verification and journal checks; storage publication alone does not authorize installation.

`HalLegacyTintaBackupSession::matchesCurrentCourse` separately verifies that current native course files still match the saved immutable backup. It checks every supported role, including absence of optional files, exact lengths, SHA-256, and checked file closes. Recovered backups can remain valid after originals disappear; that does not authorize migration of current learner state. The proof requires course-based preparation and frozen native writers, reuses retained file/hash scratch, and keeps the 436-byte manifest in the checked session owner so hashing cannot overwrite its borrowed view. No extra allocation occurs during verification. All 51 HAL journal tests pass for changed content/length, missing and newly created files, read/close errors, and unchanged sources; the C3 compile probe passes the 256-byte frame limit. Migration admission dispatch requires this proof and the complete manifest digest through `verifyMigrationBackup`.

`HalLegacyTintaBackupSession::verifyMigrationBackup` now binds current-source verification to the migration declaration: exact backup reader, storage generation, course, backup transaction, and SHA-256 of the complete verified manifest. It reuses the retained manifest and hash scratch with no extra allocation. All 52 HAL journal tests pass, including each foreign binding and a current native edit that leaves the backup intact; the C3 compile probe passes the 256-byte frame limit. This is the backup portion of admission, not authorization by itself. The caller must still authenticate the companion owner, verify installed pack compatibility and journal retention, freeze writers, and durably publish the matching admission before migration installation.

`HalTintaMigrationAdmissions` addresses each decision by SHA-256 of its 16-byte course identity followed by the complete 136-byte merge declaration. The ASCII target/stage paths therefore bind generation, companion owner, transaction, and both journal frontiers/counts/formats. Loading checks decoded course and complete merge ownership again before publishing output; misplaced valid records remain conflicts, not adopted authority. The checked off-stack owner retains paths, address scratch, and loaded state, reusing its durable record store. All 53 HAL journal tests pass, including independent address-hash verification, separated transactions, wrong ownership, unchanged outputs, and invalid admission requests. Migration commit validation and initial baseline recovery consume these addressed records.

`HalTintaJournalMergeCommitContext::admitMigration` composes the reader-side first-migration admission checks. It binds the authenticated companion owner and current reader/card generation, verifies the exact installed pack hash and catalog, requires the canonical receipt to be genuinely absent, audits the exact previous journal frontier/count/format, and rejects pre-existing learning history for this course. It then recovers the reviewed immutable backup, proves current native files and the complete manifest digest still match, and publishes the addressed admission. Checked audit and backup owners are released between phases to avoid retaining both workspaces; allocation failures refuse admission. The course-transfer integration test covers successful publication/retry and foreign owner, changed pack, changed frontier, and post-review native edits. Apple migration orchestration and physical acceptance remain required before migration is enabled.


`HalTintaInitialMigrationReconciliation` publishes the first canonical snapshot when merged-history reconciliation finds no receipt. It requires the exact addressed admission, current reader/card and installed pack binding, and unchanged reviewed native sources. A full course-membership/causal audit proves the merged frontier/count/format and retained old prefix; target-course events must bind the admitted pack while retaining their source storage generations. Admission and publication bind the receiving reader/card generation. Replay derives the latest study day and publishes revision 1 through the existing checkpoint-backed recoverable transaction. Audit, backup and admission owners are allocated once per phase and released before replay/publication; fixed scratch remains in the checked off-stack owner. The course-transfer integration test rejects missing admission and changed native sources, injects a rename that succeeds before reporting failure, resumes publication from its durable intent, verifies the canonical frontier/day/star state, and checks the immutable backup remains intact. It exercises both learning-history and preference-only migrations through actual reconciliation and startup recovery, then verifies repeated reconciliation preserves files and identity counters. All 47 course-transfer tests pass and the isolated C3 compile check passes the 256-byte frame limit. The Apple migration workflow and physical interrupted-SD acceptance remain pending.


The shared Connect exchange dispatcher accepts `TMA1` before a journal receive session starts, after backup export closes, and while no content transfer is active. `migrationAdmissionReply` decodes into the checked context owner, validates and persists the admission, and returns 24 bytes: version 1 at byte 0, `TintaJournalResult` at byte 1, zero reserved bytes 2–3, merge transaction at bytes 4–19, and declared previous count at bytes 20–23. Malformed requests produce no reply body; foreign owner/reader/card binding returns Conflict, failed verification/publication returns Invalid, and Ok requires durable admission. Merge commit validation can load the exact admission and rerun backup/source and old-journal checks when no canonical baseline exists. The incoming catalog validation and normal recoverable journal publication still apply.

BLE and encrypted Wi-Fi share this dispatcher. Wi-Fi additionally requires the outer handoff transaction to match the inner merge transaction, authenticated installation and card generation; its bounded validator checks the entire admission encoding without allocating a full record. Apple `admitTintaMigration` methods enforce reader/owner/card binding before sending and strictly validate reply framing, request ID, transaction and old count. The encrypted handoff test rejects foreign readers before sending and through direct bound exchange. Protocol tests cover every-byte admission corruption, valid-checksum missing fields, immutable reply outputs on malformed commands, and changed native files at commit. Imported events retain their originating card generation; the combined-history test verifies a foreign source generation remains acceptable. The durable Apple migration runner calls admission before merge begin and preserves jobs; the review UI now prepares independent reviewed migrations and exposes installation, pause and resume. Confirmed identical shared backups reuse canonical history; complete physical acceptance remains pending.


### Durable Apple migration jobs

Library schema 34 retains a `TintaMigrationJob` and immutable append payloads in SQLite. Queue admission requires the hash-verified native backup manifest, a completed local migration or confirmed shared-history decision, an exact installed-course inventory, the exported reader baseline, and known local events whose combined count/frontier matches the declaration. The source backup and incoming history remain immutable; source event generations are preserved. One active migration may own a reader/card, and migration and derived-snapshot queues exclude each other in both directions. An identical transaction retries its saved job even if a later export advanced the baseline; a new transaction cannot adopt a stale baseline. Phase/count updates compare the complete saved job to prevent stale owners from overwriting progress.

`TintaMigrationRunner` persists admitting before sending `TMA1`, transferring after successful admission, and committing before commit dispatch. It reconstructs candidate state with merge begin on every transfer/commit resume, checks the returned count against its acknowledged prefix, and sends only remaining immutable events. It marks completion only after a transaction-bound successful/duplicate commit reply with the declared merged count. Transport errors and cancellation pause the same phase; a restarted runner resumes without re-admitting a started candidate. Successful completion advances the last-known reader baseline only when it still matches the job's old baseline, preserving any newer export. Completed retries perform no network exchanges. BLE uses the authenticated reader session; Wi-Fi adds the same lease transaction binding and authenticated encryption as the existing transfer path. Jobs contain identities and immutable declarations, not pairing credentials.

The five migration-runner tests cover lost admission/append/commit acknowledgements with reopened SQLite and recreated owners, exact event delivery without duplication, repeated completed jobs, altered backup binding and delivery order, overlapping jobs, wrong installation before any send, stale phase updates, stale baseline refusal, atomic rollback after an injected event-row write failure, schema-33 upgrade preserving backups/history, and reciprocal snapshot exclusion. The complete 379-test Swift suite passes, including earlier schema-13/25 rollback/upgrade fixtures. Explicit user abort handling, native Apple builds and physical BLE/Wi-Fi/app-termination acceptance remain pending.

### Audited active journal snapshot

`JournalStateRequest-v1.fixture` and `JournalStateReply-v1.fixture` define an authenticated ExchangeChanges query used before migration queueing. The 20-byte request is `JST` followed by version byte 1 and the current 16-byte storage generation. The 44-byte reply is `JSS` followed by version byte 1, little-endian count at bytes 4–7, record size at 8–9, zero reserved bytes at 10–11, and full frontier SHA-256 at 12–43. The state decoder accepts only bounded 512/1024-byte journal formats and a nonzero frontier. Framing and authentication use the existing control envelope; Wi-Fi also binds the outer handoff transaction and requested card generation.

`HalJournalStateQuery` opens and recovers the active journal using the same persistence rules as export, audits causal/undo closure and frontier, and returns the actual count and record size. It can initialize an absent empty journal; it does not infer active state from backup header formats. Connect releases an existing export owner before allocating the checked audit workspace, rejects concurrent receiving/recovery, and preserves reply bytes when validation or I/O fails. The query and Wi-Fi validator pass the C3 256-byte frame check. Tests distinguish active 512/1024-byte journals from backups in the other format, compare the empty frontier to the independent fixture, verify an append advances count and frontier, and reject foreign-card, read and close errors. All 54 HAL journal and eight Wi-Fi protocol tests pass; C3 and S3 firmware builds pass.

Apple `journalState` methods support authenticated BLE and encrypted Wi-Fi. `JournalExportReceipt` now exposes its exact collected immutable mutations alongside count/frontier so migration preparation can retain the reader prefix without guessing membership from the combined library. The receipt reuses the collected Swift array; consumers must compare the separately audited snapshot's count/frontier against that prefix before queueing. Unsupported state-query replies require upgrading the reader; never substitute a guessed record size. The 25 relevant Apple tests pass, covering shared fixtures, both record formats, malformed/correlated replies, exact exported mutations, encrypted query and rejected foreign-card requests. The independent reviewed-migration screen now uses this query for queueing; physical acceptance remains unfinished.

### Reviewed migration controls

The Apple review screen prepares a reader installation from the latest completed native backup export, fresh inventory, exact exported journal prefix and separately audited active-journal snapshot. Preparation rejects mismatched count/frontier before importing or queueing history, retains the native export transaction and hashes its complete wire manifest for admission. It collects selected-course events and their required causal ancestry; required foreign-course history is refused rather than dropped. The saved job freezes admission and delivery order for subsequent retries. Review editing and local import are disabled once a job exists; Pause remains available during installation.

Six migration tests pass, including verified-export preparation and changed-snapshot refusal. SwiftUI parsing succeeds on Linux; native Apple compilation and runtime remain unverified. The current UI supports independent reviewed backups and confirmed identical shared backups over BLE. Multi-course causal dependencies and explicit abort remain unfinished; compatible cross-pack resources and Wi-Fi selection now have implementation and host coverage, with physical verification outstanding. Verify on Apple hardware by preserving a current backup, saving the review, installing, pausing during transfer, restarting the app and resuming the same saved job; confirm the original backup remains intact and the reader baseline advances only after commit acknowledgement.

Confirmed identical shared backups now prepare reader jobs using their validated canonical installation, without reserving a new migration origin or adding events. The shared-history review screen shows the canonical backup and offers installation without an independent draft. Existing saved jobs can resume using their frozen declaration even if the review cannot reload. Seven migration tests pass; the shared-backup case verifies unchanged event membership, canonical delivery identities and the second reader's native backup proof. Ambiguous shared prefixes still require further reconciliation and are not treated as identical histories.

The review screen verifies the source backup manifest and enables installation only for its exact reader/card generation. Its refresh identity includes both fields. A connection change clears the displayed job and confirmation, cancels active migration work, and reloads durable status after the task exits. The model refuses to return an old connection's completion as the current connection's success. SwiftUI parsing and string-catalog JSON validation pass; verify these controls on Apple hardware by disconnecting during installation and connecting a different reader or replacing the card, then reconnecting the original reader and resuming its saved job.

Reader migration preparation now checks cancellation, nonzero owner/transaction, the exact installed course inventory, stored reader prefix, current exported baseline and pending migration/snapshot exclusion before importing local history. Queue publication retains its transactional rechecks after asynchronous work. The migration tests reject malformed owners and missing installed courses alongside the changed-snapshot case; all 384 CompanionKit tests pass. No firmware source changed in this step.

The Apple migration model now attempts the configured Wi-Fi handoff for more than 1 MiB of remaining journal records, with the lease bound to the frozen migration transaction. It joins through the existing hotspot/manual-network flow, runs the encrypted migration transport, finishes the lease and reauthenticates BLE before returning success. Unsupported/unavailable handoff replies fall back to BLE while BLE remains ready. Failed handoff cleanup clears the connection so the saved job can resume after reconnect. SwiftUI parsing passes. Native preparation and activation now also accept a receiver-bound journal candidate with more than 1 MiB remaining; physical recovery verification remains required before claiming migration over Wi-Fi works end to end.

Journal handoff now has a lease primitive accepting a caller-validated receiver merge declaration and durable count. It rejects foreign owner/card/transaction, malformed declarations, counts before the old prefix and completed candidates, then uses the same authenticated token, expiry, network credential ownership and activation lifecycle as content leases. This adds no heap allocation. Seven lease tests pass. The native activity must still supply its receiver binding; the primitive alone does not authorize an unaudited wire declaration.

`TintaMigrationRunner.prepareForHandoff` durably admits and begins a job over authenticated BLE, validates the reader count and stops before appending. The app calls it before requesting a migration Wi-Fi lease, and finishes already-complete candidates over BLE. Failure pauses the latest persisted job phase even if preparation advanced before an acknowledgement was lost. The new test proves admission/Begin-only preparation followed by resume without repeated admission or duplicate event delivery. All 385 CompanionKit tests pass, and SwiftUI parsing passes. Native journal activity handoff integration and physical verification remain pending.

Native handoff preparation and activation now validate the merge receiver's retained declaration, authenticated owner, current card generation, exact transaction and durable count. Journal handoff requires more than 1 MiB of remaining fixed-size records. The receiver exposes a borrowed declaration only after successful Begin and before completion/abort/close; no new heap allocation is introduced. Activation rechecks the binding before stopping BLE, closes journal workspaces before enabling Wi-Fi and refuses radio startup if that close fails. Encrypted Wi-Fi Begin reopens the same durable candidate. Content leases retain their existing path. All 54 actual HAL journal tests and the C3 activity 256-byte frame probe pass. Final integration C3/S3 builds are tracked separately from the earlier lease-only builds.

Verify on Apple/C3/S3 hardware with more than 1 MiB remaining history: prepare over BLE, negotiate the configured network, resume candidate over encrypted Wi-Fi, interrupt during append/commit and reconnect over BLE to resume the frozen job. Change reader/card/owner/transaction before activation and confirm refusal. Check free and largest heap at phase transitions and confirm journal owners close before radio startup; the required physical acceptance remains outstanding.

Nine migration runner tests now pass. The added handoff-preparation recovery case loses admission and Begin replies independently, reopens SQLite and recreates the runner, checks the saved admitting/transferring phase is paused, then resumes and commits without repeating learning events. Lost admission can repeat immutable admission; a begun candidate is never re-admitted. The full 385-test suite passed before this test-only addition.

Compatible cross-pack migration now retains original event resource hashes. Apple queue validation uses logical course association claims; native initial reconciliation uses the installed catalog for stable subject membership. Event resources and identities are never rewritten to match a new installed pack.

Final native journal-handoff firmware builds pass for C3 (`default`, 101.66 seconds) and S3 (`sticky`); the build process returned success after both sequential targets. Physical BLE/Wi-Fi migration and Apple native compilation remain unverified.

Cross-pack migration queueing now checks every incoming learning event's logical course and existing local/cloud/history resource association rather than requiring its hash to match the installed pack. The installed pack admission remains bound to its exact hash and logical course. Native first-baseline reconciliation removes only the source-event hash equality restriction; its prior full causal audit, installed-catalog subject checks, retained prefix proof and canonical replay remain. No new native heap allocation is added. The source-hash test retains a distinct hash and rejects conflicting reassociation; the native first-baseline test uses a foreign source generation and distinct resource while reconciling a valid stable item. All 387 CompanionKit tests, 47 native course tests and the C3 activity frame probe pass. Final C3/S3 builds are running for this last firmware edit.

Verify on hardware by migrating histories produced under two compatible versions of the same course, checking that the saved events keep their source hashes and that replayed items/flags remain correct. A changed logical course or removed/incompatible stable item must still be refused. Multi-course causal dependencies, ambiguous shared prefixes and physical acceptance remain unfinished.

Preparing an independently reviewed backup now reuses its existing migration reservation origin when present. The current authenticated Apple installation remains the reader job owner; saved events retain the import's origin/epoch/sequence. Reconstruction still passes through exact draft validation, immutable resource/configuration reservation checks, canonical replay and installation digest verification. The reservation query closes before asynchronous reconstruction and its transactions. The preparation test imports under one origin and queues under another owner, asserting unchanged event membership and exact canonical delivery identities. All 387 CompanionKit tests pass. Verify with a backup imported on one Apple installation and a reader job prepared on another: no new learning events should appear, and the reader must authenticate the new job owner.

Final cross-pack firmware builds pass for C3 (`default`, 102.61 seconds) and S3 (`sticky`, 108.96 seconds). This covers the last native reconciliation change. Physical Apple/reader verification remains outstanding.

The native merge-receiver test now repeats an acknowledged abort through a recreated receiver, proving lost-reply retry returns success without changing storage. A foreign owner is refused against the retained terminal receipt; no active binding is resurrected. All 54 HAL journal tests pass. Apple explicit abort is still unfinished: it must persist intent before dispatch, retain frozen declarations on transport failure, retire the active job only after authenticated abort acknowledgement and keep abort distinct from completed installation. Committing jobs require resolving commit outcome because a published merge cannot be rolled back by abort.

### Durable Apple migration abort

Schema 35 adds a separate abort state to frozen migration jobs: none, requested, completed. Requested aborts remain active and paused; installation and handoff preparation refuse them. Terminal aborts leave the original install phase and acknowledged count intact, retain immutable event delivery and backup proof, and release the reader's active-job slot without advancing its journal baseline. The active index excludes acknowledged terminal aborts. Updates use whole-job compare-and-swap, so stale state cannot retire an abort. Schema-34 upgrade preserves jobs and delivery rows with abort state defaulting to none.

`TintaMigrationRunner.abort` checks authenticated reader/card/owner, persists abort intent before dispatching the frozen declaration and records terminal abort only after a correlated successful reply. Lost replies preserve requested state for restart retry; repeated acknowledged aborts perform no network exchange. Completed installations cannot enter abort. Committing jobs persist a recovery request and check the reader outcome before deciding whether to confirm completion or abort an unpublished candidate. Thirteen migration tests cover restart, retained events/baseline, stale state, active-slot reuse, committing refusal and schema upgrade; all 390 CompanionKit tests pass. The native repeated-abort receipt test already passes. Apple UI abort/retry controls are wired; physical acceptance remains unfinished. No firmware source changed in this step.

The migration review UI now presents confirmed cancellation, retry for persisted requested aborts, and separate pending/terminal cancellation status. Only queued/admitting/transferring jobs can be cancelled; committing jobs instruct the user to resume and resolve the reader result. Cancellation uses the current authenticated reader/card/owner binding, shares the activity's cancellable task ownership and leaves requested state for retry after disconnect. Starting again after an acknowledged abort ignores the old terminal job and prepares a fresh transaction from the saved review/shared history, backup proof and current reader prefix. Preserved history and backup objects are retained. All migration control strings are present in the localization catalog, SwiftUI parsing passes and whitespace checks pass. Native Apple compilation and UI/runtime acceptance remain unverified; the last CompanionKit backend suite passed all 390 tests.

Verify on Apple hardware by pausing an in-progress installation, cancelling it with the source reader connected, disconnecting before the reply and reopening the review to retry cancellation. Confirm cancellation is distinct from installation success, retained backups/history remain available, an acknowledged cancelled job permits a fresh installation, and committing jobs offer resume instead of cancellation.

Reader migration preparation now seeds existing portable Tinta preference events alongside selected-course learning events and retains their required ancestry. These are already journalled preference choices, not automatically imported fields from an unconfirmed backup. Unrelated reader preference keys are not selected except when causal closure requires them. Required foreign-course learning ancestors remain explicitly refused pending multi-course support. Fourteen migration tests and all 391 CompanionKit tests pass, including exact delivery of a confirmed Tinta preference. No firmware source changed.

Empty legacy learning histories remain an unfinished migration case: the vault's learning plan can contain no events, while admission requires a nonempty incoming candidate. A preference-only candidate is supported by native reconciliation, but backup preference confirmation/import and a verified no-review end-to-end preparation test still need implementation. Preserve the native profile and do not fabricate learning events or rewrite existing identities to bypass this gap.

An actual empty-learning backup fixture now verifies the preference-only preparation path. Its review journal is empty, lesson progress is zero and starred marks are empty; the local legacy learning migration creates zero learning events. After explicitly journalled portable profile choices, reader preparation freezes nine preference events, and the migration runner completes that candidate. Event count remains nine, all are preferences and the original backup remains verifiable. All fifteen migration tests pass; the full 391-test suite passed before this test-only addition. No production or firmware source changed.

The user-facing backup preference confirmation/import flow is still required. It must preserve the allowlist, distinguish imported profile choices from current preference conflicts, bind confirmation to the exact verified backup and avoid duplicate events on retry. The test demonstrates the backend path with confirmed choices; it does not prove UI availability or physical profile application.

### Confirmed backup preference import

Schema 36 retains a preference-import receipt for each exact backup. `importLegacyPreferences` verifies the saved native profile, requires the displayed confirmation values to match its nine portable fields and checks registered reader/card/course binding. It atomically journals the choices and stores their exact payload and resulting identities. Repeat import under another Apple owner verifies every receipt-linked event's kind, scope and body, then returns the original mutations without allocating new identities. Existing preference conflicts are preserved for explicit resolution; device-only profile fields are excluded.

The empty-learning migration fixture now uses this importer to create its preference-only candidate. Fault injection into receipt persistence rolls back all new events, and changed confirmation values are refused. All 393 tests in the full CompanionKit suite pass; an additional schema-35 upgrade test passes and preserves existing backup/review/preference history. Older schema upgrade fixtures drop the new table when constructing their earlier-version state. No firmware source changed. User-facing backup preference display/confirmation/import is wired; physical profile application acceptance remains unfinished.

The preserved-backup list and migration review now link to the backup preference screen. It displays the verified profile's nine portable choices using shared preference presentation, asks for explicit confirmation of those displayed values and calls the receipt-backed importer. Conflicting saved choices remain available through the linked portable-preference resolution screen. Import operates locally without requiring a connected reader. The migration screen explains that preferences must be imported before preparing a new installation; already-frozen jobs retain their original delivery. No device clock or device-only fields are imported.

SwiftUI parsing, localization catalog coverage for the new preference/migration controls and whitespace checks pass. The backend's full 393-test suite plus the added schema-35 upgrade test passed earlier; this step changes only app presentation and wiring. Native Apple compilation and UI/runtime behavior remain unverified. Verify by reviewing a backup's displayed values, confirming import twice without duplicate events, resolving a conflicting value explicitly and preparing a preference-only reader installation from an empty learning backup.

Confirmed identical shared backups now use the validated canonical backup's preference-import receipt. Import verifies both profiles yield the same confirmed portable values and rechecks the shared association after asynchronous vault access, so a newly changed association cannot publish a separate receipt. Canonical and shared imports return the same event identities under different Apple owners. Confirmation of shared history after a separate source preference import is refused as already prepared; existing events and receipts remain intact rather than being silently relabelled. The overlap UI explanation now includes learning reservations and preference imports.

All 394 CompanionKit tests pass. The shared-reader test verifies preference identity reuse, unchanged event membership, combined learning/preference delivery, and refusal of late shared confirmation without changing retained events. SwiftUI parsing and whitespace checks pass. No firmware source changed. Verify on Apple/reader hardware by confirming shared history first, importing preferences from either backup, repeating under another Apple installation and checking that only the canonical preference identities are delivered. Late independent imports still require explicit reconciliation; this guard preserves them.

New Apple migration preparation now refuses unresolved portable Tinta preferences before importing/queueing learning history. Queueing independently reconciles the exact previous-plus-incoming candidate and refuses conflicting Tinta values before durable job publication. The UI identifies this failure and links to explicit portable preference resolution instead of suggesting only reconnect. The resolution test preserves both original candidates and the user's resolution event, then verifies the selected value is the candidate's resolved result.

All 395 CompanionKit tests pass, SwiftUI parsing passes and whitespace checks pass. No firmware source changed. Native candidate pre-publication preference conflict validation remains required, including coverage for older frozen jobs and non-Apple clients; this Apple preflight does not prove that native guard. Verify the app flow by importing different backup values from two readers, attempting preparation, explicitly resolving the indicated conflict, then retrying preparation with the complete causal history retained.

Native candidate Commit now audits the exact candidate frontier/count/record size and resolves portable Tinta preferences before authorizing journal publication. Conflicting choices return Conflict while preserving the active journal and retained candidate. The guard uses checked off-stack audit and resolution owners because both exceed the 256-byte local budget; their scope ends before publication workspace allocation. OOM/IO/audit failures refuse publication. Begin remains available for incomplete candidate transfer and resume.

All 55 actual HAL journal tests pass, including conflicting preferences followed by safe candidate abort with unchanged active events. All 47 course integration tests pass after rebuilding a target that had compiled during the temporary Begin placement. The corrected C3 build and C3 activity frame probe pass; the final S3 build is still running. Physical heap/latency checks remain required. Older frozen Apple jobs that already reached committing need an explicit rejected-commit outcome path before they can be safely cancelled/reprepared; native refusal is implemented, but that app recovery path is not yet complete.

Committing Apple migration jobs now support outcome-aware cancellation recovery. After durable request persistence, the runner sends transaction-bound Begin and requires the full declared count. A duplicate completed receipt leads to Commit acknowledgement and atomic installation completion, preserving any newer exported baseline. An unpublished candidate is sent Abort and retired only after success. Conflicts, malformed counts and lost replies keep the recovery request pending; no generic conflict is treated as proof of cancellation. Terminal abort retains the original committing phase/count but is distinguished by its abort state.

The review screen exposes Check result or cancel for committing jobs and explains that already committed history is retained. Tests cover rejected unpublished Commit versus published Commit with lost acknowledgement, proving the former is aborted with the old baseline preserved and the latter is confirmed without an Abort command. All 396 CompanionKit tests pass, SwiftUI parsing and whitespace checks pass. Final native preference-guard builds passed for C3 (103.74 seconds) and S3 (110.92 seconds). Physical Apple/reader outcome recovery remains unverified.


Ordinary journal uploads now have their own durable Apple jobs, separate from legacy migration admission. Schema 37 retains the exact 136-byte declaration and ordered append payloads, with one active ordinary merge per reader/card generation. Queueing verifies the complete previous-plus-incoming frontier, stored event identities and bodies, installed selected-course association and portable Tinta preference resolution. Existing-prefix events retain their original identities and resource hashes. Ordinary merge, legacy migration and derived installation queues exclude each other reciprocally.

The ordinary runner authenticates reader, generation and owner before dispatch, records each acknowledged count, pauses interrupted work, and resumes the same transaction with Begin before further append or Commit. Completion updates the old matching reader baseline atomically; a newer exported baseline is preserved. Completed retries send no traffic. Wi-Fi requests use the existing encrypted lease and independently validate declaration owner, generation and transaction before sending. Handoff preparation establishes the candidate over BLE before switching transports.

All 401 CompanionKit tests pass, including schema-36 upgrade, payload retention across reopen, lost Begin/Append/Commit acknowledgements, stale caller rejection, injected payload-write rollback, migration exclusion and foreign Wi-Fi lease binding rejection. No firmware source changed in this step. Verify with `swift test --package-path apple/CompanionKit` on a supported development host. Automatic ordinary merge planning, app integration, cancellation outcome recovery, multi-course causal dependencies and physical reader/Apple acceptance remain unfinished; these APIs alone do not establish an end-to-end ordinary synchronization flow.


Ordinary merge planning now verifies the exact exported prefix and baseline, selects installed-book and active-course events plus portable preferences, and retains the complete explicit/per-origin ancestry closure. Dependencies for uninstalled books remain in the authoritative journal; required unavailable course dependencies produce an explicit error rather than dropping events. Existing pending jobs retain their frozen transaction and payloads, and a fully synchronized prefix produces no job. Resolved font/dictionary selections require the corresponding installed content kind/hash before queue publication.

All 405 CompanionKit tests pass, including prefix retention, deterministic delivery order, pending-job reuse, a no-op after completion, explicit and implicit uninstalled-book ancestry, unavailable course dependency preservation and missing dictionary refusal without publishing a job. No firmware source changed. Verify with `swift test --package-path apple/CompanionKit`. App integration, native canonical-baseline readiness discovery, cancellation outcome recovery and support for unavailable multi-course learning dependencies remain incomplete; automatic planning is implemented, but automatic end-to-end reader synchronization is not yet established.


Native canonical-baseline readiness discovery is now implemented as an authenticated, read-only ExchangeChanges query. `JRD` version 1 carries 16-byte card generation at bytes 4–19, little-endian exported journal count at 20–23, record size at 24–25, zero reserved bytes at 26–27 and full frontier at 28–59. The eight-byte `JRR` version 1 reply has status at byte 4 and zero reserved bytes at 5–7: 0 Ready, 1 MigrationRequired, 2 Unavailable, 3 NoCourse. Shared request/Ready reply fixtures use the empty TJF1 frontier and 512-byte journal format. Existing control framing binds request IDs; Wi-Fi additionally binds the lease transaction and card generation.

The reader audits the exact requested active journal count/stride/frontier before checking its installed course. Ready reuses normal merge commit validation: installed pack binding/hash, canonical receipt generation/resource/frontier/revision, all five derived file receipts and full installed-pack catalog validation. A genuinely absent canonical receipt reports MigrationRequired; malformed/unreadable baseline state does not report Ready. A missing active course reports NoCourse. Allocation failures refuse the query. Audit and commit contexts use checked heap ownership because scratch, baseline bytes and handles exceed the stack budget; the audit is destroyed before pack validation. The query publishes neither a journal candidate nor learner state and does not replace validation at Commit.

All 408 CompanionKit tests, 47 HAL course integration tests and 9 request/codec tests pass. Integration coverage explicitly checks Ready, stale frontier, damaged installed pack, changed learner file, absent receipt and corrupt receipt. C3 and S3 builds pass (104.70 and 106.20 seconds), and the C3 activity 256-byte frame check passes. Apple APIs expose readiness on authenticated BLE and encrypted Wi-Fi with foreign-generation refusal before Wi-Fi dispatch. App integration, ordinary cancellation recovery, non-course reader synchronization, multi-course dependencies and physical heap/Apple/reader acceptance remain unfinished. Verify query outcomes against a canonical reader and a preserved legacy reader before enabling automatic ordinary uploads; do not infer canonical readiness from journal count alone.


The Apple Devices refresh action now runs ordinary history synchronization through `ReaderJournalSynchronizer`. A saved ordinary job is authenticated and resumed before export because native candidate ownership excludes export. Pending legacy migration or derived installation returns a distinct deferred result before network traffic. New work exports the complete journal, checks a fresh count/frontier snapshot, requests native canonical readiness, then queues the retained causal merge. Only Ready can create an upload; other statuses give recovery/migration guidance without publishing an ordinary job.

The app sends small jobs over authenticated BLE and offers the existing saved-network/hotspot handoff above the 1 MiB journal threshold. It establishes the candidate with Begin before handoff, retains the transaction/count across transports, restores and reauthenticates BLE after success, and checks reader/card/owner identity before refreshing inventory. The Bluetooth disconnect callback preserves the owning history task during a deliberate Wi-Fi switch. Pause or negotiation/join/transfer failure preserves the saved job and durable acknowledgements; committing retries continue to verify the transaction outcome. A failed radio restoration requires reconnect rather than reporting the reader connected. Localized status, Pause and preference-resolution navigation are wired into Devices.

Device capability bit 9 now advertises the readiness-backed ordinary upload workflow on Tinta builds with an available journal export owner. Export-only firmware continues to import history but shows an upgrade instruction before readiness queries or ordinary job publication; pending ordinary jobs also require the new capability before resume. Authenticated BLE readiness and ordinary runner entry points require both bits 3 and 9. This capability check does not turn a non-Ready native result into permission to upload.

All 416 CompanionKit tests pass. Coordinator coverage includes exact prepared payloads, saved-candidate recovery before export, wrong owner, stale exported snapshot, legacy/unavailable/no-course refusal, pending migration deferral, export-only firmware compatibility and a combined prepare/upload/re-export cycle with no new job after completion. The affected native record tests (19) and C3 activity frame probe pass. SwiftUI syntax parsing, catalog coverage for static localized strings and whitespace checks pass. Native Apple typechecking/UI/runtime tests, physical BLE/Wi-Fi joining and heap acceptance, ordinary outcome-aware cancellation, last-successful-sync metadata, non-course reader application and multi-course dependencies remain unfinished. Final capability builds pass for C3 (99.74 seconds) and S3 (100.39 seconds).


Ordinary history jobs now support outcome-aware cancellation. Schema 38 adds durable abort intent and terminal cancellation state while preserving the frozen declaration, append payloads, original phase and acknowledged count. Requested cancellation keeps the reader/card slot occupied and pauses uploads; completed cancellation releases that slot without advancing the reader baseline or deleting library history. Queue idempotence retains cancelled transactions instead of reusing their nonce. Normal run/handoff paths refuse pending cancellation, and coordinator refresh returns cancellation recovery before export.

The ordinary runner authenticates reader/card/owner before persisting intent. A committing job first sends Begin and requires the full declared count: a duplicate committed outcome is acknowledged through Commit and atomically marked completed, while an unpublished full candidate is aborted. A generic conflict or lost reply remains pending. Database failure during outcome completion rolls back the baseline and job transition together; retry confirms the already committed outcome without Abort. Repeated terminal cancellation or locally confirmed completion sends no traffic. Any newer exported baseline is preserved.

Native Abort before Begin now persists its terminal receipt after proving the candidate/checkpoints absent and closing session handles. This prevents a later Begin for that cancelled transaction, including after restart. Receipt write/sync failures return IO error and preserve the active journal; retries confirm the receipt, while a fresh transaction can begin normally. Repeated Abort verifies the existing receipt without rewriting it. Foreign ownership or unexpected candidate files remain protected by the existing refusal paths. This uses the existing checked session/receipt workspaces and allocates no new application buffer.

Devices now offers cancellation, retry cancellation and Check result or cancel for paused/committing history work. The prompt explains that already committed history and library history are retained. The action is bound to the current authenticated BLE reader/card/owner, excludes concurrent upload/transfer work, and is cancelled on disconnect. Dialogs are dismissed when reader/card binding changes; stale results do not update a new connection. Lost cancellation acknowledgement remains recoverable through refresh and retry.

All 422 CompanionKit tests, 56 HAL journal tests and 47 HAL course integration tests pass. Coverage includes schema-37 upgrade, lost Abort acknowledgement and restart, terminal retry without network, slot release, wrong owner before dispatch, conflict retaining intent, unpublished versus published Commit outcomes, completion database rollback/retry and cancellation deferral before export. Native tests cover Abort before Begin, write/sync failures, late Begin after restart and unchanged active events. C3/S3 builds pass (105.41/105.28 seconds), and the C3 activity 256-byte frame check, SwiftUI syntax parsing, static localized-key coverage and whitespace checks pass. Verify cancellation on a physical reader both before Begin and after a lost Commit acknowledgement; confirm preserved library history and a subsequent fresh upload. Native Apple UI/build tests, physical radio/heap acceptance, last-successful-sync metadata, non-course reader application and multi-course causal dependencies remain unfinished.


Saved reader sync timestamps now use a verified export/state/readiness checkpoint rather than connection or upload completion alone. The library validates the exact prefix against its persisted baseline and event bodies inside a transaction, refuses pending history/install/transfer/removal work, unfinished selected-content reconciliation, unresolved preferences and reading/bookmark conflicts, and checks for relevant events arriving after verification. Updates are monotonic. Refreshing the same card preserves its timestamp; replacing its generation clears the timestamp atomically with the saved descriptor update. Old-card checkpoints cannot update the replacement card.

After an ordinary upload, Devices performs one fresh inventory and journal verification using the current authenticated session, including the restored Bluetooth session after Wi-Fi handoff. Newly arriving history stays queued for the next sync. A refused completion check shows pending-content/conflict guidance and leaves the timestamp unchanged. This does not establish native application of generic reading state or multi-course history.

The full CompanionKit suite passed with 423 tests before the final added conflict test; all four saved-reader tests pass after that addition. Coverage includes persistence across reopen, monotonic dates, invalid times, missing baselines, selected content absent from the reader, deselection retaining the library copy, a preference arriving after verification, successful retry with its newly verified prefix, fully exported concurrent preferences, and card replacement. App syntax, localization coverage and whitespace checks pass; native Apple typechecking/runtime and physical reader verification remain outstanding. Verify on hardware by completing sync, disconnecting/reopening Devices, replacing the SD card, and introducing a pending content selection or concurrent preference before retrying. Remaining plan work includes native generic history emission/application, multi-course causal dependencies, dependency-aware removals and the documented Apple/hardware acceptance gates.


Native candidate Commit now audits the full portable preference allowlist before publication, using a bounded retained resolver for variable-length language/content selections as well as integer values. The resolver follows explicit causal ancestors and implicit per-origin predecessors, retains concurrent unequal values as conflicts, and returns no application bodies on conflict or IO failure. Its checked heap-owned workspace replaces the smaller Tinta-only candidate resolver and is released before publication; actual physical heap acceptance remains required. The Tinta profile application phase continues to use its smaller resolver.

The checked HAL audit exposes portable resolution after a successful authority/index audit and checks visit/index/journal closes. Tests verify unavailable-before-audit, output clearing on reuse/failure, conflicts, original active-journal preservation before publication, variable-length language resolution and maximum 69-byte dictionary selections. All 83 native journal tests, 58 HAL journal tests and 47 course integration tests pass; the C3 activity frame probe passes. Firmware validation after subsequent language API edits is pending.

The translation generator now exposes canonical BCP 47 tags in stable Language enum order. I18n::languageFromTag accepts bounded string views and case-insensitive exact tags, rejects unavailable/unsupported tags without changing its output, and searches only the languages compiled into the current build. The native CMake suite generates independent full-language and English-only tables under its build directory and runs five GoogleTest/CTest checks, including regional Portuguese distinctions, omitted-language refusal, non-null-terminated inputs and retained enum/tag metadata. These tests pass. This strict lookup and portable resolution are prerequisites; general settings application, dependency proof for selected fonts/dictionaries, local generic event emission/application, multi-course dependencies and Apple/physical acceptance remain unfinished.


The reader preference planner now maps keys 1–14 to a bounded portable-values snapshot, including character-spacing offset conversion and null-terminated 31-byte font/dictionary names. It checks the final font size after decoding all inputs, passes exact selected content hashes to dependency validators, rejects duplicate/invalid bodies, and exposes no plan after dependency failure. A separate application boundary requires a checked conditional replacement with a recoverable-persistence contract; it reports conflicts/IO errors and avoids redundant writes for unchanged values. The HAL implementation of that store contract and native font/dictionary dependency proof are not implemented yet, and this application is not wired into successful Commit acknowledgement or startup recovery.

All 86 native journal tests pass, including full allowlist mapping, final font-size checks independent of event order, exact font/dictionary hashes, maximum names, unavailable-content refusal, retry, failed replacement, conflict refusal, read failure and unchanged-write throttling. Five full/reduced-language CTest cases pass. The final affected C3 build passes in 94.45 seconds; S3 passes in 241.52 seconds and includes the latest reduced-language lookup. The earlier HAL/course results remain 58/47 passing and the C3 activity frame check passes. Physical heap/radio acceptance and native Apple runtime validation remain outstanding. These results prove the tested helpers and current firmware compile; they do not prove general preference application, reading/bookmark application or full plan completion.

Native settings adapters now capture and conditionally apply only portable fields under the settings mutex. Application requires a caller-supplied durable-authority proof and changes the active UI language only after successful conditional replacement. Nine full/reduced-language and adapter tests pass, using a settings stub for the adapter boundary; they do not establish actual settings persistence or recovery.

Installed font proof validates the registered regular face at the requested size, its complete format and hash, and checked file closes. Registry lookup accepts bounded string views without constructing a temporary string. Dictionary proof validates all installed members, computes their canonical archive hash through a read-only hash stage, and checks retained original archives and finalized bindings. Selection checks the visible root first and falls back to the hidden root only after checked absence; enumeration errors, close failures and ambiguous names refuse selection. NFC/SdFat collision checks reuse borrowed normalization banks without new allocation.

Twenty-four hash/font tests and 23 dictionary selection/proof tests pass. Dictionary coverage includes a transferred archive whose identity differs from the canonical member archive, pending replacement backups, corrupted retained originals, Unicode names, IO failure and retry, with installed files preserved. The shared dictionary hash stage also passes 47 course-transfer integration tests. Registered-font C3/S3 builds pass; the subsequent dictionary-selection C3/S3 builds also pass. Font/dictionary helper C3 frame probes pass. These dependency proofs are not yet composed into production generic preference Commit application or startup replay. Verify those eventual flows on hardware by selecting transferred content, interrupting replacement, retrying after restart and measuring free/largest heap and task stack watermarks.

Reader preference encoding and changed-value capture now cover all 14 general keys. Capture validates the complete candidate before writing any event, records changed keys through the shared causal writer, and avoids redundant writes. A partial batch stops the writer and requires replay before derived settings may be saved; a new capture must initialize from recovered state. Content identity changes and bounded variable-length language tags participate in comparison. All 90 journal tests pass, including encode/plan round trips, invalid native values, content hashes, causal ancestry, partial batch recovery and retry refusal. Standalone C3 encoding/capture frame probes pass.

CrossPointSettings now has an optional journal save binding and its own saveToFile boundary. The callback receives a portable snapshot under storeMutex and must not re-enter settings or invert the storage lock order. Failure prevents JSON serialization/write; the caller owns the binding lifetime and unbinds before destruction. The compiled C3 save frame is 176 bytes, the binding frame is 16 bytes, and the C3 firmware build passes. Production metadata capture, binding/bootstrap, journal replay and save-failure UI handling remain unfinished. The callback is not enabled by default, and its compile evidence does not establish runtime recovery. The subsequent S3 build also passes.


Native metadata now supplies canonical compiled language tags and checked installed font/dictionary identities. Selective change capture preflights only edited dependencies, so an unavailable unchanged selection does not prevent its removal or an unrelated preference edit. A changed SD font size records both family identity and size. Partial event batches stop capture until recovery. The short-lived native replay phase borrows transfer scratch, owns normalization banks off-stack, and allocates a checked dictionary decoder/window only when compressed content requires them. Three native metadata/replay integration tests pass; they use a settings stub and do not establish actual boot settings persistence.

Boot recovery now finishes pending journal abort/publication recovery before loading reader state and settings. Generic preference replay and local capture bindings remain unactivated; save-failure UI handling and writer knowledge joining must precede activation. A local writer currently attaches only the physical last record, which does not necessarily dominate all branches received in a merge.

JournalKnowledgeHeads streams every causal head from a frozen audited journal, marking explicit parents and implicit origin/epoch sequence predecessors in disposable SD storage. It has no fixed limit on independent heads and no heap allocation in the portable scanner. The HAL audit method allocates one checked off-stack owner for scanner/visit handles, releases all files with checked closes, and treats streamed identities as tentative until the method succeeds. All 95 core journal tests and 60 HAL journal tests pass, including six independent origins, implicit predecessors, empty authority, missing indexed dependencies, journal growth, visitor rejection, read/mark failures and checked close failure. The C3 scanner/HAL frame probe passes below 256 bytes and the C3 firmware build passes. The final S3 firmware build also passes. This scanner does not itself replace the writer's single-parent behavior; generic local capture needs a complete initial-knowledge snapshot before activation. Physical free/largest-heap, task-stack and radio acceptance remain outstanding.


Preference edits can now resolve an arbitrary number of audited initial knowledge heads using the same existing protocol as Apple LibraryStore.resolve: each envelope carries up to four heads and the requested preference value, while implicit per-origin sequence predecessors join the batches. These are actual preference edits, not neutral markers or fake reviews; no new event kind or wire schema is needed. The writer borrows a stable head source and allocates no buffers per batch. Read/write/count failures stop the writer, including after a partially committed batch, so recovery must precede another capture or derived settings save.

ReaderPreferenceChangeCapture optionally borrows that initial snapshot. Unchanged saves do not consume it; the first edited key covers all initial heads, subsequent keys and later edits follow the local causal sequence, and complete preflight still occurs before the first event. The native capture binding forwards the snapshot but remains unactivated. All 97 core journal tests pass, including a six-branch conflict resolved by two real value batches through capture, unchanged-save throttling, snapshot use only for the first edit, subsequent causal edits, and retained partial batches after snapshot IO failure. The new writer C3 frame probe passes. Final C3 and S3 firmware builds pass. Runtime lifecycle/merge suspension, startup/Commit replay and settings failure UI handling are still required before production activation. This preference batching does not solve causal ancestry for non-idempotent review events.


The HAL audit now exposes a borrowed SD-backed initial preference knowledge snapshot. Preparation audits causal parents, retains the validated sorted identity index and disposable parent marks, counts unmarked entries, and closes the live journal before returning. Sequential reads filter the frozen index without reopening the growing live journal; preference batches can append through their separate writer while the index/marks remain excluded from other builders. An audit, prefix-frontier request or export close on the same owner refuses while the snapshot is retained. The caller ends the snapshot only after releasing its capture binding; visit/index closes are checked, and destruction also releases it. One checked off-stack workspace holds fixed metadata and handles, with no RAM allocation proportional to history and no per-batch allocations.

All 63 HAL journal tests pass. Coverage includes six independent roots in reverse physical order, live two-batch preference resolution without index changes, implicit and explicit parent filtering, out-of-order/read failure refusal, unchanged output on failed reads, empty journals, mismatched source counts, conflicting audit calls, checked close failure and retry. The snapshot begin/read/end C3 frame probe passes below 256 bytes. Final C3 and S3 firmware builds and all three native metadata/replay integration tests pass. Runtime save-session composition, actual startup/Commit generic replay, complete settings failure handling and physical acceptance remain unfinished; the snapshot API alone does not activate synchronization.


Runtime reader preference capture and replay are now wired into firmware. After font setup, startup replays audited generic preferences before normal reading. Tinta and companion activities suspend local portable capture while their journal writers are active. Companion Commit runs generic replay after publication and learner reconciliation, and returns an unavailable result if dependencies, conflicts or IO prevent application. Companion teardown replays after closing transfer/journal sessions, refreshes dirty font discovery, adopts the resulting baseline and restores capture. Successful content transfers mark font discovery dirty so later preference commits in the same connection see new installs.

The settings save callback compares a fixed 77-byte baseline and allocates no proof workspace or identity for unchanged portable values. Changed values use a checked short-lived save session and 8 KiB scratch. Content banks and journal buffers stay off-stack; compressed dictionary resources remain lazy. Capture prepares every edited body once before opening the journal or reserving an epoch, retains the exact prepared snapshot, then records the real edit across all observed heads. This avoids reserving identities for invalid dependencies and releases proof/journal/decompression resources before JSON serialization. The fixed baseline and mutex remain resident; retaining large content/audit buffers globally was rejected because they would consume C3 RAM during ordinary reading.

Rejected journal saves restore only portable fields to the callback's rollback snapshot and refuse JSON writes. Device-specific fields stay in memory. JSON write failure after a durable event leaves that event authoritative for startup replay. The web settings endpoint reports HTTP 500 on failed save. A checked small error activity displays a translated message and dismisses with logical Back/Confirm; it retains the parent activity. Partial batches and failed journal/knowledge closes stop further portable capture until recovery.

All 98 core journal tests and five native metadata/save/replay integration tests pass. Tests prove preparation before writer start, exact candidate retention, one-shot prepared commit, six-branch save/replay, unchanged/invalid save avoidance of identity and journal writes, dependency refusal and existing conflict retention. Integration settings and identity stores are stubs; they do not prove actual JSON rollback, mutex behavior or physical UI/radio operation. Save-session/runtime C3 frame probes pass at the 256-byte ceiling; the compiled settings save frame is 256 bytes and binding is 16 bytes. HAL's manifest now declares its display/SD dependencies explicitly, resolving missing SDK includes in PlatformIO's chain scan. Final C3/S3 builds after the font-cache invalidation edit pass (default: 106.204 seconds; sticky: 100.800 seconds).

Verify on C3 and S3 by changing portable and device-only settings, restarting after a durable save, forcing a missing/corrupt font or dictionary dependency, interrupting a multi-key save, and installing another font after the first merge within one companion connection. Check that rejected portable fields roll back, device-specific values remain local, Commit is not acknowledged before successful application, and restart applies only audited authority. Monitor free/largest internal heap and task stack watermarks during changed saves, compressed dictionary proofs and repeated companion sessions; physical acceptance still requires over 50 KiB free heap and no accumulating loss.

Remaining work includes initial import of existing JSON preferences, explicit same-name font/dictionary content-hash refresh notifications, integration acceptance of actual settings rollback and concurrent writers, generic reading/bookmark mutation/application, no-course upload readiness, multi-course causal history, non-idempotent review knowledge joining, dependency removals, native Apple builds/runtime and the full physical acceptance matrix. This runtime implementation does not complete the companion plan.

Apple now permits audited NoCourse readiness to enter ordinary merge planning. The existing planner selects generic preferences and available-book events, and rejects unavailable course ancestors; firmware incoming validation accepts generic events with a null course/catalog and rejects new learner events without a matching catalog. Generic Commit already dispatches before the installed-course fallback, and post-publication reconciliation skips learner rebuild when the checked course lookup reports absence. No learner receipt or pack catalog is invented for a fresh reader. All nine ReaderJournalSynchronizer tests pass, including a no-course portable preference upload and continued migration/unavailable refusal. End-to-end physical publication, lost Commit reply recovery and unavailable learner ancestor coverage still require acceptance; this preparation test alone does not prove the complete path.

All 64 HAL journal tests pass after adding generic-only publication without a course/catalog. The new case begins from an empty active journal, publishes one portable margin event, discards the session as if its Commit reply were lost, reconnects with the same declaration, receives Duplicate without changing active journal bytes, and resolves the audited margin from the published journal. Existing learner tests separately reject null catalogs. These HAL storage stubs cover journal durability logic, not physical SD power-loss behavior or the complete activity/radio lifecycle.

The no-course Apple integration fixture now executes coordinator preparation, durable merge runner completion, re-export and a second audited readiness check. It reaches a verified checkpoint with one retained preference, no pending job and exactly one Begin/Append/Commit. All 43 synchronizer/migration runner tests pass, including existing unavailable-course ancestor refusal with local history retention. The transport is a deterministic fake; physical reader/iPhone/Mac acceptance remains outstanding.

Initial portable import preparation now supports an explicit fourteen-key selection without manufacturing a changed baseline. Ordinary change capture still permits only font/dictionary forced refresh bits; prepareSelected validates the complete mask, preflights each selected body before writer startup, and invalidates prepared state on rejection. Its caller must prove selected keys absent from audited authority; runtime importer composition is not yet wired. All 99 core journal tests pass, including unchanged selected-key emission and unknown-mask refusal. Final C3/S3 builds for this helper change are running.

PortablePreferenceResolution now exposes missingReaderKeys only after conflict-free audited resolution. Its fourteen-bit result excludes existing reader keys, ignores learner preference slots, and leaves caller output untouched before resolution or after conflict/IO failure. All 99 core tests pass with absent-key assertions around conflict, explicit resolution and injected IO failure. Initial-import runtime composition remains unfinished. The earlier selected-capture C3 build passes (97.273 seconds); its S3 build is still active, and both targets require a final check after this resolver edit.

NativeReaderPreferenceSaveSession now composes importMissing: checked heap resolution computes absent keys from conflict-free authority, releases resolution before selective content preflight, rechecks frontier/count/record size before identity reservation, and uses the existing checked knowledge-head commit path. No absent keys returns without writer startup. All six native metadata integration tests pass; the new test imports thirteen missing keys while retaining an existing margin, repeats import without reserving another identity, then replays the retained journal margin. Runtime startup wiring, conflict/dependency refusal import tests, C3 frame checks and final board builds remain required.

All seven native metadata integration tests now pass. Import refusal coverage checks independent margin conflict and missing selected SD font; both return before writer identity reservation, retain active journal bytes, and do not require writer recovery. The C3 importMissing compile/frame probe passes the 256-byte threshold. The earlier selected-capture board builds both pass (C3 97.273 seconds; S3 107.015 seconds). Final builds for resolver/import-session changes are running. Runtime startup import activation remains outstanding.

Runtime startup and companion teardown now import absent reader preference keys after successful replay when capture is being enabled. Commit replay passes enableCapture=false and never opens an import writer. The replay owner is destroyed before import allocation; the existing checked 8 KiB scratch is reused. Import conflict and unavailable dependency remain recoverable through explicit local repair, while failed writer/knowledge recovery stops capture. Actual C3 runtime compilation passes the 256-byte frame threshold. Seven native integration tests cover importer behavior; the real settings/runtime orchestration still requires device acceptance. The pre-wiring C3 build passes (104.571 seconds), its S3 build is active, and a final C3 build is required for runtime activation.

Runtime import recovery is tracked independently from the replay application result. A failed import no longer hides Applied from startup, which must still reload font/theme state after replay changes settings. Import allocation/IO/writer failures stop local capture until restoration; missing dependency/conflict refusal remains repairable, and all failures are logged. Earlier board builds pass (C3 104.571 seconds; S3 101.521 seconds); final C3/S3 builds after this correction are running. Verify on-device that replayed font changes load even when importing another missing key fails.

All eight native metadata integration tests pass with path-specific journal write fault injection. Failure on the second record leaves one durable imported key and requires recovery; reopening with the full record buffer and retrying imports only the thirteen remaining keys, retains the first event identity and resolves all fourteen keys without conflicts. This simulates a refused write in HAL stubs, not arbitrary physical power-loss timing. The shared HAL journal target is being rechecked after adding the fault hook; final firmware builds remain active.

The shared HAL journal target passes all 64 tests after the path-specific write fault hook. Final C3 firmware passes (99.605 seconds); final S3 remains linking. Current reading synchronization remains unactivated: EpubReaderUtils::saveProgress writes only the device-local atomic progress cache, and Epub exposes a local path rather than a verified edition digest. Reader, navigation-origin restore, KOReader and speed-reading callers all share that save helper. Activation must bind verified content hash to the open book, emit ancestry-aware spine/text-offset events before derived progress publication, debounce redundant saves, exclude writes during sync, and replay only conflict-free authoritative positions while preserving concurrent alternatives. Bookmark put/delete also requires stable identities and recoverable derived publication. Existing body validation alone does not implement these lifecycle requirements.

Final firmware validation for activated initial import passes on both targets: C3 default 99.605 seconds and S3 sticky 102.513 seconds. Core journal tests (99), HAL journal tests (64), native metadata/import/replay integration tests (8), and actual C3 import/runtime frame checks pass. Physical SD power-loss, heap/largest-block and stack acceptance remain outstanding. The next reader identity path can reuse hashInventoryFile, which streams SHA-256 through borrowed scratch, checks every read, yields between chunks and verifies final file size; the reader still needs lifetime/content-writer exclusion and checked retained-handle closure before treating that digest as edition authority.

The journal writer now accepts validated reading-position bodies with nonzero edition digest and an audited initial knowledge snapshot. Preference and position writes share the same idempotent multi-envelope causal joining mechanism; identical position bodies batch four heads per event and implicit writer sequence ancestry joins the batches. No new event kind or heap allocation is introduced. All 100 core journal tests pass, including six independent reading heads joined by two position records and zero edition digest refusal without append. Actual reader save hooks, verified book lifetime binding, progress replay/conflict UI and bookmark lifecycle remain unactivated. Final C3/S3 builds for the shared writer change are running.

Reading writer regression validation passes: 101 core journal tests, eight native preference/import/replay tests and 64 HAL journal tests. A failed fifth head read preserves the first durable four-parent position batch, stops the writer, refuses subsequent writes and reopens with that record intact. The C3 reading writer frame probe passes at the 256-byte threshold. Final firmware builds are still active. Physical conflict-resolution and progress publication remain unfinished.

NativeReadingPositionSaveSession composes validated position encoding, checked authority audit, identity reservation, SD-backed initial knowledge heads and checked journal/head closure. Its checked off-stack owner holds the journal workspace and an eight-byte anchor member; no per-head or per-envelope heap allocation is added. The caller must bind a verified frozen edition digest and exclude writers; only success permits derived progress publication. All ten native integration tests pass, including maximum anchor values, exact edition retention, one-shot session refusal and empty edition rejection before journal/identity writes. The reader save hook and replay/conflict UI remain unactivated.

Final shared-writer firmware builds pass: C3 default 107.399 seconds and S3 sticky 106.127 seconds. All eleven native integration tests pass after strengthening the reading-session case: six independent reading branches use the real SD-backed knowledge snapshot and append two exact-anchor batches with four/two explicit parents and one shared sequential writer epoch. Injected active-journal audit close failure returns IO, requires recovery and reserves no writer identity. The reading save owner passes the C3 frame probe. It remains a composed helper rather than an activated reader path; verified edition binding, capture throttling, lifecycle exclusion, progress application and conflict UI are still required.

The reading save owner now supports persistFile with a borrowed scratch span, retained mutex-wrapped book handle and streaming SHA-256. It rejects missing/empty/unreadable content before reserving writer identity, retains the file through journal publication, and checks book close before returning success. The reader loader remains responsible for EPUB validity and content writers must be excluded; retaining a handle alone does not freeze bytes. All twelve native tests pass, including exact content digest, missing-file refusal and failed book close after durable journal publication. This owner is not yet included by production reader code; runtime activation and digest caching/throttling remain unfinished.

Firmware ReadingPositionResolution now resolves one verified edition through validated journal ancestry and reusable visit marks. A backward pass follows explicit parents and implicit origin predecessors, retains concurrent distinct anchors as Conflict without changing caller output, and permits identical concurrent anchors. The six-head writer test now begins with distinct offsets, proves conflict retention, records a two-batch explicit position choice and resolves the chosen anchor. All 101 core journal tests and the C3 resolver frame probe pass. No heap allocation proportional to history is introduced; the event/validation owner must remain off-stack. HAL audit composition, progress cache application, display of concurrent alternatives and production reader save activation remain unfinished.

HalJournalCausalAuditSession now composes resolveReadingPosition using checked off-stack resolver/visit owners and the validated identity index. It consumes one successful audit, checks visit/index/journal closes, and publishes a candidate anchor only after all close checks succeed. Before-audit and failed-close paths leave caller output untouched. All twelve native integration tests and 64 HAL journal tests pass; the HAL resolver C3 frame probe passes the 256-byte threshold. Final C3/S3 firmware builds for the audit header integration are running. Production reader save/application and conflict display remain unfinished.

Existing reader progress publication now checks HalFile::sync before replacing the canonical cache instead of ignoring flush failure. All thirteen native integration tests pass; the new actual ProgressFile test proves failed staging sync preserves prior progress and a subsequent successful retry publishes replacement bytes. Scope-exit close and FAT remove/rename recovery limitations of that existing helper remain; this sync check alone does not provide complete journal replay publication guarantees. HAL-resolution C3 firmware passes (115.153 seconds), its S3 build is still active, and final checks must include the progress sync edit. The shared storage stub gained the actual write-open adapter needed to compile this production helper in host tests.

ProgressFile now uses a stack-owned staging object with a HalFile member, checking write, sync and close before removing old canonical progress. No new heap allocation is introduced by the staging owner. All fourteen native integration tests pass; sync and close fault cases retain canonical bytes and successful retries replace them. The FAT remove/rename interruption window and redundant string path construction remain, so complete journal replay publication still requires recovery/throttling work. HAL-resolution firmware builds pass (C3 115.153 seconds; S3 121.407 seconds). Final C3/S3 builds including the progress publication changes are running.

NativeReadingPositionReplay now composes audited edition-scoped causal resolution, a second frontier/count/record-size audit, workspace release and checked publisher acknowledgement. Conflict/unavailable/IO returns before publisher invocation; caller must verify edition/book bounds and exclude writers through publication. All fifteen native integration tests pass, including actual ProgressFile publication of a resolved spine/text anchor and sync failure preserving canonical progress. This coordinator is not yet called by production reader/activity code; loaded-book bounds, cache binding, unchanged-write throttling and concurrent-choice display remain required. Final progress firmware builds are still active.

All sixteen native integration tests pass. Reading replay refusal coverage proves empty history, distinct concurrent positions and journal read failure make zero publisher calls and retain canonical progress and active event bytes. C3 final progress firmware passes (111.125 seconds); S3 remains active. The replay helper is still unactivated in production, and neither these callback tests nor frame probes prove complete reader lifecycle exclusion, content immutability, bookmark behavior or physical conflict UI.

Reading replay now requires the loaded book spine count (Epub::getSpineItemsCount), rejects zero/counts outside the 16-bit anchor domain, and rejects resolved anchors at or beyond that count before publisher invocation. All sixteen native tests pass, including zero publisher calls and unchanged canonical progress for these bounds failures. Final progress-cache firmware builds pass: C3 default 111.125 seconds; S3 sticky 116.092 seconds. The bounded replay helper remains unactivated in production; actual edition/spine binding and visible-offset validation still belong to the loaded reader integration.

Reading saves now resolve audited current position before reserving writer identity. An identical conflict-free authoritative anchor returns success without journal append or identity calls; a real change refreshes authority after resolution before opening the writer. All sixteen native tests pass, including repeated six-head resolved position retaining eight records and unchanged identity-call count. This avoids redundant journal/NVS work but still audits and, on persistFile, hashes content; production caching and time-based debounce remain required before page-render activation.

Epub now exposes a companion-only book-lifetime cached content identity using borrowed scratch and the existing streaming hash helper. Cache validity is established only after nonempty file hash and checked retained-handle close, and cleared at the start of load(). Output remains unchanged on failed proof. Per-book DRAM adds a 32-byte digest, validity flag and HalFile member; no hash buffer or per-call heap allocation is introduced. The retained member permits checked close without local-file destructor-only acknowledgement. Caller must serialize cache access and exclude content writers for the loaded book lifetime; this is not a general file mutation watcher. Actual Epub.cpp C3 compilation passes with a 96-byte identity method frame. Full C3/S3 builds are running. Production progress capture/replay still needs this identity API wired with lifecycle exclusion and cache invalidation on content changes; physical cache behavior is unverified.

Production reading capture/replay is now wired. EpubReaderActivity restores audited positions after book/cache setup and before loading progress.bin. Known-text-offset saves from the shared EPUB helper use cached edition identity, the preference runtime mutex and the checked reading journal session before local cache publication. Replay and capture share suspension/failure exclusion with Tinta/companion/preference writers; proof scratch is released before journal owners, and session owners before cache serialization. Default save refuses concurrent reading conflicts; only an explicit resolveConflict choice permits joining them. Offset-less legacy saves remain device-local. Existing reader save-error presentation is requested on failed capture/replay. All sixteen host integration tests pass, but actual runtime/activity orchestration is not host-tested. Actual runtime C3 compile passes with frames: identity proof 48 bytes, publisher 32, restore 144, save 112. The pre-wiring identity C3 build passes (138.319 seconds); its S3 build remains active, and final builds must include runtime wiring. Concurrent-choice UI, time-based debounce, speed/KOReader restore lifecycle, bookmark mutation/application, physical heap/stack and full-plan acceptance remain unfinished.

The native six-branch reading fixture now uses distinct offsets and proves default capture Conflict leaves active bytes unchanged and reserves no identity. Only an explicit resolveConflict=true save publishes the two-batch choice, after which replay and unchanged-save dedup succeed. All sixteen native integration tests pass. Final production runtime C3/S3 builds remain active. These tests use settings/identity/SD stubs and do not establish actual reader thread, i18n/error rendering, physical storage or battery behavior.

Final production reading runtime C3 build passes (136.666 seconds); S3 remains linking. Apple reading-position explanation now describes compatible-reader history exchange, replacing the obsolete unavailable claim; its existing concurrent-choice UI still confirms every observed head. Swift frontend parsing and localization JSON validation pass, but no native UI build/accessibility acceptance is claimed. Speed reading has one production launch site in EpubReaderActivity and shares the same loaded Epub, so it inherits cached edition identity; KOReader creates its own object and proves identity on its known-offset save path. Physical multi-mode save/exclusion behavior still requires acceptance.

Production reading saves now bypass proof allocation, journal audit, identity reservation and SD writes for an exact previously successful ten-byte progress publication within the current runtime revision. Each Epub retains one fixed progress cache (ten bytes, revision, validity plus padding), with no new heap allocation or history-sized growth. Runtime restore and reading replay invalidate revisions; a successful journal save invalidates older revisions before cache publication, and only checked publication records the new cache token. Epub reload clears the cache. All seventeen native tests pass, including stale-revision, different bytes, malformed cache entry and clear behavior. This is exact-duplicate suppression, not time-based debounce of changed positions. Prior production runtime firmware builds pass (C3 136.666 seconds; S3 140.556 seconds); final builds for duplicate suppression are running. Physical cache invalidation across retained reader/KOReader/sync activity lifetimes remains unverified.

The final duplicate-suppression firmware builds pass for C3 (132.996 seconds) and S3 (147.845 seconds). Bookmark journal writing now supports stable-identity puts and explicit deletes through the shared causal-head joining path. A checked bookmark-body encoder writes into caller-owned storage, rejects invalid identity/text or insufficient capacity before touching output, and makes no heap allocation. All 103 core journal tests pass, including maximum-size put round-trip, deletion round-trip, invalid UTF-8, embedded NUL, zero identity and short-buffer checks. Firmware builds including the bookmark additions are running. These additions do not yet assign/persist native bookmark identities, migrate legacy bookmarks, emit reader mutations or apply merged bookmark state; that integration and physical acceptance remain required.

Native bookmark entries now retain an optional sixteen-byte stable identity. Bookmark JSON saves nonzero identities as canonical lowercase hexadecimal and loads legacy entries without an identity. Loading rejects malformed, null or duplicate identities; saving rejects duplicate nonzero identities before writing. Decoding uses the JSON string's explicit length, so embedded NUL cannot conceal trailing data. The codec uses a sixteen-byte candidate and a caller-owned thirty-three-byte encoding buffer, without a separate heap allocation. Pairwise duplicate checks use constant auxiliary space; JSON and vector allocations remain the existing persistence mechanism. All 104 core journal tests pass, including identity round-trip and output preservation for invalid inputs. JSON persistence integration, identity assignment/migration, journal capture, replay and device checks remain unfinished. Firmware builds are running; the first S3 attempt exposed an Arduino HEX macro collision, corrected by naming the constant HEX_DIGITS.

Six bookmark persistence tests now compile the actual BookmarkFile.cpp and BookmarkUtil.cpp with pinned ArduinoJson 7.4.2. They prove distinct IDs survive serialization and rename, legacy entries do not receive invented identities, malformed/duplicate IDs reject the whole load without altering stored fixture bytes, duplicate saves stop before the mocked writer, and read/write failures propagate. Array/object shape validation also rejects corrupt containers instead of treating them as an empty list. PersistableStore and HAL boundaries are mocked; these tests do not prove physical SD write atomicity, activity error handling or power-loss recovery. Prior identity firmware builds pass (C3 104.526 seconds, S3 108.779 seconds); final builds including all JSON loader checks are running. Identity assignment/migration, journal capture and merged-state replay remain required.

Final bookmark JSON firmware builds pass (C3 96.727 seconds, S3 100.917 seconds). BookmarkResolution now resolves one stable bookmark identity within one EPUB edition by walking audited causal ancestry. Concurrent distinct canonical bodies, including put versus delete, return Conflict without changing caller output; an explicit choice joining both heads resolves to its body. Explicit deletion remains a resolved result rather than absence. HalJournalCausalAuditSession exposes this through one checked, short-lived heap workspace containing the large event/body/visit state. Output is copied only after checked visit/index/journal closes. All 105 core journal and 65 HAL journal tests pass. An actual C3 compile with a 256-byte frame error limit passes: resolver run is 176 bytes, HAL resolveBookmark 96 bytes. Full firmware builds including this resolver are running. Identity assignment/migration, native save/application sessions, conflict presentation and device acceptance remain unfinished.

NativeBookmarkSaveSession now preflights a stable-identity bookmark body and verified edition before journal/identity activity, resolves current authority, skips exact unchanged saves, and refuses concurrent edits unless resolveConflict is explicit. Accepted edits join every audited head before derived bookmark publication is permitted. Its two bounded 668-byte body buffers are retained in the checked off-stack session owner; no history-sized allocation is introduced. All twenty native integration tests pass, including invalid-input no-I/O, put/delete persistence, unchanged retry without identity reservation, and six concurrent branches resolved only by an explicit two-envelope deletion choice. Actual C3 compilation with a 256-byte frame error limit passes. These are HAL/identity stub tests; the session is not yet wired into native bookmark UI mutations, and stable identity assignment/migration plus merged-state application remain required. Resolver firmware builds pass for C3 (113.862 seconds) and S3 (125.668 seconds). Native bookmark save session persist has a 224-byte C3 frame; it still requires production runtime wiring.

Production captureReaderBookmark now serializes the native save session through the same runtime mutex and suspension/recovery guards as reading progress and portable preferences. It verifies the edition through the Epub's cached content proof, validates put anchors against the loaded spine count, releases the checked journal session before caller JSON publication, and returns the actual journal result. Proof scratch is released before journal allocation. All twenty-one native integration tests pass, including audit-close failure preserving journal bytes and identity reservations. The actual runtime C3 compile passes with an 80-byte capture frame under the 256-byte limit. Runtime orchestration is not host-tested, and bookmark UI callers, identity assignment/migration and merged-state application remain unfinished. Full firmware builds including the runtime entry point are running.

Bookmark capture runtime firmware builds pass (C3 115.205 seconds, S3 117.279 seconds). NativeBookmarkReplay now resolves one known stable ID, validates put anchors against the loaded spine count, rechecks journal frontier/count/record size, and releases audit owners before a checked publisher callback. Explicit deletion is published as its canonical body. Missing/conflicted authority does not publish. restoreReaderBookmark supplies the verified edition and holds runtime writer exclusion through publication; publishers must not re-enter the runtime. All twenty-two native integration tests pass, covering put/delete publication, publisher failure, failed authority closes, missing state, out-of-book anchors and concurrent-choice publication gating. Actual C3 compilation passes under the 256-byte frame limit; replay run is 176 bytes. This is a single-ID primitive: full journal ID enumeration, JSON application, stable identity migration and bookmark UI capture still remain. Final firmware builds including the replay runtime are running.

Final bookmark replay runtime firmware builds pass (C3 109.724 seconds, S3 122.740 seconds). BookmarkIdentityCursor now enumerates distinct stable IDs for one edition, including deletion-only IDs, using fixed cursor state and at most 64 journal records per step. It returns Pending between slices, emits sorted IDs, filters other editions, leaves output untouched except on Found, and detects changed committed count/record size or corrupt reads. The caller must supply audited, frozen authority and yield between pending steps. It rescans per distinct ID, costing O(bookmarks × events), without retaining a heap-sized identity list. All 107 core journal tests pass. Actual C3 compilation of begin/step passes with a 256-byte frame error limit and a cursor-size assertion of at most 128 bytes. HAL/native enumeration ownership, full JSON application, identity migration and UI capture remain unfinished.

HalJournalCausalAuditSession now exposes audited bookmark ID enumeration through a staging-only visitor. It yields between bounded slices and selected IDs, consumes audit authorization, and checks journal/index closes before returning Ok; callers must discard staging on any failure. All 67 HAL tests pass, including sorted put/delete ID enumeration, yields, missing audit, visitor/read/close failures. The cursor lives in the existing audit-session owner, adding fixed bounded state without another heap allocation. Actual C3 stack usage is 192 bytes for bookmarkIdentities. An initial 272-byte stack implementation was replaced even though the compiler frame warning did not reject it; stack-usage output remains the stronger evidence. Firmware builds are currently running, but the owner layout changed during compilation, so fresh final incremental builds are required afterward. Native staging/publication, identity migration and UI capture remain unfinished.

Fresh final enumeration firmware builds pass (C3 114.466 seconds, S3 38.463 seconds). HalBookmarkIdentityStage now stores sorted, bounded IDs in a private SD spool using HalVerifiedFileStage and borrowed readback scratch. It protects pre-existing candidates, verifies writeback SHA-256 on seal, verifies streamed readback SHA-256 before End, and checks read closes and owned cleanup. Consumers must discard derived staging unless the spool reaches End; returned individual IDs are not publication permission. NativeBookmarkIdentityEnumeration binds enumeration to audited frontier/count/record size and produces the spool without a RAM-sized ID list. All 70 HAL and 24 native integration tests pass, including empty spool, foreign-file preservation, sorted/deduplicated bounds, read corruption, close/removal failure and authority binding. Actual C3 compilation under the 256-byte frame limit passes: spool next is 80 bytes, native prepare 128 bytes. This transient spool still needs an ownership record for safe post-crash reclamation; full-body staging, conflict-checked JSON publication, legacy identity migration and UI capture remain unfinished.

NativeBookmarkEditionStage now enumerates one edition's IDs, resolves every bookmark through checked replay into HalBookmarkBodyStage, validates put anchors, verifies complete ID readback/cleanup, seals body writeback, and rechecks authority before exposing staged bodies. A conflict or invalid anchor in a later ID discards earlier body staging; no canonical bookmark JSON is touched. Empty authority returns Unavailable, preventing an empty replay from erasing legacy local state. Body spool records are 670 bytes: a two-byte little-endian canonical-body length, its 18-byte deletion or 28–668-byte put body, and zero padding. Sorted unique IDs, valid UTF-8, length/padding, SHA-256 readback and checked close are required. Returned bodies borrow the fixed record buffer until the next read; consumers must stage JSON until verified End. The record buffer stays in the checked off-stack owner, and shared scratch is borrowed across sequential stages. All 27 native integration tests pass, covering complete put/delete staging, late conflict, out-of-book anchor, foreign candidate, absent authority and final readback corruption. Actual C3 compile passes with frames: edition prepare 112 bytes, body append 112 bytes, body next 96 bytes. This new preparation owner is not yet invoked by the runtime; canonical JSON publication, crash ownership/reclamation, identity migration and UI capture remain unfinished.

BookmarkJsonWriter now streams portable bookmark fields through a fixed 128-byte buffer, escapes JSON text, preserves valid UTF-8 and maximum numeric anchors, enforces sorted unique IDs, and omits explicit deletion from local JSON while the journal retains it. It allocates no JSON document or strings. HalBookmarkJsonStage connects the writer to verified private SD staging, requires the exact input record count (including deletions), and seals only after sync, SHA-256 readback and checked close. Its conservative byte limit is 4096 bytes per expected record plus 32 envelope bytes, covering the maximum 640 text bytes escaped sixfold and fixed fields. All 11 bookmark persistence tests pass with pinned ArduinoJson and the actual native loader, including escaped/maximal text, empty state, deletion omission, sink failure, incomplete record count, foreign candidate preservation and failed sync. Actual C3 compile passes under the 256-byte frame limit; JSON append is 144 bytes. These components do not yet compose edition staging with a recoverable canonical JSON transaction; crash ownership, legacy identity migration and reader/UI lifecycle wiring remain unfinished. Existing final firmware builds cover enumeration/runtime; these new headers are not yet reached by production callers.

NativeBookmarkJsonPreparation now composes edition resolution, complete verified body readback, exact-record JSON serialization, checked body cleanup, verified JSON seal and a final journal frontier/count/record-size recheck. Only the complete pipeline marks the candidate prepared; it retains content edition, candidate SHA/length and authority metadata for a later durable publication owner. All 28 native integration tests pass, including successful exact JSON output, untouched existing bookmark fixture bytes, foreign JSON candidate preservation, failed sync and corrupted SD writes. ID/body temporary spools are removed before handing off prepared JSON. Actual C3 compile under the 256-byte frame limit passes; preparation is 80 bytes. All fixed body/JSON buffers remain in the checked heap owner and use borrowed scratch, with no per-bookmark heap list. The caller must import legacy bookmarks before using this authoritative replacement pipeline. Publication still needs scoped transaction paths, durable ownership/recovery and backup verification; the existing Transfer install sequence provides that pattern but is not yet connected. Migration and production reader/UI wiring remain unfinished.

The portable BookmarkPublication state machine now binds candidate and original file hashes to transaction, storage generation, edition and journal frontier. A durable intent precedes replacement; a durable receipt precedes verified backup cleanup. Recovery refuses foreign files and rechecks storage context before mutations. All six host tests pass, including interruption/lost acknowledgements, stale authority, changed storage context, identical content and foreign backup preservation. This abstract publisher is not yet connected to HAL intent/receipt storage or reader UI, and physical power-loss recovery remains unverified.

Bookmark publication claims now have an explicit version-1 196-byte little-endian encoding with CRC32, strict flags/reserved bytes and semantic validation. Decoding rejects truncated/corrupt records without modifying its output; encoding rejects invalid claims before modifying the caller buffer. All eight publication host tests pass, including every single-byte corruption and checksummed invalid fields. The codec allocates no heap; HAL persistence and device acceptance remain unfinished.
