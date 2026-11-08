import struct
import unittest
import xml.etree.ElementTree as ET

from survey_capture_cpt_indexed import draws_with_state, pipeline_cut_modes, referenced_positions


class IndexedSurveyTests(unittest.TestCase):
    def test_referenced_positions_and_range_check(self):
        vertices = bytearray(3 * 64)
        for i in range(3):
            struct.pack_into('<3f', vertices, i * 64, i, i + 1, i + 2)
        indices = struct.pack('<4H', 0, 2, 65535, 1)
        draw = {'vb': ('329', 0, len(vertices), 64), 'ib': ('331', 0, len(indices), 57),
                'count': 3, 'start': 0, 'base': 0, 'topology': '5', 'cut_mode': '1'}
        result = referenced_positions(draw, vertices, indices)
        self.assertEqual(result['unique_referenced_vertices'], 2)
        self.assertEqual(result['index_sentinel_count'], 1)
        self.assertEqual(result['strip_length_counts'], {2: 1})
        self.assertEqual(result['bounds'][0], [0, 2])
        draw['base'] = 1
        self.assertIn('error', referenced_positions(draw, vertices, indices))
        draw['start'] = 0
        draw['cut_mode'] = '0'
        self.assertIn('error', referenced_positions(draw, vertices, indices))
        draw['base'] = 0
        draw['start'] = 2
        self.assertIn('error', referenced_positions(draw, vertices, indices))

    def test_reset_discards_index_and_vertex_bindings(self):
        xml = '''<chunks>
        <chunk name="x::Reset"><ResourceId name="CommandList">1</ResourceId><ResourceId name="pInitialState">198101</ResourceId></chunk>
        <chunk name="x::DrawIndexedInstanced" chunkIndex="1"><ResourceId name="pCommandList">1</ResourceId>
        <uint name="IndexCountPerInstance">1</uint><uint name="StartIndexLocation">0</uint><int name="BaseVertexLocation">0</int></chunk>
        </chunks>'''
        draws = list(draws_with_state(ET.fromstring(xml)))
        self.assertEqual(len(draws), 1)
        self.assertIsNone(draws[0]['vb'])
        self.assertIsNone(draws[0]['ib'])
        self.assertIn('error', referenced_positions(draws[0], b'', b''))

    def test_pso_cut_mode_and_topology_are_recorded(self):
        xml = '''<chunks>
        <chunk name="x::CreateGraphicsPipeline"><ResourceId name="pPipelineState">198101</ResourceId>
        <struct name="desc"><enum name="IBStripCutValue">1</enum></struct></chunk>
        <chunk name="x::Reset"><ResourceId name="CommandList">1</ResourceId><ResourceId name="pInitialState">198101</ResourceId></chunk>
        <chunk name="x::IASetPrimitiveTopology"><ResourceId name="pCommandList">1</ResourceId><enum name="PrimitiveTopology">5</enum></chunk>
        <chunk name="x::DrawIndexedInstanced" chunkIndex="1"><ResourceId name="pCommandList">1</ResourceId>
        <uint name="IndexCountPerInstance">1</uint><uint name="StartIndexLocation">0</uint><int name="BaseVertexLocation">0</int></chunk>
        </chunks>'''
        chunks = ET.fromstring(xml)
        self.assertEqual(pipeline_cut_modes(chunks), {'198101': '1'})
        self.assertEqual(list(draws_with_state(chunks))[0]['topology'], '5')


if __name__ == '__main__':
    unittest.main()
