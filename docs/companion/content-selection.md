# Reader content selections

`LibraryStore.setReaderSelection` persists a desired choice by hardware reader identity and SHA-256 content ID. SQLite schema 8 keeps these rows separate from shared library metadata and requires the content to exist. EPUBs, courses, fonts, and dictionaries use this path; firmware installation requires its separate explicit update flow.

The current source advertises EPUB, font and dictionary removal through authenticated
BLE and encrypted Wi-Fi commands. A retained owner prepares complete-path plans and publishes
metadata before retiring quarantined bytes. Resource admission covers its owner,
JSON preparation and startup cohort allocations; low/fragmented-heap refusal and
retry pass the native SdFat checker. Native Apple and physical acceptance remain
unverified. Course removal still needs its own participant. Dictionary removal
uses sealed member-proof cohorts, preserves unowned folder siblings and cached
archives, and recovers retained journals before reading resumes.
Font cohort routing and recovery pass the native host checker and all five
firmware builds/image checks; physical acceptance remains pending.

Reader-only inventory content can be imported through the native Installed content
section using authenticated metadata and bounded BLE or encrypted Wi-Fi reads.
The exact reader/card/manifest binds a durable import job and immutable filename.
Downloads synchronize before SQLite checkpoints, recover saved offsets, verify
SHA-256 and kind, and atomically publish library metadata and reader selection.
Courses retain their inventory logical identity and inspected pack metadata.
Firmware is excluded from this flow.

Import/Resume, Pause, saved-progress, and Cancel controls are wired. Cancellation
persists aborted intent before deleting owned staging and leaves reader content
and completed library objects intact. Globally removed content is excluded from
automatic re-import; deletion during a download or admission aborts that job.
Deselection atomically aborts active imports for that reader and content,
including imports retained across card generations. Publication cannot override
the newer choice; library metadata and other readers’ selections remain intact.

Wi-Fi import closes the handoff on validation and download failures, including
missing jobs, without advancing the durable offset.

Terminal staging cleanup scans bounded pages and refuses foreign bindings. It
keeps the binding until payload deletion is synchronized, then removes the
binding and empty directory. Restarted cleanup accepts an empty directory but
refuses a nonempty directory without its ownership proof.

The native action offers Wi-Fi for more than 1 MiB remaining when assistance is
enabled and the reader advertises export support. It shares transaction IDs and
durable offsets across transports. Lost-reply tests reopen storage and resume
through a fresh authenticated BLE session. The native import action now reuses
the upload reconnect helper after success or a non-cancelled Wi-Fi failure,
requiring the same reader, card generation, and installation before refreshing
inventory. A failed reconnect preserves completed imports and asks the user to
reconnect. This native flow still needs Apple build and physical verification. Manual and foreground Resume retain the existing import job ID. Manual Resume
uses the bound filename even after a reader-side rename, and can bind a missing
filename to an older interrupted job without enqueuing a replacement. Cancelled
jobs refuse metadata resume. An authenticated transport test cancels the stored
job while metadata is in flight and proves no filename binding, content read,
or replacement job occurs. Resume also checks job ownership before metadata
exchange, and tests refuse foreign reader/card/installation jobs with zero
metadata requests; library metadata arriving from another app no
longer hides Resume for a matching active job. A full authenticated restart test reopens SQLite and staging, binds metadata
to the same paused job with 961 bytes acknowledged, starts the first content
read at offset 961, and verifies the final vault object matches every source
byte. Schema-39 interrupted imports can acquire their first authenticated
filename without losing phase or offset. See protocol.md for wire formats,
resource ownership, binding checks, and recovery details.

All 506 Swift tests and 1,748 host entries pass. All five dictionary-enabled
firmware profiles build, and their saved images pass chip, board, segment bounds,
checksum, SHA-256 trailer and OTA-size checks through the release validator.
These images do not include the unconnected course-removal components.
Native source syntax/localization checks pass, but native
Apple compilation, physical reader operation, power-cut recovery, accessibility,
and measured runtime heap/stack acceptance remain unverified.

Local family deletion now checks settings persistence before removing files.
Deleting the selected family first publishes the built-in fallback through the
existing preference journal/settings path. Every retry checks persistence again,
including after a failed save left the in-memory selection cleared. No new
workspace is allocated; this is one checked save per explicit deletion request.
Run `python3 test/companion/font_installer_fault_check.py` to compile the actual
installer with deterministic fake storage/settings failures. It verifies save
refusal before any removal, retries after an in-memory fallback, partial deletion
across both roots, invalid-name/oversized-path refusal, and an argument aliased
to the active settings buffer. It also checks a deletion that takes effect but
reports failure, preserving the surviving root until a checked retry, and
retaining another selected family. The exact path-capacity check accepts a
151-byte family name, preserves an unrelated directory, and refuses 152 bytes
before saving or removing anything. Deletion retains one checked 160-byte path and
reuses it for both roots; it adds no allocation or second path buffer. The check is registered as `FontInstallerFaultCheck` in CTest and passes; the
C3 build and saved-image validation pass for the installer guard. A later UI
change retains a failed deletion’s family index, routes both Confirm and touch
Retry back to deletion, uses translated deletion errors, and refreshes registry
state after every attempt. It adds one integer member; the web error reuses its
JSON document and existing string-serialization pattern for translated text.
All five profiles pass the final UI build and saved-image validation. Physical
failure-injection verification remains pending.
Companion-driven font removal still needs settings/reference recovery and native
activity/startup routing. The reusable byte participant now accepts validated
single-file EPUB and font plans (`HalSingleFileRemovalParticipant`); EPUB callers
retain their existing aliases. The real-SdFat host checker covers EPUB, TTF, OTF,
TTC and bitmap CPFONT files across nine success/fault cases each: rename failure
before/after effect, uncertain deletion, corrupt source/backup, publication failure,
foreign journal ownership, restart and repeated completion. It preserves an
unrelated sibling file. The references in this matrix are fakes, so it does not
prove font preference persistence or physical SD recovery. Run
`python3 test/companion/sdfat_path_lookup_check.py .pio/libdeps/default/SdFat/src`.
This shared worker adds no allocation or extra buffers; it retains the existing
fixed off-stack owner and borrowed hash scratch. A final firmware build batch for
this extraction is running; the saved images in the hardware table predate it.
Font removal remains unadvertised until reference recovery and routing are complete.

Font removal codecs and single-file plans use vector format 1 and bitmap format
4, matching inventory and transfer manifests. Formats 0, 2, 3, and 5 are rejected;
TTF/OTF/TTC and bitmap extensions must agree with the manifest. This validates
the shared contract; dependency-aware native font removal remains incomplete.

## Selection and removal implementation notes

The following incremental notes retain earlier validation details; current export
status and verification limits are stated above.

Selections survive app restart and SD replacement because they describe the desired content for the physical reader. Actual installed state must come from the authenticated inventory for the current SD generation. A false selection is retained as a removal request. It does not delete library metadata, content objects, or another reader's selection. Global deletion uses the separate explicit library action described below.

Identical choices return false without updating SQLite. Changed or newly recorded choices return true. `readerSelections` returns both selected and deselected rows in content-hash order. The reconciliation runner combines them with current inventory and durable jobs; completion still requires acknowledged reader operations.

Run `swift test --package-path apple/CompanionKit`. `ReaderSelectionTests` verifies independent reader choices, retained library copies, restart recovery, repeated-choice suppression, and firmware/invalid-reader rejection. Reader reconciliation, install/removal commands, and native UI remain pending.

`ContentManifest` now decodes the shared 63-byte record into typed hash/kind/length/version/course fields. `ReaderInventory` binds collected manifests to reader identity and SD generation, coalesces identical repeated entries, and rejects conflicting entries for one hash. Its complete flag must be set only after the authenticated inventory exchange finishes successfully.

`LibraryStore.reconcileContent` proposes deterministic installs/removals from saved choices and a complete matching inventory. It refuses incomplete or mismatched inventories and kind/length conflicts. Unselected inventory content with no recorded choice remains untouched. Already installed selected content produces no action, and deselected absent content produces no action. This is a read-only planner: the executor still needs to account for existing transfer jobs, dependencies, supported formats, and ambiguous commits before executing or creating jobs. Reader inventory exchange and removal commands remain pending.

The selection regression now covers typed decoding, repeated inventory entries, install/removal proposals, unchanged inventories, incomplete scans, and changed SD generations. All 134 Swift tests pass.

`LibraryStore.reconcileContentWork` now overlays durable jobs on these proposals. For the current reader/card, one pending job per content takes precedence: selected paused/queued work resumes; deselected work can abort before commit; committing work requires status/commit recovery even after deselection. Failed jobs, jobs without a recorded choice, and jobs owned by another Apple installation require inspection. Multiple pending jobs for one current-card content fail as conflicting work. Changed-card jobs are reported separately and never resumed against the new SD generation. The planner preserves all job rows and performs no reader writes.

The regression covers paused-job reuse, deselection abort, commit recovery, installation mismatch, and changed SD generation. All 134 Swift tests pass. Before execution, the runner must recheck current selections, durable job phase, and authenticated session identity/generation. New installs need supported-format/dependency checks; removal commands and authenticated paginated inventory collection remain pending.

`LibraryStore.enqueueSelectedContent` now rechecks selection and creates/reuses its job under one SQLite immediate transaction. A pending job for the same reader/card/content is reused only for the same installation; multiple jobs or foreign ownership fail, and failed work requires explicit inspection instead of implicit restart. Deselected or unselected content cannot be queued from a stale plan. Existing queued/paused/committing phases are retained. Firmware remains excluded. Completed/aborted jobs do not prevent a new transfer when a complete current inventory shows installation is needed.

The new regression concurrently opens two SQLite handles and verifies one transaction ID, paused offset reuse, foreign-owner rejection, and deselection rejection while preserving the pending job. All 135 Swift tests pass. The executor must still use current authenticated inventory to decide whether a new transfer is necessary, validate supported content/dependencies, and recover commit state before removal.

The authenticated `TransferRunner.run` entry point now requires selected content before starting a non-committing job and rechecks between chunks. `LibraryStore.commitSelectedTransfer` checks current selection, full durable offset, content kind, and job phase in the same SQLite transaction that records commit intent. A remotely verified staging file does not bypass this barrier. Work already durably committing remains recoverable after deselection; it must finish recovery before a later removal can proceed. The internal injected-transport entry point retains an explicit selection-check parameter for host tests.

Tests verify no reader requests for unselected work, rejection of incomplete commit intent, and successful lost-commit recovery after deselection. All 136 Swift tests pass. A deselection during streaming leaves paused staged work for the job-aware abort path; the abort executor and reader removal commands remain pending.

SQLite schema 9 now persists abort intents before reader requests. `TransferRunner.abort` binds the authenticated installation, reader, card, and transaction; it marks the job aborted only after a validated abort reply or the recovered reader's explicit no-transaction response. Lost replies retain intent for retry. Committing/completed jobs cannot acquire abort intent. Pending intents block resumption, entering commit, and selected-job reuse; work reconciliation continues abort recovery even if the user reselects the content. Repeated completed aborts return the terminal job without another request.

The abort regression interrupts a chunk, loses the abort reply, reopens SQLite, verifies that resumption is blocked, retries abort, and checks the reader's durable offset and repeated terminal result. All 137 Swift tests pass. Authenticated inventory collection, reader content-removal commands, and physical abort verification remain pending; aborting a staged transfer does not remove an installed library item.

Abort recovery now takes priority over a failed local phase or absent choice when the pending intent belongs to the current installation. Foreign-owned work still requires inspection. Additional tests verify that an authenticated no-transaction reply completes an unstarted abort, repeated terminal aborts send no new requests, and committing jobs neither send abort requests nor persist abort intent. The lost-reply test also verifies abort planning after a failed local phase. All 138 Swift tests pass.

SQLite schema 10 now stores explicit library deletion markers. `deleteLibraryContent` atomically hides the item from `libraryContentIDs`, deselects known readers, and persists abort intent for nonterminal pre-commit jobs. Committing jobs retain recovery intent. Metadata and immutable file objects remain available for recovery; this API does not physically erase installed reader files or vault objects. Repeated deletion returns false without additional updates. Reimporting metadata does not clear a deletion marker. `restoreLibraryContent` explicitly restores library visibility but does not reselect readers or reverse pending aborts.

`LibraryDeletionTests` covers restart persistence, retained metadata, two-reader deselection, pre-commit abort intent, commit preservation, reimport behavior, and explicit restore. All 139 Swift tests pass. Native confirmation UI, CloudKit deletion propagation, object reclamation, and reader removal remain pending.

Global deletion is now considered during inventory reconciliation even when the authenticated reader has no saved choice for that item. Matching deleted non-firmware content produces a removal proposal; kind/length mismatches still fail. Deleted content cannot produce an install proposal from a stale selected row. Selection validation/writes and explicit enqueue deletion checks now occur under SQLite immediate transactions, closing cross-handle races with deletion. The deletion regression covers an unrecorded reader's inventory and concurrent selection/deletion, verifying deleted content remains deselected. All 139 Swift tests pass. Physical removal execution remains pending.

The transfer fixture now changes desired selection while acknowledging a chunk. `TransferRunnerTests.testDeselectionDuringStreamingAndAtLastChunkNeverCommits` exercises both the first 1000-byte chunk and the final chunk, verifies paused durable offsets and zero commit requests, then completes abort recovery. Run `swift test --package-path apple/CompanionKit --filter TransferRunnerTests` to reproduce these host protocol checks. All 140 Swift tests pass; this does not substitute for physical BLE/SD acceptance testing.

Native EPUB removal now has a HAL participant that hashes the original file, quarantines it under the plan digest, verifies reference publication through a required provider, and retires the backup only after the committed journal phase. Recovery checks request ownership and both journal slots before mutations. The checked path lookup uses the installed SdFat Unicode comparison rules and reports enumeration errors separately from absence. Host fault checks cover rename failures before/after effect, retirement failure after effect, reference publication failure after effect, corrupt quarantined bytes, foreign owners, reconstructed sessions, and duplicate completion. Reading-history files remain untouched by this participant.

The participant is not yet connected to reader commands or boot recovery. A concrete reference provider must clear `CrossPointState::openEpubPath` and matching recent entries, prove their durable publication, and preserve per-book progress/bookmarks. `RecentBooksStore::removeByPath` currently returns success despite failed persistence, so it cannot be used as the transaction's publication proof. Font, course, and dictionary removal require their own dependency-aware participants. Run `python3 test/companion/sdfat_path_lookup_check.py .pio/libdeps/default/SdFat/src` after installing firmware dependencies to reproduce the EPUB/filename checks; these checks use a fake HAL filesystem and do not prove SD power-loss behavior.

Reference cleanup must also recover staged metadata before loading the reader's state/recent stores. The current SDK `writeFile` stages a complete `.tmp` but its replacement removes the old destination before rename; its comment describes recovery on read, without implementing it there. Reusing ordinary `saveToFile` alone therefore does not prove crash-safe publication of unrelated settings or recent entries. The removal provider needs owned, recoverable metadata snapshots and checked readback before the removal journal can advance to Committed. An additional host test invalidates journal ownership inside the reference callback and verifies that the quarantined EPUB survives; a reconstructed owner then recovers normally.

`HalRemovalMetadataPublisher` now provides the checked rename/recovery mechanism for `/.crosspoint/state.json` and `/.crosspoint/recent.json`. Its declaration binds the complete removal request and plan hash to the previous/next byte lengths and SHA-256 values. A mandatory provider callback must prove that declaration is durably owned and the enclosing journal still authorizes publication; an in-memory callback alone is not sufficient in firmware. Private candidate/backup names include the plan digest and metadata kind. The publisher hashes and syncs both generations, retains the previous snapshot through the remove/rename window, verifies the installed snapshot afterward, and performs no deletion. An absent original is supported only when explicitly declared with zero length/hash. Unexpected files, directories, ambiguous generations, and mismatched bytes block publication.

The workspace borrows hash scratch and owns paths/handles outside the task stack; its HAL handles allocate once through checked preparation and are reused. It imposes a 64 KiB snapshot limit without allocating a snapshot-sized buffer. The same SdFat host checker now covers both metadata files, lost acknowledgement at both renames, 79 authorization interruption points, fresh-owner recovery, repeated publication, original/candidate corruption, foreign backups, directory collisions, and read/sync/close/lookup errors. The checker passes against both default and sticky installed SdFat sources. These tests use fake SD operations. JSON snapshot preparation, reference-provider integration, backup retirement, and startup recovery before store loading still remain required before enabling reader removal.


The declaration is now a shared native `LRMS` v1 record: 240 bytes with CRC32, the full 115-byte removal request, plan digest, previous/next hashes and lengths, and an explicit State/Recent kind. Decode checks exact length, version/reserved bytes, request ownership, the 64 KiB limits, absent-original consistency, and explicit unchanged-snapshot semantics before assigning output. It uses bytewise little-endian reads and supports unaligned input without casting raw bytes to wider pointers.

`HalRemovalMetadataSnapshotStorage` persists immutable declarations through a bounded private stage, checked sync/close/readback, rename, and a final reopened verification. Persist requires the exact currently Quarantined request/plan from the removal journal and rechecks that checkpoint around I/O and before rename. Existing healthy declarations must match exactly; conflicting or corrupt published declarations remain untouched. A bounded incomplete unpublished stage can be rebuilt only by that quarantined owner. Complete foreign stages, directory collisions, and oversized stages block publication. Load returns an owned decoded record only after successful sync/close.

`HalRemovalMetadataAuthorization` loads this durable declaration and captures the current Quarantined/Committed journal checkpoint. Its callback compares the complete declaration and current checkpoint without performing an SD lookup per hash chunk. The enclosing owner must exclude declaration and metadata writers throughout publication. Advancing or invalidating the journal invalidates the authorization until it is explicitly rebound from disk. Declaration encoding scratch is 240 bytes, separate from the publisher's borrowed hash scratch; records, paths, and reused HAL handles stay in session-owned objects outside the task stack. Nine focused host tests and the integrated publisher checks against both installed SdFat versions pass. Snapshot creation, live EPUB-reference publication, startup recovery, and retirement remain unfinished; this layer is not yet enabled in reader commands.

`EpubReferenceJson` now prepares the reference edits using the installed ArduinoJson API. It clears a matching `openEpubPath` or removes every matching recent-book entry, preserving unrelated JSON fields and entries. `HalRemovalReferencePath` compares canonical long-path components using the installed SdFat uppercase rules, including non-ASCII casing; aliases must first be resolved by the inventory owner. A decomposed path is not normalized into a different native filename. Malformed known fields, embedded NUL in reference paths, non-object documents, trailing input, source I/O failure, and parser exhaustion reject the snapshot. Parsing/serialization have one fixed reader/writer signature in a `.cpp` translation unit.

The JSON workspace has a fixed 16 KiB aligned arena supplied through ArduinoJson's custom allocator. Allocation/reallocation stays in that arena, reports exhaustion through ArduinoJson, and never falls back to the general heap. The document is cleared before resetting the arena between snapshots. Allocate this large workspace once with `makeUniqueNoThrow`, outside the task stack, and release it before later removal phases; do not keep it alongside dictionary extraction workspaces. The arena trades a bounded temporary session allocation for preserving unknown settings without an unbounded JSON heap. Physical free-heap/largest-block and stack acceptance remain required before enabling this path. The source owner must bound SD input length, yield while filling its read buffer, and report I/O health; the destination owner must check buffered flush, truncate, sync, close, and complete hash readback before persisting its declaration.

The installed-library host checker covers Unicode/case references, duplicate recent entries, unrelated metadata, malformed input and adjacent trailing values, embedded NUL, arena exhaustion, short output writes, source errors, and 200 workspace reuse cycles. It passes for both default/sticky dependencies. Candidate creation, reference-provider integration, and startup/retirement wiring remain pending.

Snapshot parsing now uses `ARDUINOJSON_DEFAULT_NESTING_LIMIT`, matching ordinary native store loading rather than imposing a lower independent depth limit. The host checker verifies the deepest default-valid object/array combination and rejects the next level without producing a prepared snapshot.

After the final JSON parser edit, both firmware builds pass (`pio run -e default` and `pio run -e sticky`). The release-image validator accepts the C3 image at 6,243,440 bytes (310,160 bytes OTA headroom) and the S3 image at 5,604,640 bytes (948,960 bytes headroom). The C3 build reports 64,872 bytes of static RAM. These compile/image checks do not measure the temporary JSON workspace's runtime heap/stack costs; the workspace has not yet been wired into firmware sessions.


`HalEpubReferenceJsonReader` and `HalEpubReferenceJsonWriter` now supply the HAL-backed buffered adapters. They borrow a reusable HAL handle and scratch, enforce the 64 KiB extent, retain SHA contexts outside the task stack, check authorization around SD operations, and yield after every sixteen buffers. The reader hashes the exact buffers consumed by JSON parsing and reports short reads, source extent changes, and I/O failure separately from EOF. Its hash is available only after the entire input is consumed. The writer hashes intended output bytes, checks complete length, flush/truncate/sync, and exposes a digest only after finish succeeds. The enclosing owner must independently check close and reopened bytes against that digest before persisting a declaration. Hashing only the candidate after writing would incorrectly bless corrupted output; the new test flips written bytes and verifies that the intended and reopened digests differ.

An unchanged declaration now explicitly permits identical previous/next length and hash. Its publisher requires the original file to verify and private candidate/backup paths to be absent; it performs no rename or deletion. This lets reference cleanup retain metadata byte-for-byte when that document contains no matching entry. Tests cover both State and Recent no-ops and verify zero renames. The declaration/storage suite now has ten passing tests; installed dependency checks include adapter cancellation, short reads/writes, changed input extent, failed truncate/sync, intended/readback SHA agreement, and corrupted output rejection by the required digest comparison. Candidate preparation, concrete reference-provider integration, snapshot retirement, and startup recovery remain required.

After the final adapter/no-op edits, both default/sticky firmware builds and release-image validation pass again. The image sizes remain 6,243,440 and 5,604,640 bytes. The adapters are not yet part of a live removal session, so these build checks do not replace physical SD interruption, peak heap/largest-block, or task stack measurements after integration.

`HalEpubReferenceSnapshot` now connects the parsing and publication prerequisites for each metadata file. It requires the exact Quarantined EPUB request/plan, loads and reuses an existing owned declaration, and blocks foreign/corrupt receipts or unowned backups. With no receipt, it parses the original through the hashing HAL reader, checks sync/close, and prepares either a byte-preserving no-op or a private candidate. Intended serialized bytes are hashed by the writer; reopened candidate bytes must match that length/hash. It then reopens and verifies the original generation (or checked absence), confirms backup absence again, and persists the immutable declaration. A bounded unpublished candidate may be rewritten only while the same quarantined owner has no published declaration; directories and oversized candidates remain untouched. Missing metadata uses the native empty-store shape. Output is assigned only after successful proof publication.

Candidate and backup paths are now shared by preparation and publication through `HalRemovalMetadataPaths`. The preparer borrows the reusable 16 KiB JSON workspace and a small IO/hash slice; declaration storage owns a separate 240-byte slice. Its records, paths, SHA contexts, directory buffers, and reused HAL handles stay outside the task stack. No snapshot-sized heap allocation or allocation per buffered IO is added. Release the preparation workspace before entering the later publication/retirement phases.

The installed-library checker now runs preparation through durable authorization and metadata publication for both State and Recent. Twenty-eight combinations cover normal preparation, write corruption, failed write/read/sync/close, lost declaration rename acknowledgement, missing files, no-ops, foreign declarations, unowned backups, directory/oversized candidates, unchanged-candidate collisions, reconstruction, preserved unrelated metadata/history, and unchanged output on failure. Both default and sticky dependency checks pass. The new headers are not yet included by compiled firmware callers; the last C3/S3 image validation remains the prior adapter/no-op build. Live command integration, completed-removal receipts, and startup recovery before store loading remain unfinished.


`HalEpubRemovalReferences` now implements the participant's required reference provider. It prepares both State/Recent declarations before publishing either snapshot, loads existing owned declarations on retry, and releases its one checked preparation allocation before publication. The allocation contains the fixed JSON arena and preparer because their combined buffers exceed the task stack; it is reused for both metadata files rather than allocated per IO buffer. Each metadata publisher and its HAL handles remain provider members and are reused across phase checks. IO/declaration slices must be disjoint.

The reference interface now includes retirement and retired verification. `HalEpubRemovalParticipant` requires successful metadata retirement/verification before deleting the quarantined EPUB. Metadata retirement requires the exact Committed journal request/plan, verifies the installed generation and any remaining old snapshot, preserves malformed/unowned candidates, and tolerates a backup deletion that succeeded but reported failure. All metadata backups are verified before the provider starts deleting them. Immutable declarations and removal journals remain available for recovery. Retired authorization is read-only; publication/retirement entry points retain their explicit phase gates.

The installed-library checker now runs the actual reference provider through the complete EPUB removal coordinator. Nine cases cover normal completion, metadata delete after-effect failure, candidate sync failure, source close/read failure, declaration rename after-effect failure, candidate write corruption, unowned metadata backup, and a damaged second backup that blocks all new retirement deletions. Reconstruction completes authorized recovery, clears matching state/recent entries, preserves unrelated recent progress/bookmarks, retires both metadata and EPUB backups, and acknowledges an immediate duplicate without more renames. Wrong-phase retirement performs no mutation. Both installed dependency checks and all ten declaration/storage tests pass. The provider headers remain outside compiled firmware callers; command/session and startup integration, physical heap/stack/SD acceptance, and completed-removal receipts are still required.

Completion receipts must precede enabling reader removal: the current global removal journal retains one Retired owner and rejects a different next transaction, and retired verification still binds the exact metadata generation. Transaction admission must use the durable completed-request lookup before rechecking retired metadata, so later legitimate reader-state changes do not turn an already completed request into a conflict, and release the completed journal before admitting a new removal. Preserve history and verify the original completed owner/card/manifest when adding that lookup.


`HalCompletedContentRemovals` now stores the original Retired `LRJN` record under `/.crosspoint/companion/removal-done-<transaction-hex>`. The transaction ID is immutable and globally unique; a reused ID with a different owner, card generation, or manifest is a conflict. Load validates the complete CRC/terminal phase, syncs/closes the receipt, and returns owned fields independently of current reader metadata. Persist requires the exact current Retired journal owner, stages and verifies the full record, publishes with rename, and reopens it before acknowledging durability. Matching retries do not rename again. Corrupt/conflicting published receipts, directories, oversized stages, and valid foreign staged records remain untouched. Bounded incomplete unpublished stages can be rebuilt by the exact Retired owner.

`HalCompletedRemovalJournalRelease` verifies this receipt against the current journal request/plan, checks both physical journal files before deleting either, rechecks each file immediately before deletion, and checks final absence. It invalidates cached journal authority after successful release or ambiguous deletion/verification. If deletion removes the Retired slot and leaves the older Committed slot, reconstruction uses the independent Retired receipt to release that surviving owner safely. A foreign or corrupt journal file blocks release. The helper requires the concrete HAL journal storage so ownership reads and deletion refer to the same filesystem. No content, metadata, history, or completion receipt is deleted during journal release.

Seventeen focused journal/completion tests pass, along with both installed-library removal suites. Coverage includes changed legitimate reader state, foreign request fields, write/sync/close/rename interruptions, published receipt preservation, interrupted deletion of the newer Retired slot, release of the surviving Committed slot, both-owner validation before deletion, and admission of a second transaction after checked release. Receipt/release buffers and records are session-owned outside the task stack; encoding scratch is 168 bytes and does not allocate per file operation. These helpers are not yet wired into transaction admission or command/startup handlers. Add the completed-request fast path, completion persistence after retirement, and checked old-journal release before enabling removal. Physical SD interruption and heap/stack acceptance remain pending.

Reproduce the focused checks with `cmake -S test -B build/companion-tests`, `cmake --build build/companion-tests --target HalCompletedContentRemovalsTest CompanionContentRemovalJournalTest`, then `ctest --test-dir build/companion-tests -R '^(CompletedContentRemovals|ContentRemovalJournal|ContentRemoval)\.' --output-on-failure`.

`HalContentRemovalTransactions` now supplies admission around the participant coordinator: it checks the measured card generation, resolves durable completed requests before participant work, persists a Retired receipt before acknowledging completion, and releases the previous journal only after validating its completion. An unfinished different request blocks admission. A Retired request without a receipt can publish its receipt without rechecking metadata that later reading legitimately changed. The standalone lookup is intended before live inventory resolution. Records and borrowed receipt/release buffers remain outside the task stack; this wrapper adds no heap allocations.

Twenty-one focused journal, completion, and admission tests pass. Both installed C3/S3 SdFat/ArduinoJson suites also pass, including actual EPUB removal, changed reader metadata, a second removal, and a duplicate of the first request after journal rotation. The integration fixture explicitly recovers a fresh journal before inspecting its phase because the completed-request fast path does not load the journal. Reproduce admission tests with target `HalContentRemovalTransactionsTest` and CTest expression `^(ContentRemovalTransactions|CompletedContentRemovals|ContentRemovalJournal)\.`. Command/session factories, startup recovery before mutable reader-store loads, in-memory store refresh, and physical SD/heap/stack acceptance remain pending; removal is not enabled by this helper.

`HalContentRemovalStartupRecovery` now resumes persisted EPUB removals before transfer recovery and reader-store loads. It prepares the private journal directory and uses checked enumeration to distinguish absence from lookup failure, so a fresh card works without treating an unreadable directory as empty. Card identity is provisioned only when a removal journal is present. Recovery checks the full journal/card ownership, uses an immutable completion receipt before touching the plan or current reader metadata, and can seal a Retired journal that lacks its receipt. Otherwise it loads and hashes the persisted single-file plan, requires the exact request, binds the actual EPUB/reference participant, finishes the coordinated phases, persists completion, and releases both journal slots through checked ownership verification. Unsupported active participants, missing/corrupt plans, foreign ownership, and IO failures block resumption without discarding evidence.

The boot owner and its fixed plan/journal/declaration/IO buffers use one checked `makeUniqueNoThrow` allocation because they exceed the 256-byte stack budget. Its lifetime ends before the separate transfer workspace and font loading. JSON preparation retains its existing checked, bounded 16 KiB arena allocation only when missing reference snapshots need preparation; no buffer allocation occurs per IO chunk. Runtime peak/free/largest heap and stack watermarks still require physical acceptance.

`HalCompanionRecovery.cpp` invokes this recovery immediately after storage is mounted. `main.cpp` now stops before loading any persisted reader/settings/recent stores if startup recovery fails, draws the translated recovery popup, and blocks the main activity loop. Repair the retained transaction evidence and reboot before reading resumes; this early failure route does not open a Connect & Sync session with partially loaded stores. Normal successful startup continues to load stores from the recovered files.

Both installed-library suites pass with fresh-card absence, directory enumeration failure, corrupt journal preservation, wrong-card preservation, corrupt persisted-plan preservation, all nine EPUB interruption cases, receipt sync failure, journal deletion after-effect failure, reconstructed recovery, and repeated completed recovery. The same fixtures retain the changed-state and subsequent-removal admission checks. The checker defines Arduino's `HEX` macro while compiling: removal hexadecimal constants use `HEX_DIGITS` to remain compatible with the real firmware headers. Thirty-four focused journal/completion/admission/plan/declaration host tests pass. Reader command admission, Apple removal jobs, in-session RAM-store refresh, other content removal participants, and physical power-cut/resource acceptance remain pending; reader removal is still not advertised.

Final native builds after the startup changes pass for `default` (C3) and `sticky` (S3). The release image validator accepts both board/chip tags and image checksums/hashes: C3 is 6,276,016 bytes with 277,584 bytes of OTA headroom; S3 is 5,634,288 bytes with 919,312 bytes of headroom. C3 SHA-256 is `9a924d5f7207831d57f4bf816509a9fc64ebb49dd5bf397b174a4c9b49dbf1cd`; S3 is `201c2eec7b054df5429178cd85ba0dffca9c52fa4565c51443c536b0e34499d9`. These images include the native startup recovery caller; they do not establish hardware resource/power-cut acceptance. The physical checklist now includes fresh-card boot, early failure before mutable store loads, every removal checkpoint, completion publication, journal-slot cleanup, foreign cards/plans, and later reading-state preservation.

Command 15 and its portable authorization/completion-first handler now define the removal endpoint contract; the shared reply codec/fixture and explicit EPUB removal capability bit 8 are available on Apple. No reader advertises the bit. Authenticated BLE and encrypted Wi-Fi dispatch now invoke the native handler; the complete-path owner, startup recovery, and app jobs/UI are described below. Native backend admission must resolve the complete installed path set: `InventoryPaths::find` returns the first matching path, while inventory deduplicates content hashes and can retain multiple identical copies in its path map. A backend must not acknowledge deselection as complete after silently removing only that first copy. Persist a recoverable plan for the full content path set, clear matching live references for that set, retain progress/bookmark history, and refresh/invalidate session inventory and in-memory reader stores before resumption. Existing single-file participants remain useful for the one-path case and need composition for multiple paths. Other content kinds retain their dependency-aware removal requirements.

`InventoryPaths::nextPath` now copies a terminated path alongside its manifest instead of exposing borrowed decode bytes. Insufficient output capacity, overlapping output/scratch, and read failures invalidate enumeration without publishing new outputs. Both manifest-only and path-copying enumeration verify the complete record CRC, reported snapshot length, header identity/revision/count/CRC, and an in-memory payload-only CRC at End. The payload guard is computed while opening and while enumerating, excluding each record's CRC footer: a record plus its own valid CRC has a fixed residue, so the existing wire aggregate alone cannot detect same-length content rewritten with a new per-record CRC. This adds twelve bytes of iterator state and preserves the existing file format. CRC guards detect corruption/change; they do not authenticate SD data or replace exclusion of concurrent writers.

`RemovalPathCollection` streams every path whose hash matches the request, requires the entire manifest to match, validates the existing single-file plan path rules, and calls its sink's finish only after a verified End. A missing hash, conflicting manifest/path, later source error, or staging failure discards only the owned unpublished stage through the sink. Cleanup failure remains an IO error. The collector owns one 512-byte terminated path, one request and one manifest outside the task stack, with borrowed iterator/sink references; it adds no heap allocations or per-record containers. Its supported path grammar covers EPUBs and fonts, while their eventual participants still need content-specific cleanup/dependency handling. A durable multi-path plan sink, physical-path collision handling, participant composition, boot-format dispatch, native command/session wiring and in-memory/inventory refresh remain required; this streaming component does not enable removal.

Seventy-two focused collector, inventory path/pair, builder, source, stage, publication and recovery checks pass. Coverage includes renamed identical copies, unrelated content, exact-manifest conflicts, no matching paths, malformed requests/card/revision, read failure after the first staged path, changed headers, a changed unrelated record with a valid per-record CRC, sink begin/append/finish/discard failures, output termination and decode-scratch overlap. Collector/iterator entry points pass the optimized host 256-byte frame check with exceptions/RTTI disabled. Reproduce with the `CompanionRemovalPathCollectionTest`, `CompanionInventoryPathsTest`, `CompanionInventoryPairValidationTest`, `HalInventoryBuildTest`, `CompanionInventoryPathsBuilderTest`, `CompanionInventoryPublicationTest`, `HalInventoryPublicationValidatorTest`, `HalInventoryPathsStageTest`, `HalInventoryIndexStageTest`, and `HalInventoryFileSourceTest` CMake targets; use CTest prefixes `RemovalPathCollectionTest`, `CompanionInventoryPaths`, `CompanionInventoryPairValidation`, `HalInventoryBaseResolverTest`, `InventoryBuildTest`, `CompanionInventoryPathsBuilder`, `CompanionInventoryPublication`, `HalPublicationValidationTest`, `PathStageTest`, `IndexStageTest`, and `InventoryFileSourceTest`. These are host checks, not physical stack/resource or power-cut acceptance.

Final `default` C3 and `sticky` S3 builds pass after the iterator guards. Validated images are 6,276,336 bytes (277,264 bytes OTA headroom), SHA-256 `e69800d542a7e7f0fc84829c378faec2091f6a0dc05c37156e10eb2ef014bc51`, and 5,634,752 bytes (918,848 bytes headroom), SHA-256 `371f8830ee1aac9f8fb59b30f53ad3055f1c2d58b51e7dc4f22ae01f643d7d70`. Native builds exercise the shared iterator through existing inventory callers; the new collector remains host-tested until its durable sink/backend is connected.

The multi-path plan codec and bounded reader now exist as LRMP v1, with the complete request, original inventory revision, record count/length, and payload-only CRC. The reader validates all records before becoming ready and streams copied paths with 517 bytes of borrowed record scratch, without retaining the full list. Nine tests and host/C3/S3 256-byte-frame compile checks pass. This provides the format needed by the collector's future durable sink; it does not yet write or publish a plan on SD, hash it against a journal, resolve duplicate physical paths, or arm the cohort participant. See the LRMP layout and verification limits in `protocol.md`.

The streamed plan now has an independent LRSC v1 ownership claim and HAL publisher. It captures the full request and inventory revision, uses their complete encoded SHA-256 for its marker/stage namespace, and exposes the mutable plan-stage address only after a matching marker is synced, closed and reopened successfully. It leaves existing plan-stage bytes untouched, preserves foreign/corrupt published evidence, and reuses an already verified temporary marker during publication retry. Ten fault/codec/fixture tests and real-header C3/S3 persist/load frame checks pass. The claim is a prerequisite for safely recovering an interrupted large plan header; the actual plan writer, full-file digest publication, stage-content ownership checks, physical alias handling, and journal/participant/command integration are still required.

`HalMultiPathRemovalPlanWriter` now implements the collector sink: a durable full-request/revision claim owns its stage, records stream within an admitted SD-byte quota, and a sealed plan is reopened and checked against both intended record SHA-256 and its complete-file SHA-256 before immutable publication. Retry verifies an existing publication and removes only the matching stage. Canonical long paths are checked with SdFat case comparison; equivalent duplicates reject the collection. Duplicate detection rereads prior records, so memory stays bounded but comparisons grow quadratically with path count. The owner and its buffers must reside outside the task stack. Installed-SdFat fault tests and a C3 compiler check with a 256-byte frame limit pass. Cohort participant composition, physical alias canonicalization, boot-format dispatch and native command wiring remain unfinished; removal is still disabled.

The installed-SdFat writer checks compare published bytes directly with `protocol/fixtures/MultiPathRemovalPlan.json`, and preserve a stage carrying a different complete request owner even when its header CRC is valid. Run `python3 test/companion/sdfat_path_lookup_check.py .pio/libdeps/default/SdFat/src` after installing/building that environment to repeat these host checks. They exercise the real SDK Unicode comparison with a simulated HAL filesystem, not physical SD power-loss behavior.

`HalMultiPathRemovalPlanStorage` reopens an immutable LRMP plan by the journal's expected full-file SHA-256 and full request. It validates the complete plan before exposing paths, retains one 517-byte member buffer, and repeats the full-file hash on rewind and verified End. It uses a retained HAL handle and allocates no per-path containers. The serialized owner must exclude plan writers throughout iteration; detecting mutation at End cannot roll back actions already taken. Reader tests cover copied paths, rewind, wrong-owner binding and changed-file rejection. This reader is not yet connected to startup recovery or a cohort participant.

Reader failure tests also cover HAL read, sync, close and stat errors, trailing bytes, missing digest-addressed files, and invalid zero digests. Failed reads do not expose a ready header; clearing a transient fault permits a fresh verified open. Cohort integration must keep the parent LRMP hash as journal ownership while assigning separate path-specific quarantine addresses: the current single-file participant uses its bound plan hash for both ownership and backup naming. Reusing that participant unchanged for multiple paths would conflate those backups. Its reference provider likewise still accepts one original path, so a cohort needs one metadata publication that removes references to every included path before any backup is retired.

The JSON transformer now accepts a fallible `PathMatch` callback through `loadMatching`, allowing one parsed candidate to remove every matching cohort path. Existing single-path calls adapt their filesystem comparator through the same transformation. A matcher failure invalidates the whole candidate, even after earlier matches, so it cannot serialize a partial reference update. Installed-SdFat/ArduinoJson tests check two-path removal, preserved unrelated entries/history and failure after an earlier match. The callback adds no heap allocations; the fixed parser arena is unchanged. A native plan-backed matcher and snapshot-provider integration remain required.

`HalRemovalPlanPathMatcher` adapts the verified multi-path reader to the JSON transformer's fallible matching callback. Each metadata path triggers a complete rewind/scan through verified End, including after an early match; an unreadable later record or revoked transaction guard aborts the candidate. It owns one 512-byte path buffer outside the stack, borrows the reader and guard, and adds no heap allocation. This bounds memory while requiring repeated SD scans proportional to metadata references times cohort paths. Tests check either path, case comparison, nonmatches and revoked ownership. Snapshot-provider wiring remains unfinished.

`HalEpubReferenceSnapshot::prepareMatching` now accepts the cohort callback while retaining the existing declaration, source-generation and candidate-verification pipeline. Single-path preparation adapts its path comparator through this method. The snapshot fault matrix runs in both modes; cohort cases remove two recent-book paths through one candidate and declaration while leaving source bytes untouched until publication and retaining history. A caller must bind the matcher to the same request/plan hash as the journal and keep its guard alive. The reference provider and cohort participant still require that wiring.

`HalEpubRemovalReferences::publishMatching` now prepares both metadata snapshots with one cohort matcher, reusing the existing checked off-stack preparation allocation and immutable declaration/publication pipeline. Single-path publication uses a comparator adapter through the same code. A provider test publishes two-path removal and repeats it idempotently, preserving the unrelated recent-book entry. The caller still must bind the matcher to the exact journal request and parent plan digest; the native cohort participant and recovery dispatch are not connected yet.

`removalCohortAddress` derives a distinct quarantine address as SHA-256 of the ASCII domain `lila/removal-cohort-bytes/v1`, the complete parent plan digest and the zero-based ordinal encoded as eight little-endian bytes. The address uses the separate `removal-cohort-bytes-` namespace. It borrows a caller-owned SHA context and uses fixed small stack buffers, with no allocation. Independent fixtures cover ordinals zero, one and UINT64_MAX; invalid arguments leave output untouched. Address derivation grants no mutation authority: the cohort owner must verify the parent plan and ordinal and keep the parent digest in journal checks.

The EPUB participant can now bind one ordinal from a verified LRMP reader, retaining the parent digest as journal ownership and deriving a separate cohort quarantine address. `unbind` closes its retained handle and allows the same owner/buffers to be reused for the next path without per-path allocation. Tests quarantine two identical copies into distinct backups, reconstruct the participant and verify/repeat both quarantines, and reject wrong digests or out-of-range ordinals. This is the byte operation building block: the cohort coordinator must still verify every path before quarantine, publish the cohort metadata once, and retire backups only after that publication is committed. Single-path reference methods must not be used as a cohort publication substitute.

`HalEpubRemovalCohortParticipant` now applies each journal-phase operation to every verified plan ordinal with one reusable EPUB worker. A reference bridge routes publication through the plan-backed matcher; repeated path-level publication/retirement calls use the provider's immutable declarations and idempotent metadata operations. Every operation checks journal ownership around path binding and IO and revalidates the plan hash before completion. The host integration test resumes two already quarantined copies, publishes metadata removing both references, reaches Retired, removes both byte backups, and repeats the removal successfully. This participant is not yet dispatched by startup recovery or the native command backend; alias deduplication and broader fault/power-loss coverage remain necessary before enabling removal.

Startup recovery now handles LRMP cohorts as well as legacy single-file plans. A single-plan Corrupt result permits an independent LRMP full-request/hash/format check; Missing, Invalid and IO errors stop recovery. Unknown or damaged bytes cannot pass the LRMP check. The cohort recovery owner is allocated with checked `makeUniqueNoThrow` only when needed because its retained worker and reader buffers exceed the task-stack budget, and is destroyed before sealing the completion receipt. Interruption tests recover metadata sync failures, rename-after-effect failures and removal-after-effect failures through startup, then verify journal release and repeated boot. Native removal commands remain disabled pending admission/inventory-refresh wiring and physical acceptance.

Cohort startup tests now assert that a wrong measured card generation or a damaged immutable plan fails recovery without changing any files. Restoring the correct card/plan lets the interrupted transaction finish. Command admission must resume the journal-bound plan for an existing request before consulting live inventory, since quarantine can already have removed some paths; another unfinished request must block admission. These requirements are not yet wired into the native backend.

`HalEpubRemovalAdmission` now checks durable journal state before live path collection. An existing matching request opens its journal-bound LRMP plan even when the live inventory is unavailable; another journal request returns Busy. A matching Retired journal returns a receipt-sealing admission without reopening deleted content. Only checked journal absence permits collection at the supplied inventory revision. The authenticated caller must first consult completed receipts and release any completed previous journal owner, validate the inventory pair, and exclude logical writers. Tests prove resume and Busy paths make no inventory calls. Native backend and UI/inventory refresh wiring remain unfinished.

Admission lifecycle tests now cover fresh two-path collection, retry with unavailable inventory, a competing request, and Retired admission with both inventory and plan reader unavailable. The admission entry point passes the native C3 compiler's 256-byte frame check. Startup recovery passes full default/C3 and sticky/S3 firmware builds. These checks do not yet prove native command routing, post-removal inventory/UI refresh, or physical SD power-loss behavior.

`HalEpubRemovalBackend` composes journal-first admission and durable transactions behind the command handler's backend interface. Completed receipts are consulted before live admission. Success requires a caller-provided reader/inventory refresh callback, including duplicate completed requests; refresh failure returns IO error without erasing the durable receipt, allowing a later retry. The caller also supplies a serialization/availability guard and must release any completed prior journal owner before admitting another request. Tests seal a Retired transaction, verify duplicate receipt handling without inventory reads, and require successful refresh after an earlier refresh failure. The native activity has not yet supplied these callbacks or routed removal commands to this backend.

Transaction preparation now releases a completed previous journal owner before new live-plan admission. It verifies the prior receipt, or seals a Retired journal's receipt, before invoking checked journal release. A different unfinished request remains a conflict and its files stay unchanged. The backend invokes this step before admission, removing the earlier caller-only journal-release precondition. Tests check completed-owner release, surviving receipt lookup, same-request resume and preservation of a competing unfinished journal.

`HalEpubRemovalSession` assembles separate fixed journal, completion, release, declaration and IO buffers with the collector, writer, reader, cohort participant, transactions, backend and command handler. Allocate and retain it off stack with checked `makeUniqueNoThrow`; borrowed inventory paths/revision and callbacks must outlive it. It performs no internal per-path allocations and keeps the provider's existing bounded preparation allocation. Session tests exercise unauthorized no-write handling and completed-request replies with the correct transaction ID and no inventory reads. Native activity allocation, exclusive-storage admission, refresh callbacks and dispatch remain unfinished.

The native activity now has a refresh operation that invalidates/closes its inventory reader, reloads `APP_STATE` and `RECENT_BOOKS`, and invokes the existing verified inventory rebuild. Failure sets `recoveryBlocked`, preventing normal reading/storage activity from resuming. The removal session exposes `closeReaders` so the future callback can release its immutable plan handle before rebuilding inventory. This refresh operation is not yet connected to command dispatch; the native owner must also close its borrowed path-storage handle before calling it.

Admission accepts an optional fallible inventory-preparation callback only in the checked journal-absent branch. It can validate/open or rebuild the native index/path pair and return the actual revision used for collection. Retry, Busy and Retired branches skip this callback. Tests require no file mutation on callback failure, fresh admission with a returned revision replacing an initial zero, and no extra callback calls during durable retry/competition/retirement. The session passes this callback through to admission; activity owner/dispatch wiring remains pending.

`HalEpubRemovalNativeOwner` retains the inventory validator, one path-storage handle, a record bank plus 128-byte coverage bank, and the removal session outside the task stack. Its lazy inventory open validates the published index/path pair at the requested revision, then bounds plan bytes by the verified path-map file length plus the LRMP header. LRMP records omit each inventory record's manifest, so this is a conservative finite bound. It closes both plan and path readers before refresh. Quota changes are allowed only when no writer stage is owned; an attempted change during an active stage fails that operation. The activity now allocates this owner lazily with a checked allocation and dispatches authenticated requests through it. The owner does not allocate itself or enable removal capabilities.

BLE `RemoveContent` now routes through authenticated native activity admission. It checks full request owner/card/kind, excludes unfinished transfers and journal sessions, checks firmware installation intents with checked storage lookup, and retains a checked off-stack native owner for the connection. Fresh collection lazily validates/opens the inventory pair; completion closes removal readers and reloads state/recent books before verified inventory rebuild. IO/corrupt results block ordinary activity storage work pending recovery. Owner cleanup occurs with journal-session teardown. The EPUB removal capability remains unadvertised; physical heap/stack/SD-failure acceptance remains pending. Wi-Fi request admission does not yet permit removal.

Wi-Fi admission now permits a removal request only when its complete transaction, installation owner and storage generation match the encrypted handoff lease. Native Wi-Fi dispatch routes accepted removal through the same activity owner as BLE. Six Wi-Fi request tests include removal binding and malformed-length rejection. The complete C3 activity translation unit now passes compilation with `-Werror=frame-larger-than=256`: metadata decoding validates wire fields before output assignment without a full snapshot temporary, and snapshot preparation resets its existing member fields without a large temporary. The decoder rejects overlapping input/output; its wire format is unchanged. Capability advertisement and physical acceptance remain pending.

Companion library schema 33 adds durable EPUB removal jobs in a separate table, retaining reader identity, the immutable full request and queued/removing/paused/completed phase. Queue admission requires a complete inventory containing the exact manifest and deduplicates pending requests by reader/card/owner/manifest while retaining their transaction IDs. Jobs do not depend on library-content foreign keys. Queueing changes only that reader's selection when a library copy exists; the copy and global library visibility remain intact. A restart test checks retained request/phase, pause/resume, completed terminal state and pending-job filtering. Transport runner and UI integration remain unfinished.

`TransferRunner.removeContent` now shares the existing runner's concurrency guard with transfer/firmware work. It validates saved reader, installation owner and card identity, checks protocol/removal capability before sending, persists Removing before exchange and pauses failed exchanges while retaining the exact request. Success requires matching command/response flag/request ID and a transaction-bound Ok reply before completing the job. Locally completed jobs return without another command after identity checks. Tests simulate a lost reply, retry the identical request, reject wrong-card admission without transmission and reject mismatched response IDs. The Swift runner requires advertised EPUB removal support; native advertisement is still pending physical acceptance. Confirmed installed-content controls and pending-job presentation are now wired in the app, as described below; native Apple execution remains unverified.

The app's selected-content synchronization now executes EPUB removal actions when the reader advertises removal support. Before computing new work, it resumes durable removals belonging to the authenticated reader, installation and card, then collects a fresh inventory. A persisted removal is an accepted intent: reselecting the book does not cancel an uncertain reader transaction. After that transaction completes, reconciliation against the refreshed inventory queues reinstallation when selected. A store test checks that reselecting preserves the pending request and that the completed removal produces an install action against an empty inventory. Unsupported readers retain selection intent without sending removal commands. Device-only content removal controls, pending-removal presentation and Apple runtime acceptance remain unfinished.

The installed-content list now offers a confirmed EPUB removal action for readers advertising support, including device-only books. The model revalidates the authenticated reader/card, complete inventory, exact manifest and idle connection before persisting the request. It then uses selected-content synchronization to resume the durable job. Pending removals appear alongside transfers, with a different-card notice; reconnecting reloads them. The confirmation states that the companion library copy is retained. English catalog entries and Swift source parsing are verified; native SwiftUI typechecking and physical Apple/reader interaction remain required. Verify confirmation cancellation sends nothing, successful removal preserves the library copy, disconnect/reconnect retains the request, and swapping the card prevents execution.

Transfer enqueue now rejects transaction IDs already owned by removal jobs, matching removal enqueue's existing transfer-ID rejection. The check runs inside the same SQLite transaction as insertion. A regression test exercises both directions and checks that rejected queueing preserves reader selection and the original pending removal. These identifiers cannot acquire two durable operation types through the public queue APIs.

New transfer jobs cannot be inserted for the same reader/card/content while a removal remains queued, removing or paused, including through direct enqueue. Reselection remains permitted; the durable removal request remains unchanged. Once removal reaches Completed, ordinary selected-content enqueue succeeds. Existing transfer jobs remain available for recovery/abort. The store test covers rejected direct and selected enqueue before completion, then successful reinstallation enqueue afterward.

Removal queueing also rejects an unfinished transfer of that content on the same reader/card before changing selection. The installed-content action tells users to finish or cancel the pending transfer first. The regression test checks rejection with a fresh removal transaction, unchanged selection and no additional removal job. This prevents sync from sending a removal ahead of recovery/abort for the same bytes.

### Foreground removal recovery

An active scene now considers retained EPUB removal jobs alongside upload jobs,
scoped to the authenticated reader, card generation and Apple installation and
requiring advertised EPUB removal support. The existing selected-content executor
recovers those requests before collecting a new inventory and reconciling work.
The lost-reply regression reopens SQLite and constructs a fresh runner, verifies
that the paused job appears in pending work, rejects a mismatched reply ID, and
completes with the same serialized request; completed jobs leave the pending list.
Native source parsing passes. Apple UI/physical acceptance must still verify that
reactivating after a lost removal reply resumes the same transaction, and that a
changed card, foreign installation or unsupported reader sends no removal.

### Font reference recovery participant

`HalFontRemovalReferences` now supplies the settings side of the shared single-file
worker. It derives grouped families from the directory name and loose vector
families from the filename stem, matching names through the HAL's real SdFat
Unicode comparison. Publication requires the exact current quarantined journal
record and checks settings persistence. A retry saves again even if the first
failed save cleared the in-memory selection. Other selected families remain
selected. Retirement verifies the removed family is no longer selected.

The real-SdFat checker compiles this unchanged participant against fake settings
persistence and exercises failed saves/retries, hidden/visible roots, grouped and
loose fonts, Unicode/case matching, foreign ownership, invalid/nested paths,
unterminated settings, and preserving another selected family. It passes. The
participant adds no heap allocation or path buffer; family names borrow the
worker's retained original path, and its checkpoint belongs in the off-stack
session owner. Actual `CrossPointSettings` serialization, startup recovery,
registry refresh and command routing remain required. This new participant is
not referenced by firmware yet, so the active five-profile build batch verifies
the shared byte worker, not native settings integration. Companion font removal
remains unadvertised until those paths are connected and verified.

### Font startup recovery

Startup recovery now accepts persisted single-file font plans. It checks heap
admission, loads saved settings before creating the font worker, and refuses
recovery if settings cannot be loaded. This avoids publishing boot defaults over
unrelated preferences, because companion recovery precedes normal settings load.
The worker is allocated once with `makeUniqueNoThrow` only for a pending font
journal, borrows the existing plan/hash buffers, and is released before completion
receipt publication and before normal reading starts. Settings serialization uses
the existing checked store path; no new JSON persistence format is introduced.

The real-SdFat checker passes for TTF/OTF/TTC/CPFONT: quarantine followed by failed
settings publication, failed boot settings load, failed retry save, a reconstructed
successful recovery, journal release, unrelated file preservation and repeated
recovery. Settings persistence is a fault-injectable fake in this check. The native
firmware build batch is running. Command admission, registry refresh, capability
advertisement, Apple removal routing and physical power-loss acceptance still
remain required before companion font removal is usable.

### Font removal jobs and settings boundary

The Apple store can now retain font-removal jobs without changing the library
copy. The runner requires the separate font-removal capability, bit 13; EPUB-only
capabilities cannot authorize these commands. The new regression covers vector
format 1 and bitmap format 4, duplicate-job reuse, SQLite reopen, no wire request
to an EPUB-only reader, lost-reply recovery with the identical request and
retained library metadata. All 504 Swift tests pass. Native controls and firmware
advertisement remain disabled until font command routing is complete.

The first native startup build found that HAL cannot include the application's
settings header. The recovery path now borrows a `FontRemovalSettings` interface;
`CompanionFontRemovalSettings` supplies the checked native settings operations
from the application. The small adapter lives on the boot stack and outlives the
synchronous recovery call. HAL no longer depends on `CrossPointSettings` or the
`SETTINGS` singleton. The real-SdFat settings/startup checker passes after this
change, and the corrected native build batch is running. The failed build log is
preserved at `/tmp/lila-font-startup-default-header-failure.log`.

### Capability-gated Apple font removal controls

Installed-content controls, confirmation, foreground recovery, pending-removal
execution and new reconciliation actions now share the runner's kind/capability
check. EPUB capability bit 8 authorizes EPUB removal; font capability bit 13
authorizes font removal. Unsupported kinds remain queued for inspection. Removal
availability shares the content-transfer guard, including library/preference
activity exclusion. The confirmation retains the library copy and explains the
built-in fallback for an in-use font. Localized catalog JSON and SwiftUI syntax
checks pass, and all 504 CompanionKit tests pass after shared capability routing.
Native SDK compilation and UI execution remain unverified. Current firmware does
not advertise bit 13; native multi-path font admission/routing still needs work.

The corrected C3 startup-recovery build and image validation pass, with evidence
under `/tmp/lila-font-startup-images/default.json`. The remaining profiles are
still in the active batch. This check proves compilation/image validity rather
than physical settings or learner-state acceptance.

### Native font cohort command routing

Font capability bit 13 (`0x00002000`) is now advertised in source. Command 15 uses
the existing authenticated owner/card checks, complete inventory path collection,
SHA-bound LRMP plan, shared removal journal and completed-request receipts for
both supported font formats. A retained router allocates the checked font worker
once, only for font work. Its fixed path/lookup/hash state exceeds the stack
budget; it borrows the existing session scratch and uses pre/post internal-heap
admission rather than adding another transfer workspace. Every reference path and
selected-family field is checked before quarantine. One successful settings save
is reused within the same journal checkpoint; failed saves always retry. Removing
an in-use family selects the built-in fallback. The activity marks the font
registry dirty before publishing its refreshed inventory.

Boot recovery accepts both single-file and cohort font plans, loads saved settings
through the borrowed application adapter and completes the same journal phases.
The real-SdFat checker passes a complete command/session matrix for vector and
bitmap formats, duplicated hidden/visible-root files, partial rename after effect,
failed settings save, boot reconstruction, immutable completion replay, sibling
preservation and one successful settings save per phase. Corrupt second-copy bytes
and invalid selected-family settings cause no quarantine or removal journal. All
1,703 configured CTest entries also pass after the routing change. Native radio,
settings serialization, heap/stack and physical power-loss behavior remain
unverified. The earlier startup build batch was intentionally stopped after its
passing C3 checkpoint so final checks can cover the routed implementation. The
current batch uses `/tmp/lila-font-cohort-<profile>-build.log` and saves validated
images under `/tmp/lila-font-cohort-images`.

### Dictionary removal plan foundation

Dictionary inventory exposes an original/canonical ZIP cache object, while the
live lookup uses extracted dictionary members. Removal therefore needs an owned
member plan rather than deleting the inventory ZIP. `DictionaryRemovalPlanCodec`
now binds the exact removal request to the installed base path, original and
canonical archive manifests, and each sealed member's length/hash proof. It reuses
the installation/member proof codecs and requires their complete committed shape,
the removal transaction/card generation and the original manifest to match.
Definitions, index, info and optional synonyms paths are derived with the existing
bounded member-path helper; compressed definitions retain `.dict.dz`.

The codec retains its validation copies outside the task stack and performs no
heap allocation. The eventual native owner must allocate its large fixed state
with `makeUniqueNoThrow`, once per session, with internal-heap admission. It is not
referenced by current firmware yet. Three host tests cover roundtrip/unaligned
wire input, all compressed/synonym combinations, every truncated length and
corrupted byte, valid-checksum ownership mismatch, incomplete proofs, private
paths, hidden roots, and exact member-path output bounds. All 1,706 configured
CTest entries pass. Native plan collection/persistence,
reference/binding publication, completed receipts/startup routing, Apple controls
and physical acceptance still need implementation. No reader advertises dictionary
removal capability yet.

The final routed-font batch passes all five targets. Saved-image identities,
checksums/SHA trailers and OTA-size validation pass for each profile; see the
current table in `hardware-verification.md`. Native Apple execution, actual
settings serialization/power-loss behavior, radio transitions and measured
runtime memory acceptance remain unverified.

### Dictionary member quarantine and recovery

`DictionaryRemovalParticipant` now verifies every declared member before creating
its removal journal. It quarantines only the owned definitions, index, info and
optional synonyms, checks reference publication, and retains verified backups
until the durable commit. Recovery accepts partially renamed or retired members;
foreign ownership or corrupt member proofs stop further mutations. Reference
retirement is followed by another complete backup verification before deletion.
Five host tests cover each storage-operation failure before/after effects,
publication/retirement failure, corrupt later backups, journal ownership changes,
and repeated completion. All 1,711 configured CTest entries pass.

`HalDictionaryRemovalStorage` verifies sealed plan bytes against both their SHA
and complete decoded value, then uses checked HAL lookup, streaming member SHA,
sync/close checks and plan/member-addressed quarantine names. It borrows immutable
plan bytes and disjoint hash scratch; fixed codec/path/hash state remains outside
the task stack and adds no allocation. The installed SdFat Unicode comparison
check passes 72 scenarios across visible/hidden roots, plain/compressed definitions,
optional synonyms and injected IO failures. Unowned siblings, including undeclared
synonyms, survive removal. These tests use fault-injected HAL storage; they do not
prove physical SD power-loss behavior.

A C3 compiler probe with the repository flags, LTO disabled and a 256-byte
frame limit passes. The largest new dictionary method frame is 80 bytes; this
does not measure total call-stack depth or runtime task watermarks.

The backend is not yet connected to reader sessions. Durable native plan
collection, dictionary reference/binding publication, multiple installed copies,
startup/completed-receipt routing and Apple capability controls remain required.
Dictionary removal remains unadvertised until that path is complete.

Dictionary bindings now expose committed-removal retirement and read-only retired
verification. Cleanup requires the exact journal checkpoint, request and plan
digest, verifies the stored binding and retained archives, refuses installation
stage/backup records, and deletes only the path-specific binding. Missing bindings
are harmless on retry; archive caches remain untouched. Four additional binding
tests cover phase/ownership gates, before/after deletion failures with reconstructed
journal/storage owners, corrupt/foreign/pending records, and checkpoint changes
during lookup. All 13 binding tests and all 1,715 configured CTest entries pass after the final
host rebuild. The C3 compiler frame probe passes with
48-byte frames for the new cleanup methods. The retained checkpoint is fixed
owner state, avoiding a large local record or a separate heap allocation.

Native dictionary settings/reference integration and final affected firmware
builds remain pending; earlier font build results do not validate these latest
dictionary binding changes.

### Dictionary selection and binding references

`HalDictionaryRemovalReferences` now preflights saved selection and the installed
archive binding, publishes checked selection persistence in the Quarantined
phase, verifies publication, and retires the path-specific binding only after
Committed. Selected folder names use the installed SdFat Unicode comparison for
both dictionary roots. Other folder selections survive. A failed save that already
cleared memory is retried, including after settings/journal/participant
reconstruction. Repeated successful publication within a checkpoint avoids a
redundant save. Binding and archive validation fail closed before quarantine;
legacy canonical bundles may omit a binding when original and canonical manifests
are identical. Immutable archive caches remain available after removal.

`CompanionDictionaryRemovalSettings` adapts the bounded application settings field
without allocation. Startup must load settings before constructing recovery
participants. The native reference checker covers failed selection saves and
reconstruction through the actual member-removal coordinator, unrelated selection,
invalid settings/foreign bindings, retained caches/unowned siblings, repeated
publication and visible/hidden Unicode folder matching. The full installed-SdFat
checker passes. The C3 compile/frame probe includes the application adapter and
passes the 256-byte limit; reference methods reach at most 64 bytes individually.

These owners are not wired into sessions yet. Durable plan collection and handling
of every matching installed copy, startup/completed receipts, capability routing,
final firmware builds and physical acceptance remain required.

### Dictionary cohort plan verification

`DictionaryRemovalCohortReader` streams fixed dictionary removal records beneath
an `LDRM` version-1 header containing the complete removal request, inventory
revision, copy count and payload CRC. Every record must retain the same request
and valid complete member proofs. Base paths must be strictly increasing, refusing
duplicate exact paths and unsorted input. Header count arithmetic is bounded,
file length must be exact, and the header is checked again at iteration end.
The decoder retains its large plan/codec state off stack, borrows disjoint scratch
and output, and performs no allocation. Five tests cover every truncated length
and corrupt byte, header bounds, wrong ownership, duplicates/order, read failure,
changed headers, iteration and rewind. All 1,720 configured CTest entries pass.

`HalDictionaryRemovalCohortPlanStorage` opens a digest-addressed private plan file,
checks all records and the complete SHA before exposing the plan, and rechecks SHA
on rewind and at iteration end. It retains one HAL file and fixed record scratch;
no allocation occurs in the reader. The installed-SdFat fault checker passes with
corrupt/truncated plans, read/sync/close failures, changed files and wrong/missing
ownership bindings. C3 compile/frame probes pass: header decoding reaches 240
bytes and native storage open reaches 80 bytes. These are individual frames,
not runtime task-stack or heap acceptance.

The native plan publisher/collector must enumerate and prove every matching
installed base, exclude filesystem aliases, and retain the immutable plan through
completion. A reusable cohort participant must bind each copy/member backup to
that parent plan. These components, session/startup routing and final affected
firmware/native Apple/hardware checks remain unfinished. No dictionary removal
capability is advertised yet.

### Reusable dictionary cohort participant

`HalDictionaryRemovalCohortParticipant` now reuses one fixed member owner, decoded
copy and portable participant across the parent plan's installed copies. Each bind
rewinds/verifies the complete parent file and matches the decoded record before
arming member operations. Backup addresses derive from the parent SHA and
`copy ordinal * 4 + member`, keeping all copy/member addresses disjoint, including
when synonyms are absent. Bind arithmetic is checked; unbinding releases borrowed
context and checks the retained file close without removing payloads.

Retirement first verifies publication across every copy, retires references across
every copy, verifies all remaining backups again, then deletes verified member
backups. A corrupt later-copy backup therefore stops backup deletion after a
reference callback. The portable participant exposes separate reference and
verified-backup retirement steps while preserving its single-copy behavior.
No allocation occurs during walks; the large member/plan state must be held in
an admitted native session/startup owner outside the task stack.

The installed-SdFat cohort checker covers two identical installations across both
roots, partial renames before/after effects, failed selection persistence,
interrupted binding retirement, corrupt later sources/backups, close failure,
reconstruction, unowned sibling preservation, and repeated completion without
extra payload mutations or settings saves. All 1,720 CTest entries and the full
installed-SdFat checker pass. C3 compile/frame probes pass: cohort walk reaches
80 bytes, cohort member binding 32 bytes and the split retirement methods 16
bytes individually. Runtime heap/largest-block and task watermarks remain
unverified.

Durable plan publishing/collection, filesystem-alias exclusion, session/startup
routing and capability controls still need implementation. Dictionary removal
remains unadvertised pending those components and final builds/physical checks.

### Durable dictionary cohort plan writer

`HalDictionaryRemovalCohortPlanWriter` now claims an owned stage before writing
its unsealed header, streams ordered dictionary proof records within an admitted
SD quota, seals/syncs the header, verifies the complete staged plan, and publishes
it by its SHA. Record readback must match the SHA of the intended encoded records,
not merely their CRCs. Existing identical targets are verified and retained;
repeated publication cleans only the matching owned stage. Exact and SdFat
Unicode-equivalent path duplicates are refused. Valid foreign stage headers remain
untouched. The common removal stage claim codec now accepts dictionary requests
and retains kind in its digest/ownership binding; a new claim test verifies that
foreign marker contents cannot authorize stage access.

The native writer fault checks cover corrupted/failed record writes, sync/close
failures, publication renames before/after effects, repeated-publication cleanup
failure, quota exhaustion, exact/case-folded duplicates, foreign requests/stages,
and a changed member proof with valid record CRCs and unchanged aggregate CRC.
The intended-record SHA rejects that last case. The full installed-SdFat checker
passes; the final CRC-preserving proof-mutation case also passes in the standalone
writer binary after its addition. All 1,721 configured CTest entries pass following
the host rebuild. The C3 compile/frame probe passes with a largest writer-method
frame of 144 bytes. Writer buffers/decoders remain fixed owner state outside the
task stack, with no allocation during appends.

Native collection still must discover and prove every matching installation,
provide canonical long paths in sorted order, and admit/retain the writer under
exclusive inventory/namespace ownership. Session/startup/completed-receipt routing,
capability controls, final firmware builds and physical acceptance remain pending.

### Installed dictionary proof assembly

`HalDictionaryRemovalPlanAssembly` now discovers an installed folder, hashes its
canonical ZIP, verifies the retained canonical cache and any original-archive
binding, and produces a transaction/card-bound removal record for matching content.
A read-only cache adapter refuses publication/deletion and avoids directory
creation. Unrelated and empty folders leave caller output untouched. Invalid
bindings, changed bytes, missing caches, read/close errors, cancellation and
short/overlapping scratch refuse assembly without changing payload files.

`HalDictionaryRemovalMemberProofSource` hashes each member in the same sequential
read used to emit the canonical archive. Its single retained SHA context is reused
between members; lengths, cursors and hashes are fixed owner state. This avoids a
separate member-hash read pass and binds member proofs to the bytes that produced
the canonical archive SHA. It rejects changed lengths/order, incomplete reads and
latched errors; empty members receive the SHA of empty bytes. Inventory semantic
validation and exclusive card/revision/member/cache/binding ownership remain
required at the caller boundary.

All 1,727 configured CTest entries pass. Six assembly tests cover original versus
canonical archive identity, both roots,
plain/compressed definitions, exact member proofs, immutable caller output,
malformed bindings, changed members, read/close/missing-cache failures, empty
folders, cancellation at every guard, and scratch bounds/overlap. The C3
compile/frame probe passes: assembly reaches 240 bytes, the member-hash wrapper
80 bytes and proof finishing 32 bytes individually. Fixed assembler/decoder
state must remain off the task stack; runtime memory acceptance is still pending.

Native enumeration must still collect every matching installation, filter folders
consistently with inventory, resolve filesystem names, sort proven records and
publish under a stable inventory snapshot. Session/startup/completed-receipt
routing, final firmware builds, Apple controls and physical checks remain pending.

### Native dictionary folder collection

`HalDictionaryRemovalPlanCollection` now enumerates direct folders under both
dictionary roots, following inventory's dot-folder exclusion. Repeated directory
name scans select the next folder prefix (including its trailing separator), so
record ordering remains correct for names such as `A!` versus `A`. Each folder's
member bytes are assembled once. Fixed off-stack directory/name/plan state replaces
a growing heap list. A second name scan refuses exact, Unicode-case or short-name
alias collisions before accepting a folder. Missing roots are determined by
checked root enumeration; root/file collisions and directory IO failures stop
publication. All matching proven records are appended to the owned writer.

An authenticated card/revision guard is required. Guard failure is latched within
collection; cleanup then releases writer handles and retains the owned stage.
Failed begin also releases handles without discarding another retained owner.
Only a fresh authorized owner may verify and discard or rebuild that stage.
The collector respects the existing fixed plan/base bounds and refuses unsupported
or malformed folders rather than truncating paths or deleting their content.

The installed-SdFat collector checker covers both roots, prefix sorting, unrelated
original-archive bindings, hidden/empty folders, case and short-name aliases,
changed members, root/file collisions, directory/close failure, missing matches,
and early/mid-operation cancellation. Its successful case continues through
plan verification and cohort removal: all three selected copies disappear while
notes, caches and the unrelated bound installation survive; repeated completion
makes no further payload/settings changes. The complete installed-SdFat checker
passes after the final cleanup edit. The C3 compiler/frame probe passes with
112-byte name scans and 208-byte collection frames. Runtime heap/largest-block,
task-stack and physical SD/radio acceptance remain unverified.

Session ownership/admission, durable completed receipts, startup routing and
capability/Apple controls remain unfinished. Final affected firmware builds are
still required once these owners are connected; dictionary removal remains
unadvertised.

Dictionary removal admission and session

The native dictionary session now composes inventory admission, sealed cohort
collection, journal recovery, member quarantine, settings publication, and durable
completion receipts. New requests must match the complete inventory manifest;
reconstructed requests recover their retained plan without requiring an inventory
scan of partially quarantined content. The session releases every retained reader
at the radio lifecycle boundary and refuses requests before successful preparation.

The real-SdFat host session check covers authorization, unsupported content kinds,
missing or conflicting inventory entries, partial rename and failed settings-save
recovery, repeated completion receipts, and transaction reuse with changed content.
It verifies that unowned sibling files survive. Activity ownership, startup dispatch,
capability advertisement, Apple controls, final firmware builds, and physical
acceptance remain pending; dictionary removal is not yet advertised.

A retained native dictionary owner now validates the published inventory index/path
pair before admitting a new request, borrows one fixed workspace for validation
and removal hashing, and closes the path reader together with session readers.
The caller supplies and may adjust the bounded plan quota before stage ownership.
The real-SdFat check also constructs this owner and verifies rejection of missing
inventory, zero revision, and an undersized quota, plus repeated handle release.
The connection activity and startup recovery route dictionary removal through
this owner and the retained cohort plan.

Boot recovery now dispatches retained dictionary journals to a lazily allocated,
heap-admitted cohort worker. It loads dictionary settings before member/reference
publication, independently verifies the retained cohort plan, publishes the durable
receipt, and releases the journal. The boot path receives the native settings adapter.
The real-SdFat session test now exercises partial-rename boot recovery, unavailable
settings adapters, settings-load failure without file mutation, repeated boot
recovery, and a subsequent authenticated completion retry without inventory.
The default firmware build and installed-SdFat startup/session fault suite pass.
Final verification of the other affected targets remains pending.

The native dictionary owner bounds a fresh plan by the validated path table count:
one fixed cohort record per inventory entry, plus the cohort header. Dictionary
folder inventory emits one path record per folder, including repeated archive paths;
the path builder preserves those records. Quota arithmetic rejects overflow and
retains the caller's admitted maximum as an additional ceiling. This changes SD
write bounds without allocating a count-sized buffer.

Connect & Sync now routes dictionary removal after the shared authorization,
peer identity, storage generation, writer exclusion, and firmware-intent checks.
Its retained native owner is allocated with makeUniqueNoThrow only after heap
admission; switching removal kinds closes and releases the other owner. Resetting
the connection closes dictionary readers before destruction. BLE and encrypted
Wi-Fi both use the shared command dispatcher. Capability bit 14 advertises
dictionary removal independently of dictionary transfer, EPUB removal and font
removal. Apple selection controls and durable removal jobs use that capability;
unsupported readers retain queued intent without sending a removal command.
All 505 CompanionKit tests pass, including capability isolation and identical
request retry after a lost reply and SQLite restart. Native Apple UI execution
and physical acceptance remain unverified.
The native dictionary owner C3 compiler probe passes the 256-byte frame gate.


### Bound course removal plan

`CourseRemovalPlanCodec` seals the exact removal transaction, Apple installation,
card generation and bound pack manifest in a fixed 127-byte CRMV version-1 record
with CRC-32. Its target is always `/tinta/course.pack`; it carries no caller-selected
path and accepts only a supported bound course. Decoding requires exact length,
version, reserved bytes, CRC and request validity before changing caller output.
The codec retains fixed candidate state, uses borrowed output and allocates no heap.
A C3 compiler probe passes the 256-byte frame limit: encoding and decoding each
use 64 bytes; request decoding uses 176 bytes. These individual frame sizes do
not establish runtime task-stack headroom.

Three regression tests cover round-trip request identity, every-byte corruption,
length rejection, unbound/foreign-kind/unsupported-format requests and unchanged
outputs on refusal. The complete 1,730-entry CTest suite passes. This codec is not
linked into the native removal dispatcher and does not advertise course removal.
The participant must prove learner-state isolation before quarantining the pack;
publication, boot recovery and same-course reinstallation still need integration.
Deleting the pack while retaining the current binding would fail the existing
replacement hash check, so that alone cannot implement safe deselection.


`CourseRemovalParticipant` now implements the portable journal participant for
that fixed pack. Binding requires the sealed plan hash; preflight requires both
an exact pack proof and the reference owner's proof of learner-state isolation.
Every operation checks the journal checkpoint before and after storage callbacks.
Quarantine accepts a verified retained backup on recovery, publication must verify
before backup retirement, and repeated completed removal makes no payload writes.
The reference adapter must retain learner state and bound removal proof for later
reinstallation. The participant borrows fixed state and allocates no heap.

Three portable tests cover storage-operation failures before and after their
effects, restart and repeated completion, isolation refusal, corrupted source,
and failed reference publication or retirement retaining the pack backup. The
complete 1,733-entry CTest suite passes. These use in-memory storage and reference
owners; they do not prove native course-state persistence or reinstallation.
Native plan storage, reference/tombstone publication, boot dispatch and capability
routing remain unfinished, and course removal remains unadvertised.


`HalCourseRemovalStorage` verifies the exact sealed plan and SHA-256, rejects
borrowed-buffer overlap and nonzero pack ordinals, uses HAL checked path lookup,
and restricts quarantine/deletion to the matching journal phase. It hashes and
synchronizes the verified source or backup before mutation, retains fixed off-stack
state and allocates no heap. Nine installed-SdFat adapter scenarios pass, covering
plan corruption, overlap, absent parent ownership, rename failures before/after
effect, remove-after-effect, corrupt pack and read/sync/close failures. Recovery
removes only the proven pack and preserves an unrelated learner-state file.
The reference owner remains a test substitute; native state isolation, removal
proof publication, reinstallation and dispatcher wiring are still required.


`HalCourseRemovalPlanStorage` now publishes fixed course plans under their complete
SHA-256 and reloads them for journal recovery. Fresh staging persists a full-request,
nonzero-inventory-revision claim first. Claimed partial stages may be rebuilt;
valid foreign stages, oversized/directory collisions and corrupt immutable targets
remain untouched. Publication reopens the complete staged bytes, reloads the claim,
renames only to a missing target and verifies the final file. Existing exact plans
return without another payload or marker write. Typed load output changes only
after exact length, SHA, codec, sync and close checks; callers must release borrowed
plan bytes before reusing the load buffer, including after failed loads.

Course claims retain the bound logical identity and cannot reuse dictionary marker
authority. Five storage tests plus the claim regression cover publication/reload,
partial versus foreign stages, interrupted rename before/after effect, corrupt
published targets, directory collisions, overlapping buffers and preserved typed
outputs on read/corruption failures. The complete 1,739-entry CTest suite passes.
The store retains fixed off-stack codec/claim/path state and borrows comparison
scratch of at least 135 bytes; it allocates no heap. Native course reference
publication, state-preserving reinstallation, boot dispatch and command admission
remain unfinished. The earlier five-board dictionary image table is the committed
checkpoint; the extended claim contract passes a default firmware build and release-image validation. Its saved image is under `/tmp/lila-course-claim-images`; the other four dictionary checkpoint images predate this claim edit.


### Removed course baseline proof

Course replacement currently checks the installed pack's locale and stable item
identities. Removal therefore needs a verified private baseline, not just the
retained binding. `CompanionCourseRemovalProof.h` defines metadata matching for
that baseline: a valid Quarantined publication checkpoint must match the complete
Retired receipt, sealed plan hash, unchanged course binding and current storage
generation. Prepared or merely Committed records cannot authorize reinstallation.
The cache address derives only from the sealed plan hash under the private root.
Native callers must also load/verify the sealed plan and independently verify
baseline length and SHA-256 before using those bytes for continuity checks.

Three portable proof tests cover phase progression, altered transaction/owner/card/
content/course/length/version/hash/revision, a replaced current card, changed
binding, bounded cache-path construction and unchanged path output on refusal.
The full 1,742-entry CTest suite passes; the C3 compiler frame probe passes the
256-byte gate with a 32-byte maximum individual frame. The helpers allocate no heap.
Native publication and baseline retention, startup recovery, replacement and
state-selection integration remain unfinished; course removal is unadvertised.


`HalCourseRemovalProofStorage` now publishes the Quarantined checkpoint at the
fixed course-removal proof path while the exact journal owner remains current.
It synchronizes staged bytes, reads them back, requires a missing publication
target, renames and verifies the final file. Exact retries perform no metadata
rewrite. Valid foreign or wrong-phase staged records and corrupt published proofs
remain untouched; a partial owned stage can resume under the same journal.
Loads require the current storage generation and leave typed output unchanged on
failure. The publisher borrows a separate 168-byte workspace and retains fixed
record/lookup state off stack, without heap allocation or payload deletion.

Six host storage tests cover owner/phase admission, idempotent reopen, replaced-card
refusal, rename failures before/after effect, foreign/wrong-phase stages, partial
stage recovery, corrupt publication and sync/close failure. All 1,748 CTest entries
pass. This publisher is not linked into native course removal yet; its caller must
verify course-state isolation and retain/hash the old pack baseline before backup
retirement. The reference adapter, startup dispatch, replacement validation and
state selection still require integration. Course removal remains unadvertised.


`HalCourseRemovalBaseline` now retains the proven pack backup under the sealed
plan's private cache address during Committed reference retirement. It verifies
length/SHA and synchronizes/closes the backup before rename, then verifies the
cache. An existing cache is accepted only when its bytes match; a surviving
backup must match too before the payload participant can retire that duplicate.
Foreign/corrupt caches are preserved and stop recovery. Rename-after-effect can
resume from the verified cache without deleting learner-state files.

Completed-baseline admission requires the exact proof/Retired receipt/binding/card
match, checked absence of an active removal journal, absent active pack and absent
owned backup, followed by independent cache length/SHA verification. The verified
path is unavailable after failed admission. A caller-supplied permission guard is
checked around lookup/hash/mutation; fixed lookup/path/hash/checkpoint state stays
off stack and borrows the existing IO buffer without allocating heap memory.
The caller must independently verify the sealed course plan before admission.

The installed-SdFat course checker now composes real proof publication and baseline
retention with payload removal and completion-receipt journal release. Eleven fault
scenarios cover quarantine and baseline rename failures before/after effect,
corrupt cache preservation, source read/sync/close failures, restart, repeated
completion, exact cached bytes and preserved unrelated learner-state bytes.
Completed admission rejects a pending journal, changed card, unexpected active
pack and corrupt cache. The full installed-SdFat checker and all 1,748 CTest
entries pass. The actual C3 project-header/frame probe passes: baseline retention,
selection, hashing and completed admission use 48-byte individual frames; the
existing imported record decoder reaches 256 bytes. Runtime task headroom remains
unmeasured. Native compilation also caught and corrected an Arduino HEX macro
collision in the proof-path helper.

The production reference adapter must still verify actual state isolation, load
bindings/proofs and connect these owners. Startup, compatible replacement and
explicit different-course switching after removal still require integration.
Apple switch confirmation currently derives its previous course from installed
inventory; an uninstalled course's retained binding/baseline needs an authenticated
context before that confirmation can be offered. Course removal is unadvertised.
