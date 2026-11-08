import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('cockpit_export', Path(__file__).with_name('export_itesu_preview.py'))
exporter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(exporter)


class PreviewExporterTests(unittest.TestCase):
    def test_f16_never_reuses_itesu_hud_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, 'not been calibrated'):
                exporter.write_preview(Path(directory) / 'missing.mpl', Path(directory) / 'out',
                                       Path(directory) / 'hud.png', 'f16ck')

    def test_model_specific_preview_outputs_and_diagnostic_alpha(self):
        batch = {'offset': 1, 'positions_f32': [[0, 0, 0], [1, 0, 0], [0, 1, 0]],
                 'uvs_f32': [[0, 0], [1, 0], [0, 1]]}
        batches = {'batches': [batch], 'batch_count': 1, 'vertex_source_count': 3}
        texture = {'width': 1, 'height': 1}
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'out'
            with patch.object(exporter, 'validate_input', return_value=(batches, texture)), \
                 patch.object(exporter.gim_module, 'rgba', return_value=bytes([4, 8, 16, 0])):
                report = exporter.write_preview(Path(directory) / 'ignored', out, model='f16ck')
            self.assertEqual(report['model'], 'f16ck')
            self.assertEqual(report['hypothetical_triangle_count'], 1)
            self.assertTrue((out / 'f16ck-preview.obj').exists())
            self.assertTrue((out / 'f16ck-atlas-opaque-diagnostic.png').exists())
            html = (out / 'inspect-cockpit.html').read_text(encoding='utf-8')
            self.assertIn('Provisional f16ck geometry', html)
            self.assertIn('"off":1', html)
            self.assertNotIn('__VIEW_OPTIONS__', html)
            self.assertNotIn('__OPAQUE_URL__', html)

    def test_explicit_f16_mock_is_separate_per_batch(self):
        batch = {'offset': 0x191e0, 'positions_f32': [[0, 0, 0], [1, 0, 0], [0, 1, 0]],
                 'uvs_f32': [[0, 0], [1, 0], [0, 1]]}
        batches = {'batches': [batch], 'batch_count': 1, 'vertex_source_count': 3}
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'out'
            hud = Path(directory) / 'hud.png'
            hud.write_bytes(b'\x89PNG\r\n\x1a\n' + b'\0' * 8 + (2560).to_bytes(4, 'big') + (1440).to_bytes(4, 'big'))
            with patch.object(exporter, 'validate_input', return_value=(batches, {'width': 1, 'height': 1})), \
                 patch.object(exporter.gim_module, 'rgba', return_value=bytes([4, 8, 16, 0])):
                exporter.write_preview(Path(directory) / 'ignored', out, hud, 'f16ck', experimental_f16_mock=True)
            html = (out / 'inspect-cockpit.html').read_text(encoding='utf-8')
            self.assertIn('"offset":102880', html)  # 0x191e0
            self.assertIn('"offset":103044', html)  # 0x19284
            self.assertIn('mockScreens.get(t.batch.off)', html)


if __name__ == '__main__':
    unittest.main()
