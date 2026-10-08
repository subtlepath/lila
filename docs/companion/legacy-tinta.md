# Legacy Tinta migration preview

`LegacyTintaJournal` is a read-only Apple decoder for the current 12-byte `reviews.log`
format. It does not replace reader files, import distributed events, or claim that
migration is complete. Firmware migration, durable backups, derived-state comparison,
and conflict UI still need implementation.

Records contain item UID u32, device-local timestamp u32, learner-confirmed study day
u16, operation u8, and argument u8, all integers little-endian. Review grades are 1–4;
format is the high five operation bits and must be a supported format 0–9. The argument
is response time in quarter seconds (zero unknown; 255 saturated). Control code 1 is
undo and code 2 replaces the three item flag bits. Reserved UIDs, invalid grades,
unsupported formats/controls, invalid flag bits, and malformed undo fail validation.
Undo must target the latest eligible review of the same UID; undo and flag changes
clear its eligibility, matching `ProgressStore`.

Reader recovery can zero a failed append and the remainder of the existing log. An
all-zero suffix is recognized as padding and its exact byte count is reported. A
nonzero partial record or nonzero data after a blank record fails rather than silently
salvaging a different history. Original bytes, including padding, and their SHA-256
remain available for a future immutable backup. The in-memory preview is bounded to
16 MiB of input; larger logs require a streaming migration path before they can be
accepted. This does not impose a new limit on existing reader logs.

The decoder requires a verified stable logical course identity. Comparison reports
whether another journal has a matching record prefix. Even identical records always
produce `needsConfirmation`; independent devices can create identical local records,
and timestamps/UIDs do not prove a shared review. Only an explicit confirmed shared
history may be deduplicated in migration. Different course identities are not combined.
Diagnostic device usage logs are not inputs to this decoder.

Legacy timestamps are device-local and do not establish trusted UTC clock quality.
Legacy records also lack per-review scheduler configuration. Migration must retain
original logs/profile/state as backups, confirm the configuration used for rebuilding,
convert undo to an exact distributed review identity, and compare reconstructed state
against the reader's recovered derived state before installing it. Flag changes that
cannot be represented without altering leech state require a conflict/recovery path.
Do not infer missing metadata or enable distributed history from preview success alone.

Tests cover review/undo/flag decoding, one-level undo invalidation, zero-tail padding,
nonzero torn tails, invalid records, identical/prefix ambiguity, and distinct courses.

## Verified conversion plan

`migrationPlan` now creates typed distributed review/undo/suspension/star events and
replays them through the original C++ scheduler. Its context must contain a durably
reserved migration origin/epoch/generation, the course edition hash, and a confirmed
rebuild configuration. The API does not reserve those identities or silently infer
configuration. Legacy response quarter-seconds become milliseconds; the original
quantization and saturated 255 value remain limitations of the source measurement.
Legacy local timestamps are retained in the original backup bytes, while migrated
envelopes use unknown clock quality and zero timestamps.

Each review gets an event identity and causal link. Undo references the exact migrated
review identity. A legacy flag record becomes separate suspension/star events. If its
leech bit differs from scheduler-derived state, conversion fails with an explicit
unrepresentable-flags conflict. The plan compares every replayed packed item state
against the caller's recovered `items.bin` states, including retired item UIDs and fresh
records retained by undo. Missing/extra states or configuration differences that change
state fail the comparison. Matching state is a necessary gate, not proof of otherwise
missing historical configuration or of a shared log.

The result carries the original log hash, immutable planned events, and rebuilt
snapshot. It does not save backups, import events, replace reader files, or migrate
lesson/read completion state. Those steps still require a recoverable migration
transaction and an overlap-confirmation decision. Tests cover packed-state equivalence,
exact undo, flags, pure-undo fresh-state retention, configuration/state mismatch,
invalid expected UIDs, unsupported leech controls, and empty history.

### Immutable backup manifests

`LegacyBackupManifest` binds a reader, SD generation, and course to the hashes and lengths of its original files. Reviews, items, and profile are required; lesson, reading, and per-device usage files can also be preserved. Usage remains diagnostic data rather than merged learner state.

`ContentVault.preserveLegacyBackup` checks each captured export against its expected hash and length, then durably publishes a content-addressed manifest after all originals are stored. The caller must obtain expectations from an immutable reader export and persist the returned manifest ID before importing migration events. This API does not capture a consistent live reader snapshot or install migration events. A failed capture can leave immutable orphan objects, but returns no completed manifest receipt.

Verify with `swift test --package-path apple/CompanionKit`: `LegacyBackupTests` covers deduplication, restart recovery, changed-source rejection, corruption detection, and required roles. `verifiedLegacyBackup` rechecks both the manifest and every original before restoration or migration. Reader export, migration installation, and hardware verification remain pending.

`LibraryStore.preserveLegacyBackup` publishes the verified backup first, then registers its manifest ID in SQLite schema 5. Registration re-verifies all originals and is idempotent. `legacyBackupIDs` recovers receipts for an exact reader/SD-generation/course tuple after restart; callers must verify a receipt again before using it. Existing schema upgrades remain transactional. A crash before registration can leave an unregistered manifest, but cannot return a registered receipt before its originals are durable. Receipt storage contains no pairing credentials and does not imply that migration events have been installed.

`LibraryStore.reserveLegacyMigration` now durably reserves an origin/epoch, content resource, and confirmed scheduler configuration for a registered receipt in SQLite schema 6. Reopening the database recovers the same reservation; changed identity/resource/configuration and missing receipts are rejected. Epoch collisions with existing local origins, imported events, or another reservation fail without consuming a reservation. Linux tests inject deterministic randomness; Apple production uses the existing secure random provider. This reservation supplies `LegacyMigrationContext` but does not install events or resolve shared-history overlap. Atomic installation and reader integration remain pending.

`LibraryStore.installLegacyMigration` re-verifies the registered backup, binds the plan's original review hash and event identities to its durable reservation, checks course/generation/resource/configuration, and validates replay before installing it. SQLite schema 7 commits all immutable events and a length-delimited SHA-256 plan digest in one transaction. A matching retry rechecks stored events and returns zero inserted events; a changed retry fails. `LegacyMigrationTests.testAtomicInstallationRollbackAndRestartRetry` injects a completion-write failure and verifies that no events survive, then verifies successful installation and a restart retry. Run `swift test --package-path apple/CompanionKit` to reproduce these checks. This is Apple-side journal installation only: reader export/application, legacy item/profile decoding, shared-history confirmation/deduplication, and native UI integration remain pending.

Fresh local event epochs are also checked against migration reservations and imported events before creating their local counter. A collision fails transactionally; a distinct epoch for the same installation remains valid. The atomic migration regression test injects the same random epoch into both paths, verifies collision rejection, and then verifies that a distinct epoch starts at sequence one. The 123-test Swift suite passes after this guard.

`LegacyItemStore` now decodes clean `items.bin` exports using the reader's TIS1 header layout, alternating header offsets, CRC-32, and shared C++ packed-item validation. It chooses the newest valid header, rejects equal-sequence conflicting headers, requires pending writes to be recovered on the reader, and rejects truncated/invalid/duplicate records. Canonical retired slots remain counted but excluded from learner state. Decoder tests cover these boundaries and the standard CRC check vector. The migration coordinator still needs to obtain expected item state from this verified backup object and validate journal/header counts; profile decoding and reader integration remain pending. The Swift suite now passes 126 tests.

`ContentVault.legacyMigrationPlan` now obtains reviews and packed item state directly from a verified backup. It checks the reserved SD generation, bounds both input files before loading, requires the header's journal count to equal the decoded log count, and compares replayed packed state against every exported item. The atomic-installation test now uses a real backed-up TIS1 snapshot rather than a caller-supplied state map, and rejects changed generations and mismatched journal counts. All 126 Swift tests pass. Profile/lesson/reading conversion, shared-history confirmation, and reader integration remain pending.

`LegacyProfile` strictly decodes the current TPRF version-one, 31-byte payload with CRC and all firmware field ranges. It preserves original bytes, exposes portable learner settings and local clock/lesson fields, and converts retention permille to scheduler basis points exactly. Unsupported versions, corrupted bytes, and out-of-range values fail without silently substituting defaults. Backup-based migration planning now validates this preserved profile too. The current profile configuration does not prove historical review configuration; the reserved rebuild configuration still requires explicit confirmation and packed-state agreement. Clock, hardware settings, and lesson navigation fields are not automatically emitted as shared preferences or completion events. Run the 128-test Swift suite to verify decoding and the backed-up migration path. Preference/completion conversion and native confirmation UI remain pending.

`LegacyProfile.portablePreferences` explicitly maps the nine allowed Tinta learner settings to portable preference bodies for confirmation. Local clock, lesson navigation, display, sleep, and hardware fields are retained in the original backup without becoming shared settings. `LibraryStore.appendPreferences` writes a chosen set in one transaction, rejects duplicate keys, and uses deterministic key order plus the existing causal local sequence. It does not resolve concurrent remote preference conflicts; those remain visible through preference reconciliation and its explicit resolution APIs. The adoption test injects failure on the third event, checks that events and counter changes roll back, and verifies a nine-event retry and SQLite restart. All 129 Swift tests pass. Native confirmation UI and reader application remain pending.

`LegacyMarkLog` now decodes the TMK1 set format used by `/tinta/read.bin` and `/tinta/starred.bin` (`App.h` binds the filenames). Valid add/remove records preserve reader insertion order, repeated adds are idempotent, and removal/re-add moves a key to the end. Original bytes remain available. The migration decoder reports torn checksums, partial tails, unknown operations, and capacity overflow for explicit recovery instead of silently skipping them. Tests cover these boundaries; all 131 Swift tests pass. Reading keys are title hashes (`Library.cpp:23–27`), so completion-event conversion still requires a verified course-pack lookup and collision checks. Lesson completion is derived from profile progression rather than an assumed separate lesson log; its conversion also remains pending.

Backup roles now explicitly include `starred` for the reader's separate `/tinta/starred.bin`. `ContentVault.legacyBackupSnapshot` returns validated reviews, item state, profile, and optional reading/star mark sets from immutable objects. Absent optional files remain `nil`, distinct from a preserved empty set. Migration planning uses this snapshot and rejects malformed supplied mark logs while retaining their original backup bytes. The integration test covers absent/empty distinction and a torn star file that blocks planning without damaging its preserved original. All 131 Swift tests pass. This validates preserved state; completion/star event emission and course-pack mapping remain pending.

`LegacyProfile.completedLessonIndices` previews the reader's actual completion rule: only indices below `currentLesson` are done. Independently unlocked lessons remain incomplete. The preview validates current and unlocked indices against the matching course's lesson count, including empty and fully completed courses. `CoursePackInspector` now exposes the validated LESS count and rejects counts beyond the reader's 16-bit lesson range. Tests cover independent unlocks, complete/empty courses, and out-of-range progress; all 132 Swift tests pass. The coordinator must bind the inspected pack to the backup's course identity before using its count. Stable lesson identities and completion-event emission remain pending.

Course-pack metadata now exposes lesson identities in record order, encoded as `(authored unit number << 16) | authored lesson number`. The inspector checks positive authored numbers, unique unit numbers and lesson pairs, valid unit references, and the reserved all-ones key. These identities survive record reordering when authored numbering is retained; renumbering requires an explicit migration mapping. The course identity still namespaces each key. `LegacyProfile.completedLessonIdentities` maps validated completed indices through this table for preview. CRC-correct duplicate lesson numbers, invalid unit references, and zero unit numbers are covered by inspector tests; all 132 Swift tests pass. Reader-side encoding, course binding, and completion-event emission remain pending.

Course-pack inspection now exposes story identities using the reader's FNV-1a title-byte hash and zero-to-one rule. Title reads are bounded and cancellable; invalid offsets, unterminated strings, reserved all-ones identities, and duplicate hashes fail inspection. All story records are checked because the reader completion path uses the same key regardless of story kind. `LegacyMarkLog.completedStoryIdentities` rejects keys absent from the matching inspected pack rather than attaching retired/unknown completions to another story. Tests verify the portable fixture's three known hashes and reject CRC-correct duplicate-title and invalid-title packs. All 132 Swift tests pass. Course identity binding and event emission remain pending; title changes require explicit migration mapping.

The course-bound `ContentVault.legacyMigrationPlan` overload now appends lesson and reading completion events after verified review conversion. It requires an explicitly confirmed logical course association, checks SD generation, requires the reserved resource to match the immutable inspected pack's SHA-256, and validates profile progression and reading keys against that pack. Completion events continue the same origin/epoch sequence with direct causal ancestry and unknown clock metadata; independent unlocks and absent reading files do not create completions. The test exercises a preserved real pack and backup, checks event kinds/ancestry/lesson key, and rejects a different confirmed association. All 133 Swift tests pass. The UI must persist and present confirmation before calling this API; durable pack-association UX, star-set conversion, and reader application remain pending.

Course metadata now exposes validated item UIDs and recognition-item UIDs. Course-bound migration rejects exported item records absent from the associated pack, rejects star keys that are not recognition items, and reports stale stars on learnt items as a state conflict rather than silently discarding them. Valid fresh stars are appended after completion events in preserved set order, continuing the same causal sequence. The integration fixture includes a fresh starred recognition item and verifies its resulting packed star flag. All 133 Swift tests pass. Reader-side pruning/recovery can resolve stale star sets before a new capture; native confirmation and reader application remain pending.

`LibraryStore.prepareLegacyMigration` now coordinates validation and reservation for an already registered backup: it first previews the confirmed pack-bound conversion without writing events or reserving an epoch, then obtains the immutable durable reservation and rebuilds the plan with that identity. `ContentID.digest` supplies the validated file hash's binary representation. The extended integration test prepares, installs, reopens SQLite, prepares again, and checks identical mutations, zero duplicate insertions, packed item state, and lesson/reading completion sets. All 133 Swift tests pass. These checks cover Apple-side persistence and replay; shared-history confirmation/deduplication, native UI, reader export/application, and physical verification remain pending.
