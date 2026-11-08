"""Minimal PNG read/write (8-bit RGB/RGBA/grey, non-interlaced) with numpy + zlib only."""
import struct, zlib
import numpy as np


def read_png(path):
    b = open(path, 'rb').read()
    if b[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    i = 8; idat = []; w = h = ct = None
    while i < len(b):
        n, t = struct.unpack_from('>I4s', b, i); d = b[i + 8:i + 8 + n]; i += 12 + n
        if t == b'IHDR':
            w, h, bd, ct, _, _, il = struct.unpack('>IIBBBBB', d)
            if bd != 8 or il:
                raise ValueError('only 8-bit non-interlaced PNG')
        elif t == b'IDAT':
            idat.append(d)
        elif t == b'IEND':
            break
    ch = {0: 1, 2: 3, 6: 4, 4: 2}[ct]
    raw = np.frombuffer(zlib.decompress(b''.join(idat)), np.uint8).reshape(h, 1 + w * ch)
    out = np.zeros((h, w * ch), np.int32)
    prev = np.zeros(w * ch, np.int32)
    for y in range(h):
        f = raw[y, 0]; x = raw[y, 1:].astype(np.int32)
        if f == 0:
            r = x
        elif f == 1:
            r = (np.cumsum(x.reshape(w, ch), 0) & 255).reshape(-1)       # Sub: running sum per channel
        elif f == 2:
            r = (x + prev) & 255
        else:
            r = np.zeros(w * ch, np.int32)
            for k in range(w):
                for c in range(ch):
                    j = k * ch + c
                    a = r[j - ch] if k else 0
                    up = prev[j]; cc = prev[j - ch] if k else 0
                    if f == 3:
                        r[j] = (x[j] + ((a + up) >> 1)) & 255
                    else:
                        p = a + up - cc; pa, pb, pc = abs(p - a), abs(p - up), abs(p - cc)
                        pr = a if (pa <= pb and pa <= pc) else (up if pb <= pc else cc)
                        r[j] = (x[j] + pr) & 255
        out[y] = r; prev = r
    return out.reshape(h, w, ch).astype(np.uint8)


def write_png(path, img):
    img = np.ascontiguousarray(img, dtype=np.uint8)
    if img.ndim == 2:
        img = img[..., None]
    h, w, ch = img.shape
    ct = {1: 0, 3: 2, 4: 6}[ch]
    raw = b''.join(b'\x00' + img[y].tobytes() for y in range(h))

    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, ct, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
