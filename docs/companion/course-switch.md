# Explicit Tinta course switch

A different course requires user confirmation and preserves learner state in each
course's existing `/tinta/courses/<identity>` directory. Ordinary pack updates
continue to require the same course identity and compatible item history.

`CourseSwitchRequest` defines the consent payload shared by firmware and the
Apple companion. Its 132 bytes are:

| Offset | Length | Field |
| --- | --- | --- |
| 0 | 4 | ASCII `TCS` followed by binary version 1 |
| 4 | 16 | SD storage generation |
| 20 | 16 | Transfer transaction identity |
| 36 | 16 | Previous course identity |
| 52 | 16 | Next course identity |
| 68 | 32 | Previous pack SHA-256 |
| 100 | 32 | Next pack SHA-256 |

Every identity and hash must be nonzero. Previous and next course identities
must differ, as must their pack hashes. Receivers reject every other length or
version. C++ decoding preserves the caller's output on rejection. The shared
`protocol/fixtures/CourseSwitchRequest-v1.fixture` is tested by both codecs.

The request describes consent; it does not itself authorize installation.
Authenticated dispatch must compare its generation with the current SD,
transaction with the declared transfer, and both course identities and hashes
with the current binding and candidate manifest. Durable switch intent and
recovery must retain these exact bytes before replacing the active pack or
binding. Retries must not retarget consent to another transfer or course.

The C++ `matchesCourseSwitchRequest` and Swift `matches` checks implement this
initial comparison against the current binding and a valid initial transfer
declaration. They also reject invalid current course metadata and a transfer
whose manifest and state disagree. These checks perform no storage writes.

`CourseSwitchIntent` persists the request at
`/.crosspoint/companion/course-switch` through `course-switch-next`. The record
is the 132-byte request followed by its IEEE CRC-32 in little-endian order.
Writes and rename use the synced `TransferStorage` contract, with checked
read-back before and after publication. An exact complete staged record can
resume publication; torn or conflicting records remain untouched. Repeating
an already published request performs no write. The store borrows at least
136 bytes of scratch and allocates no heap memory. Its caller must prepare the
storage directory, authenticate, match consent to current context, and exclude
other writers before persisting.

`publishSwitchedCourseBinding` verifies the installed next pack against its
manifest and the durable consent before changing the active binding. It stages
the exact next binding, retains the previous binding as a backup, publishes and
reads back the next binding, then removes the backup. A retry can resume after
either rename or removal, including an I/O failure reported after its effect.
The consent stays present for the parent transfer to retire after completion.
Foreign or torn binding records are preserved and rejected. This publisher
allocates no heap and uses the same borrowed scratch.

The transfer controller now passes its durable `TransferState` to storage
validation and metadata publication, including Installing and Committed boot
recovery. Default storage hooks preserve existing behavior. This allows the
firmware switch integration to compare consent with the transaction, owner,
and generation without retaining a pointer to transient radio input.

Startup selection now recognizes completed legacy migration into the original
course. A read-only proof checks both migration intent/completion CRCs, matching
original identities, absence of pending stages, and absence of legacy source
files. When both state and mark migrations prove the same origin, another
bound course can select its isolated directory after active pack verification.
It neither moves original state again nor deletes migration receipts.

HAL validation and metadata publication now consume durable consent for the
matching fully received transfer. Validation checks the staged pack and old
binding, completes old-state migration or verifies its existing isolation,
and prepares the next course directory. Installing/Committed recovery verifies
the next pack and repeats binding publication. Ordinary transfers retain their
existing same-course compatibility checks when no consent is present.

Status: the codecs, context checks, consent persistence, binding publisher,
startup state selection, and HAL transfer integration are implemented.
Terminal transfer cleanup now retires matching consent after verifying the
new binding on commit or unchanged old binding on abort. Cleanup retries at
boot after an interrupted removal. Consent for another transaction remains
untouched.

Authenticated BLE and leased Wi-Fi `ExchangeChanges` dispatch now accept the
132-byte request for the owning installation's receiving course transfer. The
Wi-Fi wire body prefixes the lease transaction's 16 bytes. The reply is one
`TransferResult` byte followed by the consent transaction's 16 bytes. Both Apple
transports expose `authorizeCourseSwitch`; reply validation checks the response
command, request identity, result, and transaction. This does not yet advertise
switch capability or offer switching in the UI.

Schema 30 retains immutable confirmation bytes per transfer job. Confirmation
checks the complete inventory's reader/generation and exact previous pack, plus
the queued candidate's confirmed course identity. Existing confirmation cannot
be replaced by a changed inventory. The runner requires capability bit 4,
resends consent before chunks for a receiving transfer, and skips consent when
resuming Installing or Committed. Wi-Fi uses its lease-bound authorization API.
Tinta firmware advertises capability bit 4. Admission accepts a different course only with a saved confirmation for the
same job, reader, generation, and exact old/new identities and hashes. Ordinary
admission still rejects different courses. The native course screen offers review and confirmation for a different,
confirmed course on the connected capable reader. Confirming queues an exact
switch and selects the candidate atomically; other course selections for that
reader are cleared, while book selections remain. Another pending course
transfer prevents queueing. The user starts installation through the existing
Transfer selected content action.

Host tests cover persistence, resumed consent, binding recovery, state isolation,
and atomic queue conflicts. Native SwiftUI type-checking and physical BLE/Wi-Fi,
SD interruption, and heap/stack verification remain required. Physical SD power
loss and per-course learning-state isolation need device verification. Ordinary transfer
admission continues to reject different courses until those are connected.

The HAL interruption test walks every rename before and after its effect and
sync failures for migration stages, binding staging, and both parent transfer
journals. Each scenario reopens storage, completes the same switch, and checks
old items/stars, existing next-course items, the installed pack, cleanup, and
startup selection. The successful integration test changes the pack locale
from Spanish to French to exercise cross-language installation. These are host
fault simulations, not physical power-loss or native application tests.
