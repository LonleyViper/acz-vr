import importlib.util
from pathlib import Path
import unittest


spec = importlib.util.spec_from_file_location('display_surfaces', Path(__file__).with_name('inspect_display_surfaces.py'))
surfaces = importlib.util.module_from_spec(spec)
spec.loader.exec_module(surfaces)


class DisplaySurfaceTests(unittest.TestCase):
    def test_pairs_uv_corners_without_assuming_source_order(self):
        batch = {'positions_f32': [[1, 0, 0], [0, 1, 0], [0, 0, 0], [1, 1, 0]],
                 'uvs_f32': [[1, 1], [0, 0], [0, 1], [1, 0]]}
        found = surfaces.describe(batch)
        self.assertEqual(found['corners_by_uv']['top_left']['position'], [0, 1, 0])
        self.assertEqual(found['corners_by_uv']['bottom_right']['position'], [1, 0, 0])
        self.assertEqual(found['centre'], [0.5, 0.5, 0])
        self.assertEqual(found['fourth_corner_plane_distance_source_units'], 0)

    def test_duplicate_uv_corner_refused(self):
        batch = {'positions_f32': [[0, 0, 0], [1, 0, 0], [0, 1, 0], [1, 1, 0]],
                 'uvs_f32': [[0, 0], [0, 0], [0, 1], [1, 1]]}
        with self.assertRaisesRegex(ValueError, 'distinct corners'):
            surfaces.describe(batch)


if __name__ == '__main__':
    unittest.main()
