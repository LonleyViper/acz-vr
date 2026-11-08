"""SEAT-CLEARANCE offline check (headset test 6 item 4: "the seat we rendered in is too far forward so when I look up my head clips into it").

For every baked model (read-only: <models_dir>\\*_vrcockpit.zvrm) report how far the seat / headrest geometry behind the eye is from the head, in PHYSICAL metres
(model distance * cscale / upm), before (seatback 0 = the COCKPIT-4 placement) and after (seatback = the new default):

  python -I cockpit_seat_clearance.py [models_dir] [--cscale 1.15] [--upm 5] [--seatback 0.30] [--json out.json] [--strict]

Columns: game = nearest triangle of the game's own cockpit part (not exterior/glass/blend) in the head zone behind the eye (>= 0.10 m aft, y -0.15..+0.45,
|x| <= 0.22); seat = nearest triangle of the procedural seat (headrest, seat back, rails, pan; cockpit_model.cpp build_mesh port in cockpit_clip_survey.interior)
in the head zone; aft = the headrest front's distance behind the eye plane; clear = min(aft, game) - HEAD_BACK_M (the gap behind the occiput at the neutral
head); look = the smallest signed distance (negative = inside the seat) from the occiput / crown / back-top of a head that pitches up 0..90 deg about the neck to the
seat boxes and game triangles behind. PASS = clear >= 0.10 m and look >= 0 (no clipping); game-limited = the seat passes but a game triangle is nearer. The head model (HEAD_BACK_M 0.17, neck pivot 0.12 below / 0.10 behind the eye)
is an anthropometric estimate [I], not game data.
Model-space note: the C++ also fits the interior into the airframe skin (fit_point) and clips it to the aircraft clip volume (kClipVolumes); the port applies the clip
volume footprint but not the skin fit (which only moves vertices that poke out of the skin; the headrest is inside it on every model checked).
"""
import argparse, json, os, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from cockpit_clip_survey import interior, tris_of, seat_back_max, U   # noqa: E402
from model_baker import read_zvrm, MF_BLEND, MF_GLASS, MF_HIDDEN, MF_EXTERIOR, MF_COCKPIT_DUP   # noqa: E402

DEFAULT_SEATBACK = 0.30     # model metres (5 u/m, unscaled model): eye to the headrest front = Options::seat_back; physical = this * cscale (0.345 m at 1.15)
HEAD_BACK_M = 0.17          # eye to the back of the skull
NECK = np.array([0.0, -0.12, 0.10])                 # neck pivot relative to the eye (x right, y up, z aft), metres
HEAD_PTS = np.array([[0.0, 0.01, HEAD_BACK_M], [0.0, 0.11, 0.08], [0.0, 0.07, 0.15]])   # occiput, crown, back-top
MIN_CLEAR_M, MIN_LOOK_M = 0.10, 0.0
GAME_AFT_MIN_M = 0.10      # game cockpit triangles count as 'behind the head' from 0.10 m aft of the eye plane (overhead canopy bows at z ~ 0 do not)
SEAT_NAMES = ('seat-base', 'seat-pan', 'seat-back', 'headrest', 'seat-rail-left', 'seat-rail-right')


def sample(T, n=3):
    """Surface samples of triangles T (N,3,3): barycentric grid of n subdivisions (plus the vertices)."""
    if not len(T):
        return np.zeros((0, 3))
    w = [(i / n, j / n, (n - i - j) / n) for i in range(n + 1) for j in range(n + 1 - i)]
    w = np.array(w)
    return np.einsum('kw,nwc->nkc', w, T).reshape(-1, 3)


def clip_rows(path=os.path.join(HERE, 'cockpit_clip_volumes.json')):
    return {r['model_id']: (r['ahead'], r['behind'], r['below'], r['above'], r['half']) for r in json.load(open(path))['volumes']}


def box_sd(p, lo, hi):
    """Signed distance of point p to the axis-aligned box lo..hi (negative inside)."""
    c, h = 0.5 * (lo + hi), 0.5 * (hi - lo)
    q = np.abs(p - c) - h
    return float(np.linalg.norm(np.maximum(q, 0))) if (q > 0).any() else float(q.max())


def head_sweep(S, boxes):
    """Smallest signed distance (physical metres, negative = the head is inside a seat box) from the head points of a head pitched up 0..90 deg about the neck
    to the game samples S and the procedural seat boxes [(lo, hi)] (all metres rel. the eye, physical)."""
    best = 1e9
    for th in np.radians(np.arange(0, 91, 5)):
        c, s = np.cos(th), np.sin(th)
        for p in HEAD_PTS:
            v = p - NECK
            q = NECK + np.array([v[0], v[1] * c - v[2] * s, v[1] * s + v[2] * c])
            if len(S):
                best = min(best, float(np.linalg.norm(S - q, axis=1).min()))
            for lo, hi in boxes:
                best = min(best, box_sd(q, lo, hi))
    return best


def measure(z, eye, seatback, cscale, upm, clipbox):
    """-> dict of game_d, seat_d (aft distance of the nearest seat triangle in the head zone), seat_eye_dist, look, all PHYSICAL metres."""
    k = cscale / upm                                       # model units -> physical metres
    P = z['parts']
    game = [tris_of(z, i) for i, p in enumerate(P) if p['ic'] and not (p['mflags'] & (MF_HIDDEN | MF_COCKPIT_DUP | MF_EXTERIOR | MF_BLEND | MF_GLASS))]
    G = sample(np.concatenate(game)) if game else np.zeros((0, 3))
    G = (G - eye) * k
    it, _ = interior(z, eye, stick=0, seatback=seatback, clipbox=clipbox)
    seat = [t for n, t in it if n in SEAT_NAMES]
    S = sample(np.concatenate(seat)) if seat else np.zeros((0, 3))
    S = (S - eye) * k
    gz = G[(G[:, 2] >= GAME_AFT_MIN_M) & (G[:, 1] >= -0.15) & (G[:, 1] <= 0.45) & (np.abs(G[:, 0]) <= 0.22)]
    sz = S[(S[:, 1] >= -0.15) & (np.abs(S[:, 0]) <= 0.22)]
    both = np.concatenate([gz, sz]) if len(gz) + len(sz) else np.zeros((0, 3))
    boxes = [((t.reshape(-1, 3).min(0) - eye) * k, (t.reshape(-1, 3).max(0) - eye) * k) for n, t in it if n in SEAT_NAMES]
    return dict(
        game_d=float(np.linalg.norm(gz, axis=1).min()) if len(gz) else None,
        seat_d=float(np.linalg.norm(sz, axis=1).min()) if len(sz) else None,
        aft_d=float(sz[:, 2].min()) if len(sz) else None,          # the procedural headrest front's distance behind the eye plane
        look=head_sweep(G[G[:, 2] >= 0.0], boxes) if (len(G) or boxes) else None)


def table(models_dir, cscale, upm, seatback):
    clip = clip_rows()
    rows = []
    for f in sorted(x for x in os.listdir(models_dir) if x.endswith('_vrcockpit.zvrm')):
        mid = f[:-5]
        z = read_zvrm(os.path.join(models_dir, f))
        if z['eye'] is None:
            continue
        E = np.array(z['eye'], float)
        cb = clip.get(mid)
        b = measure(z, E, 0.0, cscale, upm, cb)
        a = measure(z, E, seatback, cscale, upm, cb)
        rows.append(dict(model=mid, before=b, after=a))
    return rows


def clear_of(m):
    """Head clearance (physical metres): the nearer of the procedural headrest face (distance behind the eye plane) and the game's own cockpit triangles in the
    head zone (Euclidean), minus HEAD_BACK_M. None when nothing is there."""
    d = [v for v in (m['aft_d'], m['game_d']) if v is not None]
    return min(d) - HEAD_BACK_M if d else None


def verdict(m):
    """'pass', 'game-limited' (the procedural seat clears the head; the game's own cockpit triangles are the nearer limit: not fixable by moving the seat) or 'FAIL'."""
    if passes(m):
        return 'pass'
    if m['aft_d'] is not None and m['aft_d'] - HEAD_BACK_M >= MIN_CLEAR_M - 1e-6 and (m['look'] is None or m['look'] >= MIN_LOOK_M):
        return 'game-limited'
    return 'FAIL'


def passes(m):
    c = clear_of(m)
    return c is not None and c >= MIN_CLEAR_M - 1e-6 and (m['look'] is None or m['look'] >= MIN_LOOK_M)


def fmt(v):
    return '   -  ' if v is None else '%6.3f' % v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir', nargs='?', default=os.path.join(os.environ.get('LOCALAPPDATA', ''), 'acz-vr', 'models'))
    ap.add_argument('--cscale', type=float, default=1.15)
    ap.add_argument('--upm', type=float, default=5.0)
    ap.add_argument('--seatback', type=float, default=DEFAULT_SEATBACK, help='Options::seat_back, model metres')
    ap.add_argument('--json')
    ap.add_argument('--strict', action='store_true', help='exit 1 when any AFTER row fails')
    a = ap.parse_args()
    rows = table(a.dir, a.cscale, a.upm, a.seatback)
    print('cscale %.2f upm %.1f seatback %.2f model m (all distances below are PHYSICAL metres = model * cscale; clear = min(aft, game) - %.2f head back)' % (a.cscale, a.upm, a.seatback, HEAD_BACK_M))
    print('%-18s | BEFORE game  seat   aft  clear  look  | AFTER  game  seat   aft  clear  look | verdict' % 'model')
    bad = 0
    for r in rows:
        b, af = r['before'], r['after']
        cb, ca = clear_of(b), clear_of(af)
        bad += verdict(af) == 'FAIL'
        print('%-18s |        %s %s %s %s %s |       %s %s %s %s %s | %s -> %s' % (r['model'], fmt(b['game_d']), fmt(b['seat_d']), fmt(b['aft_d']), fmt(cb), fmt(b['look']),
              fmt(af['game_d']), fmt(af['seat_d']), fmt(af['aft_d']), fmt(ca), fmt(af['look']), verdict(b), verdict(af)))
    print('%d models, %d FAIL after, %d game-limited after' % (len(rows), bad, sum(verdict(r['after']) == 'game-limited' for r in rows)))
    if a.json:
        json.dump(dict(cscale=a.cscale, upm=a.upm, seatback=a.seatback, head_back_m=HEAD_BACK_M, rows=rows), open(a.json, 'w'), indent=1)
    return 1 if (bad and a.strict) else 0


if __name__ == '__main__':
    raise SystemExit(main())
