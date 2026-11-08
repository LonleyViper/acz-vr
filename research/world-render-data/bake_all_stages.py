"""Bake every stage (.zvrs v3 with env + .zvro) one stage per child process and write a summary table (offline, NO network, read-only on the
game archive; game data is never written to the repository).

  python -I bake_all_stages.py [--arc <a.arc>] [--out D:\\acz-extract\\stage-all] [--table docs/world-render/stage-table.md] [--only s1,s2]
  python -I bake_all_stages.py --one <stage> --arc <a.arc> --out <dir>      (child mode: bake one stage, print one JSON line)

Each child opens the archive, extracts the stage files, runs stage_baker.assemble (env on) and objects_baker.assemble, verifies both
outputs with the same checks as the C++ loaders and reports counts + warnings.  The parent never holds more than one stage in memory.
"""
import json, os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
DEFAULT_ARC = r'D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero\a.arc'


def stages_in(arc_path):
    import arcread
    a = arcread.Arc(arc_path)
    ver, es = arcread.entries(a)
    return sorted({e[0].split('/')[2] for e in es if e[0].startswith('data/map/') and e[0].endswith('.mhd')})


def bake_one(stage, arc_path, out_dir):
    import numpy as np
    import arcread, stage_baker as sb, objects_baker as ob
    a = arcread.Arc(arc_path)
    ver, es = arcread.entries(a)
    a.ver = ver
    by = {e[0]: e for e in es}
    warn = []
    need = ['data/map/%s/%s.%s' % (stage, stage, x) for x in sb.EXTS]
    missing = [n for n in need if n not in by]
    if missing:
        return dict(stage=stage, ok=False, error='missing ' + ', '.join(missing))
    src = {x: arcread.extract_bytes(a, by['data/map/%s/%s.%s' % (stage, stage, x)]) for x in sb.EXTS}
    for k, name in sb.env_paths(stage).items():
        if name in by:
            src['env_' + k] = arcread.extract_bytes(a, by[name])
        else:
            warn.append('env input missing: ' + name)
    osrc = {}
    for n in ob.names(stage):
        full = 'data/map/%s/%s' % (stage, n)
        if full in by:
            osrc[n] = arcread.extract_bytes(a, by[full])
        else:
            warn.append('objects input missing: ' + n)
            osrc[n] = b''
    empties = [n.replace(stage, '*') for n, b in osrc.items() if len(b) <= 4 and not n.endswith('.mta')]
    dummies = [n.replace(stage, '*') for n, b in osrc.items() if b == ob.DUMMY]
    if empties:
        warn.append('empty: ' + ' '.join(sorted(empties)))
    if dummies:
        warn.append("'dummy' placeholder: " + ' '.join(sorted(dummies)))
    res = dict(stage=stage, ok=True, empty_files=sorted(empties), dummy_files=sorted(dummies))
    # terrain + env
    blob, info = sb.assemble(stage, src, 'auto', env=True)
    sb.verify(blob)
    if not info['env']:
        warn.append('no env section (v2 written)')
    words = np.frombuffer(blob, '<u2', 256 * 256, struct.unpack_from('<I', blob, 60)[0])
    flagbits = {}
    for b in range(10, 16):
        c = int(((words >> b) & 1).sum())
        if c:
            flagbits[b] = c
    hf = np.frombuffer(blob, np.uint8, 513 * 513, struct.unpack_from('<I', blob, 56)[0])
    os.makedirs(out_dir, exist_ok=True)
    p = os.path.join(out_dir, stage + '.zvrs')
    with open(p + '.tmp', 'wb') as f:
        f.write(blob)
    os.replace(p + '.tmp', p)
    flags = info['flags']
    res.update(zvrs_bytes=len(blob), version=struct.unpack_from('<I', blob, 4)[0], ntiles=info['ntiles'], pdl=info['nobj'],
               zero_frac=round(info['zero_frac'], 4), hmin=int(hf.min()), hmax=int(hf.max()),
               sea=('guess y=%.0f' % struct.unpack_from('<f', blob, 52)[0]) if flags & 2 else 'none',
               river_tiles=info['water_tiles'], water_key=info['water_key'], env=bool(info['env']), tile_flag_bits=flagbits,
               payload_sha256=blob[192:224].hex())
    # identity hashes: stages whose terrain (heights+words+tiles+clut) / env sections are byte-identical render identically
    import hashlib
    oh, ow, ot, oc = struct.unpack_from('<4I', blob, 56)
    res['terrain_sha'] = hashlib.sha256(blob[oh:oc + 1024]).hexdigest()[:12]
    eo, es = struct.unpack_from('<2I', blob, 88)
    res['env_sha'] = hashlib.sha256(blob[eo:eo + es]).hexdigest()[:12] if info['env'] else ''
    if info['env']:
        eo = struct.unpack_from('<I', blob, 88)[0]
        P = struct.unpack_from('<128f', blob, eo + 8)
        res['water_rgb'] = [round(P[16 * 4 + i] * 255) for i in range(3)]
        res['cloud_alt'] = [round(P[18 * 4 + i]) for i in range(4)]
    if flags & 2 and info['zero_frac'] < 0.2:
        warn.append('sea plane guessed from %.0f%% byte-0 cells' % (100 * info['zero_frac']))
    if info['hmax'] == 0:
        warn.append('flat heightfield (all 0)')
    del blob, src
    # objects
    try:
        oblob, oinfo = ob.assemble(stage, osrc)
        ob.read(oblob)
        p = os.path.join(out_dir, stage + '.zvro')
        with open(p + '.tmp', 'wb') as f:
            f.write(oblob)
        os.replace(p + '.tmp', p)
        import hashlib
        res['objects_sha'] = hashlib.sha256(oblob[128:]).hexdigest()[:12]
        res.update(zvro_bytes=len(oblob), props=oinfo['props'], trees=oinfo['trees'], obj_models=oinfo['models'],
                   obj_tris=oinfo['triangles'], trees_skipped=oinfo['trees_skipped'])
        for te in oinfo['tree_errors']:
            warn.append('tree model skipped (' + te + ')')
        if oinfo['trees_skipped']:
            warn.append('%d tree placements without a tree model' % oinfo['trees_skipped'])
    except Exception as e:   # report, keep the terrain
        res['zvro_error'] = '%s: %s' % (type(e).__name__, e)
        warn.append('zvro failed: ' + res['zvro_error'])
    res['warnings'] = warn
    return res


def table_md(rows, arc_path, out_dir):
    L = ['# Stage table (all baked stages)', '',
         'Generated by `research/world-render-data/bake_all_stages.py` from `%s` (read-only) into `%s` (not in git).' % (os.path.basename(arc_path), out_dir),
         'Every row is one child-process bake: `.zvrs` (terrain + env, loader checks re-run) and `.zvro` (props + trees, loader checks re-run).', '',
         'Columns: size = .zvrs / .zvro bytes; h = height bytes min..max (x80 units); byte0 = fraction of height cells at byte 0; sea = baker sea plane '
         '(a GUESS at y=0, flagged in the file; since STAGE-SWEEP only where the .mti sea bit 13 covers the byte-0 tiles, stage_baker.sea_rule; "none (sweep)" = the 12 stages that lost the old byte-0 guess, docs/world-render/stage-sweep.md); river = water tiles (.mti bit 12 only) / key palette index; '
         'flags = .mti flag bits set (bit: tile words); env = WORLD-2 env section present; water rgb = mapset water colour; clouds = 4 layer altitudes; '
         'props = .pdl instances drawn, trees = .wad/.wpd instances.', '',
         'ident = identity classes T(errain: heights+tile words+tiles+palette) / E(nv section) / O(bjects .zvro payload): stages with the same letter '
         'have byte-identical baked data for that part (e.g. the k/m/n ace-style variants of one mission).', '',
         '| stage | ident | zvrs | zvro | h | byte0 | sea | river | flags | env | water rgb | clouds | props | trees | warnings |',
         '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    cls = {}
    def letter(kind, h):
        if not h:
            return '-'
        d = cls.setdefault(kind, {})
        if h not in d:
            d[h] = len(d) + 1
        return '%s%d' % (kind, d[h])
    for r in rows:
        if not r.get('ok'):
            L.append('| %s | | FAILED | | | | | | | | | | | | %s |' % (r['stage'], r.get('error', '')))
            continue
        fb = ' '.join('%d:%d' % (int(k), v) for k, v in sorted(r['tile_flag_bits'].items(), key=lambda kv: int(kv[0])))
        ident = '%s %s %s' % (letter('T', r.get('terrain_sha')), letter('E', r.get('env_sha')), letter('O', r.get('objects_sha')))
        L.append('| %s | %s | %d | %s | %d..%d | %.3f | %s | %d / %d | %s | %s | %s | %s | %s | %s | %s |' % (
            r['stage'], ident, r['zvrs_bytes'], r.get('zvro_bytes', 'ERR'), r['hmin'], r['hmax'], r['zero_frac'], r['sea'], r['river_tiles'], r['water_key'],
            fb or '-', 'yes' if r['env'] else 'NO', ','.join(map(str, r.get('water_rgb', []))) or '-',
            ','.join(map(str, r.get('cloud_alt', []))) or '-', r.get('props', '-'), r.get('trees', '-'), '; '.join(r['warnings']) or '-'))
    ok = sum(1 for r in rows if r.get('ok'))
    L += ['', '%d of %d stages baked; %d with env; %d .zvro.' % (ok, len(rows), sum(1 for r in rows if r.get('env')),
                                                               sum(1 for r in rows if 'zvro_bytes' in r)), '']
    return '\n'.join(L)


def main(argv):
    opt, i = {}, 0
    while i < len(argv):
        if argv[i].startswith('--') and i + 1 < len(argv):
            opt[argv[i][2:]] = argv[i + 1]; i += 2
        else:
            i += 1
    arc = opt.get('arc', DEFAULT_ARC)
    out = opt.get('out', r'D:\acz-extract\stage-all')
    if 'one' in opt:
        try:
            r = bake_one(opt['one'], arc, out)
        except Exception as e:
            import traceback
            r = dict(stage=opt['one'], ok=False, error='%s: %s' % (type(e).__name__, e), tb=traceback.format_exc()[-800:])
        print('RESULT ' + json.dumps(r))
        return 0
    stages = opt['only'].split(',') if 'only' in opt else stages_in(arc)
    rows = []
    for s in stages:
        p = subprocess.run([sys.executable, '-I', os.path.abspath(__file__), '--one', s, '--arc', arc, '--out', out],
                           capture_output=True, text=True)
        line = [l for l in p.stdout.splitlines() if l.startswith('RESULT ')]
        r = json.loads(line[-1][7:]) if line else dict(stage=s, ok=False, error='child exit %d: %s' % (p.returncode, p.stderr[-400:]))
        rows.append(r)
        print(s, 'ok' if r.get('ok') else 'FAIL', r.get('error', ''), '; '.join(r.get('warnings', [])), flush=True)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, 'bake-results.json'), 'w') as f:
        json.dump(rows, f, indent=1)
    if 'table' in opt:
        with open(opt['table'], 'w', newline='\n') as f:
            f.write(table_md(rows, arc, out))
    return 0 if all(r.get('ok') for r in rows) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
