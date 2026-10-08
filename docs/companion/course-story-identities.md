# Course story identities

Distributed reading-completion subjects use a 32-bit story identity within an explicitly confirmed logical course family. They are distinct from legacy Tinta title-hash marks.

## Version one

Apply FNV-1a (initial value 2166136261, multiplier 16777619, wrapping 32-bit arithmetic) to these bytes, in order:

1. ASCII `TST1`.
2. One byte containing the pack story kind.
3. Four little-endian bytes containing the stable authored lesson identity: `(unit.number << 16) | lesson.number`. Use `0xffffffff` when the story has no lesson.
4. The title's UTF-8 bytes, excluding the terminating zero.

Map a zero result to one. Reject `0xffffffff`. Every distributed identity in a pack must be unique. Unit zero is valid; authored lesson numbers must be positive, and the all-ones authored lesson identity is reserved.

The identity does not use record indices, file offsets, filenames or the pack hash. Reordering records preserves identity when references still name the same authored lesson. Changing the authored lesson identity, story kind or title changes identity; the format has no separate authored story UUID. Duplicate titles in different lessons or of different kinds can coexist. An actual 32-bit collision or duplicate subject is rejected rather than merged.

The shared fixture is `protocol/fixtures/CourseStoryIdentity.json`, based on `test/tinta/fixtures/mini.pack`. C++ and Swift tests check its three identities. C++ uses a borrowed source and the existing 64-byte read buffer; validation does not allocate an additional identity table on the reader.

## Legacy migration

Apple inspection retains a parallel legacy title-hash identity for each story, using the original title-only FNV-1a rule. Unique legacy marks map to the corresponding distributed identity. A mark matching multiple stories throws `LegacyMarkLogError.ambiguousKey`; it does not complete every matching story. Unknown marks remain errors. Migration exposes a conflict preview and accepts explicit confirmed selections from each ambiguous key to a set of its candidate distributed identities. A selection may contain one, several or no candidates. Unknown, unrelated and stale selections are rejected. Without a selection, ambiguity remains an error. Selected identities are emitted in sorted order so plans are deterministic. A native conflict-resolution screen is still required.

The compiled repository Spanish course contains 67 stories but only 65 distinct legacy title hashes. All 67 new identities are distinct and agree between the C++ and Swift implementations. This is host validation of the development-authored pack, not release review, hardware acceptance or live installation evidence. Live Tinta marks still use their existing storage semantics until the authoritative journal and migration are integrated.

Apple inspection now includes display context for every distributed story identity: title, kind, authored unit/lesson numbers, and an explicit truncated-title flag. Display capture is bounded to 256 UTF-8 bytes and discards an incomplete trailing scalar before adding an ellipsis. Hashing still consumes the complete title. Same displayed prefixes can therefore have different identities. No reader-side allocation or identity recipe changed.

ContentVault.legacyReadingOptions returns verified conflict candidates with this context, so a future native chooser can distinguish same-title stories in different lessons. Tests check fixture titles/kinds/lessons, introductory unit zero, duplicate-title candidate labels and multibyte clipping without identity loss. All 167 CompanionKit tests pass. The full authored Spanish pack still validates and all 67 identities match C++. The native migration/conflict screen remains unfinished.
