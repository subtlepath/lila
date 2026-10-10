# Portable preference events

The Apple shared core and reader implement a version-one explicit allowlist and
causal merge. The Apple app exposes saved preferences and conflict resolution;
reader startup restores resolved values and binds journal capture to settings
saves. Credentials, controls, orientation, clocks, lighting, refresh, and sleep
behavior remain device-specific.

Preferences use the SHA-256 of UTF-8 `lila:portable-preferences:v1` as their global
resource identity. The envelope kind is Preference (4); scheduler fields are zero.
Each body begins with version u8 (`1`), kind u8 (`4`), key u8, and value-type u8.
Unknown keys/types, invalid values, truncation, and trailing bytes fail validation.

| Keys | Portable setting | Value/range |
| --- | --- | --- |
| 1 | Font selection | Built-in family integer 0–1, or installed content selection |
| 2 | Font point size | Integer 1–255; reader must verify available sizes |
| 3–4 | Line spacing, alignment | Integer 0–3, 0–4 |
| 5 | Extra paragraph spacing | Boolean integer 0–1 |
| 6 | Word spacing | Integer 50–200 in steps of 25 |
| 7 | Character spacing | Signed integer −2–2 pixels |
| 8 | Margin | Integer 5–40 in steps of 5 |
| 9 | Hyphenation | Boolean integer 0–1; capability check required |
| 10 | Language | Bounded ASCII language tag, at most 63 bytes |
| 11 | Dictionary selection | Content selection or explicit none |
| 12–14 | Text antialiasing, embedded style, focus reading | Boolean integer 0–1 |
| 32–36 | Tinta new/day, review cap, retention permille, maximum interval, session size | Integer 0–200, 0–9999, 700–970, 1–36500, 5–500 |
| 37–38 | Tinta text size, UI language | Integer 0–2 |
| 39–40 | Tinta show vulgar content, typed answers | Boolean integer 0–1 |

Type 1 is an i32 encoded little-endian as its two's-complement bits. Type 2 is a
language byte length u8 followed by UTF-8; validation accepts bounded ASCII tag
components rather than claiming support for every tag on the reader. Type 3 contains
a presence u8 (0/1); when present it carries a 32-byte content SHA-256, name length u8,
and UTF-8 selection name (1–31 bytes). Names cannot contain controls or path separators.
Font selection cannot be none; selecting a built-in family explicitly clears SD-font
selection. Dictionary none is a five-byte body. Maximum body size is 69 bytes.

Content selection identifies immutable font/dictionary bytes, not a device-local path.
`PreferenceState.missingContent` reports an unresolved required hash until installation
is verified. Reader application must validate the asset kind/name against inventory,
install first, and only then apply the dependent setting. Font-size changes also wait
for the selected font. Unsupported values/capabilities must remain pending with an
explanation rather than silently selecting a different preference. Hyphenation is
a portable boolean in the current reader settings and application plan.

Each key merges through complete causal history. One-sided successors supersede older
values. Concurrent differing values remain candidates; matching concurrent values keep
all causal identities. A resolution must acknowledge all heads using bounded joins
when needed. SQLite retains the underlying events, so conflicts survive restart.
Scheduler preference changes affect future reviews; historic review bodies keep their
own exact configuration and continue replaying with it.

Tests cover encoding/decoding, all truncated prefixes, unknown keys, invalid ranges,
UTF-8/path boundaries, required content, causal conflicts/resolution, and SQLite reopen.
Physical acceptance must confirm each allowed setting applies after required assets,
and every excluded device-specific setting remains local across repeated syncs.

Reader integration uses `restoreReaderPreferenceRuntime` at startup and
`CrossPointSettings::bindPortablePreferenceSave` for local settings edits. Changed
values are journaled before settings publication. Dependency selections are
validated against their actual content, so selecting the same name after replacing
its bytes can emit a changed content hash. Font uploads capture replacement after
closing the file. Dictionary uploads defer capture until the web-server activity
exits: writers and networking are stopped, server buffers are released, and the
selected bundle is validated before publishing its identity. Unchanged content
is deduplicated. Incomplete bundles and failed upload writes/sync/close do not
publish replacement authority.

Dictionary capture reuses the existing checked, short-lived 8 KiB preference
workspace and save-session owner. They exceed the C3 task stack budget, so they
are allocated with `makeUniqueNoThrow` after network teardown and released before
reboot. Upload tracking itself adds fixed flags and borrows path bytes; shutdown
cleanup reuses the existing upload buffer.

For a bound Tinta course, `HalTintaLearnerPreparation` audits and resolves portable
profile preferences before the learner opens. `TintaCompanionSession` owns the
mutation bindings while the activity is open. Profile changes and learning
mutations share authoritative journal ordering; a failed publication blocks
further mutation until recovery. `App::close` flushes while the borrowed bindings
are still alive, then activity exit releases them. Historical reviews retain
their recorded scheduler configuration rather than being rescheduled with a
new preference value.

Host coverage includes causal preference conflicts, dependent font/dictionary
metadata, content-hash replacement and deduplication, native profile capture,
and interrupted journal publication. C3 compilation checks the actual upload,
shutdown, and capture entry points against the 256-byte frame limit. These checks
do not establish native Apple UI behavior or reader heap/radio behavior. Run the
portable-preference and dictionary-upload checks in
[hardware-verification.md](hardware-verification.md) on the participating readers
and verify each excluded setting remains local.
