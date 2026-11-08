"""Throwaway numpy z-buffer rasteriser for ACZ stage terrain (offline research, no game needed).

  python -I terrain_render.py <stage> <out.png> x y z yaw_deg pitch_deg [--w 960] [--hfov 63.2] [--vfov 38.1]
                              [--range 160] [--objects] [--flat]

World frame (D-292): X right(+col), Y up (height byte*80), Z (+row). Camera: forward=(sin yaw,0,cos yaw)
(rotated up by pitch), right=(cos yaw,0,-sin yaw), i.e. a left-handed Y-up frame. The game's real Euler convention
is NOT verified (see the design doc) -- yaw/pitch here are just this tool's inputs.
Pipeline: triangulate the 513x513 heightfield (2 tris/cell) in a window around the eye, project, rasterise
triangles into an inverse-depth z-buffer (vectorised barycentric fill, bucketed by screen bbox), then shade each pixel by
reconstructing its world xz from depth: texel = mti tile word -> mtd tile (mip by distance) -> CLUT, x hillshade x fog.
Objects (.pdl) are drawn as fixed-size boxes (models are not decoded).
"""
import sys, os, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from terrain_lib import Stage, write_png, CELL, HSTEP, HALF


def camera(eye, yaw, pitch):
    cy, sy, cp, sp = np.cos(yaw), np.sin(yaw), np.cos(pitch), np.sin(pitch)
    fwd = np.array([sy * cp, sp, cy * cp])
    right = np.array([cy, 0, -sy])
    up = np.cross(fwd, right) * -1  # = right x fwd in a left-handed frame -> up
    up = np.array([-sy * sp, cp, -cy * sp])
    return fwd, right, up


class Rast:
    def __init__(self, W, H, hfov, vfov):
        self.W, self.H = W, H
        self.fx = (W / 2) / np.tan(hfov / 2)
        self.fy = (H / 2) / np.tan(vfov / 2)
        self.iz = np.zeros(W * H, np.float64)         # inverse depth (0 = empty)
        self.tag = np.zeros(W * H, np.int32)          # 0 terrain/none, k>0 object k

    def project(self, P, eye, fwd, right, up):
        d = P - eye
        z = d @ fwd
        x = d @ right
        y = d @ up
        zz = np.maximum(z, 1e-6)
        return self.W / 2 + self.fx * x / zz, self.H / 2 - self.fy * y / zz, z

    def draw(self, sx, sy, z, tri, tag=0, near=40.0):
        """tri: (T,3) vertex indices."""
        W, H = self.W, self.H
        a, b, c = tri[:, 0], tri[:, 1], tri[:, 2]
        ok = (z[a] > near) & (z[b] > near) & (z[c] > near)
        tri = tri[ok]
        if not len(tri):
            return
        a, b, c = tri[:, 0], tri[:, 1], tri[:, 2]
        x0, y0, x1, y1, x2, y2 = sx[a], sy[a], sx[b], sy[b], sx[c], sy[c]
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        mnx = np.floor(np.minimum(np.minimum(x0, x1), x2)); mxx = np.ceil(np.maximum(np.maximum(x0, x1), x2))
        mny = np.floor(np.minimum(np.minimum(y0, y1), y2)); mxy = np.ceil(np.maximum(np.maximum(y0, y1), y2))
        vis = (np.abs(area) > 1e-9) & (mxx >= 0) & (mnx < W) & (mxy >= 0) & (mny < H)
        idx = np.nonzero(vis)[0]
        mnx = np.maximum(mnx, 0); mny = np.maximum(mny, 0)
        bw = (np.minimum(mxx, W - 1) - mnx + 1).astype(int); bh = (np.minimum(mxy, H - 1) - mny + 1).astype(int)
        size = np.maximum(bw, bh)[idx]
        iz = 1.0 / z
        izs = np.stack([iz[a], iz[b], iz[c]], 1)
        S = 1
        while True:
            sel = idx[(size <= S) & (size > S // 2 if S > 1 else size >= 1)] if S <= 128 else idx[size > 128]
            if len(sel):
                if S <= 128:
                    self._fill(sel, S, S, mnx, mny, x0, y0, x1, y1, x2, y2, area, izs, tag)
                else:
                    for t in sel:   # big triangles one by one
                        self._fill(np.array([t]), int(bw[t]), int(bh[t]), mnx, mny, x0, y0, x1, y1, x2, y2, area, izs, tag)
            if S > 128:
                break
            S = S * 2 if S < 128 else 256

    def _fill(self, sel, sw, sh, mnx, mny, x0, y0, x1, y1, x2, y2, area, izs, tag):
        W, H = self.W, self.H
        step = max(1, int(3_000_000 // (sw * sh)))
        ox = np.arange(sw)[None, None, :]; oy = np.arange(sh)[None, :, None]
        for i in range(0, len(sel), step):
            s = sel[i:i + step]
            px = (mnx[s][:, None, None] + ox + 0.5); py = (mny[s][:, None, None] + oy + 0.5)
            ar = area[s][:, None, None]
            l1 = ((px - x0[s][:, None, None]) * (y2[s][:, None, None] - y0[s][:, None, None])
                  - (x2[s][:, None, None] - x0[s][:, None, None]) * (py - y0[s][:, None, None])) / ar
            l2 = ((x1[s][:, None, None] - x0[s][:, None, None]) * (py - y0[s][:, None, None])
                  - (px - x0[s][:, None, None]) * (y1[s][:, None, None] - y0[s][:, None, None])) / ar
            l0 = 1 - l1 - l2
            e = -1e-6
            m = (l0 >= e) & (l1 >= e) & (l2 >= e) & (px < W) & (py < H)
            z = l0 * izs[s, 0][:, None, None] + l1 * izs[s, 1][:, None, None] + l2 * izs[s, 2][:, None, None]
            flat = (np.floor(py).astype(np.int64) * W + np.floor(px).astype(np.int64))
            f = flat[m]; zv = z[m]
            np.maximum.at(self.iz, f, zv)
            win = zv >= self.iz[f] - 1e-15
            if tag:
                self.tag[f[win]] = tag


def render(st, eye, yaw, pitch, W=960, hfov=63.2, vfov=38.1, rng_cells=160, objects=True, texture=True, box=(1800., 3600., 1800.)):
    H = int(round(W * np.tan(np.radians(vfov) / 2) / np.tan(np.radians(hfov) / 2)))
    R = Rast(W, H, np.radians(hfov), np.radians(vfov))
    fwd, right, up = camera(np.asarray(eye, float), np.radians(yaw), np.radians(pitch))
    eye = np.asarray(eye, float)
    hf = st.heightfield
    # window of cells around the eye
    ci, cj = (eye[0] + HALF) / CELL, (eye[2] + HALF) / CELL
    i0, i1 = int(max(0, ci - rng_cells)), int(min(512, ci + rng_cells))
    j0, j1 = int(max(0, cj - rng_cells)), int(min(512, cj + rng_cells))
    # keep only the cells roughly in front (cheap cull: view direction cone widened)
    I, J = np.meshgrid(np.arange(i0, i1 + 1), np.arange(j0, j1 + 1))
    P = np.stack([-HALF + I * CELL, hf[J, I] * HSTEP, -HALF + J * CELL], -1).reshape(-1, 3)
    sx, sy, z = R.project(P, eye, fwd, right, up)
    nx = i1 - i0 + 1; ny = j1 - j0 + 1
    vid = np.arange(nx * ny).reshape(ny, nx)
    q00, q10, q01, q11 = vid[:-1, :-1].ravel(), vid[:-1, 1:].ravel(), vid[1:, :-1].ravel(), vid[1:, 1:].ravel()
    tri = np.concatenate([np.stack([q00, q10, q01], 1), np.stack([q10, q11, q01], 1)])
    R.draw(sx, sy, z, tri)
    ntri = len(tri)
    nobj = 0
    if objects and len(st.pdl):
        o = st.pdl
        keep = (np.abs(o[:, 0] - eye[0]) < rng_cells * CELL) & (np.abs(o[:, 2] - eye[2]) < rng_cells * CELL)
        o = o[keep]; nobj = len(o)
        bx, by, bz = box
        cube = np.array([[x, y, z_] for x in (-.5, .5) for y in (0, 1) for z_ in (-.5, .5)])
        faces = np.array([[0, 1, 3], [0, 3, 2], [4, 6, 7], [4, 7, 5], [0, 4, 5], [0, 5, 1], [2, 3, 7], [2, 7, 6], [0, 2, 6], [0, 6, 4], [1, 5, 7], [1, 7, 3]])
        for k, (x, y, z_, m) in enumerate(o, 1):
            Pk = np.array([x, y, z_]) + cube * np.array([bx, by, bz])
            ax, ay, az = R.project(Pk, eye, fwd, right, up)
            R.draw(ax, ay, az, faces, tag=k)
    # ---------- shading ----------
    zbuf = np.where(R.iz > 0, 1.0 / np.maximum(R.iz, 1e-30), np.inf).reshape(H, W)
    yy, xx = np.mgrid[0:H, 0:W]
    rx = (xx + .5 - W / 2) / R.fx; ry = -(yy + .5 - H / 2) / R.fy
    ray = fwd[None, None] + rx[..., None] * right + ry[..., None] * up       # z-component along fwd == 1
    Wp = eye + ray * zbuf[..., None]
    hit = np.isfinite(zbuf)
    # sky
    t = np.clip(0.5 - ry * 1.5, 0, 1)[..., None]
    img = (np.array([150, 185, 215]) * (1 - t) + np.array([215, 225, 232]) * t)
    xw = np.where(hit, Wp[..., 0], 0); zw = np.where(hit, Wp[..., 2], 0)
    dist = np.where(hit, zbuf, 1)
    col = np.zeros((H, W, 3))
    if texture:
        u = np.clip((xw + HALF) / CELL / 2, 0, 255.999); v = np.clip((zw + HALF) / CELL / 2, 0, 255.999)
        ti, tj = u.astype(int), v.astype(int); fu, fv = u - ti, v - tj
        words = st.tile_words()[tj, ti]
        tid = (words & 0x3ff).astype(int)
        mip = np.clip((np.log2(np.maximum(dist, 1) / 9000.0)).astype(int) + 0, 0, 3)   # ~1 texel/pixel guess
        col = np.zeros((H, W, 3))
        for m in range(4):
            s = (64, 32, 16, 8)[m]
            sel = hit & (mip == m)
            if not sel.any():
                continue
            tx = np.minimum((fu[sel] * s).astype(int), s - 1); ty = np.minimum((fv[sel] * s).astype(int), s - 1)
            col[sel] = st.clut[st.tiles[m][tid[sel], ty, tx]]
    else:
        col[:] = 120
    # hillshade from the heightfield gradient (light from sun azimuth 135 deg, elevation 45 deg)
    gy, gx = np.gradient(hf * HSTEP, CELL)
    ii = np.clip(((xw + HALF) / CELL).astype(int), 0, 511); jj = np.clip(((zw + HALF) / CELL).astype(int), 0, 511)
    nxv, nzv = -gx[jj, ii], -gy[jj, ii]
    nrm = np.sqrt(nxv ** 2 + 1 + nzv ** 2)
    L = np.array([0.5, 0.7, 0.5]); L /= np.linalg.norm(L)
    sh = np.clip((nxv * L[0] + L[1] + nzv * L[2]) / nrm, 0.15, 1)
    col = col * (0.55 + 0.9 * sh[..., None])
    fog = np.clip(1 - np.exp(-dist / 90000.0), 0, 0.85)[..., None]
    col = col * (1 - fog) + np.array([200, 212, 222]) * fog
    img = np.where(hit[..., None], col, img)
    # objects: flat marker colours
    tg = R.tag.reshape(H, W)
    if nobj:
        mid = np.concatenate([[0.0], o[:, 3]])[tg]
        pal = np.stack([(mid * 53) % 200 + 55, (mid * 97) % 200 + 55, (mid * 29) % 120 + 80], -1)
        img = np.where((tg > 0)[..., None], pal, img)
    return np.clip(img, 0, 255).astype(np.uint8), dict(tris=ntri, objects=nobj, size=(W, H), iz=R.iz, fx=R.fx, fy=R.fy)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('stage'); ap.add_argument('out')
    ap.add_argument('x', type=float); ap.add_argument('y', type=float); ap.add_argument('z', type=float)
    ap.add_argument('yaw', type=float); ap.add_argument('pitch', type=float, nargs='?', default=0.0)
    ap.add_argument('--w', type=int, default=960); ap.add_argument('--hfov', type=float, default=63.2)
    ap.add_argument('--vfov', type=float, default=38.1); ap.add_argument('--range', type=int, default=160)
    ap.add_argument('--no-objects', action='store_true'); ap.add_argument('--flat', action='store_true')
    a = ap.parse_args()
    st = Stage(a.stage)
    img, info = render(st, (a.x, a.y, a.z), a.yaw, a.pitch, a.w, a.hfov, a.vfov, a.range, not a.no_objects, not a.flat)
    write_png(a.out, img)
    print(a.stage, info, 'eye ground h=%.0f' % st.height_at(a.x, a.z))
