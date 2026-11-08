"""Calibration helper: render a stage from a logged E6 camera pose under a chosen yaw/handedness convention
and write a side-by-side with the in-game screenshot (offline; numpy only).

  python -I terrain_calibrate.py <stage> <out.png> x y z yaw_deg [--a-off-deg 180] [--sgn 1] [--hand -1] [--pitch-deg 0] [--shot cockpit.png]

Convention: forward azimuth a = sgn*yaw + a_off; forward = (sin a, 0, cos a) in (x,z); screen-right = hand*(cos a, 0, -sin a).
Calibrated result (see docs/world-render/terrain-calibration.md): sgn=+1, a_off=180 deg, hand=-1
  => forward = (-sin yaw, 0, -cos yaw), right = forward x up (right-handed, x east, -z north, y up).
"""
import sys, os, argparse, struct, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import terrain_render as tr
from terrain_lib import Stage, write_png


def read_png(path):
    d = open(path, 'rb').read(); p = 8; idat = b''
    while p < len(d):
        n, t = struct.unpack('>I4s', d[p:p + 8]); b = d[p + 8:p + 8 + n]; p += 12 + n
        if t == b'IHDR': w, h, bd, ct = struct.unpack('>IIBB', b[:10])
        elif t == b'IDAT': idat += b
    ch = {2: 3, 6: 4}[ct]; raw = np.frombuffer(zlib.decompress(idat), np.uint8); st = w * ch
    out = np.zeros((h, st), np.uint8); prev = np.zeros(st, np.int32); q = 0
    for y in range(h):
        f = raw[q]; line = raw[q + 1:q + 1 + st].astype(np.int32); q += 1 + st
        if f == 0: cur = line
        elif f == 2: cur = (line + prev) & 255
        else:
            cur = line.copy()
            for i in range(st):
                a = int(cur[i - ch]) if i >= ch else 0; b_ = int(prev[i]); c = int(prev[i - ch]) if i >= ch else 0
                if f == 1: pr = a
                elif f == 3: pr = (a + b_) >> 1
                else:
                    pa, pb, pc = abs(b_ - c), abs(a - c), abs(a + b_ - 2 * c)
                    pr = a if (pa <= pb and pa <= pc) else (b_ if pb <= pc else c)
                cur[i] = (cur[i] + pr) & 255
        out[y] = cur; prev = cur
    return out.reshape(h, w, ch)[..., :3]


def patch_camera(sgn, a_off, hand):
    def cam(eye, yaw, pitch):
        a = sgn * yaw + np.radians(a_off)
        cp, sp = np.cos(pitch), np.sin(pitch)
        fwd = np.array([np.sin(a) * cp, sp, np.cos(a) * cp])
        right = hand * np.array([np.cos(a), 0, -np.sin(a)])
        up = np.array([-np.sin(a) * sp, cp, -np.cos(a) * sp])
        return fwd, right, up
    tr.camera = cam


def resize(img, w, h):
    ys = (np.arange(h) * img.shape[0] / h).astype(int); xs = (np.arange(w) * img.shape[1] / w).astype(int)
    return img[ys][:, xs]


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('stage'); ap.add_argument('out')
    ap.add_argument('x', type=float); ap.add_argument('y', type=float); ap.add_argument('z', type=float)
    ap.add_argument('yaw', type=float, help='degrees (log value in radians * 57.29578)')
    ap.add_argument('--a-off-deg', type=float, default=180.0); ap.add_argument('--sgn', type=float, default=1.0)
    ap.add_argument('--hand', type=float, default=-1.0); ap.add_argument('--pitch-deg', type=float, default=0.0)
    ap.add_argument('--w', type=int, default=800); ap.add_argument('--range', type=int, default=250)
    ap.add_argument('--shot'); ap.add_argument('--no-objects', action='store_true')
    a = ap.parse_args()
    patch_camera(a.sgn, a.a_off_deg, a.hand)
    st = Stage(a.stage)
    img, info = tr.render(st, (a.x, a.y, a.z), a.yaw, a.pitch_deg, a.w, 63.2, 38.1, a.range, not a.no_objects)
    if a.shot:
        s = read_png(a.shot)[30:1392]          # drop the window title bar
        s = resize(s, img.shape[1], img.shape[0])
        img = np.concatenate([img, s], 0)
    write_png(a.out, img); print(info)
