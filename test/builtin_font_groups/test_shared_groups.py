#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import re
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('shared_fonts', ROOT / 'scripts/share_builtin_font_groups.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class SharedFontGroupsTest(unittest.TestCase):
    def test_every_stream_and_all_other_font_data_are_preserved(self):
        source = ROOT / 'lib/EpdFont/builtinFonts'
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = module.generate(source, output)
            self.assertEqual(report['saved_stream_bytes'], 88869)
            self.assertEqual(report['font_count'], 40)
            includes = (output / 'all.generated.h').read_text()
            source_fonts = [path for path in source.glob('*.h') if path.name != 'all.h']
            self.assertEqual(len(re.findall(r'^#include ', includes, re.M)), len(source_fonts))
            for path in source_fonts:
                expected = (path.stem + '.generated.h' if module.GROUPS.search(path.read_text())
                            else '../' + path.name)
                self.assertIn(f'#include "{expected}"', includes)
            common = (output / 'shared.generated.h').read_text()
            shared = {}
            for name, _, body in re.findall(r'static constexpr uint8_t (\w+)\[(\d+)\] = \{(.*?)\n\};', common, re.S):
                shared[name] = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', body))
            checked = 0
            for path in sorted(source.glob('*.h')):
                if not module.GROUPS.search(path.read_text()):
                    continue
                original, bitmap, groups, data, rows = module.read_font(path)
                generated = (output / (path.stem + '.generated.h')).read_text()
                local_match = re.search(r'static constexpr uint8_t \w+Bitmaps\[\d+\] = \{(.*?)\n\};', generated, re.S)
                local = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', local_match[1]))
                new_groups = module.GROUPS.search(generated)
                new_rows = re.findall(r'\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\w+)\s*\}', new_groups[2])
                self.assertEqual(len(rows), len(new_rows))
                for old, new in zip(rows, new_rows):
                    offset, size, unpacked, count, first = old
                    new_offset, new_size, new_unpacked, new_count, new_first = map(int, new[:5])
                    self.assertEqual((size, unpacked, count, first), (new_size, new_unpacked, new_count, new_first))
                    stream = shared[new[5]] if new[5] != 'nullptr' else local[new_offset:new_offset + size]
                    self.assertEqual(stream, data[offset:offset + size])
                    self.assertEqual(len(zlib.decompress(stream, -15)), unpacked)
                    checked += 1
                # Every declaration outside the two rewritten tables is unchanged.
                before = original[:bitmap.start()] + original[bitmap.end():groups.start()] + original[groups.end():]
                after = generated[:local_match.start()] + generated[local_match.end():new_groups.start()] + generated[new_groups.end():]
                after = after.split('#include "shared.generated.h"\n', 1)[1]
                self.assertEqual(before, after)
            self.assertEqual(checked, 600)
            snapshots = {path.name: path.read_bytes() for path in output.iterdir()}
            module.generate(source, output)
            self.assertEqual(snapshots, {path.name: path.read_bytes() for path in output.iterdir()})

    def test_invalid_group_extent_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'invalid.h'
            path.write_text('static const uint8_t badBitmaps[1] = {0x00\n};\n'
                            'static const EpdFontGroup badGroups[] = {\n{0, 2, 1, 1, 0}\n};\n')
            with self.assertRaises(ValueError):
                module.read_font(path)


if __name__ == '__main__':
    unittest.main()
