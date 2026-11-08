"""WORLD-4: count trees (small dark blobs) per distance band in a game-window screenshot and in our offline render of the same pose.

  python -I world4_trees.py <game shot> <ours.png> <ours-without-trees.png> --pose x,y,z,h,p [--mask-rect ...]

Detector (identical for every image): luminance darker than the 31x31 local median by > `thr`, connected components (4-neighbour) of
area 6..400 px (full-resolution client pixels) whose bounding box is taller than 0.6x its width (trees are upright cards/cones).
'ours - ours-without-trees' removes blobs that the ground texture produces by itself; the game count has no such baseline, so the
game's figure is an upper bound on 3D trees and the ratio game/ours is reported per distance band (distance from world4_trace at the blob centre).
"""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import png_io
import world4_compare as wc
import world4_trace as w4


def median_filter(a, k=15, sub=3):
    """approximate local median: median of a subsampled (2k+1)^2 window, computed on a coarse grid and upsampled."""
    H, W = a.shape
    s = 8
    ys = np.arange(0, H, s); xs = np.arange(0, W, s)
    p = np.pad(a, k, mode='edge')
    out = np.zeros((len(ys), len(xs)))
    off = np.arange(-k, k + 1, sub)
    for i, y in enumerate(ys):
        rows = p[y + k + off][:, xs[:, None] + k + off[None, :]]   # (n, len(xs), n)
        out[i] = np.median(rows.transpose(1, 0, 2).reshape(len(xs), -1), axis=1)
    return np.repeat(np.repeat(out, s, 0), s, 1)[:H, :W]


def label(mask):
    """4-neighbour connected components -> list of (ys, xs) index arrays (simple union-find over runs)."""
    H, W = mask.shape
    lab = np.zeros((H, W), np.int32)
    parent = [0]

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    nxt = 1
    for y in range(H):
        row = mask[y]
        if not row.any():
            continue
        xs = np.nonzero(row)[0]
        starts = np.r_[0, np.nonzero(np.diff(xs) > 1)[0] + 1]
        ends = np.r_[starts[1:], len(xs)]
        for s_, e_ in zip(starts, ends):
            x0, x1 = xs[s_], xs[e_ - 1]
            above = lab[y - 1, x0:x1 + 1] if y else np.zeros(0, np.int32)
            ids = np.unique(above[above > 0])
            if len(ids):
                r = find(int(ids[0]))
                for o in ids[1:]:
                    ro = find(int(o))
                    if ro != r:
                        parent[ro] = r
                lab[y, x0:x1 + 1] = r
            else:
                parent.append(nxt); lab[y, x0:x1 + 1] = nxt; nxt += 1
    roots = np.array([find(i) for i in range(len(parent))])
    lab = roots[lab]
    return lab


def blobs(img, ok, thr=14.0, amin=6, amax=400):
    lum = img[..., :3].astype(float).mean(-1)
    med = median_filter(lum)
    m = (med - lum > thr) & ok
    lab = label(m)
    ids, inv, cnt = np.unique(lab.ravel(), return_inverse=True, return_counts=True)
    out = []
    ys, xs = np.nonzero(lab)
    L = lab[ys, xs]
    order = np.argsort(L)
    L, ys, xs = L[order], ys[order], xs[order]
    cuts = np.r_[0, np.nonzero(np.diff(L))[0] + 1, len(L)]
    for a, b in zip(cuts[:-1], cuts[1:]):
        n = b - a
        if n < amin or n > amax:
            continue
        y0, y1, x0, x1 = ys[a:b].min(), ys[a:b].max(), xs[a:b].min(), xs[a:b].max()
        if (y1 - y0 + 1) < 0.6 * (x1 - x0 + 1):
            continue
        out.append((float(xs[a:b].mean()), float(y1)))   # x centre, base row
    return out


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('shot'); ap.add_argument('ours'); ap.add_argument('base')
    ap.add_argument('--pose', required=True); ap.add_argument('--mask-rect', action='append', default=[])
    ap.add_argument('--thr', type=float, default=14.0)
    g = ap.parse_args(argv)
    pose = [float(v) for v in g.pose.split(',')]
    rects = [tuple(int(v) for v in r.split(',')) for r in g.mask_rect]
    gc, _ = wc.client(png_io.read_png(g.shot))
    R, G, B = gc[..., 0], gc[..., 1], gc[..., 2]
    hud = ((G > R + 40) & (G > B + 30)) | (B > R + 60) | (gc.mean(-1) < 22)   # HUD colours, near-black cockpit only (forests and trees stay)
    ok = wc.hud_mask(np.repeat(np.where(hud[..., None], 0.0, 128.0), 3, -1), rects)
    o = png_io.read_png(g.ours)[..., :3].astype(float); b = png_io.read_png(g.base)[..., :3].astype(float)
    H, W = min(gc.shape[0], o.shape[0]), min(gc.shape[1], o.shape[1])
    gc, ok, o, b = gc[:H, :W], ok[:H, :W], o[:H, :W], b[:H, :W]
    tr = w4.trace_image('m02a', pose, W // 4, H // 4, wc.TANX, wc.TANY)
    tmap = np.repeat(np.repeat(np.where(tr['hit'], tr['t'], np.inf), 4, 0), 4, 1)
    tmap = np.pad(tmap, ((0, max(0, H - tmap.shape[0])), (0, max(0, W - tmap.shape[1]))), mode='edge')[:H, :W]
    res = {}
    for nm, im in (('game', gc), ('ours', o), ('base', b)):
        bl = blobs(im, ok, g.thr)
        t = np.array([tmap[min(int(y), H - 1), int(x)] for x, y in bl]) if bl else np.zeros(0)
        res[nm] = t
    okfrac = ok.mean()
    print('usable pixels %.2f; blobs: game %d ours %d ours-without-trees %d' % (okfrac, len(res['game']), len(res['ours']), len(res['base'])))
    for lo, hi in [(0, 6000), (6000, 9000), (9000, 12000), (12000, 16000), (16000, 24000)]:
        c = {k: int(((v >= lo) & (v < hi)).sum()) for k, v in res.items()}
        trees = c['ours'] - c['base']
        print('  band %5d-%5d  game %4d  ours %4d  baseline %4d  -> ours trees %4d  ratio game/ours-trees %s' % (
            lo, hi, c['game'], c['ours'], c['base'], trees, ('%.1f' % ((c['game'] - c['base']) / trees)) if trees > 0 else 'n/a'))
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
