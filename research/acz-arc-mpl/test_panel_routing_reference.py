import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('panel_reference', Path(__file__).with_name('panel_routing_reference.py'))
reference = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference)


class PanelRoutingTests(unittest.TestCase):
    def setUp(self):
        self.panels = [{'id': 'radar', 'kind': 'radar', 'crop': [0, 50, 20, 20], 'surface_valid': True},
                       {'id': 'stores', 'kind': 'stores_status', 'crop': [70, 50, 20, 20], 'surface_valid': True}]
        self.flags = dict(matching_aircraft=True, frame_valid=True, hud_valid=True, stereo_valid=True, depth_valid=True,
                          markers_separated=True, radar_complete=True)

    def plan(self):
        return reference.routing_plan(100, 100, self.panels, [[30, 30, 40, 20]], **self.flags)

    def test_masks_only_after_all_panels_are_ready(self):
        self.assertEqual(len(self.plan()), 2)
        self.panels[1]['surface_valid'] = False
        self.assertEqual(self.plan(), [])
        self.panels[1]['surface_valid'] = True
        self.flags['depth_valid'] = False
        self.assertEqual(self.plan(), [])

    def test_refuses_crops_over_protected_hud(self):
        self.panels[0]['crop'] = [35, 35, 10, 10]
        self.assertEqual(self.plan(), [])

    def test_required_panels_and_distinct_crops_fail_closed(self):
        self.panels.pop()
        self.assertEqual(self.plan(), [])
        self.panels.append({'id': 'stores', 'kind': 'stores_status',
                            'crop': [10, 50, 20, 20], 'surface_valid': True})
        self.assertEqual(self.plan(), [])
        self.panels[1]['crop'] = [70, 50, 20, 20]
        self.flags['frame_valid'] = 'yes'
        self.assertEqual(self.plan(), [])
        self.flags['frame_valid'] = True
        self.panels[1]['surface_valid'] = 1
        self.assertEqual(self.plan(), [])

    def test_retains_original_hud_if_markers_or_radar_are_unproven(self):
        # Markers can appear anywhere on screen, including a radar/status crop;
        # rectangles alone cannot prove their preservation.
        self.flags['markers_separated'] = False
        self.assertEqual(self.plan(), [])
        self.flags['markers_separated'] = True
        self.flags['radar_complete'] = False
        self.assertEqual(self.plan(), [])

    def test_premultiplied_source_over(self):
        self.assertEqual(reference.premultiplied_over((0.25, 0, 0, 0.5), (0, 0.5, 0, 1)), (0.25, 0.25, 0, 1))
        self.assertEqual(reference.premultiplied_over((0, 0, 0, 0), (0.2, 0.3, 0.4, 1)), (0.2, 0.3, 0.4, 1))
        with self.assertRaises(ValueError):
            reference.premultiplied_over((0, 0, float('nan'), 1), (0, 0, 0, 1))

    def test_consumer_repairs_zero_stored_alpha_before_blend(self):
        # The captured HUD can contain colour with stored alpha zero. The
        # existing consumer raises alpha to the maximum shaded colour channel.
        shaded = reference.consumer_hud_shade((0.1, 0.6, 0.05, 0.0))
        self.assertEqual(shaded, (0.1, 0.6, 0.05, 0.6))
        blended = reference.premultiplied_over(shaded, (0.2, 0.2, 0.2, 1))
        self.assertAlmostEqual(blended[0], 0.18)
        self.assertAlmostEqual(blended[1], 0.68)
        self.assertAlmostEqual(blended[2], 0.13)
        self.assertEqual(blended[3], 1)

    def test_stereo_projection_and_fail_closed(self):
        quad = [[-0.2, 0.2, 0.5], [0.2, 0.2, 0.5], [0.2, -0.2, 0.5], [-0.2, -0.2, 0.5]]
        left = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0.03, 0, 0, 1]]
        right = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [-0.03, 0, 0, 1]]
        eyes = reference.project_stereo(quad, left, right)
        self.assertIsNotNone(eyes)
        self.assertAlmostEqual(eyes[0][0][0] - eyes[1][0][0], 0.06)
        behind = [row[:] for row in right]
        behind[3][3] = -1
        self.assertIsNone(reference.project_stereo(quad, left, behind))
        self.assertIsNone(reference.project_quad([quad[0], quad[2], quad[1], quad[3]], left))


if __name__ == '__main__':
    unittest.main()
