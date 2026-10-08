# Portable preference events

The Apple shared core implements a version-one explicit allowlist and causal merge.
Reader emission, causal application, and settings UI remain pending. Arbitrary settings JSON,
credentials, controls, orientation, clocks, lighting, refresh, and sleep behavior are
not part of this protocol.

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
explanation rather than silently selecting a different preference. In particular,
current firmware forces hyphenation on; disabling it requires a capability/firmware
change before it can be applied. The shared core does not currently apply any settings.

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

`CompanionTintaPreferences` maps the nine portable Tinta integer keys (32–40)
to native Profile fields and exports the same eight-byte bodies. It validates
the shared wire schema before changing a field, preserves device-specific refresh,
lighting, clocks, and local state, and rejects unrelated keys. It uses fixed local
storage without heap allocation and passes the C3 256-byte frame compile check.
Callers must establish causal resolution and authority before applying a body,
then persist and reconfigure the runtime once per resolved batch. This mapping
is not yet wired to native settings emission or journal replay.

`persistResolvedTintaPreferences` validates at most nine distinct portable Tinta
keys against a profile copy before writing. Invalid or duplicate bodies leave both
the profile and storage unchanged. Changed batches replace the profile once and
publish the in-memory candidate only after successful persistence; identical
batches are write-free. The helper uses bounded local state without heap
allocation and passes the C3 256-byte frame check. Host coverage exercises invalid
batches, duplicate keys, failed replacement, durable reload, repeated application,
and unavailable storage. Callers still must supply ownership-verified, causally
resolved bodies and reconfigure App after success; native synchronization dispatch
remains pending.

`JournalCausalRelation::precedes` supplies the shared native ancestry primitive
needed for preference, reading-position, and bookmark conflict handling. It
validates the frozen journal/index, traverses marked dependencies backward through
physical causal order, and includes implicit per-origin sequence predecessors.
It distinguishes strict ancestry from concurrency and preserves output on errors.
Retained envelope metadata belongs off-stack; disposable visit marks remain
caller-owned, allowing SD-backed traversal without a history vector or per-query
allocation. Host coverage checks direct/transitive/implicit ancestry, reverse/self
queries, concurrent origins, invalid indices, and failed marking. The helper passes
the C3 256-byte frame probe. Preference candidate selection and conflict dispatch
remain pending.

`TintaPreferenceResolution` selects maximal native preference events for keys
32–40 from a frozen, validated journal/index. For each key it scans backward once,
propagating explicit and implicit sequence dependencies through caller-owned
disposable visit marks. Earlier causally superseded values are excluded; concurrent
equal values resolve, while differing maximal values set a per-key conflict bit.
Conflicts and errors expose no applicable batch. The retained owner keeps at most
nine eight-byte bodies and their borrowed views, plus bounded traversal metadata;
it allocates no history vector or per-query storage. Successful batches can feed
`persistResolvedTintaPreferences` after ownership admission. Host coverage checks
concurrent disagreement, explicit resolution naming both branches, equal concurrent
values, and failed marking. Native activity/connection dispatch remains pending.

`HalJournalCausalAuditSession::resolveTintaPreferences` exposes resolution through
the checked HAL identity index. It requires a completed audit, reopens the index,
allocates one checked visit-storage owner because its retained file handle exceeds
the stack budget, and closes visit/index/journal handles before returning. The
caller retains the resolution owner and its output views. Read/close errors clear
all output; successful conflicts retain only their conflict mask and no applicable
batch. The audit is consumed, requiring a fresh audit before another operation.
Host SD tests exercise pre-audit rejection, resolved output, concurrent conflict,
and read-error invalidation. The C3 256-byte frame compile probe passes.

`TintaWriter::recordPreference` emits validated portable preference bodies through
the learner session's existing durable identity, epoch, sequence, and causal
frontier. Preference scope and zero scheduler fields are enforced; body hashes
are computed before the fresh journal append. The same commit path advances
identity/frontier state for both preference and learning mutations. Invalid bodies
leave the writer usable without advancing history; uncertain storage failure stops
the writer until recovery. Retained envelope and journal workspace are reused,
with no per-event allocation. Host coverage interleaves preference and learning
mutations, checks exact ancestry, rejects invalid bodies, and injects a torn write.
The C3 256-byte frame compile probe passes. Profile-change capture and activity
writer binding remain required before enabling bidirectional application.

`TintaPreferenceCapture` initializes a session cache from the recovered native
profile, then prevalidates all nine portable fields before emitting changed
values. Device-only edits and identical retries create no events. Each successful
field advances the cache only after durable journal commit. Failure stops both
capture and writer; any already committed prefix remains authority and must be
recovered before the profile is saved or a fresh capture is bound. The fixed
previous/candidate arrays are retained in the session owner, with no per-field
allocation. Host coverage checks unchanged/device-only edits, whole-profile
preflight, exact changed-key ancestry, write-free repetition, and a cut after a
durable prefix. The native helper passes the C3 256-byte frame probe. App save
hook and activity lifetime binding remain pending.

`App::setProfileMutationJournal` installs a borrowed callback for debounced durable
profile saves. `saveIfDirty` invokes it before replacing profile.bin. Guest storage
skips the callback; failed authority blocks subsequent profile saves for that App
instance even if cleanup removes the callback. Rebinding after failure is refused;
a fresh recovered App is required. The callback context must outlive saves and
App::close, then be cleared before destruction. This adds fixed callback/flag state
and no heap allocation. Native App compilation passes. Activity capture ownership,
clock sampling, error presentation, and synchronized startup application remain
pending; the hook alone does not enable bidirectional preference sync.

Capture initialization succeeds only once per owner. Reinitialization cannot
silently adopt unjournaled edits or clear a failed batch; recovery must construct
a fresh capture from recovered profile state. Regression coverage rejects both
reinitialization after edits and reinitialization after a torn publication, while
preserving normal subsequent capture of the changed fields.

`HalTintaProfileMutationContext` adapts profile capture to App's borrowed save
callback and native clock. It prevalidates change detection before clock sampling,
so device-only edits do not require date confirmation. Changed portable values
sample a confirmed clock once; while awaiting confirmation, preference events use
explicit Unknown evidence, zero timestamp, and the recovered known day. This is
preference emission, not review scheduling. Failures log and invoke the caller's
error delegate. All buffers remain in the retained capture owner; the adapter adds
no heap allocation. Change-detection tests cover unchanged/changed/invalid profiles,
and the native callback passes the C3 256-byte frame compile probe. Activity
ownership and end-to-end callback/runtime tests remain pending.

`TintaNativeProfileMutationTest` links the production native Clock, journal writer,
identity provisioning, and profile callback adapter against host storage/RTC
fixtures. It verifies device-only edits emit no event while the date is
unconfirmed, portable edits retain Unknown evidence/known day/zero timestamp,
confirmed RTC edits carry Device evidence and the cached absolute UTC value,
repeated values emit nothing, and storage failure stops the writer and calls the
error delegate. This is runtime host coverage of the native callback chain,
separate from firmware and physical-device acceptance.
