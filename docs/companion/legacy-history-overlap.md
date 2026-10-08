# Legacy history overlap

`ContentVault.legacyHistoryOverlap(first:second:)` compares two distinct, verified backup manifests for the same confirmed course family. It decodes their journals, item snapshots, profile and mark logs before reporting evidence. Invalid backups remain errors. Different course families have no overlap result.

The preview reports either:

- `identicalLearnerFiles`: every non-usage manifest file has the same role, SHA-256 and length. Reader/card identity and diagnostic usage files may differ.
- `sharedReviewPrefix(records:)`: learner-file sets differ, but the journals begin with the same nonempty sequence of complete original 12-byte records. A mismatch ends the prefix. Padding after the committed records does not become an event.

No preview establishes that records came from one shared historical event. Independent devices can produce identical legacy records, especially with inaccurate clocks. Partial prefixes and exact file matches therefore require explicit confirmation before deduplication. Missing optional files and present-but-empty files are not treated as identical file sets.

`LibraryStore.legacyOverlapCandidates(backup:vault:)` lists overlap previews against registered backups of the same course family in deterministic backup-hash order. The source must be registered and its stored reader/card provenance must match the verified manifest. All manifests and blobs stay in the vault. The query writes no events, migration epochs or completion receipts and does not merge diagnostic logs.

Tests cover exact matches with distinct usage logs, symmetry, divergent and shorter prefixes, independent first records, different courses, unregistered sources, and preview-only persistence. ## Confirmed exact sharing

`LibraryStore.confirmSharedLegacyBackup(_:canonical:vault:)` persists a schema-14 shared-backup receipt after explicit confirmation. Both backups must be registered, verified, from the same course family, and have exactly matching non-usage learner files. The canonical backup must already have an installed migration. The source must not have its own migration reservation. Partial-prefix matches are not accepted by this API.

The receipt references the canonical installation rather than importing another set of events. Both immutable backups and their independent diagnostics remain available. The canonical event sequence, resource, generation, course and scheduler configuration are checked, and its installation digest is recomputed from the original journal hash and stored event bytes. Receipt reuse performs that validation again. A changed or missing canonical event fails validation.

Confirmation is transactional and idempotent. A source cannot be retargeted to another canonical migration, and reservation or installation of a separate migration for a shared source is rejected. The canonical target must be a real installed migration; receipts do not form alias chains.

Tests cover schema-13 upgrade with existing events, rollback on failed receipt insertion, restart, duplicate confirmation, unchanged schedules/totals/event counts, partial-prefix rejection, prepared-source rejection, retargeting rejection and canonical-event damage. Partial-prefix reconciliation, explicit independent-history decisions and a native migration/conflict screen remain unfinished.

## Migration preparation gate

The public prepareLegacyMigration workflow now checks registered same-course overlap before reserving an epoch. Unresolved candidates produce a typed overlapConfirmationRequired error containing the previews. confirmedIndependentBackups must name only current overlap candidates and must cover all unresolved candidates. This records no automatic shared-history assumption. A shared-backup receipt stops preparation with alreadyShared. An installed migration can rebuild its existing plan for idempotent retry without another independence confirmation.

Raw epoch reservation is package-internal. Preparation captures the registered same-course backup set before asynchronous validation and reservation compares it again inside its SQLite write transaction. A changed set produces staleBackupSet before the counter is allocated. Tests use a failing random provider to establish that unresolved overlap and stale sets fail before epoch generation, then verify explicit independent preparation, installation and idempotent retry. All 165 CompanionKit tests pass. Durable drafts for decisions made before installation and the native conflict screen remain pending.

## Durable migration drafts

Schema 15 stores versioned, bounded migration drafts containing the exact backup and pack hashes, confirmed course family, scheduler configuration, reading-mark choices, independent-history choices and reviewed backup set. Saving validates verified objects and a complete conversion preview, checks the backup set again transactionally, and writes neither events nor an epoch. Repeating an unchanged save does nothing. Lists and reading choices are sorted for deterministic encoding. Drafts contain no pairing credentials or installation origin.

The draft-based prepare overload reloads choices after restart and repeats validation. A changed registered backup set invalidates an uninstalled draft and requires a fresh review/save. An already installed migration can still reconstruct its immutable plan for an idempotent retry after new backups arrive. Malformed or inconsistent draft data is rejected. Installed decisions cannot be replaced by a different draft; existing reservations must retain their pack and scheduler binding.

Tests cover saving with an unavailable epoch provider, empty event storage, unchanged saves, restart with independent-history and ambiguous-reading choices, stale-set rejection and resave, installed retry after a new overlap, malformed payloads, and schema upgrade. All 166 CompanionKit tests pass. The draft APIs are ready for the native migration screen, which remains unfinished.
