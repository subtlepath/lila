"""Configure bounded host pools and borrowed RX on pinned NimBLE-Arduino 2.3.8."""
from pathlib import Path

Import("env")

NEEDLE = "    size_t getLength() const { return m_value.size(); }"
ADDITION = """

    // Borrow only while the host callback owns this value; no heap copy.
    const uint8_t* getValueData() const { return m_value.data(); }
"""
for header in Path(env.subst("$PROJECT_DIR")).glob(".pio/libdeps/*/NimBLE-Arduino/src/NimBLEValueAttribute.h"):
    source = header.read_text()
    if "const uint8_t* getValueData() const" in source:
        continue
    if source.count(NEEDLE) != 1:
        raise RuntimeError(f"Pinned NimBLE borrowed-value patch does not match {header}")
    header.write_text(source.replace(NEEDLE, NEEDLE + ADDITION))

CONFIG_MARKER = '#include "nimconfig_rename.h"'
CONFIG_ADDITION = """

// lila: one active companion, four separately bonded Apple installations.
#undef CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 1
#undef CONFIG_NIMBLE_MAX_CONNECTIONS
#define CONFIG_NIMBLE_MAX_CONNECTIONS 1
#undef CONFIG_BT_NIMBLE_MAX_BONDS
#define CONFIG_BT_NIMBLE_MAX_BONDS 4
#undef CONFIG_NIMBLE_MAX_BONDS
#define CONFIG_NIMBLE_MAX_BONDS 4
"""
for header in Path(env.subst("$PROJECT_DIR")).glob(".pio/libdeps/*/NimBLE-Arduino/src/nimconfig.h"):
    source = header.read_text()
    if "// lila: one active companion" in source:
        continue
    if source.count(CONFIG_MARKER) != 1:
        raise RuntimeError(f"Pinned NimBLE configuration patch does not match {header}")
    header.write_text(source.replace(CONFIG_MARKER, CONFIG_MARKER + CONFIG_ADDITION))
