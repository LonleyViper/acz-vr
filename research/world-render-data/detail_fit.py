"""DTEX fit (docs/world-render/detail-texture.md): the detail texture's world repeat and amplitude from the grain of a game-window shot.

The game shot's near ground (parked on k00a: eye H ~ 12 units over the runway plateau, D-378) is high-passed per row (7-px box), and two statistics are taken per
6-row band: the residual sd (8-bit display units) and its lag-1 horizontal autocorrelation (ac1, the grain size in pixels). Synthetic renders of the stage's
decoded _dt texture laid on the ground plane with the same camera (horizon row yh, focal f px, supersampled 4x4 = the desktop downsample) give the same
statistics for each candidate repeat S (units per 128 texels), averaged over 4 texture headings (the shot's heading vs the texture axes is unknown).
best S = min sum (ac1_shot - ac1_synth)^2 over the bands; amplitude = mean(sd_shot / sd_synth) in units of the CLUT value (no alpha).

Usage: python -I detail_fit.py <decoded _dt.png (env_gim.py)> <game shot.png> [x0 x1 y0 y1 yh f H cx]
Defaults = step103 runN1-shots/13-runway.png (1280x720 desktop shot, game view ~1200x675, vertical FOV 38.1 deg -> f 977 px, the HUD 0-pitch line at y 385).
stdlib + numpy only (png_io.py).
"""
import math, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import png_io


def hp(r, k=7):
    return r - np.convolve(r, np.ones(k) / k, 'same')


def band_stats(rows):
    sd = ac1 = 0.0
    for r in rows:
        r = hp(r)[4:-4]
        r = r - r.mean()
        v = (r * r).mean()
        sd += math.sqrt(v) / len(rows)
        ac1 += (r[:-1] * r[1:]).mean() / v / len(rows)
    return sd, ac1


def main(argv):
    tex = png_io.read_png(argv[1]).astype(np.float64)[..., :3].mean(2)
    shot = png_io.read_png(argv[2]).astype(np.float64)[..., :3].mean(2)
    x0, x1, y0, y1, yh, f, H, cx = [float(v) for v in argv[3:11]] if len(argv) >= 11 else (870, 990, 430, 478, 385.0, 977.0, 12.0, 605.0)
    x0, x1, y0, y1 = int(x0), int(x1), int(y0), int(y1)
    bands = list(range(y0, y1, 6))
    n = tex.shape[0]

    def bil(u, v):
        u = u - 0.5; v = v - 0.5
        iu = np.floor(u).astype(int); iv = np.floor(v).astype(int); fu = u - iu; fv = v - iv
        t = lambda a, b: tex[b % n, a % n]
        return t(iu, iv) * (1 - fu) * (1 - fv) + t(iu + 1, iv) * fu * (1 - fv) + t(iu, iv + 1) * (1 - fu) * fv + t(iu + 1, iv + 1) * fu * fv

    def synth(S, ang, ss=4):
        ca, sa = math.cos(ang), math.sin(ang)
        out = {}
        for y in range(bands[0], bands[-1] + 6):
            acc = np.zeros(x1 - x0)
            for j in range(ss):
                for i in range(ss):
                    yy = y + (j + 0.5) / ss - 0.5
                    xx = np.arange(x0, x1) + (i + 0.5) / ss - 0.5
                    d = H * f / (yy - yh)
                    lat = (xx - cx) * d / f
                    acc += bil((lat * ca - d * sa) / S * n, (lat * sa + d * ca) / S * n)
            out[y] = acc / (ss * ss)
        return out

    shot_st = [band_stats([shot[y, x0:x1] for y in range(b, b + 6)]) for b in bands]
    print('shot sd/ac1 per band:', ' '.join('%.2f/%.2f' % s for s in shot_st))
    best = None
    for S in [8, 16, 24, 32, 48, 64, 96, 128, 256]:
        per = [synth(S, a) for a in (0.0, 0.7, 1.57, 2.4)]
        st = [np.mean([band_stats([p[y] for y in range(b, b + 6)]) for p in per], axis=0) for b in bands]
        err = sum((a[1] - b[1]) ** 2 for a, b in zip(st, shot_st))
        amp = float(np.mean([b[0] / a[0] for a, b in zip(st, shot_st)]))
        print('S %4d units/repeat  ac1 err %.3f  amplitude %.3f x CLUT' % (S, err, amp))
        if best is None or err < best[1]:
            best = (S, err, amp)
    print('best repeat %d units (err %.3f), amplitude %.3f x CLUT value' % best)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
