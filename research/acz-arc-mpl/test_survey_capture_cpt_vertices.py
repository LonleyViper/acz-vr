import importlib.util
from pathlib import Path
import struct
import unittest
import xml.etree.ElementTree as ET

spec = importlib.util.spec_from_file_location('capture_survey', Path(__file__).with_name('survey_capture_cpt_vertices.py'))
survey = importlib.util.module_from_spec(spec)
spec.loader.exec_module(survey)


class CaptureSurveyTests(unittest.TestCase):
    def test_reset_clears_old_buffer_and_pso(self):
        xml = '''<chunks>
          <chunk chunkIndex="1" name="x::SetPipelineState"><ResourceId name="pCommandList">2</ResourceId><ResourceId name="pPipelineState">198101</ResourceId></chunk>
          <chunk chunkIndex="2" name="x::IASetVertexBuffers"><ResourceId name="pCommandList">2</ResourceId><uint name="StartSlot">0</uint><array name="pViews"><struct><struct name="BufferLocation"><ResourceId name="Buffer">329</ResourceId><uint name="Offset">0</uint></struct><uint name="SizeInBytes">64</uint><uint name="StrideInBytes">64</uint></struct></array></chunk>
          <chunk chunkIndex="3" name="x::DrawInstanced"><ResourceId name="pCommandList">2</ResourceId></chunk>
          <chunk chunkIndex="4" name="x::Reset"><ResourceId name="CommandList">2</ResourceId><ResourceId name="pInitialState">0</ResourceId></chunk>
          <chunk chunkIndex="5" name="x::DrawInstanced"><ResourceId name="pCommandList">2</ResourceId></chunk>
        </chunks>'''
        draws = survey.selected_draws(ET.fromstring(xml))
        self.assertEqual(len(draws), 1)
        self.assertEqual(draws[0]['view'], ('329', 0, 64, 64))

    def test_view_must_be_bounded_finite_and_expected_resource(self):
        blob = struct.pack('<3f', 1, 2, 3) + bytes(52)
        view = survey.summarize_view(blob, ('329', 0, 64, 64))
        self.assertEqual(view['position_bounds'], [[1, 1], [2, 2], [3, 3]])
        self.assertIsNone(survey.summarize_view(blob, ('329', 1, 64, 64)))
        self.assertIsNone(survey.summarize_view(blob, ('999', 0, 64, 64)))


if __name__ == '__main__':
    unittest.main()
