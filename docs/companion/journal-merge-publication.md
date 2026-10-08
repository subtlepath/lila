# Journal merge publication recovery

`CompanionJournalMergePublication.h` defines recovery for publishing a merged
journal with a different frontier/count from the active journal. The older
format-migration publisher requires equal histories and cannot authorize this
operation. The new recovery protocol is portable, with HAL intent/receipt
record persistence, a HAL publication provider and separate candidate/backup
paths. A resumable HAL candidate session now constructs and publishes the
journal. Tinta startup now recovers pending merge publication and reconciles
completed merges into verified learner snapshots. Full incoming transport dispatch,
native event capture and legacy learner-baseline migration remain unfinished.

Before writing intent, the caller must exclude journal writers, audit the active
and candidate journals, and prove that the candidate retains every previous
event with its exact envelope and body. Incoming histories must be merged and
deduplicated before this point. A candidate audit alone does not prove retention.
The intent binds the SD generation, transaction and installation plus separate
previous/merged snapshots. Each snapshot contains count, record size and global
frontier. The merged journal uses 1,024-byte records. Equal counts require equal
frontiers. The publication workspace uses fixed values smaller than 256 bytes;
the protocol allocates no heap buffers.

Recovery accepts only these directory configurations:

| Active | Candidate | Merge backup | Action |
| --- | --- | --- | --- |
| Previous | Merged | Absent | Verify both, rename active to backup, then candidate to active |
| Absent | Merged | Previous | Verify both, rename candidate to active |
| Merged | Absent | Previous | Verify both, finish transaction receipt and retire intent |

All other configurations conflict. A changed SD generation conflicts before
directory inspection. Rename failure may mean a lost acknowledgment; a later
call inspects directories and resumes the appropriate state. The caller's
storage implementation must audit each snapshot during verification, close
all journal handles before renaming, and durably write an idempotent completion
receipt before retiring intent. The backup remains available. A policy for
retiring prior merge backups before subsequent transactions still needs
implementation; merge backups must not reuse the retained format-migration
backup.

`CompanionJournalMergeIntent.h` encodes a 136-byte `JMP1` intent. Integers are
little endian: generation at 4, installation at 20, transaction at 36, previous
count/record size/frontier at 52/56/60, merged fields at 92/96/100, and CRC32 over
the first 132 bytes at 132. Reserved bytes 58–59 and 98–99 must be zero.
Decoding preserves its output on invalid length, version, reserved bytes, CRC
or snapshot fields. Encoding preserves its output on invalid arguments.

`CompanionJournalMergeRequest.h` defines bounded incoming request payloads
for `ExchangeChanges`. The 28-byte header contains `JMR` plus version 1,
operation at byte 4 (begin 1, append 2, commit 3, abort 4), zero reserved bytes
5–7, transaction identity at 8, and little-endian envelope/body lengths at
24/26. Begin, commit and abort carry the complete 136-byte `JMP1` declaration
as their body, with zero envelope length; the declaration must name the same
transaction as the header. Append carries the encoded sync envelope and
mutation body. Even their maximum combined sizes fit a 1,024-byte control
payload. Export cursor requests retain their existing 40-byte format.

The parser borrows request bytes and allocates no buffers. It rejects malformed
headers, reserved bits, invalid declarations, transaction mismatches and size
violations without modifying its output. Append envelope decoding, body hash
validation, authenticated-owner/SD-generation authorization and course catalog
validation remain responsibilities of the receiving session. These requests
are now dispatched by authenticated BLE and Wi-Fi firmware, and no incoming capability is
advertised. Apple's `JournalMergeRequest` encodes the same declarations and
operations, checks snapshot constraints, and validates typed mutation bodies
before encoding append requests. `JournalMergeBegin-v1.fixture` and
`JournalMergeAppend-v1.fixture` are independently assembled wire bytes checked
by both Apple and C++ tests. `AuthenticatedReaderSession.exchangeJournalMerge`
checks declaration owner/generation and validates the 24-byte response's
version, result, reserved bytes, echoed transaction and frame request ID.

`HalJournalMergeReceiveSession` connects parsed requests to the real HAL
candidate session. It compares declaration owner/generation with the caller's
authenticated identities before begin, commit or abort, and checks append's
transaction against the retained declaration before decoding its envelope.
Commit requires a matching begin in this session; a resumed caller begins
again with the same declaration. Completed begin/commit retries report
duplicate. Closing the session releases handles while preserving resumable SD
state. Its envelope and candidate workspaces live in a checked heap allocation
owned by the caller, not in per-request task locals. The HAL fixture exercises
begin, actual append, duplicate append and abort, and verifies foreign owner,
generation and transaction rejection without file changes. BLE allocates it
once per receiving connection and releases it on terminal commit/abort,
disconnect, reauthentication, bond removal, low heap, Wi-Fi transition or
activity exit. Invalid requests with no bound transaction release it as well.
Content transfers cannot overlap a bound receive session; exports close before
receiving and cannot restart until it ends.

For a Tinta suffix, BLE now allocates `HalTintaJournalMergeCommitContext` to
verify the installed pack's binding, length and SHA-256, decode the canonical
learner receipt, check its old frontier/generation/course/pack binding, and
verify all five active learner-file hashes. Only then does it open the actual
pack catalog and retry commit with that course/catalog. Missing or changed
baselines fail before journal publication. Non-Tinta suffixes do not require
this catalog. The context is released before replay; after a successful commit
or completed retry, BLE resolves the installed course and runs merged learner
reconciliation. Failure blocks the session pending recovery, preserving the
completed journal receipt for startup. Native event capture and legacy
baseline migration still need implementation before enabling general history
synchronization or advertising its capability.

Host tests reject changed pack bytes, wrong generation and changed learner
files without changing SD files, then validate the real fixture pack and use
its catalog to authorize the merge. Hardware peak heap, task stack and
power-loss behavior remain unverified. Monitor the companion and Tinta debug
heap logs during commit/replay on C3 and verify both retained old journal and
canonical learner files after each interrupted publication step.

The BLE reply is 24 bytes: version 1 at byte 0, `TintaJournalResult` at 1,
zero reserved bytes at 2–3, echoed transaction at 4–19, and little-endian durable
candidate count at 20–23. A completed retry reports the merged count. Malformed
or unavailable requests can instead return the existing control error frame.

Wi-Fi prefixes each request with its 16-byte authenticated handoff transaction.
The inner merge must name that same transaction; declarations must also match
the handoff installation and SD generation. Apple decodes and validates the
inner request before encrypted sending. Firmware validates those bindings
before dispatching through the same `journalExchangeReply` path as BLE.
Maximum append payloads including the prefix remain below 1,024 bytes. Tests
cover declaration binding rejection and an encrypted Apple begin/reply, plus
foreign transaction rejection without additional traffic.

The existing Wi-Fi handoff is initiated for a receiving/verified content
transfer. Journal reception conflicts with unfinished content installation,
so the current handoff can serve a merge only after that content transaction
commits or aborts, using the same transaction identity. An independent history
handoff entry path remains required. Wi-Fi radio activation is still gated on
the planned hardware acceptance tests; these changes do not enable it.

`HalJournalMergeRecordStore` stages, syncs, closes and reads back each record
before renaming it into place. An existing equal intent is a no-op; a different
pending intent conflicts. Completion receipts may replace the preceding receipt
only after verifying the new stage and checking the prior destination again.
The publication provider must keep intent durable throughout this replacement;
interruption after removing an old receipt can then regenerate the new receipt
before retiring intent. A lost rename acknowledgment is handled by reloading
the destination on retry. Malformed records and I/O errors preserve the caller's
output and prevent cleanup. These fixed buffers and lookup handles belong to a
session workspace rather than the small task stack.

Merge journals use `tinta-events-merge` and `tinta-events-previous`, separate
from `tinta-events-next`/`tinta-events-old` format migration paths. The firmware
journal-format query now includes both merge locations, so pending or retained
journals participate in firmware compatibility checks.

`HalJournalMergePublicationStorage.authorize` audits both expected snapshots,
then opens independent journals and verifies that each active envelope and body
appears unchanged at the same index in the candidate's prefix. Incoming events
must be appended after this prefix; physically reordering prior events is not
accepted by this candidate-construction contract. The retention workspace also
checks reopened counts and record sizes, and closes both files before intent
publication. Audit and retention workspaces use checked heap ownership and are
released between phases; their fixed buffers exceed the task stack budget.
Recovery verifies journals with the actual causal/undo/frontier audit. Completion
requires the matching pending intent, persists its receipt and then clears that
intent. Repeating completion after intent removal checks the exact receipt.

The HAL test builds real 512-byte active and 1,024-byte candidate journals,
authorizes retention, simulates a lost acknowledgment after the old directory
rename, and completes recovery with the old events in the separate merge backup.
It verifies the completion receipt and idempotent retry. A separately audited
candidate that changes a prior event's body is rejected before any directory
rename or intent publication.

Incoming candidate writes can use `TintaJournal.appendBounded` with the declared
merged count. Duplicate/equivocation checks and causal predecessor checks run
before the capacity guard, so retrying the final durable event succeeds without
another write while a new event cannot exceed the declaration. A lost final
header acknowledgment makes the journal unavailable until reopening; after
recovery the exact retry returns `Duplicate`. The bound adds no allocation or
extra journal scan. `HalJournalMergeCandidateSession` uses this primitive for
each delivery. Its durable `journal-merge-receiving` declaration precedes
candidate creation, binds both snapshots and the installation/transaction/SD
generation, and conflicts with a different receiving transaction. Reopening
resumes an interrupted exact-prefix copy or validates the retained prefix of a
partially appended candidate. A premature commit leaves the active journal
unchanged. At full count, commit audits, authorizes and publishes the candidate,
then clears the receiving checkpoint. Repeating begin after an interrupted
publication finishes recovery; a completed matching receipt acknowledges the
original transaction without starting another candidate. Publication intent
and receiving checkpoints are distinct, so partially received histories cannot
authorize a rename.

Allocate this session with checked heap ownership and close it when its owning
activity/session ends. Begin and commit allocate short-lived audit/publication
workspaces; append reuses the retained candidate journal and body/envelope
buffers. The host test covers cloning the old journal, stopping after one new
event, restart, rejecting a foreign installation, duplicate delivery, declared
capacity, premature commit, a lost directory-rename acknowledgment, transaction
receipt recovery and checkpoint cleanup. Native transport and derived-state
reconciliation still need integration before enabling incoming merges in the
reader UI. A previously retained merge backup currently blocks the next merge;
backup retirement policy remains unfinished.

`HalJournalMergeCandidateSession.abort` requires the matching receiving
declaration and SD generation, and refuses a pending or completed publication.
It persists `journal-merge-aborting` before closing the candidate and deleting
known candidate files. A restart with this marker cannot resume reception.
Only an empty candidate directory is removed; unexpected files remain intact
and keep cleanup pending. A durable per-transaction receipt at
`journal-merge-aborted-<transaction hex>` precedes checkpoint retirement, so
late retries cannot recreate a cancelled job even after newer jobs have run.
Repeating abort completes checkpoint cleanup after a lost receipt acknowledgment
without touching newer transaction checkpoints. Active journals, merge backups
and format-migration backups are never deleted by abort. Fixed path buffers and
record workspaces live in the checked heap session; no per-file allocation is
added to cleanup. Abort-receipt retention/garbage collection and transport/UI
integration remain unfinished.

Commit now requires course/catalog arguments, or an explicit null pair for a
merge containing no new Tinta events. `JournalIncomingCourseValidation` checks
every Tinta subject appended after the retained active prefix. It rejects a
missing catalog, foreign course, missing subject or catalog I/O failure before
publication intent creation. The caller must bind this immutable, fully
validated catalog to the selected installed pack; permitted retired item IDs
are supplied by that catalog. Unchanged older events remain protected by exact
retention proof and can refer to previously active courses. Newly appended
events from another course require the explicit course-switch/state-isolation
workflow, which remains unfinished. Reading/bookmark/preference-only suffixes
do not need a Tinta catalog.

The HAL test confirms those validation failures preserve active journal bytes
and leave publication intent absent, then permits a retry with an available
catalog. Portable validation tests cover reading-only suffixes, retained old
course history, foreign new courses and malformed catalog arguments. These
checks add fixed session-owned state and no heap allocations.

`HalJournalMergeStartupRecovery` runs after legacy-format migration and before
derived-file recovery or opening Tinta. An absent intent returns without
reserving an event epoch. Malformed intents fail before identity writes. A
present valid intent provisions the current hardware/SD identity, then uses
the publication recovery protocol above and retires the matching receiving
checkpoint. Changed-card generations preserve all journal files. Its retained
record buffers and storage handles require a checked heap workspace, released
before the Tinta app is allocated.

Companion Connect also runs publication recovery after content-transfer
recovery and before inventory/radio initialization. It supplies its already
provisioned SD generation, avoiding another epoch reservation for publication
recovery. After releasing that workspace, it resolves the installed course and
reconciles completed learner merges, releasing commit-context storage before
allocating replay workspaces. Publication or learner recovery failure blocks
the connection. The real-pack HAL fixture verifies recovery through this
generation overload without identity-binding changes and verifies that a
different generation leaves the interrupted journal files untouched.

Recovery also handles a completed receipt whose matching receiving checkpoint
survived after publication intent retirement. It requires the same current SD
generation, no candidate directory, and a full audit of the active merged
snapshot before clearing that checkpoint. Corrupt active bytes or changed
generation preserve it and fail recovery. An unrelated unfinished receiving
transaction is preserved without reserving an identity epoch. The Tinta startup
lookup checks both intent and receiving paths so it reaches this recovery state.
Host tests exercise those boundaries; on hardware, interrupt power between
publication intent removal and receiving checkpoint removal, then verify that
startup retains the active merged journal and removes only its matching
checkpoint.

`HalTintaMergedJournalReconciliation` handles a completed merge receipt. New
Tinta events require the installed course binding and pack hash, a prior
canonical snapshot matching the old frontier, and unchanged hashes for all
five active learner files. It replays the merged journal against the actual
pack catalog, exports candidates, and publishes all five files through the
existing proven derived-publication protocol. Changed local baseline files
fail closed. A course-scoped applied marker makes repeat startup a no-op;
merges without new events for this course audit the journal and mark completion
without replacing its learner files. This does not yet migrate legacy learner
files or enable native journal capture.

The real-pack HAL fixture interrupts commit after the old journal directory
rename, rejects recovery with a changed card, recovers with the original card,
and verifies that replay preserves an existing star while applying a received
suspension. It also checks unchanged files and identity bindings on repeated
startup. On hardware, use debug logging to inspect Tinta's `Before open` and
`Opened` free/largest internal-heap readings, and interrupt SD publication at
each rename to verify recovery. Host tests do not prove physical power-loss
durability or that peak heap use fits the C3 ceiling.

Companion startup now calls `recoverExistingJournalMergeAbort` after identity
provisioning and before enabling radios. It reads the durable abort checkpoint,
rejects a different current SD generation, and resumes the existing exact-file
cleanup protocol. A malformed checkpoint or cleanup failure blocks startup;
the active journal and unexpected candidate files are preserved. Its checked
heap workspace keeps record buffers and retained handles off the task stack
and is freed before radio allocation. The HAL fault fixture tests a lost abort
receipt rename acknowledgment, changed-generation rejection without file
changes, successful startup recovery and repeated no-op recovery. Verify on
hardware by interrupting abort cleanup and reopening Companion Connect;
check that only the candidate disappears and the active history stays usable.
Completed merge-backup retirement remains unfinished.

Run `CompanionTintaJournalTest` to verify rename/receipt interruption recovery,
changed-card and corrupt-snapshot rejection, ambiguous directory combinations,
and intent round trips/corruption. These portable fault tests do not establish
physical SD power-loss behavior or complete incoming-history synchronization.
