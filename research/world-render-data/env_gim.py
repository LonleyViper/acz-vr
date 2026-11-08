"""WORLD-2 (D-302): decode the per-stage environment GIMs (skytex<stage>.bin pack, _sea/_ref/_dt.gim) to PNG.

Read-only on already-extracted files (arcread.py extract); writes PNGs to an output dir outside the repo.
GIM layout as observed in these files [E]: 'GIM\\0', u8 @0x12 = flag (0/1, see csm1), u8 @0x13 = 0x13 (8-bit) or 0x14 (4-bit),
u16 w,h @0x1c; pixels from 0x20 (4-bit: low nibble first); then a 16-byte palette header and 256 (8-bit) or 16 (4-bit) RGBA
entries, alpha 0x80 = opaque (PS2 convention).  The flag @0x12 = 1 is tested as the PS2 CSM1 8-bit CLUT group swap.
Usage: python -I env_gim.py <out_dir> <file.gim|skytex.bin> ...
"""
import struct, sys, zlib
from pathlib import Path


def csm1(i):
    return (i & 0xe7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)


def parse(d):
    if d[:4] != b'GIM\0':
        raise ValueError('no GIM magic')
    flag, bpp = d[0x12], d[0x13]
    w, h = struct.unpack_from('<HH', d, 0x1c)
    if bpp == 0x13:
        npx, ncl = w * h, 256
    elif bpp == 0x14:
        npx, ncl = (w * h + 1) // 2, 16
    else:
        raise ValueError('unknown bpp %#x' % bpp)
    pal = 0x20 + npx + 0x10
    if len(d) < pal + ncl * 4:
        raise ValueError('short GIM')
    clut = [tuple(d[pal + 4 * k:pal + 4 * k + 4]) for k in range(ncl)]
    px = d[0x20:0x20 + npx]
    if bpp == 0x14:
        idx = []
        for b in px:
            idx += [b & 15, b >> 4]
        idx = idx[:w * h]
    else:
        idx = list(px)
    return dict(w=w, h=h, flag=flag, bpp=8 if bpp == 0x13 else 4, idx=idx, clut=clut, size=pal + ncl * 4)


def rgba(g, swap=None):
    if swap is None:
        swap = g['flag'] == 1 and g['bpp'] == 8
    out = bytearray()
    for i in g['idx']:
        r, gg, b, a = g['clut'][csm1(i) if swap else i]
        out += bytes((r, gg, b, min(255, a * 2)))
    return bytes(out)


def write_png(path, w, h, px):
    def chunk(n, v):
        return struct.pack('>I', len(v)) + n + v + struct.pack('>I', zlib.crc32(n + v) & 0xffffffff)
    raw = b''.join(b'\0' + px[y * w * 4:(y + 1) * w * 4] for y in range(h))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def gims_in(data):
    if data[:4] == b'GIM\0':
        return [(0, data)]
    n = struct.unpack_from('<I', data, 0)[0]
    if not 0 < n < 64:
        raise ValueError('not a GIM pack')
    offs = list(struct.unpack_from('<%dI' % n, data, 4)) if False else None
    offs = [struct.unpack_from('<I', data, 4 + 4 * k)[0] for k in range(n)]
    offs = [o for o in offs if o]
    ends = offs[1:] + [len(data)]
    return [(k, data[o:e]) for k, (o, e) in enumerate(zip(offs, ends))]


def main():
    out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
    for f in sys.argv[2:]:
        data = Path(f).read_bytes()
        for k, d in gims_in(data):
            g = parse(d)
            stem = '%s_%02d' % (Path(f).stem, k)
            px = rgba(g)
            write_png(out / (stem + '.png'), g['w'], g['h'], px)
            al = [g['clut'][i][3] for i in set(g['idx'])]
            print('%s %dx%d %dbit flag=%d clut_alpha=%d..%d' % (stem, g['w'], g['h'], g['bpp'], g['flag'], min(al), max(al)))


if __name__ == '__main__':
    main()
