"""Synthetic tests for world_objects.py (no game data).  python -I test_world_objects.py"""
import os, struct, sys, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import world_objects as wo


def pdl_bytes(recs):
    b = struct.pack('<I12x', len(recs))
    for x, y, z, m in recs:
        b += struct.pack('<fffI', x, y, z, m)
    return b


def wad_block(lists):
    """lists: {(type, k): [u16...]} -> (288-byte block, wpd u16 list)"""
    off = [0xFFFFFFFF] * 48; cnt = [0] * 48; wpd = []
    for (t, k), v in sorted(lists.items()):
        off[t * 16 + k] = len(wpd); cnt[t * 16 + k] = len(v); wpd += v
    return struct.pack('<48I48H', *off, *cnt), wpd


class PdlTests(unittest.TestCase):
    def test_round_trip(self):
        a = wo.read_pdl(pdl_bytes([(1.5, 2.0, -3.0, 7), (100.0, 0.0, 5.0, 0)]))
        self.assertEqual(len(a), 2)
        self.assertEqual(a['model'].tolist(), [7, 0])
        self.assertAlmostEqual(float(a['x'][0]), 1.5)

    def test_empty(self):
        self.assertEqual(len(wo.read_pdl(b'\0\0\0\0')), 0)

    def test_size_mismatch(self):
        with self.assertRaises(wo.FormatError):
            wo.read_pdl(pdl_bytes([(1, 2, 3, 4)]) + b'\0' * 16)
        with self.assertRaises(wo.FormatError):
            wo.read_pdl(pdl_bytes([(1, 2, 3, 4)])[:-1])

    def test_nan(self):
        with self.assertRaises(wo.FormatError):
            wo.read_pdl(pdl_bytes([(float('nan'), 0, 0, 0)]))


class GridTests(unittest.TestCase):
    def test_pml_pmi(self):
        pml = struct.pack('<8H', *[0xFFFF] * 8) + struct.pack('<8H', 3, 1, *[0xFFFF] * 6)
        self.assertEqual(wo.read_pml(pml), [[], [3, 1]])
        g = np.zeros((256, 256), '<u2'); g[10, 20] = 1
        self.assertEqual(int(wo.read_pmi(g.tobytes())[10, 20]), 1)
        self.assertIsNone(wo.read_pmi(b'\0' * 4))
        with self.assertRaises(wo.FormatError):
            wo.read_pmi(b'\0' * 100)


class TreeTests(unittest.TestCase):
    def test_positions(self):
        # block 1: tree type 1 (tree2), sub-square k = 6 (kx 2, kz 1), texel x = 3, z = 200
        b0, w0 = wad_block({})
        b1, w1 = wad_block({(1, 6): [(200 << 8) | 3]})
        wad = b0 + b1
        wpd = struct.pack('<%dH' % len(w1), *w1)
        mta = np.zeros((16, 16), np.uint8); mta[2, 3] = 1          # chunk row 2 (z), col 3 (x) uses block 1
        t = wo.read_trees(wad, wpd, mta)
        self.assertEqual(t.shape, (1, 3))
        x = -262144 + 3 * 32768 + 2 * 8192 + 3 * 32 + 16
        z = -262144 + 2 * 32768 + 1 * 8192 + 200 * 32 + 16
        self.assertEqual(t[0].tolist(), [x, z, 1.0])

    def test_block_reused(self):
        b, w = wad_block({(0, 0): [0, 1]})
        mta = np.zeros((16, 16), np.uint8)                         # every chunk uses block 0
        t = wo.read_trees(b, struct.pack('<2H', *w), mta)
        self.assertEqual(len(t), 2 * 256)

    def test_bad_sizes(self):
        b, w = wad_block({(0, 0): [0, 1, 2]})
        with self.assertRaises(wo.FormatError):
            wo.read_trees(b, struct.pack('<2H', 0, 1), np.zeros((16, 16), np.uint8))   # list past the end of wpd
        with self.assertRaises(wo.FormatError):
            wo.read_trees(b[:-2], b'', np.zeros((16, 16), np.uint8))


class YawTests(unittest.TestCase):
    def test_yaw_recovered(self):
        rng = np.random.default_rng(3)
        A = rng.uniform(-50, 50, (12, 3))
        t = np.radians(37.0); c, s = np.cos(t), np.sin(t)
        B = np.stack([c * A[:, 0] + s * A[:, 2], A[:, 1], -s * A[:, 0] + c * A[:, 2]], 1)[::-1]   # order scrambled
        err, deg, mir = wo.yaw_fit(A, B)
        self.assertLess(err, 0.5); self.assertAlmostEqual(deg, 37.0, delta=0.3); self.assertEqual(mir, 1)


if __name__ == '__main__':
    unittest.main()
