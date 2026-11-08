"""PROPS-SHADING (docs/world-render/props-shading.md): offline probes behind the "ground objects have no shaders" finding (headset test 17).
numpy + stdlib only, NO network; reads extracted game data / logs, writes only to D:\\acz-extract (nothing game-derived is committed).

  python -I props_shading_probe.py mispack [--pack D:/acz-extract/mis-pack/m02a/m02ald.bin] [--out D:/acz-extract/out/props-shading/mispack_m02a_sheet.png]
      contact sheet of the 28 mission-pack unit models (first = highest-detail ACM of each package, its first GIM): which objects are NOT .pdl props
  python -I props_shading_probe.py records <step5-producer.log>
      decode the logged DrawACM SrtData records (acm-record-hex lines): model position, VS lighting enable +352.x, light matrix +128..+160, light
      colours +192..+240, fog k0/k1 (+336/+340), fog colour (+368) -- the game's own shading inputs per drawn object
  python -I props_shading_probe.py poses [--zvrs D:/acz-extract/stage-w2/m02a.zvrs]
      close-up fixed-camera recipes (cam:x,y,z,yaw,pitch) for m02a props, with the terrain clearance of each sight line
"""
import argparse, math, os, re, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import numpy as np


def mispack(pack, out):
    from model_baker import table, gim_image
    from acm_lib import decode_acm
    from model_raster import Raster, look_at
    from png_io import write_png
    pk = table(open(pack, 'rb').read())
    tiles = []
    for i, p in enumerate(pk):
        s = table(p)
        acm = next(x for x in s[1:9] if x[:4] == b'ACM\0')
        tex = []
        for g in (x for x in s[9:] if x[:4] == b'GIM\0'):
            try:
                tex.append(gim_image(g))
            except Exception:
                tex.append(np.full((4, 4, 4), 255, np.uint8))
        m = decode_acm(acm)
        V, UV, COL, NRM, TX = [], [], [], [], []
        for k, nd in enumerate(m['nodes']):
            if not (0 <= nd['mesh'] < len(m['meshes'])) or not len(m['meshes'][nd['mesh']]['tri']):
                continue
            me = m['meshes'][nd['mesh']]; W = m['world'][k]
            pos = me['pos'] @ W[:3, :3].T + W[:3, 3]; nrm = me['nrm'] @ W[:3, :3].T
            tri = me['tri'][:, [0, 2, 1]]
            V.append(pos[tri]); UV.append(me['uv'][tri]); COL.append(me['col'][tri].astype(np.float32) * 255 / 128); NRM.append(nrm[tri])
            TX.append(np.full(len(tri), 0 if tex else -1))
        V = np.concatenate(V); lo, hi = V.reshape(-1, 3).min(0), V.reshape(-1, 3).max(0)
        c = (lo + hi) / 2; r = np.linalg.norm(hi - lo) / 2
        d = np.array([0.7, 0.55, 0.75]); eye = c + r * 1.9 * d / np.linalg.norm(d)
        _, basis = look_at(eye, c)
        R = Raster(320, 240, 45, near=0.1)
        R.draw(V, np.concatenate(UV), np.concatenate(COL), np.concatenate(NRM), np.concatenate(TX), tex, eye, basis, cull=False)
        img, _ = R.shade(np.array([110, 130, 120.0]))
        tiles.append(np.clip(img, 0, 255).astype(np.uint8))
        print('package %2d extent %s nodes %d textures %s' % (i, np.round(hi - lo, 1), len(m['nodes']), [t.shape[:2] for t in tex]))
    while len(tiles) % 7:
        tiles.append(np.zeros_like(tiles[0]))
    rows = [np.concatenate(tiles[k:k + 7], 1) for k in range(0, len(tiles), 7)]
    os.makedirs(os.path.dirname(out), exist_ok=True)
    write_png(out, np.concatenate(rows, 0)); print('wrote', out)


def records(log):
    recs, meta = {}, {}
    for line in open(log, encoding='utf-8', errors='replace'):
        m = re.search(r'event=acm-record-hex;win=(\d+);rec=(\d+);part=(\d)/4;bytes=\d+-\d+;hex=([0-9a-f]+)', line)
        if m:
            recs.setdefault((int(m[1]), int(m[2])), {})[int(m[3])] = bytes.fromhex(m[4])
        m = re.search(r'event=acm-record-decision;win=(\d+);rec=(\d+);.*?decision=(\w+);reason=([\w-]+);.*?cb10-dword0=([0-9a-f]+).*?cb10-words=([0-9a-f]+),([0-9a-f]+)', line)
        if m:
            meta[(int(m[1]), int(m[2]))] = (m[3], m[4], m[5], m[7])
    f4 = lambda b, o: struct.unpack_from('<4f', b, o)
    for k in sorted(recs):
        p = recs[k]
        if len(p) != 4:
            continue
        b = p[1] + p[2] + p[3] + p[4]
        print('win %d rec %d decision %s reason %s cb10 %s alpharef %s' % ((k[0], k[1]) + meta.get(k, ('?',) * 4)))
        print('  model position (B row 3) %.1f %.1f %.1f' % f4(b, 304)[:3])
        for o, n in ((128, 'light rows +128'), (144, '         +144'), (160, '         +160'), (192, 'colour 0 +192'), (208, 'colour 1 +208'),
                     (224, 'colour 2 +224'), (240, 'ambient  +240'), (336, 'fog k0,k1 +336'), (352, 'lit,mode,zs +352'), (368, 'fog colour +368')):
            print('  %-17s %s' % (n, ' '.join('%10.5f' % v for v in f4(b, o))))
        k1 = f4(b, 336)[1]
        if k1 < 0:
            print('  fog end (255/-k1) %.1f units' % (255.0 / -k1))


POSES = [   # (name, target (x, y, z), camera offset from the target) -- m02a .pdl placements (world3-impl.md 1.1; pdl index in the name)
    ('house83', (12120.1, 544.2, 132150.5), (420, 160, 300)),
    ('house83-low', (12120.1, 544.2, 132150.5), (250, 40, -180)),
    ('bridge46', (14660.2, 480.7, 13537.7), (1300, 350, -700)),
    ('bridge-row', (14500.0, 480.0, 14500.0), (2600, 500, 600)),
    ('radiotower55', (12082.0, 804.1, 54986.6), (420, 60, 380)),
    ('watertower58', (9980.2, 690.7, 54890.7), (300, 60, 260)),
]


def poses(zvrs):
    import objects_baker as ob
    _, H, cell, hstep, half, _ = ob.read_zvrs_ground(open(zvrs, 'rb').read())
    h = lambda x, z: float(ob.height_bilinear(H, cell, hstep, half, np.array([x]), np.array([z]))[0])
    for name, t, o in POSES:
        c = (t[0] + o[0], t[1] + o[1], t[2] + o[2])
        dx, dy, dz = t[0] - c[0], t[1] - c[1], t[2] - c[2]
        yaw = math.degrees(math.atan2(-dx, -dz))          # camera_frame: forward = (-sin h cos p, sin p, -cos h cos p)
        pitch = math.degrees(math.atan2(dy, math.hypot(dx, dz)))
        clear = min(c[1] + (t[1] - c[1]) * s - h(c[0] + dx * s, c[2] + dz * s) for s in np.linspace(0, 0.97, 60))
        print('%-14s cam:%.0f,%.0f,%.0f,%.1f,%.1f  agl %.0f, sight-line clearance %.0f, distance %.0f'
              % (name, c[0], c[1], c[2], yaw, pitch, c[1] - h(c[0], c[2]), clear, math.dist(c, t)))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=('mispack', 'records', 'poses')); ap.add_argument('arg', nargs='?')
    ap.add_argument('--pack', default='D:/acz-extract/mis-pack/m02a/m02ald.bin')
    ap.add_argument('--out', default='D:/acz-extract/out/props-shading/mispack_m02a_sheet.png')
    ap.add_argument('--zvrs', default='D:/acz-extract/stage-w2/m02a.zvrs')
    a = ap.parse_args(argv)
    if a.cmd == 'mispack':
        mispack(a.pack, a.out)
    elif a.cmd == 'records':
        records(a.arg)
    else:
        poses(a.zvrs)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
