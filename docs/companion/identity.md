# Reader and storage identity

`CompanionIdentity` provisions a reader identity, storage generation, and fresh
event epoch through `HalIdentityStorage`. Connect & Sync provisions this identity
before starting BLE; startup recovery provisions identity when transfer artifacts exist. Event writers must use the reserved epoch before creating an
origin/epoch/sequence identity; the distributed Tinta journal remains pending.

The reader identity contains an eight-byte `LILADEV` version prefix and the
reader's factory eFuse MAC value. It does not depend on paths, SD contents, or the
Bluetooth address that Apple exposes. It is an opaque identifier, not a secret.

Storage generation is bound to both:

- A canonical physical SD CID: manufacturer, OEM, product, revision, serial, and
  manufacturing date, with reserved/CRC bytes normalized. SPI reads the CID
  register; native SDMMC uses the initialized card's decoded CID.
- A nonzero 16-byte marker at `/.crosspoint/companion/card-id`.

CID reads go through the new `HalStorage::cardIdentity` method under the storage
mutex, then the SDK's card abstraction. Firmware consumers do not call SdFat or
SDK card methods directly. The browser host filesystem has no physical card CID
and returns unavailable instead of inventing a storage identity.

A replacement card, a file-copy clone with a different CID, or removal of the
marker creates a fresh generation. Returning to an earlier card also creates a
fresh generation because internal storage retains the most recent binding.
Existing transfer journals with a different generation are rejected by recovery;
a future UI must handle such imported recovery data explicitly.

The binding is an atomic Preferences/NVS blob under namespace `lila-companion`,
key `identity`. This internal record is deliberately independent of removable
storage. It holds protocol identity rather than portable settings or credentials
and must never be synchronized through CloudKit. The SD marker is created and
synced before the internal binding is committed and read back.

The 80-byte record contains `LCI` and version 1 at 0–3, hardware identity at
4–19, card CID at 20–35, marker at 36–51, generation at 52–67, little-endian event
epoch at 68–75, and IEEE CRC-32 over bytes 0–75 at 76–79. Corrupt or foreign
hardware bindings are rejected; provisioning never silently resets them.

Each successful provisioning call durably increments the event epoch before
returning it. An interrupted commit may consume an epoch without returning it;
retry reserves the next one. Epoch exhaustion returns an error instead of
wrapping. Initial provisioning seeds the epoch from the new random generation,
so erasing NVS does not deterministically restart the same hardware origin at
epoch one. The random identities are non-secret uniqueness tokens; authenticated
Wi-Fi session keys require their own cryptographic entropy lifecycle.

CID is not an authentication credential and can be spoofed by unusual cards.
Restoring older files onto the same physical card need not change its generation,
but events created after restart still receive a new internally reserved epoch.
Full history rollback detection and reconciliation depend on the authoritative
event journal and synchronization ancestry, which remain to be implemented.

## Verification

```sh
cmake -S test -B /tmp/lila-companion-tests
cmake --build /tmp/lila-companion-tests --target CompanionIdentityTest
ctest --test-dir /tmp/lila-companion-tests -R CompanionIdentity --output-on-failure
```

Tests cover stable identity, distinct-CID clones with identical files, reformat,
interrupted marker and binding commits, every corrupted binding byte, truncated
bindings, foreign hardware, empty identity/entropy, factory reset, and epoch
exhaustion. The core and both HAL adapters compile with the current C3 toolchain
without stack-frame warnings over 256 bytes. Firmware builds and physical
acceptance remain pending.

On C3 and native-SDMMC S3 hardware, record descriptor identities across reboot,
card replacement, card reformat, and cloning the same SD directory onto another
card. Require stable hardware identity, changed storage generation where
appropriate, and strictly increasing reserved epochs. Cut power during initial
marker creation and internal binding writes; confirm an error or a fresh epoch,
never a reused successful epoch. Monitor heap and stack during provisioning.
