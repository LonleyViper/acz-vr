"""Offline previews of baked .zvrm models (numpy rasteriser; reads ONLY our baked files).

  python -I model_preview.py <out.png> <a.zvrm>[,<b.zvrm>...] [--eye pvb|x,y,z] [--ypr yaw,pitch,roll]
         [--look x,y,z] [--w 1200 --h 675 --hfov 90 [--vfov 90]] [--cull] [--show-hidden] [--no-glass]
         [--cut-box x0,y0,z0,x1,y1,z1 (drop exterior triangles inside the box: cockpit region)]
         [--bg sky|r,g,b]

Model space = game model space: +X right wing, +Y up, nose at -Z; units = world units (the cockpit eye from
data/mis/mpl/<type>.pvb equals the live E6 camera offset; see docs/world-render/model-decode.md).
"""
import argparse, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from model_baker import read_zvrm, MF_ALPHA_TEST, MF_BLEND, MF_GLASS, MF_HIDDEN
from model_raster import Raster, ypr, look_at
from png_io import write_png


def gather(z, show_hidden=False, glass=True, cut_box=None, tex_base=0):
    V = z['V']; I = z['I'].reshape(-1, 3)
    out = dict(V=[], UV=[], COL=[], NRM=[], TEX=[], AT=[], BL=[])
    for p in z['parts']:
        if not p['ic']:
            continue
        mf = p['mflags']
        if (mf & MF_HIDDEN) and not show_hidden:
            continue
        if (mf & MF_GLASS) and not glass:
            continue
        tri = I[p['fi'] // 3:(p['fi'] + p['ic']) // 3]
        P = V['pos'][tri]
        if cut_box is not None:
            c = P.mean(1)
            inside = np.all((c >= cut_box[:3]) & (c <= cut_box[3:]), 1)
            tri = tri[~inside]; P = P[~inside]
            if not len(tri):
                continue
        out['V'].append(P); out['UV'].append(V['uv'][tri]); out['COL'].append(V['rgba'][tri].astype(np.float32))
        out['NRM'].append(V['nrm'][tri].astype(np.float32) / 32767)
        n = len(tri)
        out['TEX'].append(np.full(n, p['tex'] + tex_base if p['tex'] >= 0 else -1))
        out['AT'].append(np.full(n, bool(mf & MF_ALPHA_TEST)))
        out['BL'].append(np.full(n, bool(mf & MF_BLEND)))
    return {k: (np.concatenate(v) if v else None) for k, v in out.items()}


def sky(W, H, basis, fx, fy):
    yy, xx = np.mgrid[0:H, 0:W]
    d = (((xx + .5 - W / 2) / fx)[..., None] * basis[0] - ((yy + .5 - H / 2) / fy)[..., None] * basis[1] + basis[2])
    el = d[..., 1] / np.linalg.norm(d, axis=-1)
    t = np.clip(el * 2.5, -1, 1)[..., None]
    up = np.array([120, 160, 215.]); hz = np.array([205, 215, 225.]); gd = np.array([110, 120, 95.])
    return np.where(t >= 0, hz * (1 - t) + up * t, hz * (1 + t) - gd * t)


def render(models, eye, basis, W=1200, H=675, hfov=90, vfov=None, cull=False, show_hidden=False, glass=True,
           cut_boxes=None, bg='sky', gain=1.15):
    textures = []; G = []
    for k, z in enumerate(models):
        g = gather(z, show_hidden, glass, cut_boxes[k] if cut_boxes else None, len(textures))
        textures += list(z['textures'])
        if g['V'] is not None:
            G.append(g)
    A = {k: np.concatenate([g[k] for g in G]) for k in G[0]}
    R = Raster(W, H, hfov, near=0.02, vfov_deg=vfov)
    background = sky(W, H, basis, R.fx, R.fy) if bg == 'sky' else np.array(bg, np.float32)
    op = ~A['BL']
    R.draw(A['V'][op], A['UV'][op], A['COL'][op], A['NRM'][op], A['TEX'][op], textures, eye, basis, cull=cull, atest=A['AT'][op])
    img, _ = R.shade(background, gain=gain)
    bl = A['BL']
    if bl.any():   # nearest translucent layer, depth-tested against the opaque buffer
        T = Raster(W, H, hfov, near=0.02, vfov_deg=vfov); T.iz = R.iz.copy()
        T.draw(A['V'][bl], A['UV'][bl], A['COL'][bl], A['NRM'][bl], A['TEX'][bl], textures, eye, basis, cull=False,
               atest=np.zeros(bl.sum(), bool))
        timg, ta = T.shade(np.zeros(3), gain=gain)
        hit = (T.tri >= 0).reshape(H, W)
        ta = np.where(hit, np.clip(ta, 0, 1), 0)[..., None]
        img = img * (1 - ta) + timg * ta
    return np.clip(img, 0, 255).astype(np.uint8), R


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out'); ap.add_argument('models')
    ap.add_argument('--eye', default='pvb'); ap.add_argument('--ypr', default='0,0,0'); ap.add_argument('--look')
    ap.add_argument('--w', type=int, default=1200); ap.add_argument('--h', type=int, default=675)
    ap.add_argument('--hfov', type=float, default=90); ap.add_argument('--vfov', type=float)
    ap.add_argument('--cull', action='store_true'); ap.add_argument('--show-hidden', action='store_true')
    ap.add_argument('--no-glass', action='store_true'); ap.add_argument('--cut-box'); ap.add_argument('--bg', default='sky')
    a = ap.parse_args()
    models = [read_zvrm(p) for p in a.models.split(',')]
    if a.eye == 'pvb':
        eye = next(np.array(z['eye'], float) for z in models if z['eye'] is not None)
    else:
        eye = np.array([float(v) for v in a.eye.split(',')])
    if a.look:
        _, basis = look_at(eye, [float(v) for v in a.look.split(',')])
    else:
        basis = ypr(*[float(v) for v in a.ypr.split(',')])
    cut = None
    if a.cut_box:   # applies to every model except the first (the cockpit)
        b = np.array([float(v) for v in a.cut_box.split(',')])
        cut = [None] + [b] * (len(models) - 1)
    bg = 'sky' if a.bg == 'sky' else [float(v) for v in a.bg.split(',')]
    img, R = render(models, eye, basis, a.w, a.h, a.hfov, a.vfov, a.cull, a.show_hidden, not a.no_glass, cut, bg)
    write_png(a.out, img)
    print(a.out, 'eye', np.round(eye, 3), 'covered %.1f%%' % (100 * (R.tri >= 0).mean()))


if __name__ == '__main__':
    main()
