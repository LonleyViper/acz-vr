"""CLIP survey: which triangles of the model cockpit (game cockpit part p2, exterior airframe p0, our synthetic COCKPIT-4 interior) lie
OUTSIDE the airframe, i.e. poke through the skin around the pilot (offline, numpy + stdlib; reads ONLY our baked .zvrm files).

  python -I cockpit_clip_survey.py survey <models_dir> [--types j35j,m21b,...] [--json out.json] [--mesh-dir <dir of <stem>.tri dumps>]
  python -I cockpit_clip_survey.py render <a.zvrm> <out.png> [--ypr y,p,r | --from=x,y,z --look=x,y,z] [--hfov 100] [--mesh <stem>.tri]
  python -I cockpit_clip_survey.py table <models_dir> [--json cockpit_clip_volumes.json] [--cpp rows.txt]

survey/render without --mesh use the BEFORE geometry: the bake's parts + a PORT of the COCKPIT-4 interior (cockpit_model.cpp build_mesh at rest, clip:0).
With --mesh / --mesh-dir they read the mesh build_mesh actually produced (cockpit_model_offline_tests --load <zvrm> --dump <stem>.tri [--opts ...]:
u32 count, then per triangle 9 f32 + u8 source 0 exterior / 1 game cockpit / 2 synthetic), so before (clip:0) and after (default) are compared on
the real C++ output. "Outside" = outside the airframe envelope (Envelope: the drawn exterior skin incl. canopy glass, sampled on 0.1 u grids).
table: the per-type clip volume (metres from the pvb eye) = the extent of the opaque game cockpit triangles (ahead/below/above/half-width: all of them,
nothing of the game's own cockpit is cut there), behind = the farthest of those whose centroid lies INSIDE the airframe (parts that stick out of the
spine behind the cockpit are trimmed), each + 0.02 m; below >= 1.12 m (the procedural tub floor 1.05 m under the eye).
Docs: docs/world-render/cockpit-render-impl.md section 16.
"""
import argparse, json, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from model_baker import read_zvrm, MF_BLEND, MF_GLASS, MF_HIDDEN, MF_EXTERIOR, MF_COCKPIT_DUP

U = 5.0   # kModelUnitsPerMetre


def tris_of(z, k):
    p = z['parts'][k]
    return z['V']['pos'][z['I'][p['fi']:p['fi'] + p['ic']]].reshape(-1, 3, 3).astype(np.float64)


def box_tris(lo, hi):
    x0, y0, z0 = lo; x1, y1, z1 = hi
    f = [[(x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)], [(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)],
         [(x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)], [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)],
         [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], [(x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)]]
    out = []
    for q in f:
        out += [(q[0], q[1], q[2]), (q[0], q[2], q[3])]
    return out


def quad_tris(c):
    return [(c[0], c[1], c[2]), (c[0], c[2], c[3])]


def seat_back_max(back_max, seatback):
    """SEAT-CLEARANCE: mirror of cockpit_model.h seat_back_max (model metres at 5 u/m). The headrest front is back_max - 0.15; 0 / negative = off."""
    if not seatback > 0:
        return back_max
    return max(back_max, seatback + 0.15)


def interior(z, eye, stick=0, seatback=0.0, clipbox=None):
    """Port of the COCKPIT-4 procedural interior (cockpit_model.cpp build_mesh, o.interior): list of (name, tris (T,3,3)).
    seatback (model metres, 0 = the pre-SEAT placement): the SEAT-CLEARANCE rule (seat_back_max, see cockpit_seat_clearance.py);
    clipbox (ahead, behind, below, above, half; metres) limits the footprint like the C++ clip:1 path (the fit into the skin is NOT ported)."""
    E = eye; P = z['parts']
    vis = [k for k, p in enumerate(P) if p['ic'] and not (p['mflags'] & (MF_HIDDEN | MF_COCKPIT_DUP))]
    cock = [k for k in vis if not (P[k]['mflags'] & (MF_EXTERIOR | MF_BLEND))]
    if not cock:
        return [], None
    pts = np.concatenate([tris_of(z, k).reshape(-1, 3) for k in cock])
    lo, hi = pts.min(0), pts.max(0)
    if clipbox is not None:
        a_, b_, c_, d_, h_ = clipbox
        lo = np.maximum(lo, [E[0] - h_ * U, E[1] - c_ * U, E[2] - a_ * U]); hi = np.minimum(hi, [E[0] + h_ * U, E[1] + d_ * U, E[2] + b_ * U])
    zf, xl, xr = lo[2], lo[0], hi[0]
    back_old = max(0.10, min(0.36, (hi[2] - E[2]) / U - 0.02))
    back_max = seat_back_max(back_old, seatback)
    # SEAT-CLEARANCE: the tub (floor, walls, bulkhead) extends to enclose the moved seat (mirror of build_mesh; never on a real model, never with seatback 0)
    zr = max(hi[2], E[2] + (back_max + 0.02) * U) if back_max > back_old else hi[2]
    fy = min(E[1] - 1.05 * U, lo[1] - 0.05 * U)
    wall_top = max(lo[1], E[1] - 0.30 * U)
    out = []
    out.append(('tub-floor', quad_tris([(xl, fy, zr), (xr, fy, zr), (xr, fy, zf), (xl, fy, zf)])))
    out.append(('tub-wall-left', quad_tris([(xl, fy, zr), (xl, fy, zf), (xl, wall_top, zf), (xl, wall_top, zr)])))
    out.append(('tub-wall-right', quad_tris([(xr, fy, zf), (xr, fy, zr), (xr, wall_top, zr), (xr, wall_top, zf)])))
    m = (np.abs(pts[:, 0] - E[0]) < 0.5) & (E[2] - pts[:, 2] > 1.0)
    tan_gs = ((pts[m, 1] - E[1]) / (E[2] - pts[m, 2])).max() if m.any() else -1e9
    yt = E[1] - 0.25 * U
    if tan_gs > -1e8:
        yt = min(yt, E[1] + (tan_gs - 0.02) * (E[2] - zf))
    yt = max(yt, lo[1] + 0.02 * U)
    out.append(('tub-front-wall', quad_tris([(xl, fy, zf), (xr, fy, zf), (xr, yt, zf), (xl, yt, zf)])))
    spine = E[1] - 0.20 * U
    for k in vis:
        if not (P[k]['mflags'] & MF_EXTERIOR) or (P[k]['mflags'] & MF_BLEND):
            continue
        q = tris_of(z, k).reshape(-1, 3)
        s = (q[:, 0] > xl) & (q[:, 0] < xr) & (q[:, 2] > zr - 0.2 * U) & (q[:, 2] < zr + 0.4 * U)
        if s.any():
            spine = max(spine, q[s, 1].max())
    out.append(('rear-bulkhead', quad_tris([(xr, fy, zr), (xl, fy, zr), (xl, min(spine, E[1] + 0.5 * U), zr), (xr, min(spine, E[1] + 0.5 * U), zr)])))

    def bx(name, x0, x1, y0, y1, z0, z1):
        out.append((name, box_tris((E[0] + x0 * U, y0, E[2] + z0 * U), (E[0] + x1 * U, y1, E[2] + z1 * U))))
    pan_top = E[1] - 0.80 * U
    bx('seat-base', -0.20, 0.20, fy, pan_top - 0.10 * U, -0.15, back_max - 0.08)
    bx('seat-pan', -0.23, 0.23, pan_top - 0.10 * U, pan_top, -0.22, back_max - 0.08)
    bx('seat-back', -0.22, 0.22, pan_top, E[1] - 0.02 * U, back_max - 0.12, back_max - 0.02)
    bx('headrest', -0.12, 0.12, E[1] - 0.06 * U, E[1] + 0.16 * U, back_max - 0.15, back_max - 0.04)
    ctop = E[1] - 0.47 * U; cz0 = max(-0.30, (zf - E[2]) / U + 0.02); cz1 = back_max - 0.02
    lx, rx = (xl - E[0]) / U, (xr - E[0]) / U
    if lx < -0.26:
        bx('console-left', lx, -0.245, fy, ctop, cz0, cz1)
    if rx > 0.26:
        bx('console-right', 0.245, rx, fy, ctop, cz0, cz1)
    bx('seat-rail-left', -0.27, -0.23, pan_top - 0.10 * U, E[1] + 0.24 * U, back_max - 0.10, back_max)
    bx('seat-rail-right', 0.23, 0.27, pan_top - 0.10 * U, E[1] + 0.24 * U, back_max - 0.10, back_max)
    qtop = E[1] - 0.42 * U
    bx('throttle-quadrant', -0.33, -0.27, qtop - 0.10 * U, qtop, -0.12, 0.14)
    if stick == 0:
        sz = max(-0.40, (zf - E[2]) / U + 0.15); top = E[1] - 0.52 * U
        bx('stick', -0.022, 0.022, fy, top + 0.13 * U, sz - 0.06, sz + 0.06)
    for sgn in (-1, 1):
        pz = max(-0.85, (zf - E[2]) / U + 0.08); px = 0.12 * sgn
        bx('pedal', px - 0.04, px + 0.04, fy + 0.08 * U, fy + 0.24 * U, pz - 0.012, pz + 0.012)
    return [(n, np.array(t, np.float64)) for n, t in out], dict(lo=lo.tolist(), hi=hi.tolist(), floor=fy)


# ----------------------------------------------------------------------------------------------- airframe envelope
def ray_hits(T, e1, e2, O, d):
    """Moeller-Trumbore, many origins O (N,3), one direction d: (N, len(T)) hit distances (inf = miss), both faces."""
    p = np.cross(d, e2); det = (e1 * p).sum(1)
    ok = np.abs(det) > 1e-12
    inv = np.where(ok, 1.0 / np.where(ok, det, 1), 0)
    s = O[:, None, :] - T[None, :, 0]
    u = (s * p[None]).sum(2) * inv
    q = np.cross(s, e1[None])
    v = (q * d).sum(2) * inv
    t = (e2[None] * q).sum(2) * inv
    m = ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 1e-6)
    return np.where(m, t, np.inf)


class Envelope:
    """The airframe's outer skin sampled on grids (model units, step `h`): half-width on each side of the centre plane x = cx (outermost hit of a
    ray from the centre plane along +-X) per (y, z), and top / bottom (first hit from above / below) per (x, z). Includes the canopy glass.
    inside(p): |x - cx| <= half-width(side, y, z) and bottom(x, z) <= y <= top(x, z), each with `slack`. Points with no skin at all = outside."""

    def __init__(self, tris, cx, lo, hi, h=0.1):
        self.cx, self.lo, self.h = cx, np.asarray(lo, float), h
        self.n = np.ceil((np.asarray(hi, float) - self.lo) / h).astype(int) + 1
        tmin, tmax = tris.min(1), tris.max(1)
        T = tris[np.all(tmax >= self.lo - 1, 1) & np.all(tmin <= np.asarray(hi) + 1, 1)]
        e1, e2 = T[:, 1] - T[:, 0], T[:, 2] - T[:, 0]
        xs, ys, zs = [self.lo[a] + h * np.arange(self.n[a]) for a in range(3)]
        Y, Z = np.meshgrid(ys, zs, indexing='ij')
        self.hw = []
        for sgn in (1.0, -1.0):
            O = np.stack([np.full(Y.size, cx), Y.ravel(), Z.ravel()], 1)
            out = np.full(Y.size, -1.0)
            for i in range(0, len(O), 2000):
                t = ray_hits(T, e1, e2, O[i:i + 2000], np.array([sgn, 0, 0]))
                t = np.where(np.isfinite(t), t, -1).max(1)
                out[i:i + 2000] = t
            self.hw.append(out.reshape(Y.shape))
        X, Z2 = np.meshgrid(xs, zs, indexing='ij')
        ytop, ybot = hi[1] + 2.0, lo[1] - 2.0
        O = np.stack([X.ravel(), np.full(X.size, ytop), Z2.ravel()], 1)
        top = np.full(X.size, -np.inf); bot = np.full(X.size, np.inf)
        for i in range(0, len(O), 2000):
            t = ray_hits(T, e1, e2, O[i:i + 2000], np.array([0, -1.0, 0])).min(1)
            top[i:i + 2000] = np.where(np.isfinite(t), ytop - t, -np.inf)
            O2 = O[i:i + 2000].copy(); O2[:, 1] = ybot
            t = ray_hits(T, e1, e2, O2, np.array([0, 1.0, 0])).min(1)
            bot[i:i + 2000] = np.where(np.isfinite(t), ybot + t, np.inf)
        self.top = top.reshape(X.shape); self.bot = bot.reshape(X.shape)

    def _ix(self, v, a):
        return np.clip(np.rint((v - self.lo[a]) / self.h).astype(int), 0, self.n[a] - 1)

    def inside(self, P, slack=0.15):
        P = np.atleast_2d(P)
        ix, iy, iz = self._ix(P[:, 0], 0), self._ix(P[:, 1], 1), self._ix(P[:, 2], 2)
        dx = P[:, 0] - self.cx
        hw = np.where(dx >= 0, self.hw[0][iy, iz], self.hw[1][iy, iz])
        inb = np.all((P >= self.lo) & (P <= self.lo + self.h * (self.n - 1)), 1)
        return inb & (np.abs(dx) <= hw + slack) & (P[:, 1] <= self.top[ix, iz] + slack) & (P[:, 1] >= self.bot[ix, iz] - slack)


def body_parts(z):
    """Exterior parts that form the airframe skin around the cockpit incl. the canopy glass: drawn MF_EXTERIOR parts."""
    return [k for k, p in enumerate(z['parts']) if p['ic'] and (p['mflags'] & MF_EXTERIOR) and not (p['mflags'] & (MF_HIDDEN | MF_COCKPIT_DUP))]


def scene(z, interior_on=True):
    """Opaque drawn triangles as (V (T,3,3), cls (T,) 0 exterior / 1 game cockpit part / 2 synthetic interior, name list per triangle)."""
    eye = np.array(z['eye'], float)
    V, C, N = [], [], []
    for k, p in enumerate(z['parts']):
        if not p['ic'] or (p['mflags'] & (MF_HIDDEN | MF_COCKPIT_DUP)) or (p['mflags'] & MF_BLEND):
            continue
        T = tris_of(z, k); V.append(T); C.append(np.full(len(T), 0 if p['mflags'] & MF_EXTERIOR else 1)); N += [p['name'].split('/')[-1]] * len(T)
    if interior_on:
        for name, T in interior(z, eye)[0]:
            V.append(T); C.append(np.full(len(T), 2)); N += [name] * len(T)
    return np.concatenate(V), np.concatenate(C), N


def make_envelope(z, h=0.1):
    eye = np.array(z['eye'], float)
    skin = np.concatenate([tris_of(z, k) for k in body_parts(z)])
    lo = eye + np.array([-5.0, -9.0, -14.0]); hi = eye + np.array([5.0, 6.0, 12.0])
    return Envelope(skin, 0.0, lo, hi, h)   # the airframe's symmetry plane (e06b/su32 seat the pilot off-centre)


def visible_outside(z, env, V, C, poses, W=240, H=180, hfov=100, upm=5.0):
    """Render the opaque scene (back faces culled, no glass) from head poses (yaw, pitch, dx, dy, dz metres) at the pvb eye; count pixels whose
    visible surface is NOT exterior (class 1/2) and lies outside the airframe envelope. -> (total outside pixels, per-triangle hit counts)."""
    from model_raster import Raster, ypr
    eye0 = np.array(z['eye'], float)
    per_tri = np.zeros(len(V), np.int64); total = 0
    col = np.full((len(V), 3, 4), 200, np.float32); nrm = np.zeros((len(V), 3, 3), np.float32); nrm[..., 1] = 1
    for (yaw, pitch, dx, dy, dz) in poses:
        e = eye0 + upm * np.array([dx, dy, dz]); B = ypr(yaw, pitch)
        R = Raster(W, H, hfov, near=0.02)
        R.draw(V, np.zeros((len(V), 3, 2), np.float32), col, nrm, np.full(len(V), -1), [], e, B, cull=True, atest=np.zeros(len(V), bool))
        hit = np.nonzero(R.tri >= 0)[0]
        if not len(hit):
            continue
        src = R.SRC[R.tri[hit]]
        m = C[src] > 0
        hit, src = hit[m], src[m]
        yy, xx = np.divmod(hit, W)
        zc = 1.0 / R.iz[hit]
        cam = np.stack([(xx + 0.5 - W / 2) / R.fx * zc, -(yy + 0.5 - H / 2) / R.fy * zc, zc], 1)
        P = e + cam @ B
        out = ~env.inside(P)
        total += int(out.sum()); np.add.at(per_tri, src[out], 1)
    return total, per_tri


POSES = [(yaw, pitch, dx, dy, 0.0) for (dx, dy) in ((0, 0), (-0.15, 0.08), (0.15, 0.08)) for yaw in range(0, 360, 30) for pitch in (-50, -20, 10)]


def survey_model(path, poses=POSES):
    z = read_zvrm(path)
    env = make_envelope(z)
    V, C, N = scene(z)
    tri_out = ~env.inside(V.mean(1))
    total, per_tri = visible_outside(z, env, V, C, poses)
    res = dict(model=os.path.basename(path), eye=np.round(z['eye'], 3).tolist(), visible_outside_px=total, poses=len(poses), by_name={})
    for i in np.nonzero((C > 0) & (tri_out | (per_tri > 0)))[0]:
        r = res['by_name'].setdefault(N[i], dict(cls=int(C[i]), out_tris=0, visible_px=0))
        r['out_tris'] += int(tri_out[i]); r['visible_px'] += int(per_tri[i])
    return res


# ----------------------------------------------------------------------------------------------- render (colour-coded)
def render(path, out, basis, eye, hfov=100, W=800, H=600, mesh=None):
    """grey exterior, blue game cockpit part, magenta synthetic interior; RED = a cockpit/interior surface outside the airframe envelope."""
    from model_raster import Raster
    from png_io import write_png
    z = read_zvrm(path)
    env = make_envelope(z)
    V, C = read_tri(mesh) if mesh else scene(z)[:2]
    pal = np.array([[150, 150, 150, 255], [90, 120, 200, 255], [200, 70, 190, 255]], np.float32)
    col = np.repeat(pal[C][:, None, :], 3, 1)
    n = np.cross(V[:, 1] - V[:, 0], V[:, 2] - V[:, 0]); n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
    NRM = np.repeat(n[:, None, :], 3, 1).astype(np.float32)
    R = Raster(W, H, hfov, near=0.02)
    R.draw(V, np.zeros((len(V), 3, 2), np.float32), col, NRM, np.full(len(V), -1), [], eye, basis, cull=True, atest=np.zeros(len(V), bool))
    img, _ = R.shade(np.array([200, 225, 240.]))
    img = img.reshape(-1, 3)
    hit = np.nonzero(R.tri >= 0)[0]
    src = R.SRC[R.tri[hit]]
    yy, xx = np.divmod(hit, W); zc = 1.0 / R.iz[hit]
    P = eye + np.stack([(xx + 0.5 - W / 2) / R.fx * zc, -(yy + 0.5 - H / 2) / R.fy * zc, zc], 1) @ basis
    bad = (C[src] > 0) & ~env.inside(P)
    img[hit[bad]] = img[hit[bad]] * 0.3 + np.array([255, 0, 0]) * 0.7
    write_png(out, np.clip(img.reshape(H, W, 3), 0, 255).astype(np.uint8))
    print(out, 'outside px', int(bad.sum()))


def read_tri(path):
    b = open(path, 'rb').read()
    n = int(np.frombuffer(b, '<u4', 1, 0)[0])
    rec = np.dtype([('p', '<f4', 9), ('s', 'u1')])
    r = np.frombuffer(b, rec, n, 4)
    return r['p'].reshape(-1, 3, 3).astype(np.float64), r['s'].astype(np.int64)


def survey_mesh(zpath, tripath, poses=POSES):
    z = read_zvrm(zpath)
    env = make_envelope(z)
    V, C = read_tri(tripath)
    tri_out = ~env.inside(V.mean(1))
    total, per_tri = visible_outside(z, env, V, C, poses)
    names = {1: 'game-cockpit', 2: 'synthetic'}
    res = dict(model=os.path.basename(zpath), mesh=os.path.basename(tripath), visible_outside_px=total, poses=len(poses), by_name={})
    for c in (1, 2):
        m = C == c
        res['by_name'][names[c]] = dict(cls=c, tris=int(m.sum()), out_tris=int((tri_out & m).sum()), visible_px=int(per_tri[m].sum()))
    return res


def clip_table(d):
    rows = []
    for f in sorted(x for x in os.listdir(d) if x.endswith('_vrcockpit.zvrm')):
        z = read_zvrm(os.path.join(d, f)); E = np.array(z['eye'], float); P = z['parts']
        env = make_envelope(z, 0.15)
        ck = [k for k, p in enumerate(P) if p['ic'] and not p['mflags'] & (MF_HIDDEN | MF_COCKPIT_DUP | MF_EXTERIOR | MF_BLEND | MF_GLASS)]
        T = np.concatenate([tris_of(z, k) for k in ck]); R = (T - E) / U
        ins = env.inside(T.mean(1), 0.2)
        ahead = -R[..., 2].min(); behind = R[ins][..., 2].max(); below = max(-R[..., 1].min(), 1.10); above = R[..., 1].max()
        half = np.abs(R[..., 0]).max()
        box = [round(float(v) + 0.02, 2) for v in (ahead, behind, below, above, half)]
        lo = np.array([-box[4], -box[2], -box[0]]); hi = np.array([box[4], box[3], box[1]])
        outv = np.any((R < lo) | (R > hi), 2)
        rows.append(dict(model_id=f[:-5], ahead=box[0], behind=box[1], below=box[2], above=box[3], half=box[4],
                         game_tris=int(len(T)), game_tris_touched=int(outv.any(1).sum()), game_tris_removed=int(outv.all(1).sum())))
        print('%-18s ahead %.2f behind %.2f below %.2f above %.2f half %.2f | game tris touched %d removed %d of %d' % (
            f[:-5], *box, rows[-1]['game_tris_touched'], rows[-1]['game_tris_removed'], len(T)), flush=True)
    return rows


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest='cmd', required=True)
    a1 = sp.add_parser('survey'); a1.add_argument('dir'); a1.add_argument('--types'); a1.add_argument('--json'); a1.add_argument('--mesh-dir')
    a3 = sp.add_parser('table'); a3.add_argument('dir'); a3.add_argument('--json'); a3.add_argument('--cpp')
    a2 = sp.add_parser('render'); a2.add_argument('model'); a2.add_argument('out'); a2.add_argument('--mesh')
    a2.add_argument('--ypr', default='0,0,0'); a2.add_argument('--from'); a2.add_argument('--look'); a2.add_argument('--hfov', type=float, default=100)
    a = ap.parse_args()
    if a.cmd == 'survey':
        files = sorted(f for f in os.listdir(a.dir) if f.endswith('_vrcockpit.zvrm'))
        if a.types:
            files = [f for f in files if f[:4] in a.types.split(',')]
        allr = []
        for f in files:
            if a.mesh_dir:
                tp = os.path.join(a.mesh_dir, f[:-5] + '.tri')
                if not os.path.exists(tp):
                    continue
                r = survey_mesh(os.path.join(a.dir, f), tp); allr.append(r)
                print('%-22s visible-outside px %6d  %s' % (f, r['visible_outside_px'], ' '.join('%s:%dpx/%d-of-%dt' % (k, v['visible_px'], v['out_tris'], v['tris'])
                      for k, v in r['by_name'].items())), flush=True)
                continue
            r = survey_model(os.path.join(a.dir, f)); allr.append(r)
            top = sorted(r['by_name'].items(), key=lambda kv: -kv[1]['visible_px'])[:8]
            print('%-22s visible-outside px %6d  %s' % (f, r['visible_outside_px'], ' '.join('%s%s:%d/%dt' % ('*' if v['cls'] == 2 else '', k, v['visible_px'], v['out_tris']) for k, v in top)), flush=True)
        if a.json:
            json.dump(allr, open(a.json, 'w'), indent=1)
    elif a.cmd == 'table':
        rows = clip_table(a.dir)
        if a.json:
            rule = __doc__.split('table: ')[1].split('Docs:')[0].replace(chr(10), ' ').strip()
            json.dump(dict(generator='research/world-render-data/cockpit_clip_survey.py table', units='metres from the pvb eye', rule=rule, volumes=rows),
                      open(a.json, 'w'), indent=1)
        if a.cpp:
            with open(a.cpp, 'w') as fo:
                for r in rows:
                    fo.write('    {"%s", %.2ff, %.2ff, %.2ff, %.2ff, %.2ff},' % (r['model_id'], r['ahead'], r['behind'], r['below'], r['above'], r['half']) + chr(10))
    else:
        from model_raster import ypr, look_at
        z = read_zvrm(a.model)
        if getattr(a, 'from'):
            eye, basis = look_at([float(v) for v in getattr(a, 'from').split(',')], [float(v) for v in a.look.split(',')])
        else:
            eye = np.array(z['eye'], float); basis = ypr(*[float(v) for v in a.ypr.split(',')])
        render(a.model, a.out, basis, eye, a.hfov, mesh=a.mesh)


if __name__ == '__main__':
    main()
