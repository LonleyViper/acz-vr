import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('coplanar', Path(__file__).with_name('find_coplanar_batches.py'))
coplanar = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coplanar)


class CoplanarTests(unittest.TestCase):
    def test_finds_only_contained_nearby_batches(self):
        corners = {name: {'position': position} for name, position in {
            'top_left': [0, 1, 0], 'top_right': [1, 1, 0],
            'bottom_left': [0, 0, 0], 'bottom_right': [1, 0, 0]}.items()}
        display = {'batch_offset': 10, 'corners_by_uv': corners}
        def batch(offset, points):
            return {'offset': offset, 'positions_f32': points, 'uvs_f32': [[0, 0] for _ in points]}
        candidates = [batch(10, [[0, 0, 0]]),
                      batch(11, [[.2, .3, .001], [.4, .3, .001], [.2, .4, .001]]),
                      batch(12, [[.2, .3, .1], [.4, .3, .1], [.2, .4, .1]]),
                      batch(13, [[.2, .3, 0], [1.1, .3, 0], [.2, .4, 0]])]
        found = coplanar.find_nearby(display, candidates, .004, .02)
        self.assertEqual([row['batch_offset'] for row in found], [11])
        self.assertAlmostEqual(abs(found[0]['signed_distance_source_units'][0]), .001)


if __name__ == '__main__':
    unittest.main()
