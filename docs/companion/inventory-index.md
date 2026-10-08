# Inventory snapshot index

`CompanionInventoryIndex.h` supplies a checked, indexed catalog over an immutable storage provider. It supports all five manifest content kinds. It does not scan directories, normalize legacy dictionary bundles, populate an index or publish a snapshot; those integrations remain pending.

Version one stores a 48-byte header followed by 67-byte entries. Integers are little-endian. Header fields are:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | `CIDX` |
| 4 | 1 | version 1 |
| 5 | 3 | zero reserved bytes |
| 8 | 16 | SD generation |
| 24 | 8 | nonzero snapshot revision |
| 32 | 8 | entry count |
| 40 | 4 | IEEE CRC-32 of all encoded entries |
| 44 | 4 | IEEE CRC-32 of header bytes 0–43 |

Each entry contains the existing 63-byte ContentManifest and its four-byte IEEE CRC-32. Hashes must be strictly increasing; duplicates and unordered rows fail validation. The file size must equal `48 + count * 67` without overflow. Open checks the entire snapshot, record decoding, order and aggregate CRC before exposing a nonzero revision. Reads recheck the individual entry CRC; an I/O or decoding failure invalidates the catalog. CRC detects accidental corruption and torn writes; it does not authenticate SD data.

The provider lends one scratch span of at least 67 bytes. Catalog/codecs allocate no heap memory and no whole-catalog array. The provider must hold an immutable snapshot handle for the scan and report size/read failures. It must also yield as needed on firmware during long validation. The portable class does not own a HalFile or schedule FreeRTOS work.

`protocol/fixtures/InventoryIndex.json` encodes one shared course manifest. `CompanionInventoryIndexTest` covers fixture round trips and page handling, truncation, bad header/entry data, wrong card generation, duplicate hashes, all content kinds, overflow without output mutation, I/O failures and read corruption after open. All 75 Companion host tests pass; formatting and the host compiler's 256-byte stack-frame check pass. These are host results, not physical C3 RAM/stack measurements or proof of catalog completeness.

The five firmware targets passed after the earlier directory-error/HAL iterator integration in `/tmp/lila-companion-directory-build.log`. This new index header is not yet included by firmware. Next integration must build complete snapshots from actual assets, preserve original bundle identities, publish them recoverably, attach a HAL provider and invalidate revisions on content changes before enabling inventory dispatch.

`HalInventoryIndexStorage` now supplies the HAL provider. It keeps one read-only HalFile open per immutable snapshot, caches its length, performs bounded 64-bit seeks/reads through HAL, logs failures and keeps I/O failure sticky until reopen. Member handles close before reopen and at destruction. It introduces no file-data buffer or allocation per row: the existing opaque HalFile handle allocation occurs once on open, while IndexedInventoryCatalog borrows caller scratch. It yields after every 32 successful nonempty reads during long validation; no latency or watchdog guarantee is claimed without hardware measurements.

Host tests compile the actual provider against explicit HAL/FreeRTOS stubs. They verify handle reuse/release, yields, short reads and failed seeks remaining failed, unavailable/directory opens, and bounds failures without output mutation. All 78 Companion/provider host tests pass. Formatting and the provider's host 256-byte stack-frame check pass. This does not test real SD locking, SPI/SDMMC timing, allocation failure or physical memory.

One five-target firmware build after the provider's last source edit is running in `/tmp/lila-companion-inventory-provider-build.log`; results are pending. The provider is not yet owned by Connect & Sync or given a published snapshot. Complete snapshot population, legacy bundle identity preservation, recoverable publication, revision invalidation and activity dispatch remain incomplete. On hardware, verify read errors, repeated open/close/reopen and Connect & Sync exit heap/largest-block readings before claiming resource stability.

`CompanionInventoryIndexBuilder.h` constructs candidates using borrowed 67-byte scratch space and no heap allocation. Its source must return sorted manifests and distinguish complete enumeration from errors. Identical duplicate manifests are collapsed; conflicting duplicate hashes and unordered entries abort construction. Header and aggregate CRC publication happens only after complete enumeration. The sink must preserve the previous snapshot until successful `finish`, and discard candidates through `abort` on failures. This interface does not yet implement durable SD publication or populate the live inventory.

Verification: build `CompanionInventoryIndexBuilderTest` in the host CMake test project, then run `ctest --test-dir /tmp/lila-companion-tests -R 'Companion|InventoryHal' --output-on-failure`. The current run passes 81 tests. Actual SD power-cut recovery and reader integration remain required.

`HalInventoryIndexSink` implements candidate/active/backup publication entirely through HAL. It opens one reusable candidate handle, writes without per-entry synchronization, syncs and closes the complete candidate, and validates the full index before retaining the active file as a backup and publishing the candidate. On publication failure it attempts to restore the backup; recovery must succeed before a subsequent build. Recovery prefers a valid active snapshot, otherwise restores a valid backup, and never promotes an abandoned candidate. Backup cleanup occurs on the next recovery so a successful publication is not reported failed merely because later cleanup fails.

The controller must close existing catalog handles before publication and serialize all access to these paths. The candidate uses the existing fallible HAL handle allocation once per build; validation opens one additional reusable HAL handle rather than allocating per manifest. Scratch is borrowed from the controller. This adapter is not yet wired into the companion activity. Host fault-injection tests for the adapter and SD power-cut tests remain required; no atomic filesystem guarantee is inferred from a successful rename.

The sink adapter now has six host fault-injection tests compiling its actual C++ implementation against a file-backed HAL fake. They cover successful publication with deferred backup cleanup, failed write/sync, both failed publication renames, interrupted rename recovery, ignored abandoned candidates, corrupt candidate rejection, corrupt active-file rollback, failed immediate restoration followed by successful later recovery, and refusal to overwrite files when neither snapshot validates. All 88 companion/inventory HAL host tests pass. A host compiler frame-size check also passes the 256-byte threshold; this is not a measurement of the target stack or actual SD power-loss behavior. The five-target firmware build is still running.

Additional sink tests verify that metadata inspection errors on either active or backup paths prevent recovery mutations, and that failure to close a synced candidate prevents publication. All 90 companion/inventory HAL host tests pass. Read-time failure versus content corruption is still conflated by the sink's boolean validation helper and needs distinct handling before live catalog integration.

Validation now distinguishes corrupt snapshot data from failed reads or opens. The HAL provider exposes its sticky read-error status; recovery refuses mutation on an active or backup read failure rather than treating it as grounds for rollback. A fault-injection test verifies both paths preserve all snapshot files and that retry succeeds once reads recover. All 91 companion/inventory HAL host tests pass. Target builds must be rerun after the earlier live build finishes because these firmware edits occurred during that run; hardware I/O/power-cut verification remains pending.

Publication now rejects candidates whose revision is not greater than the currently recovered active snapshot, before either rename. `nextRevision` recovers first, derives one greater than the active snapshot (one for an empty cache), and rejects `UINT64_MAX` without changing its output. Host tests cover reused/older revision rejection, backup-derived revision selection and overflow; all 93 companion/inventory HAL tests pass. This advances revisions relative to the recovered cache only. A durable reservation independent of rollback remains necessary to prevent revision reuse after an older backup is restored; live integration must not treat this helper alone as a globally monotonic allocator.

`nextRevision` now reserves through `HalInventoryRevisions`, an eight-byte little-endian NVS blob in the existing companion namespace, independently of SD snapshots. It reserves above both the persisted counter and recovered snapshot revision, verifies write/readback, and never resets malformed or zero counters. Failed writes leave caller output unchanged; uncertain successful writes can consume a revision without publishing, which is safe. This avoids reuse when SD rollback restores an older snapshot, while NVS remains intact. The controller must serialize calls and use reserved revisions for every build; the sink's low-level write API cannot establish reservation provenance. NVS erase/device reprovisioning still requires identity-generation invalidation before existing clients resume. All 95 companion/inventory HAL host tests pass, including rollback and failed-write reservation tests. Target and physical verification remain pending.

The sink now enforces reservation provenance: `nextRevision` durably reserves and remembers a value, `begin` rejects a missing reservation, and `finish` rejects a candidate carrying another revision. Success or abort consumes the sink's reservation. A readback-failure test demonstrates that an uncertain stored reservation is skipped on retry and never returned as a successful value. All 97 companion/inventory HAL host tests pass, including missing/unreserved publication and NVS-open failure checks. This supersedes the earlier note that the low-level sink API could bypass reservation. Native target rebuilds and hardware recovery validation remain pending.

`buildAndPublish` combines recovery, durable revision reservation, allocation-free snapshot construction and validated HAL publication. Caller output changes only after publication succeeds. The builder uses the sink's borrowed scratch buffer; its bounded local object passes a host 256-byte frame check, without introducing a new heap buffer. Integration tests build/read all five manifest kinds through actual builder/sink/provider code and verify that failed enumeration preserves the prior snapshot while consuming its reservation. All 99 companion/inventory HAL host tests pass. This is a HAL-fake integration test, not actual asset enumeration or hardware acceptance. Live source population and activity dispatch remain pending.

## Bounded external sorting

`InventorySorter` adapts an `UnsortedInventorySource` into the sorted source
required by the index builder. It heapsorts encoded metadata chunks in borrowed
scratch, writes them to a disposable run, and merges runs through two SD files.
Merge input and output use three bounded banks and bulk reads/writes. Records
remain encoded bytes; there are no wider-pointer casts or metadata heap arrays.
Sorting compares content hashes and preserves every manifest field.

The sorter itself makes no heap allocations. Its scratch remains borrowed until
consumption completes and must be disjoint from the scanner's hash buffer. Use
separate spans within the session workspace, preserving radio/command areas if
sorting during an active connection. At least 201 bytes (three 67-byte entries)
are required. Memory use is proportional to that fixed workspace; metadata sort
work is O(n log n), while the scanner can hash each actual content file once.
No device timing or memory-watermark improvement has been measured.

Input Error, failed storage operations and corrupt run records leave the sorter
unready; `next` returns Error, never an incomplete End. Storage runs are disposable
and reset on a new build, while the authoritative published index remains owned
by the existing sink. Identical duplicates reach the index builder for deduplication;
conflicting manifests for the same hash make publication fail and retain the
previous snapshot. Read corruption after sorting also invalidates consumption.

All 156 companion/related host tests pass. Sorter coverage includes empty and
multi-pass catalogs through 1,001 records, odd scratch sizes, all manifest fields,
every injected storage operation failure, scan/read corruption and combined
index publication. Host GCC `-Os -fstack-usage` reports 208 bytes for merge,
160 bytes for build and 144 bytes for bank validation. These are host frames,
not physical ESP task watermarks.

The new sorter is currently compiled by host tests only. HAL temporary-run I/O,
complete filesystem/content enumeration and live inventory dispatch still need
integration. Existing dictionaries need an explicit bundle identity/export
contract; a partial catalog must not be labeled complete merely to unblock app
transfers. Native SDK builds and hardware acceptance remain pending.

## HAL temporary-run storage

`HalInventorySortStorage` now supplies the sorter's two disposable runs at
`/.crosspoint/companion/inventory-sort-a` and `inventory-sort-b`. It retains two
`HalFile` members opened for read/write and reuses them through all batches,
merge passes and repeated builds. The existing HAL allocates its handle objects;
retaining two handles avoids allocating a new handle per batch. Raw SdFat handles
were rejected because they bypass `storageMutex`. No payload or catalog buffers
are allocated by this adapter.

Reset truncates and syncs both runs. Writes must be contiguous, and restarting a
completed output run must begin at offset zero. Finish checks the intended
length, truncates stale tail bytes, syncs and verifies the size before allowing
reads. Reads check bounds and reject short reads. Any error invalidates the
adapter until reset; it cannot expose a failed run as completed. Member handles
are explicitly closed at the owner's release point and by the destructor, with
close errors logged. The previous authoritative index is untouched by these
private temporary files; a new session resets abandoned runs.

All 164 companion/related host tests pass. Eight adapter cases exercise multiple
merge passes using exactly two open operations in the simulated backend, repeated
reset/reuse, stale output tails, bounds/overflow, open/truncate/write/sync/close
failures and corrupted consumption through a short read. Run:

```sh
cmake --build /tmp/lila-companion-tests --target HalInventorySortStorageTest
ctest --test-dir /tmp/lila-companion-tests -R HalInventorySortStorage --output-on-failure
```

The affected firmware build passed default, sticky, x4pro, x4c and papermono. Live filesystem enumeration
and inventory dispatch remain unimplemented; this adapter does not make the
catalog complete. Physical SD timing, power-cut behavior, heap/stack watermarks
and native app end-to-end inventory acceptance are still required.


The new `HalInventoryDirectoryWalker` supplies bounded depth-first enumeration
through `HalFile::nextEntry`, keeping an entry open for the caller to hash without
reopening its path. Paths are borrowed and null terminated. Directory descent is
delayed until the next call so the future inventory source can explicitly prune
private metadata directories. End is reported only after all included directories
finish successfully; failed reads, names, closes, allocations, depth or path
limits remain errors until a new scan starts.

The session-owned object contains a 512-byte path, 256-byte name, 32 prefix
lengths and 33 HAL handles. It must not be a task-local stack object. HAL entry
implementations are allocated lazily once per visited depth and reused across
siblings; there is no per-entry application allocation. The root is opened once
per scan. The depth limit includes the root; exceeding 32 directory levels or a
511-byte path is an explicit incomplete-scan error, not silent pruning. The
256-byte name capacity likewise rejects names that cannot be represented.

Seven walker host cases cover nested traversal, prepared-handle reuse, explicit
pruning, normalized roots, read/close/allocation/name failures and path/depth
limits. The latest selected companion/inventory/course/walker run passed 151
tests. Reproduce the walker checks with:

```sh
cmake --build /tmp/lila-companion-tests --target HalInventoryDirectoryWalkerTest
ctest --test-dir /tmp/lila-companion-tests -R DirectoryWalker --output-on-failure
```

The walker is not yet consumed by a live inventory source. File-kind discovery,
manifest construction, device-local path mapping and inventory command dispatch
remain required. This header has host coverage but has not yet been compiled in a
firmware caller. Before enabling live scans, verify real SD traversal errors,
watchdog yields, free/largest heap and task stack watermarks with
`python3 scripts/debugging_monitor.py`, including deeply nested and long UTF-8
filenames. The completed five-target firmware build covers the earlier HAL sort
adapter, not this unused walker.

Host GCC `-Os -fstack-usage` reports walker frames of 112 bytes for begin,
96 bytes for next and 48 bytes for close against the simulated HAL. These are
not embedded high-water measurements. Strict `-Wall -Wextra -Werror` compilation
and `git diff --check` also passed.


`hashInventoryFile` now hashes the walker's borrowed regular-file handle using
caller-provided scratch and publishes the length/digest only after successful
SHA-256 completion and a final size check. It seeks to zero, leaves the file at
EOF on success, rejects missing handles/directories/empty or oversized scratch,
and yields after each chunk. Exact reads are required; a short read is an error.
The final size check detects size changes but does not claim an atomic snapshot
against same-length external modifications. Connect mode must continue to
exclude concurrent content mutation while a snapshot is built.

Transfer verification now uses this same helper, retaining its expected-length
and expected-digest checks. No new application heap allocation, buffer or file
handle is introduced; the SHA context and digest replace the previous verifier's
same objects. Host GCC `-Os` reports a 144-byte helper frame against the simulated
HAL/OpenSSL backend, not the embedded mbedTLS implementation.

Four new tests cover a multi-chunk file from a nonzero starting position, empty
file SHA-256, failed/short reads preserving outputs, and invalid input. The final
selected companion/inventory/course/walker/hash host run passed all 155 tests;
strict compilation and whitespace checks passed. To reproduce the focused check:

```sh
cmake --build /tmp/lila-companion-tests --target HalInventoryFileHashTest
ctest --test-dir /tmp/lila-companion-tests -R InventoryHash --output-on-failure
```

The five-target firmware build for the shared hashing refactor is running.
Live inventory classification, manifest/path persistence and dispatch remain
unfinished. On hardware, compare discovered hashes against desktop SHA-256,
exercise SD read failures, and monitor heap/stack plus watchdog behavior during
large-file scans with `python3 scripts/debugging_monitor.py`.


`HalInventoryFileSource` now joins directory walking, metadata resolution,
borrowed-file hashing and path staging into `UnsortedInventorySource`. It emits
an entry only after all those operations succeed. The resolver explicitly decides
whether to include or prune a directory, skip unrelated files, include a validated
file, or fail on unsupported/corrupt transferable content. It cannot supply a
trusted hash or length: those are overwritten by reading the open file. The
source yields for each resolved entry, including skipped files/directories, and
hashing yields for each chunk. Any failure makes subsequent calls Error until a
new scan starts; output manifests remain unchanged on failure.

`InventoryPathSink` is a staging contract. The caller still needs a durable
implementation and must publish its staged map together with the matching
complete index, or discard both on failure. This source does not itself make
staged paths authoritative. All duplicate paths are recorded before the index
builder deduplicates identical manifests, allowing the eventual path-map owner to
choose an existing location for a renamed identical file.

The source holds borrowed references and a scratch span and performs no new
application allocation. The session owns the larger walker. The hash span must
be disjoint from sorting scratch; the integration test splits one 8192-byte
workspace into 1024 hash bytes and 7168 sort/index bytes. Do not alias live radio
request/response storage while scanning.

Four source host cases cover actual SHA-256 and metadata/path propagation,
explicit private-directory pruning, traversal/metadata/hash/path/kind failures,
restart, and the complete source-sort-index-catalog pipeline. That pipeline
publishes two entries for three file paths (one renamed duplicate) and preserves
the previous snapshot after a later read failure. All 159 selected tests pass;
strict host compilation and whitespace checks pass. Run:

```sh
cmake --build /tmp/lila-companion-tests --target HalInventoryFileSourceTest
ctest --test-dir /tmp/lila-companion-tests -R InventoryFileSource --output-on-failure
```

The resolver in these tests is simulated, not a shipped content classifier.
Real EPUB/course/font validation, dictionary bundle representation, durable path
mapping and live inventory dispatch remain unfinished. The source is a header
not yet included by a firmware caller; the running firmware build validates the
shared hash helper/refactored transfer verifier, not this new source. Hardware
acceptance must scan supported content, compare hashes against desktop tooling,
inject SD failures and check watchdog/heap/stack behavior via
`python3 scripts/debugging_monitor.py` before enabling inventory capabilities.


`CompanionInventoryPaths.h` defines the private CPTH-v1 map format and a
bounded reader. Its 48-byte header contains storage generation, inventory
revision, path-record count, entry CRC and header CRC. Each P-v1 record contains
one complete ContentManifest, a two-byte path length, the path bytes and a CRC.
Paths are absolute, at most 511 bytes, without empty/dot/parent components,
controls, NUL, backslash, colon or trailing slash. The maximum record is 582 bytes;
this is caller-provided scratch, not a new buffer allocation. Multiple paths can
refer to identical content.

The reader validates the complete map before opening and requires exactly the
expected generation and revision from the associated inventory snapshot. Lookup
compares the full manifest, copies a null-terminated path into caller storage,
and preserves output on Missing/Error. Read failures invalidate the reader until
reopened. CRCs detect corruption; they do not authenticate removable SD data.
The owner must keep the underlying snapshot immutable while the reader is open.

Four path-map tests cover a maximum-length UTF-8 path, invalid paths, every
record truncation and byte corruption, trailing data, duplicate locations,
manifest/size mismatch, generation/revision mismatch, full snapshot corruption
and later read failures. All 163 selected companion/related host tests pass.
Strict compilation and whitespace checks pass; host `-Os` frames are 192 bytes
for open and 208 bytes for lookup (not embedded watermarks). Run:

```sh
cmake --build /tmp/lila-companion-tests --target CompanionInventoryPathsTest
ctest --test-dir /tmp/lila-companion-tests -R CompanionInventoryPaths --output-on-failure
```

This is the map format/reader, not durable staging or coordinated publication.
Those remain necessary, alongside content classification and live dispatch.
A matching revision is required for lookup but does not itself make two-file
publication atomic; interrupted publication must recover a coherent map/index
pair before inventory is served. The header is not yet used by firmware. The
existing five-target firmware build continues for the shared file hash helper.
On hardware, verify renamed duplicates resolve to an existing file, power cuts
cannot expose mismatched revisions, and heap/stack stay bounded across repeated
inventory scans using `python3 scripts/debugging_monitor.py`.


`InventoryPathsBuilder` implements the scanner's path sink with streamed CPTH-v1
staging. It borrows a 582-byte scratch span, writes records directly to the stage,
keeps count/offset/CRC in fixed members and rewrites the header when sealed.
`InventoryPathsStage::seal` must truncate, sync and close a private candidate; it
must not publish it. Invalid input or a failed mutation aborts the candidate and
prevents further records/sealing until begin. Destruction aborts an incomplete
scan, while a sealed candidate remains owned by the coordinating caller. The
builder is non-copyable so candidate cleanup has one owner. No application heap
allocation or whole-map buffer is added.

The path-sink interface is now portable in `CompanionInventoryPathSink.h` rather
than coupled to the HAL source. Five builder tests cover empty/duplicate maps,
matching revision, every injected begin/write/seal failure, invalid paths,
restart, invalid arguments and destructor cleanup. A sixth integration case runs
actual source/hash/sort/index/path-building/readers with disjoint 1024-byte hash,
582-byte map and 6586-byte sorting spans in one 8192-byte workspace. It resolves
every resulting manifest to an existing simulated file. All 169 selected host
tests pass; strict compilation and whitespace checks pass. Host frames are
96 bytes for begin, 80 for record and 32 for seal; embedded watermarks remain
unverified. Run:

```sh
cmake --build /tmp/lila-companion-tests --target CompanionInventoryPathsBuilderTest HalInventoryFileSourceTest
ctest --test-dir /tmp/lila-companion-tests -R 'CompanionInventoryPathsBuilder|InventoryFileSource' --output-on-failure
```

The stage backend in these tests is simulated. A HAL-backed durable stage and
coherent map/index publication/recovery are still required, as are real content
classification and live dispatch. These unused headers do not add a firmware
runtime path. The ongoing firmware build still covers the shared hashing
refactor. Hardware verification must include candidate write/sync failures and
power cuts before/after pair publication, then verify matching generations and
revisions and valid content paths before serving inventory.


`HalInventoryPathsStage` now implements the private SD candidate backend at
`/.crosspoint/companion/inventory-paths-next`. It uses one member HalFile opened
once per scan, exact writes without holes, bounded extent tracking and periodic
yields. Seal requires the expected extent, truncate, sync, size verification and
explicit member-handle close. It then transfers candidate ownership to the
snapshot coordinator; destruction does not delete a sealed candidate. No
publication rename occurs here. Abort closes before removing, logs cleanup
failures, and keeps ownership when cleanup cannot finish so begin cannot bypass
an unresolved removal error. A future session owner must resolve abandoned sealed
candidates as part of coherent pair recovery.

Five host cases exercise the actual inline HAL adapter and actual map builder/
reader against simulated SD storage: 64 records with one writer open, sealed-file
retention and reopening, open/directory/write/truncate/sync/close failures,
invalid write/seal bounds, stale-candidate restart, destructor cleanup and failed
removal/close recovery. All 174 selected host tests pass; strict compilation and
whitespace checks pass. The one HAL handle uses the existing HAL allocation;
there is no per-record allocation or new map buffer. Run:

```sh
cmake --build /tmp/lila-companion-tests --target HalInventoryPathsStageTest
ctest --test-dir /tmp/lila-companion-tests -R PathStage --output-on-failure
```

The new adapter is an unused header and has not yet been compiled in a firmware
caller. Coherent index/map publication, restart recovery, real content resolution
and live inventory remain unfinished. The five-target hash-refactor build is
still running. Hardware checks must include SD write/sync/close faults and power
cuts after candidate sealing, preserving the previous authoritative pair until
coordinated publication succeeds. Monitor actual heap and task watermarks during
repeated scans with `python3 scripts/debugging_monitor.py`.


`InventoryPublication` now coordinates the private index/map candidates using
two CRC-checked IPU-v1 intent files. Each 32-byte intent names the storage
generation and nonzero inventory revision. Both writes are synced and read back
before the first rename; recovery accepts one valid copy if the other was torn.
Conflicting valid intents and a valid intent from another generation fail closed.
Before a new intent, the previous pair and both candidates must validate. New
revisions must increase; repeating a fully installed revision with no candidates
is a no-write success.

Installation independently resumes each member's old-to-backup and
candidate-to-active renames. A member already valid for the target revision is
left in place. The complete active pair is validated before backups, unused
candidates and finally intent copies are removed. A crash after any successful
mutation can therefore finish through recovery. A torn initial intent with no
valid copy is discarded only if the existing active state is coherent (or truly
empty without backups). Invalid/missing installed candidates or conflicting
backup state preserve artifacts and return Corrupt; generalized rollback of
corrupt candidates is not implemented yet.

The coordinator borrows TransferStorage, a validation interface and at least
32 scratch bytes; it adds no heap allocation. Its validator contract requires
whole-file checks and index-to-path correspondence, not just matching headers.
The current host publication tests use a simulated validator, so a concrete
validator using the actual index/map readers remains required. The existing
independent HalInventoryIndexSink must not publish snapshots in parallel with
this coordinator; the eventual session owner must route both sealed candidates
through coordinated publication and block inventory serving during recovery.

Five publication cases cover initial install/update, repeated commit, decreasing
revision, failures before and after every mutation, torn writes to either intent,
candidate mismatch, read failure, wrong generation and conflicting valid intents.
All 179 selected host tests pass; strict compilation and whitespace checks pass.
Host frames are 48 bytes for recover, 96 for publish/install and 144 for intent
reading, not embedded watermarks. Run:

```sh
cmake --build /tmp/lila-companion-tests --target CompanionInventoryPublicationTest
ctest --test-dir /tmp/lila-companion-tests -R CompanionInventoryPublication --output-on-failure
```

The shared hashing firmware build finished successfully for default, sticky,
x4pro, x4c and papermono. No firmware build is currently running. That build does
not cover the unused publication/source/stage headers. Concrete validation,
coordinated HAL integration, corrupt-candidate rollback, content classification
and live inventory dispatch remain unfinished. Hardware power-cut tests must
exercise every intent/rename/cleanup boundary, verify the index/map generation
and revision agree before serving inventory, and measure actual stack and heap
with `python3 scripts/debugging_monitor.py`.


`InventoryPairValidation` now validates the actual CIDX/CPTH formats and their
correspondence. Both complete CRC-checked readers open against the expected
storage generation and revision. Every mapped manifest must match a binary
search result in the sorted index, including length/kind/version/family, and every
index entry must have at least one map record. Duplicate locations and unsorted
map records are supported; missing, extra or conflicting entries are rejected
even when their record/header/whole-file CRCs are valid.

The session-owned validator borrows one 582-byte record bank shared sequentially
by both readers and uses the remaining supplied scratch as a coverage bitmap.
If the bitmap cannot cover the entire index, it repeats the map scan in windows.
This avoids per-entry heap state or an unbounded index-sized allocation. Each
map record uses binary index lookup; additional windows repeat these reads.
`InventoryPaths` now offers rewindable manifest iteration with failure
invalidation; no borrowed path view leaves that iterator.

Four validator tests cover unsorted duplicates, a one-byte bitmap forcing
multiple passes, empty maps, insufficient scratch, valid-CRC missing/extra/
conflicting manifests, every injected reader failure and record corruption.
Two publication tests now use real encoded index/map files and the actual pair
validator, exercising failures before and after every publication mutation and
rejecting valid-CRC correspondence errors before writing an intent. The full
selected run passed 185 tests, followed by all 11 affected validation/publication
tests after the final stack-bound edit. Strict compilation and whitespace checks
pass. Host `-Os` frames are 176 bytes for validate, 160 for binary lookup and
144 for manifest iteration; helper boundaries prevent those local frames from
being combined through inlining. These are not embedded stack watermarks.

```sh
cmake --build /tmp/lila-companion-tests --target CompanionInventoryPairValidationTest CompanionInventoryPublicationTest
ctest --test-dir /tmp/lila-companion-tests -R 'CompanionInventoryPairValidation|CompanionInventoryPublication' --output-on-failure
```

The publication integration tests still use simulated storage handles; a HAL
validator that opens/closes the real snapshot handles and propagates SD errors
remains required. These unused headers are not yet firmware callers. No build is
currently running. HAL integration, corrupt-candidate rollback, classification
and live dispatch remain unfinished. Hardware validation must include large
catalogs spanning bitmap windows, long/duplicate file paths, SD failures and
actual task/heap measurements via `python3 scripts/debugging_monitor.py`.


`HalInventoryPublicationValidator` now opens the actual index/map snapshot
handles and runs the complete format/correspondence validator. Standalone file
validation also checks complete records and the requested generation/revision.
Missing snapshots and invalid content return Invalid; open/read/close failures
return IoError. The optional actual revision is assigned only after successful
validation and successful closing of both handles. All nonvalid results are
logged. Both member handles are closed before a new validation and in reverse
order when finishing, so publication never treats an unsuccessfully closed
snapshot as eligible for rename.

`HalInventoryIndexStorage::close` now returns success; reopening stops when the
previous handle failed to close. The existing independent index sink also treats
close failure during candidate validation as I/O failure, preventing its rename.
The validator is session-owned fixed state with two HAL handles and borrowed
scratch, with no per-record allocations. The HAL allocates its existing handle
implementation once per open validation snapshot, not per record. The owner must
allocate the larger validator outside a task stack and release it at session
exit; live ownership wiring remains unfinished.

Four new HAL tests cover both formats, actual correspondence, requested/returned
revisions, missing/invalid paths, valid-CRC conflicts, open/read/close failures and
recovery after failure. The final selected host run passed 189 tests. Strict
compilation and whitespace checks pass; host frames are 112 bytes for index
validation, 208 for map validation and 64 for file/pair dispatch, not embedded
watermarks. Reproduce with:

```sh
cmake --build /tmp/lila-companion-tests --target HalInventoryPublicationValidatorTest
ctest --test-dir /tmp/lila-companion-tests -R HalPublicationValidation --output-on-failure
```

A final firmware build for these production changes is running for default,
sticky, x4pro, x4c and papermono. The earlier shared hashing build passed all five.
The validator has a real firmware compilation unit; the unused directory/source/
stage/publication owners remain unwired. Coordinated HAL publication,
corrupt-candidate rollback, content classification and live inventory dispatch
still need implementation. Hardware checks must exercise read/close errors
before rename and check complete map/index correspondence with heap/stack
monitoring via `python3 scripts/debugging_monitor.py`.

The HAL publication tests now link actual HalTransferStorage and
HalInventoryPublicationValidator with real encoded snapshots against simulated
SD storage. Two added cases resume each of the four failed publication renames
and verify a coherent revision-8 pair, harmless repeated publication and backup
cleanup; a close failure blocks intent creation and all renames. All six focused
HAL validation/publication tests pass after formatting. The firmware build
continues on its existing handle; no production source changed in this test-only
step. Live ownership/dispatch, rollback and hardware power-cut acceptance remain
unfinished. Reproduce with `ctest --test-dir /tmp/lila-companion-tests -R
HalPublicationValidation --output-on-failure`.


`InventoryRollback` adds a separate RBU-v1 intent for restoring a validated
older index/map pair when a new candidate is corrupt. It searches active/backup
combinations for complete correspondence and a revision below the failed
publication revision, persists/read-verifies two 32-byte rollback copies before
removing any new active snapshot, then restores each member by validated
revision. A restored member is left in place during retry. Only after the complete
old pair validates does it remove failed candidates/backups and publication
intents; rollback intents are removed last. It borrows existing storage,
validation and scratch, without new heap allocation.

The pending probe lets the future recovery owner distinguish absence from a
rollback requiring recovery; pending rollback must run before publication
recovery. Recover without a valid rollback copy returns Corrupt, so the owner
must probe first. Missing/corrupt old pairs are preserved rather than deleted.
This component does not handle an initial corrupt installation with no older
pair. The owner must obtain the failed revision from validated publication state
and recover pending intents before beginning another rollback; those live APIs
and ordering remain unwired.

The new actual-HAL test first interrupts publication after installing the new
index, corrupts the remaining map candidate, and confirms ordinary publication
recovery returns Corrupt. It then restores the old pair, including interruption
of the restore rename and restart from the rollback intent. Pending-state checks
cover before, during and after recovery. Seven focused HAL validation/publication
checks pass. Strict host compilation and whitespace checks pass; host frames are
160 bytes for begin, 176 for recover and 64 for restore, not embedded watermarks.
The existing firmware build remains running and is unaffected by this unused
header/test-only addition. Live rollback wiring, exhaustive rollback mutation
faults, initial-empty rollback and physical power-cut acceptance remain required.
Verify the old coherent revision after candidate corruption and interrupted
restore before permitting inventory access on real devices.

Rollback fault coverage now exercises every mutation before and after its effect
for both restorable layouts: old index in backup with old map still active, and
both old members in backups. Tests use real encoded records and actual pair
validation with simulated durable storage. They distinguish a failure before the
first rollback intent from a lost reply after final cleanup by inspecting pending
state and validating the active old pair. Both torn intent writes preserve a
complete restorable pair; a torn first copy requires a new begin after validation,
while a valid first copy permits recovery when the second is torn. Wrong storage
generation and read failures preserve every recovery artifact.

All 17 focused publication/HAL validation tests pass after the final formatting
step, and whitespace checks pass. Reproduce with `ctest --test-dir
/tmp/lila-companion-tests -R 'CompanionInventoryPublication|HalPublicationValidation'
--output-on-failure`. This was a test-only step; the existing firmware build is
still running. Initial-empty rollback and live recovery ordering remain required,
as do physical SD/power-cut tests before treating inventory recovery as accepted.

`InventoryRollback::beginEmpty` now supports rollback to no inventory for a
failed initial publication. RBU-v2 stores the failed revision (not revision zero)
and generation, while RBU-v1 continues to name an older pair to restore. Starting
empty rollback requires a valid matching IPU publication intent, absent index/map
backups, and each present active member validating for that failed revision.
Unrelated, corrupt or older active files and any backups are preserved and cause
Corrupt rather than deletion. The owner must choose this fallback only after
normal publication recovery fails and no older pair can be restored.

The empty rollback intent is persisted and read-verified before deleting active
inventory metadata. Recovery repeats eligibility checks, removes only the private
index/map snapshot files and candidates, clears publication intents, and removes
rollback intents last. It can resume after either active removal or cleanup;
content and learner files are never selected by these fixed paths. An empty
result requires a new complete scan before an inventory can be served. The
existing scratch is reused with no additional allocation.

Three added cases cover every empty rollback mutation before/after its effect,
torn copies, and rejection of backups, absent/mismatched publication intents and
unrelated active files. All 20 focused publication/HAL tests pass after final
edits; strict compilation and whitespace checks pass. Host frames are 48 bytes
for beginEmpty, 128 for eligibility, 112 for persistence and 192 for recover,
not embedded watermarks. The strict test compile also prompted explicit braces
around a GoogleTest assertion branch to avoid an ambiguous-else warning.

The HAL-validator firmware build completed successfully for default, sticky,
x4pro, x4c and papermono. No build is running. That build does not cover the unused
rollback header, including this new mode. Live recovery ordering, owner wiring,
classification and inventory dispatch remain unfinished. Hardware acceptance
must verify interrupted initial publication returns to no snapshot, then a fresh
scan yields a coherent pair before inventory access; also exercise read/close
faults and measure heap/stack through `python3 scripts/debugging_monitor.py`.

`InventoryRecovery` now coordinates publication and rollback. It probes rollback
first, resumes a valid rollback before inspecting publication, and refuses
conflicting valid rollback copies. A torn first rollback copy can be rebuilt only
when a valid publication intent still supplies the failed revision. Ordinary
publication corruption selects a complete older pair first, then guarded empty
rollback if no older pair can be restored. Both successful outcomes are validated
again through publication recovery. `pendingRevision` exposes only validated
publication intent state, preserving its output on failure.

`recoverInventorySnapshots` supplies the concrete HAL entry point. Its temporary
nothrow-allocated state owns the validator/controllers and borrows the caller's
existing 8 KiB workspace. The state exceeds the local stack budget; no second
scratch allocation is introduced. It is destroyed, closing handles and freeing
state, before the caller continues. Startup now detects index/map candidates,
backups and both intent types, provisions the physical storage generation,
recovers content transfers first and then inventory before reading resumes.
Connect & Sync does the same after content-transfer recovery and before pairing
loads/Bluetooth startup. Failure keeps the existing recovery gate closed.

Three portable coordinator cases cover older/empty selection, no-write repeated
recovery, torn rollback copies in both modes and preservation of conflicting
valid intents. A HAL entry-point case exercises old/empty recovery through actual
HalTransferStorage and snapshot readers against simulated SD storage. All 202
selected host tests pass after final formatting; strict compilation and whitespace
checks pass. Host frames are 96 bytes for coordinated recovery, 64 for fallback
and 80 for the HAL entry point, not embedded watermarks. Run:

```sh
ctest --test-dir /tmp/lila-companion-tests -R 'CompanionInventoryPublication|HalPublicationValidation' --output-on-failure
```

The five-target final firmware build for live recovery wiring is running. The
prior validator-only build passed all five targets. Snapshot construction/owner
publication, real content classification and live inventory dispatch are still
unfinished; no inventory capability is advertised. Hardware acceptance must
interrupt content and inventory operations, reboot into old/empty coherent state
before reader use, and verify repeated sessions do not leak heap or retain tasks.
Use `python3 scripts/debugging_monitor.py` to measure actual heap and task stack
watermarks around recovery and radio startup.

`HalInventorySnapshotStage` now shares the checked SD staging implementation
between fixed index and map candidate paths. `HalInventoryPathsStage` remains the
map wrapper, while `HalInventoryIndexStage` adapts the same backend to the index
builder. Its finish seals a private candidate for the pair coordinator; it does
not rename an authoritative snapshot. Both use one member HAL handle per stage,
with no per-record allocation, extent/bounds checks, sync/close checking, yields
and retained cleanup ownership on failure. The only selectable targets are the
two private candidate paths.

Three added index-stage cases cover a complete CRC-readable index with one
writer open, preservation of the active index during scan/open/write/sync/close
failures, and building both actual snapshot formats then validating/publishing
through the actual HAL pair coordinator. The existing five map-stage cases still
pass after backend sharing. All 205 selected host tests pass after formatting;
strict compilation and whitespace checks pass. Host frames are 160 bytes for
begin, 80 for write, 64 for seal and 128 for discard, not embedded watermarks.

```sh
cmake --build /tmp/lila-companion-tests --target HalInventoryIndexStageTest HalInventoryPathsStageTest
ctest --test-dir /tmp/lila-companion-tests -R 'IndexStageTest|PathStageTest' --output-on-failure
```

The running recovery-wiring firmware build is unchanged by these unused staging
headers. A live scan owner must still reserve a durable revision, resolve/hash
content, build both candidates and route publication through the coordinator;
it must not use the older independent index publisher for that flow. Real content
classification and inventory dispatch remain unfinished. Hardware acceptance
must verify scan/SD faults preserve the active pair and interrupted publication
recovers before inventory access, with actual heap/stack monitoring through
`python3 scripts/debugging_monitor.py`.

### Complete-scan build coordinator

`InventoryBuild` now joins recovery, durable revision reservation, a complete scan, external sorting, path staging and paired publication. `InventoryScan` requires successful handle closure before either candidate is published. `HalInventoryFileSource` implements that lifecycle; `HalInventoryRevisionAllocator` delegates to the existing checked NVS counter. Reserved revisions are consumed even if scanning fails. Publication errors retain the transaction artifacts needed for restart recovery; output revision changes only after successful publication.

Four new host integration tests exercise the actual HAL revision allocator, sorter, stages, validator and publisher with a simulated scan. They cover duplicate content at multiple paths, repeated builds, scan/close failure preserving the active pair, failed reservation, and interrupted publication recovered before the next scan. All 209 selected companion/HAL tests passed. Strict host compilation passed; the coordinator build frame is 192 bytes under `-Os -fstack-usage`. This is host evidence, not a target stack measurement or physical SD fault test.

The pending firmware build completed successfully for default, sticky, x4pro, x4c and papermono. That build covers inventory recovery integration, not this unused build coordinator. Live content classification, session-owned construction, inventory request dispatch and capability advertisement remain outstanding. Verify the eventual device integration with renamed/duplicate EPUBs, supported Tinta packs, interrupted scans and publication, and heap/stack monitoring across repeated Connect & Sync sessions.

### Scheduling during external merge passes

`HalInventorySortStorage` yields one tick after every 32 completed nonempty run reads/writes. This covers merge passes and sorted output consumption, where the directory scanner and content hasher are no longer running. The yield occurs after each HAL file operation returns, outside its storage mutex; reset clears the operation counter. It adds one byte of state and no heap allocation. The existing multiple-pass test now also verifies that this path yields. All 209 selected companion/HAL tests and strict host compilation passed; a new five-target firmware build is pending. Device verification should use a library large enough to require multiple merge passes, check for watchdog resets and retained BLE responsiveness, and monitor task stack/heap during repeated builds.

### Session-owned complete scan pipeline

`HalInventoryBuildSession` constructs the HAL directory walker, file source, hash workspace, path stage, external sorter, index stage, durable revision allocator and recovery/publication controllers together. It borrows the existing session workspace: 4096 bytes for file hashing, 582 for path records and the remaining 3514 for sort/index records. Validation borrows the full workspace only before scanning or after both stages are sealed. Short workspaces are rejected before scanning or revision reservation. The owner contains the fixed directory/reader state and must be created outside the task stack when connection-mode integration is added; it allocates no second content buffer.

Three additional integration tests run the actual traversal/hash/sort/stage/publication pipeline against simulated SD storage, with an explicit test resolver. They cover duplicate paths, multi-chunk file hashing, private-directory pruning, handle reuse, traversal failure preserving the previous snapshot, short workspaces, and 120 unique files spanning multiple sort runs. All 212 selected companion/HAL tests pass. Strict host compilation passes; observed host frames are 176 bytes for owner construction, 16 for its build wrapper, 192 for the portable coordinator and 160 for source iteration. Firmware/physical stack measurements remain required.

The owner header is not yet wired into `CompanionConnectActivity`; real content classification and inventory request dispatch remain outstanding. The currently running five-target build covers the earlier sort-yield change and does not cover this unreferenced session owner.

### Active-course identity verification

`HalInventoryCourseResolver` decorates the general content resolver for `/tinta/course.pack`. On Tinta builds it borrows a session-owned Pack parser and the hash scratch bank, validates the complete pack, reads the persisted course binding and supplies the validated format/course identity. Legacy packs are represented without a guessed course identity. The new `InventoryFileResolver::verifyHashed` hook runs after streaming hash/length calculation and before path recording or manifest emission; a bound course must exactly match its stored manifest. A stale binding, corrupt pack or unreadable binding prevents publication. Builds without Tinta explicitly reject active-course resolution. Other paths and post-hash checks delegate to the general resolver. No new heap allocation is introduced by this decorator.

Tests exercise matching and mismatched hash/length/identity, legacy unbound packs, corrupt data/bindings, delegation, course-disabled behavior, and source rejection before recording. An integration test constructs the actual course resolver and complete HAL scan/hash/sort/publication pipeline against simulated SD storage: after successful publication a stale binding causes rescan failure and preserves both prior inventory files. All 216 selected companion/HAL tests pass. Strict host compilation passed; course resolve/verify frames were 192/48 bytes. This header remains unreferenced by firmware pending general content classification and live inventory dispatch; the running firmware build covers the prior sort-yield change.

### Installed bitmap-font validation

`BitmapFontValidation` validates format-4 `.cpfont` headers, all style identities, sorted Unicode intervals and glyph numbering, bitmap bounds/dimensions, kerning class entries and ligature coverage. It borrows 32 bytes of scratch and performs binary searches over SD-backed interval records instead of allocating an interval table. This trades additional bounded reads for fixed RAM. `HalInventoryFileView` adapts the walker's already-open file without allocating/closing another handle and yields periodically during validation reads. `HalInventoryFontResolver` applies this validator to bitmap files below `/fonts/` or `/.fonts/`, delegates directories/other content, and checks validated length/kind/version before emitting the actual hash. These classes introduce no heap allocation.

The shared `protocol/fixtures/BitmapFont-v4.fixture` exercises intervals, glyphs, kerning and ligatures in both C++ and Swift. Host tests cover all truncation boundaries, malformed records, every read failure, all four styles, duplicate styles, multiple-interval searches and missing coverage, plus a full HAL inventory pipeline whose corrupt font rescan preserves the prior snapshot. All 223 selected companion/HAL tests and all 201 Swift tests passed. Strict host compilation passed; observed validator methods use at most 96-byte frames, bitmap resolver resolve/verify use 128/48 and file-view read uses 80 bytes. Actual firmware stack watermarks remain unmeasured.

The prior sort-yield firmware build finished successfully for default, sticky, x4pro, x4c and papermono. The new bitmap headers remain unreferenced by firmware and are verified by host tests only. Vector-font validation, installed-dictionary bundle representation, the general resolver, connection-mode ownership and inventory request dispatch remain outstanding. Device verification should include actual converted bitmap font families, a corrupt font on SD, repeated inventories with hash/length comparison, and task/heap monitoring.

### Stored vector-font validation

`VectorFontValidation` streams SFNT/CFF table checksums, validates required tables, sorted/unique printable table tags, table alignment/bounds and TTC collection face offsets. Checksum handling excludes the `head` adjustment word and pads the final partial word. Scratch can be as small as 16 bytes; checksum chunks use a multiple of four even when the supplied bank has an odd size. Collection duplicates are checked by rereading prior offsets, so no face/table lists are allocated.

The combined `HalInventoryFontResolver` replaces the bitmap-only decorator and validates stored `.ttf`, `.otf` and `.ttc` files under either font root, including uppercase suffixes. Font manifest format 4 identifies bitmap files; format 1 identifies the validated SFNT/CFF/TTC container family. Both paths reuse the walker's file and scan hash bank. Inventory presence does not establish renderer compatibility: runtime vector rendering is disabled without PSRAM by `VectorFontSupport.h`. Installation and dependent preferences must enforce that board capability before offering a vector font. No FreeType or font-table heap allocation is introduced by validation.

Shared minimal structural/checksum fixtures cover SFNT, CFF and two-face collections in C++ and Swift. They are validator fixtures, not rendered-font acceptance tests. C++ also validates repository Noto Serif, Noto Sans Arabic and Ubuntu fonts. Tests reject truncation, every read failure, duplicate/unaligned collection faces, unknown versions, malformed tags/table bounds and missing required tables. A HAL pipeline test inventories all three container forms and preserves the prior snapshot after a checksum-corrupt rescan. All 229 selected companion/HAL tests and all 202 Swift tests passed. Strict host compilation passed; vector validation/face frames were 80/128 bytes and combined resolver resolve/verify frames were 144/64 bytes. Device stack watermarks remain required.

These headers are still not wired into connection mode, so this turn required no new firmware build. The latest actual five-target firmware build passed for the earlier sort-yield change. Installed-dictionary bundle representation, the general content resolver, live inventory construction/dispatch, font/dictionary installation and physical acceptance remain outstanding.

### Deterministic dictionary export container

`DictionaryBundleBuilder` streams a reproducible ZIP32 candidate from validated StarDict members. It accepts data, index, info and optional synonym sources; fixed archive names are `dictionary.dict` (or `dictionary.dict.dz`), `dictionary.idx`, `dictionary.ifo` and `dictionary.syn`. Member order is fixed, ZIP method is stored, UTF-8/data-descriptor flags are fixed, DOS time/date are 00:00/1980-01-01, and there are no extra fields or comments. CRCs and the central directory are written after streamed member data. The builder borrows at least 64 bytes of scratch, keeps metadata for only four entries, and introduces no heap allocation or compression workspace.

The source must validate dictionary content and compatibility before supplying members; the ZIP writer does not replace `.ifo`, `.idx`, synonym or dictzip validation. Source length is checked again after each stream and successful source closure is required before sealing. Read/write/close/length/ZIP32-limit errors abort the private candidate and preserve the caller's output length. Sealing does not publish inventory or install a dictionary. Same-length concurrent input mutation is not made atomic by this writer; live use requires stable input ownership during scanning.

Existing imported ZIPs retain their exact byte identity. A reconstructed legacy dictionary export has its own canonical archive hash, not the hash of any unknown original ZIP. Companion installation will need to retain the transferred archive and verify its installed-member binding before reusing that original identity; fresh exports must be hashed as the exact resulting bytes.

Shared plain, dictzip and no-synonym archive fixtures are byte-identical to C++ writer output at different chunk sizes and pass the Apple archive/dictionary inspector, including CRCs, index bounds and dictzip validation. Fault tests cover every source read and stage write, close/seal failures, changing lengths and preflight overflow. All 232 selected companion/HAL tests and all 203 Swift tests passed. Strict host compilation passed; build/member/central frames were 128/112/48 bytes. These are host checks, not physical SD fault or dictionary lookup acceptance tests.

The builder is not yet referenced by firmware. HAL dictionary discovery/member validation, cache staging and installed-member binding, inventory emission for bundles, original-archive retention during installation and live request dispatch remain outstanding. No new firmware build was required for this unreferenced header; the latest actual five-target build remains the successful sort-yield build.

### Streamed StarDict index validation

`DictionaryIndexValidation` validates definition indexes and synonym indexes across arbitrary read boundaries. It requires nonempty valid UTF-8 headwords of at most 255 bytes, ASCII-folded nondecreasing ordering, exact byte/record counts, nonzero in-bounds definition slices and valid synonym ordinals. Failures remain sticky until a new begin; input after a completed stream is rejected. A failed final source-size query cannot later be reported as successful completion.

The state keeps one 255-byte word buffer and an eight-byte numeric suffix buffer. Each incoming folded byte is compared with the previous word before replacing that byte in place; prefix lengths retain the shorter-prefix ordering check. No second word buffer, heap allocation or per-record allocation is used. This state must remain session-owned outside the embedded task stack. The reader-backed validation method borrows caller scratch and an existing `InventoryIndexStorage`; it leaves file ownership/closure to the caller. Reported source lengths are checked before and after reading, without claiming atomicity for concurrently modified input.

Tests cover every chunk size for mixed-case/Unicode headwords, equal words, descending/shorter-prefix words, all truncations, malformed UTF-8, 255/256-byte words, numeric boundaries, mismatched counts, synonyms, post-finish input and actual HAL file-view reads with I/O/metadata failures. Shared definition/synonym fixtures pass both C++ and Swift validators at every chunk size. All 239 selected companion/HAL tests and all 204 Swift tests passed. Strict host compilation passed; reader-backed validation used a 96-byte frame and the inlined begin/consume/finish methods had no additional host frame. Physical task stack watermarks are still required.

The header remains unreferenced by firmware. Full `.ifo` metadata validation, dictzip validation through HAL, installed-member binding/cache staging, dictionary inventory emission and live inventory dispatch remain outstanding. No firmware source dependency changed in this turn.

### Bounded `.ifo` metadata validation

`DictionaryInfoValidation` validates UTF-8 metadata up to 64 KiB, required header/version/name/count fields, optional synonym counts, numeric overflow, duplicate field names, 32-bit offsets and definition-type sequences. Compatibility-sensitive fields must finish before byte 2047 and appear exactly once in the reader-visible prefix, preventing misleading occurrences inside other values. Supported metadata field names are ASCII; values, including titles, remain UTF-8. Non-ASCII extension keys currently return failure; the Apple parser accepts a broader key grammar, so reader compatibility still needs to be checked before installation and this is not a claim of complete grammar equivalence.

The validator stores no field strings or key table. It borrows a cache bank and rescans prior keys for exact duplicate detection. This bounds RAM at the cost of extra work for unusually large metadata. An optional progress/cancellation callback runs every 512 byte lookups, including cached lookups; live HAL integration must provide it to yield during rescans. Callback/read failures remain sticky within the validation attempt. Output details change only on success.

Tests cover cache widths 1–64, UTF-8 titles, supported versions, CRLF, numeric bounds, required/duplicate fields, whitespace-only names, malformed UTF-8, unsupported keys, ambiguous/late compatibility fields, every source-read failure and cancellation. A shared metadata fixture agrees between C++ and Swift on counts, synonyms and HTML type. All 244 selected companion/HAL tests and all 205 Swift tests passed. Strict host compilation passed; validation/duplicate scan/scalar decode frames were 208/128/96 bytes. Device stack measurements remain outstanding.

This header is still not referenced by firmware. HAL dictionary discovery/validation composition, dictzip validation, original-archive/member bindings, cache staging, bundle inventory emission and live inventory dispatch remain unfinished. No firmware dependency changed in this turn.

### Streamed dictzip random-access layout checks

`DictzipLayoutValidation` validates the gzip/extra-field envelope, a unique RA version-1 table, nonzero chunk width/count (up to 8192), trailer size/chunk-count agreement, optional filename/comment bounds, compressed chunk bounds and independent flush markers. It rereads table entries from the borrowed source and uses only 12 bytes of scratch, avoiding a copied chunk-offset table. Header/table/chunk read errors preserve the caller's output.

This is structural validation only. It does not prove DEFLATE correctness, decoded per-chunk sizes, gzip termination, header CRC or decoded CRC; those checks are required before dictionary acceptance. A dedicated test demonstrates that changing the stored CRC still passes layout validation, preventing these tests from being mistaken for integrity evidence. Full validation will need a reused decompression window/state with explicitly justified allocation and task yielding.

Tests cover the existing three-chunk/120000-byte fixture, invalid RA versions/lengths/counts, shifted chunk boundaries despite a valid gzip stream, other extra fields, duplicate RA fields and every read failure. All 249 selected companion/HAL tests passed; strict host compilation passed with a 128-byte layout-validation frame. The existing Swift fixture validation remains unchanged.

The header remains unreferenced by firmware. Full gzip/chunk decode and CRC validation, dictionary discovery/member composition, private archive cache/bindings and live inventory dispatch remain unfinished. No firmware dependency changed in this turn.

### Decoder history bounds

The firmware's uzlib ring decoder now rejects LZ distances beyond the history decoded since its last initialization. A saturating `dict_filled` counter is updated for literals, stored blocks and copied bytes, and reset by `uzlib_uncompress_init`. Capacity checks alone did not reject references into an unfilled ring. This adds one unsigned integer to each decoder state, with no buffer or heap allocation.

Four host regression tests cover a reference before the first literal even with a prefilled ring, valid overlapping copies across output calls, ring wrap/reset, and exact decoding of the shared 120000-byte dictzip fixture. All 13 focused uzlib, InflateStream and dictzip-layout tests passed. The test target removes unused checksum entry points with function-section garbage collection, matching the firmware's raw-DEFLATE usage; it does not substitute checksum stubs.

This change does not make the structural dictzip validator an acceptance validator. Full decoding checks are implemented separately below and still need live integration. On a physical reader, open existing dictzip dictionaries and read entries spanning chunk boundaries; monitor errors and free/largest heap with `python3 scripts/debugging_monitor.py` across repeated sessions.

### Full dictzip decoding and integrity checks

`DictzipValidation` composes layout checks with optional gzip header CRC, independent chunk decoding, exact per-chunk output lengths, final-block consumption and the combined decoded CRC. It adds an empty final block after each flushed chunk to verify complete independent DEFLATE termination. The actual gzip final block must consume the remaining body and produce no output. Successful validation also requires unchanged reported source length; callers must own a stable input file rather than assume this detects every same-length mutation.

The validator borrows one miniz decoder, a 32768-byte history window and an input bank of at least 12 bytes. No buffers are allocated in the validator or per-chunk loop. A live session must allocate/own decoder and window outside the task stack, reuse them through the operation, and release them on exit. Miniz's new opt-in `TINFL_FLAG_VALIDATE_RING_HISTORY` rejects references before the decoded history; a saturated counter adds one `mz_uint32` to decoder state. Structural reads and decoding invoke the caller's progress/cancellation callback; HAL integration must provide yielding and cancellation.

Seven tests cover input-bank widths 12–512, the shared three-chunk fixture, incorrect independent output lengths, corrupted CRC/DEFLATE/final blocks, trailing compressed bytes, header CRC, every source-read failure, cancellation and a gzip-valid Z_SYNC_FLUSH stream whose second chunk depends on previous history. All 264 selected companion/HAL/decompression tests passed. Strict host compilation passed; validate/decode/header-CRC frames are 192/176/64 bytes. Firmware builds and physical stack/heap measurements remain separate checks.

The full validator is not yet connected to dictionary discovery or installation. Dictionary metadata/index/synonym composition, archive cache/bindings, bundle emission and live inventory dispatch remain unfinished.

### Complete dictionary member validation

`DictionaryMembersValidation` validates the data/index/info/optional-synonym members of one stable `DictionaryBundleSource`. Metadata's index length must match the actual index, definition offsets and lengths must fit the plain or decompressed data, and synonym ordinals must fit the declared word count. A supplied synonym file requires a metadata declaration; absent synonyms are allowed only when the declared count is zero or absent. Plain data is read completely to detect I/O failures. All reported member lengths are checked again before success, and close failure preserves the caller's output.

The owner contains the bounded index parser instead of placing it on the task stack. It borrows scratch and, only for compressed data, a miniz decoder and 32768-byte window. No heap allocation occurs in validation. Separate parser helper frames bound task-local storage: strict host compilation reports 176 bytes for composition, 144 for its dictzip helper, 80 for metadata setup and 48 for plain-data streaming. The index reader accepts a progress/cancellation callback and checks it for every bank. A live owner must provide task yielding and retain stable member bytes across validation and archive building; length checks do not prove same-length mutation safety.

Six composition tests cover shared plain/compressed fixtures, plain operation without an inflater allocation, cross-member length/range/order conflicts, synonym declaration policy, every read failure, close failure, cancellation during index streaming and retry. All 270 selected companion/HAL/decompression tests passed. The previously documented ASCII-only metadata-key restriction remains a compatibility difference from the Apple parser.

This composition header is not yet referenced by firmware. HAL member discovery/source ownership, archive cache/bindings, bundle emission and live inventory dispatch remain unfinished. The pending five-target verification covers the miniz strict-history implementation; physical reader stack/heap and SD interruption checks remain outstanding.

### HAL dictionary member source

`HalDictionaryBundleSource` owns four `HalFile` members and a fixed 512-byte path bank, outside the task stack. It opens the data/index/info/optional-synonym members for a discovered base path, rejects directory members and unsafe/overlong paths, validates exact read bounds, and fails closed after I/O errors. Reads yield every 32 successful banks, outside the HAL mutex. Close releases every open member in reverse order while retaining wrapper storage; reopen checks the previous member lengths. The caller must retain stable content ownership because equal lengths do not prove equal bytes.

`HalStorage::openFileForReadReusing` allocates each wrapper with `makeUniqueNoThrow` only on its first use, checks OOM, and retains that wrapper for later opens. Close and SDK read-open run under `StorageLock`. The allocation is necessary for the existing opaque HAL file implementation; this avoids four wrapper allocations/deallocations for every validation/build cycle. Member reads, seeks, size queries and closes remain HAL-only.

An actual source/validator/archive-builder integration test exposed an early source close in the builder's per-member loop. The builder now keeps the source open across all members and performs its existing checked final close before central-directory generation and archive sealing. This lets one source own the complete member set and keeps close errors from publishing a candidate.

Five source tests use simulated SD storage to cover plain/dictzip validation, reopen and archive construction, retained wrappers across repeated scans, missing/directory members, unsafe paths, short reads, bounds, length changes, close failures and read yielding. All 275 selected companion/HAL/decompression tests passed. Strict host compilation passed; source begin/reopen/read frames were 96/80/48 bytes. These are not physical SD, allocation or stack measurements. The prior uzlib five-target build completed successfully; later miniz/HAL firmware verification is still running, with a final default build queued after the last code edit.

Discovery, cache staging/bindings, bundle inventory emission and live inventory dispatch remain unfinished. The new member source is not yet constructed by the live companion activity. Verify the source tests with `ctest --test-dir /tmp/lila-companion-tests -R HalDictionaryBundleSource --output-on-failure`; after live integration, exercise repeated plain/dictzip scans with `python3 scripts/debugging_monitor.py` and record free/largest heap, stack watermarks and SD errors.

### Dictionary folder discovery

`HalDictionaryDiscovery` scans a supplied dictionary folder using two retained HAL handles and fixed session-owned buffers. The first pass identifies exactly one lowercase `.idx` stem; the second requires matching `.idx`, `.ifo` and plain/dictzip data and identifies optional synonyms. Plain data takes precedence when both forms exist, matching the Apple inspector. AppleDouble entries and unrelated files/subdirectories are ignored. No supported index yields Empty; ambiguous, duplicate, missing or unsafe members and I/O failures yield Error. A changed index set between passes is rejected. Successful output is assigned only after both complete scans and checked closes; Empty/Error expose no base path.

The base path is borrowed until the next inspect call. No member contents are accepted by discovery alone: the HAL member source and full member validator must run before archive staging. Read/close/name failures and progress cancellation preserve caller metadata. The optional progress callback runs before each directory step, and scans yield every 32 entries. Directory and entry wrappers are reused across folders, avoiding allocations in the entry loop; fixed buffers remain outside the task stack.

Six tests cover arbitrary member order, plain-data precedence, AppleDouble files, empty/incomplete/ambiguous/duplicate sets, unsafe/truncated names, directory and close failures, retained wrappers and large scans, cancellation/retry, a borrowed folder prefix, and discovered dictzip members passing complete validation. All 281 selected companion/HAL/decompression tests passed. Strict host compilation passed with a 176-byte inspect frame. The header is not yet constructed by the live companion activity. Cache staging/bindings, bundle inventory emission and live inventory dispatch remain unfinished. Reproduce discovery checks with `ctest --test-dir /tmp/lila-companion-tests -R HalDictionaryDiscovery --output-on-failure`.

### Private archive staging with SHA readback

`HalDictionaryArchiveStage` implements the archive builder's stage interface at `/.crosspoint/companion/dictionary-next`. One serialized owner may create the candidate only when that path is absent. Unknown or sealed candidates, including directories, are preserved for recovery/publication. Writes must be contiguous and fit ZIP32 limits. Sealing requires matching length, truncate/sync, SHA-256 of the complete written stream matching a full HAL readback, and a checked close. This rejects corrupted bytes returned by readback even when writes report the expected byte count. It checks the synced file's reported bytes, not ZIP semantics or physical power-loss durability; the member validator and canonical builder provide format checks, and hardware interruption tests remain required.

The stage borrows at least 64 bytes of readback scratch and stores its SHA context in the owner. `HalStorage::openFileForWriteReusing` retains one nothrow-allocated write/read wrapper across builds, while checked presence lookup retains two directory handles; SDK close/write-open run under `StorageLock`. No per-write buffer or wrapper allocation occurs. Abort/destruction removes only owned partial files; cleanup failure retains ownership for retry, while sealed candidates belong to cache publication and are preserved. The stage neither replaces installed dictionaries nor publishes inventory records.

Stage preparation/writes/sealing and the shared `hashInventoryFile` helper accept optional progress/cancellation callbacks. SHA readback checks cancellation before every bank and before returning outputs. Existing helper callers keep their default behavior, and all hash outputs remain unchanged on failure. Writes yield every 32 banks; readback yields for each bank. The live owner must supply its cancellation context and keep source members stable throughout validation/build.

Eight stage tests cover sync/readback hash matching, simulated silent write corruption, unknown/sealed candidates, directory collisions, retained wrapper allocations, write bounds and failures, prepare/open/truncate/sync/close/read errors, cleanup retry, cancellation during readback, and actual HAL member validation plus archive construction. A separate hash test checks cancellation/output preservation and retry with the same borrowed handle. All 290 selected companion/HAL/decompression tests passed, including rebuilt transfer/inventory callers of the extended hash helper. Strict host compilation passed; stage begin/write/seal frames were 144/64/160 bytes. Firmware verification is still running, with a final default build queued after the last compiled code edit; physical SD, heap and stack checks remain outstanding.

The stage header is not yet constructed by the live companion activity. Immutable cache publication/recovery, retained-original-archive/member bindings, bundle inventory emission and live dispatch remain unfinished. Reproduce staging tests with `ctest --test-dir /tmp/lila-companion-tests -R HalDictionaryArchiveStage --output-on-failure`.

### Immutable dictionary cache publication

`DictionaryCachePublication` places a validated/sealed archive at `/.crosspoint/companion/dictionary-<sha256>.zip`. It requires dictionary format 1, ZIP32 length bounds and zero course-family identity. A path becomes available only after exact length/SHA verification. A lost rename reply can be retried when the candidate is missing and the target verifies. Identical candidates are removed only after the existing target verifies. A successfully read regular cache file with mismatching bytes/length can be repaired only while a verified candidate is present; read/open/close errors and directory collisions preserve both files.

Cache additions precede authoritative inventory publication and do not replace installed dictionary members. The fixed candidate is private scratch and must never be recorded in inventory. After inventory recovery, the serialized cache owner may explicitly discard an unpublished candidate and rebuild it from validated members. No new journal is needed for immutable additions: retries verify whichever candidate/target exists, and reboot recovery can rebuild scratch. This recovery model does not apply to dictionary installation, retained-original-archive bindings or authoritative snapshot replacement; those remain separate transactions. Filesystem rename interruption and physical SD durability still require hardware testing.

`HalDictionaryCacheStorage` retains a reusable HAL file wrapper for verification and two for checked directory lookup, distinguishes successfully read mismatches from I/O failures, checks close errors, supports hash cancellation and uses HAL for every operation. It does not allocate buffers per verification; workspace is borrowed. The publisher keeps its bounded hash-addressed path in the session owner. Strict host compilation passed with 64/176-byte publication/inspection frames.

Five portable tests cover hash-addressed publication, idempotent retries, every mutation failing before/after effects, deduplication, corrupt-cache repair, read errors, collisions, invalid contracts, explicit scratch cleanup and final-read/corruption failures after rename. Four HAL tests cover actual read/close/cancellation failures, retained wrappers and the complete member-validation/archive-builder/stage/cache pipeline. All 299 selected companion/HAL/decompression tests passed; installed member bytes remain unchanged in the fault tests. Reproduce cache checks with `ctest --test-dir /tmp/lila-companion-tests -R 'DictionaryCachePublication|HalDictionaryCacheStorage' --output-on-failure`.

The queued firmware runs are terminal successes: the five-target run passed default, sticky, x4pro, x4c and papermono; the final default run passed after the compiled HAL/hash edits. There is no currently running firmware build. These builds verify compiled dependencies, not the new unused cache headers. Cache helpers still need retained-original-archive/member bindings, inventory bundle emission, live construction/dispatch and physical reader acceptance. No inventory capability is advertised by this addition.

### UTF-8 dictionary metadata field identity

Apple and firmware metadata validators accept UTF-8 field names and compare their exact encoded bytes. Canonically equivalent Unicode spellings are distinct extension keys; repeating the same bytes is rejected. Known StarDict fields retain their ASCII names. Firmware already validates the entire input as UTF-8 and its bounded duplicate scanner compares bytes, so accepting extension names adds no buffers or heap allocations. Apple uses Data keys in its existing metadata dictionary to avoid Swift String canonical-equivalence comparisons. This fixes the earlier ASCII-only firmware restriction without requiring a Unicode normalization table on the reader.

Tests cover composed/decomposed extension keys, exact duplicate rejection, malformed UTF-8 and firmware scratch widths 1–64. All 35 affected C++ metadata/member/HAL pipeline tests and all 206 CompanionKit tests passed. Reproduce with `ctest --test-dir /tmp/lila-companion-tests -R 'DictionaryInfoValidation|DictionaryMembersValidation|HalDictionaryBundleSource|HalDictionaryDiscovery|HalDictionaryArchiveStage|HalDictionaryCacheStorage' --output-on-failure` and `swift test --package-path apple/CompanionKit`. These are host checks; Apple platform branches and physical reader behavior remain unverified. The firmware helper remains unused by live compiled sources, so no firmware build was repeated for this header-only change. Retained archive bindings and live dictionary inventory/install integration remain unfinished.

### Directory bundle inventory emission

`InventoryFileResolver::resolveBundle` handles directories already accepted by the ordinary resolver. Skip preserves traversal; Include supplies a fully validated immutable dictionary ZIP and its exact manifest; Error fails the scan. The scanner prunes a bundled folder, opens the supplied archive through a reusable HAL wrapper, requires dictionary format 1/zero course family/ZIP32 bounds, hashes the actual bytes, checks exact length/hash and a checked close, then checks bindings before staging its path. The private `dictionary-next` candidate is explicitly forbidden even when its bytes match. Course/font decorators forward the hook. Unsupported folders must be rejected by the concrete resolver rather than silently omitted.

The archive path is borrowed until the next resolver call. The concrete dictionary resolver remains responsible for complete member validation, original archive/member bindings and immutable cache publication before returning Include. The scanner does not infer ZIP semantic validity from its hash. It neither installs a dictionary nor advertises inventory capability. Its additional opaque file wrapper is allocated once on first bundle use and reused; no extra hash buffer is allocated. Source close checks both its archive handle and traversal cleanup.

Eight scanner tests include repeated bundled scans, pruning, actual archive identity, ordinary traversal fallback and failures in resolution/open/read/hash/length/format/family/binding/path staging, directory collisions and private candidate rejection. The affected scanner/course tests (16) and build-session/directory-walker tests (17) passed. Strict standalone host compilation passed with 160-byte scanner and 208-byte archive verification frames, after separating verification from traversal. These are host frames, not physical ESP stack measurements. Reproduce with `ctest --test-dir /tmp/lila-companion-tests -R 'InventoryFileSourceTest|HalCourseTransferTest|InventoryBuildTest|DirectoryWalkerTest' --output-on-failure`. No live compiled firmware source constructs this scan yet, so no firmware build was repeated for these unused helper changes. Concrete dictionary resolution, retained-original bindings, live snapshot construction/dispatch and device acceptance remain unfinished.

### Concrete dictionary inventory resolver

`HalInventoryDictionaryResolver` composes discovery, member source/full validation, canonical ZIP construction, private staging and immutable cache publication. It handles `/dictionaries` and `/.dictionaries`, direct non-hidden dictionary folders, and prunes member/nested paths through the directory bundle hook. Empty folders keep ordinary traversal; malformed supported dictionaries fail the scan. Preparation explicitly discards unpublished private scratch only after authoritative inventory recovery under exclusive cache ownership. Installed source files remain untouched.

A required `DictionaryArchiveBindings` provider distinguishes unbound legacy dictionaries from bound transferred archives and errors. Legacy folders export the canonical member archive. A binding carries the canonical manifest of installed members and the exact retained original archive manifest. Bound folders require the member manifest to match the newly validated/built canonical bytes and the original cache blob to verify; missing originals, stale bindings and unreadable bindings fail without silently switching to canonical identity. Installation must publish such bindings only after validating the original ZIP and extracted members. The provider interface is not durable binding storage, and does not prove an arbitrary supplied association is semantically valid. That storage and recoverable installer remain required.

All buffers are borrowed and phase-reused. Compressed content requires a borrowed miniz decoder plus its 32-KiB window; plain content needs neither. The composition and its fixed discovery/index/manifest state must be session-owned outside the embedded task stack. Existing opaque HAL wrappers are allocated once on first use and retained across repeated scans; no new per-bank or per-folder buffer allocation is introduced. The serialized owner must prevent source member mutation throughout validation/build; reopening and length checks alone cannot detect concurrent same-length edits. Output manifest/path are exposed only after complete cache/binding checks.

Seven resolver tests cover an actual directory walk through validation/build/cache/scanner emission, repeat scans with retained handles, exact original ZIP identity despite a differing archive comment, missing originals, bad member content, I/O/silent write corruption, stale/unreadable bindings, preparation, cancellation/retry, empty/hidden/nested boundaries and compressed content. All 50 affected resolver/scanner/build/course/member/cache/walker tests passed; the seven resolver tests were rerun after resetting output readiness before delegated bundle resolution. Strict standalone host compilation passed; the final resolver frame is 240 bytes. These are host checks, not SD power-loss or ESP heap/stack measurements. Reproduce with `ctest --test-dir /tmp/lila-companion-tests -R HalInventoryDictionaryResolverTest --output-on-failure`. The helper is not yet constructed by a compiled firmware source, so no firmware build was repeated. Durable original/member bindings, installation, live resolver/session construction and inventory dispatch remain unfinished.

### Durable dictionary archive bindings

`CompanionDictionaryArchiveBinding` defines DBND v1, a 170-byte record: an eight-byte magic/version/reserved prefix, SHA-256 of the exact dictionary base-path bytes (32 bytes), canonical-member and retained-original manifests (63 bytes each), then little-endian CRC32 of the preceding 166 bytes. Both manifests require dictionary format 1, ZIP32 length bounds, zero course family and nonzero SHA-256. Decode checks exact size, key, checksum and semantic fields before assigning output. Bytewise wire decoding accepts unaligned record spans. The record does not itself prove the original ZIP semantically matches its members; the installer must establish that association.

`HalDictionaryBindings` stores records privately at `/.crosspoint/companion/dictionary-binding-<base-path-sha256>` with `.tmp` and `.bak` transaction artifacts. It borrows at least 170 bytes of scratch and reuses one record HAL wrapper, two checked-lookup wrappers, a fixed session-owned name buffer and a SHA context. There is no per-record data-buffer allocation. Reads check the record's length, bytes, close and key before exposing it; stage/backup artifacts reject normal reads. Original and canonical cache archives must both verify before publication. Stage write/truncate/sync/close and decoded readback precede active-to-backup and stage-to-active renames. The old backup remains available until parent commit; retries after reported rename errors recognize the verified new active record.

These methods are participants in the dictionary installer transaction, not an independent installation journal. `install` requires a durable Installing parent containing this exact binding and prior ZIP/member semantic validation. `finalizeInstallation` requires a durably Committed parent that still retains recovery data. Only then may obsolete owned stage/backup files be removed, even if a backup's bytes were damaged. Normal scan/read calls must wait for parent recovery; an initial installation can have no artifacts after the last rename even before parent commit. The parent must validate installed member contents during recovery, serialize content mutations, and retain original/member backups until its own completion. Rollback, original ZIP extraction and this parent transaction remain unfinished.

Bindings are keyed by the current base path. A managed folder rename must migrate its binding; external SD renames are not automatically associated with an old original archive. Binding and dictionary-cache lookup now uses the checked companion-directory helper described below. Other providers still using boolean `HalStorage::exists` require a separate audit before live integration.

Three codec tests cover complete corruption/truncation sweeps, unaligned round trips, wrong path keys and semantically invalid manifests with recomputed checksums. HAL binding tests cover reconstructed providers, retained handles, backup retention, every rename failing before/after effects, cleanup failures before/after effects, stage write/sync/truncate/readback/close failures, malformed records, missing blobs and verified replacement with a damaged owned backup. A resolver test uses the actual durable provider through directory traversal and verifies retained-original emission and pending replacement rejection. All 49 selected codec/binding/resolver/cache/stage/build/course tests passed after the final edits. Strict standalone host compilation passed; codec decode, binding install/read/path/finalize frames are 192/160/80/64/48 bytes. These are host checks; no physical SD, heap or ESP stack guarantees follow. The new helpers remain unused by compiled firmware sources. Reproduce targeted checks with `ctest --test-dir /tmp/lila-companion-tests -R 'DictionaryArchiveBindingTest|HalDictionaryBindingsTest|HalInventoryDictionaryResolverTest' --output-on-failure`. Firmware builds were not repeated for unused header-only changes.

### Checked companion-file absence

`HalCompanionFileLookup` searches the already-existing `/.crosspoint/companion` directory through HAL enumeration. It supports direct ASCII child paths, uses ASCII case folding for FAT names, and reports Present only after checked cleanup. Missing requires checked end-of-directory and both closes. Parent open/type errors, directory errors, unreadable/truncated names, cancellation and close failures report Error. The existing SdFat directory-error patch distinguishes I/O failure from normal end; no new SDK API is assumed or bypassed. A missing parent is Error: session preparation must create the private directory before lookup.

Dictionary binding reads, pending-artifact checks, binding cleanup, dictionary cache verification and cache cleanup now use this helper. They do not interpret a false boolean existence probe as absence. Explicit tests force `Storage.exists` to return false for existing files, then confirm successful binding/cache reads through enumeration. Directory I/O failures preserve binding output, hide cache paths, refuse cleanup and leave file bytes unchanged. This addresses lookup errors, not arbitrary silent filesystem corruption or uncoordinated changes after enumeration. The serialized owner remains required.

Each helper holds a 256-byte name buffer and two HAL handles in its session owner, outside the embedded task stack. Their opaque wrappers are allocated once on first lookup and reused. The additional storage is required to read directory entries without repeated wrapper allocation or a large local name buffer; no per-entry buffer is allocated. Callbacks run before enumeration steps and scans yield every 32 entries outside HAL method locks. Proving absence costs a directory scan; repeated lookups can be expensive in large private cache folders and must be included in live deadline/heap/stack acceptance. No performance improvement is claimed.

Five helper tests cover successful absence, files/directories, FAT ASCII case, reused handles, failed open/close/names, missing parents, invalid targets, cancellation/retry and a 100-entry yielding scan. Binding/cache regression tests cover false existence probes and directory errors. All 53 selected helper/binding/resolver/cache/stage/build/course tests passed after the final functional edits. Strict standalone host compilation passed: lookup inspect 96 bytes, binding path/read/install/finalize 64/80/176/48 bytes, resolver 224 bytes. These are host frames and fault simulations, not physical SD or ESP measurements. Reproduce with `ctest --test-dir /tmp/lila-companion-tests -R 'CompanionFileLookupTest|HalDictionaryBindingsTest|HalInventoryDictionaryResolverTest|HalDictionaryCacheStorageTest' --output-on-failure`. No compiled firmware source constructs these helpers yet, so no firmware build was repeated for this change. Recoverable dictionary extraction/installation and live inventory construction/dispatch remain unfinished.

### Bounded ZIP member extraction

`ZipEntryExtraction` streams a previously validated ZIP payload span into a private `ZipEntrySink`. It supports stored and raw DEFLATE data, UTF-8/data-descriptor flags and the standard DEFLATE option bits; encrypted, reserved or unsupported methods/flags are rejected before beginning a stage. Source range arithmetic is checked by subtraction, and expanded entries are limited to 256 MiB, matching the current Apple default entry limit. Stored lengths must agree. CRC32, exact expanded length, complete compressed consumption, successful final source-size check and sink seal are required before handoff. Initial cancellation does not call begin/abort; once begin is attempted, any failure aborts only owned partial bytes. Sealed prior output is preserved.

Scratch is borrowed and reused. DEFLATE requires the existing miniz decoder and a 32-KiB ring supplied by the owner; stored entries need neither. Strict ring-history validation rejects references to unfilled history, including stale bytes from earlier operations. The extractor allocates no buffers or entry tables. A dictionary installer can extract outer ZIP entries to SD, then reuse the same decoder/window for inner dictzip validation rather than hold two decompression windows concurrently. No C3 heap fit is claimed until live owner/radio integration is measured. Callbacks run before staging, input/decode steps and sealing; the live owner must provide cancellation/deadline maintenance. Source content must remain stable; a final length check cannot detect same-length concurrent edits.

This is an entry stream engine, not a ZIP archive validator or installer. Central/local headers, duplicate/unsafe paths, overlap, descriptors, archive counts/limits and selected dictionary members still require validated metadata before extraction. The sink must check actual stored bytes/durability on seal, preserve unknown files and retain failed-cleanup ownership for parent recovery. Full dictionary member validation, original archive retention/binding and recoverable multi-file publication remain separate required phases. The helper is not yet used by a compiled firmware source.

`ZipEntry-deflate.fixture` is a 197-byte raw DEFLATE stream generated with Python zlib level 6/wbits -15 from `b'abcdef' * 20000` (120,000 bytes, independent CRC32 `0x7146dd0b`). Eight tests check all input bank widths 1–512 and repeated history wrapping, stored offsets, empty stored/DEFLATE entries, CRC/truncation/trailing/length corruption, unfilled history, changed/reported-failed source size, every input/sink failure, unsupported flags/bounds, cancellation at every progress point and retry. All eight passed after the final edits. Strict standalone host compilation passed with 112/144-byte extraction/DEFLATE frames. Reproduce with `ctest --test-dir /tmp/lila-companion-tests -R ZipEntryExtractionTest --output-on-failure`. These are host checks; physical SD interruption, ESP heap/stack and live transfer acceptance remain unverified. No firmware build was repeated for this unused header-only addition.

### Verified member staging and shared file-stage engine

`HalVerifiedFileStage` now implements private staging shared by the existing dictionary archive adapter and new `HalZipEntryStage`. Creation requires checked absence in the companion directory, so a false boolean existence probe cannot authorize truncating an unknown candidate. Writes are contiguous and bounded by the adapter's limit. Seal requires declared/actual extent agreement, truncate/sync/reported size, streamed SHA matching complete HAL readback and checked close. Sealing transfers ownership without installing or indexing the file. Source semantic validation and physical power-loss durability remain separate requirements.

The archive adapter retains ZIP32 bounds and the 22-byte minimum. The member adapter uses `/.crosspoint/companion/dictionary-member-next`, enforces the entry's exact expanded length, supports empty members and limits output to the extractor's 256-MiB entry limit. Inventory bundle emission explicitly rejects both archive and raw-member candidates, including when their supplied hash and length verify. The installer must move sealed member stages into its controlled staging directory and journal their disposition before publishing an installed dictionary. Unknown/sealed candidates and directory collisions are preserved.

Abort closes and removes only owned partial bytes. Failed close, lookup or remove retains ownership for retry. Checked cleanup lookup bypasses operation cancellation while retaining directory error checks and yielding; this lets a cancelled extractor release owned stages instead of leaving them solely because its cancel flag is set. Other lookup calls remain cancellable. One write/read handle, two lookup handles, a fixed name buffer and SHA context are session-owned and reused; all scratch is borrowed. No per-write/per-entry buffer allocation is added.

Six new member-stage tests exercise actual stored/empty and DEFLATE extraction into simulated SD files, SHA readback, silent write corruption, write/sync/truncate/read/close/open/preparation failures, unknown files/directories, lying existence probes, lookup errors, cleanup retry, cancellation cleanup and declared/contiguous bounds. Existing archive-stage tests still pass through the shared engine; callback and retained-wrapper expectations account for checked lookup. All 67 selected stage/cache/resolver/scanner/build/course/lookup/binding tests passed after the final edits. Strict standalone host compilation passed with shared begin/seal/cleanup frames of 128/160/144 bytes; inventory next/archive verification remained 160/208 bytes. These are host measurements and fault simulations, not physical SD or ESP heap/stack results. Reproduce with `ctest --test-dir /tmp/lila-companion-tests -R 'HalZipEntryStageTest|HalDictionaryArchiveStageTest|InventoryFileSourceTest' --output-on-failure`. No compiled firmware source constructs these helpers, so no firmware build was repeated. ZIP layout/path validation, full original/member validation, parent installation journal/recovery and live dispatch remain unfinished.

### Bounded ZIP32/ZIP64 end-directory metadata

`ZipDirectoryLayoutValidation` finds EOCD records throughout the entire permitted 65,557-byte ZIP tail using borrowed scratch of at least 64 bytes. Backward banks overlap by 21 bytes so an EOCD crossing any boundary is still considered. Comment length must account for the exact file end. Disk fields must describe one disk; counts must agree; directory bounds use subtraction to avoid overflow and cannot overlap the end structures. The configured entry limit defaults to 20,000, matching Apple validation, and declared entries must fit the minimum central-header size. A final size/cancellation check precedes output assignment. There is no tail-buffer allocation or unaligned multi-byte load.

ZIP64 sentinel fields require a locator and complete record. An optional locator with nonsentinel ZIP32 fields is also checked. Locator/record offsets, disk counts, record length (including extensible data), version-needed floor, entry counts and central directory bounds are validated; nonsentinel ZIP32 fields must agree with ZIP64 values. The layout exposes archive size, directory offset/size/count and end-structure offset. Empty ZIP end metadata is structurally accepted; dictionary membership validation must still reject missing required files.

This helper does not validate central/local entries, duplicate/unsafe paths, data descriptors, member overlap, payload CRC or dictionary semantics. Those remain required before the existing extractor receives a span. Source content must be stable under the serialized owner; length checks alone cannot detect same-length edits. Every read checks the progress callback; live callers must provide cancellation/deadline/yield behavior through the callback or storage adapter. Small scratch increases the number of directory-tail reads; no performance gain is claimed.

Six tests cover comment lengths through 65,535, every bank width 64–512, ZIP64 sentinel/nonsentinel agreement, extensible records, empty end metadata, malformed locator/record/disk/version/count/overflow cases, exact comment length, all read failures, cancellation, entry limits and source-size changes. All six passed after final edits. Strict standalone host compilation passed with 128-byte validation and 80-byte parsing frames. Reproduce with `ctest --test-dir /tmp/lila-companion-tests -R ZipDirectoryLayoutTest --output-on-failure`. This new helper remains unused by compiled firmware; no firmware build was repeated. Complete entry/path validation and recoverable dictionary installation remain unfinished.

### Streaming decoded ZIP path grammar

`ZipPathValidation` consumes decoded UTF-8 name bytes incrementally without allocating a string or path buffer. It enforces the Apple validator's 1,024-byte limit, relative paths, nonempty components, no exact `.`/`..` components, and no ASCII controls/DEL, backslash or colon. UTF-8 sequences may cross any input boundary; overlong encodings, surrogate code points, values beyond U+10FFFF, stray continuations and truncated sequences fail. A directory may trim one trailing slash; files may not. Raw/trimmed byte lengths are assigned only after a successful finish. Failure and completion are sticky until reset.

This is grammar validation, not a canonical name key. ZIP entry encodings must first be decoded to UTF-8, including supported legacy names when the UTF-8 flag is absent. NFC normalization, archive-wide duplicate identity, FAT case ambiguity, symlink/type checks and central/local name agreement remain required separately. The helper preserves decomposed/composed spellings; it does not establish parity with Apple's normalization or make an arbitrary name safe for direct extraction. The installer continues to require controlled private member paths.

Six tests cover valid ASCII/Unicode names across every chunk width, scalar boundary values, both directory modes, traversal/absolute/control/separator rejection, malformed and split UTF-8, byte limits, sticky failure/completion and reset. All six passed after the final edits. Strict standalone host compilation passed; the optimized wrapper uses borrowed state and has no local stack frame in that host build. This does not measure ESP stack or prove live resource limits. Reproduce with `ctest --test-dir /tmp/lila-companion-tests -R ZipPathValidationTest --output-on-failure`. The helper remains unused by compiled firmware, so no firmware build was repeated. Complete central/local entry validation, name decoding/normalization, recoverable installation and live inventory dispatch remain unfinished.

### Bounded ZIP extra-field validation

`CompanionZipExtraValidation.h` checks the complete extra-field framing and expands ZIP64 sentinel values in the fixed conditional central-directory order. Local ZIP64 records require both size fields; non-sentinel local sizes must agree with their extended values. Duplicate ZIP64 fields, missing required values, truncated records, out-of-file spans, read failures, cancellation and changed source extent fail without modifying the output. Unknown fields are skipped after checking their declared bounds. The field ordering follows [PKWARE APPNOTE section 4.5.3](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT).

The parser borrows storage and at least 28 scratch bytes, allocates no heap memory, and requires exclusive immutable-source ownership. Four host tests cover all 16 central sentinel combinations and the listed failure paths. The optimized host compiler reports a 208-byte validation frame; this is not a target stack measurement. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionZipExtraValidationTest` and `ctest --test-dir /tmp/lila-companion-tests -R ZipExtraValidationTest --output-on-failure` after configuring the host test tree. This helper is not yet used by live firmware. Central/local header comparison, streaming descriptor placeholders, filename decoding and archive-wide overlap/duplicate checks remain integration work.

### Bounded ZIP data descriptors

`CompanionZipDescriptorValidation.h` compares a descriptor immediately after the checked compressed payload against central-directory CRC and size values. It accepts 32-bit or ZIP64 sizes and either signature form, following [PKWARE APPNOTE section 4.3.9](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT). ZIP64 width is supplied from per-entry extended metadata, not inferred from small sizes. A CRC equal to the descriptor signature is checked in both interpretations; ambiguous matching interpretations are rejected because they imply different member ownership ranges.

The caller supplies an upper boundary before the central directory. Reads stay within that boundary and the actual source extent. The helper borrows 24 scratch bytes and adds no heap allocation. Failed reads, mismatches, cancellation or changed source extent leave the returned end offset untouched. Four host tests cover signed/unsigned forms at both widths, signature-valued CRCs, every descriptor byte corruption and truncation, read failures, bounds, cancellation and ambiguous ownership. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionZipDescriptorValidationTest` followed by `ctest --test-dir /tmp/lila-companion-tests -R ZipDescriptorValidationTest --output-on-failure`. The optimized host frame is 128 bytes; physical target stack use remains unverified. Central/local header composition, archive-wide range checks and live installation remain incomplete.

### Composed ZIP entry metadata checks

`CompanionZipEntryMetadataValidation.h` combines checked central-directory records, local headers, extra fields and descriptors into a payload span. It compares raw names in bounded banks, requires matching methods/flags/extraction versions, checks CRC and sizes (allowing zero placeholders when bit 3 is set), rejects unsupported methods/flags and Unix special files, and bounds local metadata, compressed payload and descriptors before the central directory. ZIP64 metadata is handled per entry, including local ZIP64 headers whose central sizes fit in 32 bits. Expanded members are capped at the extractor's 256 MiB limit. Output is assigned only after all checks and an unchanged source-size check succeed.

The validator owns fixed working state and must live in the session rather than a function-local firmware stack. Scratch and source are borrowed; there are no new heap allocations or filename buffers. Exclusive source ownership must prevent same-length modifications. Four host tests cover the canonical four-member dictionary bundle, local metadata/name corruption, missing ZIP64 sizes, every read failure, cancellation, and Python-generated seekable/streamed DEFLATE archives with local ZIP64 headers. The optimized host metadata frame is 128 bytes and the composed extra-field frame is 224 bytes; these are individual host frames, not a measurement of total target stack depth. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionZipEntryMetadataValidationTest` and `ctest --test-dir /tmp/lila-companion-tests -R ZipEntryMetadataTest --output-on-failure`.

The returned name is still a raw archive span. Legacy decoding, Unicode normalization, path safety, all-entry count/coverage, duplicate names and overlapping member ranges require archive-wide validation before any installation. These helpers are not yet connected to the live firmware transfer path.

### Complete central-record and payload sweep

`CompanionZipArchiveValidation.h` composes end-directory parsing, per-entry metadata checks and streamed extraction to a fixed discard sink. It validates every declared entry's payload length and CRC. The final central-record cursor must reach the declared directory end, optionally followed by exactly one bounded digital-signature record (signature 0x05054b50 and a 16-bit payload length). Signature contents are opaque; their authenticity is not verified. This framing follows [PKWARE APPNOTE sections 4.3.12–4.3.13](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT). An underreported entry count therefore cannot accept a valid prefix while leaving unchecked records. Successful output is assigned only after the complete sweep and a final source extent/cancellation check.

The session-owned validator allocates no heap memory. It borrows scratch, a miniz decoder and a 32 KiB history window for DEFLATE; expanded bytes are checked and discarded in bounded banks. This costs a complete payload read/decompression pass before installation; no performance improvement is claimed. The same buffers can be reused during later extraction. Immutable source ownership remains required. Individual optimized host frames are 96 bytes for the sweep and at most 224 bytes among its validation helpers; total target stack depth remains unmeasured.

Four host tests cover the canonical dictionary archive, real seekable/streamed ZIP64 DEFLATE archives, underreported entry counts, payload corruption, every read failure and every cancellation point, with successful retry after cancellation. Optional signature framing is tested with zero, one and 65,535 payload bytes; incorrect signatures and declared lengths are rejected without changing output. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionZipArchiveValidationTest` and `ctest --test-dir /tmp/lila-companion-tests -R ZipArchiveTest --output-on-failure`. This validates metadata/payload coverage, not installability: raw-name decoding, normalized path safety, duplicate names, overlapping local ranges, dictionary member selection and recoverable installation are still required. Empty archives can pass structural validation and must be rejected by dictionary selection. Live firmware does not yet construct this helper.

### Disk-backed local-range overlap audit

`CompanionZipRangeValidation.h` stages each checked local-header-to-descriptor-end interval as a 16-byte little-endian record, heapsorts records in place through a borrowed storage provider, and rejects duplicates, nested/crossing intervals, invalid bounds and count overflow. Adjacent ranges are accepted. Only two fixed records are retained in memory; sorting adds no heap allocation. The disk stage uses 16 bytes per entry (320,000 bytes at the 20,000-entry limit). Random SD I/O during heapsort is an explicit cost, not a performance improvement. Storage implementations must use reusable HAL handles and private session-owned stages.

`ZipArchiveValidation::validate(output, &ranges)` collects every checked entry and requires the audit to finish. Failure invalidates the audit so a partial entry set cannot later be sealed as a successful result. The overload without an audit remains metadata/payload validation only. Installation must supply an audit. `HalZipRangeStorage` supplies the HAL-backed stage; the live installer does not yet construct these helpers. The owner retains responsibility for cleaning the private stage on cancellation/failure. A sealed range stage is diagnostic validation data, not an installed archive or a durable commit receipt.

Nine selected host tests pass across the archive and range targets, including an archive with a duplicate central entry whose metadata and payload CRC checks pass but whose local ownership overlaps. Range tests cover descending intervals, adjacency, duplicates/nesting/crossing, count/bounds, every storage failure and cancellation, and retry. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionZipRangeValidationTest CompanionZipArchiveValidationTest` and `ctest --test-dir /tmp/lila-companion-tests -R 'ZipRangeTest|ZipArchiveTest' --output-on-failure`. Filename decoding/normalization, duplicate destination names, dictionary selection and recoverable installation remain incomplete.

### HAL-backed range staging

`HalZipRangeStorage.h` implements the range storage through one reusable `HalFile`, two retained checked-presence handles, and a 16-byte readback buffer. HAL allocates its opaque handle state once per handle; these allocations are required by the mutex-owning HAL file abstraction and are reused across resets rather than repeated inside the sorting loop. The stage is capped at 320,000 bytes, supports aligned 16-byte appends/overwrites, and verifies every write through exact readback. It yields every 32 read/write operations. Seal checks truncate, sync, file size and close; it does not publish content.

The private `/.crosspoint/companion/zip-ranges-next` path must be absent according to checked directory enumeration before creation. A pre-existing file or directory is preserved. The provider owns created stages, including sealed stages, and removes only its owned file on discard/destruction. Cleanup failures retain ownership for retry. Startup recovery must explicitly handle a stage left by an interrupted session before live installation is enabled; a new provider never assumes ownership of such a file automatically.

Four HAL host tests pass for sorting/yields, exactly three retained handle preparations across resets, unknown-file/directory preservation, write corruption, write/sync/truncate/close failure and cleanup retry. Verify with `cmake --build /tmp/lila-companion-tests --target HalZipRangeStorageTest` and `ctest --test-dir /tmp/lila-companion-tests -R HalZipRangeTest --output-on-failure`. Optimized host frames are 128 bytes for reset, 96 for write, 64 for seal and 144 for discard; actual reader stack depth and SD performance remain unverified. No live firmware owner includes this new header yet.

The HAL range test now also composes `HalInventoryFileView`, `ZipArchiveValidation`, `ZipRangeValidation` and `HalZipRangeStorage` against the file-backed HAL fake. Canonical dictionary and streamed ZIP64 archives pass; corrupt range-stage writes leave the output unchanged, invalidate the partial audit, and allow cleanup followed by successful retry. Original archive bytes remain unchanged. All five selected HAL range tests pass. This verifies the real provider/helper composition under host fakes; it does not establish physical SD behavior or live activity/recovery integration. The borrowed file view snapshots length at attachment, so exclusive immutable-source ownership remains mandatory.

### Legacy ZIP filename conversion

`CompanionZipLegacyName.h` converts CP437 bytes to UTF-8 using a 256-byte static constexpr high-byte table and borrowed output storage. ASCII/control bytes retain their exact scalar values so path grammar rejects controls rather than converting them into display glyphs. Input and output must not overlap. Conversion needs at most three output bytes per input byte, checks capacity before writing anything, leaves output/count unchanged on insufficient capacity, and adds no heap allocation or filename-sized stack buffer. It neither appends a NUL nor normalizes Unicode.

The shared full-byte UTF-8 fixture was checked against all 256 entries of the [Unicode Consortium's published CP437 mapping](https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/PC/CP437.TXT). Three host tests verify the complete mapping, exact capacity behavior, accented paths and retained unsafe controls. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionZipLegacyNameTest` and `ctest --test-dir /tmp/lila-companion-tests -R ZipLegacyNameTest --output-on-failure`. The optimized host decoder frame is zero bytes; this is not target measurement. Selecting CP437 versus declared UTF-8/Unicode extras, NFC normalization, duplicate destination names and wiring decoded paths into the archive sweep remain unfinished.

### Streaming header-name grammar

`CompanionZipHeaderNameValidation.h` reads a checked raw filename span in bounded banks, consumes declared UTF-8 directly or converts CP437 into a separate borrowed scratch bank, and feeds decoded bytes into `ZipPathValidation`. The 1024-byte limit applies to decoded UTF-8 bytes, matching the Apple path grammar. Scratch must contain at least four bytes; CP437 uses one quarter for input and the remainder for up-to-threefold expansion. No heap allocation or filename-sized local buffer is added. Output is assigned only after grammar, extent and cancellation checks succeed.

Three host tests pass for all scratch widths from four through 64 bytes, split UTF-8, CP437 accents, expansion limits, unsafe/control/traversal names, directory trailing slash handling, every read failure, cancellation and retry. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionZipHeaderNameValidationTest` and `ctest --test-dir /tmp/lila-companion-tests -R ZipHeaderNameTest --output-on-failure`. The optimized host frame is 144 bytes; target stack depth remains unverified. This checks the header name only. Unicode path extra-field selection/CRC, NFC normalization, duplicate names and live sweep/installation integration remain unfinished; callers must not treat it as the complete destination-name proof.

### Header paths integrated into the archive sweep

`ZipArchiveValidation` now runs `ZipHeaderNameValidation` for every checked central name before payload extraction, selecting UTF-8 when bit 11 is set and CP437 otherwise. This matches the encoding-selection mechanism in pinned ZIPFoundation `Entry.path`; that implementation does not select Unicode path extra-field overrides. `ZipEntryMetadataValidation` recognizes raw trailing-slash directories in addition to supported external attributes, and rejects nonempty expanded directory payloads. NFC and normalized duplicate destination detection remain mandatory unfinished work.

All 15 selected metadata/archive/HAL range tests pass after composition, including matching unsafe names in local and central records, every archive read/cancellation point, canonical/ZIP64 archives and the HAL pipeline. Run `ctest --test-dir /tmp/lila-companion-tests -R 'ZipArchiveTest|ZipEntryMetadataTest|HalZipRangeTest' --output-on-failure` after building those targets. Optimized host frames are 128 bytes for the composed sweep and 144 for header-name validation, not total target stack measurements. No live firmware owner constructs the sweep yet.

CP437 control-byte policy is not yet identical across platforms: the reader preserves ASCII controls and rejects them as unsafe, whereas pinned ZIPFoundation's CP437 lookup maps bytes 1–31 and 127 to display symbols. Resolve the Apple import acceptance policy explicitly before enabling dictionary installation; do not claim complete path parity from the common encoding flag alone.

### Explicit Apple header decoder

`ZipHeaderName.swift` adds strict UTF-8/CP437 decoding using the same published mapping and decoded-byte limit as the reader. Its scalar builder reserves input capacity on the Apple host; this allocation does not affect reader RAM. Three tests verify all 256 bytes against the shared reader fixture, malformed UTF-8 and expansion limits, and control/traversal rejection through the existing canonical path validator. All 209 CompanionKit tests pass on Linux (`swift test --package-path apple/CompanionKit`). Native Apple SDK validation remains outstanding.

Source inspection qualifies the earlier CP437 observation: ZIPFoundation's custom lookup is Linux-only; native Apple builds use Foundation's codepage decoder. The Linux table also differs at byte 0xf4. The explicit decoder avoids depending on either OS implementation, but the importer still calls `Entry.path`. Wiring requires checked access to central raw filename bytes and bit 11, which ZIPFoundation does not expose publicly. Complete importer/reader name parity therefore remains unproven until that parsing/integration is implemented and tested.

`ArchiveValidator` now reads central filenames and bit 11 itself through a bounded directory cursor and invokes `ZipHeaderName.decode`, removing `Entry.path` from its path acceptance/duplicate checks. End-directory parsing supplies checked ZIP32/ZIP64 offset, size and count, including sentinel combinations and agreement with non-sentinel values. The cursor must cover exactly the declared directory, optionally followed by one framed digital-signature record. A final file-size check runs before success; immutable vault-stage ownership still prevents same-length concurrent modification.

Two additional archive-level regressions verify that CP437 byte 0xf4 produces U+2320 even under the Linux ZIPFoundation fallback, and raw control bytes 1, 31 and 127 fail path acceptance. All 211 CompanionKit tests pass on Linux after the integration. Native Apple builds remain unverified. Format-specific inspectors still use ZIPFoundation entry paths to select/extract members and must be reconciled with these explicit names before complete cross-platform import parity can be claimed. Reader NFC/normalized duplicate checking and recoverable installation remain incomplete.

### Canonical Apple member selection

`ArchiveValidator.entries` builds an internal normalized-name-to-`Entry` map during the complete validation scan. The map is returned only after payload and directory-coverage validation succeeds. Dictionary and EPUB inspectors use this map instead of ZIPFoundation's OS-dependent path subscript. EPUB rootfile and resolved resource paths use the same canonical normalization before lookup. Entry objects retain the archive's checked metadata for extraction; source files must remain immutable under vault-stage ownership while the inspectors reopen them.

The map is host-side storage with reserved initial capacity and bounded by archive limits; it adds no reader allocation. A dictionary regression uses decomposed Unicode member names and verifies successful lookup through NFC names, and the legacy-byte regression checks canonical entry selection for CP437 byte 0xf4. All 212 CompanionKit tests pass on Linux; run `swift test --package-path apple/CompanionKit`. Native Apple SDK verification remains pending. Reader NFC/duplicate destination proof, recoverable dictionary installation and the other companion-plan requirements are still incomplete.

EPUB canonical lookup has dedicated coverage: a decomposed package filename resolves from container XML, and a decomposed chapter filename resolves from a percent-encoded composed manifest URL with a fragment. Canonically equivalent composed/decomposed manifest URLs are rejected as duplicate resources. All 214 CompanionKit tests pass on Linux (`swift test --package-path apple/CompanionKit`). This verifies normalized EPUB metadata/resource selection; native Apple execution and the firmware-side normalization/install path remain outstanding.

### Reader aggregate decompression budget

`ZipArchiveValidation` now defaults to the same 1 GiB aggregate expanded-byte ceiling as Apple `ArchiveLimits`, in addition to the existing 256 MiB per-entry ceiling. A trailing constructor argument permits a smaller caller-selected total. The running sum uses subtraction-based bounds before extraction, so neither wraparound nor a limit-exceeding entry reaches decompression. Only a fixed 64-bit counter/limit is added; no buffer or heap allocation changes. A zero total permits empty payloads but rejects nonempty members.

The boundary regression accepts exactly the fixture's expanded sum, rejects one byte below it and zero, preserves failed output and supports successful retry. All 12 selected archive/HAL range tests pass; verify with `ctest --test-dir /tmp/lila-companion-tests -R 'ZipArchiveTest|HalZipRangeTest' --output-on-failure` after building those targets. The optimized host sweep frame remains 128 bytes. Native reader integration, NFC/duplicate destination checks and recoverable installation remain pending.

### Canonical Unicode data generation

`scripts/gen_companion_unicode.py` produces `CompanionUnicodeData.inc` from pinned Unicode 15.1.0 properties. It emits fully expanded canonical decompositions, nonzero combining classes and sorted composition pairs, excluding compatibility mappings and composition exclusions. Hangul is omitted for algorithmic normalization. Tables are inline constexpr constants; no runtime table construction or heap allocation is introduced. Do not edit the generated data manually. Regenerate with `python3 scripts/gen_companion_unicode.py lib/Companion/CompanionUnicodeData.inc`, or verify byte-exact reproducibility with the same command plus `--check`; a different Unicode database version fails explicitly.

The generator's bounds/uniqueness assertions, deterministic regeneration check and standalone C++20 compilation pass. This is data preparation, not an NFC validator. Canonical ordering, blocked composition, bounded scalar/output storage, Unicode conformance tests and integration with archive duplicate detection remain required. The normalization process follows [Unicode Standard Annex #15](https://www.unicode.org/reports/tr15/); parity for newer Unicode characters on native Apple systems must also be addressed before complete cross-platform equivalence is claimed.

### Caller-buffer scalar NFC

`CompanionUnicodeNfc.h` implements Unicode 15.1 canonical decomposition, stable combining-class ordering, blocked canonical composition and algorithmic Hangul normalization. It consumes valid scalar input and writes through a borrowed scalar workspace without heap allocation or recursion. Input/workspace must not overlap. Failed capacity or invalid-scalar checks leave the returned count unchanged; workspace contents are disposable on failure. Tables are constant data, and individual optimized host frames are 144 bytes for normalization and 64 for ordered append; target stack depth and linked table placement remain unmeasured.

Both host tests pass, including all five NFC invariants for every official Unicode 15.1 normalization test row, plus empty input, insufficient workspace and invalid scalars. The official fixture retains its Unicode notices, with license text in `protocol/fixtures/Unicode-LICENSE.txt`. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionUnicodeNfcTest` and `ctest --test-dir /tmp/lila-companion-tests -R UnicodeNfcTest --output-on-failure`. This is scalar NFC conformance for the pinned version, not a completed ZIP name pipeline. Bounded UTF-8/scalar adaptation, normalized-name storage/sorting, version parity on Apple and live installation remain unfinished.

### Strict UTF-8 NFC adapter

`CompanionUnicodeUtf8Nfc.h` decodes strict UTF-8 into a borrowed scalar bank, invokes scalar NFC into a second borrowed bank, checks the exact encoded size, then writes UTF-8 into caller-owned output. All spans must be disjoint. Invalid encodings, insufficient scalar storage and insufficient output capacity leave UTF-8 output/count unchanged; scalar scratch remains disposable. It appends no NUL and allocates no heap memory. The owner must reserve scalar expansion capacity and may reuse decoder-window storage during the filename phase; no live owner currently does so.

Three host tests pass for accents, canonical ordering, singleton decomposition, Hangul, supplementary scalars, malformed/truncated/overlong UTF-8, surrogates/out-of-range values, all buffer failures, empty input and retry. A composition-excluded U+0344 regression verifies that NFC can increase output length and requires capacity for four bytes from two input bytes. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionUnicodeUtf8NfcTest` and `ctest --test-dir /tmp/lila-companion-tests -R UnicodeUtf8NfcTest --output-on-failure`. The optimized adapter host frame is 64 bytes, excluding called normalizer frames; target depth remains unverified. ZIP decoding-to-normalization wiring, normalized-name persistence/sorting and duplicate checks remain unfinished.

### Composed decoded/NFC ZIP names

`CompanionZipNameNormalization.h` composes UTF-8/CP437 header decoding, strict decoded path grammar, directory trailing-slash trimming and Unicode 15.1 NFC into caller-owned output. All raw/decoded/scalar/output spans must be disjoint. The decoded-byte limit is checked before normalization, matching Apple's acceptance order; normalized output can exceed 1024 bytes. Failed calls preserve normalized output/count, while scratch banks are disposable. No heap allocation or filename-sized local array is introduced. Owners must provide adequately aligned scalar banks outside task stacks and can reuse decoder-window memory during this phase.

Three host tests pass for cross-encoding canonical identity, decomposed accents, Hangul directories, unsafe paths, capacity preservation, exact decoded limits and a 1024-byte combining sequence whose NFC output is 2048 bytes. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionZipNameNormalizationTest` and `ctest --test-dir /tmp/lila-companion-tests -R ZipNameNormalizationTest --output-on-failure`. Disk-backed normalized-name indexing/duplicate checks, buffer ownership in a live session, cross-version Apple normalization parity and recoverable installation remain unfinished.

### Disk-backed normalized-name duplicate validation

`CompanionZipNameDuplicateValidation.h` appends already validated/NFC-normalized names to borrowed byte storage and writes 16-byte offset/length references through `ZipRangeStorage`. It heapsorts references lexicographically using two borrowed comparison banks and rejects equal adjacent names. Prefixes and distinct case-sensitive names remain distinct, matching Apple's normalized exact-name duplicate rule. It checks reference bounds even for a one-entry archive. No archive-sized name set or heap allocation is introduced; the session owns two fixed reference records plus the borrowed scratch. Random storage reads during sorting are an explicit I/O cost.

Four host tests pass for arbitrary order, prefix/case distinctions, names sharing 2048-byte prefixes across every comparison-bank width, canonical duplicate bytes, every index/name storage failure, cancellation, invalid single references and entry limits. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionZipNameDuplicateValidationTest` and `ctest --test-dir /tmp/lila-companion-tests -R ZipNameDuplicateTest --output-on-failure`. Optimized host frames are 96 bytes for compare/sift and 64 for finish; target depth remains unverified. Inputs must be normalized before adding, and partial or singly sealed private stages are not commit receipts. HAL name-byte/index providers need distinct private paths from the local-range audit, ownership-safe cleanup/recovery, and live archive composition. Those integrations and FAT destination case ambiguity remain unfinished.

### Independent HAL name-reference stage

`HalZipRangeStorage` now has a fixed `Purpose` selection: `Ranges` retains `zip-ranges-next`, while `NameIndex` uses `zip-names-index-next` under the companion private directory. Both store aligned 16-byte records with readback and reuse the same retained HAL handles. Paths are static constants selected at construction, not caller-borrowed dynamic strings. Ownership/cleanup checks apply independently, so the two stages can coexist during archive validation.

All seven HAL stage tests pass, including simultaneous creation/sealing, independent cleanup and preservation of a pre-existing name index while a range stage is created/discarded. Verify with `cmake --build /tmp/lila-companion-tests --target HalZipRangeStorageTest` and `ctest --test-dir /tmp/lila-companion-tests -R HalZipRangeTest --output-on-failure`. The normalized-name byte provider and full normalization/duplicate composition remain unfinished. Live startup recovery must handle either path left by an interrupted session before installation is enabled; a newly constructed provider still refuses to claim existing stages.

### HAL normalized-name byte provider

`HalZipNameBytesStorage.h` supplies an append-only private `zip-names-bytes-next` file for the duplicate validator. Appends are contiguous, capped at 3072 bytes per normalized name, and read back through a fixed 16-byte bank before the extent advances. The whole stage is capped at 61,440,000 SD bytes (20,000 maximum-sized names), not RAM. Reads are bounded; sealing checks truncate/sync/size/close. Every 32 read/readback operations yields. The provider preserves pre-existing files/directories and removes only its owned private stage, including after seal; cleanup failure retains ownership for retry.

Three retained HAL handles are required for the mutex-owning file and checked presence enumeration. Their opaque states are allocated once and reused; no per-name allocation occurs. Together with the independent name-index provider there are six retained handles. Four host tests pass for their real composition, canonical duplicate rejection, six preparations across resets, long append/readback/yields, silent corruption, unknown-file preservation, cleanup retry and sync/truncate/close failure. Verify with `cmake --build /tmp/lila-companion-tests --target HalZipNameBytesStorageTest` and `ctest --test-dir /tmp/lila-companion-tests -R HalZipNamesTest --output-on-failure`. Optimized host frames are 128 bytes for reset/append, 64 for seal and 144 for discard, not physical target measurements. Startup stage recovery, full archive/normalizer composition and live installation remain unfinished.

### Normalized-name audit composed with archive validation

`CompanionZipNameAudit.h` supplies an optional lifecycle interface to `ZipArchiveValidation::validate(output, &ranges, &names)`. `CompanionZipNormalizedNamesValidation.h` implements it by reading each checked raw header name, decoding/validating/trimming/NFC-normalizing through borrowed workspace, and adding canonical bytes to the disk-backed duplicate validator. Both audits must finish before archive success. The sweep's guard invalidates both audits on every exit; sealed private validation files are not installation commit receipts. Metadata-only overloads still omit those proofs, so installation must explicitly supply both audits.

Two HAL-composed tests pass: composed/decomposed names in distinct valid local records fail normalized duplicate checking while preserving the result; cleanup then permits unique and canonical dictionary archives. Ten retained handle preparations (nine stage handles plus the borrowed archive handle) are reused across repeated scans. A second test injects cancellation at every callback across the full sweep/range/name pipeline, verifies unchanged output and invalidated audits, cleans up and retries successfully without modifying the source archive. Verify with `cmake --build /tmp/lila-companion-tests --target HalZipNormalizedArchiveTest` and `ctest --test-dir /tmp/lila-companion-tests -R HalZipNormalizedTest --output-on-failure`.

Workspace remains caller-owned, disjoint and aligned. Conservative banks of 1024 raw bytes, 3072 decoded bytes, 3072 output bytes, 1024 input scalars and 4096 normalized scalars total 27,648 bytes and can fit within an aligned 32 KiB decoder window during the filename phase; no live owner implements that reuse yet. Normalization and extraction must not overlap. Unicode-version parity on Apple, physical SD/stack measurements, recovery of abandoned validation stages, dictionary member selection and recoverable installation remain unfinished.

### Aligned decoder-window reuse for names

`ZipNameWorkspace::borrowDecoderWindow` now partitions a caller-owned span of at least 8192 aligned `uint32_t` objects into the conservative filename banks and an exact 32 KiB decoder byte view. It never casts a byte pointer to a wider pointer: scalar subspans refer to existing scalar objects, while DEFLATE accesses their byte representations. Insufficient capacity/alignment leaves returned views unchanged. Partitioning allocates nothing. This allows name normalization and extraction to use the same storage in serialized phases, avoiding a separate 27,648-byte name workspace; it does not reduce the existing 32 KiB DEFLATE history requirement.

All three HAL-composed tests pass using that shared storage, including streamed ZIP64, repeated normalization/DEFLATE transitions across three 120,000-byte members, normalized duplicate rejection and every composed cancellation point. The phase-switch fixture verifies archive structure/CRC, not StarDict member semantics. Workspace tests check exact view sizes, scalar offsets and insufficient-capacity preservation. Verify with `cmake --build /tmp/lila-companion-tests --target HalZipNormalizedArchiveTest` and `ctest --test-dir /tmp/lila-companion-tests -R HalZipNormalizedTest --output-on-failure`. The optimized partition helper host frame is zero bytes; physical target stack/heap checks remain outstanding. A live owner must allocate the aligned backing store once with nothrow handling and prevent simultaneous decoder/normalizer access. Live owner construction, startup recovery and recoverable dictionary installation remain unfinished.

### Dictionary ZIP member selection

`CompanionDictionaryZipSelection.h` selects StarDict payload spans in two passes over validated, normalized names. The first pass requires exactly one file ending in `.ifo`, capped at 64 KiB expanded, and retains its base path. The second finds matching `.idx`, `.dict` or `.dict.dz`, and optional `.syn`; plain definitions take precedence. Required members and present synonyms must be files. A changed `.ifo` payload descriptor between passes fails. The session owns one 3072-byte base-path bank and five descriptors, with no per-entry allocation or archive-sized member table. Keep the selector outside task stacks. Restart clears all descriptors, including optional synonyms from a previous archive.

Four host tests pass, covering all 120 member orders, plain-definition preference, compressed fallback, invalid/missing headers and members, changed header descriptors, unchanged output on failure, and reuse without stale synonyms. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionDictionaryZipSelectionTest` and `ctest --test-dir /tmp/lila-companion-tests -R DictionaryZipSelectionTest --output-on-failure`. Selection requires whole-archive normalized duplicate validation and exclusive stable source ownership; it does not prove payload integrity or StarDict semantics. Connecting both selection passes to the archive owner, staged extraction, semantic validation, recoverable member installation and physical-device checks remain unfinished.

### Normalized-name visitor and dictionary composition

`ZipNormalizedNamesValidation` accepts an optional function-pointer visitor with borrowed context. It receives the normalized name and checked entry metadata after the name is appended to the duplicate index. The name view expires on return; visitor results remain provisional until the enclosing archive sweep succeeds, including final duplicate/range checks and all payload CRC checks. Visitor rejection aborts the name audit and fails the archive without changing its output. The callback introduces no allocation or retained filename copy.

All four HAL-composed archive tests pass. The added dictionary test runs both selector passes through complete archive validation over the real plain StarDict ZIP fixture, selects required members and synonyms, verifies visitor rejection propagates without a successful layout, and retries after reset. Existing tests still cover normalized duplicates, every composed cancellation point, shared decoder storage and repeated DEFLATE transitions. Verify with `cmake --build /tmp/lila-companion-tests --target HalZipNormalizedArchiveTest` and `ctest --test-dir /tmp/lila-companion-tests -R HalZipNormalizedTest --output-on-failure`. The second pass currently repeats full archive validation, including payload reads. This proves host composition only; a live session owner, staged member extraction, semantic checks, recovery and target resource verification remain unfinished.

### Independent dictionary member stages

`HalZipEntryStage::extractMember` assigns fixed private candidates for definitions, index, info and synonyms. One sink reuses its three retained HAL handles across all four extractions, so sealed members can coexist without four sink instances or per-member handle allocations. Candidate names are static strings, never archive-controlled paths. The caller owns sealed files; failed or partial extraction removes only owned partial bytes. A new extraction invalidates the prior success receipt even when rejected before sink startup. Existing files and directories remain protected.

All seven member-stage host tests pass, including four coexisting files with only three handle preparations, existing-candidate refusal, invalid roles/extents clearing the success receipt, actual DEFLATE, readback corruption, cancellation and storage faults. Verify with `cmake --build /tmp/lila-companion-tests --target HalZipEntryStageTest` and `ctest --test-dir /tmp/lila-companion-tests -R HalZipEntryStageTest --output-on-failure`. These private candidates are not installed dictionaries. A session/parent journal must track sealed candidates and recover them after interruption before enabling live extraction; semantic validation and recoverable publication remain unfinished.

### Validated selection through HAL extraction

The HAL-composed dictionary test now carries both the plain and dictzip StarDict fixtures through two complete normalized archive sweeps, member selection and four independent verified member stages using one reusable sink. It compares every staged byte against the selected archive payload. ZIP extraction retains dictzip bytes for subsequent dictzip/StarDict semantic validation; selecting `.dict.dz` does not decompress its inner format. Injecting corruption into a later info payload fails CRC validation, removes that partial candidate, preserves earlier sealed members, and allows retry after source restoration. Source mutation here is a fault injection, not a supported live ownership mode.

All four composed host tests pass. Verify with `cmake --build /tmp/lila-companion-tests --target HalZipNormalizedArchiveTest` and `ctest --test-dir /tmp/lila-companion-tests -R HalZipNormalizedTest --output-on-failure`. This supplies integration evidence for existing helpers; production session ownership, staged-source semantic validation, parent-journal recovery, installation and physical-device verification are still required.

### Dictionary extraction coordinator

`HalDictionaryZipExtraction.h` owns fixed session state for the selected-member extraction sequence: info, index, definitions and optional synonyms. It borrows the existing extractor and reusable HAL sink, retains four SHA-256 digests and a sealed-member mask, and reports complete success only after every required extraction and optional receipt callback succeeds. The callback runs after sealing and before the next member begins, providing the parent installer a persistence boundary. Callback failure retains the sealed file and marks it in the current attempt's mask; this is not an installation commit. Starting another attempt clears in-memory receipts and refuses pre-existing candidates through the sink, so callers must retain their own durable ownership record. No new heap allocation or member-sized local buffer is introduced; keep the coordinator in the session owner.

All twelve selected member-stage and HAL archive-composition tests pass. The coordinator now drives staged extraction for both real dictionary fixtures. A new test rejects the first receipt callback, verifies extraction stops with only sealed info retained, prevents retry from claiming that file, and verifies a fresh extraction without synonyms after authorized cleanup. Verify with `cmake --build /tmp/lila-companion-tests --target HalZipEntryStageTest HalZipNormalizedArchiveTest` and `ctest --test-dir /tmp/lila-companion-tests -R 'HalZipEntryStageTest|HalZipNormalizedTest' --output-on-failure`. Durable parent receipts, staged StarDict validation, installation/recovery and physical target checks remain unfinished.

### Receipt-verified staged dictionary source

`HalDictionaryStagedSource.h` opens fixed member candidates only after a complete extraction receipt, checks each selected length and rehashes its bytes against the retained SHA-256 digest, then implements the existing dictionary source interface. Four HAL handles are retained and reused between opens; these states are required for the validator's cross-member reads and are not allocated per read. Hash scratch is borrowed. All I/O stays under HAL locking, reads are bounded and yield every 32 operations, and close failure invalidates the source. Opening does not claim cleanup ownership of sealed files. The parent must prevent same-length mutation throughout validation/build; verification at open is not a substitute for exclusive ownership.

All four HAL composition tests pass with both plain and dictzip bundles carried through archive validation, selection, extraction, receipt verification and full StarDict member validation (including inner dictzip validation). Tests also reject same-length staged corruption, length changes, read failures and cancellation, and verify close failure leaves the source unusable until cleanup retries. Verify with `cmake --build /tmp/lila-companion-tests --target HalZipNormalizedArchiveTest` and `ctest --test-dir /tmp/lila-companion-tests -R HalZipNormalizedTest --output-on-failure`. Production ownership, durable receipts, recoverable installation and device stack/heap checks remain unfinished.

### Dictionary extraction receipt format

`CompanionDictionaryExtractionReceipt.h` defines a 244-byte versioned `DEXR` record with revision, parent transaction, storage generation, original archive hash, four expected member lengths, four sealed SHA-256 digests, compression/synonym flags and CRC32. Only extraction-order masks 0, 4, 6, 7 and (with synonyms) 15 are accepted. Unsealed hashes must be zero; sealed hashes and parent identities must be nonzero. Info is capped at 64 KiB and each member at 256 MiB. All multi-byte decoding uses bytewise helpers rather than unaligned loads. Failed decoding preserves output and failed encoding preserves the byte buffer. The record identifies stages but does not authorize installation or claim an unrelated parent transaction.

Three host tests pass for every extraction boundary from unaligned storage, every single-byte corruption and every truncation, invalid identities/masks/hash states and length limits. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionDictionaryExtractionReceiptTest` and `ctest --test-dir /tmp/lila-companion-tests -R DictionaryExtractionReceiptTest --output-on-failure`. The decoder validates the wire fields before writing output, avoiding a temporary full receipt. Its optimized host frame is 64 bytes; target stack depth remains unverified. Redundant-slot persistence, matching recovery to the durable parent, initial ownership before stage creation and recoverable installation are still unfinished.

### Redundant dictionary extraction receipts

`CompanionDictionaryExtractionJournal.h` persists alternating `dictionary-extraction-a/b` records through `DictionaryExtractionJournalStorage` with length/CRC/content readback before exposing the new state. Initial ownership requires both paths missing and a revision-one unsealed record. Updates enforce extraction order, retain sealed hashes, skip identical duplicate receipts and reject conflicting duplicates. Recovery matches transaction, generation, archive identity, selected lengths and flags; rejects readable foreign parents, conflicting equal revisions, nonadjacent revisions and changed predecessor hashes; and selects the newest valid record. A torn slot can fall back to its valid predecessor, while unreadable slots fail closed. Every persistence failure invalidates the live state until recovery, including writes that may already have taken effect. No mutation or cleanup occurs during recovery.

Three receipt objects live in the session-owned journal rather than task-local arrays; the 244-byte wire scratch is borrowed. Six host tests pass, covering all four member boundaries, duplicate-write suppression, before/after-effect failures, read/stat errors, silent corruption, foreign ownership, invalid transitions, conflicting revisions and every truncated replacement length. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionDictionaryExtractionJournalTest` and `ctest --test-dir /tmp/lila-companion-tests -R DictionaryExtractionJournalTest --output-on-failure`. Optimized host frames are 112 bytes for recovery and 80 for persistence; target stack depth remains unverified. The parent must establish exclusive stage ownership before calling begin. HAL-composed persistence, live parent transaction checks, interrupted stage verification/cleanup, installation commit and journal retirement remain unfinished.

### HAL receipt persistence in the dictionary pipeline

The receipt journal now depends on a narrow storage interface for preparation, checked presence, read and synced write. `HalDictionaryExtractionJournalStorage.h` implements it with one retained record handle and two retained directory-lookup handles. Only the two fixed receipt paths are accepted. Presence errors remain errors, directories are preserved, and writes check truncate/sync/size/close. The three opaque HAL states are prepared once and reused across all extraction boundaries; no record-sized local byte buffer or per-boundary handle allocation is added.

Both real dictionary fixtures now persist initial ownership before member extraction, persist each sealed member through the coordinator callback, and recover all four exact hashes in a newly constructed journal before semantic validation. Eleven selected host tests pass, including portable torn-write recovery and HAL directory/sync/truncate/corruption/close failures. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionDictionaryExtractionJournalTest HalZipNormalizedArchiveTest` and `ctest --test-dir /tmp/lila-companion-tests -R 'DictionaryExtractionJournalTest|HalZipNormalizedTest' --output-on-failure`. These are host HAL fakes, not power-loss/device acceptance. Live parent-phase authorization, abandoned stage verification/cleanup, installation and receipt retirement remain unfinished.

### Parent-transfer authorization for extraction receipts

`CompanionDictionaryExtractionParent.h` wraps the receipt journal with borrowed references to the recovered transfer state, dictionary manifest and current storage generation. It rechecks transaction, nonzero owner, storage generation, archive hash, fully received length, manifest kind/version and supported parent phase before beginning/recovering/recording receipts. Fully received `Receiving` is allowed because transfer content validation precedes the durable `Installing` transition. `Verified` and `Installing` support recovery; `Committed` accepts only a complete sealed-member set and cannot append receipts. Aborted/incomplete/foreign parents fail without storage mutation and invalidate the journal's exposed state. The wrapper adds references only, with no allocation or receipt copy.

Seven journal tests and five HAL composition tests pass. The added authorization test exercises ten mismatched/incomplete parent cases, incomplete committed recovery, installation recovery, complete committed recovery and append refusal after commit. Both dictionary fixture pipelines now use the wrapper for initial ownership, every callback receipt and recovery. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionDictionaryExtractionJournalTest HalZipNormalizedArchiveTest` and `ctest --test-dir /tmp/lila-companion-tests -R 'DictionaryExtractionJournalTest|HalZipNormalizedTest' --output-on-failure`. Callers must supply state from an actually recovered durable transfer, not a fabricated descriptor, and keep ownership serialized. Wiring this into the live transfer controller, interrupted stage cleanup, installation and journal retirement remain unfinished.

### Interrupted dictionary extraction stage recovery

`HalDictionaryExtractionRecovery.h` checks all four private member paths are absent before persisting initial ownership through the authorized parent. This prevents a newly created receipt from claiming pre-existing candidates. Recovery requires an authorized current receipt, verifies all present stages before any removal, rehashes sealed members against their recorded digests/lengths, and rejects missing sealed members, directory collisions, oversized partial files, unexpected synonym stages and lookup errors. Only present unsealed files within the expected length are removed. Sealed files remain parent-owned. Cancellation and cleanup failure permit retry while receipts remain intact. Use only before member publication and with exclusive ownership; moved installation members need the later installation recovery protocol.

One record handle and two lookup handles are retained, with borrowed hash scratch and four fixed presence flags; no per-member allocation is introduced. Nine member-stage tests and five composed archive tests pass. Recovery tests cover pre-existing stage refusal before journal creation, corrupt sealed data blocking partial deletion, directory/size/cancellation failures, failed removal and successful/idempotent retry. Both full dictionary pipelines now establish initial ownership through this helper and verify recovered sealed stages. Verify with `cmake --build /tmp/lila-companion-tests --target HalZipEntryStageTest HalZipNormalizedArchiveTest` and `ctest --test-dir /tmp/lila-companion-tests -R 'HalZipEntryStageTest|HalZipNormalizedTest' --output-on-failure`. Resuming remaining extraction from recovered receipts, live controller integration, member installation and receipt retirement remain unfinished.

### Extraction resumption from recovered receipts

`HalDictionaryZipExtraction::resume` checks selected lengths/compression/synonym flags against the authorized recovered receipt, invokes complete stage verification/partial cleanup, restores recorded hashes and skips sealed members. Newly extracted members persist directly through the recovered parent rather than the original callback context, which may belong to a stopped session. Failure leaves complete success false; a full recovered receipt is verified and reconstructed without creating new member files. Scratch/decoder and sink handles remain borrowed/reused, with no allocation or archive-sized local state added.

Nine member-stage tests and five composed archive tests pass. The recovery test now resumes a three-member bundle, rejects changed selection lengths and permits repeated complete resumption. Each plain/dictzip fixture is also interrupted after each of its four durable member receipts, recovered through a new journal/parent wrapper, resumed, and fully semantically validated; the original callback count stays unchanged during resume. Normal uninterrupted completion exercises the same recovery path. Verify with `cmake --build /tmp/lila-companion-tests --target HalZipEntryStageTest HalZipNormalizedArchiveTest` and `ctest --test-dir /tmp/lila-companion-tests -R 'HalZipEntryStageTest|HalZipNormalizedTest' --output-on-failure`. Archive ownership/hash verification must precede renewed selection. Live transfer integration, recoverable member publication, original-archive binding and receipt retirement remain unfinished.

### Canonical archive construction from incoming stages

`HalDictionaryStagedArchive.h` composes receipt-verified source opening, full StarDict/dictzip semantic validation, receipt-verified reopening and canonical ZIP construction into one session-owned operation. The canonical dictionary manifest is exposed only after verified sealing; failure preserves the caller's output and clears the helper's success state. Rehashing on reopen checks that staged bytes still match extraction receipts before construction, while exclusive ownership remains required throughout building. The source retains four HAL handles and the archive stage retains three, each prepared once and reused; all scratch/decoder storage is borrowed. Validation/builder working objects reside in the coordinator, with no new explicit heap allocation or large task-local arrays.

Nine member-stage tests and five HAL archive-composition tests pass. Both real dictionary fixtures, including every interrupted receipt boundary, now build a canonical archive after resumption. Tests sweep that archive for structure/names/ranges/payload CRC, independently verify its SHA-256 manifest, preserve an existing candidate on rebuild refusal, and reject CRC-valid but semantically invalid staged members before creating any canonical candidate. Verify with `cmake --build /tmp/lila-companion-tests --target HalZipEntryStageTest HalZipNormalizedArchiveTest` and `ctest --test-dir /tmp/lila-companion-tests -R 'HalZipEntryStageTest|HalZipNormalizedTest' --output-on-failure`. A strict optimized host compile passes the 256-byte frame warning limit; physical target depth remains unverified. Immutable cache publication, original-archive retention/binding, recoverable installed-member publication and live session integration remain unfinished.

### Retaining original imported dictionary ZIPs

`HalDictionaryOriginalArchive.h` requires complete authorized member receipts and the exact parent archive manifest before copying the original ZIP into the verified canonical-candidate path and publishing it to immutable content-addressed cache storage. It checks source extent, rechecks parent authorization during copying, verifies final extent, seals with SHA readback and requires the result to match the parent's original hash before publication. A valid existing immutable copy is reused without touching an unrelated candidate. Partial copies are aborted on failure, including stage startup failure; sealed candidates remain parent/cache-owner responsibilities. Archive/member validation and exclusive source ownership are prerequisites, not supplied by retention alone.

The helper borrows source/cache/scratch and retains its stage's three HAL handles, with no per-chunk allocation. Five composed host tests pass. Each dictionary fixture now carries a valid original ZIP comment, so its canonical rebuild has a distinct identity; both canonical and exact original bytes are published and verified. Tests exercise cancellation, sync failure with partial cleanup and retry, and valid-cache reuse preserving an unrelated candidate. The optimized host retention frame is 128 bytes; target depth remains unverified. Verify with `cmake --build /tmp/lila-companion-tests --target HalZipNormalizedArchiveTest` and `ctest --test-dir /tmp/lila-companion-tests -R HalZipNormalizedTest --output-on-failure`. Installed-member publication, durable original/canonical binding, receipt retirement and live controller integration remain unfinished.

### Parent-gated original/canonical binding publication

`HalDictionaryBindingInstallation.h` checks an authorized `Installing` parent before binding publication and `Committed` before finalization. It requires the exact parent original manifest and a successful canonical build whose sealed member hashes, lengths and format flags match the parent's extraction receipt. `HalDictionaryStagedArchive` retains an independent 160-byte hash/length proof plus flags, avoiding a borrowed mutable extraction coordinator that could be reused before binding installation. This fixed session state adds no allocation. The binding wrapper borrows the existing provider and stores the two manifests; it adds no HAL handles. The lower-level binding provider still verifies both immutable archives and handles synced record publication/backups.

All 21 selected journal/member/composition tests pass. Each interrupted plain/dictzip pipeline publishes both archives, rebuilds/verifies the canonical proof, rejects receiving-phase installation and a changed original manifest without SD mutation, installs under the test's installing phase, rejects premature finalization, and reads back the finalized original/canonical association. A changed member hash cannot match the build proof. The strict optimized host archive-build compile still passes the 256-byte frame warning limit. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionDictionaryExtractionJournalTest HalZipNormalizedArchiveTest HalZipEntryStageTest` and `ctest --test-dir /tmp/lila-companion-tests -R 'DictionaryExtractionJournalTest|HalZipNormalizedTest|HalZipEntryStageTest' --output-on-failure`. Test phase changes use a synthetic parent, not a durable live transfer commit. A durable installation plan must bind the destination base path and canonical manifest, retain recovery data across member publication, and reconstruct proof after reboot. That plan, actual installed-member publication, receipt retirement and live controller wiring remain unfinished.

### Dictionary installation plan format

`CompanionDictionaryInstallationPlan.h` defines a 520-byte `DINS` version-one plan with its own revision, prepared/publishing/bound/committed phase, per-member publication mask, complete extraction receipt, original/canonical manifests, a bounded owned destination base path and CRC32. The original manifest must match the extraction archive hash. Publication masks follow info/index/definitions/synonyms order; bound/committed require all selected members. Destinations must have exactly one discoverable folder and stem under `/dictionaries` or `/.dictionaries`, with a maximum 127-byte base and zero padding. This fits the reader's 160-byte lookup buffer including its longest suffix. The codec's working plan resides in session state, not the task stack; it allocates nothing and preserves output on failed decode.

Four host tests pass for publication boundaries, owned path lifetime, every single-byte corruption/truncation, unsafe destinations, incomplete receipts, mismatched archive identity and semantically invalid phase/mask/padding/path-length fields even with a valid outer CRC. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionDictionaryInstallationPlanTest` and `ctest --test-dir /tmp/lila-companion-tests -R DictionaryInstallationPlanTest --output-on-failure`. Optimized host encode/decode frames are 64 bytes; physical depth remains unverified. This defines the record only. Redundant persistence, durable-parent/destination authorization, case/normalization ambiguity checks, recoverable member moves and live controller integration remain unfinished.

### Strict destination encoding and redundant installation plans

Installation base paths now reuse the strict streaming UTF-8 path validator, rejecting malformed continuation bytes, overlong encodings, surrogates, out-of-range scalars and incomplete sequences. Valid accented/decomposed/supplementary names remain accepted; NFC and FAT case ambiguity still require the installation owner's checks. Tests cover the exact 127-byte base limit and rejection at 128 bytes.

`CompanionDictionaryInstallationJournal.h` alternates `dictionary-install-a/b` records through the checked journal storage interface. It persists initial prepared ownership, the publishing transition, each ordered member publication, binding completion and commit, with complete length/CRC/content readback before advancing live state. Duplicate confirmed transitions avoid writes. Recovery compares immutable extraction/archive/path identity, rejects foreign plans and conflicting/nonadjacent transitions, and recovers the newest valid adjacent state. Truncated replacement slots fall back to the prior valid record; unreadable slots fail closed. Ambiguous writes invalidate live state until recovery. Filesystem verification and durable-parent authorization must precede protocol updates.

Active/candidate plans and codec working state are session-owned. The codec exposes a short-lived decoded view for journal readback, avoiding a fourth full-plan copy; wire scratch is borrowed and there is no allocation. Nine host tests pass for format/UTF-8 boundaries, all publication/commit transitions, duplicate-write suppression, every truncated replacement length, before/after-effect failures, foreign destinations, readback corruption and conflicting revisions. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionDictionaryInstallationPlanTest` and `ctest --test-dir /tmp/lila-companion-tests -R DictionaryInstallationPlanTest --output-on-failure`. Optimized host frames are 112 bytes for journal recovery, 80 for persistence and 48 for codec inspection; target depth remains unverified. HAL storage for these slots, parent/destination authorization, recoverable member moves, journal retirement and live integration remain unfinished.

### HAL storage for installation-plan slots

`HalDictionaryExtractionJournalStorage` now has an immutable `Purpose`: extraction retains its existing two 244-byte slots, while installation accepts only `dictionary-install-a/b` and exact 520-byte records. The same provider implementation retains one record handle and two checked-lookup handles; each provider prepares these three opaque states once and reuses them across boundaries/recovery. Both journals can coexist using six retained states. Cross-purpose paths, incorrect record lengths, offset/truncate misuse and invalid purposes are rejected. Presence errors, directories and unknown corrupt records remain closed to publication.

All sixteen selected HAL member/composition tests pass. New tests carry the installation protocol through publishing/binding/commit and a fresh journal recovery, verify exactly three handle preparations, coexist with an extraction slot without cross-purpose writes, and exercise directory/sync/truncate/silent-corruption/close failures plus preservation of pre-existing corrupt records. These protocol tests do not publish installed member files or prove a live durable parent. Verify with `cmake --build /tmp/lila-companion-tests --target HalZipNormalizedArchiveTest HalZipEntryStageTest` and `ctest --test-dir /tmp/lila-companion-tests -R 'HalZipNormalizedTest|HalZipEntryStageTest' --output-on-failure`. A strict optimized host compile passes the 256-byte frame warning limit; target depth remains unverified. Parent/destination authorization, recoverable member publication, journal retirement and live integration remain unfinished.

### Installation plans authorized by the recovered transfer destination

`Transfer::destination()` exposes a borrowed view only when the transaction is loaded and recovery is valid; ambiguous checkpoint writes and failed recovery hide it. `CompanionDictionaryInstallationParent.h` matches plan destination, exact extraction receipt, original manifest, transaction, storage generation, archive hash, fully received offset and parent phase against the actual portable transfer controller. Prepared plans may exist before installing; publication/binding receipts require `Installing`; plan commit requires an already `Committed` transfer. A committed parent can recover a bound plan for finalization. Authorization failure invalidates exposed plan state without writing records. The wrapper stores references only and the transfer accessor adds no object state or allocation.

Three new authorization tests pass using the real portable Transfer journal/state machine with in-memory storage and mocked content validation/metadata. They reject a foreign destination, gate publication/commit on durably persisted transfer phases, hide the destination after failed recovery or ambiguous checkpoint persistence, preserve journal files on rejection and permit recovery/retry. Existing thirty transfer tests and nine plan tests also passed after the API addition. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionDictionaryInstallationParentTest CompanionTransferTest CompanionDictionaryInstallationPlanTest` and `ctest --test-dir /tmp/lila-companion-tests -R 'DictionaryInstallationParentTest|CompanionTransferTest|DictionaryInstallationPlanTest' --output-on-failure`. The optimized host authorization match frame is 80 bytes; physical depth remains unverified. These tests do not perform installed dictionary member moves. Canonical-proof plan creation, destination case/normalization checks, recoverable publication, journal retirement and live controller integration remain unfinished.

### Installation-plan creation from the canonical build proof

`HalDictionaryInstallationPlanBuilder` assembles a prepared revision-one plan only from a complete authorized extraction receipt, a matching original manifest and a successful canonical build whose independent member proof matches that receipt. It validates the destination and complete plan before changing caller output. Its working plan is fixed session state rather than a large stack temporary; no heap allocation or additional HAL handles are introduced. Input destination/manifest views may refer to the output plan without being overwritten during construction. Saving still requires `DictionaryInstallationParent` authorization against the recovered transfer destination.

The composed plain/dictzip extraction pipelines exercise plan creation, invalid destinations, mismatched original identities, an unbuilt canonical archive, unchanged output on rejection, aliased inputs and persistence/recovery through installation-purpose HAL slots. These use synthetic extraction parents; actual transfer authorization is covered separately and live installed-member publication is not exercised. Verify with `cmake --build /tmp/lila-companion-tests --target HalZipNormalizedArchiveTest` and `ctest --test-dir /tmp/lila-companion-tests -R HalZipNormalizedTest --output-on-failure`. Destination case/normalization checks, recoverable member moves, reconstruction after publication, journal retirement and live integration remain unfinished.

### Checked installed-member paths

`CompanionDictionaryInstallationPaths.h` derives installed member paths from a valid durable plan, using the definitions format and optional-synonyms flag from its extraction receipt. It rejects invalid plans, member numbers, absent synonyms and insufficient output capacity before changing the caller buffer. The caller owns the fixed 136-byte buffer, sufficient for the maximum 127-byte base, `.dict.dz` and the terminating NUL; there is no allocation. This is shared path construction for the forthcoming publisher/recovery layer, not filesystem ownership or collision authorization.

All eleven installation-plan host tests pass, including suffix selection, invalid-input/output preservation and the exact longest-path terminator boundary. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionDictionaryInstallationPlanTest` and `ctest --test-dir /tmp/lila-companion-tests -R DictionaryInstallationPlanTest --output-on-failure`. Member moves and physical filesystem acceptance remain unfinished.

### Portable ordered member publication

`CompanionDictionaryMemberPublication.h` coordinates info/index/definitions/optional-synonyms moves through the actual transfer-authorized installation parent. Before mutation it inspects every selected stage and destination against the receipt. Published members require missing stages and verified destinations; unpublished members require verified stages and missing destinations. During publishing only the first unpublished member may already have moved without its receipt, allowing recovery after a successful rename followed by interruption. Conflicting or unreadable files prevent publication. Each move is checked at both locations before its durable receipt is advanced; confirmed publication is idempotent.

The storage interface requires exact length/SHA, regular-file and ambiguity checks, exclusive destination reservation and non-replacing moves. The coordinator stores four booleans and borrowed references, without allocations or path buffers. It does not supply destination ownership, replacement rollback, binding publication or a HAL filesystem provider yet. Five actual-parent host tests pass, including recovery of a rename that took effect but reported failure, ordered continuation, idempotency, full preflight conflict rejection and receiving-phase rejection without moves. File proofs/moves are mocked; the parent uses the real portable transfer/journal with mocked content validation. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionDictionaryInstallationParentTest` and `ctest --test-dir /tmp/lila-companion-tests -R DictionaryInstallationParentTest --output-on-failure`. Physical SD durability and live integration remain unverified.

Publication now rechecks parent authorization after post-move file verification and before advancing the receipt. All seven parent/publication tests pass: additional cases inject a failed journal write after a completed move, require explicit recovery before retry, avoid repeating that move, and preserve files/journals when both locations exist, a later member moved out of order or inspection fails. The optimized host publication frame is 96 bytes under the strict 256-byte frame check; target call depth is unverified. The same verification command above covers these cases. Filesystem proofs and moves remain mocked until the HAL provider is implemented.

### HAL receipt verification at either member location

`HalDictionaryMemberVerification` verifies a present staged or installed member against its installation-plan length and SHA. It rejects directories, changed lengths, altered bytes and absent optional members, and distinguishes verification conflicts from open/hash/close errors. Failed opens never establish absence. Presence, unambiguous path lookup and exclusive ownership are caller prerequisites. The path buffer is fixed session state; hashing borrows scratch and one HAL handle allocation is retained across close/reopen, avoiding allocation churn and large task-local buffers.

All ten HAL member-stage host tests pass. New coverage verifies staged and installed locations, same-length byte corruption, close failure, absent synonyms and exactly one extra handle preparation across repeated verification. Verify with `cmake --build /tmp/lila-companion-tests --target HalZipEntryStageTest` and `ctest --test-dir /tmp/lila-companion-tests -R HalZipEntryStageTest --output-on-failure`. This supplies present-file verification only; checked destination lookup/reservation, actual HAL moves and live controller wiring remain unfinished.

### Checked destination-member enumeration

`HalDictionaryDestinationLookup` scans the entire existing destination directory before reporting exact presence or absence. Checked enumeration and both closes must succeed; lookup errors never establish absence. It rejects directory matches, duplicate matching entries, ASCII case aliases and NFC-equivalent spellings that differ from the planned bytes. Normalization borrows disjoint scalar/output banks from the installation session; fixed path/name buffers and two reusable HAL handles remain session-owned. No new buffer allocation is introduced.

All ten HAL member-stage tests pass with additional missing/exact/ASCII-alias/decomposed-Unicode-alias/enumeration-error coverage. The optimized host lookup frame is 192 bytes under the strict frame check; target depth remains unverified. Use the verification commands above. This checks member names inside an existing directory only. Ancestor directory ambiguity, full non-ASCII FAT case equivalence, durable destination reservation, actual moves and live integration remain unfinished; this helper alone does not authorize publication.

### Guarded HAL member publication provider

`HalDictionaryMemberPublicationStorage` combines checked private-stage lookup, destination enumeration and exact member receipt verification. Its required owner callback must establish durable destination reservation, current parent authorization, ancestor ambiguity and full FAT equivalence under serialized ownership; no callback or a failed check rejects operations. The provider rechecks ownership after lookups/hashing, checks both locations immediately before a publishing-phase rename and never intentionally replaces a present target. Failed renames remain ambiguous and require coordinator recovery rather than destructive cleanup. This does not make the multi-call checks atomic against unrelated filesystem writers: exclusive ownership is mandatory.

The provider borrows the destination lookup/verifier, adds two reusable private-stage lookup handles and one fixed path buffer, and allocates no per-operation buffers. The ten HAL member-stage tests pass with owner rejection, stage/destination proofs, prepared-phase move rejection, rename failure preserving the stage, successful move and repeat-move rejection. The owner callback is mocked in these tests; durable reservation/ancestor/full FAT checks still need implementation before live use. Use the HAL verification commands above. Coordinated live installation, binding recovery, retirement and physical SD durability remain unfinished.

### SdFat non-ASCII long-name case equivalence

`CompanionDictionaryFatCase.h` contains the MIT-licensed range/pair mapping from the installed SdFat `common/upcase.cpp`, with fixed constexpr tables and no allocation. Destination lookup compares normalized UTF-8 scalars through this map rather than ASCII bytes, rejecting accented uppercase aliases too. This is SdFat's long-name mapping, not Unicode full case folding or a volume-specific exFAT upcase table. Ancestor lookup and short-name aliases still require owner checks; supplementary filename support and physical filesystem behavior remain unverified.

`python3 scripts/check_companion_fat_case.py` compiles against the installed dependency and verifies every one of the 65,536 BMP values; all match. `--sdfat` selects another dependency source directory for upgrades. All ten HAL member-stage tests also pass with accented uppercase alias coverage. The mapping must be rechecked when SdFat changes. Durable reservation, complete ancestor/short-name ambiguity handling and live installation remain unfinished.

### Checked destination ancestors

Destination lookup now checks the dictionary root in `/` and the selected folder in that root using the same full-directory NFC/SdFat case comparison. Exact matches must be directories; case/normalization aliases or duplicates are conflicts. The HAL publication provider requires both levels to be present before inspecting any member, so a valid member name cannot bypass an ambiguous ancestor. Both scans reuse the existing two handles and borrowed normalization banks without allocation.

All ten HAL member-stage tests pass with exact ancestors and a duplicate uppercase folder alias added to the provider composition test. Root scanning explicitly permits `/`, which the inventory's file-path grammar deliberately excludes. Use the HAL verification commands above. Short-name aliases, volume-specific exFAT equivalence, durable folder reservation, full recovery composition and live wiring remain unfinished.

### Checked dependency accessor for short-name aliases

The existing SdFat build patch now adds `FsBaseFile::getShortName`: FAT returns its checked short filename, exFAT returns a successful empty string because it has no short aliases, and closed handles or buffers smaller than 13 bytes fail. This forwards to the existing `FatFile::getSFN` inside the dependency; downstream firmware must access it through a mutex-protected HAL method, which remains to be added. The accessor introduces no allocation.

`python3 test/companion/sdfat_patch_check.py .pio/libdeps/default/SdFat/src` passes source matching, idempotence, changed-anchor rejection, host compilation and real FAT/exFAT image checks. Runtime checks retrieve the alias, reopen a FAT file through it and compare directory identity, verify exFAT's empty alias, and reject small buffers/closed handles. Existing enumeration-error tests remain passing. This does not yet check aliases during publication or prove a firmware build; HAL exposure, lookup integration and device acceptance remain unfinished.

### HAL short-name alias collision checks

`HalFile::getShortName` now forwards through `storageMutex` with argument/open-handle checks and logged failures. Destination enumeration checks each entry's long and short names against the requested component, rejecting an alias match whose actual long spelling differs. This applies to member paths and both ancestor scans. A failed alias read or invalid alias encoding prevents reporting absence. The lookup adds a fixed 13-byte session buffer and no allocation.

All ten HAL member-stage tests pass with a requested member colliding with another entry's short alias and with injected alias-read failure. The real FAT/exFAT accessor checks above remain the underlying dependency evidence. A default firmware build has been started after the HAL change; its result is pending. Durable reservation, volume-specific exFAT case handling, complete installation/recovery composition and hardware acceptance remain unfinished.

### Initial reservation of an existing empty destination

`inspectEmptyFolder` first checks both destination ancestors, then requires checked EOF in the selected directory and successful handle closure. Any entry, including an unrelated file or directory, reports occupied; enumeration/cancellation/close errors cannot prove emptiness. All ten HAL member-stage tests pass with empty/occupied/error/ambiguous-ancestor cases.

`HalDictionaryInstallationReservation` accepts only a new prepared revision-one canonical-proof plan, requires a verified empty existing folder, rehashes every staged selected member against its receipt, and finally saves through the actual transfer-authorized installation parent. It stores borrowed references only, allocates nothing, and does not create or replace folders. The serialized owner must maintain exclusive access between these checks and persistence. Strict optimized host compilation passes with 160-byte reservation and 64-byte empty-folder frames; composed runtime tests for reservation itself are still pending. The default firmware build remains running. Reservation recovery, exFAT volume equivalence, complete publication/binding composition and live wiring remain unfinished.

### Composed reservation and actual transfer authorization tests

The parent test target now links the real HAL hash implementation against its host HAL seam. Its reservation cases construct actual member SHA receipts, check empty ancestors/destination, persist the prepared plan through the real portable transfer authorization wrapper, recover it through a fresh installation journal and reject duplicate reservation without writes. Occupied folders, altered bytes, hash read failure, journal write failure and a foreign destination leave journal files unchanged and no authorized plan exposed.

All nine parent/publication/reservation host tests pass. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionDictionaryInstallationParentTest` and `ctest --test-dir /tmp/lila-companion-tests -R DictionaryInstallationParentTest --output-on-failure`. This uses in-memory transfer/journal storage and the HAL file seam, not a physical SD card or live Bluetooth controller. Canonical manifests are fixture inputs here; canonical-proof plan creation is covered separately. The default firmware build remains running. Complete HAL publication/recovery composition, binding recovery, volume-specific exFAT checks, retirement and live integration remain unfinished.

### Composed HAL member moves and journal recovery

The actual-parent test now composes destination reservation, the guarded HAL publication provider and ordered publication coordinator. The host HAL seam can enumerate its current file map, so scans reflect renames without manually updating directory entries. The test injects a rename that moves info but reports failure, recovers the persisted publishing plan, adopts that verified destination without repeating its move, publishes index/definitions, verifies all installed SHA receipts and confirms idempotent retry. The owner guard uses the actual recovered plan and durable Installing transfer phase; exclusive filesystem ownership remains a test assumption.

All ten parent/publication/reservation host tests pass. Use the parent verification commands above. The default firmware build also passed in 5:46 after the HAL short-name accessor change (`/tmp/lila-hal-short-alias-firmware.log`). Newly composed installer helpers remain unused by the live controller, so that build verifies the HAL/dependency integration, not end-to-end dictionary installation. Binding proof reconstruction, complete reboot/retirement handling, exFAT volume mapping, live dispatch and physical acceptance remain unfinished.

### Fresh dictionary recovery before metadata completion

Fresh transfer recovery previously retried metadata installation immediately, leaving its destination unavailable when dictionary metadata could not yet be completed. `TransferRecoveryMode::DeferDictionaryInstallation` explicitly verifies the raw staged archive (or already-moved raw target) and exposes the recovered Installing dictionary transaction without advancing its phase or calling metadata installation. It applies only to declared dictionaries in Installing; default recovery retains its existing behavior. Verification failure keeps the destination unavailable. The mode adds no object state or allocation and does not authorize unverified member publication.

The interrupted HAL publication test now recovers fresh Transfer, extraction-journal/parent and installation-journal/parent objects, rejects altered installed info without journal writes or more renames, then restores the bytes and resumes publication. An additional test verifies deferred recovery leaves Installing and all files unchanged even when metadata could succeed, hides the destination for a corrupt raw archive, and confirms default recovery still completes installation. All 41 selected tests pass: `ctest --test-dir /tmp/lila-companion-tests -R 'DictionaryInstallationParentTest|CompanionTransferTest' --output-on-failure`. A default firmware build after the API change is running (`/tmp/lila-deferred-dictionary-recovery-firmware.log`). Live dispatch must opt into this mode and complete the dictionary-specific recovery owner; bindings, retirement and physical acceptance remain unfinished.

The deferred staging-file branch is also covered: an injected pre-move rename failure leaves the raw archive in `incoming`, fresh recovery verifies it without mutations/phase advancement, and corruption hides the destination. Invalid recovery modes and a foreign storage generation cannot bypass authorization or mutate files. All twelve dictionary-parent tests pass. The default firmware build after the recovery API change passed in 1:10 (`/tmp/lila-deferred-dictionary-recovery-firmware.log`); the live controller still uses the default recovery mode. Verify the parent tests with the command above. Dictionary-specific binding recovery, retirement, live dispatch and physical acceptance remain unfinished.

### Binding coordination from a recovered installation plan

`DictionaryPlanBinding` uses the durable plan's original/canonical manifests, verifies every selected installed member and missing stage, and publishes the binding only under an actual Installing parent with all publication receipts. Successful binding publication precedes the durable Bound transition. Finalization requires an already Committed transfer, rechecks member proofs, performs retryable binding cleanup and then marks the plan Committed. Failures do not advance the plan. The coordinator borrows references only; it does not recreate large canonical proof objects or allocate working buffers after reboot.

`HalDictionaryPlanBindingStorage` adapts the existing immutable-archive-verifying, synced-readback binding provider. Thirteen parent tests pass, with new coverage for incomplete receipts, wrong member locations, installation/finalization failures, parent commit gates and idempotent finalization. Bindings and member inspection are mocked in that protocol test; full HAL binding composition and interrupted binding-journal writes remain to be tested. Verify with the parent test commands above. Live dispatch, committed recovery, retirement, volume-specific exFAT checks and physical acceptance remain unfinished.

Binding checkpoint failures are now covered too. The mock binding completes publication or cleanup, then injects failure into the subsequent actual installation-journal write. Both failures invalidate live plan state, prevent further calls until recovery and preserve the last durable journal files. Recovery resumes Publishing for binding retry or Bound under an already Committed transfer for finalization retry; the latter uses a fresh installation journal/parent. All fourteen parent tests pass with the same verification command. Binding side effects and member inspection remain mocked in this boundary test; full HAL/cache/binding composition is still pending.

### HAL archive/binding composition through committed recovery

The parent test target now composes the real HAL archive cache, destination reservation, member publisher, binding provider and recovered-plan binding coordinator with actual SHA receipts. The test publishes distinct immutable original/canonical blobs, moves members, rejects binding sync failure while retaining Publishing, retries to Bound, recovers fresh committed transfer/extraction/installation objects, rejects corrupt cached canonical bytes before finalization, then restores them and verifies the finalized original/canonical binding.

All fifteen parent tests pass with the parent verification commands above. These are host HAL seam and in-memory transfer-journal tests; archive/member bytes are synthetic proof fixtures, and semantic StarDict/canonical ZIP construction is covered separately by the normalized-archive pipeline. The mock transfer content validator/metadata callback does not implement live dictionary installation. Full semantic end-to-end controller composition, retirement, volume-specific exFAT behavior and physical acceptance remain unfinished.

### Deferred committed dictionary finalization

Explicit dictionary recovery deferral now also handles Committed transactions: it verifies the raw target hash, exposes the recovered owner and leaves metadata retry/backup cleanup to that owner. Default recovery remains unchanged. This prevents a metadata dependency from hiding the destination before the dictionary binding/finalization plan can be recovered, and retains cleanup files until their ownership is established. A corrupt raw target still prevents destination exposure.

All 46 selected transfer/parent tests pass. The new committed-recovery case demonstrates default metadata failure, explicit deferral with no mutations, preservation of the backup and corruption rejection. HAL binding composition now recovers through this mode with metadata unavailable before finalizing the recovered Bound plan. Verify with `ctest --test-dir /tmp/lila-companion-tests -R 'DictionaryInstallationParentTest|CompanionTransferTest' --output-on-failure`. A default firmware build after the implementation change is running (`/tmp/lila-committed-dictionary-defer-firmware.log`). Live dispatch, owned retirement and physical acceptance remain unfinished.

The firmware build after committed deferral passed in 59 seconds. All 31 portable transfer tests also pass with explicit negative coverage: requesting dictionary deferral for EPUB, font or Tinta course content still completes Installing recovery through metadata, and a committed metadata failure remains an error with no exposed destination. The option does not bypass other content installation. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionTransferTest` and `ctest --test-dir /tmp/lila-companion-tests -R CompanionTransferTest --output-on-failure`. These use mocked content validators; native device behavior remains unverified.

### Independent retirement proof format and authorization

`CompanionDictionaryRetirementProof.h` defines a 532-byte version-one `DRET` record containing the full committed installation plan, reserved prefix bytes and an outer CRC32 in addition to the plan's nested CRC. Only a committed plan under a recovered Committed installation parent can produce the initial proof. Recovery authorization checks its identities, original manifest, full received offset, storage generation and destination directly against the recovered transfer, without requiring extraction/installation journals that cleanup may already have removed. The nested codec keeps its working plan in session state and allocates nothing; failed decode preserves caller output.

All 29 selected plan/parent tests pass. New cases reject every single-byte corruption and truncation, reject reserved-prefix misuse even with a valid outer CRC, preserve output, reject premature proof creation, retain proof authorization after test deletion of the member journals, and reject foreign destinations/failed transfer recovery. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionDictionaryInstallationPlanTest CompanionDictionaryInstallationParentTest` and `ctest --test-dir /tmp/lila-companion-tests -R 'DictionaryInstallationParentTest|DictionaryInstallationPlanTest' --output-on-failure`. This defines the proof only: redundant durable publication/readback, owned cleanup, proof retirement and startup/live integration remain unfinished. Tests remove journals directly to exercise independence; no production cleanup path exists yet.

### Redundant retirement-proof publication

`DictionaryRetirementJournal` publishes two identical full proofs to `dictionary-retirement-a/b` with synced-write contract, length/CRC/content readback and direct recovered-transfer authorization. A committed installation parent seeds initial publication. Existing valid copies avoid writes; valid foreign proofs and unreadable slots fail closed. Recovery exposes a surviving valid proof even with a torn peer, but initial member-journal cleanup must require `redundant()`; publication preserves invalid existing slots rather than overwriting unknown records. Repair of a torn existing slot remains unfinished.

The active and codec working plans are session state, wire scratch is borrowed, and no allocation is introduced. All seventeen parent tests pass with proof write failure/retry, redundant readback, idempotent publication, torn-peer fallback, foreign-proof/read failure preservation and fresh recovery after direct test removal of member journals. Use the parent verification command above. The storage interface is still backed by in-memory test storage; HAL proof slots, owned cleanup, torn-slot repair, proof retirement and live integration remain unfinished.

### HAL retirement-proof slots

The journal HAL provider now supports an immutable Retirement purpose, accepting only `dictionary-retirement-a/b` and exact 532-byte records. It reuses the same one record and two checked-lookup handles with no new buffer or per-operation allocation. Extraction, installation and retirement paths remain isolated by provider purpose.

All 34 selected parent/member/composed HAL tests pass after rebuilding their targets. Retirement coverage injects sync failure, rejects exposed proof state on failure, retries to two verified copies, recovers a fresh journal, confirms exactly three handle preparations throughout and rejects writes to extraction/installation slots without changing files. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionDictionaryInstallationParentTest HalZipNormalizedArchiveTest HalZipEntryStageTest` and `ctest --test-dir /tmp/lila-companion-tests -R 'DictionaryInstallationParentTest|HalZipNormalizedTest|HalZipEntryStageTest' --output-on-failure`. Torn-slot repair, owned cleanup, proof retirement and startup/live integration remain unfinished; the live firmware does not instantiate this provider purpose yet.

### Owned member-journal cleanup

`DictionaryMemberJournalCleanup` rereads both retirement proofs and requires redundant valid copies plus installed-member/archive/finalized-binding verification before cleanup. It preflights every existing extraction and installation journal, accepting only the final record or its immediate predecessor with matching transaction and content identities. Malformed lengths and foreign records preserve all files. Removal failure leaves both retirement proofs intact; retry accepts already missing journal slots. Transfer records and retirement markers remain untouched.

Decoder and receipt state live in the session object and wire bytes use borrowed scratch; no heap allocation is introduced. All 34 selected tests pass using the command above, including installed-verification rejection, malformed/foreign journal preservation, partial deletion retry, idempotency and fresh transfer/proof recovery after cleanup. Installed-content verification is currently a required storage callback exercised by a mock; its production HAL implementation, torn-proof repair, proof retirement and startup/live integration remain unfinished. No live firmware path instantiates this helper yet.

### HAL installed-content verification

`HalDictionaryInstalledVerification` requires a valid Committed plan and serialized owner guard, checks that each selected private member stage is missing and each installed member passes checked destination lookup and exact length/SHA verification, then verifies the finalized binding and both retained archive hashes. `HalDictionaryBindings::verifyFinalized` uses the normal read path, rejecting pending binding stage/backup files, and compares the complete binding before verifying archives. It performs no binding installation or cleanup. Providers are borrowed; no allocation or additional buffer is introduced.

All 34 selected tests pass after rebuilding the three targets above. The composed HAL fixture verifies successful finalization and rejection of damaged installed members, damaged original archives and foreign transactions while preserving the file map. These fixtures use synthetic archive/member bytes, not complete semantic StarDict bundles. Optimized host frames are 64 bytes for the verifier and 48 bytes for finalized-binding verification, checked with `-Werror=frame-larger-than=256`; target call-depth and heap acceptance remain unverified. Connecting this verifier to a HAL member-journal removal adapter, proof repair/retirement and startup/live integration remains unfinished.

### HAL member-journal removal

`HalDictionaryMemberJournalCleanupStorage` connects the cleanup coordinator to installed-content verification and existing extraction/installation HAL journal providers. Its path allowlist contains only the four member-journal slots; writes and retirement-proof removal are prohibited. It requires verified installed content, redundant retirement proof and a serialized owner guard through reads, lookups and removals. Each removal uses checked lookup, releases the journal file before `Storage.remove`, and checks absence afterward. The adapter borrows all providers and adds no handles, buffers or heap allocations.

All 34 selected tests pass with actual stubbed HAL journal deletion, an after-effect removal failure followed by retry, path/write rejection, repeated cleanup and fresh transfer/retirement-proof recovery after the HAL member journals are gone. The retirement guard authorizes directly against the committed transfer, independently of extraction/installation parents; production serialization must also exclude competing SD writers. Transfer journals remain in mocked storage and archive fixtures are synthetic, so this is not device power-loss acceptance. Optimized host removal frame is 160 bytes with `-Werror=frame-larger-than=256`. Verify with the three-target build and test command above. Torn-proof repair, proof retirement and startup/live integration remain unfinished; live firmware does not instantiate the adapter.

### Torn retirement-proof repair

`DictionaryRetirementJournal::repair` rereads both proof slots, requires at least one valid proof authorized by the committed transfer, and invokes a required installed-content/exclusive-owner verification callback before replacing an invalid or missing peer. A valid foreign proof, two invalid copies, read failure or failed verification prevents repair. Only unverified slots are written; the surviving copy is preserved. Synced write and exact readback reuse publication's existing borrowed scratch and session state. Normal publication continues to preserve invalid existing slots.

All 34 selected tests pass after rebuilding the three targets above. Added coverage verifies failed validation and before-effect write failure preserve files, successful torn-peer repair restores redundant copies, two torn copies and foreign proofs are rejected, and HAL sync failure leaves the surviving proof unchanged before recovery/retry. The HAL repair callback uses installed-member/archive/finalized-binding verification. Optimized host frames are 48 bytes for the repair wrapper and 112 bytes for its write helper with `-Werror=frame-larger-than=256`; no new allocation is introduced. Proof retirement, raw transfer cleanup and startup/live integration remain unfinished. Physical power-loss and target resource acceptance are still required.

### Preserve dictionary recovery ownership during transfer admission

Before persisting a new transfer identity or truncating incoming data, `Transfer::beginImpl` checks all six extraction, installation and retirement journal slots. Any present slot returns Busy, including malformed records; lookup failure returns IoError. The current transfer and files remain intact, and repeating the same existing declaration remains idempotent. Shared constexpr path definitions live in `CompanionDictionaryJournalPaths.h` so admission and journal providers use the same paths. Recovery and dictionary cleanup retain their existing access to the current transfer.

For these six paths, `HalTransferStorage::stat` uses checked companion-directory enumeration rather than interpreting a failed `exists()` as absence. The lookup retains two reusable HAL handles and a 256-byte filename bank in session state: the bank would exceed the task-local limit alongside other locals, and retaining handles avoids repeated allocation per admission attempt. Other stat paths retain their existing behavior. This does not establish general target-path error/absence guarantees.

All 58 selected transfer, dictionary-parent and HAL course tests pass, including each pending slot, lookup-error preservation, same-declaration retry, admission after cleanup, false `exists()` with a present proof, legacy course inventory and framed Tinta pack transfer. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionTransferTest CompanionDictionaryInstallationParentTest HalCourseTransferTest` and `ctest --test-dir /tmp/lila-companion-tests -R 'CompanionTransferTest|DictionaryInstallationParentTest|HalCourseTransferTest' --output-on-failure`. The final `pio run -e default` passed in 61 seconds. Optimized host admission frame is 176 bytes with `-Werror=frame-larger-than=256`; device heap/call-depth acceptance remains unverified. Proof retirement and live dictionary installation/recovery are still required before a pending dictionary transaction can release admission for the next transfer.

### Completed-installation proof retirement

`DictionaryRetirementCompletion` rereads the surviving retirement proof, requires installed-content/staging-completion verification and absent member journals, then preflights every remaining proof marker for exact length, CRC and full-plan identity before deleting either. A missing marker permits retry after interrupted retirement; an invalid remaining marker requires repair rather than deletion. Successful removal invalidates exposed proof state. The transfer journal, raw committed target, installed members, immutable caches and finalized binding remain intact.

`HalDictionaryRetirementCompletionStorage` verifies installed members/archives/binding and checked absence of extraction/installation journals, incoming/backup, member/cache candidates, ZIP range/name staging files and all four extracted-member stages. Removal is restricted to the two retirement markers, with checked absence readback and serialized owner guard. It borrows the proof provider, installed verifier and retained lookup rather than allocating handles or buffers. Decoder state is session-owned and wire scratch is borrowed.

All 34 selected parent/member/composed HAL tests pass after rebuilding their three targets. Completion coverage preserves files with remaining ZIP staging or member journals and a torn proof, rejects candidate removal, retries after an after-effect marker deletion failure, removes both markers and confirms all unrelated files remain identical. Transfer storage and archive fixtures retain the mock/synthetic limitations described above. Verify with the three-target build and test command in HAL retirement-proof slots. Optimized host coordinator frame is 160 bytes with `-Werror=frame-larger-than=256`. Owned temporary/raw-backup cleanup, startup/live dictionary integration and physical power-loss/resource acceptance remain unfinished; live firmware does not instantiate completion yet.

### Retirement-proof discovery without member journals

The no-argument `DictionaryRetirementJournal::recover()` discovers a valid proof directly from its two slots and checks it against the recovered Committed dictionary transfer before exposing it. It then invokes paired recovery to reread both slots and reject a valid conflicting peer. This does not require a caller plan reconstructed from extraction/installation journals that cleanup may already have removed. Missing records, two corrupt records, read errors and an unavailable transfer remain distinct failures; a surviving authorized proof permits later repair or retirement.

All 34 selected tests pass after rebuilding the three targets above. Coverage discovers either surviving slot, rejects foreign proofs and read failures without mutation, discovers the committed plan through HAL after member-journal cleanup, reports Missing after proof retirement, and rejects discovery after failed transfer recovery. Optimized host discovery frame is 96 bytes with `-Werror=frame-larger-than=256`; existing codec/active-plan state and borrowed scratch are reused without new allocation. Startup/live dispatch remains unfinished.

ZIP range/name staging files currently contain no transaction identity and retain ownership only in provider memory. The completion verifier therefore preserves them after reboot rather than deleting by filename. Durable ownership evidence or verified reconstruction is still required for abandoned staging cleanup; this discovery change does not establish that evidence.

### Production base filesystem resolver

`HalInventoryBaseResolver` replaces the test-only base resolver in the composed scan/publication fixtures. It validates bounded paths, prunes `/.crosspoint`, descends other directories, classifies case-insensitive `.epub` suffixes and skips unrelated files. Recognized course/font/dictionary extensions reaching the base produce an error, requiring the specialized installed-content resolver rather than silently losing transferable content. Course/font/dictionary wrappers must surround it in production. EPUB classification uses filenames; it does not semantically validate EPUB archives.

All eleven selected inventory-build/base-resolver tests pass, including actual traversal/hash/sort/publication, duplicate content, spill sorting, short workspace rejection, font wrappers, course-disabled refusal, uppercase EPUB classification, unrelated/private-file skipping, unsafe paths and unchanged metadata on skip/error. Verify with `cmake --build /tmp/lila-companion-tests --target HalInventoryBuildTest` and `ctest --test-dir /tmp/lila-companion-tests -R 'InventoryBuildTest|HalInventoryBaseResolverTest' --output-on-failure`. The base adds no heap allocation; optimized host resolve frame is 160 bytes with `-Werror=frame-larger-than=256`. Live session composition, dictionary working-memory ownership, inventory command dispatch and native/device acceptance remain unfinished. This header is not instantiated by live firmware yet.

### Composed inventory resolver session

`HalInventoryResolverSession` composes base, font, dictionary and course resolvers with HAL archive bindings. It borrows the build session's workspace, shares its first 4 KiB hash bank across sequential resolution/hash operations, and borrows a separate tinfl decoder and exact 32 KiB DEFLATE window. The Tinta Pack parser and resolver/provider state live in the session object outside the task stack. It introduces no internal heap buffer allocation; callers must own the decoder/window/session with checked allocation and release them before reusing memory for radio queues. Preparation requires prior authoritative recovery and checks workspace/window sizes before storage access. Unprepared resolution fails closed.

All twelve selected inventory-build/base tests pass. The composed fixture scans EPUB, validated bitmap font and semantic plain dictionary members, publishes one index containing all three kinds, skips unrelated text/private files, rejects unprepared use and preserves files for an undersized workspace. Strict optimized host compilation passes with `LILA_TINTA=0` and `LILA_TINTA=1` and `-Werror=frame-larger-than=256`; preparation wrapper frame is 48 bytes. The Tinta-enabled configuration is compile-checked here, not exercised by this composed runtime fixture. Verify with the build/test commands above. Live ownership/allocation, actual Tinta composition, inventory dispatch, compressed-dictionary composition, cancellation and physical C3 heap/call-depth acceptance remain unfinished; live firmware does not instantiate this session yet.

The composed session now has runtime coverage with `LILA_TINTA=1`: one actual HAL scan publishes the validated bound mini course pack, EPUB, validated bitmap font and semantic dictzip dictionary. The course manifest exactly matches its persisted binding. Corrupting the dictionary gzip trailer makes a rescan fail while preserving the published index, path map, revision and learner data. The course test target links the same miniz implementation used by dictionary validation.

After rebuilding the host tree in a fresh environment, all 23 selected course-transfer and inventory-build/base tests pass. Verify with `cmake -S test -B /tmp/lila-companion-tests -DCMAKE_BUILD_TYPE=Release`, `cmake --build /tmp/lila-companion-tests --target HalCourseTransferTest HalInventoryBuildTest`, and `ctest --test-dir /tmp/lila-companion-tests -R 'HalCourseTransferTest|InventoryBuildTest|HalInventoryBaseResolverTest' --output-on-failure`. EPUB bytes in these fixtures are synthetic; this checks classification/hash publication rather than EPUB semantic parsing. Live ownership/allocation, command dispatch, cancellation and physical resource/power-loss acceptance remain unfinished.

### Live Connect & Sync inventory preparation and paging

Connect & Sync now renders a translated preparation message before building inventory on its serialized activity loop. It allocates one checked `InventoryScanSession` containing the decoder, 32 KiB window, composed resolvers and build state; those objects exceed the local-stack budget. The existing 8 KiB workspace is reused only while Bluetooth is stopped. Preparation rejects at most 50 KiB free internal heap after the owner allocation. This gate does not prove the later scan/radio heap floor. The scan owner is destroyed before advertising, rather than retaining its buffers throughout the radio session. The activity retains only its index reader/catalog and existing workspace.

Inventory command dispatch requires the authenticated installation's current Bluetooth session and bound peer, then calls the bounded page handler. Catalog read scratch uses the transfer partition, disjoint from radio queues and frame payloads. Exit stops Bluetooth, clears the catalog and closes the member index handle before releasing workspace. A transition into Committed invalidates the catalog; repeated already-committed requests do not invalidate a fresh catalog. This first path requires leaving and reopening Connect & Sync to rebuild after a transfer. Preparation is synchronous and currently has no cancellation; sleep/home controls are suppressed while scanning, and the screen omits the inactive Back hint.

The target build exposed an Arduino `HEX` macro collision in the cache/binding digit tables; both now use `HEX_DIGITS`. All 29 selected host tests pass, including actual Tinta/compressed-dictionary publication, authenticated paging, malformed/changed snapshots and preservation of a sentinel-filled radio-queue region while catalog scratch uses the transfer partition. The final `pio run -e default` after the last source edit passed in 45 seconds. Verify with `cmake --build /tmp/lila-companion-tests --target CompanionInventoryTest CompanionInventoryIndexTest HalCourseTransferTest HalInventoryBuildTest` and `ctest --test-dir /tmp/lila-companion-tests -R '^(CompanionInventory|CompanionInventoryIndex|HalCourseTransferTest|InventoryBuildTest|HalInventoryBaseResolverTest)\.' --output-on-failure`.

These host tests do not execute the Activity/Bluetooth lifecycle. On device, use `python3 scripts/debugging_monitor.py`, open Connect & Sync, confirm preparation completes before discovery, register the companion and collect every inventory page, transfer a book/course, then reconnect and verify the new inventory. Measure free/largest internal heap and task watermarks during both preparation and radio phases, and check repeated exit/reentry releases scan/index resources. Native Apple integration, all-board builds for this change, cancellation, in-session refresh, BLE timing, physical power-cut and C3 resource acceptance remain unfinished. Dictionary transfer installation/recovery and the broader companion plan also remain incomplete.

The Apple refresh UI now distinguishes the inventory Changed result from other failures and instructs the user to reopen Connect & Sync on the reader and reconnect to rebuild its library. `InventoryCollectorError.requiresReaderReopen` classifies the inventory result only; an error-command response with the same byte and generic I/O failures retain their existing handling. Collection still fails rather than exposing an accepted partial page.

All 216 CompanionKit tests pass on Linux after restoring the missing Swift 6.0.3 toolchain cache. New transport-backed coverage injects Changed, I/O and error-command responses after a valid first page, verifies the typed error/guidance classification, and verifies successful complete collection on a fresh transport. `swiftc -frontend -parse apple/App/CompanionApp.swift` also passes; native SwiftUI typechecking/UI and physical reconnection remain unverified. Reproduce with `swift test --package-path apple/CompanionKit`. In-session inventory rebuild and the wider companion plan remain unfinished.

### Live inventory heap checkpoints

Connect & Sync now logs free internal bytes, largest available block and the boot-wide minimum before scan allocation, after allocation, on scan completion/failure, after successful scan-owner release and after Bluetooth startup/failure. Debug logging also reports the target's compiled scan-owner size. The helper uses the existing HAL heap statistics and introduces no allocation; its extra heap queries compile out when info logging or serial logging is disabled. The existing allocation-time 50 KiB gate remains in place. The boot minimum may reflect earlier operations and is not a per-scan minimum or proof of a later radio memory floor.

The final default firmware build after the last source edit passed in 43 seconds. No host tests were added for logging. Verify on hardware with `python3 scripts/debugging_monitor.py` and a default/info-enabled build: compare the allocated, scan-complete, released and radio-started checkpoints, repeat sessions, and check failures release memory on exit. Actual heap fragmentation, task watermarks, all-board behavior and resource acceptance remain unverified; no memory savings or C3 acceptance is claimed from these diagnostics alone.

### Private-cache root aliases

The base resolver excludes the ASCII case variants of `/.crosspoint`, both the directory itself and its descendants. The slash boundary remains required, so `/.crosspoint-other` is not excluded. This prevents a case-insensitive SD directory alias from publishing cached EPUBs as library content, without allocating a path copy. The 12 inventory build/base resolver host tests pass, including a composed scan with `/.CrossPoint/private.epub`. On a reader, use a mixed-case private directory entry and confirm its cached EPUB is absent from companion inventory while ordinary books remain present.

### Inventory root length boundary

Directory traversal uses an explicit bounded root scan, stopping at its terminator before copying into the session-owned 512-byte path buffer. Roots with 511 bytes plus a terminator fit; a full 512-byte unterminated buffer is rejected and leaves the walker in its error state. Seven directory walker host tests pass, including these boundaries and the default slash root. This adds no allocation. Verify a normal reader inventory scan completes and rejects an oversized configured scan root when exercising the walker directly.

### Active course path spelling

The course resolver recognizes ASCII case variants of the exact `/tinta/course.pack` path, matching case-insensitive SD lookup semantics. It validates using the enumerated path, retains the original binding checks, and verifies the actual hashed manifest. Longer names such as `course.pack.extra` are not aliases. This adds no allocation. The 25 selected HAL course/inventory tests pass, including uppercase-path validation/hash verification and suffix rejection. On hardware, enumerate a differently cased active-course entry and confirm its inventory manifest matches the installed binding; the host fixture supplies the alternate path explicitly and does not emulate FAT directory lookup.
