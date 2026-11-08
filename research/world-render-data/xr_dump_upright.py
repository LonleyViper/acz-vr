"""Turn an xr_png_once dump (acz-vr-xr-eye0-f<N>-final.png) into the image in TRUE orientation (offline, numpy + stdlib).

  python -I xr_dump_upright.py <dump.png> <out.png>

Finding (D-298): the xr_png_once dumps are rotated by 180 degrees relative to the game's own view (sky at the bottom AND the cockpit mirrored
left/right: the numbered gauge that is on the LEFT in the game window is on the RIGHT, its digits are mirrored, the ball handle is on the wrong strut).
Undoing only the vertical flip leaves a horizontal mirror, which is how the shipped terrain's river looked "mirrored" (D-296/D-297). Rotate by
180 degrees (flip rows AND columns) to read a dump.
"""
import os, struct, sys, zlib
import numpy as np


def read_png(path):
    b = open(path, 'rb').read()
    i, idat = 8, b''
    while i < len(b):
        n = struct.unpack('>I', b[i:i + 4])[0]
        t, d = b[i + 4:i + 8], b[i + 8:i + 8 + n]
        if t == b'IHDR':
            w, h, bd, ct = struct.unpack('>IIBB', d[:10])
        elif t == b'IDAT':
            idat += d
        i += 12 + n
    assert bd == 8 and ct in (2, 6)
    bpp = 4 if ct == 6 else 3
    a = np.frombuffer(zlib.decompress(idat), np.uint8).reshape(h, w * bpp + 1)
    assert (a[:, 0] == 0).all(), 'filtered PNG not supported (the xr_png_once encoder writes filter 0)'
    return a[:, 1:].reshape(h, w, bpp)


def write_png(path, rgb):
    rgb = np.ascontiguousarray(rgb[..., :3], dtype=np.uint8)
    h, w = rgb.shape[:2]
    raw = b''.join(b'\x00' + rgb[y].tobytes() for y in range(h))

    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def upright(img):
    return img[::-1, ::-1]


if __name__ == '__main__':
    write_png(sys.argv[2], upright(read_png(sys.argv[1])))
