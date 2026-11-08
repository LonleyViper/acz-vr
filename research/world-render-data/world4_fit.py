"""WORLD-4: fit the 'game look' parameters (look:1) from registered game-window screenshots (offline, numpy only).

  python -I world4_fit.py [--shots-root <research/experiments>] [--glass 0.97,0.94,0.935]

Inputs (all game-window screenshots of mission 02 / stage m02a, heading NE):
  pre-V   step56 .../nosedown/acz-20261026-231030-441-pre-V.png   HUD view (no cockpit model, no canopy glass), level: SKY profile.
  step62  step62 .../shots/acz-20261027-004711-116-T6.75.png       cockpit view, level, E6 pose (-14340, 4003.575, 122000), registered
                                                                    (local-contrast correlation 0.70 at the logged pose): far GROUND 11-70 km.
  step56  step56 .../nosedown/acz-20261026-231038-575-T7.03.png     cockpit view, pitch -29.3 deg, E6 pose: near GROUND 4-14 km.
Cockpit-view pixels are divided by the canopy 'glass' factor (cockpit / HUD-view ratio of the same sky rows, measured here) so that every fit
describes the world WITHOUT the canopy (what our world layer must show under our own cockpit model).

Model (display domain, per channel), the same one the shader/CPU implement under look:1 (terrain_stage.cpp look_* / xr_stereo_blit.hlsl tl_*):
  low sky   S(el) = lerp(F, k * C1, smoothstep(e_lo, e1, el))                              for el < e1   (F = fog colour, C1 = sph row stop 1)
  sky       above e1: the WORLD-2 stops with stop 1 = k*C1 at e1 and the stops 2/3 at e2/e3
  ground    lerp(gain * texel * light, S(el_ray), 1 - exp(-t / L))
Prints the fitted values and per-band / per-row residuals; the C++ constants kLook* carry the result.
"""
import argparse, math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import world4_compare as wc

EXP = '<repo>/research/experiments/'
R62 = [(740, 320, 1680, 1010), (560, 760, 1760, 1353), (130, 860, 900, 1353), (1950, 1080, 2406, 1353), (0, 0, 420, 380), (2180, 0, 2406, 400)]
R56 = [(740, 360, 1680, 1010), (560, 820, 1760, 1353), (130, 860, 900, 1353), (1950, 1080, 2406, 1353), (0, 0, 2406, 160), (0, 0, 420, 330), (2100, 0, 2406, 330)]
RPV = [(0, 60, 2406, 330), (740, 330, 1680, 1010), (560, 1150, 1760, 1353)]
POSE62 = [-14340, 4003.575, 122000, -0.785398, 0.0]
POSE56 = [-13854, 3647.3, 121515.3, -0.785398, -0.5109]
POSEPV = [-18442, 4003.575, 126102, -0.785398, 0.0]   # timer-derived (7.40 s before step62 at 784 u/s); only ray elevations are used
# m02a sph row 0 (toward the moon/light azimuth), display: stop 0 (horizon) .. 3 (high)
C = np.array([[135, 156, 148], [140, 164, 155], [92, 132, 141], [65, 100, 112]], float)


def smooth(lo, hi, x):
    t = np.clip((x - lo) / (hi - lo), 0, 1)
    return t * t * (3 - 2 * t)


def sky_model(el, F, k, e_lo, e1, e2, e3):
    el = np.asarray(el, float)
    low = F + (k * C[1] - F) * smooth(e_lo, e1, el)[..., None]
    f1 = np.clip((el - e1) / (e2 - e1), 0, 1)[..., None]
    mid = k * C[1] + (C[2] - k * C[1]) * f1
    f2 = np.clip((el - e2) / (e3 - e2), 0, 1); f2 = (f2 * (2 - f2))[..., None]
    high = C[2] + (C[3] - C[2]) * f2
    return np.where((el < e1)[..., None], low, np.where((el < e2)[..., None], mid, high))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=EXP); ap.add_argument('--glass', default='')
    g = ap.parse_args(argv)
    pv = wc.prepare(g.root + 'step56-terrain-run4/game-window-shots/nosedown/acz-20261026-231030-441-pre-V.png', POSEPV, RPV, 0, 1e9)
    d62 = wc.prepare(g.root + 'step62-integrated-run1/shots/acz-20261027-004711-116-T6.75.png', POSE62, R62, 3, 70000)
    d56 = wc.prepare(g.root + 'step56-terrain-run4/game-window-shots/nosedown/acz-20261026-231038-575-T7.03.png', POSE56, R56, 3, 70000)
    # rows (elevation, colour) of the sky / beyond-terrain profile: pre-V (HUD view) and step62 (cockpit view)
    def rows(d, all_rows=True):
        el = np.degrees(np.arcsin(np.clip(d['tr']['d'][..., 1], -1, 1)))
        out = []
        for r0 in range(0, d['H'], 6):
            m = d['ok'][r0:r0 + 6]
            if m.sum() < 60:
                continue
            out.append((float(np.median(el[r0:r0 + 6][m])), np.median(d['g'][r0:r0 + 6][m], axis=0)))
        return out
    rp = [(e, c) for e, c in rows(pv) if -1.3 < e < 18.5]
    r62 = [(e, c) for e, c in rows(d62) if -1.3 < e < 12.5]
    # canopy glass: cockpit / HUD view per channel over matching elevations 2..12 deg
    if g.glass:
        glass = np.array([float(v) for v in g.glass.split(',')])
    else:
        srt = sorted(rp, key=lambda r: r[0])
        ep = np.array([e for e, _ in srt]); cp = np.array([c for _, c in srt])
        rat = []
        for e, c in r62:
            if 2.0 < e < 12.0:
                rat.append(c / np.array([np.interp(e, ep, cp[:, i]) for i in range(3)]))
        glass = np.median(np.array(rat), axis=0)
        print('canopy glass (cockpit / HUD view, sky 2..12 deg, %d rows): %s  (p25 %s, p75 %s)' % (
            len(rat), np.round(glass, 3), np.round(np.percentile(rat, 25, axis=0), 3), np.round(np.percentile(rat, 75, axis=0), 3)))
    # ground data (glass-corrected)
    def grd(d):
        m = d['m']
        el = np.degrees(np.arcsin(np.clip(wc.box(d['tr']['d'][..., 1], 3), -1, 1)))
        return d['tb'][m], d['gb'][m] / glass, d['tt'][m], el[m]
    T62, G62, t62, el62 = grd(d62); T56, G56, t56, el56 = grd(d56)
    T = np.concatenate([T62, T56]); G = np.concatenate([G62, G56]); t = np.concatenate([t62, t56]); el = np.concatenate([el62, el56])
    src = np.concatenate([np.zeros(len(t62)), np.ones(len(t56))])
    print('ground pixels: step62 %d (corr %.3f), step56 %d' % (len(t62), wc.corr_score(d62), len(t56)))
    # step 1: fog colour F, gain, L with the low-sky target; iterate sky <-> ground twice
    ep = np.array([e for e, _ in rp]); cp = np.array([c for _, c in rp])
    F = np.array([85.0, 107.0, 104.0]); best_sky = None
    for it in range(3):
        best_sky = None
        for k in np.arange(0.80, 1.001, 0.01):
            for e_lo in np.arange(-4.0, -0.49, 0.25):
                for e1 in np.arange(1.0, 4.01, 0.25):
                    for e2 in np.arange(5.0, 12.01, 0.5):
                        for e3 in (20.0, 23.0, 26.0, 30.0):
                            m_ = sky_model(ep, F, k, e_lo, e1, e2, e3)
                            sse = float(((m_ - cp) ** 2).sum())
                            if best_sky is None or sse < best_sky[0]:
                                best_sky = (sse, k, e_lo, e1, e2, e3)
        _, k, e_lo, e1, e2, e3 = best_sky
        best = None
        for L in np.arange(10000, 80001, 1000.0):
            f = 1 - np.exp(-t / L)
            S = sky_model(el, F, k, e_lo, e1, e2, e3)
            gain, sse = [], 0.0
            for ch in range(3):
                a = (1 - f) * T[:, ch]; y = G[:, ch] - f * S[:, ch]
                gg = float(a @ y / (a @ a)); r = gg * a - y
                gain.append(gg); sse += float(r @ r)
            if best is None or sse < best[0]:
                best = (sse, L, gain)
        _, L, gain = best
        # F: the steep-down limit of the haze target: least squares over pixels with el < e_lo (S == F there)
        f = 1 - np.exp(-t / L)
        sel = el < e_lo
        num = np.zeros(3); den = 0.0
        for ch in range(3):
            y = G[sel, ch] - (1 - f[sel]) * gain[ch] * T[sel, ch]
            num[ch] = float(f[sel] @ y); den = float(f[sel] @ f[sel])
        F = num / den
        print('iter %d: sky k %.2f e_lo %.2f e1 %.2f e2 %.1f e3 %.0f rms %.2f | ground L %.0f gain %s F %s rms %.2f' % (
            it, k, e_lo, e1, e2, e3, math.sqrt(best_sky[0] / (3 * len(ep))), L, np.round(gain, 3), np.round(F, 1), math.sqrt(best[0] / (3 * len(t)))))
    f = 1 - np.exp(-t / L)
    model = (1 - f)[:, None] * np.array(gain) * T + f[:, None] * sky_model(el, F, k, e_lo, e1, e2, e3)
    for s_ in (0, 1):
        for lo, hi in [(0, 5000), (5000, 8000), (8000, 11000), (11000, 14000), (14000, 18000), (18000, 24000), (24000, 32000), (32000, 42000), (42000, 55000), (55000, 70000)]:
            s = (src == s_) & (t >= lo) & (t < hi)
            if s.sum() > 100:
                print('  ground %s %6d-%6d n %6d tex %s game %s model %s' % (['step62', 'step56'][s_], lo, hi, s.sum(), np.round(T[s].mean(0)), np.round(G[s].mean(0)), np.round(model[s].mean(0))))
    ms = sky_model(ep, F, k, e_lo, e1, e2, e3)
    print('  sky rows (pre-V, HUD view): elevation game model')
    for e, c, mm in list(zip(ep, cp, ms))[::3]:
        print('    %6.2f %s %s' % (e, np.round(c).astype(int), np.round(mm).astype(int)))
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
