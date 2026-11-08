import json
from pathlib import Path
import unittest


SOURCE = Path(__file__).parent / 'cockpit-profiles' / 'itesu-display-surface-candidates.json'


class ItesuSurfaceCandidatesTests(unittest.TestCase):
    def test_candidate_record_is_internally_coherent(self):
        record = json.loads(SOURCE.read_text(encoding='utf-8'))
        self.assertEqual(record['schema'], 'acz-cockpit-display-surface-candidates/v1')
        self.assertEqual(record['status'], 'offline candidate evidence only; not a selectable runtime profile')
        self.assertEqual(record['model']['texture_size'], [512, 512])
        self.assertEqual(len(record['candidates']), 2)
        self.assertEqual({candidate['id'] for candidate in record['candidates']}, {
            'port-forward-green-display', 'starboard-forward-green-display'})
        for candidate in record['candidates']:
            u0, v0, u1, v1 = candidate['uv_rect_normalized']
            x, y, width, height = candidate['atlas_rect_pixels']
            self.assertLess(u0, u1)
            self.assertLess(v0, v1)
            self.assertAlmostEqual(x, u0 * 512, places=2)
            self.assertAlmostEqual(y, v0 * 512, places=2)
            self.assertAlmostEqual(width, (u1 - u0) * 512, places=2)
            self.assertAlmostEqual(height, (v1 - v0) * 512, places=2)
            self.assertEqual(candidate['source_vertex_count'], 4)
            self.assertEqual(len(candidate['position_bounds_source_lanes']), 6)


if __name__ == '__main__':
    unittest.main()
