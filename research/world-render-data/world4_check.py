"""WORLD-4: check the C++ CPU reference renders of the step62 pose (terrain_offline_tests with ACZ_W4_RENDER_DIR: env:1 with look:0 / look:1)
against the game window of the same frame (cockpit view, divided by the canopy glass factor) -- per distance band (ground) and per elevation row (sky).

  python -I world4_check.py [--dir D:/acz-extract/out/world4] [--glass 0.951,0.94,0.935]

Writes <dir>/step62_look_sheet.png (game / look:0 / look:1, half resolution) and prints the table used in docs/world-render/world4-impl.md.
"""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import png_io
import world4_compare as wc
from world4_fit import EXP, R62, POSE62


def read_ppm(p):
    b = open(p, 'rb').read()
    parts = b.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return np.frombuffer(parts[3], np.uint8).reshape(h, w, 3).astype(float)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', default='D:/acz-extract/out/world4'); ap.add_argument('--glass', default='0.951,0.94,0.935')
    g = ap.parse_args(argv)
    glass = np.array([float(v) for v in g.glass.split(',')])
    d = wc.prepare(EXP + 'step62-integrated-run1/shots/acz-20261027-004711-116-T6.75.png', POSE62, R62, 3, 160000)
    H, W = d['H'], d['W']
    imgs = {k: read_ppm(os.path.join(g.dir, 'm02a-step62-env-look%d.ppm' % k))[:H, :W] for k in (0, 1)}
    game = d['g'] / glass
    t = np.where(d['tr']['hit'], d['tr']['t'], np.inf)
    ok = d['ok']
    el = np.degrees(np.arcsin(np.clip(d['tr']['d'][..., 1], -1, 1)))
    print('ground (mean rgb over usable pixels; game / glass):')
    print('  band            n     game            look:0          look:1')
    for lo, hi in [(11000, 16000), (16000, 22000), (22000, 30000), (30000, 42000), (42000, 60000), (60000, 160000)]:
        m = ok & (t >= lo) & (t < hi)
        if m.sum() < 100:
            continue
        f = lambda a: np.round(a[m].mean(0)).astype(int)
        print('  %6d-%6d %6d %s %s %s' % (lo, hi, m.sum(), f(game), f(imgs[0]), f(imgs[1])))
    print('sky rows (median rgb; usable sky pixels):')
    err = {0: [], 1: []}
    for r0 in range(0, H, 24):
        m = ok[r0:r0 + 24] & ~d['tr']['hit'][r0:r0 + 24]
        if m.sum() < 100:
            continue
        e = float(np.median(el[r0:r0 + 24][m]))
        gg = np.median(game[r0:r0 + 24][m], axis=0)
        a0 = np.median(imgs[0][r0:r0 + 24][m], axis=0); a1 = np.median(imgs[1][r0:r0 + 24][m], axis=0)
        err[0].append(np.abs(a0 - gg).max()); err[1].append(np.abs(a1 - gg).max())
        print('  el %6.2f game %s look:0 %s look:1 %s' % (e, np.round(gg).astype(int), np.round(a0).astype(int), np.round(a1).astype(int)))
    print('sky worst channel error: look:0 %.1f, look:1 %.1f (mean %.1f / %.1f)' % (max(err[0]), max(err[1]), np.mean(err[0]), np.mean(err[1])))
    allm = ok
    for k in (0, 1):
        dif = np.abs(imgs[k] - game)[allm]
        print('all usable pixels: look:%d mean abs error %.1f/255' % (k, dif.mean()))
    sheet = np.concatenate([np.clip(d['g'], 0, 255), imgs[0], imgs[1]], 0).astype(np.uint8)
    png_io.write_png(os.path.join(g.dir, 'step62_look_sheet.png'), sheet[::2, ::2])
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
