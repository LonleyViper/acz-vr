"""Offline search of EE block dumps (tools/ee-block-dump.ps1) for the game's unit records.

Lead (docs/research/ac0vr-lessons.md 1, from ac0vr's static RE of SLUS-21346): unit records of 0x4A0 bytes with the
world position at +0x260 (3 floats), angles at +0x270. D-295 showed absolute PS2 addresses do not map in the PC port,
so this looks for the *shape* anywhere in the block:
  1. the E6 aircraft position (+0xFDD42C) as an exact float triple anywhere else in the same dump (player copies);
  2. chains of >= MIN_CHAIN records at a fixed stride whose +POS triples are plausible world positions, at least one
     of which moves between two dumps 1 s apart at an aircraft-like speed.
Strides tried: 0x4A0 first, then any stride from a shortlist, so a different record size in the port still shows up.
Usage: python -I unit_table_search.py <dump dir> [--stride 0x4A0]
"""
import argparse, glob, os, re, sys
import numpy as np

E6_AIR = 0xFDD42C
WORLD = 300000.0          # |x|, |z| bound (terrain spans +-262144)
YMIN, YMAX = -200.0, 30000.0
MIN_CHAIN = 3


def load(path):
    return np.fromfile(path, dtype='<f4')


def plausible(a):
    """mask over float index i: (a[i], a[i+1], a[i+2]) looks like a world position (not all small)."""
    x, y, z = a[:-2], a[1:-1], a[2:]
    with np.errstate(invalid='ignore'):
        m = np.isfinite(x) & np.isfinite(y) & np.isfinite(z)
        m &= (np.abs(x) < WORLD) & (np.abs(z) < WORLD) & (y > YMIN) & (y < YMAX)
        m &= (np.abs(x) + np.abs(z)) > 500.0
        # reject integer-looking or denormal noise: both x and z must have a fractional part or be large
    return np.concatenate([m, [False, False]])


def chains(mask, stride_f, min_chain):
    """start indices (float index) of runs of >= min_chain with mask true at i, i+s, i+2s ..."""
    n = len(mask)
    rows = -(-n // stride_f)
    M = np.zeros(rows * stride_f, dtype=np.int32); M[:n] = mask
    M = M.reshape(rows, stride_f)
    R = np.zeros_like(M)
    # run length counted backwards: run[i] = mask[i] * (1 + run[i+s]), one row (all columns) at a time
    acc = np.zeros(stride_f, dtype=np.int32)
    for k in range(rows - 1, -1, -1):
        acc = (acc + 1) * M[k]
        R[k] = acc
    run = R.reshape(-1)[:n]
    # chain heads: run >= min_chain and the previous element not part of the run
    heads = np.nonzero(run >= min_chain)[0]
    prev = heads - stride_f
    keep = (prev < 0) | (run[np.clip(prev, 0, None)] == 0)
    return heads[keep], run


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--stride', default='0x4A0')
    ap.add_argument('--pos', default='0x260')
    ap.add_argument('--maxlist', type=int, default=40)
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.dir, 'blk*-t*.bin')))
    blocks = {}
    for f in files:
        m = re.search(r'blk([0-9A-F]+)-t(\d+)\.bin$', f)
        blocks.setdefault(m.group(1), {})[int(m.group(2))] = f
    stride = int(a.stride, 0); pos = int(a.pos, 0)
    for blk, ts in sorted(blocks.items()):
        times = sorted(ts)
        print(f'== block 0x{blk} dumps t={times}')
        A = load(ts[times[0]])
        B = load(ts[times[1]]) if len(times) > 1 else None
        dt = (times[1] - times[0]) if B is not None else 0
        air = A[E6_AIR // 4:E6_AIR // 4 + 3]
        print(f'E6 air at t{times[0]}: {air.tolist()}')
        # 1. copies of the player's position
        if np.all(np.isfinite(air)) and abs(air[0]) + abs(air[2]) > 1:
            hit = np.nonzero((A[:-2] == air[0]) & (A[1:-1] == air[1]) & (A[2:] == air[2]))[0]
            print(f'player position copies: {len(hit)}: ' + ' '.join(f'0x{4*h:X}' for h in hit[:30]))
            near = np.nonzero((np.abs(A[:-2] - air[0]) < 50) & (np.abs(A[1:-1] - air[1]) < 50) & (np.abs(A[2:] - air[2]) < 50))[0]
            print(f'within 50 u: {len(near)}: ' + ' '.join(f'0x{4*h:X}' for h in near[:40]))
        # 2. chains at the stride
        mask = plausible(A)
        heads, run = chains(mask, stride // 4, MIN_CHAIN)
        rows = []
        for h in heads:
            L = int(run[h])
            idx = h + np.arange(L) * (stride // 4)
            P = np.stack([A[idx], A[idx + 1], A[idx + 2]], 1)
            if B is not None:
                Q = np.stack([B[idx], B[idx + 1], B[idx + 2]], 1)
                sp = np.linalg.norm(Q - P, axis=1) / max(dt, 1)
            else:
                sp = np.zeros(L)
            moving = int(np.sum((sp > 20) & (sp < 2000)))
            spread = float(np.ptp(P[:, 0]) + np.ptp(P[:, 2]))
            dist = float(np.min(np.linalg.norm(P - air, axis=1))) if np.all(np.isfinite(air)) else -1
            rows.append((moving, L, spread, 4 * h, dist, P, sp))
        rows.sort(key=lambda r: (-r[0], -r[1]))
        print(f'chains stride 0x{stride:X}: {len(rows)} (>= {MIN_CHAIN}); with movers: {sum(1 for r in rows if r[0])}')
        for moving, L, spread, off, dist, P, sp in rows[:a.maxlist]:
            rec = off - pos
            print(f'  pos@0x{off:X} (record 0x{rec:X} if +0x{pos:X}) n={L} movers={moving} spread={spread:.0f} '
                  f'nearest-to-player={dist:.0f}')
            for k in range(min(L, 6)):
                print(f'     [{k}] ({P[k,0]:.1f}, {P[k,1]:.1f}, {P[k,2]:.1f}) v={sp[k]:.0f} u/s')


if __name__ == '__main__':
    main()
