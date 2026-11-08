"""AIRCRAFT-AUTO: bake every player aircraft of aircraft_cockpits.json (cockpit + exterior + vrcockpit) one at a time.

Offline, read-only on the game files (an extracted a.arc data folder), numpy + stdlib only, no network.

  python -I bake_all_aircraft.py <a/data> <out dir> [--table aircraft_cockpits.json] [--report report.json] [--only a10a,f15c] [--md table.md]
  python -I bake_all_aircraft.py --md-from report.json --md docs/world-render/aircraft-table.md [--runtime check.txt]
      (no baking: the check table from an existing report; --runtime = the output of aircraft_auto_offline_tests run with
       ACZ_MODELS_ALL=<out dir>, whose 'real <model id> ok ...' lines add the DLL loader's view: drawn parts, canopy, interior, animated surfaces)

Per type (model id <type><skin>, skin from the table; the file names equal what model_baker.py's CLI writes, byte-identical):
  <id>_cockpit.zvrm    model_baker mpl --parts 2 --eye-pvb   (the game's cockpit part alone, cockpit atlas)
  <id>_exterior.zvrm   model_baker mpl --parts 0             (airframe, skin texture)
  <id>_vrcockpit.zvrm  model_baker vrcockpit                 (what xr_cockpit_model=<id>_vrcockpit / auto loads)
A failure of one bake is recorded (exception text) and the next one runs. The report also carries what is needed to judge eye-point ties:
SHA-256 of the raw cockpit part (mpl part 2), cockpit atlas (part 9), exterior part (part 0) and of the baked cockpit geometry (positions
rounded to 1e-4 units, uvs, material flags, the textures the cockpit parts use).
"""
import argparse, hashlib, json, math, os, re, struct, sys, time, traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np  # noqa: E402
import model_baker as mb  # noqa: E402
from acm_lib import mpl_parts, decode_acm  # noqa: E402


# STORES (headset test 7, F-5E 'missiles aren't showing on the racks'): the weapons are separate game models (data/mis/mpl/<type>ex.bin, texture
# data/lodweapon/weapon.gim), never part of the .mpl, so the vrcockpit bake had racks (pylon plates, part 0 groups 53/54/55) and no missiles.
# type -> weapon pack (relative to the data dir), weapon index in the pack (0 = the 3.0 m missile with a motor nozzle node), the weapon-station
# locator groups that carry a drawn pylon plate.  [I] which weapon the game mounts where per loadout is not decoded; this is the full-rack view.
# Station sets: f05e (hand-set at the headset test 7 bake) and every other type from `--propose-stores` (model_baker.find_missile_index / station_candidates /
# choose_stations: the first missile-like entry of the pack, mirrored wing PAIRS of locators whose weapon box does not sit inside a pylon-plate mesh and does not
# overlap another chosen station; at most 4).  j35j (the Draken the null batches fly) was checked by preview: four parallel missiles beside the wing tank.
# Types missing here have no proposed pair (internal weapons, no missile-like pack entry, or every locator inside a rack mesh): their baked model is unchanged.
# [I] the real loadout per station and mission is not decoded; this is the full-rack view of the aircraft's own missile.
STORES = {
    'a10a': dict(pack='mis/mpl/a10aex.bin', index=0, stations=[69, 70]),
    'f01a': dict(pack='mis/mpl/f01aex.bin', index=1, stations=[71, 72]),
    'f04e': dict(pack='mis/mpl/f04eex.bin', index=0, stations=[67, 68]),
    'f05e': dict(pack='mis/mpl/f05eex.bin', index=0, stations=[67, 68, 75, 76]),
    'f14d': dict(pack='mis/mpl/f14dex.bin', index=0, stations=[69, 70, 71, 72]),
    'f15c': dict(pack='mis/mpl/f15cex.bin', index=0, stations=[67, 68, 69, 70]),
    'f15e': dict(pack='mis/mpl/f15eex.bin', index=0, stations=[67, 68]),
    'f15s': dict(pack='mis/mpl/f15sex.bin', index=0, stations=[67, 68, 71, 72]),
    'f16x': dict(pack='mis/mpl/f16xex.bin', index=0, stations=[75, 76]),
    'f17a': dict(pack='mis/mpl/f17aex.bin', index=1, stations=[67, 68]),
    'f18c': dict(pack='mis/mpl/f18cex.bin', index=0, stations=[71, 72]),
    'fa22': dict(pack='mis/mpl/fa22ex.bin', index=0, stations=[67, 68, 69, 70]),
    'j35j': dict(pack='mis/mpl/j35jex.bin', index=0, stations=[71, 72, 73, 74]),
    'm29a': dict(pack='mis/mpl/m29aex.bin', index=0, stations=[75, 76]),
    'm31b': dict(pack='mis/mpl/m31bex.bin', index=0, stations=[67, 68, 75, 76]),
    'mr2d': dict(pack='mis/mpl/mr2dex.bin', index=0, stations=[69, 70, 71, 72]),
    'rm01': dict(pack='mis/mpl/rm01ex.bin', index=0, stations=[73, 74]),
    'su27': dict(pack='mis/mpl/su27ex.bin', index=0, stations=[69, 70]),
    'su37': dict(pack='mis/mpl/su37ex.bin', index=0, stations=[69, 70]),
    'su47': dict(pack='mis/mpl/su47ex.bin', index=0, stations=[71, 72]),
    'typn': dict(pack='mis/mpl/typnex.bin', index=0, stations=[67, 68, 69, 70]),
    'x29a': dict(pack='mis/mpl/x29aex.bin', index=0, stations=[71, 72]),
    'yf23': dict(pack='mis/mpl/yf23ex.bin', index=0, stations=[71, 72]),
    'zoef': dict(pack='mis/mpl/zoefex.bin', index=1, stations=[71, 72]),
}

# TANKS (gap 3 of headset test 7): mpl part 7 (the external-stores/tank model) at the centre-line locator group.  Only f05e is baked: the previews of
# f05e (one belly tank) and j35j (two belly pods, 356 triangles) look right, but whether the game mounts them by default is not known.
TANKS = {
    'f05e': dict(station=79, part=7),
}


# SP-STORES (2026-11-01): the selected special weapon as its own station group '<model>/sp<station>/n<node>' (the HUD's SP row: SAAM ---8 on the F-5E).
# Evidence (aircraft-auto.md section 15): [E structure] the mpl's weapon-station locators hang under three slot nodes (group 53/54/55); [E] the hangar's PLAYER weapon
# screen of the Draken (step124 runK10, png t79569) lists MISSILE 50 / SAAM 8 / UGB -- / RCL -- with three icons, and the Draken's ex.bin entries 0/1/2 are an olive
# missile, a black bomb (UGB) and a 1.4 m rocket (RCL) [shapes previewed] = the hangar list order; [I] therefore ex.bin entry i = special weapon i of the hangar
# list, mounted on the stations of slot i (model_baker.slot_stations).  SAAM is the FIRST special of both the Draken and the F-5E (the hangar default), so SAAM = entry 0
# = slot 0 (f05e: stations 67, 68).  That is the very weapon the STORES bake called the 'standard missile' (the real MISSILE has no ex.bin entry: [I] lodweapon/wz*_ms.bin).
# The std group therefore skips the stations the SP group takes (dedupe_std), so no weapon is drawn twice.  `--sp-slot 0|1|2` bakes another slot, `--sp-slot none`
# no SP group; only the F-5E is listed (asked first): every other file stays byte-identical to the STORES bake.
SP_STORES = {
    'f05e': dict(label='SAAM', slot=0),
}


# STD-MS (2026-11-01): the STANDARD missile (HUD 'MSSL', 50 rounds) is NOT an ex.bin entry (entry i = special weapon i, section 15): it is one of the four
# data/lodweapon/wz<L>1_ms.bin files (L = a, f, j, r; same container as one ex.bin weapon, same weapon.gim atlas; 106..138 triangles, 10.4..15.8 u long, plain AAMs
# distinct from the SAAM entry 0).  [E] four files exist and the letters equal the first letter of four type codes (a10a, f05e, j35j, rm01).  [I] the game picks the
# file by the aircraft's first letter; the types whose first letter has no file (e m s t x y z and 'mr', 'ty') get STD_MS_FALLBACK, a guess.
STD_MS_LETTERS = 'afjr'
STD_MS_FALLBACK = 'f'


def std_ms_name(t):
    """File name (under data/lodweapon) of the standard missile for aircraft type `t`: wz<first letter>1_ms.bin when that file exists in the game, else the fallback."""
    L = t[:1].lower()
    return 'wz%s1_ms.bin' % (L if L in STD_MS_LETTERS else STD_MS_FALLBACK)


def dedupe_std(st, spx):
    """The standard-store spec without the stations the SP group takes (one locator must not carry two weapons); None when nothing is left."""
    if not st or not spx:
        return st
    keep = [g for g in st['stations'] if g not in spx['stations']]
    return dict(st, stations=keep) if keep else None


def sp_spec(t, data, ex, slot_override=None):
    """The `sp=` argument of model_baker.bake_vr_cockpit for type `t`, or None.  The weapon index is the slot number, the stations are the slot's locators."""
    ent = SP_STORES.get(t)
    if slot_override == 'none' or (ent is None and slot_override is None):
        return None
    slot = int(ent['slot'] if slot_override is None else slot_override)
    st = mb.slot_stations(ex, slot)
    if not st:
        return None
    return dict(pack=os.path.join(data, 'mis', 'mpl', t + 'ex.bin'), index=slot, stations=st, gim=os.path.join(data, 'lodweapon', 'weapon.gim'))


def sha(b):
    return hashlib.sha256(b).hexdigest()


def baked_geometry_sha(path, source_part=None):
    """SHA-256 over the triangles of a .zvrm (optionally only parts baked from `source_part`): positions rounded to 1e-4 units (float noise of
    the node transforms, e.g. f15e/f15s differ by 4e-6), uvs, material flags and texture slot per part, and the textures those parts use."""
    z = mb.read_zvrm(path)
    h = hashlib.sha256()
    used = set()
    for p in z['parts']:
        if source_part is not None and p.get('src', None) != source_part:
            continue
        fi, ic = p['fi'], p['ic']
        if not ic:
            continue
        used.add(p['tex'])
        idx = z['I'][fi:fi + ic]
        h.update(np.round(z['V']['pos'][idx].astype(np.float64) * 1e4).astype('<i8').tobytes())
        h.update(np.ascontiguousarray(z['V']['uv'][idx], '<f4').tobytes())
        h.update(struct.pack('<II', p['mflags'], p['tex']))
    for i, t in enumerate(z['textures']):
        if i in used:
            h.update(np.ascontiguousarray(t).tobytes())
    return h.hexdigest()


def bake(g):
    tab = json.load(open(g.table))
    os.makedirs(g.out, exist_ok=True)
    only = {x for x in g.only.split(',') if x}
    rows = []
    for a in tab['aircraft']:
        t = a['type']
        if only and t not in only:
            continue
        stem = a['model_id'][:-len('_vrcockpit')]
        mpl = os.path.join(g.data, 'plmodel', a['mpl'])
        pvb = os.path.join(g.data, 'mis', 'mpl', t + '.pvb')
        r = dict(type=t, model_id=a['model_id'], mpl=a['mpl'], eye_table=a['pvb']['eye'], stick=a['stick'], bakes={}, notes=[])
        try:
            P = mpl_parts(open(mpl, 'rb').read())
            r['mpl_parts'] = len(P)
            r['sha_part2'] = sha(P[2]) if len(P) > 2 else ''
            r['sha_part9'] = sha(P[9]) if len(P) > 9 else ''
            r['sha_part0'] = sha(P[0]) if len(P) > 0 else ''
            r['eye_pvb'] = [float(v) for v in mb.pvb_eye(pvb)]
        except Exception as e:  # noqa: BLE001
            r['notes'].append('source: %s' % e)
        for kind in ('cockpit', 'exterior', 'vrcockpit'):
            out = os.path.join(g.out, '%s_%s.zvrm' % (stem, kind))
            t0 = time.time()
            try:
                if kind == 'cockpit':
                    res = mb.bake_mpl(mpl, out, [2], mb.pvb_eye(pvb))
                elif kind == 'exterior':
                    res = mb.bake_mpl(mpl, out, [0], None)
                else:
                    st = None
                    if t in STORES and not g.no_stores:
                        st = dict(STORES[t]); st['pack'] = os.path.join(g.data, *st['pack'].split('/'))
                        st['gim'] = os.path.join(g.data, 'lodweapon', 'weapon.gim')
                        if g.std_ms != 'none':
                            st['ms'] = os.path.join(g.data, 'lodweapon', std_ms_name(t))   # the standard missile, not ex.bin entry `index` (that is a special weapon)
                    tk = dict(TANKS[t]) if t in TANKS and not g.no_stores else None
                    spx = None if g.no_stores else sp_spec(t, g.data, decode_acm(P[0]), g.sp_slot)
                    res = mb.bake_vr_cockpit(mpl, out, mb.pvb_eye(pvb), stores=dedupe_std(st, spx), tank=tk, sp=spx)
                z = mb.read_zvrm(out)
                res = dict(res)
                res['ok'] = True
                res['sha256'] = z['sha256']
                res['eye'] = [round(float(v), 4) for v in z['eye']] if z['eye'] is not None else None
                res['bbox'] = [[round(float(v), 2) for v in z['bbox'][0]], [round(float(v), 2) for v in z['bbox'][1]]]
                res['seconds'] = round(time.time() - t0, 2)
                if kind == 'vrcockpit':
                    res['cockpit_geometry_sha'] = baked_geometry_sha(out, source_part=2)
                    res['exterior_geometry_sha'] = baked_geometry_sha(out, source_part=0)
                    res['hidden_exterior_nodes'] = list(res.get('hidden_exterior_nodes', []))
                r['bakes'][kind] = res
            except Exception as e:  # noqa: BLE001
                r['bakes'][kind] = dict(ok=False, error='%s: %s' % (type(e).__name__, e), trace=traceback.format_exc(limit=3))
                if os.path.exists(out):
                    os.remove(out)   # never leave a half-written bake behind
            print(t, kind, 'ok' if r['bakes'][kind]['ok'] else 'FAILED ' + r['bakes'][kind]['error'], flush=True)
        rows.append(r)
    return dict(source='bake_all_aircraft.py', table=os.path.basename(g.table), data=g.data, out=g.out, aircraft=rows)


def parse_runtime(path):
    """'real <model id> ok parts=.. tris=.. drawn=.. hud=.. canopy=N[(ext)] interior=.. anim=D/U eye-diff=..' lines (console-wrapped lines are joined)."""
    if not path:
        return {}
    txt = open(path, encoding='utf-8-sig', errors='replace').read().replace('\r', '')
    txt = re.sub(r'\s+', ' ', txt)
    out = {}
    pat = (r'real (\S+_vrcockpit) (\S+) parts=(\d+) tris=(\d+) drawn=(\d+) hud=(\S+) canopy=(\d+)(\(ext\))? interior=(\d+) '
           r'anim=(\d+)/(\d+) eye-diff=([0-9.]+)')
    for m in re.finditer(pat, txt):
        out[m.group(1)] = dict(status=m.group(2), parts=int(m.group(3)), tris=int(m.group(4)), drawn=int(m.group(5)), hud=m.group(6),
                               canopy=int(m.group(7)), canopy_ext=bool(m.group(8)), interior=int(m.group(9)), anim_driven=int(m.group(10)),
                               anim_not=int(m.group(11)), eye_diff=float(m.group(12)))
    return out


def write_md(rep, runtime_txt, path):
    rt = parse_runtime(runtime_txt)
    rows = rep['aircraft']
    ck_groups, ex_groups, eyes = {}, {}, {}
    for r in rows:
        v = r['bakes'].get('vrcockpit', {})
        ck_groups.setdefault(v.get('cockpit_geometry_sha', r['type']), []).append(r['type'])
        ex_groups.setdefault(v.get('exterior_geometry_sha', r['type']), []).append(r['type'])
        eyes.setdefault(tuple(r['eye_table']), []).append(r['type'])
    L = ['# Player aircraft bake table (AIRCRAFT-AUTO)', '',
         'Generated by `python -I research/world-render-data/bake_all_aircraft.py --md-from <report.json> --md docs/world-render/aircraft-table.md'
         ' --runtime <check.txt>` from the bakes in `%s` (game-derived, not committed; report `bake-report.json` next to them).' % rep.get('out', '?'),
         'Columns: **c/e/vr** = `<id>_cockpit` (mpl part 2) / `<id>_exterior` (part 0) / `<id>_vrcockpit` (part 2 + part 0: what `xr_cockpit_model`'
         ' loads), y = baked; **tris** = baked triangles; **eye** = the `.pvb` eye point (model units, +X right, +Y up, -Z nose) and its length = the'
         ' live E6 camera-aircraft distance with the look at rest; **runtime** = the DLL loader + mesh builder on the baked file'
         ' (`aircraft_auto_offline_tests` with `ACZ_MODELS_ALL`, options `auto;anim:1`): status, parts drawn / canopy glass parts (ext = the exterior'
         ' canopy seen from inside) / procedural interior triangles (190 = side stick, 202 = centre stick) / animated surfaces driven/not driven.', '',
         '| # | type | model id | c/e/vr | tris c/e/vr | eye x, y, z | eye len | stick | runtime | notes |',
         '|---|---|---|---|---|---|---|---|---|---|']
    nok = nrt = 0
    for i, r in enumerate(rows, 1):
        b = r['bakes']
        ok = ''.join('y' if b.get(k, {}).get('ok') else 'N' for k in ('cockpit', 'exterior', 'vrcockpit'))
        nok += ok == 'yyy'
        tris = '/'.join(str(b[k]['triangles']) if b.get(k, {}).get('ok') else '-' for k in ('cockpit', 'exterior', 'vrcockpit'))
        e = r['eye_table']
        el = math.sqrt(sum(x * x for x in e))
        q = rt.get(r['model_id'])
        if q and q['status'] == 'ok':
            nrt += 1
        rts = ('%s %d / %d%s / %d / %d/%d' % (q['status'], q['drawn'], q['canopy'], ' ext' if q['canopy_ext'] else '', q['interior'],
                                              q['anim_driven'], q['anim_not'])) if q else 'not checked'
        notes = []
        same = eyes[tuple(e)]
        if len(same) > 1:
            notes.append('eye identical to ' + '+'.join(x for x in same if x != r['type']) + ' (tie rule, aircraft-auto.md)')
        vg = b.get('vrcockpit', {})
        grp = ck_groups.get(vg.get('cockpit_geometry_sha', r['type']), [])
        if len(grp) > 1:
            notes.append('cockpit part geometry+atlas identical to ' + '+'.join(x for x in grp if x != r['type']))
        egrp = ex_groups.get(vg.get('exterior_geometry_sha', r['type']), [])
        if len(egrp) > 1:
            notes.append('exterior identical to ' + '+'.join(x for x in egrp if x != r['type']))
        if abs(e[0]) > 1e-6:
            notes.append('eye off the centre line (x %.3f)' % e[0])
        if vg.get('hidden_exterior_nodes') is not None:
            notes.append('%d exterior nodes hidden (pilot/tub)' % len(vg['hidden_exterior_nodes']))
        if q and not q['canopy_ext'] and q['canopy']:
            notes.append('canopy glass from the cockpit part')
        if q and q['eye_diff'] > 1e-4:
            notes.append('baked eye differs from the table!')
        if r.get('eye_pvb') and max(abs(a - c) for a, c in zip(r['eye_pvb'], e)) > 1e-4:
            notes.append('pvb differs from the table!')
        notes += r.get('notes', [])
        for k in ('cockpit', 'exterior', 'vrcockpit'):
            if not b.get(k, {}).get('ok'):
                notes.append('%s: %s' % (k, b.get(k, {}).get('error', 'missing')))
        L.append('| %d | %s | `%s` | %s | %s | %.3f, %.3f, %.3f | %.3f | %s | %s | %s |' % (
            i, r['type'], r['model_id'], ok, tris, e[0], e[1], e[2], el, r['stick'], rts, '; '.join(notes)))
    L += ['', 'Baked (all three files): %d/%d types. Runtime-checked (loads + builds, baked eye = table eye): %d/%d.' % (nok, len(rows), nrt, len(rows))]
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(L) + '\n')
    print('wrote', path)


def propose_stores(g):
    """The STORES table the rules propose (see the comment above STORES): one line per type with at least one mirrored pair; hand-set entries are NOT applied."""
    tab = json.load(open(g.table))
    only = {x for x in g.only.split(',') if x}
    lines = []
    for a in tab['aircraft']:
        t = a['type']
        if only and t not in only:
            continue
        pk = os.path.join(g.data, 'mis', 'mpl', t + 'ex.bin')
        if not os.path.exists(pk):
            print('# %s: no weapon pack' % t); continue
        idx = mb.find_missile_index(pk)
        if idx is None:
            print('# %s: no missile-like pack entry' % t); continue
        ex = decode_acm(mpl_parts(open(os.path.join(g.data, 'plmodel', a['mpl']), 'rb').read())[0])
        ch = mb.choose_stations(mb.station_candidates(ex, mb.weapon_model(pk, idx)))
        if not ch:
            print('# %s: no mirrored pair of free stations' % t); continue
        lines.append("    '%s': dict(pack='mis/mpl/%sex.bin', index=%d, stations=%s)," % (t, t, idx, ch))
    print(os.linesep.join(lines))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('data', nargs='?'); ap.add_argument('out', nargs='?')
    ap.add_argument('--table', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'aircraft_cockpits.json'))
    ap.add_argument('--report'); ap.add_argument('--only', default='')
    ap.add_argument('--md'); ap.add_argument('--md-from'); ap.add_argument('--runtime'); ap.add_argument('--no-stores', action='store_true', help='bake without the weapon meshes and tanks (the pre-STORES models)')
    ap.add_argument('--sp-slot', default=None, help='SP-STORES: 0|1|2 = bake the special weapon of that slot (default: the SP_STORES table), none = no SP group')
    ap.add_argument('--std-ms', default='auto', choices=('auto', 'none'), help="STD-MS: auto = the standard station group is the data/lodweapon/wz<L>1_ms.bin missile; none = the old ex.bin entry (the SAAM model; test2 bake)")
    ap.add_argument('--propose-stores', action='store_true', help='print the STORES table the rules propose from <data> (no baking)')
    g = ap.parse_args()
    if g.propose_stores:
        if not g.data:
            ap.error('--propose-stores needs <data>')
        return propose_stores(g)
    if g.md_from:
        rep = json.load(open(g.md_from))
        if g.md:
            write_md(rep, g.runtime, g.md)
        return 0
    if not (g.data and g.out):
        ap.error('data and out are required unless --md-from is given')
    rep = bake(g)
    if g.report:
        with open(g.report, 'w') as f:
            json.dump(rep, f, indent=1)
    if g.md:
        write_md(rep, g.runtime, g.md)
    bad = [(r['type'], k) for r in rep['aircraft'] for k, v in r['bakes'].items() if not v['ok']]
    print('baked %d types, failures: %s' % (len(rep['aircraft']), bad or 'none'))
    return 0 if not bad else 1


if __name__ == '__main__':
    raise SystemExit(main())
