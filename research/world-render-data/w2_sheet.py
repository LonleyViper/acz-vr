"""WORLD-2: stack PPM renders (terrain_offline_tests with ACZ_W2_RENDER_DIR) and optionally a game PNG into one PNG sheet; prints sky/ground medians.
Usage: python -I w2_sheet.py <out.png> [--game <shot.png> <crop x0,y0,x1,y1>] <a.ppm> <b.ppm> ...   (numpy + stdlib only)"""
import sys, os
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import env_gim
from pngread import read_png


def read_ppm(p):
    b = open(p, 'rb').read()
    parts = b.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return np.frombuffer(parts[3], np.uint8)[:w * h * 3].reshape(h, w, 3)


def fit(img, w):
    h = img.shape[0] * w // img.shape[1]
    ys = np.arange(h) * img.shape[0] // h
    xs = np.arange(w) * img.shape[1] // w
    return img[ys][:, xs]


def main(a):
    out = a[0]
    rest = a[1:]
    rows = []
    if rest and rest[0] == '--game':
        g = read_png(rest[1])[..., :3]
        x0, y0, x1, y1 = map(int, rest[2].split(','))
        rows.append(('game', g[y0:y1, x0:x1]))
        rest = rest[3:]
    for p in rest:
        rows.append((os.path.basename(p), read_ppm(p)))
    W = 968
    tiles = []
    for name, im in rows:
        f = fit(im, W)
        h = f.shape[0]
        print('%-34s sky(top 10%%) %s  mid-row %s  ground(bottom 15%%) %s' % (name, np.median(f[:h // 10].reshape(-1, 3), 0), np.median(f[h // 2 - 3:h // 2 + 3].reshape(-1, 3), 0),
                                                                            np.median(f[-h * 15 // 100:].reshape(-1, 3), 0)))
        tiles.append(np.pad(f, ((3, 3), (0, 0), (0, 0)), constant_values=255))
    img = np.concatenate(tiles, 0)
    h, w, _ = img.shape
    env_gim.write_png(out, w, h, np.concatenate([img, np.full((h, w, 1), 255, np.uint8)], 2).tobytes())


if __name__ == '__main__':
    main(sys.argv[1:])
