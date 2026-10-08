# Journal export pages

The portable C++ `JournalExportPage` and Apple decoder share a bounded wire
format for exporting committed journal events. Authenticated BLE now dispatches
`ExchangeChanges` to this exporter when the reader advertises capability bit 3.
`AuthenticatedReaderSession.journalExportPage` requests and validates one page.
Installed-content refresh automatically collects this journal into the Apple
library. An authenticated Wi-Fi handoff also dispatches journal export;
`JournalExportCollector.collect(wifi:library:maximumEvents:)` uses its encrypted,
transaction-bound requests. Inbound reader journal merging remains unfinished.

The caller must audit the journal and compute its frontier before beginning an
export, exclude journal writers for the session, and reset the exporter when the
authenticated installation or connection changes. An SD read error or changed
record count invalidates the exporter. The exporter owns no heap buffers: its
event and body views borrow the journal's existing workspace, and replies use
the caller's 1,024-byte control buffer. Request, reply and journal scratch must
not overlap.

The dedicated companion activity retains one checked heap audit/export owner
after the inventory scan and releases it on exit. The first zeroed request audits
and freezes the journal; later page requests reuse its buffers. Initializing an
empty journal may create its durable headers, and opening an interrupted journal
uses the existing recovery rules before auditing. Export authorization resets on
reauthentication, connection change, forgetting the bond, Wi-Fi handoff and
activity exit. Unauthenticated requests receive an error without opening the
journal. The Apple session checks capability before sending and rejects replies
that do not match the command, request ID or response flag.

`JournalExportCollector` assembles a complete bounded export before importing it.
The app currently accepts at most 100,000 events per export. Its Apple-side array
and identity set reserve the declared count only after checking this limit;
these containers keep received events outside the MCU and enable a single
atomic library transaction. A limit failure leaves the library unchanged.
Library schema 28 adds a reader journal baseline keyed by reader identity and
storage generation. Events and the exact remote frontier/count commit in the
same transaction. The store revalidates the complete history and typed bodies
before recording this baseline; cancellation, conflicting events or invalid
metadata preserve the earlier baseline. A different SD generation gets its own
record. This is a verified last-observed remote frontier, not proof that the
reader has remained unchanged since that export; future uploads must compare
the current reader frontier before publication.
Duplicate identities, missing causal ancestry, sequence gaps, body corruption,
frontier mismatch, interrupted transfers and cancellation before commit also
prevent import. A verified retry deduplicates existing events; equivocation
rolls back the entire import. The library checks cancellation on its own actor
before beginning the transaction, and a completed commit returns its receipt.
Tinta imports still require a verified course/resource association in the
library. Unknown course content must be imported before its history can be
accepted. This ingestion does not publish merged history back to the reader.

All integers are little endian. A request is exactly 40 bytes:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 32 | Audited frontier hash |
| 32 | 4 | Frozen committed record count |
| 36 | 4 | Next physical record index |

A zeroed request starts an export. Later requests copy frontier, count and next
index from the previous reply. Retrying the same cursor returns the same page.
BLE carries this cursor directly. Wi-Fi prefixes the cursor with its 16-byte
handoff transaction identity; both platforms require exactly 56 request bytes
and verify the transaction before dispatch. Replies keep the same page format.
Wi-Fi export currently requires an existing content-transfer handoff; it does
not establish a separate history-only Wi-Fi session.
Cursor indices follow committed physical journal order; deterministic causal
replay remains a separate operation after assembling and validating history.

A reply starts with a 48-byte header:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 1 | Version, currently 1 |
| 1 | 1 | 0: event; 1: explicit end |
| 2 | 2 | Reserved, zero |
| 4 | 4 | Frozen record count |
| 8 | 4 | Next record index |
| 12 | 32 | Audited frontier hash |
| 44 | 2 | Encoded SyncEvent length |
| 46 | 2 | Body length |
| 48 | variable | Encoded SyncEvent, then exact body bytes |

Each event page advances exactly one index. End pages contain no event or body
and keep the index equal to count; an empty journal ends immediately. The largest
supported envelope/body pair occupies 1,009 bytes including this header. The
Apple decoder checks lengths, cursor progression, reserved bytes, frontier
continuity, event encoding and SHA-256 of the body. It also validates typed
reading, bookmark, preference and Tinta bodies and their resource/scheduler
bindings before accepting a page. Receiving an end page alone
does not prove that a caller durably stored or validated all earlier pages.

Verify with `CompanionTintaJournalTest` and Swift `JournalExportPageTests`. Both
compare the same independently constructed binary fixture at
`protocol/fixtures/JournalExportPage-v1.fixture`. Tests also cover retries,
explicit completion, empty histories, stale cursors, malformed bytes and
corruption invalidating further reads. HAL tests exercise the actual audited
SD-backed journal through both pages and explicit completion. Apple session
tests cover capability rejection without sending, reader errors and reconnects.
Collector tests exercise atomic commit, retry deduplication, interruption,
limits, invalid frontiers, missing sequences, duplicate identities, conflict
rollback, empty exports and cancellation at commit.
Additional collector cases reject malformed bodies even when their SHA-256 is
correct, reject wrong preference scopes and invalid reading scheduler/resource
bindings, and import a complete mixed reading/bookmark/preference history.
Wi-Fi request tests reject every other payload length and foreign transactions.
Apple encrypted handoff tests exercise the shared event-page fixture and an
empty-journal collector import, and verify a foreign transaction closes the
handoff without another network request.
Hardware transfer and heap acceptance remain pending.
