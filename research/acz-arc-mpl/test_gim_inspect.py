import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('gim_inspect', Path(__file__).with_name('gim_inspect.py'))
gim = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gim)


def fixture(width=2, height=2):
    body = bytearray(gim.HEADER_BYTES + width * height + gim.PALETTE_HEADER_BYTES + gim.PALETTE_BYTES)
    body[:4] = b'GIM\0'
    struct.pack_into('<HH', body, 0x1c, width, height)
    body[gim.HEADER_BYTES:gim.HEADER_BYTES + width * height] = bytes((0, 8, 16, 255))
    palette = gim.HEADER_BYTES + width * height + gim.PALETTE_HEADER_BYTES
    for index in range(256):
        body[palette + index * 4:palette + index * 4 + 4] = bytes((index, 0, 255 - index, 255))
    return bytes(body)


class GimInspectTests(unittest.TestCase):
    def test_parse_and_candidates(self):
        image = gim.parse_gim(fixture())
        self.assertEqual((image['width'], image['height']), (2, 2))
        self.assertEqual(gim.rgba(image)[:4], bytes((255, 0, 0, 255)))
        self.assertNotEqual(gim.csm1_index(8), 8)
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / 'fixture.gim'
            source.write_bytes(fixture())
            output = Path(temp) / 'out'
            metadata = gim.inspect_file(source, output)
            self.assertEqual(metadata['source_sha256'], hashlib.sha256(fixture()).hexdigest())
            self.assertTrue((output / 'linear_palette.png').read_bytes().startswith(b'\x89PNG'))
            self.assertTrue((output / 'csm1_palette_candidate.png').read_bytes().startswith(b'\x89PNG'))

    def test_refuses_extra_or_invalid_layout(self):
        with self.assertRaisesRegex(ValueError, 'unsupported GIM layout'):
            gim.parse_gim(fixture() + b'bad')
        with self.assertRaisesRegex(ValueError, 'missing GIM magic'):
            gim.parse_gim(b'bad!' + fixture()[4:])

    def test_reads_a_gim_part_without_extracting_it(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / 'fixture.mpl'
            offset = 8
            source.write_bytes(struct.pack('<I', 1) + struct.pack('<I', offset) + fixture())
            output = Path(temp) / 'out'
            metadata = gim.inspect_mpl_part(source, 0, output)
            self.assertEqual(metadata['source_context']['part_offset'], offset)
            self.assertEqual(metadata['source_context']['part_length'], len(fixture()))


if __name__ == '__main__':
    unittest.main()
