# Tinta event bodies and Apple replay

This is the version-one body schema implemented by the Apple shared core. Reader
emission and legacy migration are not enabled yet. Existing `reviews.log` remains
local; it must not be treated as a distributed journal without migration.

Each body starts with a version byte (`1`), the envelope event-kind byte, a nonzero
16-byte logical course identity, and a little-endian 32-bit subject UID. Subject UIDs
are stable course identities, never catalog offsets. Zero and `0xffffffff` are reserved.
Lesson and reading UIDs use their respective namespaces; pack/migration adapters must
supply stable identities before those events can be emitted.

| Kind | Bytes after the 22-byte common prefix | Exact body size |
| --- | --- | --- |
| Review (5) | Grade u8 (1–4), exercise format u8 (0–9), response milliseconds u32, scheduler configuration (6 bytes) | 34 |
| Undo review (6) | Review event identity: origin (16 bytes), epoch u64, sequence u64 | 54 |
| Suspension (7) | Enabled u8 (0 or 1) | 23 |
| Lesson complete (8) | Completed u8 (0 or 1) | 23 |
| Star (9) | Enabled u8 (0 or 1) | 23 |
| Reading complete (10) | Completed u8 (0 or 1) | 23 |

All integers are little-endian. Extra bytes, truncation, unknown versions, invalid
identities, and invalid enum values fail validation. The enclosing SHA-256 body hash
must match. Study days must fit the firmware's unsigned 16-bit day number. The resource
hash must be nonzero and identify the course edition used for the event.

Review scheduler configuration consists of schema version u8 (`1`), fixed rule/weight
set u8 (`1`), retention basis points u16 (1–9999), and maximum interval u16 (1–65535).
Rule set one uses the repository's existing FSRS-6 default weights and learning rules.
The envelope scheduler version is `1`, and its configuration digest is SHA-256 of
these six bytes. Control events use scheduler version zero and a zero digest. Future
weights/rules require a new supported rule set; replay must reject unsupported sets.

Undo names an exact review and includes that identity among its direct ancestors. The
target must be a review for the same course and item. Multiple undo deliveries are
idempotent. Replay excludes targeted reviews, then recomputes subsequent reviews with
the original C++ scheduler. Concurrent reviews from other origins remain effective.
Suspension and star events change only their own flag bit, preserving the leech bit
computed by the scheduler. Completion events update separate stable-identity sets.
Concurrent controls follow the deterministic causal ordering defined by `SyncHistory`.

`LibraryStore.importTintaEvents` validates body/envelope bindings before committing.
`replayTinta` loads and validates the durable journal, checks complete causal history,
and computes item states, completed lessons/readings, undone-review identities, and
per-course/day totals. Totals count a new item when its pre-review state is new, and
a review when a previously seen item's last study day differs from the event day,
matching `ProgressStore`'s local counting rule. Non-Tinta envelopes remain in the
causal graph but do not mutate learner state.

Daily totals also retain the number of effective graded reviews, correct answers
(all grades except Again), and exact response milliseconds in a 64-bit sum. These
are separate from the daily review-limit counter. Duplicate delivery and undone
reviews contribute nothing; conversion to the reader's seconds-based day log must
happen when publishing derived state, rather than rounding individual events.
`StudyTotals.readerDayTotals()` provides this checked conversion: reader reviews
come from graded reviews, time rounds once to the nearest second, and values beyond
the reader's 32-bit seconds field or inconsistent counters are rejected. This
conversion does not yet publish `days.bin` or install the other derived files.
`TintaDayLog.encode` builds a deterministic `TDL1` file from these checked daily
totals. It sorts days, splits each field into the reader's 16-bit record chunks,
and writes the low 16 bits of CRC-32 over each ten-byte record payload. Portable
tests cover an independently calculated checksum fixture, splitting, ordering,
empty files and unrepresentable time. The buffer is allocated in the Apple app;
reader-side validation and coordinated publication remain unfinished.

`CompanionTintaDayLog.h` now provides a fixed-size streaming validator for staged
canonical daily snapshots. It requires an exact header and complete record count,
validates every checksum, requires nondecreasing days, and rejects 32-bit aggregate
overflow or daily correct/new-item counts exceeding grades given. A validation
failure remains latched until a new `begin`. It allocates no heap memory. Host
tests share the Apple checksum fixture and exercise corruption, truncation,
ordering, inconsistent totals and overflow. The installer still needs to invoke
this validator and publish daily state together with the other derived files.

`HalTintaDayLogValidation.h` reads a borrowed open `HalFile` through the storage
mutex and feeds the validator one record at a time using twelve bytes of caller
scratch. It rewinds the handle, rejects short reads and changing extents, and
supports cancellation every 32 records. It retains no handles and allocates no
heap memory. The caller must exclude writers, verify the complete snapshot hash,
and close its handle at the intended release point. Two HAL host tests cover the
shared fixture, repeated validation, scratch bounds, corrupt/trailing bytes,
read failures and cancellation; firmware installation wiring is still pending.

`LegacyMarkLog.encodeSnapshot` encodes a canonical `TMK1` set with sorted keys,
one checked add record per key, and an explicit 96-key capacity check matching
the reader's `MarkLog`. It allocates its output in the Apple app. Tests verify an
independent CRC fixture, empty sets and the exact capacity boundary. Callers must
map completed-reading identities to the selected pack's legacy story keys before
encoding and must not silently combine ambiguous identities. Snapshot assembly,
reader validation and coordinated installation of mark files remain pending.

`LegacyMarkLog.encodeCompletedReadings` now maps stable reading identities through
the selected pack's verified metadata. It rejects unknown identities, mismatched
mapping lengths and duplicate stable identities. When multiple readings share a
legacy key, it emits that key only if every associated reading is complete; a
partial collision is rejected because `TMK1` cannot represent it faithfully. It
does not drop the authoritative completion events. Tests cover full, empty and
partial groups, independent readings, unknown identities and malformed mappings.
Supporting partial collisions on the reader requires a stable-identity completion
format and loader; this compatibility encoder is not that implementation.

`CompanionTintaStoryIdentity.h` implements the inspector's `TST1` identity
algorithm on the reader side with incremental title input, fixed scalar state
and no heap allocation. Independent fixtures verify kind and lesson separation
and every title chunk boundary. Invalid kinds and embedded NUL bytes latch a
failure; the caller must supply the complete checked title and detect duplicate
identities across the pack. The helper does not yet change live `storyKey`, read
marks or usage logs, which require a versioned migration before replacement.

`CompanionTintaPackStoryIdentity.h` connects this algorithm to `Pack`, resolves
lesson identities from unit/lesson numbers, and consumes titles through the new
checked `Pack::visitStr` API. Streaming uses a 32-byte stack buffer, no string
arena and no heap allocation; read or callback failure is reported explicitly.
Host tests compare every simulator-pack story with a full-title reference and
check unchanged output on invalid title offsets. Existing pack-source tests
also verify byte-exact streamed strings and callback rejection. Live completion
storage and migration still need to adopt stable identities.

The non-memory-source test now injects failure at every source read used to
derive each simulator story identity, including lesson/unit reads and title
chunks. Each failure preserves the caller's output; a fresh attempt computes
the complete original identity. The test runs without a string arena and checks
that arena usage and overflow counters remain zero. All nine targeted companion
codec/identity tests pass. This host coverage does not verify SD hardware faults
or implement completion-file migration.

### Stable completion snapshot format

The companion now has a `TCS1` completion-set codec for derived state. Its
twelve-byte header contains magic `TCS1`, kind u8 (1 lessons, 2 readings), three
zero reserved bytes and count u32 little-endian. It is followed by `count`
strictly increasing u32 stable identities and a u32 IEEE CRC-32 over all preceding
bytes. Zero and UINT32_MAX identities are invalid. Count is bounded to 65,535,
matching the supported pack section limits; exact file length is `16 + count*4`.
Empty sets are valid. Course identity, pack hash and journal frontier must be
bound by the enclosing installation transaction; this file is not standalone
proof of those bindings or of identity membership in a selected pack.

Apple's `TintaCompletionSet` encodes and strictly decodes the format. Reader
`TintaCompletionSetValidator` validates the header, then four-byte identity
chunks, then the checksum using only fixed scalar state. It rejects wrong kinds,
extent mismatches, unordered/duplicate keys, incomplete sets and checksum errors.
It adds no reader heap allocation. This supports sets beyond the legacy 96-key
capacity and partial legacy-title collisions without merging identities. Live
membership lookup, mutation persistence, migration and coordinated publication
remain required before the reader can consume these snapshots.

`HalTintaCompletionSetView` now validates a borrowed open completion file in
chunks of up to 32 identities, then queries membership with binary search. It
uses caller scratch for validation and four stack bytes per lookup, allocates
no heap memory and routes all reads/seeks through `HalFile`. The caller must
retain the handle, exclude writes/replacement, and establish course/frontier/hash
bindings. Extent changes and read failures invalidate the view; failed queries
leave the caller's result unchanged until revalidation. Four HAL tests cover
daily-log validation and completion views, including the shared checksum fixture,
wrong kind, corruption, cancellation and a 65,535-entry set with at most sixteen
identity reads per lookup. Live App integration and persistence remain pending.

`TintaSnapshot.derivedCourseFiles(course:studyDay:)` now assembles clean item state,
an empty local review log, both typed stable completion files and the daily log
for an explicit nonzero course identity and selected study day. It
filters foreign-course subjects, applies replayed completion removals, and emits
valid empty files when that course has no events. Stable completion identities
are retained without converting to legacy title keys. The mixed-course replay
test checks foreign exclusion, duplicate delivery, removal, file types and invalid
course rejection. Course/pack/frontier installation receipts, journal reconciliation and atomic
publication are still required, and the assembly API does not authorize a course
switch or installation by itself.

`LegacyItemStore.encodeSnapshot` writes sorted 16-byte records and two clean
checksummed `TIS1` headers, enforcing the reader's 32,767-record limit and checking
each dictionary UID against its item bytes. Headers contain no pending mutation
or local undo and set the local journal count to zero. The course assembler pairs
them with an empty local review log and uses saturated 16-bit daily-limit counts
for the explicitly selected study day. The authoritative companion journal is
separate and must be retained; installing `items.bin` beside an old local review
log would replay old mutations and is forbidden. Tests verify record ordering,
states, header counters, alternate-header recovery, invalid UID bindings, empty
files and mixed-course assembly. Installation and reader end-to-end validation
remain pending.

`protocol/fixtures/TintaItemSnapshot-v1.fixture` is now shared by the Apple item
encoder and the production C++ ProgressStore test. Swift must reproduce its exact
bytes. ProgressStore opens it alongside an empty local review log, checks both
fresh item states and the selected-day counters, then persists a subsequent
review and increments the daily new-item count. All eighteen journal tests pass.
This establishes format compatibility with the actual engine; it does not prove
multi-file installation, power-cut recovery or on-device execution.

### Derived installation manifest

`TintaDerivedCourseFiles.installationManifest` encodes a 332-byte `TDS1` receipt.
Offsets are: 4 course identity (16 bytes), 20 pack SHA-256 (32), 52 canonical
journal-frontier SHA-256 (32), 84 snapshot identity (16), 100 storage generation
(16), 116 study day u16, 118 zero reserved u16, 120 revision u64. Five forty-byte
receipts start at 128, each length u64 then SHA-256, ordered item state, empty local
review log, completed lessons, completed readings, daily log. A u32 IEEE CRC-32
over bytes 0–327 ends the file. All integers are little-endian. Identities and
digests must have their exact lengths and be nonzero; revision must be positive.
Study day comes from the assembled files, preventing a separate mismatched value.

The encoder hashes the actual immutable output files. Storage, pack and frontier
bindings remain caller inputs and must be independently verified by the reader;
this receipt is not authentication or proof of complete replay. File verification,
durable intent publication and recovery remain unfinished.

`TintaDerivedManifestView` now strictly decodes the receipt on the reader without
allocating or copying its full contents. It borrows immutable caller-owned bytes,
validates checksum, reserved fields, nonzero bindings, revision and supported
file extents, and exposes typed length/hash receipts. Its `matches` method checks
course, storage generation, pack and frontier together against expected values.
Failed decoding clears the view. The shared `TintaDerivedManifest-v1.fixture`
must match the Swift encoder exactly; C++ tests exercise every one-byte corruption,
every truncation, binding mismatch and invalid fields with recomputed checksums.
These checks still require independent file hashing, journal validation and an
installation transaction before the files become active.

`HalTintaDerivedFileVerification.h` now checks an open staged file against its
typed manifest receipt using the existing HAL SHA-256 stream. It rejects length
mismatches before reading, distinguishes receipt conflicts from I/O/cancellation
failure, and keeps the borrowed handle open. It allocates no buffer or handle;
scratch is caller-owned and must not alias the immutable manifest. Tests use the
shared manifest to verify daily-log and empty-review-log bytes, same-length
corruption, extent mismatch, read failure and retry. This establishes receipt
matching only; file-format checks and transaction binding/recovery remain separate
requirements before publication.

`HalTintaItemSnapshotValidation.h` validates the canonical clean item file
produced by the Apple encoder. It borrows the open HAL handle and eighty bytes
of caller scratch, requires both header CRCs and expected sequences, matching
study day/counters, zero local journal count and inactive pending/undo fields,
exact record extent, sorted unique UIDs and valid production ItemState decoding.
Read failures, cancellation and changed extents fail validation. It adds no heap
allocation and leaves the handle open. Host tests use the shared item fixture
and reject wrong days, corrupt/pending headers, duplicate UIDs, trailing bytes
and source read failure before successful retry. This validator intentionally
does not accept every recoverable local ProgressStore file; installation still
must verify receipts, authoritative replay and publish the entire generation.

`HalTintaDerivedGenerationValidation` now combines manifest binding, SHA-256
receipts and typed file-format validation in one fixed-size session. `begin`
requires expected course, storage, pack and frontier bindings. Sequential borrowed
handles let the caller reuse one handle and scratch; `complete` becomes true only
after all five file kinds pass. Any failure clears all progress and requires a
new `begin`. No reader heap allocation is introduced. Tests verify a complete
shared-fixture generation, incomplete progress, rejection/restart, wrong frontier,
and malformed daily data whose SHA-256 receipt has been recomputed to match it.
The latter ensures receipt matching cannot bypass format validation. The caller
must keep candidate files and manifest immutable through publication. Pack
membership, complete authoritative replay, durable intent and startup recovery
remain required before installation can be enabled.

### Derived generation publication state machine

`CompanionTintaDerivedPublication.h` now coordinates publication through an
explicit storage interface. Fresh publication requires valid bindings, all
validated candidates and no ambiguous backups. A durable intent precedes any
rename. Existing active files move to owned backups, candidates become active,
and every active receipt is rechecked before the durable commit receipt. Only
then are backups removed and the intent cleared. Recovery uses the same manifest
and rolls forward; conflicting/missing candidates cannot be guessed or overwritten.
A matching completed receipt makes repeated commits harmless, including a lost
acknowledgment after intent removal.

The coordinator uses fixed scalar/view state and no heap allocation. Backend
mutations must log errors, enforce path ownership, and provide durable writes
with checked readback; commit receipts must be idempotent. Readers and writers
must remain excluded until recovery finishes. Two model tests exercise all
eighteen mutation boundaries before and after effect (36 interrupted runs),
complete recovery, repeated commits and rejection of invalid candidates or
pre-existing backups. These tests verify the state machine, not SD durability.
The HAL backend, authoritative journal/frontier checks, transaction transport and
startup gate remain unfinished; live synchronization is not enabled by this work.

`CompanionTintaDerivedPaths.h` now confines publication to fixed names within the
course namespace. Active files are `items.bin`, `reviews.log`, `lessons.bin`,
`readings.bin` and `days.bin`; their candidates use `.sync` and backups use
`.sync-old`. Intent and receipt records are `sync-intent`/`sync-receipt`, with
separate `-stage` names for checked publication. These nineteen paths are distinct,
fit the existing path buffers and avoid the local activity's `.tmp` recovery
names. Stable `readings.bin` does not overwrite legacy `read.bin`. Path tests
cover every role, invalid enum values, zero course and insufficient output space.
The HAL backend must use these mappings and reject conflicting ownership before
mutation; live stable-completion loading and legacy migration remain required.

`HalTintaDerivedRecordReader` now inspects course-scoped intent/receipt records
using checked directory enumeration, so lookup failure cannot masquerade as a
missing recovery record. It requires a valid expected manifest for the same
course, compares complete fixed-size bytes through caller scratch, checks reads,
extent and close results, and distinguishes missing, matching, conflicting and
I/O-error states. Its retained HAL handles are reused and closed at release; the
fixed name/path buffers belong to the session. The object exceeds the small-local
budget and must be placed in a checked session allocation when integrated, while
the caller supplies at least 332 scratch bytes separate from manifest storage.
The host fixture covers every classification, source-read failure and directory
enumeration failure. Durable record writing and the complete HAL publication
backend remain unfinished.

`HalTintaDerivedIntentWriter` now publishes the fixed intent record with checked
HAL writes, truncate, sync, close, full staged readback, rename and final readback.
It rejects a different existing intent or mismatched course; an identical durable
intent returns success without rewriting. The caller must first validate all
candidate files and own the reserved private stage path exclusively. Orphan stage
bytes can be rebuilt while the intent is absent, because no learning files have
been published yet. The writer borrows the record reader and retains one reusable
HAL handle plus fixed path buffers; no content buffer allocation is added. Ten
HAL tests now pass, including sync/readback failure, rename failure before and
after effect, retry, repeated intent publication and wrong-course rejection.
Commit receipt replacement, file rename ownership checks, startup recovery and
the full publication backend remain pending; SD hardware power-cut acceptance is
still required.

`HalTintaDerivedReceiptWriter` now publishes the commit receipt through synced,
closed and byte-verified private staging. A new receipt requires the exact durable
intent. Replacing an older receipt also requires its verified previous bytes,
matching course/storage generation and a strictly increasing revision; old pack
and frontier hashes may differ across legitimate updates. Receipt removal precedes
rename while the intent remains durable, allowing retry after either operation
reports failure after effect. Identical receipts return success without writes.
Intent clearing requires the matching receipt and is idempotent after a lost remove
acknowledgment; the coordinator must finish backup cleanup before invoking it.

Twelve HAL tests pass, covering receipt replacement sync/rename/remove failures,
retry, repeated commits, lost intent-removal acknowledgment, missing intent and
non-increasing revision rejection. The writer adds fixed session path state and
one reusable handle, not a content buffer; it must be placed in checked session
storage rather than a large local. File ownership operations, complete HAL backend,
startup gating and physical durability acceptance remain unfinished.

`HalTintaDerivedFileOwnership` now provides checked file-state inspection and
the coordinator's two allowed rename directions. It uses course-scoped fixed
paths, checked directory enumeration and SHA-256 receipts, and closes retained
handles before rename/remove. Every rename requires the exact durable intent and
a missing destination. Candidate cleanup requires matching candidate and active
receipts; backup removal requires the matching commit receipt. Active-file removal
and other rename directions are rejected. It reuses caller scratch and retained
handles; fixed buffers require checked session storage at integration.
Thirteen HAL tests pass, including intent-gated renames, receipt-gated backup
removal and idempotent cleanup. Complete backend composition, startup recovery,
legacy completion migration and physical power-cut tests remain pending.

`HalTintaDerivedPublicationStorage` now composes the record readers/writers,
candidate validation and ownership operations into the coordinator's concrete HAL
backend. Admission compares the exact manifest and expected course/storage/pack/
frontier, and requires an explicit `proveJournal` callback; no default proof is
accepted. Fresh intent publication additionally requires successful full candidate
validation and consumes that permission. Rename/remove/commit/clear methods are
gated by admission, and intent clearing rechecks that every backup is absent.
It borrows immutable bindings, manifest, previous receipt and caller scratch;
large fixed session state must be allocated with checked ownership at integration.

Fourteen HAL tests pass. The composed test rejects missing journal proof without
mutation, then uses an explicit fixture-only proof to publish all five real
fixture files through HAL methods, verifies candidate/backup cleanup and confirms
repeated publication changes no files. This does not implement the production
journal proof. Recovery fault injection for the composed backend, previous-receipt
loading, live startup/transport integration and physical acceptance remain pending.

The composed HAL publication test now restarts the entire backend after failure
at all twelve rename boundaries, both before and after effect (24 interrupted
runs), plus intent-sync failure and backup removal before/after effect. All 27
interrupted cases recover to the exact five fixture files, remove candidates and
backups, and accept repeated commits without changing files. Fourteen targeted
HAL tests pass. This exercises the actual helper composition over simulated HAL
storage; it does not establish physical SD rename/sync durability, production
journal proof or live startup gating. Those requirements remain open.

`HalTintaDerivedRecordReader.load` now loads an on-card intent/receipt without
prior knowledge of its bytes. It validates exact extent, the full manifest and
course identity, then copies into caller-owned output only on success. Missing,
invalid and I/O-error statuses are distinct; failure preserves output. Output
must not overlap scratch, and overlapping ranges are rejected. The reader
retains checked HAL handles and introduces no content allocation. Fifteen HAL
tests pass, including unchanged output on missing/corrupt/unreadable records and
scratch-alias rejection. This supplies manifest loading for recovery and previous
receipt replacement; external storage/pack/frontier proof and live startup gating
remain unfinished.

Apple review replay now obtains item state and daily counter increments from
`CompanionTintaItemReplay.h` through the C scheduler bridge. Swift no longer derives
these counts from packed-state offsets. The bridge uses C++20 and returns a fixed
three-boolean count structure alongside the existing 16-byte state; it adds no C++
heap allocation. Envelope validation, causal ordering and undo filtering still run
in TintaHistory. Native Apple SDK builds remain required in addition to portable tests.

Suspension and star replay also use the shared C++ flag helper through a typed
ScheduledItem API. Swift no longer edits packed flag offsets. This path preserves
schedule fields and derived leech flags and does not construct an FSRS scheduler.
The bridge test toggles both flags and verifies the original item remains immutable.

Validation covers concurrent reviews followed by exact undo, reversed arrival,
duplicate delivery, flag preservation, completion sets, totals, configuration mismatch,
invalid undo targets, body bounds, and replay after SQLite reopen. Hardware journal
persistence/migration, consistent installation of derived state, and rebuilding active
study sessions remain required before enabling synchronization.

The portable reader codec is `lib/Companion/CompanionTintaBody.h`. It accepts bounded
spans, performs bytewise little-endian reads, and changes the caller's decoded output
only after full validation. It allocates no memory. Envelope binding takes freshly
computed body/configuration digests from the storage/crypto adapter; copying envelope
digests into those arguments does not verify integrity. Review and undo fixtures under
`protocol/fixtures` are decoded and re-encoded by both C++ and Swift tests. The codec
is not yet called by live firmware mutations, so these additions do not enable sync.

The host codec probe compiles with no exceptions/RTTI and `-Wframe-larger-than=256`
as an error, and asserts the body object is no larger than 128 bytes. ESP32 compiler
and physical stack/heap validation are still required when the codec is integrated.

### Recovery entry point

`HalTintaDerivedRecovery.h` loads the course's durable `sync-intent` and optional
`sync-receipt` without requiring the caller to know the pending manifest bytes.
The caller supplies three disjoint buffers of at least 332 bytes and a record
reader for the same course. Invalid records, unreadable storage, overlapping
buffers, and failed journal proof block recovery without publishing files.
`NoPending` means only that checked intent lookup found no pending record; it
is not proof that active learner state is valid.

When an intent exists, recovery creates one checked, scoped publication backend
with `makeUniqueNoThrow`. Its retained HAL handles and fixed path buffers exceed
the 256-byte local-variable budget, so the backend lives on the heap and is
released on every return. The helper reports allocation failure separately.
Production integration must supply journal proof and exclude other state writers
before invoking recovery; the fixture proof used by host tests is not sufficient
for reader startup authorization.

Run `HalInventoryFileHashTest` to verify interrupted publication recovery and
rejection of corrupt intents/receipts, I/O failures, missing or rejected journal
proof, mismatched courses, and overlapping or undersized buffers. Failure tests
assert that the stored file map remains unchanged. Physical SD power-loss and
startup acceptance remain pending.

### Canonical authoritative journal digest

`TintaJournalFrontier.digest` defines `TJF1` canonical hashing for a complete
journal. It validates Tinta bodies/envelopes and replay (including causal
closure and exact undo references), rejects equivocation, deduplicates delivery,
and sorts unique events by origin bytes, then numeric epoch and sequence. Hash
input is ASCII `TJF1`, unique event count as u64 little-endian, then each event's
u16 little-endian envelope length and exact protocol envelope bytes. Envelopes
include body SHA-256, scheduler configuration, and causal dependencies. Undone
reviews remain in the digest; this binds authoritative history rather than only
its current effects. The empty digest hashes the twelve-byte zero-count prefix.

This digest covers the complete authoritative journal, including non-Tinta events. Delivery
order and duplicate delivery do not affect it. It does not replace validation
of reconstructed item/completion state or pack membership. Reader-side digest
calculation and binding this result into production installation authorization
remain pending. `TintaHistoryTests` covers delivery permutation, duplicates,
changed bodies, equivocation, missing ancestry, undo retention, and empty input.

`CompanionTintaJournalFrontier.h` provides the reader's streaming `TJF1` encoder.
It borrows existing record-sized session scratch and sends prefix, length, and
envelope chunks to a function-pointer sink. It allocates no heap storage and
retains only the previous identity and counters. Identity order compares epoch
and sequence numerically, not their little-endian byte encodings. Duplicate,
out-of-order, excess, unencodable, or sink-failed appends invalidate
the session. `complete()` requires exactly the declared event count; `begin()`
starts a new encoding session, but the caller must reset its hash sink separately.

The caller must supply a fully validated, deduplicated journal in canonical
identity order and exclude writers. The encoder checks ordering and envelope
encoding, not bodies, causal closure, undo semantics, or reconstructed state.
It is not yet connected to the live reader hash implementation or startup gate.
`CompanionTintaJournalTest` checks exact framing, count completion, duplicate
rejection, numeric epoch ordering across 255/256, failed sinks, and short scratch.
All 19 journal host tests pass; Apple/reader shared nonempty digest fixtures and
physical acceptance remain pending.

`protocol/fixtures/TintaJournalFrontier-v1.fixture` is an independently generated
Python `struct`/`hashlib` vector: 378 bytes of canonical `TJF1` input followed by
its 32-byte SHA-256. It contains star-on followed by causally linked star-off
for course `07` repeated sixteen times, item 1, origin `01` repeated sixteen
times, epoch 1, sequences 1/2, storage `02` repeated sixteen times, resource
`03` repeated thirty-two times, and study day 1. Both tests require the exact
input bytes and digest: Swift constructs validated mutations and tests reordered
duplicate delivery; C++ decodes the fixture envelopes and streams them through
the production encoder. All 12 Tinta history tests and 20 journal host tests
pass. This replaces the pending nonempty cross-language fixture check above;
reader journal ordering, live hash wiring, and state proof remain pending.

`TintaHistory.derivedInstallation` now builds immutable course files, canonical
frontier hash, and installation manifest together from one supplied complete
Tinta journal. Callers cannot supply an unrelated frontier hash through this
entry point or mutate its returned fields. Foreign-course events remain bound
into the frontier while only selected-course derived state enters the files.
This API does not obtain or prove journal completeness itself; transport/storage
integration must provide the full journal and verified pack/storage bindings.
It rejects incomplete ancestry and invalid Tinta replay before
returning an installation. A regression checks reordered duplicate input, exact
file receipt hashes/lengths, foreign-course binding without foreign derived
state, and missing ancestry. All 13 Tinta history tests pass. Pack membership,
reader replay proof, and actual installation transport remain pending.

`CoursePackMetadata.itemHistoryCount` now exposes the inspector-validated
contiguous IDEN history when present. `containsHistoricalItem` accepts both
active and retired IDs within that history, rejects zero/sentinel/out-of-range
IDs, and falls back to explicit active identities for legacy packs without IDEN.
The count is derived only after IDEN records, active UID coverage, and full pack
CRC validation. It does not prove a course association or content hash. Course
pack tests exercise actual IDEN inspection and legacy/retired membership; this
supports the pending installation membership validator without dropping valid
retired-item history.

The history-based installation builder now requires inspected course metadata.
Before producing files it checks every selected-course mutation in its own
namespace: review/undo/suspension/star use active IDs or validated IDEN history;
lesson completions use lesson identities; completed readings use distributed
story identities, never legacy title keys. The check includes undone reviews
and completion removals. Retired item states and their events remain retained,
while unknown identities reject the build. Other courses remain part of the
canonical frontier without being checked against this course's metadata.

Callers must bind metadata to the verified pack hash and confirmed course
association; this API does not establish that binding. Tests cover active and
retired items, unknown items even when undone, namespace confusion, legacy
reading keys, and valid lesson/reading completions. All 14 Tinta history tests
pass. Reader-side membership and replay proof, pack-byte binding at the caller,
and transport/startup integration remain pending.

`ContentVault.tintaDerivedInstallation` now obtains pack metadata and manifest
pack hash from the same verified content object. It holds the shared vault lock
against cooperative pruning, hashes the object, fully inspects the pack, builds
membership-checked state from the supplied journal, and re-verifies the object
before returning. It creates no derived files on disk or transfer jobs. The
existing immutable-vault contract excludes external writers; the shared lock
is not protection against arbitrary filesystem modification.

Tests cover manifest hash binding to a real course fixture, unchanged empty
journal binding, corrupted vault objects, and hash-valid invalid pack bytes.
All 13 course-pack tests pass. The caller still must verify the confirmed course
association, journal completeness, reader/card identity, and revision authority.
Library orchestration, state transfer, reader proof, and startup remain pending.

Mixed event kinds share per-origin sequence numbers in LibraryStore. Frontier
hashing and installation building therefore consume the complete mixed journal,
retaining reading/preference/bookmark envelopes for sequence and causal closure.
Only Tinta events enter learner replay and course membership checks; all event
kinds enter the canonical digest. JournalMutation verifies every body hash, while
non-Tinta semantic validation remains the importing store's responsibility.
The reader streaming encoder accepts the full protocol event-kind range. The
Tinta-only reader journal cannot yet serve as a complete mixed-journal source.
A new regression starts with a preference followed by a review in the same
origin sequence, verifies successful installation, rejects the filtered review,
and proves changed preference content changes the frontier. All 15 Tinta
history tests and 20 C++ journal tests pass. Unified reader journal storage and
production orchestration remain pending.

`LibraryStore.tintaDerivedInstallation` orchestrates a library-backed build. It
requires an undeleted verified course entry and confirmed course association,
captures the full mixed journal, and passes expected pack length/details to the
vault. The vault compares them with the actual verified pack before building.
After the asynchronous vault call the library rechecks the course manifest and
canonical digest of its current journal; changed history throws `staleFrontier`.
No transfer job or published learner state is created by a rejected build.

The internal build callback separates orchestration from the vault adapter. A
regression inserts a preference while that callback runs, builds through the
real vault from the captured journal, checks stale-frontier rejection, and then
successfully rebuilds from current history. Additional coverage requires an
association, binds preference-only history, and rejects deleted content. All
14 course-pack tests pass. This protects the build boundary only: a later
transfer commit still needs reader/card/revision binding and fresh journal
proof. State payload delivery and reader integration remain pending.

`TintaDerivedReceipt` adds strict Apple decoding for the reader's 332-byte TDS1
manifest. It checks exact length, magic, reserved fields, nonzero bindings and
revision, each file's nonzero hash and canonical size/alignment bounds, and CRC.
It exposes immutable bindings and five ordered length/hash receipts. It does
not verify the referenced files, course membership, or journal authority.
The shared manifest fixture decodes on both platforms; Swift tests reject every
truncated prefix, a bit change at every byte, and invalid fields with recomputed
CRC. All 16 Tinta history tests pass. Before this decoder addition, the full
portable Swift suite passed all 296 tests. Native Apple SDK and physical checks
remain pending.

`TintaDerivedInstallation(files:manifest:)` now verifies artifact integrity on
construction, including restoration from persisted bytes. It strictly decodes
TDS1, requires course/study-day agreement, and verifies length and SHA-256 for
all five files, including the required empty local review log. The frontier
property comes from the checked receipt. The history-based builder also uses
this constructor before returning. A restored artifact has integrity only;
its journal authority, file-format semantics, and pack membership must still
be proved before installation. Tests alter or truncate every file and alter
course/day bindings with a recomputed manifest CRC. The full portable Swift
suite passes all 298 tests after this change.

`ContentVault.storeTintaDerivedInstallation` durably publishes each of the five
content-addressed state files before publishing the manifest object last.
Repeated stores reuse identical immutable objects. A failed/cancelled store
may leave unreferenced objects, but does not return a manifest reference before
all file publications succeed. The caller must durably retain the manifest and
all receipt-hash references in job metadata before allowing vault pruning.
This API does not create that job or authorize reader installation.

`restoreTintaDerivedInstallation` holds the shared pruning lock, verifies the
manifest object, checks aggregate size against a caller-provided budget before
loading state bytes, verifies all five referenced objects, and constructs an
integrity-checked artifact. Tests persist, reopen the vault, restore exact bytes,
repeat storage, reject a short budget, and reject a corrupted referenced item
file. All 14 course-pack tests pass. SQLite retention/job orchestration and
reader transfer remain pending.

Library schema 23 adds `tinta_artifacts` with manifest hash and exact TDS1
payload. `persistTintaDerivedInstallation` builds through the library/vault,
publishes file objects and manifest, then enters a SQLite immediate transaction,
rechecks confirmed pack/course and current journal frontier, and atomically
retains the manifest. Stale builds can leave unreferenced vault objects but
cannot create a retained artifact. `retainedTintaArtifactObjects` validates each
stored payload/hash and returns the manifest, pack, and five file object IDs;
shared identical files naturally deduplicate. `releaseTintaArtifact` removes
only the retention row and is idempotent; it does not delete vault bytes or
installed reader state. Job orchestration must prevent release while needed.

Tests persist via the library, reopen SQLite, restore exact vault bytes, check
all object references, and release twice. The legacy schema-13 upgrade fixture
now removes the new table when simulating old storage. All 298 portable Swift
tests pass. Actual vault pruning must include these references and serialize
publication/retention against collection; no live pruning flow is implemented.
Artifact-to-reader jobs, commit authority, and reader integration remain pending.

`LibraryStore.restoreRetainedTintaDerivedInstallation` is the library-backed
restart path. It checks the retained payload against its content hash, expected
storage generation, confirmed undeleted pack/course, and current mixed-journal
frontier. After bounded vault restoration and pack-object hash verification it
rechecks retention, pack manifest, and journal frontier across the asynchronous
boundary. Missing/released records and changed history block restoration; wrong
storage generation fails before vault access. This is library eligibility and
artifact integrity, not reader commit authorization or replay proof.
Tests reopen SQLite, resume exact files, reject a foreign storage generation,
change history through another library handle and reject the stale artifact,
then release and reject further restoration. All 14 course-pack tests pass.
Reader identity/revision authority, transfer jobs, and reader integration remain
pending.

Schema 24 adds a durable, queued-only Tinta installation request with transaction,
manifest, reader, storage generation, and paired installation owner. Admission
runs under SQLite immediate transaction, requires complete inventory, matching
storage, exact installed course manifest, confirmed library association, and
current journal frontier. One request per reader/card is allowed. Identical
requests reuse the transaction; different artifacts or owners conflict. A queue
foreign key and explicit release guard retain its artifact. Owner-matched
cancellation removes only a queued request and leaves artifact/reader bytes.
There is no dispatch/staging phase yet; this cancellation API must not be used
for an active reader transaction when the runner is added.

Tests exercise two library handles, repeated queue identity, owner conflicts,
wrong-owner cancellation, and release protection. The old-schema fixture drops
the queue before its referenced artifact table. All 298 portable Swift tests
pass. Inventory provenance/authenticated connection is the caller's obligation;
queue admission does not reserve reader state or verify capabilities. Transport
runner, durable offsets/phases, reader commit proof, and startup remain pending.

Queue admission now requires exactly one installed course entry, equal to the
artifact's pack manifest, matching the reader's single-active-pack model.
Tests reject incomplete inventories, missing/ambiguous course entries, wrong
cards, malformed owners, and a transaction reused for another reader, asserting
no request is created for failed admissions. Existing pending requests remain
unchanged by identity conflicts. All 14 course-pack tests pass.

Schema 25 adds queued/staging phases. `claimTintaInstallation` checks exact
pending transaction/artifact/owner, complete matching reader/card inventory,
installed pack/course, and fresh journal frontier under an immediate transaction
before durably entering staging. A repeated claim with the same bindings is
idempotent; the current journal is still revalidated. Queued cancellation now
explicitly excludes staging, preserving a potentially active transaction and
its artifact until future protocol recovery/abort support can resolve it.
Tests claim through one handle, observe/reclaim through another, attempt queued
cancellation, and verify staging identity and artifact retention survive. All
298 portable Swift tests pass. This phase transition sends no bytes. The
transport runner must claim before Begin, then persist per-file offsets and
commit/abort phases; those operations remain pending.

`HalTintaDerivedCandidateStage` stages one received file at its fixed course
candidate path, binding course/card/pack/frontier to the manifest and limiting
writes to its receipt length. `HalVerifiedFileStage` now accepts a borrowed
parent directory (existing callers retain the companion-directory default) and
an optional required hash at seal. Synced SD readback must match both received
stream SHA and manifest SHA before ownership is handed to publication. A hash
mismatch retains cleanup ownership; active files are never opened for writes.
Cleanup must succeed before the wrapper changes its borrowed path buffers.
Sealed candidates remain on SD for generation validation/publication.

The wrapper adds no allocation and borrows caller scratch. Its fixed paths and
retained handles exceed the small local-variable budget: production integration
must own it in a checked session allocation and exclude state writers/pending
publication before begin. It does not validate file format or journal authority.
Tests stage wrong/right daily-log bytes, verify mismatch cleanup, preserve active
files and sealed candidates, and reject stale bindings/existing candidates.
All 9 archive-stage and 10 ZIP-entry-stage tests pass. Live dispatch, resumable
candidate offsets, full generation proof, and physical SD acceptance remain
pending; these unused helpers do not yet enable reader state transfer.

Candidate staging now tracks seal success for the current attempt independently
of the shared stage's retained diagnostic flag. Every Begin clears that status,
including binding rejection before the shared stage begins. A new cleanup fault
test attempts to switch file kinds while removing the old partial candidate
fails, verifies neither path is redirected/deleted, then retries cleanup and
seals the required empty review log. Existing sealed candidates survive rejected
Begin while `isSealed()` remains false for that rejected attempt. All 10
archive-stage tests pass. No heap allocation was introduced.

The composed HAL publication regression now receives every candidate through
`HalTintaDerivedCandidateStage` in 37-byte chunks (and seals the zero-byte local
review log), verifies all active files stay unchanged during reception, then
runs generation validation/publication and the existing 27 interrupted-mutation
recovery cases. Candidate data is no longer injected directly into the fake SD
map for this scenario. All 16 HAL inventory/hash tests pass. Journal proof is
still an explicit fixture callback, so this demonstrates staging-to-publication
composition, not production journal authorization or a live radio endpoint.

`CompanionTintaReceiveCheckpoint.h` defines TRC1 (132 bytes) for pending candidate
reception: magic at 0; course/transaction/owner/storage IDs at 4/20/36/52;
installation manifest SHA-256 at 68; current file kind and sealed-file mask at
100/101; zero reserved bytes at 102/103; durable offset, declared file length,
and journal sequence as u64 little-endian at 104/112/120; CRC32 at 128 over the
first 128 bytes. IDs/hash and sequence must be nonzero, file kind valid, offset
within length, length within SD's u32 extent, mask limited to five files, and
current file absent from the sealed mask. Decode preserves output on failure.
It allocates no heap; the decode temporary remains below 256 bytes.

This codec does not establish ownership itself. Durable dual-slot storage,
manifest/receipt binding, prefix verification, cross-language fixtures, and
transport resume are pending; candidate staging must not resume from an
untrusted offset. Codec tests cover roundtrip, every byte corruption/truncated
prefix, bad offsets, zero owner, and inconsistent seal mask. All 4 publication,
path, and checkpoint host tests pass.

`matchesTintaReceiveCheckpoint` now binds a shape-valid checkpoint to the
expected transaction/owner/course/card, SHA-256 of the exact installation
manifest, manifest pack/frontier bindings, and the selected file's receipt
length. The caller must compute the expected manifest hash and obtain current
bindings from authenticated, verified state; this comparison does not supply
that authority. Tests calculate SHA-256 of the shared manifest fixture and
reject mismatched identities/hash/length/file/offset, stale pack/frontier, and
an undecoded manifest. All 17 HAL hash tests and 4 publication/checkpoint tests
pass. Encode uses explicit offsets matching TRC1's decoder. Durable checkpoint
storage and prefix-resume validation remain pending.

`CompanionTintaReceiveJournal.h` adds allocation-free dual-slot TRC1 persistence
through a storage interface requiring durable successful writes. Recovery checks
sequence parity and consistent adjacent transitions, uses the highest valid
slot, and distinguishes missing, corrupt, and unreadable records. Start requires
checked absence, sequence one, Items, zero offset/mask. Advances preserve all
ownership/manifest bindings, increment sequence exactly once, and permit only
monotonic same-file offsets or the next file after full length with the previous
file's sealed bit set. Exact repeated checkpoints write nothing. Writes require
readback equality; uncertain I/O invalidates readiness until reopen.

A test cuts every checkpoint write at all 133 byte boundaries, reopens, recovers
the old or complete new offset, and verifies no further uncertain writes occur.
All 5 publication/path/checkpoint/journal tests pass. The caller must sync received
bytes before advancing and verify file SHA before marking it sealed. The journal
retains two checkpoints outside the task stack and borrows 132-byte scratch;
no heap allocation is introduced. HAL slots, explicit manifest binding before
resume, prefix verification, and transport integration remain pending.

`HalTintaReceiveJournalStorage` implements checked HAL read/write for fixed
course `receive-a`/`receive-b` slots. It uses mutex-wrapped handles and private
parent lookup, validates course/sequence parity before writes, checks seek/write,
truncate/sync/close, and reports lookup/read/close errors. Existing empty or
wrong-sized slots return an invalid nonzero extent so journal recovery cannot
mistake them for absence. The course directory must already be prepared and
writers excluded. Its retained handles and path buffers are session-owned
outside the task stack; no allocation is introduced by the adapter.

HAL tests inject sync failure, require reopen before updates, recover the fully
written checkpoint after an ambiguous sync result, reject wrong-slot writes
without mutations, block on read errors, and preserve an existing empty corrupt
slot. All 19 HAL hash tests pass. Physical SD persistence, prefix/data syncing,
manifest ownership checks at integration, and live transfer dispatch remain
pending. The adapter alone does not make candidate reception resumable.

Candidate staging now exposes `syncPending(durableOffset)`. It verifies the
open extent, syncs SD bytes, rechecks extent, and changes the caller's offset
only on success. Failure latches the session against further writes/seal;
cleanup remains available. It neither finalizes streaming SHA nor declares a
file sealed. No new buffers or allocations are introduced.

The composed receive/publication test now starts a real HAL-backed TRC1 journal,
syncs each received chunk before advancing its checkpoint, and switches files
only after receipt SHA seal succeeds, including the empty local review log.
The 27 publication interruption cases run from this received/checkpointed state.
A sync-failure regression preserves the last reported offset and blocks uncertain
continuation. All 11 archive-stage, 19 HAL hash, and 10 ZIP-stage tests pass.
Physical persistence and reopening/trimming/rehashing candidate prefixes remain
pending; these checks do not yet provide restart resume or live transport.

Candidate staging can now resume from a recovered checkpoint after explicit
manifest/transaction/owner/course/card/pack/frontier and receipt-length checks.
It opens the fixed candidate with HAL O_RDWR without create/truncate, rejects
missing/directories/short prefixes, rebuilds streaming SHA through caller scratch,
trims/syncs any unacknowledged tail, and seeks to the durable offset. Final seal
still verifies the full manifest hash: TRC1 carries no independent prefix hash,
so silent prefix corruption is detected at final seal, not at resume admission.
Pre-admission failures preserve existing candidate bytes for recovery.

HAL `Storage.open` uses one checked handle allocation per resume session. This
avoids adding a second HAL open API; the handle remains session-owned and RAII
released, with no allocation per read/chunk. Tests resume a partial daily log,
trim a torn tail, complete/seal correct content, reject corrupted prefix at seal,
and preserve a too-short file. All 12 archive-stage, 19 HAL hash, and 10 ZIP-stage
tests pass. Integration must supply a checkpoint from the recovered durable
journal, exclude publication/writers, and verify prior sealed files. Live restart
orchestration/radio dispatch and physical persistence remain pending.

`retainPending(durableOffset)` hands a partial candidate to journal recovery
before a graceful session exit. The caller must already have committed that
offset to TRC1. The stage requires a successfully synced extent covering it and
an existing prefix at least that long, stops writes, relinquishes deletion
ownership, and closes the handle. Even a lost close acknowledgement preserves
the file. Unacknowledged tail data remains for checked trimming on next resume.
An unsynced candidate cannot be retained through this API. Tracking adds one
u64 and a boolean, with no buffer or heap allocation.

Tests sync a prefix, write an extra tail, retain with successful/lost close,
destroy/recreate the session, trim/re-hash/complete/seal, and reject unsynced
retention. All 13 archive-stage, 19 HAL hash, and 10 ZIP-stage tests pass. Live
session teardown must integrate journal ownership and this handoff; abrupt power
loss still requires physical SD acceptance. Radio dispatch and full replay proof
remain pending.

### Companion commit confirmation

`LibraryStore.confirmTintaInstallation` clears a committing request only when the
caller supplies the authenticated reader, storage generation, owner, and exact
committed manifest for that request. The manifest must decode as TDS1, hash to the
queued content identity, and equal the retained artifact bytes. Confirmation is
transactional across database handles; unrelated replies preserve the queue.
A repeated confirmation with no pending request returns false. Artifact retention
is separate and remains intact after confirmation. A history change after staging
does not prevent acknowledging the exact generation that was installed.

This method does not authenticate transport or establish reader publication by
itself. The live transport must obtain a committed receipt, rather than treating
a transfer acknowledgment or locally generated manifest as commit evidence.

Schema 26 adds a durable committing phase while preserving queued and staging
rows during migration. `prepareTintaInstallationCommit` validates the current
pack, inventory, and journal frontier before moving a staging request to committing.
Repeated preparation of that same committing request returns it without requiring
the frontier to remain unchanged. It cannot return to staging or be canceled by
the queued cancellation API. The transport must persist this phase before sending
a publication request, then query the reader after an uncertain reply.

`restoreCommittingTintaInstallation` retrieves the exact retained generation for
an existing committing request even if subsequent events change the frontier.
It verifies the manifest, all five derived objects, and the pack object in the
vault, then rechecks the pending request and retained manifest after asynchronous
reads. Staging and completed requests cannot use this path. This recovery path
does not authorize a new installation; new installations retain current-frontier
validation through `restoreRetainedTintaDerivedInstallation`.

### Reader preference records in the shared journal

The reader journal now accepts typed portable preference bodies alongside Tinta
bodies. Preference records bind SHA-256 of the body to the envelope, require the
fixed portable-preferences resource digest and zero scheduler fields, and validate
integer ranges, language tags, and dictionary/font selections. Borrowed string
views remain length-delimited. The 69-byte maximum preference body fits the
existing 512-byte record with four causal ancestors; a compile-time assertion
checks that capacity. No additional heap buffer is allocated.

This extends accepted record semantics, not the record layout. Older firmware
that accepts only Tinta bodies cannot read a journal containing preferences;
downgrade compatibility remains a required firmware-installation gate. Reading
and bookmark bodies, causal closure validation, journal exchange, preference
application, and live preference mutation capture remain pending.

Journal admission now requires each explicitly named ancestor and the preceding
sequence in the same origin/epoch to exist before a new record is published.
These checks reuse the duplicate scan and need no allocated graph. An atomic
pair can also depend on its first event. Exact duplicates remain idempotent;
partial duplicate pairs remain conflicts. The writer's private fresh-append path
continues to use its reserved epoch, checked committed count, contiguous local
sequence, and known committed frontier without rescanning history.

Exchange must assemble reordered deliveries and admit them in causal order.
These admission checks do not retroactively audit old journals or implement the
complete distributed merge/replay proof needed before derived-state publication.

`JournalCausalValidation` audits an opened committed journal through a caller-owned
stable identity index. Each event must map to its exact record position. Every
explicit ancestor and implicit per-origin predecessor must map to an earlier
record whose decoded identity matches the requested dependency. Thus an index
cannot turn a different record into a valid ancestor. Checks use session-owned
fixed metadata, allocate no heap, and do not mutate files. Missing entries,
forward dependencies, identity mismatches, and I/O errors fail validation.

The SD index builder and startup/publication integration remain pending. This
validator requires persisted causal order; ordering arbitrary incoming deliveries
is still the exchange layer's responsibility. It establishes journal closure,
not pack membership, exact undo replay, or equality with a proposed snapshot.

The disposable journal identity index uses a 16-byte JIX1 header (magic/version,
record count, entry-stream CRC32, header CRC32) and 40-byte entries (origin16,
epoch u64LE, sequence u64LE, record position u32LE, CRC32). Entries are strictly
ordered by origin bytes, then numeric epoch and sequence; duplicate keys fail
opening. `IndexedJournalIdentities` checks exact extent, count, every entry, and
the stream CRC before enabling binary search. Lookup rechecks entry CRC and
latches unavailable on read corruption. CRC is an integrity check, not authority;
`JournalCausalValidation` still verifies index results against journal records.
The reader borrows forty bytes of scratch and allocates no heap. Sorting, HAL
storage, and startup integration of this index remain pending.

`CommittedJournalIdentitySource` streams each validated journal identity and its
original record position while checking a frozen journal count.
`JournalIdentitySorter` heap-sorts bounded chunks, then merges two disposable
runs until all entries are ordered by origin and numeric epoch/sequence. It
borrows at least 120 bytes of scratch (two input banks plus an output bank), keeps
entries encoded, rechecks entry CRCs when reading runs, and allocates no heap.
The algorithm uses O(N log N) comparisons and merge work rather than requiring
all identities in RAM. Test runs cover 120-, 121-, and 511-byte buffers, numeric
255/256 ordering, preserved positions, and every modeled storage failure point.
HAL run storage, index publication, and startup integration remain pending.

`JournalIdentityIndexBuilder` consumes the sorted stream into a disposable index
sink. It requires the exact expected journal count, strictly increasing unique
identities, and in-range record positions, writes the entry-stream checksum, and
publishes only after the sink's durable finish succeeds. A scoped guard aborts
unfinished candidates. The composed host test sorts a causal journal whose
physical order differs from identity order, builds and opens the index, and
runs the production causal validator. Modeled failures at every sink operation
preserve the preceding published index. HAL storage and startup wiring remain
pending; these helpers do not themselves establish replay equivalence.

`HalJournalIdentitySortStorage` now reuses the HAL retained-handle sorter with
separate `journal-sort-a`/`journal-sort-b` paths. The shared implementation borrows
two path pointers, rejects null or identical paths, syncs/truncates completed
runs, and periodically yields. It opens at most two retained HAL handles per
session; the existing checked HAL handle allocations are reused across chunks
and resets, rather than allocating per operation. The adapter itself allocates
no heap. A composed host test sorts 257 journal identities across multiple passes
while preserving inventory run files. Index sink/storage and startup wiring are
still pending.

`HalJournalIdentityIndexSink` builds a staged index, syncs it, validates it through
the retained HAL reader, then renames the previous cache to a backup before
publishing the candidate. The next build restores a backup when the active path
is absent and discards leftover disposable candidates. All presence checks use
checked HAL enumeration; file operations stay behind `HalStorage`. Read, sync,
close, lookup, and rename failures prevent a successful build result. Lost rename
acknowledgments can leave either complete cache generation in place; failed
sessions must recover/rebuild before use. The cache is never authoritative and
these operations do not touch the journal. Model tests cover both rename steps
before and after their effects, failed sync, and failed validation reads.
Startup/publication proof wiring remains pending.

### Live Tinta entry gate

`TintaActivity` now checks for an existing companion journal after course-state
selection and before allocating `App`. An absent journal keeps the legacy entry
path and does not create event/header files. An existing journal is recovered,
streamed through the HAL external sorter, indexed through the recoverable HAL
sink, reopened, and causally audited. Failure leaves the app unopened and uses
the existing translated open-failure UI plus module logs. The checked audit
workspace and retained handles are released before UI allocation. It borrows
fixed session buffers (512 bytes each for journal and sorting, 40 for the index)
and avoids buffers on the task stack.

Host coverage exercises absence, a valid two-record history, failed cache sync,
and an origin sequence gap with recomputed record CRC; journal/header bytes are
preserved during successful audits. Device verification must measure entry time
on large histories and inspect the existing TNT free/largest-heap logs before
opening and after exit. This gate does not enable mutation capture, history
exchange, pack-membership/replay proof, or received derived-state publication.

`HalJournalCausalAuditSession::run` can now return the canonical TJF1 frontier
SHA-256 after a successful causal audit. `streamJournalFrontier` traverses the
identity index in sorted order, rereads each referenced journal record, verifies
its identity, and feeds the exact event envelope into the existing encoder.
Body hashes are checked by the journal reader. The hash state and encoder are
session-owned, and the encoder reuses the completed sort buffer; no second heap
buffer is allocated. The caller's digest is assigned only after streaming,
SHA finalization, and index close succeed. The default Tinta entry audit does not
request this optional digest.

The composed HAL test reproduces the independently generated shared Apple
frontier fixture exactly. A failed subsequent build leaves the prior output
unchanged. Frontier equality establishes an exact journal-envelope binding;
pack membership, undo replay, and candidate snapshot equivalence remain separate
required checks before publication.

`JournalTintaUndoValidation` now checks every undo through the identity index.
Its target must be an earlier record with the exact named identity, review kind,
and matching course/item subject. Both decoded bodies live in fixed session
storage; lookup/read errors fail validation without journal writes. The live
Tinta entry audit and optional frontier hash run this check after causal closure.
Host tests accept a matching review and reject wrong-item, wrong-course, and
non-review targets. This validates undo references; excluding undone reviews
from scheduler/day replay and proving candidate-state equality remain pending.

`JournalCourseMembershipValidation` checks all selected-course Tinta subjects,
including undo and disabled flag/completion events, through a typed catalog.
Other courses remain in the global history but are not checked against this
pack. `TintaPackSubjectCatalog` uses the validated IDEN history range for item
subjects, preserving retired item IDs; legacy packs admit only active item IDs.
Lesson keys use unit/lesson numbers, and reading keys use the distributed TST1
full-title identity. Preparation checks uniqueness of both completion namespaces
using borrowed scratch. Sources must remain immutable and have passed complete
pack structure/content/CRC validation. The helpers allocate no heap.

Host tests check namespace separation, selected-course filtering, I/O failure,
actual pack item/lesson/reading membership, and a retired IDEN item absent from
the active catalog. Binding this catalog into the received-generation proof and
proving replayed snapshot equality remain pending.

The HAL causal audit accepts an optional selected course and validated subject
catalog together. It rejects missing subjects and catalog I/O failures before
calculating the global frontier; other courses remain in that frontier without
being checked against the selected pack. Invalid or incomplete course arguments
fail before journal recovery or cache writes. Failed audits leave the caller's
frontier output unchanged. The caller must bind the immutable catalog to the
verified pack and course; this check does not establish deterministic replay or
candidate snapshot equivalence. Default Tinta startup still performs the causal
and undo audit without a pack catalog.

`CompanionReadingBody.h` decodes the Apple version-1 reading anchor and bookmark
body formats without allocations. Bookmark text is borrowed, length-delimited
UTF-8; names are limited to 128 bytes and summaries to 512 bytes, with NUL
rejected. Tombstones contain only a nonzero 16-byte bookmark identity. Failed
decodes leave the output unchanged, and numeric fields use bytewise little-endian
reads for C3 alignment safety. The codec is admitted by the extended journal format described below; existing
512-byte journals cannot hold maximum bookmark bodies and still need migration.

The journal now supports a header-declared extended format for new journals when
its caller supplies 1024 bytes of scratch. TJH3 retains the 64-byte alternating
header, uses byte 16 = 10 (log2 record size), zeros bytes 17–59, and keeps the CRC
at byte 60. TJE2 records are 1024 bytes with CRC at byte 1020; envelope/body lengths
and reserved fields retain their existing positions. The maximum body is 668
bytes, accommodating Apple bookmarks with 128-byte names and 512-byte summaries,
even with four causal ancestors. Reading anchors and bookmark puts/deletes now
receive typed validation and require a nonzero content hash and zero scheduler
fields. The existing causal dependency and exact duplicate rules still apply.

TJH1/TJH2 journals retain 512-byte records even when opened with larger scratch;
no in-place conversion occurs. A 512-byte workspace rejects TJH3 as unavailable
before truncation or publication. The HAL supports both record lengths, and the
startup audit uses 1024-byte scratch in its existing short-lived checked heap
workspace (512 extra bytes, released before the Tinta UI allocation). Existing
512-byte journals still need a recoverable migration before receiving maximum
bookmark bodies. Older firmware must be excluded from extended journals by the
companion downgrade compatibility gate; that end-to-end gate remains pending.

`JournalMigration` copies an audited journal into a separate extended-format
journal using reusable caller-owned envelope/body storage. Before copying it
checks causal closure and undo targets against the source identity index. Resume
requires every committed destination event and body to equal the corresponding
source prefix; mismatches fail before appending. Appends use the verified prefix
to establish dependencies without rescanning prior records per event. Source and
destination writers must be excluded for the operation, and their storage must
be distinct. The copier neither renames nor deletes files: recoverable HAL
publication and startup integration are still required. Host tests cover every
0–64-byte interrupted header write, including a complete write with a lost
success response, exact retries, and conflicting prefixes.

`HalTintaJournalStorage` now accepts a fixed active or migration-candidate
location. Candidate events and headers live together under
`/.crosspoint/companion/tinta-events-next`; active paths retain their original
names. Both locations use retained mutex-wrapped HAL handles, checked header
lookups in their own parent directory, and durable sync/truncation. The selector
borrows a constexpr path table (one additional pointer per storage object), with
no dynamically constructed paths or new allocation. Invalid selectors fail
before creating files. A HAL integration test copies legacy history into the
extended candidate, reopens and retries it, and verifies all active files are
unchanged. Directory publication/recovery is not yet connected.

`recoverJournalMigration` defines the publication/recovery sequence behind a
storage interface. A durable intent binds count and global frontier; verification
must also enforce 512-byte legacy and 1024-byte extended record sizes. Valid
layouts are active+candidate, backup+candidate, or active+backup. Each transition
verifies the journals before renaming complete directories, retains the legacy
backup, and reopens the published generation before clearing the intent. Lost
rename acknowledgements and failed intent cleanup are retried by inspecting the
current layout. Other layouts and failed verification do not authorize moves.
There is no concrete intent codec/HAL publication provider or startup invocation
yet. Host tests cover both rename failures before/after their effects, cleanup
retry, conflicting verification, and all ambiguous presence combinations.

Migration authorization uses a strict 64-byte JMG1 record: magic/version at 0–3,
record count as u32 little-endian at 4, the global frontier SHA-256 at 8–39,
source/destination log2 record sizes 9/10 at 40–41, zero reserved bytes at 42–59,
and CRC32 of bytes 0–59 at 60. Count must fit an extended journal and the frontier
must be nonzero (the empty journal also has a nonzero hash). Decoding rejects
truncated, oversized, unsupported, corrupt, and invalid-binding records without
changing output; encoding performs validation before touching its output. This
codec adds no heap allocation and uses bytewise integer access. HAL intent
persistence and startup publication remain pending. Tests exercise every short
length and single-byte corruption, plus CRC-valid unsupported format/reserved
bytes and invalid count/hash bindings.

`HalJournalMigrationIntentStore` persists JMG1 under
`/.crosspoint/companion/journal-migration`, staging first at `journal-migration-next`.
It syncs and closes the staged file, reads back its complete strict record, checks
that the published intent is still absent, renames, and verifies the published
record. An exact existing intent is an idempotent retry; conflicting or malformed
records block replacement. A lost rename acknowledgement returns failure, and a
subsequent retry recognizes the already published matching intent. Clearing
requires an exact matching authorization and leaves conflicting records intact.
The store uses session-owned fixed scratch and retained HAL/lookup handles;
no per-record allocation or dynamic paths are added. Tests cover failed sync,
lost rename acknowledgement, readback, conflicts, clearing, and malformed files.
This store is not yet invoked by live startup/publication.

`HalJournalMigrationPublicationStorage` implements the publication storage
interface using fixed active/candidate/backup paths; backup is
`/.crosspoint/companion/tinta-events-old`. It loads the JMG1 authorization,
checks directory types and existing events/header presence, then constructs a
checked short-lived heap audit workspace for the requested location. Count,
record size, closure, undo targets, body integrity, and global frontier are
verified before a directory move. Empty reserved directories are rejected without
initializing journals. Audit workspaces release their retained file handles
before the recovery state machine calls rename. Moves require present source
and missing destination; intent clearing uses the loaded exact authorization.
The parent/child lookup workspaces are provider members, avoiding large local
buffers. Host integration tests exercise real source/candidate verification and
wrong count/format/empty-directory rejection. Directory-rename failure testing
and live startup invocation remain pending.

Tinta startup now calls `recoverExistingJournalMigration` before its existing
journal audit and UI allocation. It skips absent intents, blocks malformed or
unverifiable intents, and resumes authorized directory publication with the
legacy backup retained. Provider and audit buffers use checked short-lived heap
workspaces because their fixed buffers/handles exceed the task stack budget;
these are released before allocating the Tinta App. No migration is initiated
by startup in the absence of an intent. The initial copy/intent orchestration and
companion downgrade gate remain pending.

The HAL test provider now models whole-directory renames, including child files,
and faults before/after each directory rename independently of disposable index
renames. Integration tests use actual HAL journal audits and intent storage,
exercise both publication steps with lost acknowledgements, resume through the
startup helper, preserve the exact legacy header, and confirm the active extended
format and cleared intent. Tests also confirm absent intent causes no file writes
and malformed intent prevents moves. Physical SD power-cut and startup heap/time
measurements remain required.

`HalJournalMigrationSession::run` now orchestrates an explicit migration request:
recover pending publication, audit the active source and compute its frontier,
resume the exact extended candidate through the source identity index, check
journal handle closes, release copy/source workspaces, verify both generations,
persist JMG1, and publish through the recovery state machine. Already extended
active journals are idempotent successes; an existing legacy backup blocks a new
legacy migration. The session must be allocated outside the task stack with
`makeUniqueNoThrow`. Its fixed candidate scratch and retained handles exceed the
local budget; publication/audit/copy workspaces also use checked allocations and
are released before the next verification phase to limit simultaneous residency.
No allocation is made per event. `copyTo` requires a successful preceding audit
and consumes that authorization. Audit success now also requires a checked journal
handle close before exposing the frontier.

The HAL integration test migrates a nonempty legacy journal, loses the first
directory-rename acknowledgement, retries the whole operation, verifies the exact
legacy backup, and confirms the extended active journal and cleared intent.
This explicit operation is not yet dispatched by the companion; the downgrade
compatibility gate and physical SD interruption/heap measurements remain pending.

Release firmware assets now include `supportedJournalHeaderVersions`, populated
only by an explicit `--journal-header-versions` declaration to the release
manifest builder. Supported declarations are unique versions 1, 2, and 3,
serialized in sorted order; the omitted default is an empty list. Firmware
without companion protocol support cannot declare these capabilities. Release
operators must match declarations to the actual released binary. The manifest
field supplies compatibility evidence for the future updater; it does not yet
enforce a reader-side or Apple-side downgrade gate. Release-manifest tests cover
default absence of capabilities, explicit extended support, duplicates, unknown
versions, invalid types, and declarations inconsistent with protocol support.

Apple `FirmwareJournalCompatibility` consumes the release asset's declared
journal header versions. Missing declarations default to no support; duplicate,
unknown, and malformed declarations are rejected. Its compatibility check rejects
unknown reader state and any unsupported persisted format, including formats in
retained backups/candidates. A verified absence of all journals permits this
journal-only check, but board/image/state-schema checks still apply separately.
Unit tests cover legacy manifests, unavailable versus empty reader state,
extended active journals with legacy backups, malformed declarations, and
canonical encode/decode. This policy is not yet connected to the updater or
reader-state reporting, so it is not an enforced end-to-end downgrade gate.

`HalJournalFormatInventory` reads existing active/candidate/backup header files
without creating or recovering journals. It returns a bitmask of strict TJH1–3
headers, using the journal's shared header decoder (including CRC/count/parity
checks). Missing events, missing headers, wrong file types, malformed headers,
and lookup/read errors leave output unchanged and fail rather than declaring an
empty inventory. The companion parent directory must already exist. Lookup and
header buffers are session members; no new heap allocation is introduced by this
helper. It reports format requirements, not full journal/body integrity. A HAL
migration test confirms the combined legacy+extended mask after publication,
verifies no file changes, and rejects a corrupt retained backup without changing
the output. Authenticated protocol delivery and updater enforcement remain pending.

BLE command 14 (`JournalFormats`) now queries persisted header formats. Its
request payload is empty. Replies are `[0, mask]` on success (bits 0–2 = TJH1–3),
`[1]` when unavailable/invalid, and `[2]` when installation authorization fails.
Discovery advertises capability bit 2. The firmware handler verifies the current
installation/session/peer before allocating a checked short-lived format lookup
workspace, and scans without changing journal files. Existing wire command IDs
and descriptor size remain unchanged; older peers can reject the new command.

Apple `AuthenticatedReaderSession.journalHeaderVersions(requestID:)` checks the
advertised capability and uses the connection-bound authenticated transport.
`JournalFormatStatus` requires the expected response command/request ID, strict
status lengths, and no unknown mask bits; failure never becomes an empty inventory.
The result can feed `FirmwareJournalCompatibility`. Tests cover every mask byte,
error/truncated/oversized responses, and wrong request IDs/direction, alongside
frame authentication checks for command 14. This is not yet used by an updater,
and physical BLE query/reconnect verification remains pending. Wi-Fi dispatch for
this metadata query is not yet provided.

The encrypted Wi-Fi handoff now permits command 14 with the exact 16-byte bound
transaction ID as its request body. The shared request validator rejects wrong
transactions, lengths, and response-direction requests before dispatch. The
activity checks the installation owner and routes the query to the same format
scan/reply function as BLE. Apple `WifiHandoffTransport.journalHeaderVersions`
constructs the bound request and uses the existing encrypted exchange and strict
reply decoder. A transport test verifies successful encrypted delivery and
rejection of a foreign metadata request without another network exchange.
C++ binding tests include the query alongside status/commit/abort. Wi-Fi remains
disabled by its existing physical heap/radio qualification gate; enabling it and
end-to-end updater enforcement are still pending.

Apple `FirmwareReleaseCompatibility` decodes firmware asset compatibility fields
from release metadata and rejects contradictory board/chip, length/partition,
battery threshold, state-schema, and protocol declarations. Its admission check
requires matching reader board, current protocol support, at least the declared
battery threshold (never below 30%), known compatible state schema, a known
matching OTA partition size, and journal-format compatibility. Initial-upgrade
assets cannot be admitted for companion installation. This metadata check does
not validate firmware image bytes and is not yet invoked by an updater; firmware
remains unsupported by the transfer runner. Tests cover compatible metadata,
unknown/out-of-range reader state, low battery, wrong board/partition, and invalid
release declarations. Reader schema/partition reporting and image validation are
still required before enabling firmware transfer.

Apple `FirmwareImageInspector` now streams ESP image validation from a file with
at most 16 KiB segment chunks. It checks release length/partition bounds, header
magic/segment count/hash flag, chip ID, every segment extent, matching bounded
board tags (including tags crossing chunk boundaries), exact padded image length,
segment XOR checksum, and the optional SHA-256 trailer. It returns the SHA-256 of
the complete image, including the trailer, for binding to release asset metadata.
Chunk storage and SHA state are on the Apple device, not the reader's constrained
heap. Tests distinguish header/chip/board/checksum/hash failures, validate a tag
split across chunks, accept checksum-only images, and reject truncated images.
Release SHA binding, updater invocation, and reader-side flashing orchestration
remain pending; this does not enable firmware transfer.

Firmware asset decoding now requires a canonical lowercase 64-digit nonzero
SHA-256 declaration. `FirmwareImageInspector` compares its streamed whole-image
digest with that declaration before returning success, in addition to the image's
internal checksum and optional trailer. The tests include a different image with
its checksum and trailer recomputed correctly, which is rejected specifically for
release-hash mismatch. Malformed/uppercase/zero declarations are rejected during
metadata decoding. This binds validated bytes to the selected release metadata;
release discovery/download, immutable staging, reader-side revalidation, and
flashing/reconnect orchestration remain unfinished.

`ContentVault.importFirmware` now stages the supplied file through the existing
synced, locked, immutable import path with the release hash and length as expected
bindings. The firmware inspector validates the private stage before atomic object
publication. Exact repeated imports reuse the existing verified object. Tests
import a valid image twice, replace the source with a different internally valid
image, confirm that import fails and the original object remains exact, and check
that staging files are reclaimed. Byte validation/storage is now connected;
library/update metadata persistence, download UI, reader admission and transfer,
and flasher/reconnect integration remain pending.

SQLite schema 27 adds `firmware_assets`, keyed by the existing content hash with
bounded release-asset JSON metadata. `ContentImporter.importFirmware` requires
explicit asset metadata, publishes the validated immutable vault object first,
then commits firmware content and compatibility metadata together. Metadata hash
and length must match the content; contradictory compatibility metadata for an
existing hash is rejected transactionally. `firmwareCompatibility` reloads and
validates the stored binding. Generic file import does not guess compatibility
for arbitrary `.bin` files. Integration tests reopen the database, recover the
stored compatibility, and verify conflicting metadata leaves the original intact.
The complete portable Swift suite is the migration regression check; native Apple
builds, update UI/job orchestration, reader admission and flashing remain pending.

`LibraryStore.admitFirmwareTransfer` now reloads persisted asset metadata,
requires compatible known reader state and battery, verifies the immutable vault
object, reruns image/release-hash validation, and checks metadata again after the
asynchronous vault lookup. It returns only the verified object; it does not queue
or authorize flashing. Integration tests admit compatible stored firmware and
reject low battery and unavailable reader schema. Callers must obtain fresh
state from the authenticated reader, then recheck at reader installation time.
The existing transfer runner continues to reject firmware until reader staging,
flash admission, radio release, and reboot confirmation are wired.

`CompanionReleaseManifest` now parses bounded release JSON, validates schema,
version/tag/channel agreement and full lowercase Git revision, requires one
firmware asset for each supported board, rejects duplicate asset names/boards,
and binds HTTPS asset URLs to the configured GitHub repository and exact tag.
Stable releases are the default; RC metadata requires explicit opt-in. Each
firmware entry uses strict compatibility/hash decoding and can be selected by
reader board. Tests cover board selection, foreign repository URLs, default RC
rejection, explicit RC acceptance, and contradictory channel metadata. This
parsing/selection is not yet connected to release discovery, download UI, or
firmware installation.

Firmware compatibility metadata now has canonical Codable encoding with the
same release-asset field names, including lowercase SHA-256 and journal formats.
`ContentImporter.importFirmware(source, asset:)` accepts a selected manifest asset,
validates/stages its bytes through the vault, and persists encoded compatibility
with the release filename rather than the temporary download filename. Tests
check metadata round-trip, selected-asset import, stable library filename/title,
and unchanged content identity. Network downloading and update UI remain pending;
this bridge connects release selection to the existing validated import path.

Firmware image inspection now observes task cancellation before opening the
source, between bounded read chunks, and before returning a verified digest.
Cancellation propagates through the vault's existing staged-import cleanup.
The integration test cancels validation of an otherwise valid image, requires
`CancellationError`, and confirms the source bytes remain unchanged. Download
and updater orchestration remain pending.

`FirmwareDownloader` now downloads a selected asset to an owned temporary file,
imports it through release-bound validation/library persistence, and removes the
temporary file on success or failure. The URLSession transport uses an ephemeral
session without cookies/credentials, HTTPS-only redirect approval, timeouts,
progress cancellation above the declared length, exact completed length checks,
and HTTP 200 validation. Downloads are limited to the reader's 16 MiB flash
ceiling. The coordinator rejects concurrent work and observes cancellation.
Injected-transport integration tests cover successful download/import and corrupt
download rejection, verifying temporary-file cleanup in both cases. Actual
URLSession network/redirect/cancellation behavior still needs native Apple
verification; release discovery/UI and reader flashing remain unfinished.

Downloader lifecycle tests now hold a completed temporary file at the transport
boundary, reject an overlapping request, cancel the active task, release the held
transport, and verify cancellation cleanup followed by a successful retry. This
exercises the coordinator's busy-state and temporary-file ownership across actor
suspension. The URLSession progress delegate also cancels when the server's
reported total exceeds the release length; redirects with URL credentials are
rejected alongside non-HTTPS redirects. Native HTTP/delegate behavior remains
unverified, and updater UI/reader flashing are still pending.

The native Updates tab now exposes manual release-manifest import, stable-default
RC opt-in, reader-model selection, firmware download/cancel controls, and saved
firmware library entries. Manifest import reads at most 256 KiB plus one byte
from a security-scoped file; invalid input clears the selection. Changing RC
policy clears the prior manifest. Downloads use the validated importer, refresh
the library, and expose cancellation/errors through the existing app model.
New labels/errors have English string-catalog entries. The screen explicitly
states that reader installation is not available yet. Swift syntax parsing and
portable firmware tests pass; native SwiftUI compilation, accessibility, network
behavior and interaction checks require Xcode. Manual acceptance should import a
valid manifest, select the matching board, download/cancel, and confirm saved
firmware survives app restart. Release discovery and reader installation remain
unfinished.

`StableReleaseDiscovery` queries the configured repository's GitHub latest-release
endpoint, rejects draft/prerelease or malformed metadata, finds exactly one
uploaded `companion-release.json` asset, and binds its URL/size/tag before parsing
the manifest. It validates response sizes (API up to 1 MiB, manifest up to 256 KiB)
and requires manifest tag agreement. The URLSession implementation uses ephemeral
credential-free requests and checks HTTP 200/HTTPS and cancellation. Delegate
callbacks reject oversized declared lengths and chunks before appending them to
the bounded response workspace. Tests cover truncated/unknown-length responses,
cancellation before startup, late callbacks, actual delegate routing, and retry.
Injected tests also cover stable discovery,
prerelease rejection, and duplicate manifest assets. The Updates screen now offers
check/cancel controls with localized labels and retains manual manifest import as
fallback. Native UI/network checks and reader installation remain pending.


Reader replay ordering now selects causally ready events using study day, trusted
clock time (zero for other clock qualities), and numeric event identity. Explicit
ancestors and implicit per-origin predecessors must have been emitted first.
`JournalReplayOrder` stores the envelope in caller-owned session memory and uses
an abstract visitation store. `HalJournalReplayVisits` stores one bit per event
on SD through a retained HAL handle; reset discards stale marks. Its initialization
uses a fixed 64-byte flash constant and yields periodically. Marks are disposable
working state, not evidence that derived learner files were committed.

`HalJournalCausalAuditSession::replay` consumes a successful audit, opens its
validated identity index, allocates the selector once with `makeUniqueNoThrow`,
and invokes a visitor with borrowed event/body views. Checked file cleanup runs
on completion and selection/visitor failure. Host tests cover ordering, untrusted
clocks, visitation failures, reset, audit admission, and actual HAL iteration.
The current selector rescans all unvisited records for each emission, requiring
quadratic record scans in the worst case; hardware SD timing remains unmeasured.
Rebuild replay can optionally collect validated undo targets into a second SD
bitset, mark those reviews in the visitor, and omit undo records. Selection still
emits their dependency marks, preserving causal readiness for subsequent events.
Repeated undo records exclude a target once; concurrent reviews remain included.
HAL tests exercise this through the real identity index and journal. Derived-state
rebuilding/publication and live mutation recovery integration remain pending.


`TintaReplayReducer` applies the ordered stream to caller-owned disposable candidate
storage through per-item/per-day operations and completion-set updates. An undone
review retains a fresh item entry when needed, matching Apple snapshot behavior,
without grading or incrementing totals. Applied reviews reuse the portable scheduler
and aggregate new-item, daily-review, grade, correct-grade, and exact millisecond
counts with overflow checks. A failed write invalidates the reducer; partially
written candidates must be discarded and cannot be published. The reducer adds no
heap allocation or history-sized container. Tests verify undone-item preservation,
scheduler results, counts, and failure latching. Canonical snapshot serialization and live startup reconciliation remain pending.


`HalTintaReplayStore` provides course-bound disposable `replay-work` records through
one retained HAL read/write handle. Fixed 36-byte records separate item, day,
lesson-completion and reading-completion namespaces, with header/record CRC checks.
Missing values initialize to fresh items, zero totals or disabled completion.
Repeated updates reuse a slot; read, integrity and sync failures invalidate access
until reset. Lookup currently scans records, so its SD cost also requires hardware
measurement. No active learner file is modified.

`HalTintaReplaySession` audits causal closure, undo targets and membership against
the caller's immutable course catalog, then runs marked replay through the reducer
into that store. Audit and reducer workspaces use checked one-time heap allocations
because they exceed task-local storage limits; they are released after replay.
The working store and journal frontier are exposed only after success. HAL tests
cover concurrent-review preservation, flags/totals, repeatability, and withholding
results after membership failure. Canonical export, manifest validation/publication,
and live learner-state startup integration remain unfinished.


Canonical replay export now streams the complete five-file candidate set: sorted
item records with dual TIS1 headers, an empty local review log, enabled sorted
lesson/reading completion sets with CRCs, and sorted TDL1 daily totals. Day export
rounds accumulated milliseconds once per day and splits 16-bit record parts.
Export performs format readback and streams length/SHA-256 receipts. TDS1 encoding
binds receipts to the exported course/day plus caller-provided storage, pack,
frontier, snapshot identity and revision; invalid encoding preserves caller output.

Proof export uses separate `*.proof` paths, preserving candidates and active files.
`HalTintaDerivedJournalProof` reaudits course membership/closure/undo targets,
replays the frozen journal, and compares its actual frontier and all five rebuilt
receipts against the proposed manifest. `publishProvenTintaDerived` wires that
callback into the existing intent/backup/receipt publication backend using checked
session allocations. Host tests verify forged receipt/frontier rejection, unchanged
candidates during proof, a lost candidate-rename acknowledgment followed by retry,
and repeated publication. These helpers are not yet dispatched by the live reader;
actual pack/storage binding, startup recovery, mutation capture and physical SD
power-cut/heap/timing acceptance remain unfinished.


Bound-course Tinta startup now invokes `recoverBoundTintaDerivedPublication` after
journal-format migration and before opening the learner UI. The checked, temporary
workspace reads a pending intent first; absent intent returns before provisioning
identity or validating the pack. Pending recovery requires the installed course
binding, exact pack length/SHA-256, current provisioned storage generation, complete
pack validation, and a prepared immutable subject catalog. It passes the pending
manifest and previous receipt to journal-proven publication. Failure prevents learner
files from opening. The workspace and pack handles are released before App allocation.
The C3 default firmware build passes without warnings. Host publication tests cover
replay proof and lost-rename recovery; startup-path host tests now cover absent/malformed intents, missing installed
binding and directory I/O without identity writes. Physical SD power cuts/heap
observations remain pending. Mutation capture and incoming-event
exchange still require live integration.


The direct startup host test now uses the actual mini.pack fixture, a provisioned
identity, catalog validation, a real HAL-backed journal event, replay/export, and
an installed pending manifest. It simulates an item candidate already renamed to
active: startup finishes the other four files, publishes its receipt, clears intent,
and makes a repeat startup a no-op. Changing the card CID creates a new storage
generation and rejects the old pending manifest without changing learner files.
All 35 course-transfer host tests pass. Physical power-cut and resource acceptance
remain outstanding.

Every new causal audit attempt invalidates its previous replay authorization,
including rejected course/catalog arguments. Verify this with
`HalTintaJournalStorageTest`: the replay test performs a successful audit,
rejects an invalid audit request, and confirms that no visitor runs until another
audit succeeds. All 22 HAL journal tests pass; this guard adds no allocation.
