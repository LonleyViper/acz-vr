import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('stereo_fixture', Path(__file__).with_name('inspect_stereo_panel_fixture.py'))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


class StereoFixtureTests(unittest.TestCase):
    def test_fixture_projects_both_eyes_in_perimeter_order(self):
        corners = {'top_left': {'position': [-0.9, 2.8, -35]},
                   'top_right': {'position': [-0.5, 2.8, -35]},
                   'bottom_right': {'position': [-0.5, 2.4, -35]},
                   'bottom_left': {'position': [-0.9, 2.4, -35]}}
        fake = {'model': {'mpl_sha256': 'fixture'}, 'displays': [
            {'id': 'panel', 'batch_offset': 123, 'corners_by_uv': corners}]}
        with patch.object(fixture.surfaces, 'inspect', return_value=fake):
            result = fixture.fixture(Path('unused'), {})
        panel = result['panels'][0]
        self.assertEqual(panel['source_batch_offset'], 123)
        self.assertEqual(len(panel['left_ndc']), 4)
        self.assertEqual(len(panel['right_ndc']), 4)
        self.assertGreater(panel['left_ndc'][0][0], panel['right_ndc'][0][0])
        self.assertAlmostEqual(panel['left_ndc'][0][1], panel['right_ndc'][0][1])


if __name__ == '__main__':
    unittest.main()
