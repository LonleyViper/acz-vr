"""ACZ PC model containers (.mpl / .acm / pmmdl*.bin) -> numpy meshes (offline, read-only, numpy+stdlib).

Format (derived from the PC data, see docs/world-render/model-decode.md):
  .mpl      u32 n (10 for plmodel), u32 offsets[n]; parts are ACM / AHM / GIM blocks (an empty part is 16 zero bytes)
  ACM v5    0x00 'ACM\\0'  0x04 u32 version=5  0x08 f32 radius  0x0c f32 (centre height / secondary radius)
            0x10 u32 node_count  0x14 u32 node_table_offset (=80)  0x18 u32 mesh_count  0x1c u32 mesh_table_offset
            0x20.. u32 pairs (count, offset) of trailing tables (locators); node table = node_count * 64 bytes
            mesh table: mesh_count * u32 offsets relative to the 16-aligned end of the table.
  node      u32 flags, u32 child_count, i32 mesh (-1 = locator), u32 0, u32 group/locator id, i32 a, i32 anim channel,
            i32 b, f32 t[3], f32 1.0, f32 rot[3] (radians, R = Rz*Ry*Rx), f32 0           (64 bytes)
            nodes are a depth-first pre-order tree (child_count children follow each node); vertices are in the
            node's local frame; world = parent_world * T(t) * R(rot). For animated nodes (anim channel >= 0) rot[0]
            is NOT applied at rest (constant 0.349 = 20 deg on control surfaces: a deflection range, inference).
  mesh      DMA tag (id 6 = RET, qwc) + VIF stream: repeated batches
              STCYCL cl=5 wl=1; UNPACK V2-32 @0 (nverts, kind=1); V3-32 @1 positions; V3-16 @2 normals (/4096);
              V4-8 @3 colour (0x80 = 1.0); V2-32 @4 uv; FLUSH; MSCNT; STCYCL 4/4
            each batch is ONE triangle strip of nverts vertices (no GIF data: the VU1 program owns PRIM/texture).
  GIM       'GIM\\0' ... u16 w,h @0x1c; w*h 8-bit indices @0x20 (linear, not swizzled); 16-byte CLUT header;
            256 RGBA entries in PS2 CSM1 order (alpha 0x80 = 1.0)
"""
import struct
import numpy as np


class FormatError(ValueError):
    pass


def mpl_parts(b):
    n = struct.unpack_from('<I', b)[0]
    if not 0 < n < 4096 or 4 + 4 * n > len(b):
        raise FormatError('bad part count %d' % n)
    o = list(struct.unpack_from('<%dI' % n, b, 4)) + [len(b)]
    return [b[o[i]:o[i + 1]] for i in range(n)]


NODE = np.dtype([('flags', '<u4'), ('nchild', '<u4'), ('mesh', '<i4'), ('r3', '<u4'), ('group', '<u4'),
                 ('a', '<i4'), ('chan', '<i4'), ('b', '<i4'), ('t', '<f4', 3), ('w', '<f4'),
                 ('rot', '<f4', 3), ('r15', '<f4')])


def acm_header(p):
    if p[:4] != b'ACM\0':
        raise FormatError('not ACM')
    ver, rad, f2, nn, nto, nm, mto = struct.unpack_from('<I2f4I', p, 4)
    if ver != 5 or nto != 80:
        raise FormatError('unsupported ACM v%d' % ver)
    extra = struct.unpack_from('<12I', p, 0x20)
    return dict(version=ver, radius=rad, f2=f2, node_count=nn, mesh_count=nm, mesh_table=mto, extra=extra)


def acm_nodes(p):
    h = acm_header(p)
    return np.frombuffer(p, NODE, h['node_count'], 80)


def node_parents(nodes):
    """Depth-first pre-order tree from child counts -> parent index per node (-1 for roots)."""
    par = [-1] * len(nodes)
    stack = []          # [node, remaining children]
    for i in range(len(nodes)):
        while stack and stack[-1][1] == 0:
            stack.pop()
        if stack:
            par[i] = stack[-1][0]; stack[-1][1] -= 1
        stack.append([i, int(nodes[i]['nchild'])])
    return par


def _rot(r):
    cx, sx = np.cos(r[0]), np.sin(r[0]); cy, sy = np.cos(r[1]), np.sin(r[1]); cz, sz = np.cos(r[2]), np.sin(r[2])
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return Rz @ Ry @ Rx


def node_world(nodes, rest_hinge=True, rest_x=None):
    """-> (N,4,4) model-space matrices (column vectors: p_world = M @ [p,1]).
    rest_x: optional {node: angle} rest rot[0] of animated nodes (COCKPIT-4 fit_rest_angles); default 0 (COCKPIT-1)."""
    par = node_parents(nodes)
    out = np.zeros((len(nodes), 4, 4))
    for i, nd in enumerate(nodes):
        r = np.array(nd['rot'], float)
        if rest_hinge and nd['chan'] >= 0:
            r[0] = rest_x.get(i, 0.0) if rest_x else 0.0
        L = np.eye(4); L[:3, :3] = _rot(r); L[:3, 3] = nd['t']
        out[i] = (out[par[i]] @ L) if par[i] >= 0 else L
    return out


def _mesh_ranges(p):
    h = acm_header(p)
    nm, mt = h['mesh_count'], h['mesh_table']
    offs = struct.unpack_from('<%dI' % nm, p, mt) if nm else ()
    base = (mt + nm * 4 + 15) & ~15
    out = []
    for mo in offs:
        a = base + mo
        tag = struct.unpack_from('<I', p, a)[0]
        if (tag >> 28) & 7 != 6:
            raise FormatError('mesh DMA tag is not RET at 0x%x' % a)
        out.append((a + 8, a + 16 + (tag & 0xffff) * 16))
    return out


def decode_mesh(p, start, end):
    """-> list of strips; each strip dict(pos (n,3) f4, nrm (n,3) f4, col (n,4) u1, uv (n,2) f4, kind int)."""
    strips = []
    i = start
    cur = {}
    while i < end:
        w = struct.unpack_from('<I', p, i)[0]
        cmd = (w >> 24) & 0x7f; num = (w >> 16) & 0xff; imm = w & 0xffff
        i += 4
        if cmd == 0x64 and (imm & 0x3ff) == 0:          # V2-32 @0: (nverts, kind)
            nv, kind = struct.unpack_from('<2I', p, i); i += 8
            cur = dict(n=nv, kind=kind)
        elif cmd == 0x68:
            cur['pos'] = np.frombuffer(p, '<f4', num * 3, i).reshape(num, 3); i += num * 12
        elif cmd == 0x69:
            cur['nrm'] = np.frombuffer(p, '<i2', num * 3, i).reshape(num, 3).astype(np.float32) / 4096.0
            i += (num * 6 + 3) & ~3
        elif cmd == 0x6e:
            cur['col'] = np.frombuffer(p, np.uint8, num * 4, i).reshape(num, 4); i += num * 4
        elif cmd == 0x64:
            cur['uv'] = np.frombuffer(p, '<f4', num * 2, i).reshape(num, 2); i += num * 8
        elif cmd == 0x17:                                  # MSCNT: the batch is complete
            if cur.get('n') and all(k in cur for k in ('pos', 'nrm', 'col', 'uv')):
                strips.append(cur)
            cur = {}
        elif cmd in (0x00, 0x01, 0x11, 0x10, 0x13, 0x14, 0x15):
            pass
        else:
            raise FormatError('unexpected VIF cmd 0x%02x at 0x%x' % (cmd, i - 4))
    return strips


def strip_tris(n, i0=0):
    """Triangle strip -> index triples with alternating order so all triangles share the strip's winding."""
    t = np.arange(n - 2)
    a = i0 + t; b = i0 + t + 1; c = i0 + t + 2
    odd = (t % 2) == 1
    return np.stack([np.where(odd, b, a), np.where(odd, a, b), c], 1)


REST_FIT_MAX = 0.15     # model units (~3 cm): a candidate rest pose must put the part's vertices this close (median) to the static geometry


def fit_rest_angles(nodes, meshes):
    """COCKPIT-4: rest rot[0] of every animated node (anim channel >= 0) with a mesh, FITTED to the model's static geometry.

    The stored rot[0] of an animated node is NOT always a deflection range around 0 (COCKPIT-1 assumed rest = 0): on the Draken the outer
    elevons store 159.9 deg (rest 180, the mirrored part), the rudder -105 (rest -90), the upper-right airbrake 180 (rest 180). Candidates
    0, stored and stored rounded to 90 deg; the one whose posed vertices lie closest (median nearest-vertex distance) to the vertices of the
    non-animated nodes wins if that median is <= REST_FIT_MAX. -> {node: (rest, fitted, median)}; unfitted nodes keep rest 0 (COCKPIT-1)
    and must not be animated (their hinge is not located). Evidence/limits: docs/world-render/cockpit4-impl.md section 3."""
    try:
        from scipy.spatial import cKDTree
    except ImportError:          # numpy fallback (small models)
        cKDTree = None
    par = node_parents(nodes)
    base = node_world(nodes, rest_hinge=True)
    ref = []
    for i, nd in enumerate(nodes):
        if nd['chan'] < 0 and 0 <= nd['mesh'] < len(meshes) and len(meshes[nd['mesh']]['pos']):
            ref.append(meshes[nd['mesh']]['pos'] @ base[i][:3, :3].T + base[i][:3, 3])
    out = {}
    if not ref:
        return out
    ref = np.concatenate(ref)
    tree = cKDTree(ref) if cKDTree else None

    def med(q):
        if tree is not None:
            return float(np.median(tree.query(q)[0]))
        return float(np.median(np.sqrt(((q[:, None, :] - ref[None]) ** 2).sum(-1)).min(1)))
    for i, nd in enumerate(nodes):
        if nd['chan'] < 0 or not (0 <= nd['mesh'] < len(meshes)) or not len(meshes[nd['mesh']]['pos']):
            continue
        stored = float(nd['rot'][0])
        cands = [0.0, stored, float(np.round(stored / (np.pi / 2)) * (np.pi / 2))]
        P = base[par[i]] if par[i] >= 0 else np.eye(4)
        best = None
        for a in cands:
            r = np.array(nd['rot'], float); r[0] = a
            L = np.eye(4); L[:3, :3] = _rot(r); L[:3, 3] = nd['t']
            W = P @ L
            d = med(meshes[nd['mesh']]['pos'] @ W[:3, :3].T + W[:3, 3])
            if best is None or d < best[2] - 1e-6:
                best = (a, True, d)
        out[i] = (best[0], best[2] <= REST_FIT_MAX, best[2]) if best[2] <= REST_FIT_MAX else (0.0, False, best[2])
    return out


def decode_acm(p, fit_rest=False):
    """-> dict(header, nodes, parents, world (N,4,4), meshes=[dict(pos,nrm,col,uv,tri,nstrips)], rest {node: (rest, fitted, median)})
    fit_rest (COCKPIT-4): animated nodes get the fitted rest rot[0] (fit_rest_angles) instead of 0."""
    h = acm_header(p)
    meshes = []
    for (s, e) in _mesh_ranges(p):
        strips = decode_mesh(p, s, e)
        P = []; N = []; C = []; U = []; T = []; base = 0
        for st in strips:
            n = st['n']
            P.append(st['pos']); N.append(st['nrm']); C.append(st['col']); U.append(st['uv'])
            T.append(strip_tris(n, base)); base += n
        if strips:
            meshes.append(dict(pos=np.concatenate(P), nrm=np.concatenate(N), col=np.concatenate(C),
                               uv=np.concatenate(U), tri=np.concatenate(T), nstrips=len(strips)))
        else:
            meshes.append(dict(pos=np.zeros((0, 3), np.float32), nrm=np.zeros((0, 3), np.float32),
                               col=np.zeros((0, 4), np.uint8), uv=np.zeros((0, 2), np.float32),
                               tri=np.zeros((0, 3), np.int64), nstrips=0))
    nodes = acm_nodes(p)
    rest = fit_rest_angles(nodes, meshes) if fit_rest else {}
    world = node_world(nodes, rest_x={i: v[0] for i, v in rest.items()})
    return dict(header=h, nodes=nodes, parents=node_parents(nodes), world=world, meshes=meshes, rest=rest)


def csm1(i):
    """PS2 CSM1 CLUT order: swap bits 3 and 4 of the index."""
    return (i & 0xe7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)


def decode_gim(p):
    """-> (rgba uint8 (h,w,4) with alpha rescaled 0x80->255, raw palette alpha (256,) in CSM1-resolved order)"""
    if p[:4] != b'GIM\0':
        raise FormatError('not GIM')
    w, h = struct.unpack_from('<HH', p, 0x1c)
    if len(p) != 0x20 + w * h + 16 + 1024:
        raise FormatError('unsupported GIM layout %dx%d len %d' % (w, h, len(p)))
    idx = np.frombuffer(p, np.uint8, w * h, 0x20).reshape(h, w)
    pal = np.frombuffer(p, np.uint8, 1024, 0x20 + w * h + 16).reshape(256, 4)
    pal = pal[np.array([csm1(i) for i in range(256)])]
    rgba = np.stack([pal[:, 0], pal[:, 1], pal[:, 2],   # RGBA (COCKPIT-3; COCKPIT-1 read BGRA)
                     np.minimum(pal[:, 3].astype(np.int32) * 255 // 128, 255)], 1).astype(np.uint8)
    return rgba[idx], pal[:, 3].copy()
