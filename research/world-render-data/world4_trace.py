"""WORLD-4 offline helper: numpy port of the runtime terrain ray march (terrain_stage.cpp trace(): same height_bilinear,
step law, bisection and tile/mip lookup) for a pinhole camera in the game convention, returning per pixel the hit
distance t, the hit point and the RAW texel colour (CLUT/255, display domain, no light/haze). Used by world4_compare.py
to fit the game's ground gain and haze law against a game-window screenshot of the same frame.

  python -I world4_trace.py <out.npz> --pose x,y,z,heading_rad,pitch_rad [--w 1203 --tanx 0.6152 --tany 0.3460]

Camera (D-297/D-300/WORLD-1): right-handed, forward = (-sin h cos p, sin p, -cos h cos p), right = (cos h, 0, -sin h).
Nothing here is game-derived output that gets committed; outputs go to D:\\acz-extract\\out\\world4.
"""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from terrain_lib import Stage, CELL, HSTEP, HALF

# terrain_stage.h
K_MARCH_STEPS, K_BISECT = 320, 8
K_STEP_MIN, K_STEP_GROW, K_STEP_MAX, K_STEP_FRAC = 128.0, 0.008, 8192.0, 0.4
K_LOD_SCALE = 1.0 / 16000.0


def basis(h, p):
    fwd = np.array([-np.sin(h) * np.cos(p), np.sin(p), -np.cos(h) * np.cos(p)])
    right = np.array([np.cos(h), 0.0, -np.sin(h)])
    up = np.cross(right, fwd)
    return right, up, fwd


def rays(W, H, tanx, tany, h, p):
    r, u, f = basis(h, p)
    xs = ((np.arange(W) + 0.5) / W * 2 - 1) * tanx
    ys = (1 - (np.arange(H) + 0.5) / H * 2) * tany
    X, Y = np.meshgrid(xs, ys)
    d = f[None, None] + X[..., None] * r + Y[..., None] * u
    return d / np.linalg.norm(d, axis=-1, keepdims=True)


class Height:
    def __init__(self, st):
        self.hf = st.heightfield.astype(np.float64)

    def __call__(self, x, z):
        u = np.clip((x + HALF) / CELL, 0, 512.0); v = np.clip((z + HALF) / CELL, 0, 512.0)
        i = np.minimum(np.floor(u).astype(int), 511); j = np.minimum(np.floor(v).astype(int), 511)
        fu, fv = u - i, v - j
        hf = self.hf
        return ((hf[j, i] * (1 - fu) + hf[j, i + 1] * fu) * (1 - fv) + (hf[j + 1, i] * (1 - fu) + hf[j + 1, i + 1] * fu) * fv) * HSTEP


def march(st, o, d, far=160000.0):
    """-> t (inf on a miss), hit mask. Vectorised copy of trace()'s loop + bisection."""
    hgt = Height(st)
    n = d.shape[0]
    hmax = float(st.heightfield.max()) * HSTEP
    t = np.zeros(n); prev = np.zeros(n)
    active = np.ones(n, bool); hit = np.zeros(n, bool)
    for _ in range(K_MARCH_STEPS):
        idx = np.nonzero(active)[0]
        if not len(idx):
            break
        ti = t[idx]
        px = o[0] + d[idx, 0] * ti; py = o[1] + d[idx, 1] * ti; pz = o[2] + d[idx, 2] * ti
        dy = py - hgt(px, pz)
        h_ = dy < 0
        hit[idx[h_]] = True
        stop = h_ | ((py > hmax) & (d[idx, 1] >= 0)) | (ti >= far)
        active[idx[stop]] = False
        go = idx[~stop]
        prev[go] = t[go]
        tg = t[go]
        t[go] = tg + np.minimum(np.maximum(K_STEP_FRAC * dy[~stop], K_STEP_MIN + K_STEP_GROW * tg), K_STEP_MAX)
    lo, hi = prev.copy(), t.copy()
    hidx = np.nonzero(hit)[0]
    for _ in range(K_BISECT):
        m = 0.5 * (lo[hidx] + hi[hidx])
        below = o[1] + d[hidx, 1] * m < hgt(o[0] + d[hidx, 0] * m, o[2] + d[hidx, 2] * m)
        hi[hidx[below]] = m[below]; lo[hidx[~below]] = m[~below]
    tout = np.where(hit, hi, np.inf)
    return tout, hit


def texels(st, x, z, t):
    """raw texel colour (0..1) with the runtime's lod law; nearest texel of the nearest available mip (mips 0..3 in the file)."""
    words = st.tile_words()
    u = np.clip((x + HALF) / CELL * 0.5, 0, 255.999); v = np.clip((z + HALF) / CELL * 0.5, 0, 255.999)
    ti, tj = u.astype(int), v.astype(int)
    tid = (words[tj, ti] & 0x3ff).astype(int)
    lod = np.where(t * K_LOD_SCALE > 1, np.log2(np.maximum(t * K_LOD_SCALE, 1e-9)), 0.0)
    mip = np.clip(np.round(lod).astype(int), 0, 3)
    out = np.zeros(x.shape + (3,)); idx = np.zeros(x.shape, int)
    for m in range(4):
        s = (64, 32, 16, 8)[m]
        sel = mip == m
        if not sel.any():
            continue
        px = np.minimum(((u[sel] - ti[sel]) * s).astype(int), s - 1); py = np.minimum(((v[sel] - tj[sel]) * s).astype(int), s - 1)
        k = st.tiles[m][tid[sel], py, px]
        idx[sel] = k; out[sel] = st.clut[k] / 255.0
    return out, idx


def trace_image(stage, pose, W, H, tanx, tany, far=160000.0):
    st = Stage(stage)
    x, y, z, h, p = pose
    d = rays(W, H, tanx, tany, h, p).reshape(-1, 3)
    o = np.array([x, y, z])
    t, hit = march(st, o, d, far)
    P = o + d * np.where(hit, t, 0)[:, None]
    col, idx = texels(st, P[:, 0], P[:, 2], np.where(hit, t, 0))
    col[~hit] = 0
    return dict(t=t.reshape(H, W), hit=hit.reshape(H, W), p=P.reshape(H, W, 3), tex=col.reshape(H, W, 3),
                idx=idx.reshape(H, W), d=d.reshape(H, W, 3))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('out'); ap.add_argument('--stage', default='m02a'); ap.add_argument('--pose', required=True)
    ap.add_argument('--w', type=int, default=1203); ap.add_argument('--h', type=int, default=0)
    ap.add_argument('--tanx', type=float, default=0.6152); ap.add_argument('--tany', type=float, default=0.3460)
    g = ap.parse_args(argv)
    pose = tuple(float(v) for v in g.pose.split(','))
    H = g.h or int(round(g.w * g.tany / g.tanx))
    r = trace_image(g.stage, pose, g.w, H, g.tanx, g.tany)
    np.savez_compressed(g.out, **r)
    print(g.out, 'hit fraction %.3f' % r['hit'].mean())
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
