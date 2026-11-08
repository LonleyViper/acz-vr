"""Small numpy z-buffer rasteriser for decoded ACZ models (offline previews only, numpy + stdlib).

Triangles are given as per-corner attribute arrays.  Pipeline: world -> camera (right-handed, Y up, camera looks
along its +forward row), near-plane clipping (Sutherland-Hodgman, all attributes interpolated), projection,
bucketed vectorised barycentric fill into an inverse-depth buffer with perspective-correct UVs, alpha test per
triangle, then deferred shading (texture * vertex colour * Lambert).  A translucent layer (nearest blended
fragment over the opaque result) is supported for glass.
"""
import numpy as np


def look_at(eye, target, up=(0, 1, 0)):
    eye = np.asarray(eye, float); f = np.asarray(target, float) - eye; f /= np.linalg.norm(f)
    r = np.cross(f, up); r /= np.linalg.norm(r); u = np.cross(r, f)
    return eye, np.stack([r, u, f])          # rows: right, up, forward


def ypr(yaw_deg, pitch_deg, roll_deg=0.0):
    """Camera basis in the game/model frame: yaw 0 looks along -Z (the nose), +yaw turns left (towards -X),
    +pitch looks up, +roll banks right.  Rows: right, up, forward."""
    y, p, r = np.radians([yaw_deg, pitch_deg, roll_deg])
    f = np.array([-np.sin(y) * np.cos(p), np.sin(p), -np.cos(y) * np.cos(p)])
    rt = np.array([np.cos(y), 0, -np.sin(y)])
    u = np.cross(rt, f)
    if r:
        c, s = np.cos(r), np.sin(r)
        rt, u = rt * c - u * s, u * c + rt * s
    return np.stack([rt, u, f])


def clip_near(cam, attrs, near):
    """cam (T,3,3) camera-space corners; attrs list of (T,3,k) arrays.  Returns clipped (cam, attrs, src_index)."""
    z = cam[..., 2]
    inside = z > near
    n_in = inside.sum(1)
    keep = n_in == 3
    out_c = [cam[keep]]; out_a = [[a[keep] for a in attrs]]; out_i = [np.nonzero(keep)[0]]
    part = np.nonzero((n_in > 0) & (n_in < 3))[0]
    nc, na, ni = [], [[] for _ in attrs], []
    for t in part:
        poly = [(cam[t, k], [a[t, k] for a in attrs]) for k in range(3)]
        res = []
        for k in range(3):
            (p0, a0), (p1, a1) = poly[k], poly[(k + 1) % 3]
            in0, in1 = p0[2] > near, p1[2] > near
            if in0:
                res.append((p0, a0))
            if in0 != in1:
                s = (near - p0[2]) / (p1[2] - p0[2])
                res.append((p0 + (p1 - p0) * s, [x0 + (x1 - x0) * s for x0, x1 in zip(a0, a1)]))
        for k in range(1, len(res) - 1):
            tri = [res[0], res[k], res[k + 1]]
            nc.append(np.stack([q[0] for q in tri]))
            for j in range(len(attrs)):
                na[j].append(np.stack([q[1][j] for q in tri]))
            ni.append(t)
    if nc:
        out_c.append(np.stack(nc)); out_i.append(np.array(ni))
        out_a.append([np.stack(x) for x in na])
    cam2 = np.concatenate(out_c)
    attrs2 = [np.concatenate([oa[j] for oa in out_a]) for j in range(len(attrs))]
    return cam2, attrs2, np.concatenate(out_i)


class Raster:
    def __init__(self, W, H, hfov_deg, near=0.05, vfov_deg=None):
        self.W, self.H = W, H
        self.fx = (W / 2) / np.tan(np.radians(hfov_deg) / 2)
        self.fy = self.fx if vfov_deg is None else (H / 2) / np.tan(np.radians(vfov_deg) / 2)
        self.near = near
        self.iz = np.zeros(W * H)
        self.tri = np.full(W * H, -1, np.int64)
        self.l1 = np.zeros(W * H, np.float32); self.l2 = np.zeros(W * H, np.float32)

    def draw(self, V, UV, COL, NRM, TEX, textures, eye, basis, cull=False, atest=None, cut=0.5):
        """Per-corner arrays: V (T,3,3) world, UV (T,3,2), COL (T,3,4) 0..255, NRM (T,3,3); TEX (T,) texture index
        or -1; atest (T,) bool alpha-test.  cull: drop triangles that are clockwise on screen (CCW-front data)."""
        T = len(V)
        if atest is None:
            atest = np.ones(T, bool)
        cam = (V - eye) @ basis.T
        cam, (UV, COL, NRM), src = clip_near(cam, [UV, COL.astype(np.float32), NRM], self.near)
        self.UV, self.COL, self.NRM = UV, COL, NRM
        self.TEX, self.ATEST, self.textures, self.SRC = TEX[src], atest[src], textures, src
        z = cam[..., 2]
        sx = self.W / 2 + self.fx * cam[..., 0] / z
        sy = self.H / 2 - self.fy * cam[..., 1] / z
        area = (sx[:, 1] - sx[:, 0]) * (sy[:, 2] - sy[:, 0]) - (sx[:, 2] - sx[:, 0]) * (sy[:, 1] - sy[:, 0])
        ok = np.abs(area) > 1e-9
        if cull:
            ok &= area < 0          # screen y points down: CCW in the world/camera frame == negative area here
        mnx = np.floor(sx.min(1)); mxx = np.ceil(sx.max(1)); mny = np.floor(sy.min(1)); mxy = np.ceil(sy.max(1))
        ok &= (mxx >= 0) & (mnx < self.W) & (mxy >= 0) & (mny < self.H)
        self.sx, self.sy, self.izv, self.area = sx, sy, 1.0 / z, area
        idx = np.nonzero(ok)[0]
        mnx = np.clip(mnx, 0, self.W - 1).astype(int); mny = np.clip(mny, 0, self.H - 1).astype(int)
        mxx = np.clip(mxx, 0, self.W - 1).astype(int); mxy = np.clip(mxy, 0, self.H - 1).astype(int)
        size = np.maximum(mxx - mnx + 1, mxy - mny + 1)[idx]
        self.cut255 = cut * 255
        S = 1
        while True:
            lo = S // 2 if S > 1 else 0
            sel = idx[(size <= S) & (size > lo)] if S < 4096 else idx[size > lo]
            if len(sel):
                self._fill(sel, min(S, self.W), min(S, self.H), mnx, mny)
            if S >= 4096:
                break
            S *= 2

    def _sample(self, tx, u, v, chan):
        out = np.zeros((len(tx), len(chan)), np.float32) + 255
        for k in np.unique(tx):
            if k < 0:
                continue
            img = self.textures[k]; m = tx == k
            h, w = img.shape[:2]
            xi = np.floor(u[m] * w).astype(np.int64) % w; yi = np.floor(v[m] * h).astype(np.int64) % h
            out[m] = img[yi, xi][:, chan]
        return out

    def _fill(self, sel, sw, sh, mnx, mny):
        W, H = self.W, self.H
        step = max(1, int(2_000_000 // (sw * sh)))
        ox = np.arange(sw)[None, None, :]; oy = np.arange(sh)[None, :, None]
        sx, sy, izv, area = self.sx, self.sy, self.izv, self.area
        for i in range(0, len(sel), step):
            s = sel[i:i + step]
            px = mnx[s][:, None, None] + ox + 0.5; py = mny[s][:, None, None] + oy + 0.5
            x0, y0 = sx[s, 0][:, None, None], sy[s, 0][:, None, None]
            x1, y1 = sx[s, 1][:, None, None], sy[s, 1][:, None, None]
            x2, y2 = sx[s, 2][:, None, None], sy[s, 2][:, None, None]
            ar = area[s][:, None, None]
            l1 = ((px - x0) * (y2 - y0) - (x2 - x0) * (py - y0)) / ar
            l2 = ((x1 - x0) * (py - y0) - (px - x0) * (y1 - y0)) / ar
            l0 = 1 - l1 - l2
            e = -1e-5
            m = (l0 >= e) & (l1 >= e) & (l2 >= e) & (px < W) & (py < H)
            tt = np.broadcast_to(s[:, None, None], m.shape)[m]
            L0, L1, L2 = l0[m], l1[m], l2[m]
            iz = L0 * izv[tt, 0] + L1 * izv[tt, 1] + L2 * izv[tt, 2]
            f = (np.floor(py).astype(np.int64) * W + np.floor(px).astype(np.int64))[m]
            at = self.ATEST[tt]
            if at.any():
                w0, w1, w2 = L0 * izv[tt, 0] / iz, L1 * izv[tt, 1] / iz, L2 * izv[tt, 2] / iz
                uv = self.UV[tt]
                u = w0 * uv[:, 0, 0] + w1 * uv[:, 1, 0] + w2 * uv[:, 2, 0]
                v = w0 * uv[:, 0, 1] + w1 * uv[:, 1, 1] + w2 * uv[:, 2, 1]
                a = self._sample(self.TEX[tt], u, v, [3])[:, 0]
                keep = ~at | (a >= self.cut255)
                f, iz, tt, L1, L2 = f[keep], iz[keep], tt[keep], L1[keep], L2[keep]
            np.maximum.at(self.iz, f, iz)
            win = iz >= self.iz[f]
            self.tri[f[win]] = tt[win]; self.l1[f[win]] = L1[win]; self.l2[f[win]] = L2[win]

    def shade(self, background, light=(0.35, 0.8, 0.45), ambient=0.6, gain=1.0):
        """-> (rgb float (H,W,3), alpha (H,W)) ; background (3,) or (H,W,3)."""
        W, H = self.W, self.H
        bg = np.asarray(background, np.float32)
        img = (bg.reshape(1, 3) * np.ones((W * H, 1), np.float32)) if bg.ndim == 1 else bg.reshape(-1, 3).copy()
        alpha = np.zeros(W * H, np.float32)
        hit = self.tri >= 0
        t = self.tri[hit]; L1 = self.l1[hit]; L2 = self.l2[hit]; L0 = 1 - L1 - L2
        izv = self.izv; iz = L0 * izv[t, 0] + L1 * izv[t, 1] + L2 * izv[t, 2]
        ww = np.stack([L0 * izv[t, 0] / iz, L1 * izv[t, 1] / iz, L2 * izv[t, 2] / iz], 1)[..., None]
        uv = (self.UV[t] * ww).sum(1)
        tex = self._sample(self.TEX[t], uv[:, 0], uv[:, 1], [0, 1, 2, 3])
        vc = (self.COL[t] * ww).sum(1) / 255.0
        col = tex[:, :3] * vc[:, :3]
        n = (self.NRM[t] * ww).sum(1)
        n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-6)
        Ld = np.asarray(light, float); Ld /= np.linalg.norm(Ld)
        lam = np.clip(n @ Ld, 0, 1)
        col *= (ambient + (1 - ambient) * lam)[:, None] * gain
        img[hit] = col
        alpha[hit] = tex[:, 3] / 255.0 * vc[:, 3]
        return img.reshape(H, W, 3), alpha.reshape(H, W)
