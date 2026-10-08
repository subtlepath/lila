# Apple installation bindings

`CompanionPairings` stores up to four installation identities, 32-byte secrets,
and canonical bonded peer identities (address type followed by six identity
address bytes). The core owns fixed entries and borrows 232 bytes of scratch
space. It allocates no heap memory. Keep it and `HalPairingsStorage` as session
members; the HAL owns a fixed readback buffer to stay within the stack budget.

Credentials belong in the Apple installation's Keychain and reader NVS. They
must never enter CloudKit or content/event records. A bonded BLE connection is
required before registering or presenting credentials. Registration must use
cryptographically random secrets after the authenticated-radio entropy lifecycle
starts. This module does not generate keys or authorize registration by itself.

The LCP/version-1 record contains four fixed 56-byte entries, each with used u8,
installation identity 16 bytes, secret 32 bytes, and peer 7 bytes. Unused entries
are zero. The first four bytes are magic/version and the final four bytes are
little-endian CRC32. CRC detects torn/corrupt data, not malicious modification.
`HalPairingsStorage` commits the entire record atomically through NVS key
`pairings` in `lila-companion`, then verifies readback before success.

Adding an existing installation never overwrites its secret. Capacity exhaustion
requires explicit forgetting. Forgetting a peer removes all installations bound
to it and avoids redundant writes when none remain. Any uncertain commit blocks
all authentication and mutations until a successful durable reload. Corrupt
records fail closed and must not be silently replaced.

The registry and HAL adapter are host-tested. Connect & Sync now handles BLE
RegisterInstallation (12) and AuthenticateInstallation (13), both with exactly
48 payload bytes: installation identity followed by its 32-byte secret. Success
returns byte 0; failure returns Error (11), byte 2. Malformed bodies return byte 1.
Authentication also requires the currently bonded peer to match the stored peer.
A successful registration authenticates that installation for the current BLE
session. After a lost registration response, authenticate with the original
credentials; registering an existing identity does not replace its secret.

The reader-side forget action first durably removes credentials for the current
peer, then deletes that BLE bond. A failed credential commit retains the bond and
blocks credential operations for the session. BLE transfer handlers now require
this authenticated installation session.

The shared Apple `PairingVault` persists a 16-byte installation identity and one
version-1 record per reader (version byte + installation 16 + secret 32). Prepare
credentials before sending RegisterInstallation and reuse them after a lost
response. The Keychain backend atomically inserts without overwriting existing
records, disables synchronization, and uses device-only accessibility. Missing
or corrupt installation state alongside a stored reader credential fails closed.
The portable lifecycle is tested; signed Apple Keychain and physical end-to-end
verification remain pending. Verify separate iPhone/Mac identities, app restart,
locked-device access errors without key rotation, and local forget isolation.

Run `ctest --test-dir <build-dir> -R CompanionPairings --output-on-failure` after
building the corresponding target. Physical verification must cover power loss
during NVS writes, reconnect after app restart, separately installed iPhone/Mac
credentials, and reader unpair removing credentials as well as the BLE bond.
