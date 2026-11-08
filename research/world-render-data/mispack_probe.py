"""WORLD-4: offline probe of the mission pack data/mis/pack/<mission>/ (read-only from a.arc; outputs only to D:/acz-extract/mis-pack).

  python -I mispack_probe.py m02a [--arc <a.arc>] [--out D:/acz-extract/mis-pack]

Extracts <m>.mmb and <m>{d,ld,md,sd,td}.bin, then reports what can be said about their layout: size, leading u32 table, float runs that look
like world coordinates of the stage (|x|,|z| <= 262144, y in the terrain height range), and whether any of those coordinates coincide with
the game window's extra buildings (the right-hand cluster of the step56 shot). Feasibility report only: nothing here feeds the renderer.
"""
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np

DEFAULT_ARC = r'D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero\a.arc'


def extract(mission, arc_path, out):
    import arcread
    a = arcread.Arc(arc_path)
    ver, es = arcread.entries(a)
    a.ver = ver
    by = {e[0]: e for e in es}
    os.makedirs(out, exist_ok=True)
    got = {}
    for suffix in ('.mmb', 'd.bin', 'ld.bin', 'md.bin', 'sd.bin', 'td.bin'):
        name = 'data/mis/pack/%s/%s%s' % (mission, mission, suffix)
        if name not in by:
            continue
        b = arcread.extract_bytes(a, by[name])
        got[suffix] = b
        open(os.path.join(out, mission + suffix), 'wb').write(b)
    return got


def coord_triples(b, lo_y=-500.0, hi_y=21000.0):
    """offsets (4-aligned) of f32 triples (x, y, z) inside the stage box with |x|,|z| > 1000 (not small model-space numbers)."""
    n = len(b) // 4
    f = np.frombuffer(b[:n * 4], '<f4')
    ok = np.isfinite(f)
    x, y, z = f[:-2], f[1:-1], f[2:]
    m = ok[:-2] & ok[1:-1] & ok[2:] & (np.abs(x) <= 262144) & (np.abs(z) <= 262144) & (np.abs(x) > 1000) & (np.abs(z) > 1000) & (y >= lo_y) & (y <= hi_y)
    idx = np.nonzero(m)[0]
    return idx, f


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('mission'); ap.add_argument('--arc', default=DEFAULT_ARC); ap.add_argument('--out', default='D:/acz-extract/mis-pack')
    g = ap.parse_args(argv)
    got = extract(g.mission, g.arc, os.path.join(g.out, g.mission))
    for k, b in got.items():
        head = struct.unpack_from('<%dI' % min(8, len(b) // 4), b)
        print('%s%s: %d bytes, head u32 %s' % (g.mission, k, len(b), ' '.join('%08x' % v for v in head)))
        if len(b) < 64:
            continue
        idx, f = coord_triples(b)
        # stride between consecutive hits: a fixed record size shows up as the most common delta
        if len(idx) > 2:
            dd = np.diff(idx)
            vals, cnt = np.unique(dd[dd > 2], return_counts=True)
            top = sorted(zip(cnt, vals), reverse=True)[:5]
            print('   world-coordinate triples: %d; most common spacing (floats): %s' % (len(idx), ', '.join('%d x%d' % (v, c) for c, v in top)))
            for i in idx[:12]:
                print('     @+0x%06x  (%.1f, %.1f, %.1f)' % (4 * i, f[i], f[i + 1], f[i + 2]))
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
