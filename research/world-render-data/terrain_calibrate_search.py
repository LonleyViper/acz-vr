"""Brute-force pose/convention search against cockpit.png (offline; numpy+scipy).
Metric: normalised cross-correlation of high-passed luminance over the few screenshot regions not covered by canopy/HUD.
Usage: python -I terrain_calibrate_search.py <stage> <shot.png> x y z yaw_deg   (pose = logged E6 camera)
Poses searched: camera moved BACK along the (constant) heading by 0..BACK_MAX units, pitch sweep, 4 conventions.
Honest caveat: ground is hazed/low contrast and the screenshot pose is not logged, so scores are weak evidence.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from scipy.ndimage import gaussian_filter
import terrain_render as tr
import terrain_calibrate as tc
from terrain_lib import Stage

CW, CH = 320, 180
# regions in fractions of the game view (x0,x1,y0,y1) from the 2000x1150 display of the screenshot (title bar 26 px)
def frac(x0, x1, y0, y1): return (x0 / 1995, x1 / 1995, (y0 - 26) / 1120, (y1 - 26) / 1120)
REGIONS = [frac(1450, 1990, 640, 740), frac(1440, 1600, 740, 950), frac(10, 120, 700, 1000), frac(120, 420, 690, 745)]


def lum(a): return a[..., 0] * .3 + a[..., 1] * .59 + a[..., 2] * .11


def hp(a): return a - gaussian_filter(a, 4)


def mask():
    m = np.zeros((CH, CW), bool)
    for x0, x1, y0, y1 in REGIONS:
        m[int(y0 * CH):int(y1 * CH), int(x0 * CW):int(x1 * CW)] = True
    return m


def score(img, shot_hp, m):
    r = lum(tc.resize(img, CW, CH).astype(float)); r = hp(r)
    a, b = r[m], shot_hp[m]
    a = a - a.mean(); b = b - b.mean()
    return float((a * b).sum() / (np.sqrt((a * a).sum() * (b * b).sum()) + 1e-9))


if __name__ == '__main__':
    stage, shotp = sys.argv[1], sys.argv[2]
    x, y, z, yaw = map(float, sys.argv[3:7])
    st = Stage(stage)
    shot = tc.read_png(shotp)[30:]
    shot_hp = hp(lum(tc.resize(shot, CW, CH).astype(float)))
    m = mask()
    convs = {'RH fwd=(-sin,-cos) [x east,-z north]': (1, 180, -1), 'mirror of RH': (1, 180, 1),
             'RH-yaw-negated fwd=(sin,-cos)': (-1, 180, 1), 'LH-default fwd=(sin,cos)': (1, 0, 1)}
    f = np.array([-np.sin(np.radians(yaw)), -np.cos(np.radians(yaw))])
    res = []
    for name, (sg, off, hd) in convs.items():
        tc.patch_camera(sg, off, hd)
        for back in range(0, 40001, 2000):
            # screenshot is not at the logged pose: sweep along the *logged* travel direction (cam moved +x,-z)
            ex, ez = x - f[0] * back, z - f[1] * back
            for pit in (-6, -3, 0, 3, 6):
                img, _ = tr.render(st, (ex, y, ez), yaw, pit, 320, 63.2, 38.1, 250, False)
                res.append((score(img, shot_hp, m), name, back, pit))
    res.sort(reverse=True)
    for name in convs:
        best = [r for r in res if r[1] == name][:3]
        print(name, ['%.3f back=%d pit=%d' % (s, b, p) for s, _, b, p in best])
    print('median score all', np.median([r[0] for r in res]), 'max', res[0][0])
