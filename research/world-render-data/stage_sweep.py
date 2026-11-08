"""STAGE-SWEEP: offline look at every baked stage before anyone flies it (docs/world-render/stage-sweep.md).

numpy port of the C++ CPU reference trace_env() (src/acz-vr/terrain_stage.cpp) with env:1 + look:1 exactly as shipped (kLook* constants of
terrain_stage.h, haze length 21,500, far 160,000, default yaw convention), reading the BAKED .zvrs v3 file (the file the runtime loads, loader checks
re-done here) and the .zvro (prop / tree counts). Output = the DISPLAY-domain colour (what a game-window screenshot would show; the runtime converts
it once to linear for the sRGB RTV). Not rendered: props/trees geometry, the canopy glass, the HUD.

  python -I stage_sweep.py render <stage> [--dir D:/acz-extract/stage-ps-all] [--out D:/acz-extract/out/sweep] [--w 480] [--fog const|stage|stagelight]
  python -I stage_sweep.py all [--stages a,b,..]      # one child process per stage, waits while system commit >= 88 %
  python -I stage_sweep.py sheets                     # contact sheets (6 stages each) + metrics.json summary
  python -I stage_sweep.py selftest

Poses per stage (game camera convention, forward = (-sin h cos p, sin p, -cos h cos p)), at the stage's representative point P:
  hi   ground(P) + 2500, level, heading = the azimuth of the moon sprite (else the light): sky row 0 ('toward the sprite')
  low  ground(P) + 300,  level, heading + 180 deg: sky row 2 (away)
  down ground(P) + 2500, pitch -30 deg, heading + 90 deg
P = m02a: the mission-02 E6 start (25137, 4000, 82532); else the centroid of the .pdl placements (the .zvrs object list); else the centroid of the
non-filler 32x32 height chunks (.mha cells whose chunk differs from the most common chunk).
"""
import argparse, ctypes, hashlib, json, math, os, struct, subprocess, sys, time
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

STAGE_DIR = 'D:/acz-extract/stage-ps-all'
OUT_DIR = 'D:/acz-extract/out/sweep'
STAGES = ['k00a', 'm01a', 'm02a', 'm03k', 'm03m', 'm03n', 'm04k', 'm04m', 'm04n', 'm05a', 'm06a', 'm07a', 'm08k', 'm08m', 'm08n', 'm09a',
          'm10k', 'm10m', 'm10n', 'm11a', 'm12a', 'm13a', 'm14k', 'm14m', 'm14n', 'm15a', 'm16k', 'm16m', 'm16n', 'm17a', 'm18a', 'r01a',
          'v01a', 'v02a', 'v03a', 'v04a', 'v05a', 'v06a', 'v08a']
M02A_E6 = (25137.0, 4000.0, 82532.0)

# ---- terrain_stage.h constants (mirrored; keep in sync)
K_MARCH, K_BISECT = 320, 8
K_STEP_MIN, K_STEP_GROW, K_STEP_MAX, K_STEP_FRAC = 128.0, 0.008, 8192.0, 0.4
K_LOD = 1.0 / 16000.0
K_FARFADE = 0.8
K_SEA_EPS = 0.5
K_CLOUD_LO, K_CLOUD_HI, K_CLOUD_OP, K_CLOUD_DARK, K_CLOUD_BLUR = 0.15, 0.85, 0.5, 0.4, 2.0
K_SEA_TINT = 0.25
K_WAVE_AMP, K_WAVE_FADE, K_WATER_F0, K_WATER_SPOW, K_WATER_SPEC = 0.12, 20000.0, 0.02, 256.0, 0.8
K_LIGHT_MIN, K_LIGHT_MAX, K_LIGHT_DEN_MIN_Y = 0.4, 1.4, 0.05
K_SUN_COS0, K_SUN_COS1, K_SUN_GLOW_POW, K_SUN_GLOW, K_SUN_MOON_SAME = 0.999954, 0.999981, 300.0, 0.25, 0.99
LOOK_GAIN = (0.64, 0.61, 0.50)
LOOK_FOG = (85 / 255.0, 105 / 255.0, 101 / 255.0)
LOOK_HAZE_L = 21500.0
LOOK_K, LOOK_ELO, LOOK_E1, LOOK_E2, LOOK_E3 = 0.90, -3.5, 2.5, 8.5, 20.0
LOOK_MOON = 0.11
FAR = 160000.0
TANX, TANY = 0.6152, 0.3460          # the game lens (63.2 x 38.2 deg), as the WORLD-4 fits
P_MOON, P_LIGHT, P_LCOL, P_AMB, P_SKY, P_WATER, P_WHI, P_CALT, P_CON, P_CCOL, P_STOPS, P_MISC = 0, 1, 2, 3, 4, 16, 17, 18, 19, 20, 24, 25


# ------------------------------------------------------------------------------------------------ file readers (loader rules re-done)
class LoadError(Exception):
    pass


def read_zvrs(path):
    b = open(path, 'rb').read()
    n = len(b)
    if n < 256 or b[:4] != b'ZVRS':
        raise LoadError('bad_magic')
    ver = struct.unpack_from('<I', b, 4)[0]
    if not 1 <= ver <= 3:
        raise LoadError('bad_version')
    flags, hdim, tgrid, ntiles, tilepx, nobj = struct.unpack_from('<6I', b, 16)
    cell, hstep, half, sea = struct.unpack_from('<4f', b, 40)
    off = struct.unpack_from('<5I', b, 56)
    size = struct.unpack_from('<I', b, 76)[0]
    if size != n:
        raise LoadError('bad_size')
    if (hdim, tgrid, tilepx) != (513, 256, 64) or not 0 < ntiles <= 1024 or nobj > 4096:
        raise LoadError('bad_header')
    if hashlib.sha256(b[256:]).digest() != b[192:224]:
        raise LoadError('bad_hash')
    s = dict(id=b[8:16].rstrip(b'\0').decode(), version=ver, flags=flags, ntiles=ntiles, nobj=nobj, cell=cell, hstep=hstep, half=half, sea=sea,
             bytes=n, payload_sha=b[192:224].hex())
    s['heights'] = np.frombuffer(b, np.uint8, 513 * 513, off[0]).reshape(513, 513)
    s['words'] = np.frombuffer(b, '<u2', 256 * 256, off[1]).reshape(256, 256)
    if int((s['words'] & 0x3ff).max()) >= ntiles:
        raise LoadError('bad_content(tile id)')
    s['tiles'] = np.frombuffer(b, np.uint8, ntiles * 4096, off[2]).reshape(ntiles, 64, 64)
    s['clut'] = np.frombuffer(b, np.uint8, 1024, off[3]).reshape(256, 4)
    s['objects'] = np.frombuffer(b, '<f4', nobj * 4, off[4]).reshape(nobj, 4) if nobj else np.zeros((0, 4), np.float32)
    s['tile_water'] = np.zeros(ntiles, np.uint8); s['water_key'] = 0
    if ver >= 2:
        otw, wk = struct.unpack_from('<II', b, 80)
        s['tile_water'] = np.frombuffer(b, np.uint8, ntiles, otw).copy(); s['water_key'] = wk
        if (s['tile_water'] > 1).any():
            raise LoadError('bad_content(tile_water)')
    s['has_water'] = bool(flags & 4)
    s['env'] = None
    s['env_refused'] = None
    if ver >= 3 and flags & 8:
        oe, es = struct.unpack_from('<II', b, 88)
        if b[oe:oe + 4] != b'ENV1':
            s['env_refused'] = 'bad env magic'
        else:
            P = np.frombuffer(b, '<f4', 128, oe + 8).reshape(32, 4).astype(np.float64)
            if not np.isfinite(P).all() or (np.abs(P) > 1e7).any():
                s['env_refused'] = 'non-finite parameter'
            elif not (P[P_MISC, :3] > 0).all() or not (P[P_STOPS, 0] < P[P_STOPS, 1] < P[P_STOPS, 2] < P[P_STOPS, 3]):
                s['env_refused'] = 'misc/stops'
            else:
                m = oe + 8 + 512
                cloud = np.frombuffer(b, np.uint8, 65536, m).reshape(256, 256); m += 65536
                wave = np.frombuffer(b, np.uint8, 16384, m).reshape(128, 128); m += 16384
                si = np.frombuffer(b, np.uint8, 1024, m).reshape(32, 32); m += 1024
                sa = np.frombuffer(b, np.uint8, 1024, m).reshape(32, 32)
                s['env'] = dict(P=P, cloud=cloud / 255.0, wave=wave / 255.0, si=si / 255.0, sa=sa / 255.0)
    s['sea_present'] = bool(flags & 2)
    return s


def read_zvro(path):
    """-> dict(props, trees, models_prop, models_tree, inst xyz) or raises."""
    import objects_baker
    r = objects_baker.read(open(path, 'rb').read())
    kinds = np.array([m[3] for m in r['models']], np.int64) if r['models'] else np.zeros(0, np.int64)
    inst = r['inst']
    k = kinds[inst['model'].astype(np.int64)] if len(inst) else np.zeros(0, np.int64)
    return dict(props=int((k == 0).sum()), trees=int((k == 1).sum()), models_prop=int((kinds == 0).sum()), models_tree=int((kinds == 1).sum()),
                version=r['version'], snapped=r['snapped'], prop_xyz=np.stack([inst['x'], inst['y'], inst['z']], 1)[k == 0].astype(np.float64))


# ------------------------------------------------------------------------------------------------ helpers
def sat(x):
    return np.clip(x, 0.0, 1.0)


def smooth01(lo, hi, x):
    t = sat((x - lo) / (hi - lo))
    return t * t * (3 - 2 * t)


def lerp(a, b, f):
    return a + (b - a) * f


def dot(a, b):
    return (a * b).sum(-1)


class Stage:
    def __init__(self, s, look=True, fog='const'):
        self.s = s
        self.hf = s['heights'].astype(np.float64)
        self.cell, self.half, self.hstep = float(s['cell']), float(s['half']), float(s['hstep'])
        self.sea = float(s['sea']) if s['sea_present'] else None
        self.hmax = float(s['heights'].max()) * self.hstep
        if self.sea is not None:
            self.hmax = max(self.hmax, self.sea)
        self.words = s['words']
        self.build_atlas()
        e = s['env']
        self.env = e
        if e is not None:
            P = e['P'].copy()
            if look:
                P[26] = LOOK_GAIN + (1.0,)
                # PROPOSALS (not shipped): fog:stage = F from the sph row-2 horizon colour (world4-impl.md 2.4 data candidate; m02a (85,102,98)
                # vs the fitted (85,105,101)); fog:stagelight = that + ground gain = mapset light colour / 128 (m02a (83,83,68)/128 =
                # (0.65,0.65,0.53) vs the fitted (0.64,0.61,0.50) +-0.06)
                P[27, :3] = LOOK_FOG if fog == 'const' else P[P_SKY + 8, :3]
                if fog == 'stagelight':
                    P[26, :3] = P[P_LCOL, :3] * 255.0 / 128.0
                P[28] = (LOOK_K, math.radians(LOOK_ELO), math.radians(LOOK_E1), math.radians(LOOK_E2))
                P[29, 0] = math.radians(LOOK_E3)
                P[P_MOON, 3] *= LOOK_MOON
            self.P = P
            self.look = look

    # -- atlas (build_atlas: RGBA8, river key texels alpha 0 with the tile's mean land colour, 2x2 box mips with +2>>2 rounding)
    def build_atlas(self):
        s = self.s
        clut = s['clut']
        L0 = np.empty((s['ntiles'], 64, 64, 4), np.uint8)
        L0[..., :3] = clut[s['tiles'], :3]
        L0[..., 3] = 255
        if s['has_water'] and len(s['tile_water']) == s['ntiles']:
            for t in np.nonzero(s['tile_water'])[0]:
                key = s['tiles'][t] == s['water_key']
                land = L0[t][~key][:, :3]
                mean = ((land.astype(np.int64).sum(0) + len(land) // 2) // len(land)).astype(np.uint8) if len(land) else np.array([20, 20, 20], np.uint8)
                L0[t][key] = np.concatenate([mean, [0]]).astype(np.uint8)
        self.lev = [L0]
        for L in range(1, 7):
            p = self.lev[-1].astype(np.uint16)
            q = p[:, 0::2, 0::2] + p[:, 0::2, 1::2] + p[:, 1::2, 0::2] + p[:, 1::2, 1::2]
            self.lev.append(((q + 2) >> 2).astype(np.uint8))

    def sample_level(self, L, tid, u, v):
        a = self.lev[L]
        sz = 64 >> L
        px, py = u * sz - 0.5, v * sz - 0.5
        fx, fy = np.floor(px), np.floor(py)
        wx, wy = px - fx, py - fy
        x0, y0 = fx.astype(np.int64), fy.astype(np.int64)
        acc = np.zeros(u.shape + (4,))
        for j in (0, 1):
            for i in (0, 1):
                xx = np.clip(x0 + i, 0, sz - 1); yy = np.clip(y0 + j, 0, sz - 1)
                w = (wx if i else 1 - wx) * (wy if j else 1 - wy)
                acc += w[:, None] * (a[tid, yy, xx].astype(np.float64) / 255.0)
        return acc

    def atlas_sample(self, tid, u, v, lod):
        lod = np.clip(lod, 0, 6)
        l0 = np.floor(lod).astype(np.int64); l1 = np.minimum(l0 + 1, 6); f = lod - l0
        c0 = np.zeros(u.shape + (4,)); c1 = np.zeros(u.shape + (4,))
        for L in range(7):
            m = l0 == L
            if m.any():
                c0[m] = self.sample_level(L, tid[m], u[m], v[m])
            m = (l1 == L) & (f > 0)
            if m.any():
                c1[m] = self.sample_level(L, tid[m], u[m], v[m])
        c1[f <= 0] = c0[f <= 0]
        return lerp(c0, c1, f[:, None])

    def height_bilinear(self, x, z):
        u = np.nan_to_num((x + self.half) / self.cell, nan=0.0); v = np.nan_to_num((z + self.half) / self.cell, nan=0.0)
        u = np.clip(u, 0, 512.0); v = np.clip(v, 0, 512.0)
        i = np.minimum(np.floor(u).astype(np.int64), 511); j = np.minimum(np.floor(v).astype(np.int64), 511)
        fu, fv = u - i, v - j
        hf = self.hf
        return lerp(lerp(hf[j, i], hf[j, i + 1], fu), lerp(hf[j + 1, i], hf[j + 1, i + 1], fu), fv) * self.hstep

    def height_eff(self, x, z):
        h = self.height_bilinear(x, z)
        return np.maximum(h, self.sea) if self.sea is not None else h

    # -- env (mirrors sky_at / env_sky / env_haze / cloud_alpha with look on)
    def env_map(self, img, u, v, wrap):
        n = img.shape[0]
        px, py = u - 0.5, v - 0.5
        fx, fy = np.floor(px), np.floor(py)
        wx, wy = px - fx, py - fy
        x0, y0 = fx.astype(np.int64), fy.astype(np.int64)
        acc = np.zeros(u.shape)
        for j in (0, 1):
            for i in (0, 1):
                xx, yy = x0 + i, y0 + j
                if wrap:
                    xx, yy = xx % n, yy % n
                else:
                    xx, yy = np.clip(xx, 0, n - 1), np.clip(yy, 0, n - 1)
                acc += (wx if i else 1 - wx) * (wy if j else 1 - wy) * img[yy, xx]
        return acc

    def sky_at(self, d, el):
        P = self.P
        ref = P[P_MOON] if P[P_MOON, 3] > 0 else P[P_LIGHT]
        hl = math.hypot(ref[0], ref[2])
        c = (d[:, 0] * ref[0] + d[:, 2] * ref[2]) / hl if hl > 1e-4 else np.zeros(len(d))
        t = 1 - c
        rows = []
        if self.look:
            F = P[27, :3]; k, elo, e1, e2 = P[28]; e3 = P[29, 0]
            for r in range(3):
                c1, c2, c3 = P[P_SKY + 4 * r + 1, :3], P[P_SKY + 4 * r + 2, :3], P[P_SKY + 4 * r + 3, :3]
                g = k * c1
                a = lerp(F, g, smooth01(elo, e1, el)[:, None])
                b_ = lerp(g, c2, sat((el - e1) / (e2 - e1))[:, None])
                f3 = sat((el - e2) / (e3 - e2)); f3 = f3 * (2 - f3)
                c_ = lerp(c2, c3, f3[:, None])
                rows.append(np.where((el < e1)[:, None], a, np.where((el < e2)[:, None], b_, c_)))
        else:
            st = P[P_STOPS]
            for r in range(3):
                C = [P[P_SKY + 4 * r + q, :3] for q in range(4)]
                f0 = sat((el - st[0]) / (st[1] - st[0])); f1 = sat((el - st[1]) / (st[2] - st[1]))
                f2 = sat((el - st[2]) / (st[3] - st[2])); f2 = f2 * (2 - f2)
                rows.append(np.where((el <= st[1])[:, None], lerp(C[0], C[1], f0[:, None]),
                                     np.where((el <= st[2])[:, None], lerp(C[1], C[2], f1[:, None]), lerp(C[2], C[3], f2[:, None]))))
        a1 = smooth01(0, 1, t)[:, None]; a2 = smooth01(1, 2, t)[:, None]
        return lerp(lerp(rows[0], rows[1], a1), rows[2], a2)

    def env_sky(self, d, sprite):
        els = np.arcsin(np.clip(d[:, 1], -1, 1))
        out = self.sky_at(d, els if self.look else np.maximum(els, 0))
        if not sprite:
            return out
        P = self.P
        sp, L = P[P_MOON], P[P_LIGHT]
        if L[3] > 0 and not (sp[3] > 0 and dot(sp[:3], L[:3]) > K_SUN_MOON_SAME):
            cs = dot(d, L[:3])
            m = cs > 0
            if m.any():
                lc = P[P_LCOL, :3]; mx = max(lc.max(), 1e-3)
                c = cs[m]
                disc = smooth01(K_SUN_COS0, K_SUN_COS1, c)[:, None]; glow = (c ** K_SUN_GLOW_POW * K_SUN_GLOW)[:, None]
                out[m] = sat(lerp(out[m] + lc / mx * glow, lc / mx, disc))
        if sp[3] > 0:
            q = dot(d, sp[:3]); rl = math.hypot(sp[0], sp[2])
            if rl > 1e-4:
                R = np.array([-sp[2] / rl, 0.0, sp[0] / rl]); U = np.cross(R, sp[:3])
                ts = P[P_MISC, 0]
                with np.errstate(divide='ignore', invalid='ignore'):
                    u = dot(d, R) / (q * ts); v = dot(d, U) / (q * ts)
                m = (q > 0) & (np.abs(u) < 1) & (np.abs(v) < 1)
                if m.any():
                    tu = (u[m] * 0.5 + 0.5) * 32; tv = (0.5 - v[m] * 0.5) * 32
                    gi = self.env_map(self.env['si'], tu, tv, False); ga = self.env_map(self.env['sa'], tu, tv, False) * sp[3]
                    out[m] = sat(out[m] + (gi * ga)[:, None])
        return out

    def env_haze(self, d):
        return self.sky_at(d, np.arcsin(np.clip(d[:, 1], -1, 1)) if self.look else np.zeros(len(d)))

    def cloud_alpha(self, x, z):
        rep = self.P[P_MISC, 1]
        u, v = x / rep * 256, z / rep * 256
        cl = self.env['cloud']
        cov = (2 * self.env_map(cl, u, v, True) + self.env_map(cl, u + K_CLOUD_BLUR, v, True) + self.env_map(cl, u - K_CLOUD_BLUR, v, True) +
               self.env_map(cl, u, v + K_CLOUD_BLUR, True) + self.env_map(cl, u, v - K_CLOUD_BLUR, True)) / 6
        return smooth01(K_CLOUD_LO, K_CLOUD_HI, cov) * K_CLOUD_OP

    def cloud_on(self):
        return self.P[P_CON, 0] > 0.5

    def env_water(self, w, x, z, t, d):
        P = self.P
        hl, L = P[P_WHI, :3], P[P_LIGHT, :3]
        rep = P[P_MISC, 2]
        amp = K_WAVE_AMP * sat(1 - t / K_WAVE_FADE)
        uw, vw = x / rep * 256, z / rep * 256
        wv = self.env['wave']
        gx = self.env_map(wv, uw + 1, vw, True) - self.env_map(wv, uw - 1, vw, True)
        gz = self.env_map(wv, uw, vw + 1, True) - self.env_map(wv, uw, vw - 1, True)
        N = np.stack([-gx * amp, np.ones_like(gx), -gz * amp], 1)
        N /= np.linalg.norm(N, axis=1, keepdims=True)
        dn = dot(d, N)
        rf = d - 2 * dn[:, None] * N
        rf[:, 1] = np.abs(rf[:, 1])
        fr = K_WATER_F0 + (1 - K_WATER_F0) * (1 - sat(-dn)) ** 5
        sk = self.env_sky(rf, False)
        sp = sat(dot(rf, L)) ** K_WATER_SPOW * K_WATER_SPEC
        return sat(lerp(w, sk, fr[:, None]) + hl * sp[:, None])

    @staticmethod
    def haze_fraction(t, L):
        w = sat((t - K_FARFADE * FAR) / ((1 - K_FARFADE) * FAR)); w = w * w * (3 - 2 * w)
        return 1 - np.exp(-t / L) * (1 - w)

    # -- trace_env (env bits = all, look on): -> display (N,3), classes
    def trace(self, o, d):
        n = len(d)
        t = np.zeros(n); prev = np.zeros(n)
        active = np.ones(n, bool); hit = np.zeros(n, bool)
        for _ in range(K_MARCH):
            idx = np.nonzero(active)[0]
            if not len(idx):
                break
            ti = t[idx]
            px, py, pz = o[0] + d[idx, 0] * ti, o[1] + d[idx, 1] * ti, o[2] + d[idx, 2] * ti
            dy = py - self.height_eff(px, pz)
            h_ = dy < 0
            hit[idx[h_]] = True
            stop = h_ | ((py > self.hmax) & (d[idx, 1] >= 0)) | (ti >= FAR)
            active[idx[stop]] = False
            go = ~stop
            gi = idx[go]
            prev[gi] = t[gi]
            t[gi] = t[gi] + np.minimum(np.maximum(K_STEP_FRAC * dy[go], K_STEP_MIN + K_STEP_GROW * t[gi]), K_STEP_MAX)
        hidx = np.nonzero(hit)[0]
        lo, hi = prev[hidx].copy(), t[hidx].copy()
        dh = d[hidx]
        for _ in range(K_BISECT):
            m = 0.5 * (lo + hi)
            below = o[1] + dh[:, 1] * m < self.height_eff(o[0] + dh[:, 0] * m, o[2] + dh[:, 2] * m)
            hi = np.where(below, m, hi); lo = np.where(below, lo, m)
        out = np.zeros((n, 3))
        cls = np.zeros(n, np.uint8)            # 0 sky, 1 land, 2 sea plane, 3 river
        sky = ~hit
        out[sky] = self.env_sky(d[sky], True)
        tt = np.full(n, 1e30); tt[hidx] = hi
        if len(hidx):
            th = hi
            p = o + dh * th[:, None]
            x, z = p[:, 0], p[:, 2]
            hraw = self.height_bilinear(x, z)
            col = np.zeros((len(hidx), 3))
            u = np.clip(np.nan_to_num((x + self.half) / self.cell * 0.5), 0, 255.999); v = np.clip(np.nan_to_num((z + self.half) / self.cell * 0.5), 0, 255.999)
            tx, tz = np.floor(u).astype(np.int64), np.floor(v).astype(np.int64)
            tid = (self.words[tz, tx] & 0x3ff).astype(np.int64)
            lod = np.where(th * K_LOD > 1, np.log2(np.maximum(th * K_LOD, 1e-30)), 0.0)
            tex = self.atlas_sample(tid, u - tx, v - tz, lod)
            water = (hraw <= self.sea + K_SEA_EPS) if self.sea is not None else np.zeros(len(hidx), bool)
            P = self.P
            if water.any():
                base = lerp(tex[water, :3], P[P_WATER, :3], K_SEA_TINT)
                col[water] = self.env_water(base, x[water], z[water], th[water], dh[water])
            land = ~water
            if land.any():
                xl, zl, pl = x[land], z[land], p[land]
                c = tex[land, :3].copy()
                gx = (self.height_bilinear(xl + self.cell, zl) - self.height_bilinear(xl - self.cell, zl)) / (2 * self.cell)
                gz = (self.height_bilinear(xl, zl + self.cell) - self.height_bilinear(xl, zl - self.cell)) / (2 * self.cell)
                nl = np.sqrt(gx * gx + 1 + gz * gz)
                L, lc, am = P[P_LIGHT, :3], P[P_LCOL, :3], P[P_AMB, :3]
                sh = np.ones(len(xl))
                if self.cloud_on() and L[1] > K_LIGHT_DEN_MIN_Y:
                    sd = (P[P_CALT, 0] - pl[:, 1]) / L[1]
                    ok = sd > 0
                    sh[ok] = 1 - self.cloud_alpha(xl[ok] + L[0] * sd[ok], zl[ok] + L[2] * sd[ok]) * P[P_MISC, 3]
                ndl = np.maximum((-gx * L[0] + L[1] - gz * L[2]) / nl, 0) * sh
                den = np.maximum(am + lc * max(L[1], K_LIGHT_DEN_MIN_Y), 1e-3)
                shade = np.clip((am + lc * ndl[:, None]) / den, K_LIGHT_MIN, K_LIGHT_MAX)
                c = sat(c * shade)
                if self.look:
                    c = c * P[26, :3]
                a = tex[land, 3]
                rv = a < 1
                if rv.any():
                    wc = self.env_water(np.broadcast_to(P[P_WATER, :3], (int(rv.sum()), 3)), xl[rv], zl[rv], th[land][rv], dh[land][rv])
                    c[rv] = lerp(wc, c[rv], a[rv][:, None])
                col[land] = c
                cl = np.ones(len(xl), np.uint8); cl[a < 0.5] = 3
                cls[hidx[land]] = cl
            cls[hidx[water]] = 2
            f = self.haze_fraction(th, LOOK_HAZE_L if self.look else 55000.0)
            hz = self.env_haze(dh)
            out[hidx] = lerp(col, hz, f[:, None])
        # clouds (layer 0) over whatever the ray saw before its hit (sky: 1e30)
        if self.cloud_on():
            H = self.P[P_CALT, 0]
            with np.errstate(divide='ignore', invalid='ignore'):
                tc = (H - o[1]) / d[:, 1]
            m = (np.abs(d[:, 1]) > 1e-4) & (tc > 0) & (tc < tt)
            if m.any():
                al = self.cloud_alpha(o[0] + d[m, 0] * tc[m], o[2] + d[m, 2] * tc[m])
                lit, dark = self.P[P_CCOL + 1, :3], self.P[P_CCOL + 2, :3]
                cc = lerp(lit, dark, K_CLOUD_DARK) if o[1] < H else lit
                f = self.haze_fraction(tc[m], LOOK_HAZE_L if self.look else 55000.0)
                cc = lerp(np.broadcast_to(cc, (int(m.sum()), 3)), self.env_haze(d[m]), f[:, None])
                al = al * (1 - f)
                out[m] = cc * al[:, None] + out[m] * (1 - al[:, None])
        return out, cls, tt


def rays(W, H, h, p):
    fwd = np.array([-math.sin(h) * math.cos(p), math.sin(p), -math.cos(h) * math.cos(p)])
    right = np.array([math.cos(h), 0.0, -math.sin(h)])
    up = np.cross(right, fwd)
    xs = ((np.arange(W) + 0.5) / W * 2 - 1) * TANX
    ys = (1 - (np.arange(H) + 0.5) / H * 2) * TANY
    X, Y = np.meshgrid(xs, ys)
    d = fwd[None, None] + X[..., None] * right + Y[..., None] * up
    d = d.reshape(-1, 3)
    return d / np.linalg.norm(d, axis=1, keepdims=True)


# ------------------------------------------------------------------------------------------------ representative point
def rep_point(stage, s):
    if stage == 'm02a':
        return M02A_E6[0], M02A_E6[2], 'm02a E6 start (mission 02)'
    o = s['objects']
    if len(o):
        return float(o[:, 0].mean()), float(o[:, 2].mean()), 'pdl centroid (%d)' % len(o)
    hf = s['heights']
    keys = [hf[r * 32:r * 32 + 33, c * 32:c * 32 + 33].tobytes() for r in range(16) for c in range(16)]
    vals, counts = np.unique(np.array(keys, dtype=object), return_counts=True)
    filler = vals[counts.argmax()]
    pts = [(c, r) for r in range(16) for c in range(16) if keys[r * 16 + c] != filler]
    cell = s['cell']
    if not pts:
        return 0.0, 0.0, 'map centre (no non-filler chunk)'
    pts = np.array(pts, np.float64)
    x = (pts[:, 0].mean() * 32 + 16) * cell - s['half']; z = (pts[:, 1].mean() * 32 + 16) * cell - s['half']
    return float(x), float(z), 'non-filler chunk centroid (%d of 256)' % len(pts)


def lum(c):
    return c[..., 0] * 0.2126 + c[..., 1] * 0.7152 + c[..., 2] * 0.0722


# ------------------------------------------------------------------------------------------------ static per-stage checks
def static_checks(stage, s, zro):
    r = {}
    hf = s['heights']
    r['h_min'], r['h_max'] = int(hf.min()), int(hf.max())
    r['byte0_frac'] = round(float((hf == 0).mean()), 4)
    r['sea_flag'] = s['sea_present']; r['sea_level'] = s['sea']
    w = s['words']
    bits = {b: int(((w >> b) & 1).sum()) for b in range(10, 16) if ((w >> b) & 1).any()}
    r['tile_flag_bits'] = bits
    tid = (w & 0x3ff).astype(np.int64)
    use = np.bincount(tid.ravel(), minlength=s['ntiles'])
    r['ntiles'] = s['ntiles']
    r['tile_unused'] = int((use == 0).sum())
    r['tile_top_share'] = round(float(use.max() / use.sum()), 4)
    tiles = s['tiles']; clut = s['clut'][:, :3].astype(np.float64)
    tl = lum(clut)[tiles].reshape(s['ntiles'], -1)
    tmean = tl.mean(1)
    r['tile_flat'] = int((tiles.reshape(s['ntiles'], -1).max(1) == tiles.reshape(s['ntiles'], -1).min(1)).sum())
    r['tex_mean_lum'] = round(float((tmean[tid]).mean()), 1)
    r['clut_distinct'] = int(len(np.unique(s['clut'][:, :3], axis=0)))
    r['clut_alpha'] = [int(s['clut'][:, 3].min()), int(s['clut'][:, 3].max())]
    r['tex_black_frac'] = round(float((tl[tid.ravel()] < 8).mean()), 4)
    # near-white low-saturation texels (snow / concrete) over the map: the look gain (0.64, 0.61, 0.50) turns them khaki
    snowc = (lum(clut) > 150) & (clut.max(1) - clut.min(1) < 30)          # per palette entry
    per_tile = snowc[tiles].reshape(s['ntiles'], -1).mean(1)
    r['snow_frac'] = round(float((per_tile * use).sum() / use.sum()), 4)
    # sea evidence: tile cells (2x2 height cells) whose corners are all byte 0; their texture colour vs the rest; bit-13 overlap
    corners = np.stack([hf[0:-1:2, 0:-1:2], hf[0:-1:2, 2::2], hf[2::2, 0:-1:2], hf[2::2, 2::2]], 0).max(0)[:256, :256]
    z0 = corners == 0
    r['tile_cells_at0'] = int(z0.sum())
    rgb = clut[tiles].reshape(s['ntiles'], -1, 3).mean(1)
    if z0.any():
        r['tex_rgb_at0'] = [round(float(v)) for v in rgb[tid[z0]].mean(0)]
        r['tex_rgb_land'] = [round(float(v)) for v in rgb[tid[~z0]].mean(0)] if (~z0).any() else None
        b13 = ((w >> 13) & 1).astype(bool)
        r['bit13_on_at0'] = round(float(b13[z0].mean()), 3)
        r['bit13_on_land'] = round(float(b13[~z0].mean()), 3) if (~z0).any() else None
    r['water_key'] = s['water_key']; r['river_tiles'] = int(s['tile_water'].sum()); r['has_water'] = s['has_water']
    r['nobj_pdl'] = s['nobj']
    r['zvro'] = zro
    e = s['env']
    r['env'] = 'refused: ' + s['env_refused'] if s['env_refused'] else ('present' if e is not None else 'MISSING')
    if e is not None:
        P = e['P']
        tod = lambda v: [round(float(c) * 255) for c in v[:3]]
        r['moon_strength'] = round(float(P[P_MOON, 3]), 3)
        r['moon_elev_deg'] = round(math.degrees(math.asin(max(-1, min(1, P[P_MOON, 1])))), 1)
        r['light_elev_deg'] = round(math.degrees(math.asin(max(-1, min(1, P[P_LIGHT, 1])))), 1)
        r['light_is_moon'] = bool(P[P_MOON, 3] > 0 and dot(P[P_MOON, :3], P[P_LIGHT, :3]) > K_SUN_MOON_SAME)
        r['light_rgb'] = tod(P[P_LCOL]); r['ambient_rgb'] = tod(P[P_AMB])
        r['sky_rows'] = [[tod(P[P_SKY + 4 * row + k]) for k in range(4)] for row in range(3)]
        r['sky_lum_row0'] = round(float(np.mean([lum(P[P_SKY + k, :3]) * 255 for k in range(4)])), 1)
        r['sky_lum_all'] = round(float(np.mean([lum(P[P_SKY + k, :3]) * 255 for k in range(12)])), 1)
        r['water_rgb'] = tod(P[P_WATER]); r['cloud_alt'] = [round(float(v)) for v in P[P_CALT]]; r['cloud_on'] = [int(v) for v in P[P_CON]]
        r['cloud_rgb'] = [tod(P[P_CCOL + i]) for i in range(4)]
        r['cloud_cover'] = round(float((e['cloud'] > 0.5).mean()), 3)
        r['moon_tex_max'] = round(float((e['si'] * e['sa']).max()), 3)
        r['fog_stage_rgb'] = tod(P[P_SKY + 8])
    return r


def render_stage(stage, sdir, out, W, fog='const'):
    t0 = time.time()
    path = os.path.join(sdir, stage + '.zvrs')
    res = dict(stage=stage, fog=fog)
    try:
        s = read_zvrs(path)
    except (LoadError, OSError) as ex:
        res['load'] = 'REFUSED: %s' % ex
        return res
    res['load'] = 'ok'
    try:
        zro = read_zvro(os.path.join(sdir, stage + '.zvro'))
        zinfo = {k: v for k, v in zro.items() if k != 'prop_xyz'}
    except Exception as ex:   # noqa: BLE001  (any refusal is a finding)
        zro, zinfo = None, 'REFUSED: %s' % ex
    res.update(static_checks(stage, s, zinfo))
    x, z, why = rep_point(stage, s)
    st = Stage(s, look=True, fog=fog)
    g = float(st.height_eff(np.array([x]), np.array([z]))[0])
    res['rep'] = dict(x=round(x), z=round(z), why=why, ground=round(g))
    rr = np.arange(-10240, 10241, 1024.0)
    GX, GZ = np.meshgrid(rr + x, rr + z)
    res['rep']['ground_max_10km'] = round(float(st.height_eff(GX.ravel(), GZ.ravel()).max()))
    res['rep']['underground_at_4000'] = bool(g >= 4000.0)
    if stage == 'm02a':
        res['rep']['e6_y_above_ground'] = round(M02A_E6[1] - g)
    if s['env'] is None:
        return res
    P = st.P
    ref = P[P_MOON] if P[P_MOON, 3] > 0 else P[P_LIGHT]
    h0 = math.atan2(-ref[0], -ref[2])
    res['heading_deg'] = round(math.degrees(h0), 1)
    poses = dict(hi=(g + 2500, h0, 0.0), low=(g + 300, h0 + math.pi, 0.0), down=(g + 2500, h0 + math.pi / 2, math.radians(-30)))
    H = int(round(W * TANY / TANX))
    import png_io
    os.makedirs(out, exist_ok=True)
    res['poses'] = {}
    for name, (y, h, p) in poses.items():
        d = rays(W, H, h, p)
        o = np.array([x, y, z])
        col, cls, tt = st.trace(o, d)
        m = {}
        m['nonfinite'] = int((~np.isfinite(col)).any(1).sum())
        c255 = np.nan_to_num(col) * 255
        img = np.clip(np.round(c255), 0, 255).astype(np.uint8).reshape(H, W, 3)
        png_io.write_png(os.path.join(out, '%s_%s%s.png' % (stage, name, '' if fog == 'const' else '_fog' + fog)), img)
        L = lum(c255)
        for k, nm in ((0, 'sky'), (1, 'land'), (2, 'sea'), (3, 'river')):
            sel = cls == k
            m[nm + '_frac'] = round(float(sel.mean()), 4)
            if sel.sum() > 20:
                m[nm + '_lum'] = round(float(L[sel].mean()), 1)
                m[nm + '_rgb'] = [round(float(v)) for v in c255[sel].mean(0)]
        if (cls == 1).sum() > 20:
            near = (cls == 1) & (tt < 12000)
            if near.sum() > 20:
                m['land_near_rgb'] = [round(float(v)) for v in c255[near].mean(0)]
        m['lum_mean'] = round(float(L.mean()), 1); m['lum_min'] = round(float(L.min()), 1); m['lum_max'] = round(float(L.max()), 1)
        m['lum_std'] = round(float(L.std()), 1)
        m['eye_y'] = round(y); m['heading_deg'] = round(math.degrees(h), 1); m['pitch_deg'] = round(math.degrees(p), 1)
        res['poses'][name] = m
        del d, col, cls, tt, img, c255, L
    res['seconds'] = round(time.time() - t0, 1)
    return res


# ------------------------------------------------------------------------------------------------ memory gate
def commit_pct():
    class PI(ctypes.Structure):
        _fields_ = [('cb', ctypes.c_uint32), ('CommitTotal', ctypes.c_size_t), ('CommitLimit', ctypes.c_size_t), ('CommitPeak', ctypes.c_size_t),
                    ('PhysicalTotal', ctypes.c_size_t), ('PhysicalAvailable', ctypes.c_size_t), ('SystemCache', ctypes.c_size_t),
                    ('KernelTotal', ctypes.c_size_t), ('KernelPaged', ctypes.c_size_t), ('KernelNonpaged', ctypes.c_size_t),
                    ('PageSize', ctypes.c_size_t), ('HandleCount', ctypes.c_uint32), ('ProcessCount', ctypes.c_uint32), ('ThreadCount', ctypes.c_uint32)]
    pi = PI(); pi.cb = ctypes.sizeof(PI)
    if not ctypes.windll.psapi.GetPerformanceInfo(ctypes.byref(pi), pi.cb):
        return 0.0
    return 100.0 * pi.CommitTotal / pi.CommitLimit


# ------------------------------------------------------------------------------------------------ contact sheets (3x5 bitmap labels)
FONT = {'0': '###,#.#,#.#,#.#,###', '1': '.#.,##.,.#.,.#.,###', '2': '###,..#,###,#..,###', '3': '###,..#,###,..#,###', '4': '#.#,#.#,###,..#,..#',
        '5': '###,#..,###,..#,###', '6': '###,#..,###,#.#,###', '7': '###,..#,..#,..#,..#', '8': '###,#.#,###,#.#,###', '9': '###,#.#,###,..#,###',
        'a': '.#.,#.#,###,#.#,#.#', 'k': '#.#,#.#,##.,#.#,#.#', 'm': '#.#,###,###,#.#,#.#', 'n': '##.,#.#,#.#,#.#,#.#', 'r': '##.,#.#,##.,#.#,#.#',
        'v': '#.#,#.#,#.#,#.#,.#.', 'h': '#.#,#.#,###,#.#,#.#', 'i': '###,.#.,.#.,.#.,###', 'l': '#..,#..,#..,#..,###', 'o': '###,#.#,#.#,#.#,###',
        'w': '#.#,#.#,###,###,#.#', 'd': '##.,#.#,#.#,#.#,##.', 'g': '###,#..,#.#,#.#,###', 'f': '###,#..,##.,#..,#..', 's': '###,#..,###,..#,###',
        't': '###,.#.,.#.,.#.,.#.', 'e': '###,#..,##.,#..,###', 'c': '###,#..,#..,#..,###', ' ': '...,...,...,...,...', '_': '...,...,...,...,###'}


def label(img, text, x0, y0, sc=3):
    for i, ch in enumerate(text):
        g = FONT.get(ch, FONT[' ']).split(',')
        for r, row in enumerate(g):
            for c, v in enumerate(row):
                if v == '#':
                    x = x0 + (i * 4 + c) * sc; y = y0 + r * sc
                    img[y:y + sc, x:x + sc] = (255, 255, 0)
                    img[y + sc:y + sc + 1, x:x + sc] = 0


def sheets(out, stages, suffix=''):
    import png_io
    paths = []
    for k in range(0, len(stages), 6):
        grp = stages[k:k + 6]
        rows = []
        for stg in grp:
            ims = []
            for pose in ('hi', 'low', 'down'):
                p = os.path.join(out, '%s_%s%s.png' % (stg, pose, suffix))
                if os.path.exists(p):
                    im = png_io.read_png(p)[..., :3].astype(np.uint8).copy()
                else:
                    im = np.zeros((270, 480, 3), np.uint8)
                label(im, '%s %s' % (stg, pose), 6, 6)
                ims.append(im)
                ims.append(np.full((im.shape[0], 4, 3), 40, np.uint8))
            row = np.concatenate(ims[:-1], 1)
            rows.append(row); rows.append(np.full((4, row.shape[1], 3), 40, np.uint8))
        sheet = np.concatenate(rows[:-1], 0)
        p = os.path.join(out, 'sheet%s_%d_%s-%s.png' % (suffix, k // 6 + 1, grp[0], grp[-1]))
        png_io.write_png(p, sheet)
        paths.append(p)
    return paths


FALSE_SEA_FIXED = {'m03k', 'm03m', 'm03n', 'm06a', 'm10k', 'm10m', 'm10n', 'm16k', 'm16m', 'm16n', 'r01a', 'v05a'}


def flags_for(r, ref):
    """Automatic anomaly flags for one stage's metrics r (shipped look) against m02a's metrics ref."""
    f = []
    if r.get('load') != 'ok':
        return ['LOAD_REFUSED']
    if r.get('env') != 'present':
        f.append('ENV_' + str(r.get('env')).upper())
        return f
    P = r['poses']
    if any(p['nonfinite'] for p in P.values()):
        f.append('NONFINITE')
    for k, p in P.items():
        if p['sky_frac'] > 0.05 and p.get('sky_lum', 100) < 8:
            f.append('SKY_BLACK(%s)' % k)
        if p['sky_frac'] > 0.05 and p.get('sky_lum', 0) > 245:
            f.append('SKY_WHITE(%s)' % k)
        if p['lum_max'] > 254.5 and p['lum_mean'] > 200:
            f.append('BLOWN_OUT(%s)' % k)
    rel = r['sky_lum_all'] / ref['sky_lum_all']
    if rel < 0.35:
        f.append('NIGHT(sky %.2fx m02a)' % rel)
    elif rel < 0.75:
        f.append('DARK_SKY(%.2fx m02a)' % rel)
    rows = r['sky_rows']
    lumc = lambda c: 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]
    F = (85, 105, 101)
    hz = rows[2][0]
    if abs(lumc(F) - lumc(hz)) > 20 or max(abs(a - b) for a, b in zip(F, hz)) > 35:
        f.append('HAZE_TARGET_MISMATCH(F (85,105,101) vs stage %s)' % (tuple(hz),))
    if lumc(rows[0][1]) * LOOK_K + 20 < lumc(rows[0][2]):
        f.append('DARK_HORIZON_BAND(glow %d < mid %d)' % (lumc(rows[0][1]) * LOOK_K, lumc(rows[0][2])))
    land = [p.get('land_lum') for p in P.values() if p.get('land_frac', 0) > 0.1 and p.get('land_lum') is not None]
    if land and min(land) < 0.4 * min(p.get('land_lum', 1e9) for p in ref['poses'].values() if p.get('land_lum')):
        f.append('DARK_GROUND(%.0f)' % min(land))
    if r['light_is_moon']:
        f.append('LIGHT_IS_MOON')
    if r['stage'] in FALSE_SEA_FIXED:
        f.append('FALSE_SEA_REMOVED')
    if r['sea_flag']:
        f.append('SEA(bit13 %.2f of byte-0 tiles, %s)' % (r.get('bit13_on_at0', 0), 'flat stage: all water' if r['h_max'] == 0 else 'y=0'))
        if r['h_max'] == 0 and r.get('bit13_on_at0', 1) < 0.9:
            f.append('FLAT_STAGE_PARTLY_LAND(only %.0f%% sea tiles)' % (100 * r['bit13_on_at0']))
    z = r['zvro']
    if isinstance(z, str):
        f.append('ZVRO_REFUSED')
    else:
        if z['trees'] == 0:
            f.append('NO_TREES')
        if z['props'] == 0:
            f.append('NO_PROPS')
        if r['nobj_pdl'] and z['props'] < 0.8 * r['nobj_pdl']:
            f.append('PROPS_DROPPED(%d of %d)' % (z['props'], r['nobj_pdl']))
    rp = r['rep']
    if rp['underground_at_4000']:
        f.append('UNDERGROUND_AT_4000')
    elif rp['ground_max_10km'] >= 4000:
        f.append('TERRAIN_ABOVE_4000_WITHIN_10KM(%d)' % rp['ground_max_10km'])
    odd_bits = [b for b in r['tile_flag_bits'] if b not in ('10', '12', '13')]
    if odd_bits:
        f.append('TILE_FLAG_BITS(%s)' % ','.join('%s:%d' % (b, r['tile_flag_bits'][b]) for b in odd_bits))
    if r['tile_top_share'] > 0.25:
        f.append('TILE_ONE_DOMINATES(%.2f)' % r['tile_top_share'])
    if r['tile_flat'] > 0.05 * r['ntiles']:
        f.append('FLAT_TILES(%d)' % r['tile_flat'])
    if r['tex_black_frac'] > 0.10 or r['clut_distinct'] < 64:
        f.append('TEXTURE_ODD(black %.2f, clut %d)' % (r['tex_black_frac'], r['clut_distinct']))
    if r['snow_frac'] > 0.10:
        f.append('SNOW/WHITE_TEXELS(%.2f: look gain tints them khaki)' % r['snow_frac'])
    return f


def summary(mdir, out_json):
    ref = json.load(open(os.path.join(mdir, 'm02a.json')))
    rows = []
    for stg in STAGES:
        r = json.load(open(os.path.join(mdir, stg + '.json')))
        fl = flags_for(r, ref)
        keep = {k: r.get(k) for k in ('stage', 'load', 'env', 'h_min', 'h_max', 'byte0_frac', 'sea_flag', 'tile_flag_bits', 'ntiles', 'tile_top_share',
                                       'tile_flat', 'tex_mean_lum', 'tex_black_frac', 'clut_distinct', 'snow_frac', 'bit13_on_at0', 'bit13_on_land',
                                       'tex_rgb_at0', 'tex_rgb_land', 'river_tiles', 'nobj_pdl', 'zvro', 'moon_strength', 'light_elev_deg', 'light_is_moon',
                                       'light_rgb', 'ambient_rgb', 'sky_rows', 'sky_lum_all', 'water_rgb', 'cloud_alt', 'cloud_cover', 'rep', 'heading_deg')}
        keep['poses'] = {k: {q: p.get(q) for q in ('sky_frac', 'sky_lum', 'land_frac', 'land_lum', 'land_rgb', 'sea_frac', 'sea_rgb', 'river_frac',
                                                    'lum_mean', 'lum_min', 'lum_max', 'nonfinite', 'eye_y', 'heading_deg', 'pitch_deg')}
                         for k, p in r.get('poses', {}).items()}
        keep['flags'] = fl
        rows.append(keep)
    with open(out_json, 'w', newline='\n') as f:   # one stage per line (small, diffable)
        f.write('{"generator": "research/world-render-data/stage_sweep.py summary", "look": "env:1 look:1 (shipped kLook* constants)",\n')
        f.write(' "stage_dir": %s,\n "stages": [\n' % json.dumps(STAGE_DIR))
        f.write(',\n'.join('  ' + json.dumps(k, separators=(',', ':')) for k in rows))
        f.write('\n ]}\n')
    for k in rows:
        print('%s | %s' % (k['stage'], '; '.join(k['flags'])))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['render', 'all', 'sheets', 'selftest', 'summary'])
    ap.add_argument('--json', default=os.path.join(HERE, '..', '..', 'docs', 'world-render', 'stage-sweep-metrics.json'))
    ap.add_argument('stage', nargs='?')
    ap.add_argument('--dir', default=STAGE_DIR); ap.add_argument('--out', default=OUT_DIR); ap.add_argument('--w', type=int, default=480)
    ap.add_argument('--fog', default='const', choices=['const', 'stage', 'stagelight']); ap.add_argument('--stages', default='')
    g = ap.parse_args(argv)
    mdir = os.path.join(g.out, 'metrics')
    os.makedirs(mdir, exist_ok=True)
    if g.cmd == 'summary':
        summary(mdir, os.path.abspath(g.json))
        return 0
    if g.cmd == 'render':
        r = render_stage(g.stage, g.dir, g.out, g.w, g.fog)
        with open(os.path.join(mdir, '%s%s.json' % (g.stage, '' if g.fog == 'const' else '_fog' + g.fog)), 'w') as f:
            json.dump(r, f, indent=1)
        print(g.stage, r.get('load'), r.get('env'), r.get('seconds'))
        return 0
    if g.cmd == 'all':
        for stg in (g.stages.split(',') if g.stages else STAGES):
            while commit_pct() >= 88.0:
                print('commit %.1f%% >= 88%%: waiting 60 s' % commit_pct(), flush=True)
                time.sleep(60)
            subprocess.run([sys.executable, '-I', os.path.abspath(__file__), 'render', stg, '--dir', g.dir, '--out', g.out, '--w', str(g.w), '--fog', g.fog],
                           check=False)
        return 0
    if g.cmd == 'sheets':
        sfx = '' if g.fog == 'const' else '_fog' + g.fog
        stg = g.stages.split(',') if g.stages else STAGES
        for p in sheets(g.out, stg, sfx):
            print(p)
        return 0
    if g.cmd == 'selftest':
        # synthetic flat stage: sky above, ground below, finite colours; the env/look path vs hand values at a few rays
        s = dict(heights=np.zeros((513, 513), np.uint8), words=np.zeros((256, 256), np.uint16), tiles=np.full((1, 64, 64), 1, np.uint8),
                 clut=np.tile(np.array([[100, 120, 80, 128]], np.uint8), (256, 1)), objects=np.zeros((0, 4)), tile_water=np.zeros(1, np.uint8),
                 water_key=0, has_water=False, sea_present=False, sea=0.0, cell=1024.0, half=262144.0, hstep=80.0, ntiles=1, nobj=0,
                 env_refused=None)
        P = np.zeros((32, 4))
        P[P_MOON] = (0, 0.17, -0.98, 0.85); P[P_LIGHT] = (0.0, 0.77, -0.64, 1); P[P_LCOL, :3] = 0.3; P[P_AMB, :3] = 0.4
        for k in range(12):
            P[P_SKY + k, :3] = 0.5
        P[P_WATER, :3] = 0.3; P[P_CALT] = 9000; P[P_STOPS] = np.radians([0, 1.5, 6.5, 23]); P[P_MISC] = (0.014, 65536, 4096, 0.3)
        s['env'] = dict(P=P, cloud=np.zeros((256, 256)), wave=np.zeros((128, 128)), si=np.zeros((32, 32)), sa=np.zeros((32, 32)))
        st = Stage(s)
        d = rays(32, 18, 0.0, -0.2)
        col, cls, tt = st.trace(np.array([0, 2500.0, 0]), d)
        assert np.isfinite(col).all() and (cls == 1).any() and (cls == 0).any()
        # a ray straight down at 2500 u: t = 2500, colour = lerp(texel * gain * shade, F, haze)
        col1, cls1, tt1 = st.trace(np.array([0, 2500.0, 0]), np.array([[0, -1.0, 0]]))
        assert abs(tt1[0] - 2500) < 1.0, tt1
        f = 1 - math.exp(-2500 / LOOK_HAZE_L)
        exp = (np.array([100, 120, 80]) / 255 * np.array(LOOK_GAIN)) * (1 - f) + np.array(LOOK_FOG) * f
        assert np.abs(col1[0] - exp).max() < 2e-3, (col1, exp)
        print('selftest ok')
        return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
