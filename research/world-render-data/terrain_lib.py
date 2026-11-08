"""Stage terrain loader for Ace Combat Zero (offline, numpy only, throwaway research code).

Reads files already extracted by arcread.py (default D:/acz-extract/a/data/map/<stage>/).
Data model (see docs/world-render/terrain-preview-and-design.md):
  .mha  16x16 u8   chunk index into .mhd (33x33 u8 height chunks)  -> 513x513 heightfield
  .mta  16x16 u8   per-chunk TEXTURE BLOCK number (0..B-1)
  .mti  B*256 u16  for block b: 16x16 tile words, row-major; word = tile id | flag bits
  .mtd  N tiles    64x64 8-bit (+ 32/16/8 mips)      .mtc 256-entry CLUT
"""
import os, struct, zlib
import numpy as np

ROOT = os.environ.get('ACZ_EXTRACT', 'D:/acz-extract/a/data/map')
CELL = 1024.0       # world units per heightfield cell
HSTEP = 80.0        # world units per height byte
HALF = 262144.0     # terrain spans [-HALF, +HALF] on X and Z


def write_png(path, rgb):
    rgb = np.ascontiguousarray(rgb, dtype=np.uint8)
    h, w = rgb.shape[:2]
    if rgb.ndim == 2:
        rgb = np.repeat(rgb[:, :, None], 3, 2)
    raw = b''.join(b'\x00' + rgb[y].tobytes() for y in range(h))

    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def csm1_order():
    """Index permutation of the PS2 CSM1 CLUT order (swap bits 3 and 4); the native default (terrain_stage.h clut_entry, W5-3 P2 #2)."""
    i = np.arange(256)
    return (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)


class Stage:
    """clut_raw=False (default, = the native decode): self.clut is the palette in CSM1-resolved order, so tile texel t selects raw .mtc entry csm1(t).
    clut_raw=True (or env ACZ_CLUT_RAW=1): the pre-K00A-SNOW identity order (A/B only). self.clut_stored always holds the stored .mtc bytes."""
    def __init__(self, name, root=ROOT, clut_raw=None):
        self.name = name
        p = os.path.join(root, name, name)
        rd = lambda e: open(p + '.' + e, 'rb').read()
        mhd = np.frombuffer(rd('mhd'), np.uint8)
        self.chunks = mhd.reshape(-1, 33, 33)
        self.mha = np.frombuffer(rd('mha'), np.uint8).reshape(16, 16)
        self.mta = np.frombuffer(rd('mta'), np.uint8).reshape(16, 16)
        self.mti = np.frombuffer(rd('mti'), '<u2')
        self.clut_stored = np.frombuffer(rd('mtc'), np.uint8).reshape(256, 4)[:, :3]
        self.clut_raw = (os.environ.get('ACZ_CLUT_RAW') == '1') if clut_raw is None else bool(clut_raw)
        self.clut = self.clut_stored if self.clut_raw else self.clut_stored[csm1_order()]
        d = rd('mtd')
        n = struct.unpack('<I', d[:4])[0]
        self.n = n
        offs = [0x20, 0x20 + n * 4096, 0x20 + n * 5120, 0x20 + n * 5376]
        self.tiles = [np.frombuffer(d, np.uint8, n * s * s, o).reshape(n, s, s)
                      for o, s in zip(offs, (64, 32, 16, 8))]
        self.pdl = self._pdl(rd('pdl'))
        self._hf = None

    @staticmethod
    def _pdl(b):
        cnt = struct.unpack('<I', b[:4])[0]
        if cnt == 0 or len(b) < 16 + 16 * cnt:
            return np.zeros((0, 4))
        a = np.frombuffer(b, np.dtype([('x', '<f4'), ('y', '<f4'), ('z', '<f4'), ('m', '<u4')]), cnt, 16)
        return np.stack([a['x'], a['y'], a['z'], a['m'].astype(np.float32)], 1)

    @property
    def heightfield(self):
        """513x513 float array of height bytes, [row(z), col(x)]."""
        if self._hf is None:
            hf = np.zeros((513, 513), np.float32)
            for r in range(16):
                for c in range(16):
                    hf[r * 32:r * 32 + 33, c * 32:c * 32 + 33] = self.chunks[self.mha[r, c]]
            self._hf = hf
        return self._hf

    # ---- tile mapping (D-293 finding) ----
    def tile_words(self):
        """256x256 array of u16 tile words for the whole stage (16x16 tiles per 32-cell chunk)."""
        w = np.zeros((16, 16, 16, 16), np.uint16)
        blocks = self.mti.reshape(-1, 16, 16)
        for r in range(16):
            for c in range(16):
                w[r, c] = blocks[self.mta[r, c]]
        return w.transpose(0, 2, 1, 3).reshape(256, 256)

    def tile_image(self, word, mip=0, flagmode=0):
        t = self.tiles[mip][word & 0x3ff]
        f = word >> 10
        if flagmode == 1:
            if f & 1: t = t[:, ::-1]
        return self.clut[t]

    def height_at(self, x, z):
        """bilinear world height (units) at world x,z (arrays ok)."""
        hf = self.heightfield
        u = np.clip((np.asarray(x) + HALF) / CELL, 0, 511.999)
        v = np.clip((np.asarray(z) + HALF) / CELL, 0, 511.999)
        i, j = u.astype(int), v.astype(int)
        fu, fv = u - i, v - j
        h = (hf[j, i] * (1 - fu) * (1 - fv) + hf[j, i + 1] * fu * (1 - fv)
             + hf[j + 1, i] * (1 - fu) * fv + hf[j + 1, i + 1] * fu * fv)
        return h * HSTEP
