# Reader content selections

`LibraryStore.setReaderSelection` persists a desired choice by hardware reader identity and SHA-256 content ID. SQLite schema 8 keeps these rows separate from shared library metadata and requires the content to exist. EPUBs, courses, fonts, and dictionaries use this path; firmware installation requires its separate explicit update flow.

The current source advertises EPUB, font and dictionary removal through authenticated
BLE and encrypted Wi-Fi commands. A retained owner prepares complete-path plans and publishes
metadata before retiring quarantined bytes. Resource admission covers its owner,
JSON preparation and startup cohort allocations; low/fragmented-heap refusal and
retry pass the native SdFat checker. Native Apple and physical acceptance remain
unverified. Course removal has a portable participant, sealed-plan publication,
removal-proof publication and verified baseline retention. Its production
native activity owner and authenticated routing are connected, including Tinta
state preparation, same-course reinstall and removed-source switch authorization.
Complete course-flow verification remains pending, so course removal is not advertised. Dictionary removal
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

The reader and course-removal recovery now share `completedCourseStateIsolation`.
It checks the complete state and mark migration file lists and requires both
receipts to name the same original course. It reads metadata and checks legacy
root-file absence without requiring an installed pack or writing learner state.
Migration and proof verification use the same constant file lists. The caller
must separately verify the current course directory; this helper alone does
not establish complete removal admission.

The regression migrates all 14 legacy state/mark filenames, removes the active
pack, verifies the original-course proof, and rejects each reappearing root
file, unfinished/missing/corrupt receipts, and CRC-valid crossed origins. Files,
mutation counts and failure outputs remain unchanged. All 1,749 host tests and
the installed-SdFat native checker pass.
A portable ESP32-C3 compiler probe reports individual frames of 96 bytes for the
aggregate check and 144 bytes for the receipt check, with 256-byte frame errors
enabled. This is not a runtime task-stack or heap measurement.

The default C3 firmware build passes after this shared-check edit. Its image
passes the release validator for board/chip, segment bounds, checksum, SHA-256
trailer and OTA partition size. The saved checkpoint image/proof are under
`/tmp/lila-course-isolation-images/`. Other board images recorded above predate
this edit; final affected-board builds remain required after course integration.

`HalCourseStateIsolation` now verifies those receipts together with the actual
current course directory. It requires a permission callback and scans the whole
parent directory, rejecting regular-file collisions, duplicate case-equivalent
names, malformed names/aliases, enumeration errors and failed closes. Its name
buffer and two handles are members of a retained owner; verification allocates
no heap and writes no learner state.

`HalCourseRemovalReferences` composes that check with the exact retained course
binding, absent binding stage/backup, removal-proof publication and independently
verified cached pack baseline. Fresh/prepared admission refuses an existing or
unfinished removal proof. Later checkpoints preserve the binding and learner
files while retaining the verified old pack, and refuse mismatched journals or
proofs. The proof store now accepts the same permission callback used by the
session, including cancellation checks during lookup and before publication.
The eventual session owner must lend disjoint scratch and pass its permission
callback consistently to the reference, state, proof and baseline owners.

The installed-SdFat checker passes with 19 native removal scenarios using this
production reference adapter rather than unconditional state isolation. They
cover missing/corrupt/mismatched bindings, missing scoped directories, legacy
root files, crossed migration receipts, binding/proof stages and revoked
permission in addition to quarantine/baseline recovery faults. A separate native
test checks every isolation permission checkpoint, checked closes, duplicates,
malformed siblings, and yields across a 65-entry parent. These run against an
in-memory HAL fixture; physical SD and actual Tinta replay remain unverified.
All 1,750 host tests pass, including cancellation at every observed proof
publication/load permission check, safe retry, and unchanged failure outputs.

An actual C3 project-header probe compiles all reference entry points with
256-byte frame errors enabled: state directory verification is 128 bytes,
binding/state verification is 80 bytes and the reference operations are at most
32 bytes individually. The maximum imported frame is the existing 256-byte
removal-record decoder. This is not an aggregate task stack-watermark result.
These new classes are not yet dispatched by firmware; the last linked default
firmware image remains the shared-proof checkpoint described above. Final
firmware builds are still required after admission/recovery/routing integration.

`HalCourseRemovalAdmission` now validates a fresh request against the complete
inventory-path snapshot, requiring one exact manifest at `/tinta/course.pack`.
Wrong-card, foreign-owner, duplicate-path and mismatched-manifest requests refuse
without changing the output record. A mandatory state-preparation callback runs
before sealing the plan and before the parent removal journal is created.
Matching retained journals load their sealed plan without requiring a live
inventory or running state migration again; retired journals use the completed
receipt path. The native callback still needs to be connected to the reader's
actual active-course selection/migration owner.

`HalCourseRemovalBoundParticipant` reloads the immutable SHA-addressed plan at
every checkpoint, compares its full request, and lends fixed retained bytes to
the payload participant only while the matching journal remains current.
`HalCourseRemovalSession` composes admission, references, payload recovery,
completed receipts, refresh and the authenticated command handler. Fixed owners
and encoding banks stay off stack; IO is borrowed from the eventual sync owner.
The admission interface is now shared portable code rather than requiring the
EPUB plan implementation to declare its contract.

All 1,755 host tests and the installed-SdFat native suite pass. The native course
fixture exercises this retained-plan participant in all 19 removal scenarios.
It also executes a full session command: unauthorized requests preserve files,
removal retains scoped learner history and binding, failed inventory refresh
returns a failure after a durable completion, and a reconstructed session retries
that exact command through its receipt with live inventory/state preparation
unavailable. These are in-memory HAL checks, not physical radio/SD acceptance.

The actual C3 project-header probe includes session construction and command
handling with 256-byte individual-frame errors enabled. Session construction is
128 bytes and admission is 192 bytes; the maximum imported frame in this probe
is 240 bytes. Runtime task watermarks and heap admission remain to be measured
when the native activity/boot owners are connected. The default firmware build
for the shared admission-header edit passes; its image passes the release
validator and is saved with its proof under `/tmp/lila-course-session-images/`.
These course classes
remain unadvertised pending the remaining integration and reinstall/switch flow.

`HalCourseRemovalRecovery` is a retained boot worker that resumes an exact current
course journal without inventory or migration. Its parent must publish the
completed receipt and release the journal before allowing reader/state writers.
`HalCourseRemovalMetadata` supplies its read-only binding/migration access through
HAL: complete parent scans reject duplicate names/aliases and directory
collisions, checked end and closes establish absence, and reads check length,
sync and close. It requires a permission callback and refuses every mutation API.
Its name/parent buffers and three handles are retained off stack; no allocations
occur inside a scan. It avoids relying on `Storage.exists()` for absence proof.

The native fixture now uses this production metadata adapter, including with
false `exists()` results, and resumes retained journals through the boot worker.
Its 24 course-removal scenarios include metadata enumeration errors, failed
binding sync/close, duplicate case-equivalent binding names and an unavailable
legacy-state parent. Invalid fresh admission leaves pack bytes and learner state
intact. The actual C3 project-header probe passes 256-byte frame errors: boot
worker construction is 96 bytes, metadata scan is 128 bytes and metadata read
is 80 bytes individually; maximum imported frame is 256 bytes. Physical SD,
runtime watermarks, and dispatch from the parent startup owner remain pending.

Startup dispatch now recognizes retained course-removal journals before reader
or Tinta activities open. `HalContentRemovalStartupRecovery` admits one checked
heap allocation for a fixed metadata/recovery owner, borrows its existing IO
bank, completes the matching journal, publishes the completed receipt and only
then releases the journal. Its worker is freed before normal reading stores and
fonts load. No inventory or migration is required after quarantine.

All 1,755 host tests and the installed-SdFat suite pass with this dispatch. Across
the retained native course scenarios, parent startup rejects a wrong generation,
refuses insufficient heap and learner-state conflicts, then completes valid
recovery and leaves a durable receipt with no pending journal. Repeated startup
preserves files. Conflict checks allow recovery to repair only its own journal
slots while requiring the same recovered checkpoint and unchanged pack, binding
and learner files. They do not assume that journal confirmation performs no SD
writes. These tests use in-memory HAL state and synthetic pack bytes.

The actual C3 project-header probe compiles full startup dispatch with 256-byte
frame errors: `recoverCourse` is 48 bytes and `run` is 96 bytes individually;
maximum imported frame is 256 bytes. Hardware watermarks and physical recovery
remain unverified. The default firmware build and release image validation pass;
the saved checkpoint image/proof are under `/tmp/lila-course-startup-images/`.
Course removal remains
unadvertised until its native activity owner, reinstall/switch flow and transport
routing are ready. Final affected-board builds are required after that integration.

`HalCourseRemovalNativeOwner` now wraps the retained session with complete
inventory-pair validation and its read-only metadata owner. Its fixed IO bank
includes a 64-byte coverage bitmap beyond the maximum inventory record; the
existing pair validator checks larger catalogs in bounded windows. The owner
checks the exact bound manifest and missing binding stage/backup before and
after the mandatory state-preparation callback. That callback must validate the
active pack and prepare scoped learner files while all state writers are closed.

The installed-SdFat native round trip uses this owner with actual published
index/path files. It rejects wrong revisions and corrupt pairs before state
preparation, refuses unfinished binding stages/backups and mismatched bindings,
preserves pack/history when preparation fails or changes the binding, and retries
a durable completed removal after reconstruction with both inventory files
corrupt and preparation unavailable. The remaining 24 removal/startup scenarios
continue using the production retained-plan, metadata and recovery adapters.
These fixtures use synthetic pack bytes and a preparation callback, not the
actual Tinta selection engine or physical SD.

The actual C3 project-header/construction probe passes 256-byte frame errors:
native inventory opening is 32 bytes, binding validation is 64 bytes, and the
maximum imported individual frame is 240 bytes. No allocation occurs inside
owner scans or hashing loops. The new owner remains unconnected to the native
activity; its actual Tinta preparation callback, reinstall/switch handling and
BLE/Wi-Fi routing still need integration before advertising course removal.
The linked firmware checkpoint remains the startup-dispatch build above.

`HalRemovedCourseBaseline` now verifies a removed pack before lending its path
and original manifest to same-course update validation. It requires the current
card generation, exact retained binding, completed Retired receipt, full sealed
plan and independently hashed cached bytes. The active pack and owned quarantine
backup must be absent, binding/proof stages must be absent, and read-only state
isolation must still pass. Different course identities are refused. Its fixed
records, decoder banks and handles stay off stack; hashing borrows caller IO.
The loan is invalidated by close, another open or observed permission loss.

All 1,755 host tests and the installed-SdFat suite pass. The 24 native removal
scenarios now check the selector before journal release and after completed
receipt publication/release. Each exercises same-course candidate selection and
12 refusal cases covering active-pack reappearance, corrupt cache/plan/proof,
unfinished publications, legacy-state reappearance, missing/corrupt/foreign
receipts and a changed binding. Failure preserves files and exposes no old path
or manifest; wrong-generation and different-course selection also refuse.
These are synthetic pack/in-memory HAL tests, not installed Tinta update tests.

The actual C3 project-header/construction probe passes 256-byte frame errors:
construction is 96 bytes and `open` is 144 bytes individually, with a 256-byte
maximum imported frame.

The transfer validator uses this verified source when the active bound pack is
missing and the candidate has a matching, fully received transfer with nonzero
owner, transaction and card generation. It applies the same locale and stored
item-identity continuity checks used for live packs; the four-argument entry
without transfer context cannot authorize a removed baseline. One checked,
nothrow allocation retains the selector while validation borrows the transfer
workspace. Admission and selector operations preserve the 50 KiB internal-heap
reserve. No allocation occurs in its scan or hash loops.

Real `mini.pack` host regressions complete native course removal and journal
release before validating a compatible update. They reject changed language,
reassigned retired item identity, unfinished/wrong-phase transfer, wrong card
generation, corrupt proof and insufficient heap, preserving stored files.
Same-course committed installation now retires the old removal proof through
checked finalization. The native activity callback, routing and explicit removed
course switch flow remain required. These host checks do not establish physical
SD or device behavior.

The integrated validator checkpoint passes all 1,759 host tests and the
installed-SdFat checker. The actual C3 translation-unit probe passes
`-Werror=frame-larger-than=256`; its stack-usage report records 272 bytes for
`validateCourseContent`, including saved registers and call space. This is not
a runtime stack-watermark or heap measurement. Device acceptance remains pending.

The default X4 firmware build passes. The release validator checks its `x4`
board tag, chip ID 5, 6,553,600-byte OTA partition, image segments, checksum and
SHA trailer. The 6,482,256-byte image has SHA-256
`07ba7cc874de286057bfca3f516d56528a3f51f6ceadd15ecb59a7a964822bff`.
The other firmware targets still require their final integration builds.

Course finalization verifies the exact committed transfer, new binding and
installed SHA before removing obsolete proof metadata. If a proof exists, its
Retired receipt, sealed plan, original cached pack hash, state isolation and
absence of a retained removal journal or publication stages must all pass.
The proof is deleted only after readers close; its checked absence completes
retirement. Applied deletion that reports failure can retry against the verified
replacement without repeating state changes. Cached packs, sealed plans, receipts
and learner state are retained. One checked nothrow owner allocation keeps the
fixed banks and handles off stack, with the existing 50 KiB heap reserve.

Real-pack host regressions cover reinstall followed by another native removal,
corrupt/foreign proof refusal, installed-file sync failure, and retry after an
applied proof deletion reports failure. The installed-SdFat scenarios also
verify that a rejected replacement invalidates a previous cached-path loan.
The C3 probe reports individual retirement and installed-hash stack usage of
128 and 96 bytes respectively; physical stack/heap acceptance remains pending.

The retirement checkpoint passes all 1,762 host tests, the installed-SdFat suite
and the actual C3 translation-unit frame check. The default firmware build and
release image validation pass. The 6,483,904-byte image has SHA-256
`985485cc4e16d45171286164f093e7df8f24a05d51f17121cdf837ca762214e5` and passes the X4 board/chip, partition, segment, checksum
and SHA-trailer checks. The preparation helper is connected to the native activity.

`prepareBoundCourseRemovalState` supplies the native admission preparation callback.
It validates the active bound pack through the retained transfer parser before
moving legacy state with Tinta's existing course-state migration functions.
Permission and internal-heap reserve checks precede validation and follow both
validation and migration. Parser admission accounts for its fixed size and
largest required block; no new callback-owned allocation or scan-loop allocation
is introduced. Real-pack tests refuse unauthorized/corrupt input without file
changes, preserve the pack and binding, and prove repeated successful preparation
leaves the scoped learner files and migration receipts unchanged. Its actual C3
header/call probe passes the local-frame check. The native activity uses this callback.

The preparation checkpoint passes all 1,763 host tests; its C3 callback frame is
48 bytes. The removed-course switch and matching Apple context flow remain open.

`CompanionConnectActivity` routes authenticated course removal through one retained
`HalCourseRemovalNativeOwner` on Tinta-enabled builds. BLE and encrypted Wi-Fi
both use the shared removal handler, with owner/card checks, writer exclusion and
firmware-intent refusal. Other removal owners close and release before course
allocation; leaving the course path, resetting the connection and activity exit
release its handles and buffers. Checked nothrow admission accounts for the owner
size and largest block, with the existing 50 KiB reserve. Decoder/migration IO is
retained in the owner and no allocation occurs in scan/hash loops.

The complete activity translation unit passes actual C3 frame checks with Tinta
both enabled and disabled. The course route reports 112-byte individual stack
usage and owner release 16 bytes. A real-pack native-owner test publishes a full
inventory pair, invokes the production preparation callback, preserves scoped
learner bytes, and proves completed retries bypass invalid live inventory and
missing pack bytes. All 1,764 host tests pass. Physical BLE/Wi-Fi acceptance and
the final target builds remain pending. The course-removal capability remains
unadvertised until removed-course switching and its Apple confirmation/context
flow are connected.

The native routing default build passes release image validation. Its
6,489,280-byte X4 image has SHA-256
`71d7790e0082610e28b73791022aa8241fa3be2541f29ea316a1101bcaa154fc`.
Board/chip, OTA partition, image segments, checksum and SHA trailer pass.

The shared course-context body codec distinguishes live and verified removed
sources without adding cached packs to installed inventory. Requests contain a
versioned prefix and nonzero card generation (20 bytes). Successful replies
contain status, source, generation and the complete course manifest (85 bytes);
failures contain no source or manifest (22 bytes). Wrong-storage replies report
the current generation; other replies must match the requested generation.
Only course format 1 with nonzero identity/hash and positive length is accepted.
Truncated, extended or malformed replies fail without changing C++ outputs.

C++ and Swift tests consume the same request/removed-response fixtures and cover
both source types, malformed metadata and all failure statuses. All 509 Swift
package tests pass, and the C3 codec probe passes its local-frame check. The
context query is routed to a read-only native provider on BLE and encrypted
Wi-Fi. Apple inventory/context retrieval, explicit confirmation and reader
switch-source validation remain required before enabling the course-removal capability.

The combined routing/context checkpoint passes all 1,768 host tests and 509 Swift
package tests. Native Apple builds and physical acceptance remain unverified.

The native bound-course query now distinguishes a checked live binding/source
from a verified removed baseline, a missing course and an unbound legacy pack.
Unfinished metadata or a retained removal journal refuse context, and corrupt
binding/cache/evidence never exposes a source manifest. The native owner uses
checked sync/close, invalidates any old cached-path loan and publishes outputs
only after verification/close succeeds. Real-pack host tests preserve files and
outputs across refusal; the 24 installed-SdFat scenarios check busy-before-release
and removed-after-release without changing stored bytes.

Command 19 is routed through both authenticated transports. The Swift frame
helper matches command, response flag and request ID; its Wi-Fi helper checks
transaction/card binding before sending and uses encrypted replies. The complete
activity C3 probes pass with Tinta on and off: query handling uses 192 bytes and
source inspection 208 bytes individually. The default firmware build passes.
Apple query/discovery is capability gated; native removed-source switching remains unconnected,
and course removal/context capabilities remain unadvertised.

The query checkpoint passes all 1,772 host tests, 511 Swift package tests and
the installed-SdFat suite. Default firmware image verification passes;
physical acceptance and native removed-source switch integration remain pending.

The query firmware checkpoint is 6491536 bytes (SHA-256
`bbc2d72523ec9bd5ff85aec511567796bf2d543bb69e19ff4fd7dba0fce129ae`); its x4/chip-5 tag,
segments, checksum, SHA trailer and OTA partition fit are checked. This does
not establish runtime heap headroom or physical acceptance.

Apple complete inventories now carry optional checked bound-course context
separately from installed content. The capability-gated collector accepts live
context only when it matches the inventoried course and removed context only
when that inventory contains no course. Busy, corrupt, unauthorized and other
failures withhold the snapshot; wrong-card responses require reader reopening.
Ordinary admission still rejects a different removed course identity. Explicit
switch confirmation persists its exact previous identity/hash, survives library
reopening and refuses retargeting. The confirmation screen checks the reviewed
context as well as the installed inventory. Native removed-source switch
validation and advertising remain pending; native Apple SDK/UI validation and
physical acceptance are still unverified.

The Apple inventory/confirmation checkpoint passes all 515 Swift package tests
and parses the app source. Firmware remains the validated query checkpoint;
this change adds no embedded allocation or compiled firmware source. Native
Apple typechecking/UI execution and physical-reader verification remain pending.

Native switch consent and precommit preparation now verify either the live
course or the completed removed-course baseline. Removed-source validation
rechecks binding, generation, sealed plan, receipt, cached SHA and old state
isolation; it does not attempt legacy migration from a missing pack. Aborts
retain that baseline and learner files. Committed different-course switches
reload exact durable consent and retire only the matching old removed proof,
before deleting consent. The old cached pack, sealed plan, receipt and learner
files remain preserved. The replacement bytes and both state directories are
verified, and applied proof/consent deletion failures recover idempotently.

The C3 local-frame checks pass for the actual activity with Tinta on/off and the
storage translation unit with Tinta on/off. Individual stack usage is 144 bytes
for source inspection, 208 for state preparation, 224 for metadata finalization
and 128 for proof retirement. These compiler figures do not establish runtime
stack watermarks or heap headroom. The installed-SdFat checks pass with the new
switched-baseline checks. All 1,775 host tests and the default firmware build
pass; native Apple builds, physical acceptance and completion of the full companion plan
remain unverified.

Before enabling course removal/context, verify continuity when switching back to
an already-used destination course with different pack bytes. The current switch
preparation verifies its source and creates/selects the destination state
folder (`HalTransferStorage.cpp`, `prepareCourseSwitch`); the stored item-history
comparison is in the ordinary same-course update path. Source authorization and
state isolation tests do not establish destination-course UID/language
continuity. This requirement remains open alongside final target builds and
physical acceptance.

The removed-source switch firmware is 6493024 bytes (SHA-256
`60f86503bf394d1d9a116b649ebc520e3be6477eaf6d460db000ca2859d649a6`). Its x4/chip-5
tag, segments, checksum, SHA trailer and OTA partition fit pass the release
validator. This is a default-target checkpoint, not final validation of all
firmware targets or runtime resource headroom.

A portable append-only pack archive and checked HAL owner now retain immutable
pack bytes plus course-scoped references. This supplies the persistence primitive
needed for return-to-course history validation: consumers must check all retained
versions, rather than infer a baseline from the current active course or folder
existence. Native transfer preparation/publication and complete destination-scope
reference enumeration remain to be connected.

Publication uses a full-manifest ownership record and bounded borrowed IO;
resuming compares the durable copy prefix against the source before appending.
Torn ownership records, conflicting identities, unowned stages and changed prefixes
are preserved and refused. Cache/reference publication is idempotent across
before/after mutation failures. Copy/hash/lookup handles are retained and reused;
resuming opens its writer once, and subsequent chunks reuse that handle. Native
checks cover false-positive absence APIs, lookup/sync/close failures and byte-exact
prefix resume without changing the original pack.

The C3 frame probes pass for portable and native archive publication/opening.
The portable object is 1,016 bytes; the native object is 2,304 bytes, excluding
lazy HAL handle allocations. Both must be retained off stack after heap admission.
The portable functions use individual stack frames of 80–112 bytes; native
publication uses 96 bytes. These sizes do not prove runtime heap headroom or stack
watermarks. Archives remain unconnected to production transfer flow, and do not
yet enable returning to a previously used course with different bytes.

The archive checkpoint passes all 1,785 host tests and the installed-SdFat suite,
including owned-prefix resume, permission loss, corrupt cache and failed close.
The native C3 constructor/publication probe also passes the 256-byte local-frame
check. The default firmware build and image validation pass; these check the
existing target and do not establish archive behavior until transfer wiring uses
these owners.

The archive persistence checkpoint firmware is 6493024 bytes (SHA-256
`8047325ad19d84435b2309eb74846c228e8fdec724bb9f3257b8330e56f252a9`). Its x4/chip-5
tag, segments, checksum, SHA trailer and OTA partition fit are verified. Archive
publication is still exercised through host/C3 probes rather than native transfer
routing in this image.

### Archived course-history enumeration

`HalCoursePackHistory` performs a complete checked scan of `/tinta/courses`
and the selected scope before visiting references. Duplicate folded scope names,
invalid owned reference names, unfinished reference stages, corrupt archives,
failed enumeration/close and lost permission refuse the operation. An empty
scope is distinguishable from learner files that have no archived baseline;
the latter needs an authoritative legacy baseline before reuse.

Visitors receive a read-only manifest/path loan for each verified archive.
The reader keeps no growing reference list and checks the visit count against
its first scan. Namespace/state writers must remain excluded throughout both
scans and callbacks. Pack parsing and item-identity comparison remain the
visitor's responsibility; a valid archive hash alone does not establish course
compatibility.

The history checkpoint passes all 1,790 host tests. Its C3 constructor and
visit probe pass the 256-byte frame limit: scope selection uses 96 bytes,
reference classification 144 bytes and scanning 96 bytes. The retained owner
is 2,696 bytes, excluding lazy HAL handle allocations; callers must admit it
on the heap rather than use stack or permanent static storage. It is not yet
wired into native transfer preparation, so this checkpoint does not establish
return-to-course safety in firmware or on hardware.

### Native metadata archive publication

The authenticated transfer metadata overload now retains the installed course
as an immutable archive before the parent transfer can become committed.
It requires a complete matching Installing/Committed transfer context and
uses checked heap admission for the temporary archive owner. Explicit switch
binding publication and ordinary binding publication remain recoverable when
archiving fails; retry verifies the same cache/reference without overwriting it.
The four-argument metadata helper remains available for existing local callers.

All 1,795 host tests pass, including archive-close failure, unchanged-file
retry, invalid transfer context without writes, and validation of every archived
pack version after active-pack removal. The C3 compilation of `HalTransferStorage.cpp`, with Tinta enabled and disabled,
passes the 256-byte
frame limit: archive publication uses 64 bytes and authenticated metadata uses
192 bytes. The default firmware build and image validation pass. This establishes retention
of newly installed versions; outgoing legacy baseline retention and destination
history compatibility checks are still required before return-to-course safety
is complete.


The native publication checkpoint image is 6,498,112 bytes (SHA-256
`b4f92321b7aa6786aa1d2794dd5d9035d83bc739948f3540e6989cca8a9e2aae`). Its x4/chip-5
tag, segments, checksum, SHA trailer and OTA partition fit are verified. These
checks do not establish physical recovery or runtime heap headroom.

`HalCoursePackHistoryValidator` borrows a caller-owned parser and scratch,
validates the candidate's hash/format, parses every verified archived version,
checks language and compares stable item histories. Missing identity history
requires identical legacy records regardless of current learner-file presence.
Host tests accept timestamp-only updates and reject changed retired identity
meaning and changed language without writes. The C3 helper is 36 bytes and its
visitor frame is 176 bytes. It remains separate from transfer preparation;
return-to-course compatibility enforcement is not complete.

### Explicit switch preparation with archived history

Native explicit-switch preparation now retains the verified outgoing pack before
replacement. A removed-course cache loan remains open and permission-valid while
its bytes are copied. Archive and baseline owners use checked heap admission;
the outgoing owners are released before destination-history admission.

The target's archived versions are completely parsed and checked for language
and stable item continuity before learner state is reused. A used scope without
an archived baseline refuses pending authoritative legacy-baseline migration.
This does not silently assign existing learner files to a different pack.

All 1,797 host tests pass, including unchanged active/binding/learner bytes when
a returned course reinterprets a retired UID or lacks a historical baseline, and
recovery through every switch rename boundary. The C3 frame check passes: source retention uses 64 bytes,
history validation 144 bytes and switch preparation 208 bytes. The default
firmware build and image validation pass. Ordinary same-course updates still need all-version
history integration, and historical scopes from older firmware need a verified
baseline import path; the full course-history requirement remains incomplete.

Legacy baseline recovery must distinguish an absent archive from absent evidence.
Completed removal receipts retain the full request and sealed plan hash after the
active binding changes (`HalCompletedContentRemovals.h`). Their plan-addressed
caches remain available after proof retirement. A future historical reader must
check the receipt's canonical transaction name, Retired phase, generation,
course/manifest, sealed plan request/hash, cache length/SHA and scope-isolation
proof before lending pack bytes. It must scan the complete receipt namespace and
validate every matching version without allocating a growing list.

The existing completed-removal loader creates its parent directory and the
current-course baseline verifier requires the active pack to be absent (or an
explicitly verified replacement). Neither is a historical, read-only baseline
selector. Do not relax those current-course guards to implement legacy return.
Older switched scopes with no retained receipt/cache need an explicit verified
baseline-import/conflict flow; the directory name alone cannot establish earlier
item meaning. These paths remain to be implemented.

`CompanionHistoricalCourseBaseline.h` now provides address selection for a
Retired receipt with an exact matching plan, verified plan hash, generation and
course identity. Refusal leaves the output buffer unchanged. This helper does
not reconstruct a current-course proof or authorize learner-state reuse; its
future caller still needs checked receipt enumeration, sealed-plan hashing,
cache validation and state isolation. All 1,799 host tests pass, including
foreign/unfinished evidence and output-preservation cases. Its C3 frame is
64 bytes and it adds no heap allocation. Complete receipt enumeration and transfer integration remain
unimplemented; address selection alone does not enable legacy return.


The explicit-switch checkpoint image is 6,501,696 bytes (SHA-256
`1bfb97db4b915fc346b929db5817b46f702e2e989609b54da8161a76483e2a3d`). Its x4/chip-5
tag, segments, checksum, SHA trailer and OTA partition fit are verified.

`HalHistoricalCourseBaseline` now inspects one supplied Retired receipt without
creating directories or writing files. It checks the canonical receipt against
the supplied record, hashes and decodes its sealed plan, verifies missing removal
journals/cohort backup, proves state isolation and verifies cache length/SHA.
A different active binding or retired current-course proof does not invalidate
this historical loan. Close, reopen or lost permission invalidates it.

All 1,801 host tests pass, including changed binding/proof retirement, corrupt
receipt/plan/cache, failed sync/close/enumeration, unfinished removal and permission
loss with unchanged files. The C3 constructor/open probe passes the frame limit;
the retained owner is 2,792 bytes excluding lazy HAL handle allocations, opening
uses 160 bytes and hashing uses 64 bytes. Future callers must heap-admit this
owner. It is not yet used by transfer preparation, and complete receipt scanning,
legacy baseline import/conflict handling and ordinary-update history checks
remain unfinished. Physical recovery and runtime memory acceptance are pending.

### Ordinary updates with archived history

Authenticated ordinary updates now retain the exact bound live or removed
baseline before replacement, then check every archived version. Removed baseline
loans remain valid through copying; those owners are released before admitting
the two history readers. First unbound association keeps its existing admission
rules and archives the installed version during metadata publication.

A legacy archive may establish continuity through an archived identity-bearing
pack whose legacy records are identical and whose stable identity history remains
compatible with the candidate. The bridge scan checks complete history and does
not accept a conflicting identity-bearing version. The secondary reader is
allocated once and reused across visits; no growing bridge list is retained.
Both 2,696-byte history owners are admitted before allocation, with lazy HAL
handle costs checked by the permission guard during I/O. This does not prove
runtime headroom, and legacy bridge searches can require repeated SD reads.

All 1,805 host tests pass, including legacy-to-identity migration followed by a
compatible edition, a valid legacy bridge alongside conflicting identity history,
an earlier archive conflicting with the active pack, pack
rename recovery and outgoing archive interruptions before replacement, including
rename failure reported after publication takes effect. The C3
frame check passes: ordinary update validation uses 224 bytes and history
admission uses 144 bytes. The C3 compilation also passes with Tinta disabled.
The default firmware build and image validation pass. Legacy receipt
enumeration/import and physical memory/recovery acceptance remain unfinished.


The ordinary-update checkpoint image is 6,502,464 bytes (SHA-256
`fabd20eea98a02def6b084632f956a624e1b7507af012cf72adbbe26ef6d83ad`). Its x4/chip-5
tag, segments, checksum, SHA trailer and OTA partition fit are verified. Host
admission tests reject insufficient combined reader headroom and an undersized
largest block while preserving the installed pack, binding and learner files;
retry succeeds after restoring admission. These mocks and compiler frame checks
do not establish physical runtime heap or recovery acceptance.

### Complete retained-removal history enumeration

`HalHistoricalCourseHistory` now performs two read-only scans of the receipt
namespace. The first rejects malformed/staged receipts and verifies canonical
transaction names, complete record decoding and matching course/generation
before callbacks. The second visits every matching sealed-plan/cache/isolation
baseline through the historical selector and checks its count against the first
scan. Visitors borrow manifest/path loans only until return and must remain
read-only; the final result must succeed before authorizing learner-state reuse.

All 1,809 host tests pass, including two retained versions, missing course,
foreign generation, duplicate folded names, incomplete enumeration, unfinished
stages, corrupted receipts, permission loss and unchanged files. The C3
constructor/visit probe passes the frame limit. The retained owner is 4,496 bytes,
excluding lazy HAL handle allocations; classification uses 96 bytes and scanning
144 bytes. Future callers must heap-admit the owner rather than put it on the
stack or reserve it permanently. Its streaming scans keep no growing version
list and do not allocate owners inside callbacks.

The enumerator is not yet connected to transfer validation. Native legacy return
still refuses a used scope without archived references; historical receipt
validation/import and the no-evidence conflict workflow remain to be integrated.
No new firmware image or physical recovery result is claimed for this unconnected
header; the C3 probe forces its constructor and visit implementation directly.


### Retained removal history in native transfer validation

Ordinary bound-course updates and explicit course switches now validate all
matching retained removal receipts after checking immutable archive references.
A used scope without archive references may reuse learner state only when its
retained receipts, sealed plans, caches and completed state isolation all verify.
An absent receipt set remains an explicit refusal for that used scope. Matching
receipts are also checked when archive validation succeeds, so a compatible
current archive cannot hide an older incompatible item identity or language.

`HalCoursePackHistoryValidator` accepts either history reader and applies the same
complete pack parsing, locale and stable-item continuity checks. A retained
legacy pack can use an identity-bearing retained receipt with identical legacy
learning records as a bridge. The validator borrows its parser, scratch and
readers; it adds no allocation during callbacks. Archive and removal sets still
use separate bridge scans; cross-set legacy bridge selection and the no-evidence
baseline import/conflict workflow remain unfinished.

The receipt reader is admitted and allocated off stack because its fixed owner
is 4,496 bytes on C3. A metadata-only inspection avoids allocating a second reader
when no matching receipts exist. When receipts exist, a separately admitted
second owner supplies bridge loans; both owners are reused for the complete scan
and released afterward. Archive readers are released before this phase. The
validator is 52 bytes on C3. Forced native source compilation passes the
256-byte frame limit with Tinta enabled and disabled: archive and receipt
validation each use 128-byte frames, and ordinary course validation uses 224.
These compiler sizes exclude lazy HAL handles and do not establish runtime heap
headroom.

The real transfer regression removes course A, installs B, retires A's current
removal proof, removes A's immutable archive to represent an older installation,
and returns to A through its retained receipts. The original pack succeeds and
reconstructs the archive; changed retired-item meaning, changed language and a
corrupt retained cache refuse before replacement. A's and B's learner files are
preserved in every case. A separate ordinary-update regression checks that an
older receipt conflict refuses even when the current archive is compatible.
All 1,814 host tests pass, including both native transfer regressions and
contiguous-heap admission refusal followed by successful retry.

On hardware, repeat A-to-B-to-A switching after removal, including a reboot
between operations. Verify each course's item/star/day files, retained receipts,
active binding and pack hash; try an incompatible item identity and language and
confirm the active course remains usable. Measure free/largest heap and task
watermarks throughout repeated sessions. Physical recovery and memory acceptance
remain unverified.

The default firmware build passes (544.54 seconds). The retained checkpoint image
is 6,507,552 bytes, SHA-256 `11ef46f0e3270370256a97d96a49b6ad33cfd498c0c07185256da488ebab4e86`. The release
validator verifies its x4/chip-5 tag, segment bounds, checksum, SHA trailer and
OTA partition fit. This image validation does not prove physical acceptance.


### Legacy bridges across both retained history sources

Native transfer validation now lends both archive and removal-receipt readers to
legacy bridge selection. An archived legacy pack may use an identity baseline
from a retained receipt, and a legacy receipt may use an archived identity
baseline. The outer validation still visits every version in both sets; finding
a bridge does not make conflicting stable identities or language acceptable.
Used scopes with neither verified source still refuse reuse and need the explicit
baseline import/conflict workflow.

A metadata-only receipt inspection runs first. If no matching receipts exist,
that owner is released before allocating the two archive readers. Otherwise it
is reused for archive bridge visits and then the complete receipt scan. One
archive reader is released before the receipt phase; the other is retained as an
archive bridge while a second receipt reader is admitted. These fixed owners
peak at 9,888 bytes during archive validation and 11,688 bytes during receipt
validation on C3, excluding lazy HAL handles. Heap admission uses current free
and largest-block measurements before each allocation. Owners are allocated
once and reused for scans, never inside callbacks; stack placement exceeds the
256-byte limit, while a permanent static pool would consume this RAM outside
connection mode. The helpers retain 128-byte C3 frames with Tinta enabled; the
source also compiles with Tinta disabled.

All 1,815 host tests pass. The native transfer regression exercises A-to-B-to-A
returns with the legacy version in either source and the identity baseline only
in the other. A valid bridge succeeds; a conflicting retired identity added to
either archives or receipts refuses before active pack/binding replacement.
Both course scopes' learner files remain intact. Retained-cache read and close
failures also preserve the installed course and leave Receiving resumable;
clearing each fault makes the same transaction succeed in both bridge layouts.
Physical heap, power-cut recovery and native Apple acceptance remain unverified. On hardware, repeat both
layouts after a reboot, check active pack/binding and each scope's learner files,
and measure free/largest heap plus task stack watermarks during repeated returns.

The default firmware build passes (548.64 seconds), with no reported warnings or
errors. The retained image is 6,507,568 bytes, SHA-256
`facb47ae7f085e6da32167e8ba8ef348c64358e280c96f355ac020956e8d18bf`. Release validation confirms the x4/chip-5
tag, segment bounds, checksum, SHA trailer and OTA partition fit. This does not
establish runtime heap headroom or physical recovery acceptance.


### Durable original-baseline consent foundation

`CourseBaselineImportRequest` binds an explicit original-pack confirmation to
one storage generation, authenticated installation owner, transfer transaction,
complete course manifest and frozen review hash. The 155-byte version-1 record
has an explicit confirmation marker, reserved bytes and CRC. It rejects zero
identities/hashes, non-course content, unsupported format and pack lengths above
32-bit parser limits. Initial admission compares every binding against the
frozen review and exact fresh transfer declaration. The shared binary fixture
is `protocol/fixtures/CourseBaselineImportRequest-v1.fixture`.

`CourseBaselineImportConsent` saves an immutable record under
`/.crosspoint/companion/course-baseline-<transaction32>.consent`, using an exact
`.tmp` staging record, readback and checked non-overwriting rename. Completed
writes/renames with lost acknowledgement resume with the same request. Changed
requests, conflicting stages, transaction/name mismatch, invalid CRC or truncated
records refuse without deleting evidence. Loading canonical approval also
refuses leftover staging evidence. Torn staging requires explicit recovery or a
new reviewed transaction; it is never silently rewritten as a different decision.
Repeated approval performs no writes. Permission loss and reentry refuse, and
load failures preserve the caller's output.

This component stores consent only. It neither verifies pack bytes nor establishes
compatibility of arbitrary item meanings, applies learning state, changes the
active binding, publishes a pack archive or grants learner-state reuse. A caller
must verify a native frozen review and authenticated transfer before saving it.
Native complete-cohort capture, immutable review storage and snapshot preservation
are described below. Their integration with native preflight, a separate resumable
baseline upload destination, archive publication and completion recovery, and
Apple review/queue/UI remains required.
The current course destination and history guards are unchanged, and no new
command or capability is advertised for baseline import.

All 1,824 host tests pass, including nine new consent tests: shared fixture,
malformed/aliased inputs, retargeting and changed reviews, immutable idempotency,
write/rename faults before and after application, torn/foreign/duplicate evidence,
permission loss, read/stat failures and reentry. Tests preserve the active pack
and saved learner files and do not call pack verification or publication.
The forced C3 constructor/codec/persist/load probe passes the 256-byte frame
limit: request object 152 bytes, retained consent owner 376 bytes, decoder frame
224 bytes and persistence frame 48 bytes. The owner contains fixed path and
comparison storage and must be admitted off stack when integrated; the component
allocates nothing internally and borrows existing scratch. This unconnected
header does not change the previously verified firmware image. No new firmware
build, native import success or physical acceptance is claimed here.


### Complete frozen baseline review capture

`CourseBaselineReviewView` defines the bounded version-1 `TCBV` review format.
It binds the native reader, storage generation and selected course to canonical
file records: all isolated course files, the three active authoritative journal
files (including explicit absence), and both intent/completion records for state
and mark isolation. Records contain normalized ASCII names, presence, length and
full SHA-256. They sort by domain/name; duplicate names, pending learner artifacts,
unknown journal roles, malformed absence, reserved fields, invalid CRC and an
incomplete record set refuse. The shared fixture is
`protocol/fixtures/CourseBaselineReview-v1.fixture`.

`HalCourseBaselineReviewCapture` verifies completed isolation before and after
capture, scans the entire selected directory and active journal namespace, uses
strict parent lookups to detect ambiguous file access, hashes complete source
files, and checks sync/close and permission before offering a loan. Scope file
spelling is preserved for reads and normalized only in the review. The review
SHA-256 covers the complete encoded manifest and native identity context.
Reversed enumeration and ASCII casing yield the same hash; changed learner bytes,
authoritative journal bytes or reader identity change it. The operation is
read-only and never publishes an archive or changes learner state. Hash/byte
loans end on close, recapture, permission loss or reuse of borrowed scratch.

Capture is bounded to 64 total records: up to 57 course files plus seven journal
and isolation records, at most 4,416 encoded bytes. Oversized cohorts refuse
without truncation or a growing allocation list. The remaining shared workspace
is reused for streaming source hashing (3,776 bytes from an 8 KiB workspace).
The fixed C3 owner is 2,144 bytes, excluding lazy HAL handles, and must be
heap-admitted when integrated rather than put on the stack or kept as a permanent
pool. It allocates no owners or buffers during enumeration. The forced C3
constructor/capture probe passes the 256-byte frame limit: capture 112 bytes,
scope scan 128, journal scan 112, file hashing admission 96 and view decoding 160.

All 1,829 host tests pass. New cases cover the shared format and malformed
reviews, complete state capture including an unknown valid learner file,
deterministic order/casing, changed same-size state and authority, corrupted
isolation proofs, incomplete journals, staged files, folded duplicate names,
enumeration/read/sync/close failure, oversized cohorts and permission-latched
loans. File maps remain unchanged by capture/refusal.

The caller must supply native identities, establish canonical journal readiness,
close activities and exclude all namespace/state writers; this snapshot does not
validate event semantics or replace those recovery gates. This header is not yet
connected to companion commands. Copies of the reviewed
cohort, native preflight and re-verification, a separate baseline upload route,
archive/completion recovery and Apple review/queue/UI wiring remain required.
No baseline-import capability is advertised. The previously verified firmware
image is unchanged; this unconnected header is checked by the direct C3 probe.
Physical heap, SD fault/power-cut recovery and Apple acceptance remain unverified.

### Immutable frozen review storage

`HalCourseBaselineReviewStore` publishes captured `TCBV` bytes under
`/.crosspoint/companion/course-review-<sha256>`, with a `.tmp` staging file.
The full review hash fixes the file identity. Publication validates the format
and hash, compares existing canonical bytes without rewriting them, syncs and
reads back the stage, and uses a checked non-overwriting rename. A staged prefix
can be extended only after comparing every existing byte with the expected
review; foreign, oversized, corrupt and duplicate files are retained on refusal.
A lost rename acknowledgement can be retried with the same review.

Loading checks the complete file hash, review format and all three native reader,
generation and course identities. These identities are copied before reading
into caller output, so aliases into that output cannot replace the expected
context. A remaining stage refuses loading. Failure may change output bytes but
offers no valid path loan; close, a new operation and permission loss revoke loans.
Source and comparison buffers must be disjoint, and the caller excludes writers.

The C3 fixed owner is 1,568 bytes excluding lazy HAL handles. It borrows at least
64 bytes of comparison scratch and allocates no separate review buffer. The
forced constructor/publication/load C3 probe passes the 256-byte frame limit:
publication 96 bytes, load 176, comparison 80 and finalization 16. Integration
must admit this owner on the heap rather than exceed the stack limit or retain a
permanent pool. This unconnected store does not change the firmware image or
advertise baseline import. Reviewed-cohort backup integration and the native/Apple
import flow still need implementation and recovery verification.

All 1,834 host tests pass. Native store tests use a captured review and cover
immutable repeated publication, both rename failure boundaries, matching staged
prefixes, foreign/corrupt/oversized/duplicate evidence, wrong native identities
including output aliasing, remaining stages, missing files, read/stat/sync/close
failures, write retry and permission-latched loans. Refused loads and conflicts
leave the file map unchanged. Physical SD/power-cut acceptance remains unverified;
when the import route is integrated, interrupt review publication and restart,
then check the sealed hash/context and unchanged learner files before resuming.

### Backups of the frozen reviewed cohort

`HalCourseBaselineReviewBackup` loads the immutable review against native reader,
generation and course identities, then recaptures the current state and requires
the same complete review hash before copying. Each present record gets a private
file at `/.crosspoint/companion/course-review-state-<review-sha256>-<index-hex>`;
the sealed review fixes that index's domain, name, length and hash. Absent journal
roles require missing backup and stage files, without synthetic empty copies.
Source lookup rejects ambiguous names and retains actual ASCII spelling for
case-sensitive HALs.

Each new copy checks the complete source hash before and after streaming, syncs
and checks closes, reads back the complete staged hash, and publishes only when
the canonical destination is proven absent. Existing canonical copies require
the exact expected hash and a missing stage. A matching staged prefix is compared
byte for byte with its fully verified source before appending; foreign, corrupt,
oversized or duplicate evidence is retained. A lost write/rename acknowledgement
can be retried using the same sealed review. Successful preservation requires a
second complete native recapture; changed state or journal files refuse even
when all previous copies exist. Original files and diagnostic usage remain intact.

The fixed C3 owner is 5,928 bytes excluding lazy HAL handles, comprising retained
review capture/store, strict metadata readers and bounded copy paths/handles.
It must be heap-admitted once per operation rather than exceed the stack limit or
occupy a permanent pool. It allocates no separate copy buffer or per-file owner:
copy and prefix comparison reuse the 3,776-byte tail of the shared 8 KiB workspace,
while the bounded review remains in the first 4,416 bytes. The forced C3 probe
passes the 256-byte frame limit: preservation 176 bytes, copy 128, hash comparison
96 and canonical verification 32.

This component still requires the caller to close activities, establish journal
readiness and exclude all namespace/state writers. Its completion loan expires
on close, another operation or permission loss. It neither authorizes pack reuse
nor publishes an archive, and is not yet connected to companion commands.
Native preflight, separate baseline upload, archive/completion recovery and Apple
review/queue integration remain required. This unconnected header is verified by
the direct C3 probe; it does not alter the previously verified firmware image.
Physical heap and SD/power-cut acceptance remain unverified.

All 1,838 host tests pass. New native cases check every present copy's independent
SHA-256, absent roles, immutable repeated preservation, original file maps,
12,000-byte copies interrupted at a later chunk, stage sync/close and both rename
boundaries, changed state or unknown added files, foreign stages/canonical copies,
duplicate names, wrong native generation, read failures, permission-latched
completion, source casing and present/absent journal headers. A writer injected
after the first canonical copy changes source bytes; final recapture refuses
completion and leaves both the original changed state and verified copy intact.
Restoring the reviewed state lets the same operation complete. When integrated,
interrupt each copy on hardware and reboot, verify all original learner files,
sealed review identity/hash and canonical backup hashes, and measure free/largest
heap and task stack watermarks before admitting archive publication.

### Native original-baseline preparation

`HalCourseBaselineImportPreparation` binds an original-pack request to native
reader/storage identities, the authenticated installation owner and an exact
fresh transfer declaration. It verifies that the selected used scope has no
retained archive baseline and that no matching retained removal receipt exists.
Existing history, a fresh empty scope, corrupt records, incomplete directory
scans and permission loss refuse. Both history readers check closes and are
reused after preserving the sealed review's complete cohort. A receipt that
appears during copying prevents preparation from offering a loan.

The operation copies its request before any scratch-backed reads. The prepared
request loan ends on close, another preparation or observed permission/heap
admission loss. Repeated preparation checks the same immutable backups without
rewriting them. By itself it does not save consent, validate incoming pack bytes, mutate
the active course/binding, publish an archive or authorize learner-state reuse.
The caller must establish journal readiness and exclude all namespace/state
writers through subsequent consent and upload admission.

The fixed C3 preparation owner is 224 bytes. Its three checked, nothrow child
allocations total 13,120 fixed bytes (4,496 receipt history, 2,696 archive history
and 5,928 reviewed backup), excluding lazy HAL handles and the already borrowed
8 KiB session workspace. Each allocation checks current free internal heap and
largest block while retaining the 50 KiB reserve. Children remain off stack,
are allocated once per preparation rather than during scans, and are reused for
the final absence checks. A static pool would retain those bytes outside sync
mode. The forced C3 probe passes the 256-byte frame limit: preparation workflow
64 bytes, archive absence check 64 and the constructor/prepare probe 96.

Preparation remains unconnected to companion commands. Consent command wiring,
the separate baseline upload route, full pack validation, archive/completion
recovery and Apple review/queue/UI integration remain required. Committed transfer
recovery re-runs metadata installation, so that integration needs durable
completion evidence before current state can change; a stale frozen review must
not become a new approval. These unconnected headers are checked by direct C3
probes rather than a new firmware image. Physical acceptance remains unverified.

All 1,840 host tests pass. Preparation cases cover retained receipts and archives,
exact owner/generation/transaction/manifest/review bindings, repeated preservation,
missing sealed review, malformed receipt names, failed history enumeration,
insufficient free/largest heap and permission-latched loans. Injecting a valid
receipt after the first backup publication preserves the original state, receipt
and verified copy while refusing the prepared loan. On hardware, verify these
refusals before upload admission and monitor free/largest internal heap and task
stack watermarks across repeated attempts; no active pack or binding should change.

### Native consent after reviewed-state preservation

`HalCourseBaselineImportConsentStore` combines native preparation with the
immutable `TCBI` consent codec. A prepared-request callback saves consent only
after native request binding, absence of retained baselines and complete verified
backups. The same admitted readers then recheck both history sources and the
same backup owner re-verifies the current full review. A changed cohort or newly
retained baseline prevents an approved loan even if consent publication already
completed; the durable record remains evidence of the earlier decision.

Its private HAL storage accepts only the selected transaction's canonical/staged
consent paths. Reads use complete parent scans to reject duplicate names and
aliases. Writes require a missing private stage and exactly one encoded record,
with checked sync/close and readback; rename requires a complete stage and a
missing canonical destination. It never removes or overwrites existing consent.
Complete staged writes and lost rename acknowledgements can resume with the same
request. Torn, corrupt, foreign and duplicate evidence refuse unchanged.

Loading a durable record checks the authenticated owner, native storage generation
and the immutable review's full hash, native reader and course. Failed loads leave
caller output unchanged. A successful load offers the earlier request rather
than an approved-state loan; current learning state may have changed since that
decision. A fresh approval must pass native preparation again. Approved loans end
on another operation, close or observed permission/heap admission loss. The
operation preserves original learning files, active pack/binding and diagnostics.

The fixed C3 store is 1,936 bytes, including its preparation owner and retained
strict readers/codec state. It must be admitted off stack and borrows the existing
8 KiB workspace. Approval retains the three 13,120-byte preparation children,
for 15,056 fixed owner bytes, excluding lazy HAL handles and the borrowed workspace.
Those children are allocated once and reused across persistence and final checks;
there is no growing list or per-file allocation. Loading admits one 1,568-byte
review reader after decoding consent, then releases it. Allocations check current
internal free/largest heap and retain the 50 KiB reserve. A permanent pool would
retain this RAM outside connection mode. The forced C3 probe passes the 256-byte
frame limit: workflow 64 bytes, private record write 64, rename 48, sealed review
load admission 48, public load 32 and constructor/approve/load probe 112.

This connects consent to the native preparation components, but it is not yet
called by companion commands. Baseline upload integration, full pack validation,
archive/completion recovery and Apple review/queue/UI remain required. Loading
consent never bypasses those gates or grants learner-state reuse. No new capability
is advertised. The unconnected headers are checked by direct C3 compilation;
the previously verified firmware image is unchanged. Physical SD/power-cut,
heap/stack and Apple acceptance remain unverified.

All 1,843 host tests pass. Native consent tests cover permission refusal before
mutation, complete preparation/persistence/load, repeated approvals without
writes, output preservation for wrong native reader/generation/owner, lost sync,
close and both rename acknowledgements, torn/corrupt stages, foreign canonical
decisions, duplicate names and leftover stages. A learner mutation injected after
canonical consent publication leaves the original changed state and decision
intact while refusing approval; loading that decision still gives no approval
loan. Restoring the exact reviewed state allows the same request to complete.
When command integration is available, interrupt consent write/rename on hardware,
reboot and verify owner/generation/reader bindings, all backups and unchanged
active pack/binding before resuming the upload; repeat with changed learner state.

### Separate resumable original-baseline upload admission

`beginCourseBaselineTransfer` constructs a fresh declared course transfer from
the exact `TCBI` request and uses `/tinta/course-baseline.pack` as its distinct
destination. It verifies native generation and authenticated owner, requires an
explicit native approval callback, and refuses retargeting an existing transaction
or displacing another active transfer before approval runs. The callback must
save/re-verify the exact native consent with activities closed and other writers
excluded. The declaration is copied before that callback can reuse session scratch;
no request fields are borrowed afterward. The helper allocates no heap buffer;
its forced C3 compile passes the 256-byte frame limit with a 240-byte frame.

Accepted uploads use the existing parent transaction, durable offsets, chunk
verification, reconnect recovery and abort. Repeating begin with the same exact
request keeps the receiving checkpoint and staged bytes. Tinta-disabled builds
refuse before invoking approval or mutating storage. Ordinary declared begin
commands continue to reject this destination, so they cannot bypass the callback.
No protocol command or new capability is advertised for this helper yet.

Native installation remains unfinished. The current HAL rejects committing this
destination before moving the uploaded payload or modifying active pack/binding.
Full original-pack validation, verified archive publication and durable completion
evidence must be connected before offering baseline import. That completion
evidence is necessary for committed recovery after learner progress changes;
consent alone cannot replace it. Apple review/queue/UI and reader command wiring
also remain required. This is upload admission progress, not completed baseline
import. The unconnected helper is checked by direct C3 compilation; the previously
verified firmware image is unchanged, and physical acceptance is unverified.

All 1,848 host tests pass. Native upload tests exercise actual preparation and
durable consent, the 27,104-byte pack fixture streamed in bounded chunks,
reconstruction at a 13-byte durable checkpoint, repeated begin preserving that
checkpoint, and abort retaining consent/backups and the valid active pack/binding.
They also cover absent/refused approval, wrong owner/generation, manifest and
transaction retargeting, ordinary-begin refusal, and caller-buffer reuse during
approval without changing the frozen declaration. Both Tinta-disabled host
variants refuse before approval or storage writes. On hardware after installation
is connected, interrupt begin/chunks and reconnect, check the same durable offset,
then verify unchanged active content and all reviewed-state evidence before commit.

### Historical copies and publication record format

`HalCourseBaselineReviewBackup::verifyStored` checks the immutable review's native
reader/generation/course and every saved copy's full length/hash, including missing
copies for absent journal roles and missing staging files. It performs no source
enumeration, repair or publication. This read-only path remains usable after an
archive reference is added or learner/journal files change, even if a source file
is removed. It never offers the fresh-preservation completion loan; native consent
still requires a matching live review. Missing, corrupt, duplicate or staged
backup evidence and foreign native identities refuse without mutation.

`CourseBaselinePublicationRecord` defines the bounded version-1 `TCBP` publication
record: eight header bytes (magic, version, Prepared=1 or Published=2, two reserved
zeros), native reader identity (16 bytes), exact nested `TCBI` request (155 bytes),
then outer CRC32 (four bytes), totaling 183 bytes. It binds reader, generation,
owner, transaction, original pack manifest and frozen review hash. Both nested
and outer CRCs are checked; unknown phases, reserved fields, zero identities,
invalid requests and overlapping buffers refuse while preserving decode output.
The shared fixture is `protocol/fixtures/CourseBaselinePublication-v1.fixture`.
Prepared is intended to precede archive publication; Published must follow native
archive/copy verification. Decoding either phase grants no publication authority.

The fixed C3 backup owner remains 5,928 bytes and borrows the same 8 KiB workspace;
stored verification adds no owner or buffer allocation and has an 80-byte frame.
The publication record object is 176 bytes, with no codec heap allocation. Forced
C3 compilation passes the 256-byte frame limit: encoding 48 bytes and decoding
240. Native publication persistence and wiring, native full-pack
validation, archive/completion recovery, command wiring and Apple review/queue/UI
remain required. These headers remain unconnected to firmware commands; no new
capability or completed import is claimed, and physical acceptance is unverified.

All 1,852 host tests pass. New native cases verify stored copies after actual
archive publication and changed/removed learner files, refuse incomplete,
corrupt, duplicate and staged evidence plus read/sync/close errors, check absent
roles, and preserve file maps and fresh-approval loan separation. Codec cases
round-trip the shared fixture and both phases, reject a bit change at every byte,
re-signed reserved/phase/identity/nested-record errors, truncation and buffer aliasing.
On hardware after publication recovery is connected, advance learner progress
after a completed import, reboot and verify immutable review/copies and the exact
archive receipt while preserving that newer progress; a receipt alone must never
become a fresh approval.

### Persisted publication workflow

`CourseBaselinePublicationStore` persists immutable Prepared and Published `TCBP`
records under `/.crosspoint/companion/course-baseline-<transaction>.prepared` and
`.published`, each with a private `.tmp` stage. Initial native verification runs
before Prepared is saved. Its canonical readback must succeed before the archive
publisher runs; artifact verification must succeed before Published is saved and
again afterward before the completed loan is offered. The copied request and
hooks remain outside borrowed scratch throughout callbacks.

Complete staged writes and lost rename acknowledgements resume with exact
reader/request/phase matches. Prepared recovery allows the native verifier to
recognize only the exact archive owned by that intent. Published recovery requires
the matching canonical Prepared record and read-only verification of consent,
immutable review/copies and archive; it does not call fresh preparation or archive
publication. It can therefore preserve newer learner state. Orphan Published
records, foreign requests, wrong phases, torn/corrupt stages and canonical-plus-stage
conflicts refuse without deleting evidence. Read failures never fall back to a
fresh publication. Permission loss, close and a new operation revoke completed
loans; reentry refuses.

The generic store allocates nothing internally, borrows at least 183 bytes of
existing workspace and copies records into fixed members. Its C3 fixed owner is
784 bytes and must be heap-admitted off stack rather than occupy a permanent pool.
Forced C3 compilation passes the 256-byte frame limit: publication 48 bytes,
record read 80, immutable save 48 and finalization 16. Callbacks are function
pointers with borrowed context rather than heap-allocated closures.

All 1,860 host tests pass. Faulting portable storage and modeled archive callbacks
cover write/rename failure before and after every phase mutation, applied archive
publication before completion, repeated completed recovery with changed learner
bytes and no fresh approval, foreign/orphan/torn phase evidence, permission/read
failure, input reuse and reentry. A final verification failure leaves completion
evidence intact but offers no loan; retry rechecks historical artifacts without
rewriting records or advancing publication again.

This verifies workflow ordering and recovery with portable fixtures, not a complete
native import. The native storage adapter below supplies checked HAL persistence. The caller
still must supply native full-pack, consent, review/copy and archive verifiers,
hold writer exclusion and finish parent transfer recovery before reading resumes.
Archive integration, command/capability wiring and Apple review/queue/UI remain
required.
No new command is advertised, and these unconnected headers do not alter the
previously verified firmware image. On hardware after integration, cut power at
both record stages and archive publication, recover before entering reading mode,
and verify original/newer learner bytes, archive hashes and native identities.
Physical acceptance remains unverified.


### Native publication persistence

`HalCourseBaselinePublicationStore` binds publication to the native reader,
current card generation and authenticated companion owner before storage or
artifact callbacks run. It copies the selected request before scratch reuse and
only accepts the Prepared request form. The private HAL adapter restricts reads
to this transaction's Prepared/Published canonical files and stages, restricts
writes to a missing corresponding stage and checks the encoded transaction and
phase. Renames must keep the phase and cannot replace a canonical file.

Every lookup uses `HalCourseRemovalMetadata` to finish the parent scan and reject
ambiguous names. Writes require exact length, sync and checked close; namespace
publication requires checked reader closes, complete source/destination lookups
and a missing destination. Resize, removal and generic verification are refused.
A failed final close or permission check withholds the published loan. Observed
permission loss revokes it until another successful operation.

The adapter borrows workspace and allocates no buffer or owner per file. Its
fixed C3 owner is 2,144 bytes, including the portable publication store and strict
metadata reader; admit it off stack with `makeUniqueNoThrow`, retaining the 50 KiB
heap reserve. A stack owner would exceed the 256-byte local limit and a static
pool would retain memory outside the import lifecycle. Forced C3 compilation
passes the frame gate: native publish 32 bytes, write 64, rename 48, path checking
128 and the off-stack construction probe 48. Lazy HAL handles and caller-supplied
artifact verifiers are additional resources.

All 1,863 host tests pass. Native tests exercise context refusal, permission
revocation, completed replay
without fresh approval, both phases' sync/close/write/rename failures including
applied rename acknowledgements, corrupt/torn evidence, duplicate names and
lookup errors. Their artifact callbacks model archive state; they do not prove
native full-pack or archive validation. This header is not yet connected to the
firmware command path. After integration, verify both phase files and their CRCs,
cut power around each stage/rename and archive publication, and confirm recovery
preserves newer learner state while checking free/largest heap through Serial.

### Native completed-publication evidence

`HalCourseBaselineArchiveSession` supplies the read-only `verifyPublished`
callback for the publication workflow. It binds the input to native reader,
storage generation and authenticated owner, copies the request before scratch
reuse, loads the exact immutable consent, checks every historical review copy and
opens the exact retained archive. Archive opening verifies its full SHA-256 and
matching manifest; the production `validateStagedCourse` parser then validates
all pack sections and checks the declared format version. Missing, ambiguous,
corrupt or pending evidence, permission loss and checked-close failures refuse
verification. The original upload is not needed once the archive is retained.

Historical verification never recaptures or modifies live learner state, creates consent,
repairs files or offers fresh approval. Its callback can accept the workflow's
copied Prepared request while proving historical artifacts; it does not establish
that a Published phase record exists. The publication store separately proves
those phase records. Caller-owned writer exclusion and journal readiness remain
required, as do initial full-pack/item compatibility checks and Prepared recovery.

Forced C3 compilation passes the 256-byte frame gate: verification and the
construction probe each use 64 bytes. The fixed off-stack archive session is 12,064
bytes, including retained consent, backup and archive owners. Its borrowed parser
is another 720 bytes; consent loading transiently admits a 1,568-byte review
reader, totaling 14,352 fixed bytes before lazy HAL handles and borrowed session
workspace. A single retained owner avoids per-file allocation and repeated child
owner allocation during final verification; a stack/static owner would violate
the local limit or retain this memory outside the import lifecycle. Admit owner
and parser with `makeUniqueNoThrow`, check failures and retain the 50 KiB reserve.

All 1,865 host tests pass. Native tests use actual consent, copied review files,
archived pack bytes and the
production parser, including repeated verification after newer learner writes,
removed upload source, missing/corrupt evidence, archive sync/close failures and a
malformed pack whose SHA-256 matches its manifest. The command path remains unconnected. On hardware after integration,
verify completed recovery without the original upload, compare newer learner
bytes before/after, inspect archive and copy hashes, and monitor free/largest heap
and parser task stack. Hardware acceptance remains unverified.


### Native archive publication

The archive session also supplies `publishArchive(request, source)`. Before any
archive mutation, it checks the exact canonical Prepared intent through complete
HAL namespace lookup, requires its stage to be absent, verifies native immutable
consent and every retained review copy, and checks source length and SHA-256. It
then validates the full source pack with the production parser before publishing
through `HalCoursePackArchive`. The request and bounded source path are copied
before scratch reuse. Checked source sync/close and final closes are mandatory.
The existing upload and learner files are preserved; no fallback archive is made
from unverifiable input.

This does not grant fresh approval or prove live learner compatibility. The
publication workflow must still supply honest fresh and Prepared recovery
verification, including recognition of only its own archive reference after an
interrupted publication. Initial item compatibility and parent transfer commit
remain required before command wiring. The session reuses its existing archive,
consent, backup and parser owners, adding fixed metadata, source handle and path
members rather than per-file buffers. C3 publication is 96 bytes, intent checking
80 and source hashing 96; the revised fixed footprint below includes these fields.

An integration test combines actual native consent/copies, phase persistence,
full-pack archive publication and historical verification. It refuses publication
without Prepared intent, then completes and replays after removal of the upload
and a newer learner write while fresh/publication callbacks are disabled. This test now uses the native fresh verification callback. Hardware and
command-path acceptance remain unverified.

A source-refusal matrix checks complete-stage conflicts, torn/foreign Prepared
records, source length/hash mismatch, source sync/close failures, wrong owner and
matching immutable intent/consent for a malformed source. Every refusal preserves
all file bytes and rename counts. Restoring valid evidence permits publication
and subsequent historical verification through the same retained session.

All 1,867 host tests pass after the native publication integration and refusal
matrix. With the 2,144-byte native phase store retained alongside the session,
parser and transient review reader, the combined fixed peak is 16,496 bytes,
excluding shared workspace, caller-owned transfer state and lazy HAL handles.


### Prepared recovery after canonical archive publication

`HalCourseBaselineArchiveSession::verifyPrepared` checks native consent, immutable
review copies, absence of retained removal receipts and the current reviewed
learner/journal/isolation cohort. Fresh verification refuses an existing archive.
Recovery first proves the exact canonical Prepared intent, then accepts only its
matching archive manifest, full hash and full pack validation. The capture accepts
the verified native archive handle and omits only its exact immutable reference
from learner hashing. It checks that reference through complete HAL lookup and
exact binding decode before and after enumeration, requires exactly one matching
entry and rejects other references/stages or ambiguous names. All learner bytes,
file membership and isolation/journal evidence must still produce the approved
review SHA-256. The read-only comparison never offers a backup-complete loan.

An integration test stops after actual archive publication with only Prepared
intent durable, then exercises native recovery. Fresh verification refuses the
archive; changed learner bytes or a foreign reference refuse recovery and retain
all evidence; restoring the cohort permits idempotent publication and completion.
The ordinary completion test now uses native fresh verification rather than a
modeled preparation callback. Completed recovery still uses historical evidence
and preserves newer learner state.

C3 compilation passes the 256-byte frame limit: Prepared verification 80 bytes,
receipt admission 48, capture 112, scope enumeration 112 and reference comparison
144. Capture's fixed owner is now 2,352 bytes; the backup owner is 6,136, consent
1,936 and archive session 12,272. The extra 208 bytes in capture hold a copied
reference path/manifest and flags, avoiding borrowed names that scratch reuse
could invalidate. Prepared verification admits a 4,496-byte receipt reader once
per operation with `makeUniqueNoThrow`; its size exceeds the local stack limit and
an import-only static pool would retain memory outside the session. Including
session, parser (720), phase store (2,144) and receipt reader, the fixed peak is
19,632 bytes before lazy HAL handles, shared workspace and parent transfer state.

Initial item compatibility, parent transfer/command/capability wiring and Apple
review/queue UI still remain. The staged-reference extension below adds verified
prefix recovery; foreign or corrupt stages remain preserved and refused. Hardware
acceptance remains unverified. After integration, cut power after canonical
reference publication and verify Prepared recovery before reading resumes;
compare learner bytes/review hash and monitor free/largest heap and stack.

All 1,868 host tests pass after the canonical-reference Prepared recovery changes.


### Prepared recovery with a pending archive reference

`CoursePackArchive::inspectPrepared` is read-only. It recognizes a canonical
reference or an exact pending binding prefix only when the archive owner matches
the complete requested manifest, the canonical cache passes its full length and
SHA-256 checks, and no cache stage exists. A canonical reference plus a pending
reference is a conflict. Oversized or foreign prefixes, torn/foreign owners,
missing/corrupt caches, errors and permission loss withhold all archive loans.
Ordinary `open` still refuses pending references; the inspection loan is
specifically for caller-proved Prepared recovery and cannot prove publication.

The native wrapper offers this inspection with checked reader closes. Prepared
verification proves exact native intent/consent/review copies and receipt absence
first. It validates the full cached pack and recaptures the live reviewed cohort,
omitting only the proved pending reference. The capture snapshots its exact
length and expected binding prefix, checks them before and after the scan and
requires exactly one matching entry. A changed or foreign reference, alias or
another staged reference cannot silently disappear from the review. Publication
then uses the existing byte-proved reference-stage resume; no learner state or
immutable evidence is rewritten during verification.

Portable tests inspect every prefix length without writes and keep ordinary open
refused. Native integration resumes empty, one-byte, half and complete pending
references through the actual phase workflow, preserving learner bytes. Foreign
prefixes, duplicate canonical/staged reference evidence and reference close
failures refuse unchanged. Initial item compatibility, parent transfer and
command/capability/Apple wiring remain required. Hardware acceptance is unverified;
after integration, cut power during each reference-stage write/rename and verify
recovery before reading resumes, learner hashes and free/largest heap.

Forced C3 compilation passes the 256-byte frame gate: Prepared inspection is
80 bytes and reference comparison 144. Revised fixed owners are capture 2,360,
backup 6,144, consent 1,936 and archive session 12,288 bytes. The added stage flag
and snapshot length use fixed members and existing borrowed comparison workspace;
there are no new buffers or per-file allocations.

All 1,871 host tests pass with this extension. The measured receipt reader remains
4,496 bytes and native archive owner is 2,312; session, parser, phase store and
receipt reader total 19,648 fixed bytes, excluding borrowed workspace, parent
transfer state and lazy HAL handles. The affected default firmware image passed validation as recorded below;
host/frame results do not prove hardware behavior.

### Native baseline transfer installer adapter

`HalCourseBaselineNativeInstaller` implements a borrowed serialized installer
interface for the generic transfer's preparation and metadata callbacks. It
freezes transfer/manifest bindings before consent I/O, requires native reader,
card generation and authenticated companion owner, and accepts only the dedicated
baseline destination with the complete transaction length and hash. Preparation
accepts only the transfer stage in Receiving, validates its complete SHA-256 and
pack structure, requires explicit item-compatibility verification against the
immutable review, and checks the live reviewed cohort without publishing files.

Installing validates the canonical upload and compatibility again, then drives
the native Prepared/archive/Published workflow. A repeated Installing call uses
that workflow's durable recovery. Committed metadata checks both exact canonical
phase records, absence of both stages and historical archive/consent/review-copy
artifacts. It cannot synthesize missing publication evidence or require new
compatibility approval; newer learner progress remains untouched. The canonical
upload must remain until the parent transfer no longer depends on it during its
own recovery. Neither the installed active pack nor its course binding is changed
by this baseline adapter.

The factory checks permission, requires a compatibility function and admits the
entire 18,888-byte C3 owner plus its contiguous block while preserving the 50 KiB
reserve before `makeUniqueNoThrow`. It logs admission/allocation failure. The
owner retains the parser, consent reader, archive session, phase store and strict
source/phase lookup handles outside the stack; it borrows session workspace and
allocates no buffer per chunk. Prepared verification additionally admits a
4,496-byte receipt reader, so its fixed peak is 23,384 bytes before SDK/lazy handles,
parent transfer and compatibility-callback resources. A stack object would exceed
the local limit; a static pool would keep those resources after sync mode exits.
C3 preparation is 48 bytes, metadata 64, source validation 112 and phase checking
64. Caller must detach the borrowed interface before destruction.

The adapter test uses actual native approval, source bytes, phase records and
archive validation, while the compatibility callback is modeled. It checks
read-only preparation, owner/card/transaction/offset refusals, explicit callback
refusal, Installing publication and Committed verification after learner progress
changes without another compatibility call. Missing completion evidence refuses
without repairs. Admission coverage checks permission, missing compatibility,
reserve and largest-block constraints. The HAL attachment below connects this interface to
`HalTransferStorage`; native item compatibility, command/capability wiring and
Apple review/queue/UI still remain. The concrete installer remains unconnected to firmware session/startup ownership;
the HAL-attachment image validation below does not prove that ownership.
Hardware acceptance remains unverified.

All 1,873 host tests passed after installer-admission coverage. The final validation
below includes the subsequent HAL attachment and parent recovery cases.


### HAL attachment and parent transfer recovery

`HalTransferStorage::setCourseBaselineInstaller` lends a serialized installer owner
for the dedicated baseline destination. The state-aware validation callback routes
Receiving preparation to it; Installing and Committed metadata route publication
and historical verification to it. An unattached owner refuses baseline commit
and recovery. The generic unstated callbacks still do not grant baseline
installation. Caller attaches before parent recovery and detaches before owner
destruction; the HAL adds one borrowed pointer (4 bytes on C3) and no allocation.
No command/capability is advertised by this attachment.

Native integration tests use real `Transfer`, `HalTransferStorage`, consent,
source validation and phase/archive persistence with a modeled item-compatibility
callback. Full commit preserves the active pack and course binding. Repeated
commit and reconstructed Committed recovery preserve newer learner bytes without
another compatibility call or file writes. Detaching the installer refuses
metadata without altering evidence. Installing recovery covers destination
rename failure before/after application and complete Prepared/Published stage
sync failures; reconstructed native owners resume to Committed without replacing
reviewed learner files. This does not prove native item compatibility or firmware
session/startup owner attachment, which remain required before command exposure.

Whole-source C3 compilation passes the 256-byte frame gate with Tinta enabled and
disabled. Enabled state-aware validation and metadata each use 192 bytes; disabled
variants each use 48. The previous pending-reference default image also passed its
firmware build (552.95 seconds, no warnings/errors) and official x4/chip-5 validator:
6,507,600 bytes, SHA-256
`2f36572b57cb3b5b9e15d6095424d1fc43c5ead233b40b3c4bb6af8fcfb79a35`,
with 46,000 bytes remaining in the 6,553,600-byte OTA partition. It was retained
before the HAL-attachment build; it does not prove the subsequent attachment.
The affected HAL-attachment default build passed in 81.30 seconds with no warnings
or errors. Its official x4/chip-5 image validation passed: 6,507,808 bytes, SHA-256
`44dd46c9b7b4c6ee14903de74292449a78268630e6f1fd2c8ca4476ee3672d27`,
with 45,792 bytes remaining in the OTA partition. The image was retained before
further changes. Hardware acceptance, other final affected targets and Apple
builds remain unverified.

Final host rebuild has no warnings or errors; all 1,875 tests pass. The default
firmware image and enabled/disabled C3 HAL frame checks passed after the final
firmware source edits. Later test-only brace fixes do not alter that image.

The native baseline compatibility work now shares the local `ProgressStore`
header decoder through `core/srs/ProgressHeader.h`. `HalTintaLegacyItemView`
borrows a hash-verified immutable reviewed file and 160 bytes of workspace. It
selects the newest valid header and presents its pending record without writing
local recovery back into the reviewed evidence. A missing final pending record
can be inspected from its header; other missing records, ambiguous equal-sequence
headers, failed reads, changed extents, invalid item records, and cancellation
invalidate the view. One corrupt header can fall back to the other valid header.
The caller must exclude writers and verify the complete reviewed file hash.
This is a record inspection component, not a complete compatibility decision:
UID meaning, duplicate detection, review-log replay, and the remaining learner
formats still require validation before replacing the modeled installer callback.

This view adds no allocation and occupies 104 bytes on ESP32-C3. Forced C3
compilation reports 128 bytes for `begin`, 32 bytes for the record probe, and
112 bytes for the shared header decoder, each below the 256-byte frame limit.
All 1,883 host tests pass, including corruption of every header bit, pending and
undo evidence, torn final-record inspection, read faults, and cancellation. The
default firmware build for the shared decoder refactor has passed;
the standalone view is not yet included in the production import path. These
checks do not establish item compatibility or physical power-cut acceptance.

`CourseUidLookup` now provides read-checked lookup in a previously fully
validated immutable pack. Binary search reads the IUID records directly and
checks a found index against the ITEM UID. Successful absence returns index -1;
read failure, extent change, or an inconsistent index invalidates the lookup
without changing the caller's output. This avoids treating `Pack::indexOfUid`'s
shared absent/read-failure return as evidence that an item was retired.

`inspectTintaLegacyItemCatalog` combines that lookup with the reviewed-item
view. A borrowed bitmap detects repeated active UIDs, and a report distinguishes
mapped, retired, and tombstone records. Retired records are preserved as in local
`ProgressStore`; their absence is not proof of compatible meaning. The retained
undo before-image must name the UID of its logical slot. Original-pack consent,
identity continuity, review-log replay, and other learner formats remain separate
gates; the production baseline installer still uses its required compatibility
callback and does not yet create these inspectors itself.

The lookup occupies 44 bytes on C3 and allocates nothing. Its forced C3 frames
are 128 bytes for directory loading and 64 bytes for lookup. The item audit uses
at most 4,256 borrowed workspace bytes (160-byte header workspace plus a bitmap
for 32,767 catalog entries), with a 256-byte C3 frame. No new heap or static
workspace is introduced. All 1,886 host tests pass, including actual fixture-pack
UID lookup, every read-failure position in a selected search, extent changes,
and duplicate versus retired reviewed records. This does not establish the
remaining baseline integration or physical acceptance gates.

`inspectTintaLegacyReviews` now streams an immutable hash-verified reviewed
`reviews.log` through the existing `LegacyTintaJournalDecoder`. It checks review,
flags, and undo syntax; retains the decoder's zero-tail rules; distinguishes
mapped and retired UIDs using the read-checked course lookup; and refuses fewer
valid records than the item header's committed count. Failed reads, malformed
undo references, nonzero data after the tail, changed extents, and cancellation
withhold the report. Caller-owned handles and workspace remain borrowed; no
learner files are replayed, truncated, or replaced. This is syntax and catalog
inspection, not proof that replay yields the retained snapshot or that legacy
records have distributed provenance.

All 1,887 host tests pass after this addition. Its forced C3 probe has a 128-byte
frame and introduces no allocation. The default firmware build passed
for the shared local header-decoder refactor; these inspection helpers are still
outside the production baseline installer until the remaining learner formats
and full native compatibility composition are implemented.


The completed default C3 build took 557.11 seconds with no warnings or errors.
The official release-image validator accepts the resulting X4/chip-5 image:
6,507,808 bytes, SHA-256
`7210283c248689c81b259f05e3596af19ce2a4fc74f9f90c48d4683937b3791f`,
with 45,792 bytes left in its 6,553,600-byte OTA partition. The retained binary
and validation metadata are `/tmp/lila-progress-header-images/default.bin` and
`default.json`. This image includes the shared decoder refactor and existing HAL
baseline attachment, but the new standalone inspection helpers are not yet wired
into the production baseline installer. The host suite (1,887 tests), forced C3
helper probes, and this default build cover separate scopes; they do not prove
other board builds, complete native compatibility, or physical acceptance.

The reviewed-profile inspector now uses `Profile::load` through a read-only
`StateStore` adapter over a borrowed HAL handle. It verifies the declared exact
extent, streams the native CRC/field decoder, refuses defaults/corrupt results,
and reports Loaded versus Upgraded explicitly. No save or replacement is called;
future/older payloads do not require an allocation sized to the payload. The
caller still decides whether an Upgraded profile is acceptable and checks its
course-dependent lesson fields against the candidate.

`HalTintaLegacyMarkView` reconstructs the actual legacy `TMK1` add/remove logs
used by `starred.bin` and `read.bin` (see `App.h` and `StateFiles.cpp`). It retains
insertion order and idempotent changes, and refuses malformed/torn records,
invalid keys, overflow beyond the native 96-key capacity, read failures, changed
extents, and cancellation. This is distinct from the canonical companion
completion-set format. Caller-owned immutable bytes remain untouched; the view
borrows 384 bytes for keys and occupies 40 bytes on C3. All key access uses byte
codecs, including the borrowed buffer, so no alignment assumption is introduced.

All 1,891 host tests pass. Standalone C3 probes report 128 bytes for profile
inspection, 32 bytes for adapter reads, 112 bytes for mark loading, and 32 bytes
for the mark-key access probe. Neither helper adds heap allocation. The new
headers are not included by the production installer yet; no existing firmware
translation unit changed in this checkpoint, so the previous successful default
image remains the firmware evidence for its stated scope. Profile lesson checks,
mark-key continuity, saved-session/usage validation, and the native compatibility
composition remain unfinished, along with the broader companion-plan gates.

The native legacy reference inspectors now check profile lesson indices against
the explicitly confirmed original pack, including the one-past-the-end completed
sentinel, and read stable identities for the completed prefix. Mark references
use the read-checked IUID lookup for starred items and the existing title-key
resolver for completed readings. Failed reads and ambiguous title keys refuse
the report; absent references are counted separately without deleting them or
asserting compatible historical retirement. Explicit original-pack consent and
course identity continuity remain caller obligations.

The generic `TMK1` view permits `UINT32_MAX` as a legacy title hash. The item-UID
role rejects that reserved value separately; a title-derived key is not an item
UID. Tests cover native lesson ranges, the completed sentinel, actual pack UID
and title references, duplicate-title ambiguity, cancellation, and unchanged
reviewed marks. All 1,894 host tests pass. Forced C3 compilation reports 32 bytes
for the lesson probe and 80 bytes for the mark-reference probe; the existing
story/subject identity readers use 80-byte frames. No allocation or learner
writes are added. These standalone headers are not yet in the production
baseline installer. Saved-session/usage validation and a complete native
compatibility owner are still required before enabling baseline commands.

Saved-session inspection now reuses the native `SessionFile` decoder for TSES
versions 1–3 and audits the nested controller/TSQ1 queue without restoring it.
Both queue versions are supported. The inspector checks nested CRCs, declared
extents, reserved fields, statistics bounds, queue flags/UIDs, caller-supplied
screen/depth/capacity limits, and lesson/phrase-category practice targets. Checked
UID lookup distinguishes retired queue references from read failures. A changed
saved journal count is reported rather than rejected: native restoration can
rebuild a queue when authority changes. Snapshot presence is reported separately
and does not establish that its hash matches the retained learner cohort.

The file and report remain unchanged on failure; no session or progress writes
occur. The HAL helper borrows workspace for the complete saved file. Native App
uses depth 8 and SessionController capacity 200, giving a 1,105-byte maximum
saved-file buffer; these limits remain caller arguments rather than a duplicate
UI enum in the protocol layer. No heap allocation is introduced. Forced C3
compilation reports 160 bytes for the HAL inspection probe, 112 bytes for nested
session decoding, and 96 bytes for the existing envelope decoder.

All 1,898 host tests pass, including every single-bit envelope corruption,
old envelope/queue versions, nested corruption, retired UIDs, journal changes,
HAL read failure, insufficient workspace, cancellation, and missing practice
targets. These standalone helpers remain outside the production baseline
installer. Full immutable-cohort composition, snapshot/replay correspondence,
usage-file inspection, native session ownership, command/Apple integration, and
physical acceptance remain incomplete; the prior default firmware image is
unchanged by these new unwired headers and host tests.

### Native baseline inspection ownership

`HalCourseBaselineLearnerInspection` now composes exact persisted consent,
hash-verified immutable review backups, complete candidate-pack validation, and
read-only native items, reviews, profile, marks, session, and day-log inspection.
It reuses the borrowed workspace and reloads the reviewed manifest before each
file, so scratch reuse does not overwrite the next entry. Current learner files
are never substituted for reviewed backups. Missing item references require
retained candidate identity history; ambiguous reading references and unknown
learner filenames refuse publication without changing evidence. Diagnostic
`usage.seq` and numbered usage logs are preserved by their verified hashes;
their contents do not define learner progress, and torn diagnostic chunks do not
invalidate otherwise compatible learner state.

`HalCourseBaselineImportSession` owns the inspector, its parser, and the native
installer in one checked allocation, with destruction ordered to release the
callback user before its target. Callers must retain the borrowed workspace and
permission context, exclude writers, prove native journal readiness, and detach
the installer from transfer storage before releasing the owner. This composition
does not establish authoritative journal replay or saved-snapshot correspondence.

The forced C3 compile measures a 31,136-byte session and a 4,496-byte temporary
archive history reader. Admission accounts for both (35,632 bytes), requires a
contiguous session block, and retains over 50 KiB free internal heap. Workspace,
allocator overhead, SDK allocations, and parent-session objects are additional;
these compile-time sizes are not measured device peak heap. No per-file heap
allocation is introduced by learner inspection. All emitted frames pass the
256-byte compiler limit.

All 1,903 host tests pass, including full baseline transfer publication through
the native compatibility callback, immutable-backup inspection despite newer live
state, semantic rejection of hash-valid malformed evidence, and combined-owner
heap admission. Startup/Connect wiring, protocol and Apple review commands,
recoverable torn consent/publication stages, and physical acceptance remain
unfinished. These headers are not yet instantiated by firmware recovery. Device
verification must exercise interrupted installation and repeated sessions, check
free/largest heap and stack watermarks, and confirm retained progress and newer
live state survive retries.

Connect's ordinary transfer partition is only 940 bytes: four queued command
slots and three frame buffers occupy the other 7,252 bytes of the 8 KiB workspace.
Baseline parsing therefore needs an exclusive loan of the full workspace while
transport queues and frame loans are inactive. The import session now bridges
the exact parent partition to that full loan without allocating another buffer.
Its permission callback must prove this exclusion throughout every operation.
Foreign, short, or out-of-loan parent spans are rejected. The native transfer
fixture now uses the actual 940-byte parent partition for interrupted recovery;
revoking permission leaves the queued area unchanged. This is a workspace
contract, not yet a transport lease implementation or Startup/Connect wiring.
Both bridge methods compile with 32-byte C3 frames.

### Boot and Connect baseline recovery

`HalCompanionRecovery` and `CompanionConnectActivity::onEnter` now inspect the
pending transfer before normal recovery and attach a native baseline session
only for the dedicated baseline destination. The helper rechecks native hardware,
card marker, and storage-generation binding without reserving another epoch. It
reloads the durable pairing registry and requires the recorded installation owner
to remain known. Native installer callbacks still require exact persisted consent
and verified immutable evidence before publication; recognition does not authorize
new consent or transfer commands.

Canonical journal publication/abort recovery precedes this attachment. Journal
readiness first proves absence or requires an existing events file and at least
one header, so an empty or incomplete directory cannot create new authority.
For an existing journal it invokes the native recovery and causal/undo audit;
only uncommitted trailing bytes may be truncated by that recovery. This audits
current canonical journal readiness, not correspondence between reviewed journal
backups, candidate subjects, and saved-session snapshots.

The full 8 KiB workspace is lent only before BLE starts and while frame/queue
loans are inactive. Both callers revoke that permission, detach the installer,
and release the import owner immediately after normal transfer recovery, before
inventory work or transport activation. Live baseline review/transfer commands
still need a separate exclusive transport lease and are not enabled by this
change. The startup recovery gate remains closed on identity, pairing, audit,
consent, or publication failures; retained evidence is not silently discarded.

Persisted screen IDs and the native depth/queue capacities now share
`app/PersistedSessionLimits.h`; the enum values are unchanged. The native helper
uses these limits rather than test literals. Forced C3 compilation passes all
256-byte frame checks: journal readiness uses 80 bytes, attachment 256 bytes,
boot recovery 144 bytes, and Connect entry 160 bytes. Presence (548 bytes),
metadata (1,072 bytes), and audit (4,200 bytes) owners are admitted off stack and
released sequentially before admitting the 31,136-byte import session. These
figures exclude SDK handles, allocator overhead, and parent-session objects.

All 1,912 host tests pass. Native integration covers interrupted publication
through the actual attachment helper, read-only identity/pairing proofs,
exclusive-workspace refusal, missing/incomplete journal evidence, and native
uncommitted-tail recovery. The final default build containing these production
callers passes the repository image validator for x4/chip 5: 6,546,128 bytes,
SHA-256 `78fadb84672908adc13fbc8a8206f2bd4ff233eea0522b0ea6cb106333b113ce`.
The 6,553,600-byte OTA partition has 7,472 bytes of headroom. Other affected
firmware targets and physical boot/Connect acceptance remain pending; protocol
and Apple review commands, torn consent/publication-stage recovery, reviewed
authority correspondence, and full transport leasing remain unfinished.

The subsequent board batch built and image-validated sticky (5,789,600 bytes,
764,000 bytes of OTA headroom), x4c (6,550,272 bytes, 3,328 bytes of headroom), and
papermono (5,904,080 bytes, 649,520 bytes of headroom). X4pro failed during framework
package copying with system error 23 (`Too many open files`), before compilation.
The SDK speech components also emitted their existing discarded-const warnings
on sticky. This batch is not final acceptance of the later BLE workspace-lease
changes: a final five-target run is pending, including the x4pro retry. Physical
acceptance and the remaining baseline protocol/authority work are still required.

The Apple package now defines the native 155-byte `CourseBaselineImportRequest`
codec, including its CRC and exact generation/owner/transaction/manifest/review
binding. Added tests use the shared native fixture and cover truncation, byte
corruption, re-signed empty identities, oversized content, and retargeted consent.
The fixture layout and CRC were checked independently. This codec does not enable
a baseline command, create consent, or provide the original-pack confirmation UI.

Apple also has a bounded `CourseBaselineReview` decoder matching the native roster
rules: canonical ASCII names/order, stable learner filenames, exact journal and
isolation records, lengths/presence/hashes, identities, and CRC. It hashes the
complete frozen record and checks confirmation against its reader, generation,
course, and hash. The shared fixture's eight entries, CRC, and SHA-256
`d02419c360bc310e304f166a08cfcc28cafcfb114d514097f02fc2991b1100eb`
were independently checked. Added Swift tests cover malformed/re-signed rosters
and retargeted confirmations. Roster validation
does not establish the contents or semantic correspondence of backed-up files.

All 529 CompanionKit tests pass with Swift 6.0.3 on Debian 12/aarch64, including
the confirmation/review and durable-consent tests. The isolated official toolchain's archive
signature was verified against Swift's published release key. This validates
the portable package; Xcode iOS simulator/native Mac builds, UI tests, and
physical device acceptance remain required.

SQLite schema 41 stores baseline confirmation and its exact frozen review in a
local job-bound table. Confirmation is explicit through
`confirmCourseBaselineImport`, restricted to queued/paused jobs with zero durable
offset, matching reader/generation/course, and no abort or library deletion.
Baseline and course-switch consent are mutually exclusive. Loading revalidates
the review hash and full manifest against the job; retries cannot replace a saved
review. Tests cover reopen, changed/foreign reviews, started transfers, version-40
migration with job preservation, and corrupt stored bindings. These records do
not enter CloudKit and do not authorize a command without reader-side checks.

`queueCourseBaselineImport` selects the original pack, saves confirmation, and
creates or reuses the exact queued job in one SQLite transaction. It refuses
conflicting reader jobs, pending removals, aborts, and changed owners/reviews;
selection failures roll back the new job and consent together. A retained ordinary
transfer declaration prevents converting a zero-offset job into baseline intent.
The ordinary BLE/Wi-Fi runner and handoff declaration preparation refuse baseline
jobs before sending Begin; the runner leaves the job unchanged. Dedicated live
baseline transfer commands and their Apple UI still require implementation.

`CourseBaselineJournalSnapshot` now exposes verified reviewed journal copies to
the native `TintaJournal` parser through a read-only storage adapter. It validates
the canonical review/hash and the three journal-copy hashes, copying all file
descriptors before verification reuses shared scratch. It refuses writes, header
creation, and trailing-byte truncation. Native-header integration tests prove
empty committed journals can open, corrupt headers/tails cannot be repaired,
absent evidence cannot create authority, and review/permission failures preserve
all files. All 1,918 C++ host tests pass. Forced C3 compilation reports a 516-byte
metadata owner, 112-byte open frame, 80-byte header-read frame, and 144-byte review
decode frame. The owner must be admitted off stack; the adapter allocates no
buffer or heap itself.

Native learner inspection now audits a present reviewed journal through this
adapter before inspecting learner files. The existing causal audit accepts a
borrowed source with explicit idempotent cleanup, preserving its native causal
closure and undo-target checks. A validated candidate subject catalog checks
item/lesson/reading membership, including retained item identity history. Actual
capture/consent/backup integration tests accept a matching subject and refuse a
hash-verified missing subject while preserving learner and review files. Cleanup
failure withholds the frontier; no reviewed copy becomes writable. Disposable
journal indices may be rebuilt by the audit.

All 1,920 C++ host tests pass. Forced C3 compilation checks the full wired learner
inspector with the 256-byte limit: inspection uses 192 bytes and native causal
audit 144 bytes. The temporary reviewed-journal owner is 5,344 bytes, admitted
once per inspection outside the learner-file loop and released before that loop.
It reuses the existing workspace for hash and catalog verification; fixed native
journal/index buffers and paths require checked heap ownership rather than the
small task stack. The import owner remains 31,136 bytes and its admitted peak is
now 36,480 bytes plus the 50 KiB reserve, taking the larger of historical-history
and reviewed-journal owners. The general causal-audit owner measures 4,208 bytes
after adding borrowed-source cleanup metadata. SDK handles/allocator overhead and
physical free/largest heap remain separate acceptance checks.

The preceding image batch validated default, sticky, x4c, and papermono for the
workspace-lease checkpoint. X4pro again failed during framework package copying,
before compilation. Those images do not establish acceptance of this later
reviewed-journal wiring. The new batch at checkpoint `5b6b0ee3` has built
default and sticky successfully. Their actual binaries pass the repository
image validator: default is 6,548,608 bytes (4,992 bytes of OTA partition
headroom), and sticky is 5,789,776 bytes (763,824 bytes of headroom). X4pro
failed again during framework package copying before compilation. X4c has
also built successfully and passed image validation at 6,552,752 bytes
(848 bytes of OTA partition headroom). Papermono also built successfully
and passed image validation at 5,904,208 bytes (649,392 bytes of headroom).
The batch finished with four successful targets and the x4pro package-copy
failure. The x4pro retry with packages and caching on local `/tmp` storage
passed package installation and compilation, then failed the program-size gate:
6,576,986 bytes against 6,553,600 allowed (23,386 bytes over). The linker
reported 6,582,062 bytes of image content before binary padding. No validated
x4pro image was produced for this checkpoint. Flash reduction is required
before that target can pass. A temporary `-Oz` experiment, verified on both
compiler and LTO invocations, also failed: the program-size gate reported
6,577,162 bytes, 176 bytes larger than the existing settings. That experiment
was not adopted; repository optimizer flags remain unchanged. Full authoritative
replay correspondence with learner caches and saved sessions, torn-stage recovery,
live commands/Apple UI, and physical acceptance remain unfinished.


The automatic-inlining experiment also failed the x4pro size gate at 6,575,554
bytes (1,432 bytes below the original program-size result); its flags were not
adopted. The subsequent shared-font implementation retains the source font
headers and generates deduplicated headers through
`scripts/share_builtin_font_groups.py` during the normal PlatformIO pre-build.
`python3 test/builtin_font_groups/test_shared_groups.py` verifies all 600 groups
byte for byte, decompresses their streams to the original lengths, checks that
other declarations are unchanged, and checks deterministic regeneration.
The input fonts contain 88,869 duplicate stream bytes. Shared pointers add
2,400 bytes to their 600 group descriptors on 32-bit targets; this is a flash
data change with no new heap allocation or decompression buffer. Original
five-field group initializers retain the offset-based path through the default
null shared pointer. The generated headers compile for ESP32-C3 with 24-byte
aligned group descriptors. All 1,920 host tests pass after the change. The
normal-config x4pro build passes, and its actual image passes the repository
validator at 6,495,760 bytes, leaving 57,840 bytes of OTA partition headroom.
The image SHA-256 is
`0c19778e56a3e5632ba7fbe7dc154e9f7afe3b6c3ad4243b5aa70f1d45b12738`.
The previous overflowing build did not produce a final binary, so an exact
before/after binary-size delta is unavailable. Final builds for the other four
affected targets need re-verification: their process handle, process and `/tmp`
logs disappeared before terminal results could be retained. The exact x4pro
binary was recovered from `.pio/build/x4pro/firmware.bin`, revalidated with
the SHA-256 above, and copied with its metadata to ignored workspace cache
`.cache/companion-verification/shared-fonts/`. New verification logs and images
use that workspace cache. The host typesetting-preview target also runs
the generator before compiling, preserving its fresh-checkout build path.
That target built successfully and rendered a one-page sample in both Times
and Helvetica with regular, bold, italic, bold-italic, Cyrillic, Greek and
accented text. The output pages contain 6,993 and 8,436 ink pixels respectively.
This is a host rendering smoke check, not pixel-equivalence or physical-display
acceptance. The generator test also verifies that every source font remains in
the include roster, including uncompressed UI fonts.
A subsequent original-font reference renderer was linked with the same host
renderer code and the unchanged source tables. Its page bytes match the shared
tables exactly for Times and Helvetica at sizes 8, 9, 12, 14 and 16: ten sample
pages covering the styled multilingual text above. This establishes pixel
equivalence for that host sample, not all books or physical-display acceptance.
On hardware, verify the built-in font families, styles and sizes, including
non-Latin fallback glyphs, across reading and UI screens and repeated cache
warm-up/eviction. Physical rendering and heap acceptance remain pending.


The restarted shared-font verification batch has completed default successfully.
Its actual image passes the repository validator for x4/chip 5 at 6,462,480
bytes, leaving 91,120 bytes of OTA partition headroom. Compared with the
previously validated 6,548,608-byte default image at checkpoint `5b6b0ee3`,
the binary is 86,128 bytes smaller. SHA-256:
`e37d21604a73286f3a87d1f14cffc4f53480fe83362b1d01372acaa687020a6a`.
The image and validator metadata are retained under the workspace cache above.
Sticky has also built successfully and passed image validation at 5,703,616
bytes, leaving 849,984 bytes of OTA partition headroom. Its SHA-256 is
`d16c73eebe641d8466122d3f483ec480873b14e8e201ecce7c31678468adedc8`.
X4c has built successfully and passed image validation at 6,465,344 bytes,
leaving 88,256 bytes of OTA partition headroom. Its SHA-256 is
`6ebb2b866ef1a155c06951426013a48149cef2c0b1e0088c59e8e496a37a0a5c`.
Papermono also built successfully and passed image validation at 5,816,752
bytes, leaving 736,848 bytes of OTA partition headroom. Its SHA-256 is
`77f1b2483fa85637038b38adf3606a5a740142fe5f483b240ccacc396f177dd8`.
All five affected targets now have passing builds and validated images for the
shared-font change. The fresh host test
rebuild completed successfully, and all 1,921 CTest cases pass in 13.30 seconds,
including the shared-font generator test. Configure, build and test logs are
retained in `.cache/companion-verification/shared-fonts/`. These build results do not replace the
remaining physical rendering and companion acceptance checks.

Publication-stage recovery at checkpoint `62aea35c` accepts a short
`.prepared.tmp` or `.published.tmp` only when every retained byte matches the
expected publication-record prefix and the canonical phase evidence permits
recovery. Native preparation verification must succeed before removal. The
stage is read and compared again after verification, since verification borrows
the same scratch workspace. Changed bytes, permission loss, read/stat errors,
and replacement with a complete or oversized record prevent removal. Truncated
canonical records and mismatched stages remain preserved and refuse publication.
The HAL restricts removal to the selected transaction's short staging file,
checks its namespace and size, and closes readers before calling `HalStorage`.
Prefix comparison reuses the existing scratch loan without another allocation.

All 1,925 host tests pass, including exhaustive matching stage lengths with
successful and refused verification, canonical truncation, mismatched prefixes,
removal power cuts, changed evidence during verification, and native HAL
write/close/rename recovery. An ESP32-C3 production-header compile probe with
the 256-byte frame limit passes. The final five-target firmware build is pending;
logs are retained in `.cache/companion-verification/baseline-torn-stage-firmware.log`.
Torn consent-stage recovery, authoritative replay correspondence with learner
caches and saved sessions, live baseline commands and Apple UI remain unfinished.
On hardware, interrupt staging writes and removal at both publication phases,
reboot, and verify that recovery finishes before reading resumes, the original
learner files remain intact, and repeated commits do not change the result.
Monitor free/largest heap and task stack watermarks throughout recovery; the
host fault tests and compile probe do not establish physical acceptance.

The subsequent native installer test exercises both torn phases through the real
consent store, archive session and publication store, rather than modeled native
verification hooks. Valid persisted consent permits recovery and repeated retry;
damaged consent, missing/corrupt immutable backups, or changed current learner
items preserve all evidence and refuse recovery. All 145 HAL course
transfer tests pass after this addition. The default firmware build has completed
and its image passes board/chip validation at 6,463,120 bytes, leaving 90,480 bytes
of OTA headroom. SHA-256:
`50af54aca3e7f02bbc050711bd2a312982b870c6dc659f26bc14c5d94f33a46a`.
The other four target builds remain pending in the running batch. This test does
not establish the remaining authoritative learner-state replay correspondence.

Consent is persisted before the parent transfer is begun, so a power cut can
leave a consent stage without a parent transfer journal. Startup currently
selects baseline recovery from the parent transfer. Handling an orphan consent
stage therefore requires a separate recovery path; reconstructing a truncated
record must not itself grant approval or learner-state reuse. Complete consent
stages can be retried through explicit approval. Truncated stages now have an
opt-in retry through `HalCourseBaselineImportConsentStore::approve`: native
preparation first revalidates the frozen review and backups, then persistence
compares the stage with the exact newly approved request prefix. Canonical
consent must be absent before removal. Ordinary `load()` does not repair or
grant consent, and ordinary core persistence keeps recovery disabled by default.
Mismatched/full corrupt stages, storage errors, permission loss and conflicting
canonical evidence remain preserved. Removal is restricted to the selected
transaction's short consent stage and uses HAL namespace/size checks.

All 13 core consent tests and 145 HAL course-transfer tests pass after this
change. Coverage includes every truncated request length, mismatched bytes,
removal power cuts, permission loss during reads, read/stat errors, insufficient
scratch and canonical consent appearing before removal. The production consent
path compiles for ESP32-C3 with the 256-byte frame limit; reported frames are
64 bytes for prefix removal and 48 bytes for persistence. The existing scratch
loan supplies both comparison buffers; no heap allocation was added. These
checks do not establish orphan-stage boot recovery or physical power-cut
acceptance. Final firmware builds after this consent change are still required.

The native approval retry test additionally seeds a nonempty matching consent
prefix after preserving review backups. `load()` leaves it untouched and reports
missing canonical consent. Explicit approval recovers it only while the reviewed
learner items still match; changed learner items refuse approval and preserve all
files. The final host suite passes all 1,931 tests after this addition, including
146 HAL course-transfer tests. Its log is retained at
`.cache/companion-verification/baseline-native-consent-final-ctest.log`.

`CourseBaselineOrphanConsent` supplies the portable rollback component for
consent stages without a parent transfer. Its caller must prove parent absence
from native journal inspection and exclude writers. It refuses any canonical
consent or Prepared/Published evidence, then moves
the bounded consent stage to `.consent.orphan`. This inactive file preserves
the original bytes and grants no approval. Retry handles a lost rename
acknowledgement without deleting evidence. The component keeps its path buffers
in an owner retained off stack and adds no allocation. Four host tests pass,
covering every stage length, malformed retained bytes, protected phase evidence,
destination exhaustion, permission/stat failures, invalid identities, oversized
stages and both rename failure boundaries.

Native `HalCourseBaselineOrphanConsentRecovery` now scans the companion directory
to checked end, closes scan handles before mutation, inspects the parent transfer
journals and freezes their state, then performs rollback through a restricted HAL
adapter. Matching parent transactions, malformed/case-variant consent names,
alias/lookup errors, canonical approval and publication evidence refuse recovery.
The adapter only renames the selected bounded stage to its inactive orphan path;
it cannot read, write, resize or delete learner files. After each rollback the
directory is scanned again, avoiding an iterator retained across mutation.

Boot checks for orphan consent stages before its no-work early return. Both boot
and Connect run native rollback before baseline session attachment and normal
transfer recovery. The temporary owner is allocated with `makeUniqueNoThrow`,
checked against the 50 KiB reserve and largest-block requirement, and released
before the larger baseline session is allocated. Its path/name buffers and HAL
handles exceed the stack budget, so stack storage was rejected; it borrows the
existing transfer workspace for parent inspection rather than allocating another.
All 149 HAL course-transfer tests pass, including multiple orphan stages, parent
and canonical/phase conflicts, corrupt journals, name/alias failures, permission
loss and both rename power-cut boundaries. A C3 production-header probe passes
the 256-byte frame limit: scan 112 bytes, portable rollback 128 bytes and native
recovery 64 bytes. Final firmware builds and physical acceptance after this native
integration remain pending. Repeated failed approval attempts now choose the
first free `.consent.orphan-00` through `.consent.orphan-ff` destination after
the base orphan path is occupied. The search is bounded and uses the existing
path buffers without allocation. Earlier copies are never overwritten. Exhaustion,
uncertain lookup and oversized diagnostic files preserve the new stage and
refuse rollback. The HAL accepts only the exact selected transaction and
lowercase two-digit slot suffixes. Native tests cover recovery of a second
attempt while preserving the first; core tests cover multiple retained attempts
and full destination exhaustion.

All 1,938 host tests pass after numbered orphan handling. Final firmware builds
run one target at a time, retaining and validating each image before starting
the next target, since framework reconfiguration can remove earlier build
directories. Logs, task source fingerprints, images and validator metadata are
retained under `.cache/companion-verification/baseline-orphan-final/`. Physical
power-cut/heap acceptance and the remaining full companion plan are still pending.

The final native-orphan default image builds and passes the board/chip validator
at 6,467,680 bytes, leaving 85,920 bytes of OTA headroom. SHA-256:
`5d1a1400870a4f6740899798d6f66886bfe01d2d96db14a8318c4fe3dba795a1`.
Its image and metadata have been retained before starting Sticky. All five
targets have now completed successfully and passed image validation. Sticky is
5,703,616 bytes (849,984 bytes of OTA headroom), X4 Pro is 6,499,616 bytes
(53,984 bytes), X4c is 6,470,384 bytes (83,216 bytes), and Papermono is
5,816,752 bytes (736,848 bytes). Each actual image, SHA-256 and board/chip
validator result is retained in the final verification directory above.
The changed boot recovery file also passes a C3 compile with the 256-byte
frame limit; `recoverAtStartup()` reports a 144-byte frame. These build and
stack checks do not establish physical heap or power-cut acceptance.

Saved-session replay correspondence must use the derived receipt digest, not
the journal frontier alone. `HalTintaLearnerPreparation::run` verifies native
derived preparation, matches the receipt to course/storage/pack/frontier, and
hashes that receipt before exposing `sessionSnapshot()`. `TintaActivity` binds
that digest to the app. Baseline session inspection currently checks saved-file
syntax and item coverage but does not establish this receipt-backed proof.
Legacy learner state also cannot be assumed to come entirely from a partial
canonical journal; the legacy migration draft separately confirms scheduler
configuration and ambiguous history/reading correspondence. Full baseline
replay verification and live commands remain unfinished.

The reviewed-journal audit now supports native replay from the immutable backup
copies. `CourseBaselineJournalSnapshot::reopen` rechecks the captured paths,
presence, lengths and full hashes without requiring the borrowed review bytes
again. A failed or absent initial capture cannot be reopened as authority.
The native owner consumes one successful audit for replay, reopens before visiting
records and rechecks source hashes after callbacks before reporting success.
Callback failure, permission loss and changed source bytes withhold success;
neither audit nor replay repairs immutable journal headers or tails.

All 149 HAL course-transfer tests pass, including reopening after audit cleanup,
changed backup bytes, missing subjects, replay before/after an audit, source
changes between audit and replay, callback failure, permission loss during the
callback and source mutation detected by the final proof. A C3 production-header
probe passes the 256-byte frame limit: snapshot verification 64 bytes, reopen
32 bytes and native audited replay 160 bytes. The reviewed owner occupies
5,352 bytes and the existing replay workspace occupies 640 bytes on this target.
That workspace is heap-admitted against the 50 KiB reserve and largest block,
allocated once for replay and released through RAII; its ordering state and
retained SD handles exceed the local stack budget. Captured descriptors and the
borrowed scratch loan are reused; no second 8 KiB buffer was added. All 1,938 host
tests pass; final firmware and physical-device checks after this change remain pending. This replay visitor
path is a prerequisite; it does not yet compare learner caches or establish the
saved-session receipt digest.

The reviewed owner also retains the native audited frontier in a 32-byte member;
the C3 production-header probe measures the resulting owner at 5,384 bytes and
passes the 256-byte frame limit.
`journalFrontier()` exposes it only after replay and the final immutable-source
checks succeed, while permission, storage and heap admission remain valid. A new
audit or replay attempt invalidates that result. The 149 HAL course-transfer tests
pass with a comparison against an independently audited frontier and assertions
that unaudited, failed and repeated replay attempts expose no frontier. This
frontier is journal evidence, not the saved-session receipt hash; full learner
cache correspondence and final firmware verification remain pending. After the
permission-loss and failed-subsequent-audit assertions were added, the complete
host rebuild and all 1,938 tests pass (15.89 seconds).

`HalCourseBaselineReplaySession` now projects frozen reviewed journal records
through the portable scheduling reducer into the disposable course-local
`replay-work` file. The session withholds its working store and frontier unless
native membership audit, ordered replay, immutable-source revalidation and course
binding all succeed. Permission loss or a new failed run invalidates both outputs.
Retained journal backups and learner files are not rewritten. This helper is not
yet wired into baseline learner-cache correspondence or receipt verification;
its fresh-item defaults cannot establish equivalence with a partial legacy history.

The C3 production-header probe passes the 256-byte frame limit and measures the
projection entry frame at 112 bytes, the projection owner at 264 bytes and the
portable reducer at 232 bytes. The owner is admitted and allocated off stack;
each projection allocates one checked audit owner and one checked reducer, then
reuses the audit's existing 640-byte replay workspace. Reducer and replay workspace
requirements are admitted together against the 50 KiB reserve and largest block.
The scratch loan is borrowed; no additional 8 KiB allocation is introduced.
Physical retained-handle, pool and heap-watermark acceptance remains pending.
The final host rebuild and all 1,938 tests pass (9.55 seconds), including projected
star state, independent frontier agreement, missing subjects, permission loss,
failed-run invalidation, overlapping-loan refusal and retained source-file
preservation. The five-target firmware batch has validated and retained `default`
and `sticky`; the other three targets remain in progress. These builds cover the
current native audit integration; the projection helper still has no live caller.

`compareTintaReplayItems` provides a read-only item component of that proof. It
validates the retained catalog, compares each logical cached state with replay,
then checks that every non-fresh projected item is represented. Missing fresh
states are equivalent to the native fresh-item default. Retired or duplicate
cached UIDs refuse correspondence. The comparison also checks current-day header
new/review counters with the native 16-bit saturation rule; pending cached records
are read logically without repairing their file. This does not prove legacy
provenance, undo-header correspondence, all day logs or saved-session receipts.

No heap allocation is added by the comparison. A coverage bitmap occupies the
existing loan after the 160-byte header/read area (at most 4,096 more bytes for
32,767 catalog items). Separate non-inlined walks keep item/iterator locals out
of the view-owning frame: the C3 stack report measures the entry at 224 bytes and
the walks at 96/112 bytes, with no compiled probe frame exceeding 256 bytes.
The full host rebuild and all 1,938 tests pass (9.95 seconds). Checks include
matching states, changed flags, a missing projected item, header-counter mismatch,
logical pending-record recovery without a write, short workspace and cancellation.
The interrupted firmware batch was resumed after its process and temporary
toolchain disappeared; `x4pro` is now validated and retained alongside `default`
and `sticky`. `x4c` and `papermono` remain pending.

The native audit checkpoint has now built all five targets successfully. Their
validated retained images are under
`.cache/companion-verification/baseline-replay-final/`, with lengths/headroom:
`default` 6,467,744/85,856 bytes; `sticky` 5,703,616/849,984 bytes;
`x4pro` 6,500,768/52,832 bytes; `x4c` 6,471,552/82,048 bytes;
`papermono` 5,817,904/735,696 bytes. Each retained image was revalidated against
its board/chip, checksum, hash and OTA partition, and task-owned native source
fingerprints remained unchanged. These firmware builds exercise the current
native audit integration; the new projection/comparison helpers still require
live baseline integration and have been checked with C3 production-header probes.

`compareTintaReplayDays` now checks unordered native day-log records against
canonical replay totals in both directions. It sums split per-day records with
checked 32-bit arithmetic, verifies every record CRC, rounds projected day-level
milliseconds using the canonical export rule, and refuses missing or extra
nonzero days. The existing 8 KiB scratch loan holds the entire 65,536-day coverage
bitmap; record buffers and sums stay small, and no heap allocation is added.
The native entry/aggregation frames measure 128/80 bytes; no compiled C3 probe
frame exceeds 256 bytes. Legacy per-session rounding differences remain evidence
to resolve, not permission to replace learner files.

After the final edits, the full host rebuild and all 1,941 tests pass (9.96
seconds), including unordered/split records, minimum/maximum day identities,
both coverage directions, zero-day equivalence, rounding boundaries, corrupt
CRCs, read failure, short scratch, mid-scan cancellation and counter overflow.
All source-byte preservation assertions pass. Full baseline cache/receipt proof,
live commands, Apple baseline UI and physical resource acceptance remain pending.

Read-only completion correspondence now covers the native profile and legacy
mark logs. `compareTintaReplayLessons` uses the portable native lesson projection
against the confirmed original pack: current progress must agree, existing higher
unlock choices remain local, and unknown enabled lesson identities refuse proof.
The original profile, including preferences, is never saved or changed.
`compareTintaReplayMarks` reconstructs native add/remove order, validates stars
against the original item catalog, resolves reading title keys only when unique,
and compares the resulting enabled set with replay in both directions. Corrupt or
retired marks and ambiguous readings remain preserved and unproved.

Neither helper adds an allocation or a second scratch loan. C3 production-header
probes measure lesson comparison at 96 bytes (lookup callback 48 bytes) and mark
comparison at 208 bytes. The final full host rebuild and all 1,943 tests pass
(10.21 seconds), including original-pack lesson indices, preserved unlock and
preference choices, unknown completions, enabled/disabled transitions, star and
resolved reading add/remove logs, missing marks, permission refusal and corrupt
CRC preservation. The helpers still need integration into the native baseline
inspector before live baseline commands can use their results.

The capture's checked course-directory scan already includes stable canonical
files such as `sync-receipt`, `lessons.bin` and `readings.bin` in its immutable
reviewed cohort. The existing inspector currently has no handlers for these
files. Receipt verification must read their retained backups, bind the receipt to
the confirmed course/storage/pack and verified journal frontier, and establish
the saved-session receipt digest before authorizing that session. It must not
substitute the frontier digest for the receipt hash or infer authority from an
unknown learner filename.

Baseline proof work now uses private companion paths (`bp-<course>-w` for replay
and separate item/review/lesson/reading/day proof suffixes). Ordinary course replay
and existing candidate/proof exports retain their original paths. This separation
keeps newly generated baseline files out of the frozen learner-directory scan;
the native test asserts that no new course-state file appears and all retained
source bytes remain unchanged.

`HalCourseBaselineReplayReceipt` copies a hash-verified retained manifest before
exports reuse the shared loan, binds it to course/storage/pack and the audited
frontier, reproduces all five canonical derived-file hashes in the private proof
namespace, and exposes the receipt SHA-256 as `sessionSnapshot()` only on success.
A CRC-valid receipt with an incorrect file hash is refused even if its own full
hash matches the reviewed descriptor. This is receipt reproducibility evidence;
matching actual retained learner files and integrating native saved-session
handling remain required in the baseline inspector.

The C3 header probe measures the receipt owner at 1,464 bytes and its entry frame
at 144 bytes, with no compiled frame above 256 bytes. Its 332-byte manifest copy
cannot remain in the overwritten loan or fit the local-variable budget; retaining
it in the checked temporary heap owner avoids a permanently resident static
buffer. The owner and largest block are admitted against the 50 KiB reserve, and
RAII releases all owned export handles and buffers. The projection owner is now
268 bytes after adding its explicit store location; allocations use actual
`sizeof` admission. No second 8 KiB workspace is introduced.

The final host rebuild and all 1,943 tests pass (11.32 seconds), including private
namespace preservation, borrowed receipt/scratch overlap, wrong receipt hashes
and CRC-valid false file declarations. Firmware targets need final builds after
the private-path integration changes; the prior five-target checkpoint does not
cover these edits. Native baseline integration, legacy reconciliation, saved
session handling and physical acceptance remain unfinished.

Native baseline inspection now consumes the frozen receipt cohort. When a
reviewed `sync-receipt` exists, it replays the reviewed journal in the private
namespace, proves receipt reproducibility, and requires all five retained derived
files to match the receipt's lengths and hashes. It applies the read-only item,
day, profile-lesson and legacy star/reading correspondence checks, validates
canonical completion sets, and rechecks every backup hash before success. These
checks run through the existing native import compatibility callback, with all
temporary replay ownership released before parser/file cleanup.

A version 3 saved session cannot pass without a verified receipt. A syntactically
valid stale session remains retained for the native app's rebuild path;
`sessionRequiresRebuild()` reports the verified outcome only after successful
inspection. It uses the receipt digest and complete retained legacy review count,
not a journal-frontier digest or only the item header's committed count. Earlier
legacy histories still require the separate migration/reconciliation proof; this
canonical receipt path does not establish their distributed provenance.

The final host rebuild and all 1,944 tests pass (13.82 seconds). The native fixture
contains a real star event and covers matching and stale queue bindings, a
CRC-valid false receipt declaration, same-length valid learner headers that
disagree with the receipt, and a version 3 session lacking receipt evidence. All
retained source-byte assertions pass. C3 production-header probes measure the
import owner at 31,512 bytes and the inspector at 11,880 bytes, with entry frames
of 192 bytes for inspection and 144 bytes for receipt preparation; no compiled
probe frame exceeds 256 bytes. The 332-byte retained manifest and projection
handle are members of the already heap-admitted owner, preserving the single
scratch loan. These are object/frame measurements, not hardware heap acceptance.

The native receipt fixture also covers a missing retained day file, CRC-valid
receipts bound to a different course, pack, journal frontier or storage generation,
and a malformed saved queue with valid inner and outer checksums. Incorrect
bindings remain nonzero and structurally valid so these cases exercise native
cohort correspondence rather than only receipt syntax checks.

A subsequent C3 allocation audit measures the archive reader at 4,496 bytes,
reviewed journal audit at 5,384, retained projection at 268, reducer at 232 and
replay workspace at 640. During native replay the last four coexist: 6,524 bytes
above the import owner, compared with its current 5,384-byte temporary preflight.
The former entry preflight understated that branch by 1,140 bytes. It now admits
the import owner plus the largest complete temporary branch: historical archive
inspection, journal replay or receipt export. This adds no heap allocation; the
calculation uses compile-time object sizes and preserves the 50 KiB reserve.
Boundary coverage checks refusal when the replay peak leaves exactly 50 KiB and
admission with one additional byte, alongside largest-block and authorization
checks. On C3 the resulting payload budget is 38,036 bytes. Allocator overhead and
other concurrent allocations remain subject to the per-allocation guards and
physical heap acceptance.

The preceding five-target firmware batch is running under
`.cache/companion-verification/baseline-native-receipt-final/`. Live baseline
commands, Apple baseline UI, legacy reconciliation and physical acceptance remain
unfinished; the full companion plan is not complete.

That batch began before the import peak-preflight correction and does not prove
the final firmware source state. A subsequent final batch must include the import
session header in its source fingerprints.

The preceding batch subsequently completed all five targets. Retained images were
independently revalidated for board/chip, image integrity and OTA size: default
6,477,808 bytes, sticky 5,703,616, x4pro 6,509,920, x4c 6,478,816 and papermono
5,816,752. These are intermediate results because the preflight changed during
that batch. The final peak-admission batch has now started and fingerprints all
13 relevant source files, including the import session header.

After the preflight correction, the host rebuild and all 1,944 tests pass (10.86
seconds), including the replay reserve boundary and the expanded eleven-case
native receipt fixture. The C3 factory probe has a 192-byte frame and no compiled
frame above 256 bytes. Final firmware verification is running under
`.cache/companion-verification/baseline-peak-admission-final/`, serialized after
the preceding build batch. Hardware verification must measure free/largest
internal heap throughout replay and receipt export, retaining more than 50 KiB
free heap across repeated Connect & Sync sessions.

Frozen review paging now has a portable codec in
`CompanionCourseBaselineReviewPage.h`. The `TCBP` version 1 envelope has a
44-byte header: magic at 0, version/reserved bytes at 4/5, little-endian review
length at 6, byte offset at 8, slice length at 10 and whole-review SHA-256 at 12.
Review bytes follow at 44, then a four-byte CRC32 over the preceding page bytes.
Maximum review data per page is 976 bytes, keeping the envelope within the
1,024-byte control payload. The producer supplies a verified immutable review
and its full digest; the codec does not compute or authenticate that digest.
The receiver must require one matching digest/length across pages, reassemble
the complete review, verify its SHA-256 and validate its reader, generation and
course before presenting approval. Page receipt never grants consent.

The codec borrows caller buffers, rejects overlapping outputs, checks canonical
review syntax before encoding and preserves the output view on decode failure.
It adds no heap allocation or static RAM buffer. Tests cover exact reconstruction
with one-byte, 97-byte and maximum slices, all byte corruptions/truncations, and
CRC-valid malformed lengths, offsets and missing hashes. Live command handling
and Apple reassembly are still required; this codec alone does not enable them.

The C3 paging probe passes with an 80-byte encode frame and a 128-byte decode
frame; no compiled probe frame exceeds 256 bytes. The helper is not yet included
by a firmware translation unit, so the active five-board batch validates the
corrected import path while this separate probe validates the paging API.
The final host rebuild and all 1,945 tests pass (17.96 seconds).

Apple now decodes the same bounded page envelope and collects a single frozen
review with `CourseBaselineReviewAssembly`. It requires a matching whole-review
digest and length throughout collection, rejects skipped or partial-overlap
pages, and accepts byte-identical complete duplicates. Rejected pages preserve
accepted state. Completion decodes the canonical roster, verifies its actual
SHA-256, and checks the expected reader, storage generation and course before
returning a review. This is collection evidence, not import consent or proof of
the retained files' contents. Live command routing and approval UI remain
unfinished.

`protocol/fixtures/CourseBaselineReviewPage-v1.fixture` contains the first 97
bytes of the existing shared review with its actual full SHA-256. The C++ codec
round-trips it exactly and the Swift tests compare it with the Apple encoder
oracle. Tests also cover one-byte and maximum-slice collection, harmless
duplicates, out-of-order/changed/overlapping pages, whole-record hash mismatch
and incorrect reader/generation/course contexts. The final C++ rebuild and all
1,945 tests pass (11.77 seconds). All 533 Swift package tests pass (16.514 seconds)
with the restored, signature-verified Swift 6.0.3 toolchain, including the four new
paging/assembly tests. Apple-native builds and physical acceptance remain required.

`readHalCourseBaselineReviewPage` now connects the portable codec to a borrowed
`HalCourseBaselineReviewStore`. Each page reopens the persisted canonical roster,
verifies its complete SHA-256 and expected reader/generation/course, closes all
store readers, then encodes from the verified bytes. Permission and heap guards
run before and after I/O/encoding. The caller must prove authentication and an
exclusive full-workspace lease; page response bytes must be outside the 4,416-byte
review region. The response can occupy the tail of the same 8 KiB workspace after
the store's comparison scratch is released.

This helper adds no allocation: it borrows the existing admitted store and
workspace, and copies only the 32-byte digest onto the stack to keep its binding
stable when input aliases loaned scratch. The C3 probe passes with a 144-byte
frame and no compiled frame above 256 bytes. Native tests collect the exact
review in 97-byte pages and refuse permission loss, overlapping response loans,
foreign reader identity and corrupt persisted bytes without changing learner
files. It remains a read-only producer; live command dispatch, review capture,
approval UI and the workspace-lease consumer still require integration.

The final peak-admission firmware batch completed all five targets. Independent
validation of the retained images confirms board/chip, image checksum and OTA
size, and all 13 recorded import-path source fingerprints still match. Results:
default 6,477,808 bytes (75,792 headroom), sticky 5,703,616 (849,984), x4pro
6,509,920 (43,680), x4c 6,478,816 (74,784) and papermono 5,816,752 (736,848).
The new standalone page producer is not yet called by a firmware translation
unit; its C3 probe is separate evidence for that API. Physical resources and the
full companion acceptance gates remain unverified.
The final host rebuild and all 1,946 tests pass (10.91 seconds), including native
verified page production; all 533 Swift tests remain green for the Apple changes.
