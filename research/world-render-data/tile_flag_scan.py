"""Tile flag-bit scan (offline, numpy only): which dihedral transform (identity, flip X, flip Y, transpose, rotations) of a tile
makes its edges continuous with its neighbours, per .mti flag combination.

  python -I tile_flag_scan.py <stage> [stage ...]

Metric: for every 4-neighbour pair of the 256x256 tile grid the mean absolute RGB difference between the touching pixel rows/columns
(8-bit CLUT colours, 64 px edges). Per flag value f: for pairs where tile A has flag f and its neighbour B has flag 0 (and the reverse, and
f,f pairs) apply each of the 8 dihedral variants to the f tiles only and report the mean edge difference. The identity score of the all-zero-flag
pairs is the "natural seam" level; a shuffled-neighbour control gives the random level. The best variant is credible only if it is clearly below
the identity score AND approaches the natural level.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from terrain_lib import Stage

# 8 dihedral variants of a (64,64) array t[row, col]
VARIANTS = [
    ('id', lambda t: t),
    ('flipX', lambda t: t[:, ::-1]),
    ('flipY', lambda t: t[::-1, :]),
    ('rot180', lambda t: t[::-1, ::-1]),
    ('transpose', lambda t: t.T),
    ('rot90cw', lambda t: t.T[:, ::-1]),   # (r,c) <- (63-c, r)... one of the two quarter turns
    ('rot90ccw', lambda t: t.T[::-1, :]),
    ('antitranspose', lambda t: t.T[::-1, ::-1]),
]


def edge_tables(st):
    """edges[v][id] -> (left col, right col, top row, bottom row) each (64,3) int16 for variant v."""
    n = st.n
    rgb = st.clut.astype(np.int16)
    out = []
    for name, fn in VARIANTS:
        L = np.zeros((n, 64, 3), np.int16); R = L.copy(); T = L.copy(); B = L.copy()
        for i in range(n):
            t = np.ascontiguousarray(fn(st.tiles[0][i]))
            c = rgb[t]
            L[i] = c[:, 0]; R[i] = c[:, 63]; T[i] = c[0, :]; B[i] = c[63, :]
        out.append((L, R, T, B))
    return out


def pair_scores(ed, ids, flags, va, vb, mask_a=None, mask_b=None):
    """Mean edge difference over all 4-neighbour pairs, tile A with variant function va(flag), B with vb(flag).
    va/vb: arrays indexed by flag value (0..15) giving a variant index. mask_*: boolean 256x256 of tiles allowed for A/B (pair counted if both ok)."""
    tot = 0.0; cnt = 0
    for axis in (0, 1):   # 0: vertical neighbours (A above B), 1: horizontal (A left of B)
        if axis == 1:
            ia, ib = ids[:, :-1], ids[:, 1:]; fa, fb = flags[:, :-1], flags[:, 1:]
            ma = None if mask_a is None else mask_a[:, :-1]; mb = None if mask_b is None else mask_b[:, 1:]
            ea, eb = 1, 0   # A's right column vs B's left column
        else:
            ia, ib = ids[:-1], ids[1:]; fa, fb = flags[:-1], flags[1:]
            ma = None if mask_a is None else mask_a[:-1]; mb = None if mask_b is None else mask_b[1:]
            ea, eb = 3, 2   # A's bottom row vs B's top row
        sel = np.ones(ia.shape, bool)
        if ma is not None: sel &= ma
        if mb is not None: sel &= mb
        if not sel.any():
            continue
        A_id, B_id, A_f, B_f = ia[sel], ib[sel], fa[sel], fb[sel]
        va_ = va[A_f]; vb_ = vb[B_f]
        d = 0.0
        for v in range(8):
            for w in range(8):
                m = (va_ == v) & (vb_ == w)
                if not m.any():
                    continue
                a = ed[v][ea][A_id[m]]; b = ed[w][eb][B_id[m]]
                d += float(np.abs(a - b).mean(axis=(1, 2)).sum())
        tot += d; cnt += int(sel.sum())
    return (tot / cnt if cnt else float('nan')), cnt


def scan(stage):
    st = Stage(stage)
    w = st.tile_words()
    ids = (w & 0x3ff).astype(np.int64); flags = (w >> 10).astype(np.int64)
    ed = edge_tables(st)
    ident = np.zeros(16, np.int64)
    print('=== %s: %d tiles, flag values %s' % (stage, st.n, sorted(set(flags.ravel().tolist()))))
    zero = flags == 0
    base, n0 = pair_scores(ed, ids, flags, ident, ident, zero, zero)
    allp, nall = pair_scores(ed, ids, flags, ident, ident)
    rng = np.random.default_rng(1)
    perm = rng.permutation(ids.size).reshape(ids.shape)
    shuf, _ = pair_scores(ed, ids.ravel()[perm], flags.ravel()[perm], ident, ident)
    print('  natural seam (flag0-flag0 pairs, identity): %.2f over %d pairs ; all pairs identity: %.2f ; shuffled control: %.2f' % (base, n0, allp, shuf))
    res = {}
    for f in sorted(set(flags.ravel().tolist())):
        if f == 0:
            continue
        isf = flags == f
        row = []
        for v in range(8):
            var = ident.copy(); var[f] = v
            s_ff, n_ff = pair_scores(ed, ids, flags, var, var, isf, isf)
            s_f0, n_f0 = pair_scores(ed, ids, flags, var, ident, isf, zero)
            s_0f, n_0f = pair_scores(ed, ids, flags, ident, var, zero, isf)
            nn = n_ff + n_f0 + n_0f
            comb = (s_ff * n_ff + s_f0 * n_f0 + s_0f * n_0f) / nn if nn else float('nan')
            row.append((VARIANTS[v][0], comb, s_ff, s_f0 if n_f0 else float('nan'), n_ff, n_f0 + n_0f))
        res[f] = row
        npairs = row[0][4] + row[0][5]
        print('  flag %2d (bits %s): %d tiles, %d pairs' % (f, bin(f), int(isf.sum()), npairs))
        for name, comb, sff, sf0, nff, nf0 in row:
            print('     %-14s combined %.2f   f-f %.2f (n=%d)  f-0 %.2f (n=%d)' % (name, comb, sff, nff, sf0, nf0))
        best = min(row, key=lambda r: r[1] if r[1] == r[1] else 1e9)
        print('     -> best %s (%.2f) vs identity %.2f; natural %.2f' % (best[0], best[1], row[0][1], base))
    return res


if __name__ == '__main__':
    for s in sys.argv[1:] or ['m02a']:
        scan(s)
