import runpy
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/patch_nimble.py"

class NimblePatchTests(unittest.TestCase):
    def run_patch(self, root):
        class Env:
            def subst(self, value):
                if value != "$PROJECT_DIR":
                    raise AssertionError(value)
                return str(root)
        runpy.run_path(str(SCRIPT), init_globals={"env": Env(), "Import": lambda name: None})

    def headers(self, root):
        directory = root / ".pio/libdeps/default/NimBLE-Arduino/src"
        directory.mkdir(parents=True)
        value = directory / "NimBLEValueAttribute.h"
        config = directory / "nimconfig.h"
        value.write_text("class Value {\n    size_t getLength() const { return m_value.size(); }\n};\n")
        config.write_text('#include "sdkconfig.h"\n#include "nimconfig_rename.h"\n')
        return value, config

    def test_bounds_follow_sdk_headers_and_patch_is_idempotent(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            value, config = self.headers(root)
            self.run_patch(root)
            before = (value.read_text(), config.read_text())
            self.run_patch(root)
            self.assertEqual(before, (value.read_text(), config.read_text()))
            self.assertEqual(before[0].count("getValueData()"), 1)
            self.assertGreater(before[1].index("#undef CONFIG_BT_NIMBLE_MAX_CONNECTIONS"), before[1].index('include "nimconfig_rename.h"'))
            self.assertIn("#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 1", before[1])
            self.assertIn("#define CONFIG_BT_NIMBLE_MAX_BONDS 4", before[1])

    def test_unexpected_value_api_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            value, _ = self.headers(root)
            value.write_text("unexpected upstream API\n")
            with self.assertRaisesRegex(RuntimeError, "borrowed-value patch"):
                self.run_patch(root)

    def test_unexpected_configuration_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _, config = self.headers(root)
            config.write_text("unexpected upstream config\n")
            with self.assertRaisesRegex(RuntimeError, "configuration patch"):
                self.run_patch(root)

if __name__ == "__main__":
    unittest.main()
