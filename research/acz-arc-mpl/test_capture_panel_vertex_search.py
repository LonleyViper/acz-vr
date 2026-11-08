import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('capture_search', Path(__file__).with_name('search_capture_panel_vertices.py'))
search = importlib.util.module_from_spec(spec)
spec.loader.exec_module(search)


class CaptureSearchTests(unittest.TestCase):
    def test_exact_float32_sequences_and_misses(self):
        xyz = (1.5, -2.0, 3.25)
        needle = struct.pack('<3f', *xyz)
        self.assertEqual(search.search(b'abc' + needle + b'def' + needle, xyz), {'offsets': [3, 18], 'truncated': False})
        self.assertEqual(search.search(needle, (1.5, -2, 3.5)), {'offsets': [], 'truncated': False})


if __name__ == '__main__':
    unittest.main()
