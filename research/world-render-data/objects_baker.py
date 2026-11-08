"""Stage objects baker (WORLD-3): props (.pdl -> pmmdl models) + trees (.wad/.wpd -> <stage>_treeN) -> <stage>.zvro, the companion of
<stage>.zvrs for the XR prop/tree pass (src/acz-vr/world_props.{h,cpp}).  Offline, numpy + stdlib only, NO network; game data is never written to
the repository.

  python -I objects_baker.py <stage> [--src <dir containing <stage>/...>] [--arc <a.arc>] [--out <dir>] [--forest <trees per forest cell>]
                                     [--stage <the stage's .zvrs: bake the snapped v2 file>]
  python -I objects_baker.py --selftest

Default --out is %LOCALAPPDATA%\\acz-vr\\stage (next to the .zvrs).  Decode evidence: docs/world-render/world3-impl.md, world_objects.py.

.zvro v1 (little endian)
  header 128 bytes
    0   "ZVRO"           4  u32 version = 1        8  char[8] stage id (NUL padded)
    16  u32 flags (0)    20 u32 nmodels           24 u32 ntex          28 u32 texsize (128: every texture resampled, nearest, to texsize^2)
    32  u32 nverts       36 u32 nidx              40 u32 ninst
    44  u32 off_models   48 u32 off_tex           52 u32 off_verts     56 u32 off_idx     60 u32 off_inst     64 u32 file_size
    68..95 zero          96 byte[32] SHA-256 of bytes [128, file_size)
  models   32 B each: u32 first_index, u32 index_count, u32 base_vertex, u32 kind (0 prop, 1 tree), f32 centre[3], f32 radius (bound sphere, model space)
  textures ntex * texsize * texsize * 4  RGBA8, straight alpha (PS2 0x80 -> 255); alpha < 128 = cut-out (alpha test 0.5)
  vertices 32 B: f32 pos[3] (model space: world = instance position + scale * pos; NO rotation, it is baked into each prop model), f32 uv[2],
           i16 nrm[3] (snorm), u16 texture layer, u8 rgba[4] (vertex colour, 0x80 -> 255)
  indices  u32, relative to the model's base_vertex; CCW-front triangle lists (strips converted; degenerate triangles dropped)
  instances 24 B: f32 x, y, z, f32 scale, u32 model, u32 flags (bit0 = y is relative to the terrain: the runtime adds height_bilinear(x, z))
           props: (.pdl x, y, z), scale 1, flags 0 (models without geometry are skipped); trees: (x, 0, z), scale 1, flags 1, model = nprops + type

PERF --stage <path to the stage's .zvrs> (docs/world-render/perf-impl.md): .zvro v2 = v1 with header flags bit0 = 1 (SNAPPED), bytes 68..83 =
the first 16 bytes of that .zvrs payload sha256 (84..95 zero), and every ground-relative instance (flags bit0) replaced by its absolute y =
y + height_bilinear(x, z) of that stage (float32, the C++ terrain_stage.cpp arithmetic), instance flags 0.  The runtime then does no CPU snapping and
accepts the file ONLY with that exact .zvrs bake (otherwise: props-disabled reason=snapped-against-another-stage-bake).  Without --stage: v1.

WORLD-4 --forest N (default 0 = the WORLD-3 file byte for byte): the .wbd value-1 cells (512 units) are FOREST: 96% of their texels are the dark
forest colour (mean CLUT luminance 29.8 vs 73.2 elsewhere) and they hold NONE of the 174,302 listed m02a trees, while the game window shows trees
standing inside them (docs/world-render/world4-impl.md). The game's fill rule is not decoded; this fills each forest cell with N tree-1 instances
at deterministic hashed positions (FITTED density: N = 3 matches the step56 window within a factor ~2), appended after the listed trees.
"""
import hashlib, os, struct, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import world_objects as wo

MAGIC = b'ZVRO'
VERSION = 1               # written without --stage (byte for byte the WORLD-3/4 file)
VERSION_SNAPPED = 2       # PERF: written with --stage: flag bit0, every y absolute (no runtime ground snap); see snap_to_stage()
FLAG_SNAPPED = 1
STAGE_SHA_BYTES = 16
HEADER = 128
TEXSIZE = 128
VDT = np.dtype([('pos', '<f4', 3), ('uv', '<f4', 2), ('nrm', '<i2', 3), ('layer', '<u2'), ('rgba', 'u1', 4)])
IDT = np.dtype([('x', '<f4'), ('y', '<f4'), ('z', '<f4'), ('scale', '<f4'), ('model', '<u4'), ('flags', '<u4')])
assert VDT.itemsize == 32 and IDT.itemsize == 24
DUMMY = b'dummy'          # placeholder content of absent stage files in the archive
DEFAULT_ARC = r'D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero\a.arc'


def names(stage):
    n = ['%s.%s' % (stage, e) for e in ('pdl', 'wad', 'wpd', 'mta')]
    n += ['pmmdl%s.bin' % stage, 'pmtex%s.bin' % stage]
    n += ['%s_tree%d.%s' % (stage, k, e) for k in (1, 2, 3) for e in ('acm', 'gim')]
    return n


def load_from_dir(stage, root):
    out = {n: open(os.path.join(root, stage, n), 'rb').read() for n in names(stage)}
    w = os.path.join(root, stage, stage + '.wbd')          # WORLD-4 forest mask: optional (only --forest reads it)
    if os.path.exists(w):
        out[stage + '.wbd'] = open(w, 'rb').read()
    return out


def load_from_arc(stage, arc_path):
    import arcread
    a = arcread.Arc(arc_path)
    ver, es = arcread.entries(a)
    a.ver = ver
    by = {e[0]: e for e in es}
    out = {}
    for n in names(stage):
        full = 'data/map/%s/%s' % (stage, n)
        if full not in by:
            raise SystemExit('missing in archive: ' + full)
        out[n] = arcread.extract_bytes(a, by[full])
    wf = 'data/map/%s/%s.wbd' % (stage, stage)              # WORLD-4 forest mask: optional
    if wf in by:
        out[stage + '.wbd'] = arcread.extract_bytes(a, by[wf])
    return out


def resample(rgba, size=TEXSIZE):
    h, w = rgba.shape[:2]
    ys = (np.arange(size) * h // size); xs = (np.arange(size) * w // size)
    return np.ascontiguousarray(rgba[ys][:, xs])


def mesh_of(acm, layer_for_node):
    """ACM -> (vertices VDT, local u32 indices) in model space, CCW front."""
    from acm_lib import decode_acm
    m = decode_acm(acm)
    V, I, base = [], [], 0
    for k, nd in enumerate(m['nodes']):
        if not (0 <= nd['mesh'] < len(m['meshes'])):
            continue
        me = m['meshes'][nd['mesh']]
        if not len(me['tri']):
            continue
        W = m['world'][k]
        pos = me['pos'] @ W[:3, :3].T + W[:3, 3]
        nrm = me['nrm'] @ W[:3, :3].T
        nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-6)
        tri = me['tri'][:, [0, 2, 1]]
        p = pos[tri]
        tri = tri[np.linalg.norm(np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]), axis=1) > 1e-10]
        v = np.zeros(len(pos), VDT)
        v['pos'] = pos; v['uv'] = me['uv']
        v['nrm'] = np.clip(np.round(nrm * 32767), -32767, 32767)
        v['layer'] = layer_for_node(nd)
        v['rgba'] = np.minimum(me['col'].astype(np.int32) * 255 // 128, 255)
        V.append(v); I.append((tri + base).astype(np.uint32).ravel()); base += len(pos)
    if not V:
        return np.zeros(0, VDT), np.zeros(0, np.uint32)
    return np.concatenate(V), np.concatenate(I)


def forest_cells(wbd, wad_len, mta):
    """-> (M, 2) world (x0, z0) corners of the 512-unit .wbd value-1 (forest) cells over the whole stage."""
    nb = wad_len // wo.WAD_BLOCK
    if len(wbd) < nb * 1024:
        raise ValueError('wbd shorter than %d blocks' % nb)
    v = wo.read_wbd(wbd, nb)
    out = []
    for R in range(16):
        for C in range(16):
            b = int(mta[R, C])
            if b >= nb:
                continue
            j, i = np.nonzero(v[b] == 1)                      # [row = z cell, col = x cell] inside the 32768-unit block
            out.append(np.stack([-wo.HALF + C * wo.BLOCK + i * 512.0, -wo.HALF + R * wo.BLOCK + j * 512.0], 1))
    return np.concatenate(out) if out else np.zeros((0, 2))


def forest_points(cells, n):
    """n deterministic positions per cell (integer hash of the cell corner and k; inset 32 units), 16-unit grid."""
    if n <= 0 or not len(cells):
        return np.zeros((0, 2))
    cx = ((cells[:, 0] + wo.HALF) // 512).astype(np.uint64); cz = ((cells[:, 1] + wo.HALF) // 512).astype(np.uint64)
    pts = []
    for k in range(n):
        h = (cx * np.uint64(73856093)) ^ (cz * np.uint64(19349663)) ^ np.uint64((k + 1) * 83492791)
        h = (h ^ (h >> np.uint64(13))) * np.uint64(0x5bd1e995) & np.uint64(0xFFFFFFFF)
        h = h ^ (h >> np.uint64(15))
        u = (h & np.uint64(0xFFFF)).astype(np.float64) / 65536.0; w = ((h >> np.uint64(16)) & np.uint64(0xFFFF)).astype(np.float64) / 65536.0
        pts.append(np.stack([cells[:, 0] + 32 + np.floor(u * 28) * 16, cells[:, 1] + 32 + np.floor(w * 28) * 16], 1))
    return np.concatenate(pts)


HDIM = 513


def read_zvrs_ground(blob):
    """The parts of a .zvrs the ground snap needs: (stage id, heights u8 (513, 513) [row = z, col = x], cell, hstep, half, payload sha256).
    Checks magic, size and the payload hash (the same bytes the C++ loader hashes)."""
    if len(blob) < 256 or blob[:4] != b'ZVRS':
        raise ValueError('not a .zvrs')
    hdim, = struct.unpack_from('<I', blob, 20)
    cell, hstep, half = struct.unpack_from('<3f', blob, 40)
    off_h, = struct.unpack_from('<I', blob, 56)
    size, = struct.unpack_from('<I', blob, 76)
    if hdim != HDIM or size != len(blob) or off_h + HDIM * HDIM > len(blob):
        raise ValueError('zvrs header')
    sha = hashlib.sha256(blob[256:]).digest()
    if sha != blob[192:224]:
        raise ValueError('zvrs payload hash')
    heights = np.frombuffer(blob, np.uint8, HDIM * HDIM, off_h).reshape(HDIM, HDIM)
    return blob[8:16].rstrip(b'\0').decode(), heights, np.float32(cell), np.float32(hstep), np.float32(half), sha


def height_bilinear(heights, cell, hstep, half, x, z):
    """Vectorised float32 copy of terrain_stage.cpp height_bilinear (same operation order: lerp a + (b - a) * t)."""
    f = np.float32
    x = np.asarray(x, np.float32); z = np.asarray(z, np.float32)
    u = np.clip((x + f(half)) / f(cell), f(0), f(512)).astype(np.float32)
    v = np.clip((z + f(half)) / f(cell), f(0), f(512)).astype(np.float32)
    i = np.minimum(np.floor(u).astype(np.int64), 511); j = np.minimum(np.floor(v).astype(np.int64), 511)
    fu = (u - i.astype(np.float32)).astype(np.float32); fv = (v - j.astype(np.float32)).astype(np.float32)
    h = heights.astype(np.float32)
    h00, h10, h01, h11 = h[j, i], h[j, i + 1], h[j + 1, i], h[j + 1, i + 1]
    a = (h00 + (h10 - h00) * fu).astype(np.float32); b = (h01 + (h11 - h01) * fu).astype(np.float32)
    return ((a + (b - a) * fv).astype(np.float32) * f(hstep)).astype(np.float32)


def snap_to_stage(inst, ground):
    """PERF: ground-relative instances (flags bit0) -> absolute y (y + height_bilinear), flags 0.  Returns a new IDT array."""
    out = inst.copy()
    rel = (out['flags'] & 1) == 1
    if rel.any():
        _, heights, cell, hstep, half, _ = ground
        out['y'][rel] = (out['y'][rel] + height_bilinear(heights, cell, hstep, half, out['x'][rel], out['z'][rel])).astype(np.float32)
        out['flags'][rel] = out['flags'][rel] & ~np.uint32(1)
    return out


def assemble(stage, src, texsize=TEXSIZE, forest=0, ground=None):
    """-> (bytes, info).  `src` maps the names() file names to bytes.  `ground` = read_zvrs_ground(<stage>.zvrs) bakes the snapped v2 file (PERF)."""
    from model_baker import table, pmtex_textures, gim_image
    # some stages (v01a..v08a, ...) ship 5-byte placeholder files containing the ASCII text 'dummy' instead of .wad/.wpd/.wbd/tree data:
    # treat them as absent (no trees / no tree model) instead of failing the whole stage (bake_all_stages.py, docs/world-render/stage-table.md)
    src = {k: (b'' if v == DUMMY else v) for k, v in src.items()}
    pdl = wo.read_pdl(src['%s.pdl' % stage])
    mta = np.frombuffer(src['%s.mta' % stage], np.uint8)
    if mta.size != 256:
        raise ValueError('mta size')
    trees = wo.read_trees(src['%s.wad' % stage], src['%s.wpd' % stage], mta.reshape(16, 16))
    textures, models, Vs, Is = [], [], [], []
    nv = ni = 0

    def add_model(v, i, kind):
        nonlocal nv, ni
        if len(v):
            c = (v['pos'].min(0) + v['pos'].max(0)) / 2
            r = float(np.linalg.norm(v['pos'] - c, axis=1).max())
        else:
            c, r = np.zeros(3), 0.0
        models.append((ni, len(i), nv, kind, float(c[0]), float(c[1]), float(c[2]), r))
        Vs.append(v); Is.append(i); nv += len(v); ni += len(i)

    nprops = 0
    pm = src.get('pmmdl%s.bin' % stage, b'')
    if len(pdl) and len(pm) > 16:
        ents = table(pm)
        tex = pmtex_textures(src['pmtex%s.bin' % stage])
        t0 = len(textures); textures += [resample(t, texsize) for t in tex]
        for e in ents:
            v, i = mesh_of(e, lambda nd: t0 + int(nd['r3']) if int(nd['r3']) < len(tex) else t0)
            add_model(v, i, 0)
        nprops = len(ents)
        if int(pdl['model'].max()) >= nprops:
            raise ValueError('pdl model id beyond pmmdl')
    tree_models, tree_errors = [], []
    for k in (1, 2, 3):
        acm, gim = src.get('%s_tree%d.acm' % (stage, k)), src.get('%s_tree%d.gim' % (stage, k))
        if not acm or not gim:
            tree_models.append(-1); continue
        try:
            img = gim_image(gim)
            v, i = mesh_of(acm, lambda nd, layer=len(textures): layer)
        except Exception as e:            # e.g. v08a ships ACM v3 trees (decoder handles v5 only): skip this tree type, keep the stage
            tree_errors.append('tree%d: %s' % (k, e)); tree_models.append(-1); continue
        textures.append(resample(img, texsize))
        tree_models.append(len(models)); add_model(v, i, 1)
    inst = []
    for r in pdl:
        mi = int(r['model'])
        if models[mi][1] == 0:
            continue                      # locator-only model: nothing to draw
        inst.append((float(r['x']), float(r['y']), float(r['z']), 1.0, mi, 0))
    nt_skipped = 0
    for x, z, t in trees:
        mi = tree_models[int(t)]
        if mi < 0 or models[mi][1] == 0:
            nt_skipped += 1; continue
        inst.append((float(x), 0.0, float(z), 1.0, mi, 1))
    nforest = 0
    mi = tree_models[0] if tree_models else -1
    wbd = src.get('%s.wbd' % stage, b'')
    if forest > 0 and mi >= 0 and models[mi][1] > 0 and len(wbd):   # stages without the tree-1 model or the mask: nothing to fill
        fp = forest_points(forest_cells(wbd, len(src['%s.wad' % stage]), mta.reshape(16, 16)), forest)
        inst += [(float(x), 0.0, float(z), 1.0, mi, 1) for x, z in fp]
        nforest = len(fp)
    I_arr = np.array(inst, IDT) if inst else np.zeros(0, IDT)
    ntrees = int((I_arr['flags'] == 1).sum())
    if ground is not None:
        if ground[0] != stage:
            raise ValueError('--stage is the .zvrs of %s, not %s' % (ground[0], stage))
        I_arr = snap_to_stage(I_arr, ground)
    V = np.concatenate(Vs) if Vs else np.zeros(0, VDT)
    I = np.concatenate(Is) if Is else np.zeros(0, np.uint32)
    mod = b''.join(struct.pack('<4I4f', *m) for m in models)
    tex = b''.join(t.tobytes() for t in textures)
    off_models = HEADER
    off_tex = off_models + len(mod)
    off_verts = off_tex + len(tex)
    off_idx = off_verts + V.nbytes
    off_inst = off_idx + I.nbytes
    size = off_inst + I_arr.nbytes
    body = mod + tex + V.tobytes() + I.tobytes() + I_arr.tobytes()
    snapped = ground is not None
    hdr = struct.pack('<4sI8sIIIIIII6II', MAGIC, VERSION_SNAPPED if snapped else VERSION, stage.encode()[:8].ljust(8, b'\0'),
                      FLAG_SNAPPED if snapped else 0, len(models), len(textures), texsize,
                      len(V), len(I), len(I_arr), off_models, off_tex, off_verts, off_idx, off_inst, size, 0)
    hdr = hdr[:68] + (ground[5][:STAGE_SHA_BYTES] if snapped else b'')
    hdr = hdr.ljust(96, b'\0') + hashlib.sha256(body).digest()
    assert len(hdr) == HEADER
    info = dict(models=len(models), props=len(I_arr) - ntrees, trees=ntrees,
                textures=len(textures), vertices=len(V), triangles=len(I) // 3, bytes=size, trees_skipped=nt_skipped, tree_errors=tree_errors,
                forest_trees=nforest, snapped=snapped)
    return hdr + body, info


def read(blob):
    """Parse + check (same rules as the C++ loader, roughly). -> dict.  Raises ValueError."""
    if len(blob) < HEADER or blob[:4] != MAGIC:
        raise ValueError('magic')
    (ver,) = struct.unpack_from('<I', blob, 4)
    f = struct.unpack_from('<IIIIIIIIIIIIII', blob, 16)
    flags, nm, nt, ts, nv, ni, ninst, om, ot, ov, oi, oinst, size, z = f
    if ver not in (VERSION, VERSION_SNAPPED) or size != len(blob):
        raise ValueError('version/size')
    if (ver == VERSION and any(blob[68:96])) or (ver == VERSION_SNAPPED and (flags & ~FLAG_SNAPPED or any(blob[68 + (STAGE_SHA_BYTES if flags else 0):96]))):
        raise ValueError('header flags/reserved')
    if hashlib.sha256(blob[HEADER:]).digest() != blob[96:128]:
        raise ValueError('payload hash')
    models = [struct.unpack_from('<4I4f', blob, om + 32 * k) for k in range(nm)]
    inst = np.frombuffer(blob, IDT, ninst, oinst)
    if ver == VERSION_SNAPPED and flags and (inst['flags'] & 1).any():
        raise ValueError('ground-relative instance in a snapped file')
    return dict(stage=blob[8:16].rstrip(b'\0').decode(), models=models, texsize=ts, version=ver, snapped=bool(ver == VERSION_SNAPPED and flags),
                stage_sha_prefix=blob[68:68 + STAGE_SHA_BYTES] if ver == VERSION_SNAPPED and flags else b'',
                textures=np.frombuffer(blob, np.uint8, nt * ts * ts * 4, ot).reshape(nt, ts, ts, 4),
                V=np.frombuffer(blob, VDT, nv, ov), I=np.frombuffer(blob, '<u4', ni, oi), inst=inst)


def selftest():
    """Synthetic: no game data.  Two props (one locator-only model), three tree types (the third missing), round trip + corruption."""
    import struct as st
    from model_baker import VDT as _  # noqa: F401  (import check)
    # minimal ACM: one node, one mesh = one strip of 4 vertices (a quad)
    def acm(quad_y=0.0, r3=0):
        nodes = st.pack('<IIiIIiii3ff3ff', 8, 0, 0, r3, 0, -1, -1, -1, 0, 0, 0, 1.0, 0, 0, 0, 0.0)
        pos = [(-1, quad_y, -1), (1, quad_y, -1), (-1, quad_y, 1), (1, quad_y, 1)]
        vif = st.pack('<I', (0x01 << 24) | 0x0105) + st.pack('<I', (0x64 << 24) | (1 << 16)) + st.pack('<2I', 4, 1)
        vif += st.pack('<I', (0x68 << 24) | (4 << 16) | 1) + b''.join(st.pack('<3f', *p) for p in pos)
        vif += st.pack('<I', (0x69 << 24) | (4 << 16) | 2) + b''.join(st.pack('<3h', 0, 4096, 0) for _ in pos)
        vif += st.pack('<I', (0x6e << 24) | (4 << 16) | 3) + bytes([0x80] * 16)
        vif += st.pack('<I', (0x64 << 24) | (4 << 16) | 4) + b''.join(st.pack('<2f', (p[0] + 1) / 2, (p[2] + 1) / 2) for p in pos)
        vif += st.pack('<I', 0x17 << 24)
        vif += b'\0' * (-len(vif) % 16)
        qwc = len(vif) // 16
        mesh = st.pack('<4I', (6 << 28) | qwc, 0, 0, 0) + vif      # DMA RET tag (16 bytes incl. two VIF NOPs)
        mt = 80 + 64
        base = (mt + 4 + 15) & ~15
        hdr = b'ACM\0' + st.pack('<I2f4I', 5, 1.5, 0.0, 1, 80, 1, mt) + st.pack('<12I', *([0] * 12))
        body = hdr + nodes + st.pack('<I', 0)
        body += b'\0' * (base - len(body))
        return body + mesh
    def gim(w, h, rgba):
        b = b'GIM\0' + b'\0' * 15 + bytes([0x00]) + b'\0' * 8 + st.pack('<HH', w, h)
        a = np.array(rgba, np.uint8).reshape(1, 1, 4).repeat(h, 0).repeat(w, 1).copy(); a[..., 3] = 0x80
        return b + a.tobytes()
    def container(items):
        n = len(items); offs = []; o = 4 + 4 * n; o = (o + 15) & ~15; body = b''
        for it in items:
            offs.append(o + len(body)); body += it; body += b'\0' * (-len(body) % 16)
        head = st.pack('<I%dI' % n, n, *offs); head += b'\0' * ((4 + 4 * n + 15 & ~15) - len(head))
        return head + body
    loc = b'ACM\0' + st.pack('<I2f4I', 5, 0.0, 0.0, 1, 80, 0, 144) + st.pack('<12I', *([0] * 12)) + st.pack('<IIiIIiii3ff3ff', 8, 0, -1, 0, 0, -1, -1, -1, 0, 0, 0, 1.0, 0, 0, 0, 0.0)
    clut = b'GIM\0' + b'\0' * 15 + bytes([0x00]) + b'\0' * 8 + st.pack('<HH', 16, 16) + bytes([10, 20, 30, 0x80] * 256)
    tex8 = b'GIM\0' + b'\0' * 15 + bytes([0x13]) + b'\0' * 8 + st.pack('<HH', 4, 4) + bytes(16)
    pmtex = container([clut, container([tex8])])
    pdl = st.pack('<I12x', 3) + st.pack('<fffI', 100.0, 50.0, -200.0, 0) + st.pack('<fffI', 0.0, 0.0, 0.0, 1) + st.pack('<fffI', 7.0, 8.0, 9.0, 0)
    wad = bytearray(st.pack('<48I48H', *([0xFFFFFFFF] * 48), *([0] * 48)))
    st.pack_into('<I', wad, 0, 0); st.pack_into('<H', wad, 192, 2)            # type 0, sub 0: 2 trees
    st.pack_into('<I', wad, 64, 2); st.pack_into('<H', wad, 192 + 32, 1)      # type 1, sub 0: 1 tree
    src = {'synth.pdl': pdl, 'synth.wad': bytes(wad), 'synth.wpd': st.pack('<3H', 0x0102, 0x0304, 0x0506),
           'synth.mta': bytes(256), 'pmmdlsynth.bin': container([acm(), loc]), 'pmtexsynth.bin': pmtex,
           'synth_tree1.acm': acm(2.0), 'synth_tree1.gim': gim(4, 8, (0, 200, 0, 0)), 'synth_tree2.acm': acm(3.0),
           'synth_tree2.gim': gim(2, 2, (0, 100, 0, 0)), 'synth_tree3.acm': b'', 'synth_tree3.gim': b''}
    b1, info = assemble('synth', src, 8)
    b2, _ = assemble('synth', src, 8)
    assert b1 == b2, 'not deterministic'
    r = read(b1)
    assert info['props'] == 2 and info['trees'] == 3 * 256 and len(r['models']) == 4, info      # 2 pmmdl + 2 trees; locator skipped
    assert r['models'][1][1] == 0 and r['models'][0][1] == 6 and r['models'][2][3] == 1, r['models']
    assert r['textures'].shape == (3, 8, 8, 4)
    i0 = r['inst'][0]
    assert (float(i0['x']), float(i0['y']), float(i0['z']), int(i0['model']), int(i0['flags'])) == (100.0, 50.0, -200.0, 0, 0)
    t = r['inst'][r['inst']['flags'] == 1]
    assert set(t['model'].tolist()) == {2, 3}
    x0 = -262144 + 0x02 * 32 + 16; z0 = -262144 + 0x01 * 32 + 16
    assert any(abs(float(a['x']) - x0) < 1e-3 and abs(float(a['z']) - z0) < 1e-3 for a in t[:3]), t[:3]
    # WORLD-4 forest fill: block 0 everywhere (mta zero); cells (x 1, z 2) and (x 5, z 0) of block 0 are forest -> 2 cells x 256 blocks x n
    wbd = bytearray(1024); wbd[2 * 16 + 0] = 1 << 2; wbd[0 * 16 + 1] = 1 << 2      # 2 bits per cell, low bits first: byte = row*16 + col//4
    src2 = dict(src, **{'synth.wbd': bytes(wbd)})
    b3, info3 = assemble('synth', src2, 8, forest=3)
    assert info3['forest_trees'] == 2 * 256 * 3 and info3['trees'] == 3 * 256 + 2 * 256 * 3, info3
    r3 = read(b3); f = r3['inst'][len(r['inst']):]
    assert (f['model'] == 2).all() and (f['flags'] == 1).all()
    cxs = ((f['x'] + 262144) // 512) % 64; czs = ((f['z'] + 262144) // 512) % 64
    assert set(zip(cxs.tolist(), czs.tolist())) == {(1.0, 2.0), (5.0, 0.0)}, set(zip(cxs.tolist(), czs.tolist()))
    assert assemble('synth', src2, 8)[0] == b1, 'forest 0 must be the WORLD-3 file'
    bad = bytearray(b1); bad[HEADER + 3] ^= 1
    try:
        read(bytes(bad)); raise AssertionError('corruption not detected')
    except ValueError:
        pass
    # 'dummy' placeholder files (v01a style): no trees, props unchanged, no failure
    srcd = dict(src, **{n: DUMMY for n in ('synth.wad', 'synth.wpd', 'synth_tree1.acm', 'synth_tree1.gim', 'synth_tree2.acm', 'synth_tree2.gim')})
    bd, infod = assemble('synth', srcd, 8)
    read(bd)
    assert infod['trees'] == 0 and infod['props'] == 2, infod
    # an undecodable tree model (ACM v3, as in v08a) skips that tree type only
    srcv = dict(src, **{'synth_tree1.acm': b'ACM\0' + st.pack('<I', 3) + bytes(120)})
    bv, infov = assemble('synth', srcv, 8)
    read(bv)
    assert infov['trees'] == 256 and infov['trees_skipped'] == 512 and len(infov['tree_errors']) == 1, infov
    # PERF --stage: snapped v2 file (synthetic ground; heights rise with x and z so every tree gets a distinct y)
    hh = ((np.arange(HDIM)[None, :] // 7 + np.arange(HDIM)[:, None] // 11) % 41).astype(np.uint8)
    ground = ('synth', hh, np.float32(1024.0), np.float32(80.0), np.float32(262144.0), hashlib.sha256(b'synthetic zvrs').digest())
    bs, infos = assemble('synth', src, 8, ground=ground)
    rs = read(bs)
    assert rs['version'] == 2 and rs['snapped'] and rs['stage_sha_prefix'] == ground[5][:16] and infos['snapped'], rs['version']
    assert infos['trees'] == info['trees'] and infos['props'] == info['props'] and (rs['inst']['flags'] == 0).all()
    assert bs[16:20] == struct.pack('<I', 1) and bs[84:96] == bytes(12)
    tr = r['inst']['flags'] == 1
    assert (rs['inst']['y'][~tr] == r['inst']['y'][~tr]).all(), 'absolute props untouched'
    for a, b in zip(r['inst'][tr][:50], rs['inst'][tr][:50]):      # scalar float32 reference of the C++ formula
        f = np.float32
        u = min(max((f(a['x']) + f(262144.0)) / f(1024.0), f(0)), f(512)); v = min(max((f(a['z']) + f(262144.0)) / f(1024.0), f(0)), f(512))
        i = min(int(np.floor(u)), 511); j = min(int(np.floor(v)), 511); fu = f(u - f(i)); fv = f(v - f(j))
        H = hh.astype(np.float32)
        l0 = f(H[j, i] + (H[j, i + 1] - H[j, i]) * fu); l1 = f(H[j + 1, i] + (H[j + 1, i + 1] - H[j + 1, i]) * fu)
        assert f(b['y']) == f(f(a['y']) + f(f(l0 + (l1 - l0) * fv) * f(80.0))), (a, b)
    try:
        assemble('other', src, 8, ground=ground); raise AssertionError('stage mismatch not refused')
    except (ValueError, KeyError):
        pass
    bad = bytearray(bs); bad[90] = 1
    bad[96:128] = hashlib.sha256(bytes(bad[HEADER:])).digest()
    try:
        read(bytes(bad)); raise AssertionError('reserved bytes not refused')
    except ValueError:
        pass
    assert assemble('synth', src, 8)[0] == b1, 'no --stage: v1 byte for byte'
    print('selftest ok', info)
    return b1


def main(argv):
    if '--selftest' in argv:
        selftest(); return 0
    opt, flat, i = {}, [], 0
    while i < len(argv):
        if argv[i].startswith('--') and i + 1 < len(argv):
            opt[argv[i][2:]] = argv[i + 1]; i += 2
        else:
            flat.append(argv[i]); i += 1
    if not flat:
        print(__doc__); return 2
    stage = flat[0]
    if 'fixture' in opt:                      # write the synthetic selftest file (C++ loader fixture)
        b = selftest()
        open(opt['fixture'], 'wb').write(b); print('wrote', opt['fixture']); return 0
    out_dir = opt.get('out') or os.path.join(os.environ.get('LOCALAPPDATA', '.'), 'acz-vr', 'stage')
    src = load_from_dir(stage, opt['src']) if 'src' in opt else load_from_arc(stage, opt.get('arc', DEFAULT_ARC))
    ground = read_zvrs_ground(open(opt['stage'], 'rb').read()) if 'stage' in opt else None   # PERF: bake-time ground snap (v2)
    blob, info = assemble(stage, src, forest=int(opt.get('forest', '0')), ground=ground)
    read(blob)
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, stage + '.zvro')
    with open(path + '.tmp', 'wb') as f:
        f.write(blob)
    os.replace(path + '.tmp', path)
    print('wrote', path, info, 'sha256(payload)=' + blob[96:128].hex())
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
