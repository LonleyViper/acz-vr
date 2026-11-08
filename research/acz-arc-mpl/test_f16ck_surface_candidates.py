import json
from pathlib import Path
import unittest


class F16CandidateTests(unittest.TestCase):
    def test_uv_overlap_is_explicit_and_aircraft_sides_are_unassigned(self):
        path = Path(__file__).parent / 'cockpit-profiles' / 'f16ck-display-surface-candidates.json'
        data = json.loads(path.read_text(encoding='utf-8'))
        self.assertEqual(data['model']['archive_model_id'], 'f16ck')
        self.assertIn('not a selectable runtime profile', data['status'])
        left, right = data['candidates']
        self.assertEqual(left['proposed_domain'], 'unassigned')
        self.assertEqual(right['proposed_domain'], 'unassigned')
        u0, v0, u1, v1 = left['uv_rect_normalized']
        a0, b0, a1, b1 = right['uv_rect_normalized']
        self.assertLess(u0, a0)
        self.assertGreater(u1, a1)
        self.assertLess(v0, b0)
        self.assertGreater(v1, b1)
        self.assertLess(left['position_bounds_source_lanes'][3], 0)
        self.assertGreater(right['position_bounds_source_lanes'][0], 0)


if __name__ == '__main__':
    unittest.main()
