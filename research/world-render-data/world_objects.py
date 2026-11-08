"""ACZ stage static objects: prop placements (.pdl/.pmi/.pml + pmmdl models) and tree scatter (.wad/.wpd/.wbd).

Offline, read-only, numpy + stdlib.  Evidence and the verification statistics: docs/world-render/world3-impl.md.

Layouts (little endian) as decoded in WORLD-3:
  .pdl   u32 count, 12 pad bytes, count x {f32 x, f32 y, f32 z, u32 model}   (a 4-byte file = no placements)
         model = index into pmmdl<stage>.bin (max id = n-1, every model used: m02a 44/44, m07a 415/415).
         NO rotation/scale field: each placement has its OWN pmmdl model whose vertices are already rotated
         (house variants are the same mesh yawed by 0/86.75/88.5/164/176 deg) -> world = (x,y,z) + model vertex.
  .pmi   256x256 u16 grid, cell 2048 units, [row = z, col = x]; value = record number in .pml (0 = none)
  .pml   16-byte records of up to 8 u16 pdl indices (0xFFFF = unused); record 0 is empty
         -> together a spatial grid: cell -> objects in/near that cell (lookup/culling; not needed for drawing).
  .wad   per texture block (the .mta block number, like .mti) 288 bytes:
             u32 off[3][16]   (0xFFFFFFFF = empty)   index into .wpd (u16 units)
             u16 cnt[3][16]
         [t][k]: tree type t (-> <stage>_tree{t+1}.acm/.gim), sub-square k of the 32768-unit block,
         kx = k % 4, kz = k // 4 (8192 units each).
  .wpd   u16 positions, sorted per list: x = v & 255, z = v >> 8, 32-unit steps (= one terrain texel) inside the
         sub-square.  world x = -262144 + C*32768 + kx*8192 + x*32 + 16 (texel centre: inference), same for z with R.
  .wbd   per block 1024 bytes = 64x64 x 2-bit values (512-unit cells); a coarse tree/density mask (not needed to place
         trees; reported only).
"""
import os, struct, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

HALF = 262144.0
BLOCK = 32768.0          # one .mha/.mta chunk = 32 cells of 1024 units
SUB = 8192.0             # tree sub-square (4x4 per block)
TEXEL = 32.0             # tree position step (one terrain texel: 2048-unit tile / 64 px)
PMI_CELL = 2048.0
WAD_BLOCK = 288

PDL_DT = np.dtype([('x', '<f4'), ('y', '<f4'), ('z', '<f4'), ('model', '<u4')])


class FormatError(ValueError):
    pass


def read_pdl(b):
    """-> structured array (x, y, z, model).  Raises FormatError on a truncated/oversized file."""
    if len(b) < 4:
        raise FormatError('pdl too short')
    n = struct.unpack_from('<I', b)[0]
    if n == 0:
        return np.zeros(0, PDL_DT)
    if n > 1_000_000 or len(b) != 16 + 16 * n:
        raise FormatError('pdl size %d does not match count %d' % (len(b), n))
    a = np.frombuffer(b, PDL_DT, n, 16).copy()
    if not np.all(np.isfinite(a['x']) & np.isfinite(a['y']) & np.isfinite(a['z'])):
        raise FormatError('pdl has non-finite positions')
    return a


def read_pmi(b):
    if len(b) == 4:
        return None
    if len(b) != 256 * 256 * 2:
        raise FormatError('pmi size %d' % len(b))
    return np.frombuffer(b, '<u2').reshape(256, 256)


def read_pml(b):
    """-> list of records; record r = list of pdl indices (up to 8 u16, 0xFFFF = unused slot).
    Record 0 is the empty record; .pmi cells hold record numbers."""
    if len(b) < 16:
        return []
    if len(b) % 16:
        raise FormatError('pml size %d' % len(b))
    a = np.frombuffer(b, '<u2').reshape(-1, 8)
    return [[int(v) for v in row if v != 0xFFFF] for row in a]


def read_trees(wad, wpd, mta):
    """-> (N,3) float64 array (world x, world z, type 0..2) for the whole 16x16-block stage.
    mta: 16x16 block numbers (as for .mti)."""
    if len(wad) % WAD_BLOCK:
        raise FormatError('wad size %d not a multiple of %d' % (len(wad), WAD_BLOCK))
    nb = len(wad) // WAD_BLOCK
    pos = np.frombuffer(wpd[:len(wpd) // 2 * 2], '<u2')
    per_block = []
    for b in range(nb):
        off = np.frombuffer(wad, '<u4', 48, b * WAD_BLOCK)
        cnt = np.frombuffer(wad, '<u2', 48, b * WAD_BLOCK + 192)
        lists = []
        for t in range(3):
            for k in range(16):
                o, n = int(off[t * 16 + k]), int(cnt[t * 16 + k])
                if o == 0xFFFFFFFF or n == 0:
                    continue
                if o + n > len(pos):
                    raise FormatError('wad list (%d,%d) outside wpd (%d)' % (o, n, len(pos)))
                v = pos[o:o + n].astype(np.int64)
                lx = (k % 4) * SUB + (v & 255) * TEXEL + TEXEL / 2
                lz = (k // 4) * SUB + (v >> 8) * TEXEL + TEXEL / 2
                lists.append(np.stack([lx, lz, np.full(n, t, np.float64)], 1))
        per_block.append(np.concatenate(lists) if lists else np.zeros((0, 3)))
    out = []
    mta = np.asarray(mta).reshape(16, 16)
    for R in range(16):
        for C in range(16):
            b = int(mta[R, C])
            if b >= nb or not len(per_block[b]):
                continue
            p = per_block[b].copy()
            p[:, 0] += -HALF + C * BLOCK
            p[:, 1] += -HALF + R * BLOCK
            out.append(p)
    return np.concatenate(out) if out else np.zeros((0, 3))


def read_wbd(wbd, nb):
    """-> (nb, 64, 64) 2-bit values (bit order: low bits first; layout inferred)."""
    a = np.frombuffer(wbd[:nb * 1024], np.uint8).reshape(nb, 64, 16)
    v = np.stack([(a >> s) & 3 for s in (0, 2, 4, 6)], -1)
    return v.reshape(nb, 64, 64)


# ------------------------------------------------------------------------------------------- models
def model_points(acm):
    """All mesh vertices of an ACM model in model space (node transforms applied)."""
    from acm_lib import decode_acm
    m = decode_acm(acm)
    P = []
    for k, nd in enumerate(m['nodes']):
        if 0 <= nd['mesh'] < len(m['meshes']) and len(m['meshes'][nd['mesh']]['pos']):
            W = m['world'][k]
            P.append(m['meshes'][nd['mesh']]['pos'] @ W[:3, :3].T + W[:3, 3])
    return np.concatenate(P) if P else np.zeros((0, 3))


def yaw_fit(A, B, step=0.25):
    """Best yaw (deg, about +Y, x' = c x + s z, z' = -s x + c z) and mirror that maps point set A onto B
    (correspondence-free: symmetric max nearest-neighbour distance).  -> (err, yaw_deg, mirror)."""
    best = (np.inf, 0.0, 1)
    for mir in (1, -1):
        x = A[:, 0] * mir
        for deg in np.arange(0, 360, step):
            t = np.radians(deg); c, s = np.cos(t), np.sin(t)
            R = np.stack([c * x + s * A[:, 2], A[:, 1], -s * x + c * A[:, 2]], 1)
            d = np.sqrt(((R[:, None, :] - B[None, :, :]) ** 2).sum(-1))
            err = max(d.min(1).max(), d.min(0).max())
            if err < best[0]:
                best = (err, float(deg), mir)
    return best


# ------------------------------------------------------------------------------------------- stage io
def load_stage_objects(stage, root):
    d = os.path.join(root, stage)
    rd = lambda n: open(os.path.join(d, n), 'rb').read()
    out = dict(stage=stage, pdl=read_pdl(rd(stage + '.pdl')), pmi=read_pmi(rd(stage + '.pmi')),
               pml=read_pml(rd(stage + '.pml')))
    mta = np.frombuffer(rd(stage + '.mta'), np.uint8).reshape(16, 16)
    out['trees'] = read_trees(rd(stage + '.wad'), rd(stage + '.wpd'), mta)
    out['mta'] = mta
    return out


def main(argv):
    import argparse
    from terrain_lib import Stage
    from model_baker import table
    ap = argparse.ArgumentParser(description='decode + verification statistics')
    ap.add_argument('stage'); ap.add_argument('--root', default='D:/acz-extract/a/data/map')
    g = ap.parse_args(argv)
    st = g.stage
    o = load_stage_objects(st, g.root)
    S = Stage(st, g.root)
    pdl, trees = o['pdl'], o['trees']
    print('%s: placements %d, trees %d (types %s)' % (st, len(pdl), len(trees),
          np.bincount(trees[:, 2].astype(int), minlength=3).tolist() if len(trees) else []))
    if len(pdl):
        ents = table(open(os.path.join(g.root, st, 'pmmdl%s.bin' % st), 'rb').read())
        ids = pdl['model'].astype(int)
        print('  model ids max %d, models %d, distinct ids %d' % (ids.max(), len(ents), len(set(ids))))
        # pmi consistency: the cell under each placement holds index+1 (or the cell is shared)
        if o['pmi'] is not None:
            c = ((pdl['x'] + HALF) // PMI_CELL).astype(int); r = ((pdl['z'] + HALF) // PMI_CELL).astype(int)
            hit = o['pmi'][r.clip(0, 255), c.clip(0, 255)].astype(int)
            pml = o['pml']
            ok = sum(1 for i, v in enumerate(hit) if 0 < v < len(pml) and i in pml[v])
            used = sorted({i for rec in pml for i in rec})
            print('  pml: %d records (max %d per record), every placement listed: %s' % (
                len(pml), max(len(r) for r in pml), used == list(range(len(pdl)))))
            print('  placement i in pml[pmi[row=z,col=x]] at its own anchor cell: %d / %d (non-zero cells %d, max record %d)' % (
                ok, len(pdl), int((o['pmi'] > 0).sum()), int(o['pmi'].max())))
        pts = [model_points(e) for e in ents]
        # vertical: model bottom + record y vs terrain under the anchor
        ymin = np.array([p[:, 1].min() if len(p) else 0 for p in pts])[ids]
        base = pdl['y'] + ymin
        h = S.height_at(pdl['x'], pdl['z'])
        dv = base - h
        print('  model base - terrain: median %.1f, p10 %.1f, p90 %.1f, corr %.4f' % (
            np.median(dv), np.percentile(dv, 10), np.percentile(dv, 90), np.corrcoef(base, h)[0, 1]))
        # baked yaw: equal-shape models related by a pure yaw
        groups = {}
        for i, p in enumerate(pts):
            if not len(p):
                continue
            u = np.unique(p.round(2), axis=0)
            key = (len(u), round(float(np.ptp(u[:, 1])), 0), round(float(np.linalg.norm(u - u.mean(0), axis=1).max()), 0))
            groups.setdefault(key, []).append((i, u))
        print('  equal-shape models related by a pure yaw (symmetric NN error < 1 unit):')
        for key, mem in groups.items():
            if len(mem) < 2:
                continue
            i0, A = mem[0]
            fits = [(i, f) for i, B in mem[1:] for f in [yaw_fit(A, B)] if f[0] < 1.0]
            if not fits:
                continue
            print('  shape %s: model %d ->' % (key[:1], i0), ', '.join('m%d yaw %.2f%s err %.2f' % (
                i, f[1], '' if f[2] == 1 else ' MIRROR', f[0]) for i, f in fits))
    if len(trees):
        words = S.tile_words()
        lum = S.clut.astype(float) @ [0.3, 0.59, 0.11]

        def texel(x, z):
            tx = ((x + HALF) // TEXEL).astype(int).clip(0, 16383); tz = ((z + HALF) // TEXEL).astype(int).clip(0, 16383)
            w = words[tz // 64, tx // 64]
            return S.tiles[0][w & 0x3ff, tz % 64, tx % 64], w
        rng = np.random.default_rng(1)
        rx, rz = rng.uniform(-HALF, HALF, 100000), rng.uniform(-HALF, HALF, 100000)
        print('  texel luminance: random %.1f' % lum[texel(rx, rz)[0]].mean())
        for t in range(3):
            m = trees[:, 2] == t
            if m.any():
                ti, w = texel(trees[m, 0], trees[m, 1])
                print('  type %d: n %d, luminance under trees %.1f, in water-flag (bit 12) tiles %.3f, on palette-40 '
                      '(m02a river key) texels %.4f' % (t, m.sum(), lum[ti].mean(), ((w & 0x1000) > 0).mean(), (ti == 40).mean()))
        # swapped-axis hypothesis for comparison (x<->z inside each list)
        sx = trees.copy()
        bx = (sx[:, 0] + HALF) // SUB * SUB - HALF; bz = (sx[:, 1] + HALF) // SUB * SUB - HALF
        sx[:, 0], sx[:, 1] = bx + (trees[:, 1] - bz), bz + (trees[:, 0] - bx)
        print('  alternative (x/z swapped inside the sub-square): luminance %.1f' % lum[texel(sx[:, 0], sx[:, 1])[0]].mean())
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
