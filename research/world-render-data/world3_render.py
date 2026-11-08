"""WORLD-3 offline check: stage terrain + placed props (.pdl -> pmmdl) + trees (.wad/.wpd) rendered from a game pose.

  python -I world3_render.py <out.png> [--stage m02a] [--pose x,y,z,heading_rad,pitch_rad] [--w 1210]
                             [--hfov 63.2 --vfov 38.1] [--shot <game png>] [--tree-range 16000] [--prop-range 60000]
                             [--tree-scale 1.0] [--no-trees] [--no-props]

Camera = the game convention verified in D-297/D-300: right-handed, forward = (-sin h cos p, sin p, -cos h cos p).
Terrain: terrain_render.py (inverse-depth z-buffer, CLUT texels, hillshade, fog).  Props/trees: model_raster.Raster with
its depth buffer PRE-FILLED with the terrain depth, i.e. exactly the runtime rule (props depth-tested against terrain).
Props: world vertex = pdl (x,y,z) + model vertex (no rotation: yaw is baked into each pmmdl model, world_objects.py).
Trees: world = (x, terrain(x,z), z) + tree vertex (tree type = list index; y from the bilinear heightfield: inference).
With --shot the output is a sheet: game window (title bar cropped) | ours | 50:50.
"""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import terrain_render as tr
from terrain_lib import Stage, write_png
from model_raster import Raster, ypr
from acm_lib import decode_acm
from model_baker import table, pmtex_textures, gim_image
import world_objects as wo

ROOT = 'D:/acz-extract/a/data/map'
STEP56 = '<repo>/research/experiments/step56-terrain-run4/game-window-shots/nosedown/acz-20261026-231038-575-T7.03.png'
# E6 at the step56 png (ee-camera-obs-around-png.txt ticks 196764726/196764743 interpolated to 196764734)
POSE56 = (-13854.0, 3647.3, 121515.3, -0.785398, -0.5109)


def acm_tris(acm, tex_for_node):
    """-> dict(V (T,3,3), UV (T,3,2), COL (T,3,4), NRM (T,3,3), TEX (T,), ATEST (T,)) in model space, CCW front."""
    m = decode_acm(acm)
    out = dict(V=[], UV=[], COL=[], NRM=[], TEX=[], ATEST=[])
    for k, nd in enumerate(m['nodes']):
        if not (0 <= nd['mesh'] < len(m['meshes'])):
            continue
        me = m['meshes'][nd['mesh']]
        if not len(me['tri']):
            continue
        W = m['world'][k]
        pos = me['pos'] @ W[:3, :3].T + W[:3, 3]
        nrm = me['nrm'] @ W[:3, :3].T
        tri = me['tri'][:, [0, 2, 1]]
        p = pos[tri]
        tri = tri[np.linalg.norm(np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]), axis=1) > 1e-10]
        out['V'].append(pos[tri]); out['UV'].append(me['uv'][tri]); out['COL'].append(me['col'][tri].astype(np.float32) * 255 / 128)
        out['NRM'].append(nrm[tri]); out['TEX'].append(np.full(len(tri), tex_for_node(nd)))
        out['ATEST'].append(np.ones(len(tri), bool))
    if not out['V']:
        return None
    return {k: np.concatenate(v) for k, v in out.items()}


def gather(meshes, inst):
    """inst: list of (mesh_index, offset (3,)) -> concatenated triangle arrays."""
    acc = dict(V=[], UV=[], COL=[], NRM=[], TEX=[], ATEST=[])
    for mi, off in inst:
        me = meshes[mi]
        if me is None:
            continue
        acc['V'].append(me['V'] + off)
        for k in ('UV', 'COL', 'NRM', 'TEX', 'ATEST'):
            acc[k].append(me[k])
    return {k: np.concatenate(v) for k, v in acc.items()} if acc['V'] else None


def render(stage, pose, W=1210, hfov=63.2, vfov=38.1, tree_range=16000.0, prop_range=60000.0, props=True, trees=True,
           tree_scale=1.0, root=ROOT, forest=0):
    st = Stage(stage, root)
    x, y, z, h, p = pose
    eye = np.array([x, y, z])
    basis = ypr(np.degrees(h), np.degrees(p))            # rows right, up, forward
    tr.camera = lambda e, yaw, pitch: (basis[2], basis[0], basis[1])
    img, info = tr.render(st, eye, 0.0, 0.0, W, hfov, vfov, 64, False)
    H = info['size'][1]
    R = Raster(W, H, hfov, near=1.0, vfov_deg=vfov)
    R.iz = info['iz'].copy()                             # terrain depth (1/forward distance) -> props depth-test against it
    textures, tris, stats = [], [], dict(props=0, trees=0)
    objs = wo.load_stage_objects(stage, root)
    fwd = basis[2]
    if props and len(objs['pdl']):
        tex = pmtex_textures(open(os.path.join(root, stage, 'pmtex%s.bin' % stage), 'rb').read())
        t0 = len(textures); textures += tex
        ents = table(open(os.path.join(root, stage, 'pmmdl%s.bin' % stage), 'rb').read())
        meshes = [acm_tris(e, lambda nd: t0 + int(nd['r3']) if int(nd['r3']) < len(tex) else -1) for e in ents]
        pd = objs['pdl']
        inst = []
        for r in pd:
            o = np.array([r['x'], r['y'], r['z']], float)
            me = meshes[int(r['model'])]
            if me is None:
                continue
            c = me['V'].reshape(-1, 3).mean(0) + o           # model geometry may sit far from the anchor (m07a)
            if np.linalg.norm(c - eye) < prop_range and (c - eye) @ fwd > -2000:
                inst.append((int(r['model']), o))
        stats['props'] = len(inst)
        g = gather(meshes, inst)
        if g:
            tris.append(g)
    if trees and forest > 0:   # WORLD-4: the objects_baker --forest fill (tree type 0 in .wbd forest cells)
        import objects_baker as ob
        wad = open(os.path.join(root, stage, stage + '.wad'), 'rb').read(); wbd = open(os.path.join(root, stage, stage + '.wbd'), 'rb').read()
        fp = ob.forest_points(ob.forest_cells(wbd, len(wad), objs['mta']), forest)
        objs['trees'] = np.concatenate([objs['trees'], np.stack([fp[:, 0], fp[:, 1], np.zeros(len(fp))], 1)])
    if trees and len(objs['trees']):
        tg = objs['trees']
        d = np.hypot(tg[:, 0] - x, tg[:, 1] - z)
        ahead = (tg[:, 0] - x) * fwd[0] + (tg[:, 1] - z) * fwd[2] > -500
        tg = tg[(d < tree_range) & ahead]
        tmesh = []
        for t in range(3):
            a = os.path.join(root, stage, '%s_tree%d.acm' % (stage, t + 1))
            gi = len(textures); textures.append(gim_image(open(a[:-4] + '.gim', 'rb').read()))
            me = acm_tris(open(a, 'rb').read(), lambda nd, gi=gi: gi)
            if me is not None and tree_scale != 1.0:
                me['V'] = me['V'] * tree_scale
            tmesh.append(me)
        ty = st.height_at(tg[:, 0], tg[:, 1])
        inst = [(int(t), np.array([tx, hy, tz])) for (tx, tz, t), hy in zip(tg, ty)]
        stats['trees'] = len(inst)
        g = gather(tmesh, inst)
        if g:
            tris.append(g)
    out = img.astype(np.float32)
    if tris:
        A = {k: np.concatenate([t[k] for t in tris]) for k in tris[0]}
        R.draw(A['V'], A['UV'], A['COL'], A['NRM'], A['TEX'], textures, eye, basis, cull=False, atest=A['ATEST'])
        col, alpha = R.shade(out, light=(0.5, 0.7, 0.5), ambient=0.55, gain=1.0)
        hit = (R.tri >= 0).reshape(H, W)
        dist = 1.0 / np.maximum(R.iz.reshape(H, W), 1e-30)
        fog = np.clip(1 - np.exp(-dist / 90000.0), 0, 0.85)[..., None]           # same fog as the terrain pass
        col = col * (1 - fog) + np.array([200, 212, 222]) * fog
        out = np.where(hit[..., None], col, out)
        stats['prop_pixels'] = int(hit.sum())
    stats['tris'] = int(sum(len(t['V']) for t in tris))
    return np.clip(out, 0, 255).astype(np.uint8), stats


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('out'); ap.add_argument('--stage', default='m02a')
    ap.add_argument('--pose', default=','.join(str(v) for v in POSE56))
    ap.add_argument('--w', type=int, default=1210); ap.add_argument('--hfov', type=float, default=63.2)
    ap.add_argument('--vfov', type=float, default=38.1); ap.add_argument('--shot', default=None)
    ap.add_argument('--tree-range', type=float, default=16000.0); ap.add_argument('--prop-range', type=float, default=60000.0)
    ap.add_argument('--tree-scale', type=float, default=4.5)
    ap.add_argument('--no-trees', action='store_true'); ap.add_argument('--no-props', action='store_true')
    ap.add_argument('--forest', type=int, default=0)
    g = ap.parse_args(argv)
    pose = tuple(float(v) for v in g.pose.split(','))
    img, stats = render(g.stage, pose, g.w, g.hfov, g.vfov, g.tree_range, g.prop_range, not g.no_props, not g.no_trees,
                        g.tree_scale, forest=g.forest)
    if g.shot:
        import terrain_calibrate as tc
        shot = tc.read_png(g.shot)[31:, :, :3]           # drop the window title bar
        shot = tc.resize(shot, img.shape[1], img.shape[0]).astype(np.uint8)
        mix = ((shot.astype(np.int32) + img) // 2).astype(np.uint8)
        img = np.concatenate([shot, img, mix], 1)
    write_png(g.out, img)
    print(g.out, stats)
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
