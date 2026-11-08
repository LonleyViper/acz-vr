"""WORLD-4: quantitative comparison of a game-window screenshot with the terrain data at the same pose (offline, numpy only).

  python -I world4_compare.py fit  <shot.png> --pose x,y,z,h,p [--mask-rect x0,y0,x1,y1 ...] [--out D:/acz-extract/out/world4/<name>]
  python -I world4_compare.py scan <shot.png> --pose x,y,z,h,p --along -3000,3000,500 [--across ...]   (pose refinement)
  python -I world4_compare.py ours <eye-upright.png> <shot.png> ...   (our eye dump resampled into the game lens, same statistics)

What it does:
* crops the game client area (title bar/borders found from the screenshot), half resolution, masks HUD/cockpit pixels
  (HUD green, marker blue, dark cockpit, near-white text; plus --mask-rect rectangles in client pixels at full resolution);
* traces the stage at the pose with world4_trace (the runtime's ray march) -> per pixel distance t and the RAW texel colour;
* blurs game and texel images with the same box (misregistration of a few pixels would otherwise flatten the regression);
* fits the PS2-style law  game = (1 - f(t)) * gain * texel + f(t) * fog,  f = clamp((t - n0) / (n1 - n0), 0, 1)
  by a grid over (n0, n1) with closed-form per-channel least squares for gain/fog at each grid point, and reports the
  per-band means for game, model and (for 'ours') our dump; also the sky profile (median colour per elevation row).
The game lens is the logged lens-fov 63.20 x 38.17 deg (tan 0.6152 x 0.3460, D-309 runB log).
"""
import argparse, math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import png_io
import world4_trace as w4

TANX, TANY = 0.6152, 0.3460


def client(shot):
    """-> client-area crop of a windowed screenshot (rows below the white title bar, inside the dark side borders)."""
    g = shot[..., :3].astype(float)
    rm = g.mean(axis=(1, 2))
    top = 0
    while top < 80 and rm[top] < 200:
        top += 1
    while top < 120 and rm[top] >= 200:
        top += 1
    cm = g[top:].mean(axis=(0, 2))
    left = 0
    while left < 20 and cm[left] > 60:
        left += 1
    while left < 20 and cm[left] < 40:
        left += 1
    # bottom: the one black separator row above the window's lower border (screenshots of the 2560x1440 windowed game: rows 31..1382)
    rows = g[:, left + 4:-12].mean(axis=(1, 2))
    bottom = top + 1
    while bottom < g.shape[0] and rows[bottom] >= 3:
        bottom += 1
    # width from the logged lens aspect (tan 0.6152 / 0.3460): the right border shading makes the last columns ambiguous
    right = left + int(round((bottom - top) * TANX / TANY)) - 1
    return g[top:bottom, left:right + 1], (left, top)


def hud_mask(g, rects=(), dil=7):
    """True = usable world pixel."""
    R, G, B = g[..., 0], g[..., 1], g[..., 2]
    lum = g.mean(-1)
    sat = g.max(-1) - g.min(-1)
    bad = (lum < 38) | ((G > R + 40) & (G > B + 30)) | (B > R + 60) | ((lum > 165) & (sat < 40))
    H, W = bad.shape
    p = np.pad(bad, dil); d = np.zeros_like(bad)
    for dy in range(-dil, dil + 1):
        d |= p[dil + dy:dil + dy + H, dil:dil + W]
    p = np.pad(d, dil); bad = np.zeros_like(d)
    for dx in range(-dil, dil + 1):
        bad |= p[dil:dil + H, dil + dx:dil + dx + W]
    for x0, y0, x1, y1 in rects:
        bad[y0:y1, x0:x1] = True
    return ~bad


def half(a):
    H, W = a.shape[0] // 2 * 2, a.shape[1] // 2 * 2
    a = a[:H, :W]
    return a.reshape(H // 2, 2, W // 2, 2, *a.shape[2:]).mean(axis=(1, 3))


def box(a, k):
    if k <= 0:
        return a
    c = np.cumsum(np.cumsum(np.pad(a, ((k + 1, k), (k + 1, k)) + ((0, 0),) * (a.ndim - 2), mode='edge'), 0), 1)
    n = 2 * k + 1
    return (c[n:, n:] - c[:-n, n:] - c[n:, :-n] + c[:-n, :-n]) / (n * n)


def fog_fit(T, G, t, n0s, n1s):
    """grid over linear fog (n0, n1); per channel least squares G = (1-f) * a * T + f * c. -> best dict."""
    best = None
    for n0 in n0s:
        for n1 in n1s:
            if n1 <= n0 + 1000:
                continue
            f = np.clip((t - n0) / (n1 - n0), 0, 1)
            sse, a, c = 0.0, [], []
            for ch in range(3):
                A = np.stack([(1 - f) * T[:, ch], f], 1)
                sol, *_ = np.linalg.lstsq(A, G[:, ch], rcond=None)
                r = A @ sol - G[:, ch]
                sse += float(r @ r); a.append(sol[0]); c.append(sol[1])
            if best is None or sse < best['sse']:
                best = dict(sse=sse, n0=n0, n1=n1, gain=a, fog=c, rms=math.sqrt(sse / (3 * len(t))))
    return best


def exp_fit(T, G, t, Ls):
    best = None
    for L in Ls:
        f = 1 - np.exp(-t / L)
        sse, a, c = 0.0, [], []
        for ch in range(3):
            A = np.stack([(1 - f) * T[:, ch], f], 1)
            sol, *_ = np.linalg.lstsq(A, G[:, ch], rcond=None)
            r = A @ sol - G[:, ch]
            sse += float(r @ r); a.append(sol[0]); c.append(sol[1])
        if best is None or sse < best['sse']:
            best = dict(sse=sse, L=L, gain=a, fog=c, rms=math.sqrt(sse / (3 * len(t))))
    return best


def prepare(shot_path, pose, rects, blur=3, tmax=60000.0):
    g, _ = client(png_io.read_png(shot_path))
    ok = hud_mask(g, rects)
    g2 = half(g); ok2 = half(ok[..., None].astype(float))[..., 0] > 0.99
    H, W = g2.shape[:2]
    tr = w4.trace_image('m02a', pose, W, H, TANX, TANY)
    tex = tr['tex'] * 255
    hit = tr['hit']; t = tr['t']
    okb = box(ok2.astype(float), blur) > 0.999
    hb = box(hit.astype(float), blur) > 0.999
    gb = box(g2, blur); tb = box(tex, blur); tt = box(np.where(hit, t, 0), blur)
    m = okb & hb & (tt < tmax)
    return dict(g=g2, ok=ok2, tr=tr, gb=gb, tb=tb, tt=tt, m=m, H=H, W=W)


def sky_profile(d, step=10):
    g, ok, tr = d['g'], d['ok'], d['tr']
    H = d['H']
    out = []
    el = np.degrees(np.arcsin(np.clip(tr['d'][..., 1], -1, 1)))
    for r0 in range(0, H, step):
        m = ok[r0:r0 + step] & ~tr['hit'][r0:r0 + step]
        if m.sum() < 50:
            continue
        out.append((float(np.median(el[r0:r0 + step][m])), np.median(g[r0:r0 + step][m], axis=0)))
    return out


def corr_score(d, k=12):
    """correlation of the LOCAL contrast (image minus a (2k+1)^2 box mean) of game and texel luminance over usable ground
    pixels: insensitive to the haze/distance trend, peaks at the registered pose."""
    m = d['m'] & (box(d['m'].astype(float), k) > 0.999)
    ga = d['gb'].mean(-1); tb = d['tb'].mean(-1)
    a = (ga - box(ga, k))[m]; b = (tb - box(tb, k))[m]
    return float(np.corrcoef(a, b)[0, 1]) if m.sum() > 100 else float('nan')


def report_fit(d, label='game', G=None):
    m = d['m']
    T = d['tb'][m]; t = d['tt'][m]
    G = d['gb'][m] if G is None else G[m]
    lin = fog_fit(T, G, t, np.arange(0, 30001, 1000.0), np.arange(10000, 90001, 2000.0))
    ex = exp_fit(T, G, t, np.arange(10000, 200001, 2500.0))
    print('%s: n=%d  linear fog n0=%.0f n1=%.0f gain=%s fog=%s rms=%.2f' % (label, len(t), lin['n0'], lin['n1'], np.round(lin['gain'], 3), np.round(lin['fog'], 1), lin['rms']))
    print('%s: exp fog L=%.0f gain=%s fog=%s rms=%.2f' % (label, ex['L'], np.round(ex['gain'], 3), np.round(ex['fog'], 1), ex['rms']))
    f = np.clip((t - lin['n0']) / (lin['n1'] - lin['n0']), 0, 1)
    model = (1 - f)[:, None] * np.array(lin['gain']) * T + f[:, None] * np.array(lin['fog'])
    for lo, hi in [(0, 6000), (6000, 9000), (9000, 12000), (12000, 16000), (16000, 22000), (22000, 30000), (30000, 45000), (45000, 60000)]:
        s = (t >= lo) & (t < hi)
        if s.sum() < 100:
            continue
        print('   band %5d-%5d n %6d tex %s %s %s model %s' % (lo, hi, s.sum(), np.round(T[s].mean(0)), label, np.round(G[s].mean(0)), np.round(model[s].mean(0))))
    return lin, ex


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=['fit', 'scan', 'ours'])
    ap.add_argument('paths', nargs='+')
    ap.add_argument('--pose', required=True)
    ap.add_argument('--mask-rect', action='append', default=[])
    ap.add_argument('--along', default='-3000,3000,500'); ap.add_argument('--across', default='0,0,1')
    ap.add_argument('--blur', type=int, default=3); ap.add_argument('--tmax', type=float, default=60000.0)
    g = ap.parse_args(argv)
    pose = [float(v) for v in g.pose.split(',')]
    rects = [tuple(int(v) for v in r.split(',')) for r in g.mask_rect]
    shot = g.paths[-1]
    if g.mode == 'scan':
        a0, a1, da = (float(v) for v in g.along.split(',')); c0, c1, dc = (float(v) for v in g.across.split(','))
        h = pose[3]
        fwd = np.array([-math.sin(h), -math.cos(h)]); rgt = np.array([math.cos(h), -math.sin(h)])
        for a in np.arange(a0, a1 + 1e-6, da):
            for c in np.arange(c0, c1 + 1e-6, dc):
                p = list(pose); p[0] += a * fwd[0] + c * rgt[0]; p[2] += a * fwd[1] + c * rgt[1]
                d = prepare(shot, p, rects, g.blur, g.tmax)
                print('along %7.0f across %7.0f  corr %.4f' % (a, c, corr_score(d)))
        return 0
    d = prepare(shot, pose, rects, g.blur, g.tmax)
    print('pose', pose, 'corr', round(corr_score(d), 4))
    report_fit(d, 'game')
    print('sky profile (elevation deg -> game median rgb):')
    for e, c in sky_profile(d):
        print('   %6.2f  %s' % (e, np.round(c).astype(int)))
    if g.mode == 'ours':
        o = png_io.read_png(g.paths[0])[..., :3].astype(float)
        oh, ow = o.shape[:2]
        H, W = d['H'], d['W']
        xs = ((np.arange(W) + 0.5) / W * 2 - 1) * TANX; ys = (1 - (np.arange(H) + 0.5) / H * 2) * TANY
        U, V = np.meshgrid(xs, ys)
        ox = np.clip(np.round((U + 1) / 2 * ow - 0.5).astype(int), 0, ow - 1); oy = np.clip(np.round((1 - V) / 2 * oh - 0.5).astype(int), 0, oh - 1)
        r = o[oy, ox]
        report_fit(d, 'ours', box(r, g.blur))
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
