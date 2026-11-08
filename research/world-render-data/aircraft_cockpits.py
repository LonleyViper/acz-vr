"""COCKPIT-4: player aircraft -> cockpit model table (offline, read-only on game files, numpy + stdlib).

  python -I aircraft_cockpits.py <data dir (…/a/data)> <out.json> [--zvrm-dir D:\\acz-extract\\models-c4]

For every type in data/plmodel/playerlist.txt (36 player types): the skins present (<type><letter>.mpl), the runtime model id
(`<type>k_vrcockpit` style: the first skin in the preference order k, e, l, m, n, u, v, o), the cockpit eye point from data/mis/mpl/<type>.pvb
(floats at bytes 40/44/48 = the live E6 camera offset, model-decode.md finding 2), the other small .pvb fields, the cockpit-part glass and
animated-node facts, and the interior stick style (INFERENCE from the real aircraft: side-stick types listed in SIDE_STICK). Eye-point
collisions are grouped: the E6 camera offset alone (campos - aircraft position, rotated back into the aircraft frame) identifies every type
except the members of a collision group (docs/world-render/cockpit4-impl.md section 5). Writes only the table (numbers and ids, no geometry).
"""
import argparse, json, os, struct, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from acm_lib import mpl_parts, decode_acm  # noqa: E402

SKIN_ORDER = 'kelmnuvo'
# real aircraft with a side-stick (F-16, F-35, F-22, Rafale, Su-37, Su-47, YF-23) [I: public aircraft facts, not game data]
SIDE_STICK = {'f16c', 'f16x', 'f35c', 'fa22', 'rm01', 'su37', 'su47', 'yf23'}


def pvb_fields(path):
    b = open(path, 'rb').read()
    f = struct.unpack_from('<16f', b, 0) if len(b) >= 64 else ()
    return dict(eye=[round(v, 4) for v in struct.unpack_from('<3f', b, 40)], f36=round(struct.unpack_from('<f', b, 36)[0], 4),
                f52=round(struct.unpack_from('<f', b, 52)[0], 4), f56=round(struct.unpack_from('<f', b, 56)[0], 4),
                f60=round(struct.unpack_from('<f', b, 60)[0], 4), bytes=len(b)) if f else None


def cockpit_facts(mpl):
    P = mpl_parts(open(mpl, 'rb').read())
    out = dict(part_bytes=[len(x) for x in P])
    if P[2][:4] == b'ACM\0':
        m = decode_acm(P[2])
        out['cockpit_nodes'] = len(m['nodes'])
        out['cockpit_animated_nodes'] = int((m['nodes']['chan'] >= 0).sum())
        out['cockpit_glass_parts'] = int(sum(1 for nd in m['nodes'] if nd['flags'] & 0x2000 and 0 <= nd['mesh'] < len(m['meshes'])))
    if P[0][:4] == b'ACM\0':
        m = decode_acm(P[0])
        groups = sorted({int(nd['group']) for nd in m['nodes'] if nd['chan'] >= 0 and 0 <= nd['mesh'] < len(m['meshes'])})
        out['exterior_animated_groups'] = ['0x%x' % g for g in groups]
    if len(P) > 9 and P[9][:4] == b'GIM\0':
        out['cockpit_atlas'] = list(struct.unpack_from('<HH', P[9], 0x1c))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('data'); ap.add_argument('out'); ap.add_argument('--zvrm-dir', default='')
    a = ap.parse_args()
    pl = os.path.join(a.data, 'plmodel')
    types = [t.strip() for t in open(os.path.join(pl, 'playerlist.txt'), encoding='ascii', errors='replace') if t.strip()]
    rows = []
    for t in types:
        skins = [c for c in SKIN_ORDER if os.path.exists(os.path.join(pl, t + c + '.mpl'))]
        skins += sorted(c for c in 'abcdefghijklmnopqrstuvwxyz' if c not in skins and os.path.exists(os.path.join(pl, t + c + '.mpl')))
        stem = (t + skins[0]) if skins else None
        pvb = os.path.join(a.data, 'mis', 'mpl', t + '.pvb')
        r = dict(type=t, skins=''.join(skins), model_id=(stem + '_vrcockpit') if stem else None, mpl=(stem + '.mpl') if stem else None,
                 pvb=pvb_fields(pvb) if os.path.exists(pvb) else None, stick='side' if t in SIDE_STICK else 'centre')
        if stem:
            r.update(cockpit_facts(os.path.join(pl, stem + '.mpl')))
        if a.zvrm_dir:
            r['baked'] = os.path.exists(os.path.join(a.zvrm_dir, r['model_id'] + '.zvrm')) if r['model_id'] else False
        rows.append(r)
    # eye-point collision groups (1 mm in model units = 0.001)
    groups = {}
    for r in rows:
        if r['pvb']:
            groups.setdefault(tuple(round(v, 3) for v in r['pvb']['eye']), []).append(r['type'])
    for r in rows:
        if r['pvb']:
            g = groups[tuple(round(v, 3) for v in r['pvb']['eye'])]
            r['eye_collides_with'] = [x for x in g if x != r['type']]
    doc = dict(source='data/plmodel/playerlist.txt + <type><skin>.mpl + data/mis/mpl/<type>.pvb (read-only)', generator='research/world-render-data/aircraft_cockpits.py',
               runtime='xr_cockpit_model=<model_id>[;stick:side] (bake: model_baker.py vrcockpit <mpl> <pvb> <model_id>.zvrm)',
               eye_unique_types=sum(1 for r in rows if r['pvb'] and not r['eye_collides_with']),
               eye_collision_groups=[g for g in groups.values() if len(g) > 1], aircraft=rows)
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(doc, f, indent=1)
    print('%d types, %d with a unique eye point, collision groups %s -> %s' % (len(rows), doc['eye_unique_types'], doc['eye_collision_groups'], a.out))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
