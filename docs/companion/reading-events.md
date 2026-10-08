# Reading-position reconciliation

The Apple shared core now implements reading-position bodies and causal reconciliation.
Reader emission, position application, conflict UI, and legacy anchor migration are
not connected yet. Position sync remains disabled until those integrations are verified.

A version-one body is exactly eight bytes: version u8 (`1`), event kind u8 (`1`),
spine index u16, and visible-text offset u32, all integers little-endian. Spine and
visible-text offsets match the reader's progress representation; rendered page numbers
are not included. The envelope resource is the EPUB's SHA-256 edition identity.
Scheduler version and configuration digest are zero. The body is protected by the
normal event-body SHA-256 check. Missing offsets in legacy progress need explicit
migration; a cached page number cannot be assumed to be a text anchor.

`CausalHistory` validates complete global history, including explicit ancestry and
implicit per-origin sequence links. Its maximal-candidate query walks the ancestor
graph once across the candidate set and returns every candidate that another candidate
does not causally supersede. `ReadingHistory.reconcile` selects candidates only for the
requested edition, while preserving other event kinds/resources in the ancestry graph.
Timestamp, study day, and arrival order never resolve a concurrent position conflict.

A one-sided later change replaces its predecessor. Concurrent different positions are
retained and require a user choice. Concurrent equal anchors do not need a position
choice, but retain both causal identities. A resolution event must acknowledge all
remaining heads, including equal-anchor heads. If more than four heads exist, the
coordinator creates bounded causal joins before the final resolution through the
SQLite resolution APIs. Never truncate the ancestor list silently.

`LibraryStore.readingPositions` loads and verifies durable event bodies before
reconciliation, so unresolved candidates survive restart. Tests cover one-sided
changes, reversed/duplicate delivery, inaccurate clocks, explicit resolution, equal
concurrent anchors, malformed anchors, incomplete history, and SQLite reopen.
To verify integration on hardware later, read to different anchors on two offline
readers, sync in either order, confirm both positions are offered, select one, then
sync again and confirm no conflict or further change remains.

## Bookmark events

`BookmarkBody` uses a stable nonzero 16-byte bookmark identity scoped to the EPUB
edition hash. A body begins with version u8 (`1`), kind u8 (`2` put or `3` delete),
and that identity. Delete is exactly 18 bytes and is an explicit tombstone. Put adds
spine u16, visible-text offset u32, name byte length u16 plus UTF-8 name (at most 128
bytes), and summary byte length u16 plus UTF-8 summary (at most 512 bytes). Embedded
NUL, invalid UTF-8, unknown types/versions, oversized strings, truncation, and trailing
bytes fail validation. Put is at most 668 bytes. Envelope scheduler fields are zero.
Cached percentages and rendered chapter page numbers are derived device values.

Reconciliation groups events by stable bookmark identity and selects causal heads.
Different bookmark additions merge independently. A deletion that observes an earlier
put supersedes it but remains in the journal, preventing resurrection by delayed
repeat delivery. Concurrent edits with different values, or a concurrent put/delete,
retain both and require a user choice. A later resolution acknowledges all causal
heads. Equal concurrent values retain their causal identities without requiring a
value choice. Deleted bookmarks remain represented as tombstone states; the UI may
hide them from the ordinary list but must not discard their history.

`LibraryStore.bookmarks` rebuilds these states from the validated durable journal.
Tests cover independent additions, observed deletion, duplicate/reversed delivery,
edit/delete conflict and resolution, UTF-8/size boundaries, and SQLite reopen.
Reader stable-ID migration, event emission/application, conflict UI, and safe history
compaction remain pending. Legacy bookmarks without text offsets require anchor
migration before synchronization.

## Local Apple events

SQLite schema 4 adds installation-local origin counters, with epoch and sequence stored
as unsigned eight-byte blobs. `appendReadingPosition`, `appendBookmark`, and
`appendPreference` commit each immutable event and its advanced counter in one
transaction. Restart continues the existing epoch/generation and sequence. A newly
created database chooses a fresh random nonzero epoch and generation; pairing secrets
are not stored here, and counter rows must never be replicated through CloudKit.
Callers supply the current installation's Keychain identity as origin.

Locally created events default to unknown clock quality and zero timestamp/study day;
causal reconciliation does not require invented clock accuracy. Explicit dependencies
and the previous local sequence must have complete validated ancestry before commit.
Unrelated incomplete imported pages do not prevent independent local event creation.
Failed validation consumes neither a counter nor a journal row. SQLite's immediate
transaction serializes competing handles and prevents duplicate sequence allocation.
Individual event creation supports at most four explicit ancestors. Transactional
resolution now creates bounded joins for larger frontiers; conflict-resolution UI is
still pending.

Tests cover restart, causal reading resolution, rollback without sequence gaps,
concurrent handles, database reset, and local operations during partial remote history.

## Atomic conflict resolution

`resolveReadingPosition`, `resolveBookmark`, and `resolvePreference` take the causal
heads shown to the user. Inside one immediate SQLite transaction they recompute the
current frontier and require an exact set match. Empty, duplicate, or stale supplied
heads fail without writes. Each group of at most four heads creates a same-value event;
per-origin sequence ancestry links each join to its predecessor, so the final event
acknowledges every original head. No ancestor is silently dropped. All join events and
counter changes commit together. A failure in any join rolls back the whole decision.
Intermediate delivery can still show a remaining conflict on another device until it
receives the final causal event, preserving rather than prematurely discarding history.
A retry with an old frontier creates no additional event; the UI must reload the state.
Tests cover nine-head joins, stale/duplicate frontiers, injected failure on the second
join, counter rollback, and bookmark/preference resolution.
