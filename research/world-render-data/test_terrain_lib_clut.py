"""W5-3 P2 #2: the research terrain loader applies the native CSM1 palette order by default (terrain_stage.h clut_entry); raw is explicit."""
import os, struct, sys, tempfile, unittest
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import terrain_lib


def native_clut_entry(idx):   # terrain_stage.h clut_entry(idx, raw=false)
    return (idx & 0xE7) | ((idx & 0x08) << 1) | ((idx & 0x10) >> 1)


def make_stage(root):
    d = os.path.join(root, 's'); os.makedirs(d)
    def w(e, b):
        with open(os.path.join(d, 's.' + e), 'wb') as f: f.write(b)
    w('mhd', bytes(33 * 33)); w('mha', bytes(256)); w('mta', bytes(256)); w('mti', bytes(512))
    mtc = bytearray(1024)
    for i in range(256):
        mtc[4 * i:4 * i + 3] = bytes([i, 255 - i, (i * 7) & 255])   # every entry distinct
    w('mtc', bytes(mtc))
    n = 1
    mtd = bytearray(0x20 + n * (4096 + 1024 + 256 + 64)); mtd[:4] = struct.pack('<I', n)
    for i in range(4096): mtd[0x20 + i] = i & 255          # mip 0 tile: texel index = (x + 64y) mod 256
    w('mtd', bytes(mtd)); w('pdl', bytes(16))


class ClutOrder(unittest.TestCase):
    def test_default_is_csm1_and_raw_is_identity(self):
        with tempfile.TemporaryDirectory() as root:
            make_stage(root)
            st = terrain_lib.Stage('s', root); raw = terrain_lib.Stage('s', root, clut_raw=True)
            self.assertFalse(st.clut_raw); self.assertTrue(raw.clut_raw)
            for i in range(256):
                self.assertEqual(tuple(st.clut[i]), tuple(raw.clut[native_clut_entry(i)]))
                self.assertEqual(tuple(raw.clut[i]), tuple(st.clut_stored[i]))
            # texel index 8 resolves to stored entry 16, 40 to 48
            self.assertEqual(tuple(st.tile_image(0)[0, 8]), tuple(st.clut_stored[16]))
            self.assertEqual(tuple(st.clut[40]), tuple(st.clut_stored[48]))
            self.assertNotEqual(tuple(st.clut[8]), tuple(raw.clut[8]))

    def test_env_override(self):
        with tempfile.TemporaryDirectory() as root:
            make_stage(root); os.environ['ACZ_CLUT_RAW'] = '1'
            try: self.assertTrue(terrain_lib.Stage('s', root).clut_raw)
            finally: del os.environ['ACZ_CLUT_RAW']


if __name__ == '__main__':
    unittest.main()
