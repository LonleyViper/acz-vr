"""Orientation-convention scan for the m02a terrain renderer against the game-window screenshot
(step54, acz-20261026-222415.png).  Offline, numpy only.

Method: river tiles = tile words whose 8x8-mip mean CLUT colour has (r-g) > 12 (the red-brown 'river' tiles, ~370 tiles).
For every convention (forward azimuth a = 135 deg + yaw offset, handedness RH or mirrored) and every along-track shift of the
camera, the river tile centres are projected with the E6 camera (level, FOV 63.2 x 38.1, game view 2421x1362 at y>=31) and
compared with what the screenshot shows in its unobstructed ground strips: a river band at the LEFT edge, x 0..150, y ~972,
and none in the right strip. Score = pixel distance from the projected river to the observed band minus a penalty for river
predicted in the right strip. Usage: python -I orient_scan.py [shot_y_px=972]
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from terrain_lib import Stage, HALF

OBS = (80.0, 972.0)                    # observed river band centre in the 2421x1392 screenshot (left edge)
CX, CY = 1210.0, 31 + 681.0
F = 1210.0 / np.tan(np.radians(31.6))
POSE = np.array([-6987.0, 114648.0])   # E6 air pos interpolated at png-once (22:24:15.19), see the doc
CAMY = 3876.0 + 3.5


def river_points(st):
    w = st.tile_words(); tid = (w & 0x3ff).astype(int)
    cols = np.stack([st.clut[st.tiles[3][i]].reshape(-1, 3).mean(0) for i in range(st.n)])
    r, c = np.nonzero((cols[:, 0] - cols[:, 1])[tid] > 12)
    return np.stack([-HALF + (c + .5) * 2048, -HALF + (r + .5) * 2048], 1)


def project(P, hts, eye, a_deg, mirror):
    a = np.radians(a_deg); f = np.array([np.sin(a), np.cos(a)]); rt = np.array([-np.cos(a), np.sin(a)])  # RH right
    if mirror: rt = -rt
    d = P - eye; fw = d @ f; lt = d @ rt
    ok = fw > 3000
    sx = CX + F * lt / np.where(ok, fw, 1); sy = CY - F * (hts - CAMY) / np.where(ok, fw, 1)
    return sx[ok], sy[ok], fw[ok]


def score(sx, sy, fw):
    inL = (sx >= -30) & (sx <= 170) & (sy > 860) & (sy < 1250)
    inR = (sx >= 2030) & (sx <= 2421) & (sy > 860) & (sy < 1050)
    dist = np.hypot(sx - OBS[0], sy - OBS[1]) if len(sx) else np.array([1e9])
    hit = float(dist.min()) if len(dist) else 1e9
    return hit, int(inL.sum()), int(inR.sum())


def local_band(sx, sy, x0=80.0, span=450.0):
    """(y at x0, slope) of the projected river polyline near x0 in the left strip, or None."""
    k = (np.abs(sx - x0) < span) & (sy > 860) & (sy < 1250)
    if k.sum() < 2: return None
    p = np.polyfit(sx[k], sy[k], 1); return float(np.polyval(p, x0)), float(p[0])


OBS_SLOPE = -0.06   # observed band: y 975 at x=10 -> 967 at x=145 (screenshot, original px)


def full_table(window=1500, step=500):
    st = Stage('m02a'); P = river_points(st); H = st.height_at(P[:, 0], P[:, 1]); rows = []
    for mirror in (0, 1):
        for off in (0, 90, 180, 270):
            best = None
            for fine in (-10, 0, 10):
                for sh in range(-window, window + 1, step):
                    eye = POSE + sh * np.array([0.7071, -0.7071])
                    sx, sy, fw = project(P, H, eye, 135.0 + off + fine, mirror)
                    d, nl, nr = score(sx, sy, fw); lb = local_band(sx, sy)
                    if lb is None: s = d + 400 * nr + 500
                    else: s = abs(lb[0] - OBS[1]) + 1000 * abs(lb[1] - OBS_SLOPE) + 400 * nr
                    if best is None or s < best[0]: best = (s, d, nr, sh, fine, lb)
            rows.append((best[0], mirror, off, best))
    rows.sort(); return rows


if __name__ == '__main__':
    for s_, m_, off_, b_ in full_table():
        print('mirror=%d yaw-off=%3d score %6.0f  nearest %5.0f px  R-hits %d  shift %+5d fine %+d  band(y@x80,slope)=%s' % (m_, off_, s_, b_[1], b_[2], b_[3], b_[4], None if b_[5] is None else '(%.0f, %+.3f)' % b_[5]))
    sys.exit(0)
if False:
    st = Stage('m02a'); P = river_points(st); H = st.height_at(P[:, 0], P[:, 1])
    ang = lambda off: 135.0 + off
    print('river tiles', len(P))
    res = []
    for mirror in (0, 1):
        for off in list(range(0, 360, 90)):
            best = None
            for fine in (-10, 0, 10):
                for shift in range(-30000, 30001, 1500):        # along the NE track relative to the logged pose
                    eye = POSE + shift * np.array([0.7071, -0.7071])
                    sx, sy, fw = project(P, H, eye, ang(off + fine), mirror)
                    d, nl, nr = score(sx, sy, fw)
                    s = d + 400 * nr          # px; river in the right strip is penalised
                    if best is None or s < best[0]: best = (s, d, nl, nr, shift, fine)
            res.append((best[0], mirror, off, best))
    res.sort()
    for s, m, off, b in res:
        print('mirror=%d yaw-off=%3d  score %7.0f px  (nearest-to-observed %6.0f px, L-strip hits %d, R-strip hits %d)  best shift %+6d u fine %+d' % (m, off, s, b[1], b[2], b[3], b[4], b[5]))
