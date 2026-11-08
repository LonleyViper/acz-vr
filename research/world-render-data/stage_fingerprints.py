"""Stage fingerprint table for runtime stage detection (xr_terrain_stage=auto; docs/world-render/stages-and-detection.md section 3).
Offline, numpy + stdlib only, NO network, read-only on the archive.

  python -I stage_fingerprints.py [--arc <a.arc>] [--results D:\\acz-extract\\stage-all\\bake-results.json] [--out <dir>\\fingerprints.json]
  python -I stage_fingerprints.py --selftest

A fingerprint is a set of 32-byte windows ("signatures") cut from the stage's RAW game files as the game loads them (.mhd height chunk rows,
.mti tile words, .mtc palette, .mtd tile texel rows, mapset<id>.bin, sph<id>.sph).  Each signature lists the identity groups whose files
contain it (owners).  At runtime the proxy scans the game's readable memory read-only for these windows (C++ acz::stagefp::Matcher);
the identity group with the most distinct signature hits wins.  Identity groups = stages whose baked terrain + env + objects are
byte-identical (bake-results.json terrain_sha/env_sha/objects_sha): e.g. m03k/m03m/m03n render identically, so they are one group.

Selection per stage and source: high-variety windows (>= MIN_DISTINCT distinct byte values, no 8-byte prefix that is all one value), the
ones owned by the fewest groups first; at most PER_SOURCE per source.  Rows are cut so a window never crosses a texture row (64-byte .mtd
rows) or a height-chunk row (33-byte .mhd rows): the windows also survive row-padded copies.

fingerprints.json (version 1):
  {"format": "acz-stage-fingerprints", "version": 1, "window": 32, "min_hits": 3,
   "groups": [{"stage": "<representative id>", "members": [...], "zvrs_sha256": "<payload sha of the representative>"}, ...],
   "sigs": [{"src": "mhd", "stage": "<stage cut from>", "off": <byte offset in that file>, "groups": [<group indices>], "hex": "<64 hex chars>"}, ...]}
"""
import hashlib, json, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
DEFAULT_ARC = r'D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero\a.arc'
WINDOW = 32
MIN_HITS = 3
PER_SOURCE = 4
CANDIDATES = 48
MIN_DISTINCT = {'mhd': 6, 'mti': 8, 'mtc': 10, 'mtd': 10, 'mapset': 8, 'sph': 8}
SOURCES = ('mhd', 'mti', 'mtc', 'mtd', 'mapset', 'sph')


def source_name(stage, src):
    if src == 'mapset':
        return 'data/mapset/map/mapset%s.bin' % stage
    if src == 'sph':
        return 'data/spheredata/map/sph%s.sph' % stage
    return 'data/map/%s/%s.%s' % (stage, stage, src)


def candidate_offsets(src, blob):
    """Offsets where a WINDOW-byte window starts, never crossing a natural row."""
    n = len(blob)
    if src == 'mhd':                                     # 33x33 chunks, 33-byte rows
        return [c * 1089 + r * 33 for c in range(n // 1089) for r in range(33)]
    if src == 'mtd':                                     # 0x20 header, 64x64 tiles, 64-byte rows
        nt = struct.unpack_from('<I', blob, 0)[0] if n >= 4 else 0
        return [0x20 + t * 4096 + r * 64 + h for t in range(nt) for r in range(0, 64, 4) for h in (0, 32) if 0x20 + t * 4096 + r * 64 + h + WINDOW <= n]
    step = 16 if src in ('mapset', 'sph') else 32
    return list(range(0, n - WINDOW + 1, step))


def good(src, w):
    if len(w) != WINDOW or len(set(w)) < MIN_DISTINCT[src]:
        return False
    if len(set(w[:8])) == 1:
        return False
    return True


def build(files, groups, rep_sha=None):
    """files: {stage: {src: bytes}}; groups: list of member lists (identity groups).  -> fingerprint dict."""
    gidx = {s: i for i, g in enumerate(groups) for s in g}
    out_sigs, seen = [], set()
    for src in SOURCES:
        # identical source files across stages are searched once
        uniq = {}
        for s in sorted(files):
            b = files[s].get(src, b'')
            if b:
                uniq.setdefault(hashlib.sha256(b).digest(), (b, set()))[1].add(gidx[s])
        blobs = list(uniq.values())
        for gi, g in enumerate(groups):
            s = g[0]
            b = files[s].get(src, b'')
            if not b:
                continue
            offs = [o for o in candidate_offsets(src, b) if good(src, b[o:o + WINDOW])]
            if not offs:
                continue
            if len(offs) > CANDIDATES:                    # evenly spread subsample (deterministic)
                k = len(offs) / CANDIDATES
                offs = [offs[int(i * k)] for i in range(CANDIDATES)]
            scored = []
            for o in offs:
                w = b[o:o + WINDOW]
                owners = set()
                for bb, gs in blobs:
                    if bb.find(w) >= 0:
                        owners |= gs
                if gi not in owners:                       # cannot happen (w is cut from b), kept as a guard
                    continue
                scored.append((len(owners), o, w, owners))
            scored.sort(key=lambda t: (t[0], t[1]))
            taken = 0
            for n_owners, o, w, owners in scored:
                if taken >= PER_SOURCE:
                    break
                if w in seen:
                    continue
                seen.add(w)
                out_sigs.append(dict(src=src, stage=s, off=o, groups=sorted(owners), hex=w.hex()))
                taken += 1
    return dict(format='acz-stage-fingerprints', version=1, window=WINDOW, min_hits=MIN_HITS,
                groups=[dict(stage=g[0], members=g, zvrs_sha256=(rep_sha or {}).get(g[0], '')) for g in groups], sigs=out_sigs)


def identity_groups(results):
    key = {}
    for r in results:
        if not r.get('ok'):
            continue
        k = (r.get('terrain_sha'), r.get('env_sha'), r.get('objects_sha'))
        key.setdefault(k, []).append(r['stage'])
    return sorted((sorted(v) for v in key.values()), key=lambda g: g[0])


def coverage(fp):
    """Per group: (distinct signatures owned, signatures owned by this group only)."""
    n = len(fp['groups'])
    tot = [0] * n; own = [0] * n
    for s in fp['sigs']:
        for g in s['groups']:
            tot[g] += 1
            if len(s['groups']) == 1:
                own[g] += 1
    return tot, own


def selftest():
    import random
    rnd = random.Random(5)
    def blob(n): return bytes(rnd.randrange(256) for _ in range(n))
    shared_h = blob(1089 * 4)
    files = {
        'aa1': dict(mhd=shared_h, mtc=blob(1024), mapset=blob(864)),
        'aa2': None,                                       # identical to aa1 (identity group)
        'bb1': dict(mhd=shared_h, mtc=blob(1024), mapset=blob(864)),   # shares the heights only
        'cc1': dict(mhd=blob(1089 * 4), mtc=blob(1024), mapset=bytes(864)),   # flat mapset: no signatures from it
    }
    files['aa2'] = dict(files['aa1'])
    groups = [['aa1', 'aa2'], ['bb1'], ['cc1']]
    fp = build(files, groups)
    assert fp['window'] == WINDOW and len(fp['groups']) == 3
    tot, own = coverage(fp)
    assert own[0] >= 4 and own[1] >= 4 and own[2] >= 4, (tot, own)
    fam = [s for s in fp['sigs'] if s['src'] == 'mhd' and s['groups'] == [0, 1]]
    assert fam, 'shared heights must give family signatures owned by both groups'
    assert not any(s['src'] == 'mapset' and 2 in s['groups'] for s in fp['sigs']), 'zero mapset must give no signature'
    for s in fp['sigs']:
        w = bytes.fromhex(s['hex'])
        assert len(w) == WINDOW and files[s['stage']][s['src']][s['off']:s['off'] + WINDOW] == w
    assert build(files, groups) == fp, 'not deterministic'
    print('selftest ok', len(fp['sigs']), 'signatures', tot, own)
    return fp


def main(argv):
    if '--selftest' in argv:
        selftest(); return 0
    opt, i = {}, 0
    while i < len(argv):
        if argv[i].startswith('--') and i + 1 < len(argv):
            opt[argv[i][2:]] = argv[i + 1]; i += 2
        else:
            i += 1
    if 'fixture' in opt:                                   # the synthetic selftest table (C++ test fixture)
        fp = selftest()
        with open(opt['fixture'], 'w', newline='\n') as f:
            json.dump(fp, f, indent=0)
        print('wrote', opt['fixture']); return 0
    res_path = opt.get('results', r'D:\acz-extract\stage-all\bake-results.json')
    results = json.load(open(res_path))
    groups = identity_groups(results)
    rep_sha = {r['stage']: r.get('payload_sha256', '') for r in results}
    import arcread
    a = arcread.Arc(opt.get('arc', DEFAULT_ARC))
    ver, es = arcread.entries(a)
    a.ver = ver
    by = {e[0]: e for e in es}
    files = {}
    for g in groups:
        for s in g:
            files[s] = {src: arcread.extract_bytes(a, by[source_name(s, src)]) for src in SOURCES if source_name(s, src) in by}
    fp = build(files, groups, rep_sha)
    out = opt.get('out', os.path.join(os.path.dirname(res_path), 'fingerprints.json'))
    with open(out + '.tmp', 'w', newline='\n') as f:
        json.dump(fp, f, indent=0)
    os.replace(out + '.tmp', out)
    tot, own = coverage(fp)
    for gi, g in enumerate(fp['groups']):
        print('%-28s sigs=%3d unique=%3d' % (','.join(g['members']), tot[gi], own[gi]))
    print('wrote', out, len(fp['sigs']), 'signatures,', len(fp['groups']), 'groups')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
