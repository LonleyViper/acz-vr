"""COCKPIT-5 offline check: a baked <type>_vrcockpit.zvrm against its source .mpl (part counts, triangles, alpha survival).

  python -I check_cockpit_parts.py <file.mpl> <baked.zvrm> [--old <older baked.zvrm>] [--cut 0.5]

Reads the source ACM parts with its own VIF scan (independent of acm_lib.decode_mesh) and compares per source part (2 = cockpit, 0 = exterior):
nodes with geometry in the source vs baked parts with triangles, strip triangles in the source vs baked triangles (degenerate strip triangles are
dropped by the baker; more than 3% dropped is a failure), and, for the cockpit part, how many triangles of the parts the runtime draws survive the
alpha test (texture alpha < cut discards, only on MF_ALPHA_TEST parts, sampled at 7 barycentric points per triangle). Exit 1 when a lit non-blend
cockpit node still carries MF_ALPHA_TEST or when fewer than 90% of the drawn cockpit triangles survive. Offline, numpy + stdlib only.
"""
import os, struct, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from acm_lib import mpl_parts, acm_header, acm_nodes, _mesh_ranges
import model_baker as mb

BARY = [(1 / 3, 1 / 3, 1 / 3), (.8, .1, .1), (.1, .8, .1), (.1, .1, .8), (.5, .5, 0), (0, .5, .5), (.5, 0, .5)]


def source_counts(acm):
    """-> (nodes with a non-empty mesh, strip triangles) by scanning the VIF stream of every mesh (n-2 per strip batch)."""
    nodes = acm_nodes(acm); ranges = _mesh_ranges(acm)
    tris = []
    for (s, e) in ranges:
        i = s; t = 0; cur = {}
        while i < e:
            w = struct.unpack_from('<I', acm, i)[0]; cmd = (w >> 24) & 0x7f; num = (w >> 16) & 0xff; imm = w & 0xffff; i += 4
            if cmd == 0x64 and (imm & 0x3ff) == 0:
                cur = dict(n=struct.unpack_from('<I', acm, i)[0]); i += 8
            elif cmd == 0x68: i += num * 12
            elif cmd == 0x69: i += (num * 6 + 3) & ~3
            elif cmd == 0x6e: i += num * 4
            elif cmd == 0x64: i += num * 8
            elif cmd == 0x17:
                t += max(cur.get('n', 0) - 2, 0); cur = {}
        tris.append(t)
    with_geo = sum(1 for nd in nodes if 0 <= nd['mesh'] < len(tris) and tris[nd['mesh']] > 0)
    stri = sum(tris[nd['mesh']] for nd in nodes if 0 <= nd['mesh'] < len(tris))
    return with_geo, stri


def alpha_survival(z, cut):
    """-> (drawn cockpit triangles, survivors, per-part list of (index, tris, discarded)) for the parts the runtime draws (not MF_HIDDEN)."""
    tx = z['textures'][0]; h, w = tx.shape[:2]
    tot = ok = 0; rows = []
    for i, p in enumerate(z['parts']):
        if not p['ic'] or p['src'] != 2 or p['mflags'] & mb.MF_HIDDEN:
            continue
        idx = z['I'][p['fi']:p['fi'] + p['ic']].reshape(-1, 3); n = len(idx)
        if p['mflags'] & mb.MF_ALPHA_TEST:
            uv = z['V']['uv'][idx]; fr = []
            for b in BARY:
                q = (uv * np.array(b)[None, :, None]).sum(1)
                fr.append(tx[np.clip(((q[:, 1] % 1) * h).astype(int), 0, h - 1), np.clip(((q[:, 0] % 1) * w).astype(int), 0, w - 1), 3] < cut * 255)
            d = int((np.mean(fr, 0) >= 0.5).sum())
        else:
            d = 0
        tot += n; ok += n - d; rows.append((i, n, d))
    return tot, ok, rows


def report(tag, z, cut):
    tot, ok, rows = alpha_survival(z, cut)
    lost = [r for r in rows if r[2] > 0.5 * r[1]]
    print('%s: parts %d (with triangles %d), triangles %d, cockpit triangles drawn %d, surviving the alpha test %d (%.1f%%), parts mostly discarded %d'
          % (tag, len(z['parts']), sum(1 for p in z['parts'] if p['ic']), len(z['I']) // 3, tot, ok, 100.0 * ok / max(tot, 1), len(lost)))
    return tot, ok


def main(argv):
    if len(argv) < 3:
        print(__doc__); return 2
    mpl, baked = argv[1], argv[2]
    old = argv[argv.index('--old') + 1] if '--old' in argv else None
    cut = float(argv[argv.index('--cut') + 1]) if '--cut' in argv else 0.5
    P = mpl_parts(open(mpl, 'rb').read()); z = mb.read_zvrm(baked); bad = []
    for pi in (2, 0):
        geo, stri = source_counts(P[pi])
        mine = [p for p in z['parts'] if p['src'] == pi]
        btri = sum(p['ic'] for p in mine) // 3; bgeo = sum(1 for p in mine if p['ic'])
        drop = stri - btri
        print('mpl part %d: source nodes with geometry %d, baked parts with triangles %d | source strip triangles %d, baked %d (degenerate dropped %d)'
              % (pi, geo, bgeo, stri, btri, drop))
        if bgeo != geo or drop < 0 or drop > 0.03 * max(stri, 1):
            bad.append('part %d count mismatch' % pi)
    # lit non-blend cockpit nodes must not alpha test
    for i, p in enumerate(z['parts']):
        g = p['gflags']
        if p['src'] == 2 and p['ic'] and (g & 8) and not g & 0x2001 and p['mflags'] & mb.MF_ALPHA_TEST:
            bad.append('part %d (flags %#x) lit non-blend but alpha tested' % (i, g))
    if old:
        report('old ' + os.path.basename(old), mb.read_zvrm(old), cut)
    tot, ok = report('new ' + os.path.basename(baked), z, cut)
    if tot and ok < 0.9 * tot:
        bad.append('only %.0f%% of the drawn cockpit triangles survive the alpha test' % (100.0 * ok / tot))
    print('FAIL: ' + '; '.join(bad) if bad else 'OK')
    return 1 if bad else 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
