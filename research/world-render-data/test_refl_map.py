"""Offline tests of refl_map.py (synthetic GIM, no game data)."""
import os, struct, sys, tempfile, unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import refl_map


def synthetic_gim(w=4, h=2, flag=0):
    """8-bit GIM in the layout env_gim.parse reads: pixels at 0x20, then a 16-byte palette header and 256 RGBA entries."""
    hdr = bytearray(0x20)
    hdr[:4] = b'GIM\0'; hdr[0x12] = flag; hdr[0x13] = 0x13
    struct.pack_into('<HH', hdr, 0x1c, w, h)
    px = bytes(range(w * h))
    clut = b''.join(bytes((i, 255 - i, 7, 0x80 if i % 2 == 0 else 0x40)) for i in range(256))
    return bytes(hdr) + px + bytes(16) + clut


class ReflMapTest(unittest.TestCase):
    def test_round_trip_and_alpha_scale(self):
        w, h, px = refl_map.from_gim(synthetic_gim())
        self.assertEqual((w, h), (4, 2))
        self.assertEqual(px[0:4], bytes((0, 255, 7, 255)))      # alpha 0x80 -> 255
        self.assertEqual(px[4:8], bytes((1, 254, 7, 128)))      # alpha 0x40 -> 128
        b = refl_map.encode(w, h, px)
        self.assertEqual(len(b), refl_map.HEADER + w * h * 4)
        self.assertEqual(refl_map.decode(b), (w, h, px))

    def test_refusals(self):
        b = bytearray(refl_map.encode(1, 1, bytes(4)))
        for mut in (lambda x: x.__setitem__(0, 0), lambda x: x.__setitem__(4, 2), lambda x: x.__setitem__(20, x[20] ^ 1)):
            c = bytearray(b); mut(c)
            with self.assertRaises(ValueError):
                refl_map.decode(bytes(c))
        with self.assertRaises(ValueError):
            refl_map.decode(bytes(b) + b'\0')
        with self.assertRaises(ValueError):
            refl_map.encode(65, 1, bytes(65 * 4))

    def test_bake_tree(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, 'data', 'map', 'x01a'))
            with open(os.path.join(d, 'data', 'map', 'x01a', 'x01a_ref.gim'), 'wb') as f:
                f.write(synthetic_gim(flag=1))
            out = os.path.join(d, 'out')
            r = refl_map.bake(os.path.join(d, 'data'), out)
            self.assertEqual([x[0] for x in r], ['x01a'])
            w, h, px = refl_map.decode(open(os.path.join(out, 'x01a_ref.zvrt'), 'rb').read())
            self.assertEqual((w, h), (4, 2))
            # flag 1: CSM1 CLUT order (index 0..7 unchanged, 8 <-> 16): pixel 0 still entry 0
            self.assertEqual(px[0:3], bytes((0, 255, 7)))


if __name__ == '__main__':
    unittest.main()
