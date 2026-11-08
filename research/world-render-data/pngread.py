"""Minimal PNG reader (8-bit RGB/RGBA, non-interlaced) -> numpy array. stdlib + numpy only (no PIL on this machine)."""
import struct, zlib
import numpy as np


def read_png(path):
    b = open(path, 'rb').read()
    if b[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos, idat, w = 8, [], None
    while pos < len(b):
        n, t = struct.unpack('>I4s', b[pos:pos + 8])
        v = b[pos + 8:pos + 8 + n]
        if t == b'IHDR':
            w, h, depth, ctype, _, _, inter = struct.unpack('>IIBBBBB', v)
            if depth != 8 or ctype not in (2, 6) or inter:
                raise ValueError('unsupported PNG (depth %d type %d interlace %d)' % (depth, ctype, inter))
            ch = 3 if ctype == 2 else 4
        elif t == b'IDAT':
            idat.append(v)
        pos += 12 + n
    raw = np.frombuffer(zlib.decompress(b''.join(idat)), np.uint8)
    stride = w * ch
    out = np.zeros((h, stride), np.int32)
    prev = np.zeros(stride, np.int32)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)].astype(np.int32)
        if f == 0:
            cur = line
        elif f == 2:
            cur = (line + prev) & 255
        elif f in (1, 3, 4):
            cur = np.zeros(stride, np.int32)
            for x in range(stride):
                a = cur[x - ch] if x >= ch else 0
                if f == 1:
                    p = a
                elif f == 3:
                    p = (a + prev[x]) >> 1
                else:
                    bb = prev[x]; c = prev[x - ch] if x >= ch else 0
                    pa, pb, pc = abs(bb - c), abs(a - c), abs(a + bb - 2 * c)
                    p = a if pa <= pb and pa <= pc else (bb if pb <= pc else c)
                cur[x] = (line[x] + p) & 255
        else:
            raise ValueError('bad filter')
        out[y] = cur
        prev = cur
    return out.reshape(h, w, ch).astype(np.uint8)
