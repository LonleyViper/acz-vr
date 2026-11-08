"""Bake ACZ PC models (plmodel .mpl, stage pmmdl*.bin props, stage tree .acm) into a compact .zvrm file.

Offline, read-only on game files, numpy + stdlib only.  Format and evidence: docs/world-render/model-decode.md.

  python -I model_baker.py mpl   <file.mpl> <out.zvrm> [--parts 2] [--eye-pvb <type.pvb>] [--name NAME]
  python -I model_baker.py vrcockpit <file.mpl> <type.pvb> <out.zvrm> [--stores-pack f05eex.bin --stores-gim weapon.gim --stores-stations 67,68,75,76]
      (cockpit part 2 + exterior part 0 [+ the game's weapon mesh at the named weapon-station locators])
  python -I model_baker.py pmmdl <pmmdlXXXX.bin> <index> <pmtexXXXX.bin> <out.zvrm>
  python -I model_baker.py acm   <file.acm> <file.gim> <out.zvrm>
  python -I model_baker.py info  <file.zvrm>

Texture policy (evidence: the j35j/m21b/f16c renders against the game's own cockpit image):
  mpl part 2 (cockpit) -> GIM part 9 (cockpit atlas, alpha = real transparency)
  mpl parts 0,1,3,6,7  -> GIM part 8 (exterior skin, alpha = NOT transparency: reflection mask, ignored)
  pmmdl                 -> node.r3 indexes the textures of pmtex entry 1 (mip 0); pmtex entry 0 is the shared CLUT
"""
import argparse, hashlib, os, struct, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from acm_lib import (FormatError, mpl_parts, decode_acm, csm1)

MAGIC = b'ZVRM'
VERSION = 1
HEADER = 160
PART = 128
TEXHDR = 16
VSTRIDE = 32

# header flags (u32 at byte 60)
ZF_CCW = 1               # CCW-front triangle lists
ZF_REST_POSE = 2         # model-space rest pose
ZF_RGBA_CLUT = 4         # COCKPIT-3: textures from embedded GIM CLUTs were decoded R,G,B,A (correct). A file WITHOUT this flag from an
                         # mpl/acm source has R and B swapped (the COCKPIT-1 BGRA reading); the runtime loader swaps them back.
ZF_ANIM_REST = 8         # COCKPIT-4: animated parts carry a FITTED rest rot[0] (part record +120 f32 rest, +124 u32 anim flags bit0 = hinge
                         # located); local_to_model and the vertices include that rest. Without it: rest 0, nothing located (no animation).
PF_HINGE_FITTED = 1      # part anim flag: the rest pose was fitted to the static geometry (acm_lib.fit_rest_angles)

# material flags (ours)
MF_ALPHA_TEST = 1        # texture alpha < 0.5 discarded
MF_BLEND = 2             # premultiplied/straight alpha blend, draw after opaque, no depth write (game flag bit0)
MF_GLASS = 4             # game flag 0x2000: glass / reflective overlay (env-map unknown; tint from texture)
MF_HIDDEN = 8            # hidden by default (alternate-state duplicates, gear, collision-only, ...)
MF_UNLIT = 16            # game flag bit3 clear (lighting off), inference
MF_EXTERIOR_TEX_ALPHA = 32  # texture alpha is not coverage (exterior skins)
MF_EXTERIOR = 64         # part comes from the exterior model (mpl part 0) in a combined VR-cockpit bake
MF_COCKPIT_DUP = 128     # exterior node hidden in the cockpit view: pilot body/helmet or low-detail cockpit tub

def cockpit_alpha_test(g):
    """COCKPIT-5: does the texture alpha of a cockpit-atlas (mpl part 2) node mean COVERAGE? Only where the game turns alpha blending on:
    node flag bit0 (blend) or 0x2000 (glass overlay), and for unlit nodes (bit3 clear: decal-like, kept as before; unverified).
    A lit, non-blend node (flags 0x0008) writes the texel alpha as a plain mask (the exterior-skin rule), so it must stay OPAQUE.
    The J35 atlas has alpha 1.0 under every such node, which hid the bug; the F-5E atlas (and 13 more types) stores 0.1..0.5 under
    its main panels, and alpha testing at 0.5 deleted 93% of the F-5E cockpit (docs/world-render/cockpit-fidelity.md, COCKPIT-5)."""
    g = int(g)
    return bool(g & 0x2001) or not g & 8


# source kinds
SRC_MPL, SRC_PMMDL, SRC_ACM = 1, 2, 3


# ----------------------------------------------------------------------------------------------- textures
def gim_image(p, clut=None):
    """Decode one GIM image -> RGBA8 (h,w,4), alpha rescaled from PS2 0x80=1.0 to 255.
    psm byte at 0x13: 0x13/0x1b 8-bit indexed, 0x14 4-bit indexed, 0x00 32-bit direct.  Pixels are linear (not
    GS-swizzled).  An appended CLUT (16-byte header + entries) is used when present, else `clut` (RGBA 256x4 raw)."""
    if p[:4] != b'GIM\0':
        raise FormatError('not GIM')
    psm = p[0x13]
    w, h = struct.unpack_from('<HH', p, 0x1c)
    if psm == 0x00:
        a = np.frombuffer(p, np.uint8, w * h * 4, 0x20).reshape(h, w, 4).copy()
        a[..., 3] = np.minimum(a[..., 3].astype(np.int32) * 255 // 128, 255)
        return a
    if psm in (0x13, 0x1b):
        n = w * h; idx = np.frombuffer(p, np.uint8, n, 0x20).reshape(h, w); rest = 0x20 + n; ncol = 256
    elif psm == 0x14:
        n = w * h // 2; raw = np.frombuffer(p, np.uint8, n, 0x20)
        idx = np.stack([raw & 15, raw >> 4], 1).reshape(h, w); rest = 0x20 + n; ncol = 16
    else:
        raise FormatError('unsupported GIM psm 0x%02x' % psm)
    if len(p) >= rest + 16 + ncol * 4:
        pal = np.frombuffer(p, np.uint8, ncol * 4, rest + 16).reshape(ncol, 4)
        pal = palette_rgba(pal, bgra=False)   # COCKPIT-3: the appended CLUT is RGBA like every PS2 CT32 CLUT (was read as BGRA)
    elif clut is not None:
        pal = clut
    else:
        raise FormatError('indexed GIM without CLUT')
    return pal[idx]


def palette_rgba(pal, bgra):
    """raw palette entries (n,4) -> RGBA8 (n,4) in index order (CSM1 resolved for 256 entries)."""
    pal = np.asarray(pal)
    if len(pal) == 256:
        pal = pal[np.array([csm1(i) for i in range(256)])]
    r, g, b = (pal[:, 2], pal[:, 1], pal[:, 0]) if bgra else (pal[:, 0], pal[:, 1], pal[:, 2])
    a = np.minimum(pal[:, 3].astype(np.int32) * 255 // 128, 255)
    return np.stack([r, g, b, a], 1).astype(np.uint8)


def table(b):
    n = struct.unpack_from('<I', b)[0]
    if not 0 < n < 100000 or 4 + 4 * n > len(b):
        raise FormatError('bad offset table')
    o = list(struct.unpack_from('<%dI' % n, b, 4)) + [len(b)]
    out = []
    for i in range(n):
        if o[i] == 0:              # zero padding entry
            out.append(b'')
            continue
        e = o[i + 1]
        j = i + 1
        while e == 0 and j < n:
            j += 1; e = o[j + 1] if j + 1 <= n else len(b)
        out.append(b[o[i]:e])
    return out


def pmtex_textures(b):
    """pmtex container: entry 0 = shared CLUT (16x16 PSMCT32 GIM, CSM1 order); entries 1.. = mip levels, each a
    container of the stage's prop textures.  Returns the mip-0 textures."""
    ents = table(b)
    clut_gim = ents[0]
    if clut_gim[0x13] != 0x00:
        raise FormatError('pmtex entry 0 is not a CT32 CLUT')
    raw = np.frombuffer(clut_gim, np.uint8, 1024, 0x20).reshape(256, 4)
    clut = palette_rgba(raw, bgra=False)
    mip0 = [s for s in table(ents[1]) if s]
    return [gim_image(s, clut) for s in mip0]


# ----------------------------------------------------------------------------------------------- assembly
class Builder:
    def __init__(self, name, kind):
        self.name = name; self.kind = kind
        self.parts = []; self.textures = []; self.V = []; self.I = []; self.nv = 0; self.ni = 0
        self.strings = bytearray(b'\0'); self.eye = None

    def string(self, s):
        off = len(self.strings); self.strings += s.encode('utf-8') + b'\0'; return off

    def add_texture(self, rgba):
        self.textures.append(np.ascontiguousarray(rgba, np.uint8)); return len(self.textures) - 1

    def add_acm(self, acm, tag, texture_for_node, mflags_for_node, source_part=0, pre=None):
        m = decode_acm(acm, fit_rest=self.kind == SRC_MPL)   # COCKPIT-4: aircraft parts get fitted hinge rest poses
        base = len(self.parts)
        for ni, nd in enumerate(m['nodes']):
            M = m['world'][ni]
            if pre is not None:   # STORES: place a whole model (e.g. a missile) by a 4x4 (column-vector convention) in the host model's space
                M = np.asarray(pre, np.float64) @ M
            mesh = m['meshes'][nd['mesh']] if 0 <= nd['mesh'] < len(m['meshes']) else None
            fi, fv, ic, vc = self.ni, self.nv, 0, 0
            if mesh is not None and len(mesh['tri']):
                pos = mesh['pos'] @ M[:3, :3].T + M[:3, 3]
                nrm = mesh['nrm'] @ M[:3, :3].T
                nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-6)
                tri = mesh['tri'][:, [0, 2, 1]]          # strips are clockwise-front: emit CCW-front lists
                p = pos[tri]
                area = np.linalg.norm(np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]), axis=1)
                tri = tri[area > 1e-10]                   # drop degenerate strip triangles
                vc = len(pos); ic = tri.size
                v = np.zeros(vc, dtype=VDT)
                v['pos'] = pos; v['uv'] = mesh['uv']
                v['nrm'] = np.clip(np.round(nrm * 32767), -32767, 32767)
                v['part'] = len(self.parts)
                v['rgba'] = np.minimum(mesh['col'].astype(np.int32) * 255 // 128, 255)
                self.V.append(v); self.I.append((tri + self.nv).astype(np.uint32).ravel())
                self.nv += vc; self.ni += ic
            par = m['parents'][ni]
            self.parts.append(dict(
                name=self.string('%s/n%d' % (tag, ni)), parent=(base + par) if par >= 0 else -1, node=ni,
                gflags=int(nd['flags']), group=int(nd['group']), chan=int(nd['chan']),
                mflags=mflags_for_node(nd) if mesh is not None else MF_HIDDEN,
                tex=texture_for_node(nd) if mesh is not None else -1,
                fi=fi, ic=ic, fv=fv, vc=vc, M=M, hinge=float(nd['rot'][0]), src=source_part,
                rest=float(m['rest'].get(ni, (0.0, False, 0.0))[0]),
                aflags=PF_HINGE_FITTED if m['rest'].get(ni, (0.0, False, 0.0))[1] else 0))

    def write(self, path):
        V = np.concatenate(self.V) if self.V else np.zeros(0, VDT)
        I = np.concatenate(self.I) if self.I else np.zeros(0, np.uint32)
        parts = bytearray()
        for p in self.parts:
            parts += struct.pack('<IiIIIiIiIIII', p['name'], p['parent'], p['node'], p['gflags'], p['group'], p['chan'],
                                 p['mflags'], p['tex'], p['fi'], p['ic'], p['fv'], p['vc'])
            parts += np.asarray(p['M'], '<f4').tobytes()            # row-major 4x4, column-vector convention
            parts += struct.pack('<fIfI', p['hinge'], p['src'], p.get('rest', 0.0), p.get('aflags', 0))
        assert len(parts) == PART * len(self.parts)
        nt = len(self.textures)
        texhdr = bytearray(); texdata = bytearray()
        tex_off = HEADER + len(parts)
        data0 = tex_off + TEXHDR * nt
        for t in self.textures:
            texhdr += struct.pack('<IIII', t.shape[1], t.shape[0], 1, data0 + len(texdata))
            texdata += t.tobytes()
        v_off = (data0 + len(texdata) + 15) & ~15
        pad1 = v_off - (data0 + len(texdata))
        i_off = v_off + V.nbytes
        s_off = i_off + I.nbytes
        body = bytes(parts) + bytes(texhdr) + bytes(texdata) + b'\0' * pad1 + V.tobytes() + I.tobytes() + bytes(self.strings)
        total = HEADER + len(body)
        P = V['pos'] if len(V) else np.zeros((1, 3))
        eye = self.eye if self.eye is not None else (0.0, 0.0, 0.0)
        hdr = struct.pack('<4sIIIIIIIIIIIIIII', MAGIC, VERSION, HEADER, total, len(self.parts), nt, len(V), len(I),
                          HEADER, tex_off, v_off, i_off, s_off, len(self.strings), VSTRIDE,
                          ZF_CCW | ZF_REST_POSE | ZF_RGBA_CLUT | ZF_ANIM_REST)
        hdr += struct.pack('<3f3f3fII', *P.min(0), *P.max(0), *eye, 1 if self.eye is not None else 0, self.kind)
        hdr += hashlib.sha256(body).digest()
        hdr += b'\0' * (HEADER - len(hdr))
        assert len(hdr) == HEADER
        with open(path, 'wb') as f:
            f.write(hdr + body)
        return dict(parts=len(self.parts), textures=nt, vertices=len(V), triangles=len(I) // 3, bytes=total)


VDT = np.dtype([('pos', '<f4', 3), ('uv', '<f4', 2), ('nrm', '<i2', 3), ('part', '<u2'), ('rgba', 'u1', 4)])
assert VDT.itemsize == VSTRIDE


def read_zvrm(path):
    b = open(path, 'rb').read()
    if b[:4] != MAGIC:
        raise FormatError('not zvrm')
    (ver, hb, total, np_, nt, nv, ni, p_off, t_off, v_off, i_off, s_off, s_len, stride, flags) = struct.unpack_from('<15I', b, 4)
    if ver != VERSION or hb != HEADER or total != len(b) or stride != VSTRIDE:
        raise FormatError('unsupported zvrm header')
    f = struct.unpack_from('<9fII', b, 64)
    digest = b[108:140]
    if hashlib.sha256(b[HEADER:]).digest() != digest:
        raise FormatError('zvrm hash mismatch')
    parts = []
    for k in range(np_):
        o = p_off + k * PART
        (name, parent, node, gflags, group, chan, mflags, tex, fi, ic, fv, vc) = struct.unpack_from('<IiIIIiIiIIII', b, o)
        M = np.frombuffer(b, '<f4', 16, o + 48).reshape(4, 4)
        hinge, src, rest, aflags = struct.unpack_from('<fIfI', b, o + 112)
        nm = b[s_off + name:b.index(b'\0', s_off + name)].decode()
        parts.append(dict(name=nm, parent=parent, node=node, gflags=gflags, group=group, chan=chan, mflags=mflags,
                          tex=tex, fi=fi, ic=ic, fv=fv, vc=vc, M=M, hinge=hinge, src=src, rest=rest, aflags=aflags))
    texs = []
    for k in range(nt):
        w, h, fmt, off = struct.unpack_from('<4I', b, t_off + k * TEXHDR)
        texs.append(np.frombuffer(b, np.uint8, w * h * 4, off).reshape(h, w, 4))
    V = np.frombuffer(b, VDT, nv, v_off)
    I = np.frombuffer(b, '<u4', ni, i_off)
    return dict(parts=parts, textures=texs, V=V, I=I, bbox=(f[0:3], f[3:6]), eye=f[6:9] if f[9] else None,
                kind=f[10], sha256=digest.hex(), flags=flags)


# ----------------------------------------------------------------------------------------------- sources
def pvb_eye(path):
    """data/mis/mpl/<type>.pvb: floats at byte 40/44/48 = cockpit eye point in model units (j35j: 0,3.575,-36.3
    equals the live E6 camera offset)."""
    b = open(path, 'rb').read()
    return struct.unpack_from('<3f', b, 40)


def bake_mpl(path, out, parts=(0,), eye=None, name=None, hide_groups=(51,), hide_glass=False):
    P = mpl_parts(open(path, 'rb').read())
    B = Builder(name or os.path.basename(path), SRC_MPL)
    tex = {}
    for gi in sorted({9 if pi == 2 else 8 for pi in parts}):     # embed only the textures the parts use
        if gi < len(P) and P[gi][:4] == b'GIM\0':
            tex[gi] = B.add_texture(gim_image(P[gi]))
    for pi in parts:
        if P[pi][:4] != b'ACM\0':
            continue
        cockpit = pi == 2
        t = tex.get(9 if cockpit else 8, -1)

        def mfl(nd, cockpit=cockpit):
            f = 0
            g = int(nd['flags'])
            if cockpit:
                if cockpit_alpha_test(g):
                    f |= MF_ALPHA_TEST
            else:
                f |= MF_EXTERIOR_TEX_ALPHA
            if g & 1:
                f |= MF_BLEND
            if g & 0x2000:
                f |= MF_GLASS | MF_BLEND
                if hide_glass:
                    f |= MF_HIDDEN
            if not g & 8:
                f |= MF_UNLIT
            if cockpit and int(nd['group']) in hide_groups:
                f |= MF_HIDDEN
            return f
        B.add_acm(P[pi], '%s/p%d' % (B.name, pi), lambda nd, t=t: t, mfl, source_part=pi)
    B.eye = eye
    return B.write(out)


def bake_vr_cockpit(path, out, eye, name=None, inside_frac=0.2, hide=(), keep=(), stores=None, tank=None, sp=None):
    """Combined VR cockpit: detailed cockpit (part 2, cockpit atlas) + exterior airframe (part 0, skin) in ONE
    model space.  Exterior nodes that would sit in the pilot's face are flagged MF_COCKPIT_DUP|MF_HIDDEN: opaque
    (game flags without bit0/0x2000), NOT articulated (game flag bit1 clear: canopy frames, control surfaces and
    the airframe root carry bit1), with >= `inside_frac` of their triangle centroids inside the cockpit part's
    bounding box.  That selects the exterior pilot+seat and the low-detail cockpit tub (j35j n8,n13; m21b n28-31;
    f16c n10,n11).  Heuristic from three aircraft; `hide`/`keep` node lists override it (runtime profile)."""
    P = mpl_parts(open(path, 'rb').read())
    ck = decode_acm(P[2])
    pts = [ck['meshes'][nd['mesh']]['pos'] @ ck['world'][i][:3, :3].T + ck['world'][i][:3, 3]
           for i, nd in enumerate(ck['nodes']) if 0 <= nd['mesh'] < len(ck['meshes']) and len(ck['meshes'][nd['mesh']]['pos'])]
    allp = np.concatenate(pts); lo, hi = allp.min(0), allp.max(0)
    ex = decode_acm(P[0])
    dup = set()
    for i, nd in enumerate(ex['nodes']):
        if not (0 <= nd['mesh'] < len(ex['meshes'])) or int(nd['flags']) & 0x2003:
            continue
        me = ex['meshes'][nd['mesh']]
        if not len(me['tri']):
            continue
        W = ex['world'][i]
        c = (me['pos'] @ W[:3, :3].T + W[:3, 3])[me['tri']].mean(1)
        if np.all((c >= lo) & (c <= hi), 1).mean() >= inside_frac:
            dup.add(i)
    dup = (dup | set(hide)) - set(keep)
    B = Builder(name or os.path.basename(path) + ':vrcockpit', SRC_MPL)
    t_ck = B.add_texture(gim_image(P[9])); t_ex = B.add_texture(gim_image(P[8]))

    def f_ck(nd):
        g = int(nd['flags']); f = MF_ALPHA_TEST if cockpit_alpha_test(g) else 0
        if g & 1: f |= MF_BLEND
        if g & 0x2000: f |= MF_GLASS | MF_BLEND
        if not g & 8: f |= MF_UNLIT
        if int(nd['group']) == 51: f |= MF_HIDDEN
        return f
    B.add_acm(P[2], '%s/p2' % B.name, lambda nd: t_ck, f_ck, source_part=2)
    first_ex = len(B.parts)
    B.add_acm(P[0], '%s/p0' % B.name, lambda nd: t_ex,
              lambda nd: MF_EXTERIOR | MF_EXTERIOR_TEX_ALPHA | (MF_BLEND if int(nd['flags']) & 1 else 0)
              | (MF_GLASS | MF_BLEND if int(nd['flags']) & 0x2000 else 0), source_part=0)
    for i in dup:
        B.parts[first_ex + i]['mflags'] |= MF_COCKPIT_DUP | MF_HIDDEN
    r_stores = add_stores(B, ex, stores) if stores else []
    r_sp = add_stores(B, ex, sp, prefix='sp') if sp else []   # SP-STORES: the selected special weapon, its own station group '<model>/sp<station>/...'
    r_tank = add_tank(B, ex, P, t_ex, tank) if tank else None
    B.eye = tuple(float(v) for v in eye)
    r = B.write(out)
    r['hidden_exterior_nodes'] = sorted(dup)
    r['stores'] = r_stores
    r['sp'] = r_sp
    r['tank'] = r_tank
    return r


STORES_SRC_PART = 7     # `src` of the weapon parts (the mpl's own part 7 is the external-stores/tank model, which the vrcockpit bake does not carry)


def weapon_model(pack_path, index=0):
    """The high-detail ACM of weapon `index` of a per-aircraft weapon pack (data/mis/mpl/<type>ex.bin: a container of weapon containers, each
    [16-byte header, high-detail ACM, low-detail ACM, hull ACM]).  Returns the ACM bytes."""
    top = mpl_parts(open(pack_path, 'rb').read())
    sub = mpl_parts(top[index])
    if sub[1][:4] != b'ACM\0':
        raise FormatError('weapon pack %s entry %d has no ACM at part 1' % (pack_path, index))
    return sub[1]


def ms_weapon_model(ms_path):
    """The high-detail ACM of a STANDARD missile file (data/lodweapon/wz<L>1_ms.bin: [16-byte header, high-detail ACM, low-detail ACM, hull ACM], the same
    container layout as one weapon of a <type>ex.bin pack; no pack around it).  Returns the ACM bytes."""
    sub = mpl_parts(open(ms_path, 'rb').read())
    if len(sub) < 2 or sub[1][:4] != b'ACM\0':
        raise FormatError('standard missile %s has no ACM at part 1' % ms_path)
    return sub[1]


def add_stores(B, ex, stores, prefix='store'):
    """STORES (headset test 7: 'the F5 missiles aren't showing on the racks'): the aircraft's weapons are NOT part of its .mpl (the pylon plates
    are, part 0 nodes with group 53/54/55; the weapons are separate models the game attaches at the weapon-station locators, group ids 65..76 in
    part 0).  Adds the game's own weapon mesh once per station: `stores` = dict(pack=<ex.bin path>, index=<weapon in the pack>, gim=<weapon.gim path>,
    stations=[locator group ids]).  The model is authored centred on its own origin with the nose at -z and the tail (fins, motor nozzle disk,
    node 1) at +z, the same as the aircraft (nose -z), so it is placed by the locator's world matrix as is (headset test 8: the earlier 180 degree
    turn about y made every missile point backwards; the narrow seeker end of wz?1_ms.bin and of the ex.bin weapons is at -z, the fins at +z).  Every node is drawn.  Returns the list of (station group, part count) added.
    `prefix` names the station group of the parts ('store' = the standard missile; 'sp' = SP-STORES, the special weapon: '<model>/sp<station>/n<node>').
    The weapon atlas is added once per model and shared by both groups (the GPU has 4 texture slots)."""
    # `ms` (a data/lodweapon/wz<L>1_ms.bin path) = the standard missile file; without it the weapon is entry `index` of the `pack` (the special weapons)
    acm = ms_weapon_model(stores['ms']) if stores.get('ms') else weapon_model(stores['pack'], int(stores.get('index', 0)))
    t_wp = getattr(B, '_weapon_tex', None)
    if t_wp is None:
        t_wp = B._weapon_tex = B.add_texture(gim_image(open(stores['gim'], 'rb').read()))
    out = []
    want = [int(g) for g in stores['stations']]
    for g in want:
        locs = [i for i, nd in enumerate(ex['nodes']) if int(nd['group']) == g and not (0 <= nd['mesh'] < len(ex['meshes']))]
        if len(locs) != 1:
            raise FormatError('weapon station group %d: %d locator nodes in the exterior (need exactly 1)' % (g, len(locs)))
        n0 = len(B.parts)
        B.add_acm(acm, '%s/%s%d' % (B.name, prefix, g), lambda nd: t_wp,
                  lambda nd: MF_EXTERIOR | MF_EXTERIOR_TEX_ALPHA, source_part=STORES_SRC_PART,
                  pre=np.asarray(ex['world'][locs[0]], np.float64))
        out.append((g, len(B.parts) - n0))
    return out


def slot_stations(ex, slot):
    """SP-STORES: the weapon-station locator group ids of weapon slot `slot` (0..2) of the exterior `ex` (decode_acm of mpl part 0), ascending.
    [E structure] mpl part 0 carries three loadout slots, the nodes of group 53, 54 and 55 (a pylon-plate mesh or an empty container), and the
    weapon-station locators (groups 65..76) hang under them as CHILD nodes: f05e 53 -> 67,68; 54 -> 71,72,73,74; 55 -> 75,76 (j35j the same; f15c 53 -> 67..70,
    54 -> 71,72, 55 -> 75,76).  [I] slot i = weapon i of data/mis/mpl/<type>ex.bin (three of each per aircraft; the f15c fits: slot 0 = four fuselage
    stations with the 18.3 u Sparrow-like weapon 0, slot 2 = two wing pylons with the 15.0 u Sidewinder-like weapon 2).  Empty list = no such slot."""
    gid = 53 + int(slot)
    par = ex['parents']
    roots = [i for i, nd in enumerate(ex['nodes']) if int(nd['group']) == gid]
    out = []
    for i, nd in enumerate(ex['nodes']):
        if par[i] in roots and 65 <= int(nd['group']) <= 80 and not (0 <= nd['mesh'] < len(ex['meshes'])):
            out.append(int(nd['group']))
    return sorted(out)


def weapon_shape(acm):
    """(triangles, extent x/y/z) of a weapon ACM in its own frame, or None when it has no triangles."""
    m = decode_acm(acm)
    pts = [me['pos'] for me in m['meshes'] if len(me['pos'])]
    if not pts:
        return None
    a = np.concatenate(pts)
    return sum(len(me['tri']) for me in m['meshes']), a.max(0) - a.min(0)


def find_missile_index(pack_path, min_tris=40, max_tris=300, min_aspect=4.5, max_width=4.5):
    """The first entry of a weapon pack that looks like an air-to-air missile: a high-detail ACM of 40..300 triangles at least `min_aspect` times as long as it
    is wide and at most `max_width` units wide (bombs and big stores are wider; f05e entry 0, j35j entry 0: the olive finned missile; entry 1 is the common black bomb).  Entries that do not decode (an empty first entry:
    e06b) are skipped.  None when no entry qualifies.  [I] 'a missile of the aircraft's own', not the weapon the game mounts for a given loadout."""
    top = mpl_parts(open(pack_path, 'rb').read())
    for i in range(len(top)):
        try:
            sub = mpl_parts(top[i])
            if len(sub) < 2 or sub[1][:4] != b'ACM\0':
                continue
            sh = weapon_shape(sub[1])
        except Exception:   # noqa: BLE001  (a malformed entry is skipped, never fatal for the pack scan)
            continue
        if sh is None:
            continue
        tris, ext = sh
        if min_tris <= tris <= max_tris and max(ext[0], ext[1]) <= max_width and ext[2] >= min_aspect * max(ext[0], ext[1], 1e-6):
            return i
    return None


STATION_GROUPS = (67, 68, 69, 70, 71, 72, 73, 74, 75, 76)   # weapon-station locators under the wings/fuselage; 65/66 (wing tips, no rail drawn) and 79/80 (centre line/dorsal) are not proposed


def station_candidates(ex, acm, groups=STATION_GROUPS):
    """One candidate per weapon-station locator of the exterior `ex` (decode_acm of mpl part 0) whose group is in `groups`: the weapon `acm` placed exactly as
    add_stores places it, its world bounding box, and the number of vertices of the pylon plates (nodes with group 51..58) that fall inside that box (+0.05 u):
    a plate vertex count above a few attachment points means the weapon sits INSIDE a rack / tank mesh.  Sorted by group."""
    w = decode_acm(acm)
    wp = np.concatenate([me['pos'] for me in w['meshes'] if len(me['pos'])])
    plate = []
    for i, nd in enumerate(ex['nodes']):
        if 0 <= nd['mesh'] < len(ex['meshes']) and 51 <= int(nd['group']) <= 58 and len(ex['meshes'][nd['mesh']]['pos']):
            W = np.asarray(ex['world'][i], np.float64)
            plate.append(ex['meshes'][nd['mesh']]['pos'] @ W[:3, :3].T + W[:3, 3])
    plate = np.concatenate(plate) if plate else np.zeros((0, 3))
    out = []
    for i, nd in enumerate(ex['nodes']):
        g = int(nd['group'])
        if g not in groups or (0 <= nd['mesh'] < len(ex['meshes'])):
            continue
        M = np.asarray(ex['world'][i], np.float64)
        q = wp @ M[:3, :3].T + M[:3, 3]
        lo, hi = q.min(0), q.max(0)
        pen = int(np.all((plate >= lo - 0.05) & (plate <= hi + 0.05), 1).sum()) if len(plate) else 0
        out.append(dict(group=g, pos=[float(v) for v in np.asarray(ex['world'][i])[:3, 3]], lo=lo, hi=hi, plate=pen))
    return sorted(out, key=lambda c: c['group'])


def choose_stations(cands, max_plate=30, max_overlap=0.2, max_n=4):
    """Greedy: candidates by (plate vertices inside the weapon box, group); skip a candidate with more than `max_plate` plate vertices inside (the weapon would
    sit in a rack) and one whose box overlaps an already chosen box by more than `max_overlap` of its own volume (two loadout variants share a position);
    at most `max_n` stations, and only mirrored PAIRS survive: a station whose mirror (opposite x, same |x|, y and z within 1 u) was not chosen is dropped.
    Returns the group ids sorted ascending.  Deterministic."""
    def vol(lo, hi):
        d = np.maximum(np.asarray(hi) - np.asarray(lo), 0.0)
        return float(d[0] * d[1] * d[2])
    chosen = []
    for c in sorted(cands, key=lambda c: (c['plate'], c['group'])):
        if c['plate'] > max_plate or len(chosen) >= max_n or abs(c['pos'][0]) <= 1.0:   # centre-line stations are never proposed (wing pairs only)
            continue
        v = vol(c['lo'], c['hi'])
        ok = True
        for k in chosen:
            ov = vol(np.maximum(c['lo'], k['lo']), np.minimum(c['hi'], k['hi']))
            if v > 0 and ov / v > max_overlap:
                ok = False; break
        if ok:
            chosen.append(c)
    def mirror(c):
        return [k for k in cands if k is not c and abs(k['pos'][0] + c['pos'][0]) < 1.0 and abs(k['pos'][1] - c['pos'][1]) < 1.0 and abs(k['pos'][2] - c['pos'][2]) < 1.0
                and abs(c['pos'][0]) > 1.0]
    names = {id(c) for c in chosen}
    keep = [c for c in chosen if any(id(m) in names for m in mirror(c))]   # pairs only: a station without its chosen mirror is dropped
    return sorted(c['group'] for c in keep)


def add_tank(B, ex, P, tex_skin, tank):
    """TANK (STORES gap 3, F-5E belly tank): mpl part 7 is the aircraft's external-stores model (f05e: one node, 216 triangles, 5.9 x 4.2 x 26.7 u; j35j: two
    pods, 356 triangles).  The vrcockpit bake carries parts 2 and 0 only.  Adds part 7 once at the centreline station locator (group `tank['station']`, 79) as an
    exterior part drawn with the airframe skin texture (`tex_skin`), named '<model>/tank<station>/n<node>' (NOT 'store<station>': a drop tank is never part of
    the weapon mask), source part 7.  The authored frame already has the nose at -z like the aircraft (no 180 degree turn: with the turn the tail fins
    point forward).  [I] that the game mounts it by default, the texture and the exact placement (previews only).  Returns (station, part count)."""
    g = int(tank['station'])
    locs = [i for i, nd in enumerate(ex['nodes']) if int(nd['group']) == g and not (0 <= nd['mesh'] < len(ex['meshes']))]
    if len(locs) != 1:
        raise FormatError('tank station group %d: %d locator nodes in the exterior (need exactly 1)' % (g, len(locs)))
    ix = int(tank.get('part', 7))
    if ix >= len(P) or P[ix][:4] != b'ACM\0':
        raise FormatError('mpl part %d is not an ACM (no external-stores model)' % ix)
    n0 = len(B.parts)
    B.add_acm(P[ix], '%s/tank%d' % (B.name, g), lambda nd: tex_skin,
              lambda nd: MF_EXTERIOR | MF_EXTERIOR_TEX_ALPHA, source_part=STORES_SRC_PART,
              pre=np.asarray(ex['world'][locs[0]], np.float64))
    return (g, len(B.parts) - n0)


def bake_pmmdl(path, index, pmtex, out, name=None):
    ents = table(open(path, 'rb').read())
    texs = pmtex_textures(open(pmtex, 'rb').read())
    B = Builder(name or '%s#%d' % (os.path.basename(path), index), SRC_PMMDL)
    ids = [B.add_texture(t) for t in texs]
    B.add_acm(ents[index], B.name, lambda nd: ids[int(nd['r3'])] if int(nd['r3']) < len(ids) else -1,
              lambda nd: MF_ALPHA_TEST | (MF_BLEND if int(nd['flags']) & 1 else 0) | (0 if int(nd['flags']) & 8 else MF_UNLIT))
    return B.write(out)


def bake_acm(path, gim, out, name=None):
    B = Builder(name or os.path.basename(path), SRC_ACM)
    t = B.add_texture(gim_image(open(gim, 'rb').read()))
    B.add_acm(open(path, 'rb').read(), B.name, lambda nd: t,
              lambda nd: MF_ALPHA_TEST | (MF_BLEND if int(nd['flags']) & 1 else 0))
    return B.write(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd', required=True)
    a = sp.add_parser('mpl'); a.add_argument('mpl'); a.add_argument('out'); a.add_argument('--parts', default='0')
    a.add_argument('--eye-pvb'); a.add_argument('--name'); a.add_argument('--hide-glass', action='store_true')
    a = sp.add_parser('vrcockpit'); a.add_argument('mpl'); a.add_argument('pvb'); a.add_argument('out')
    a.add_argument('--hide', default=''); a.add_argument('--keep', default='')
    a.add_argument('--stores-pack', default='', help='data/mis/mpl/<type>ex.bin (the aircraft weapon pack)')
    a.add_argument('--stores-gim', default='', help='data/lodweapon/weapon.gim (the weapon atlas)')
    a.add_argument('--stores-index', type=int, default=0); a.add_argument('--stores-stations', default='', help='locator group ids, e.g. 67,68,75,76')
    a.add_argument('--tank-station', type=int, default=0, help='centreline locator group id for the mpl part-7 tank (79); 0 = none')
    a = sp.add_parser('pmmdl'); a.add_argument('bin'); a.add_argument('index', type=int); a.add_argument('pmtex'); a.add_argument('out')
    a = sp.add_parser('acm'); a.add_argument('acm'); a.add_argument('gim'); a.add_argument('out')
    a = sp.add_parser('info'); a.add_argument('zvrm')
    g = ap.parse_args()
    if g.cmd == 'vrcockpit':
        st = dict(pack=g.stores_pack, gim=g.stores_gim, index=g.stores_index, stations=[int(x) for x in g.stores_stations.split(',') if x])             if g.stores_stations else None
        r = bake_vr_cockpit(g.mpl, g.out, pvb_eye(g.pvb), hide=[int(x) for x in g.hide.split(',') if x],
                            keep=[int(x) for x in g.keep.split(',') if x], stores=st,
                            tank=dict(station=g.tank_station, part=7) if g.tank_station else None)
    elif g.cmd == 'mpl':
        eye = pvb_eye(g.eye_pvb) if g.eye_pvb else None
        r = bake_mpl(g.mpl, g.out, [int(x) for x in g.parts.split(',')], eye, g.name, hide_glass=g.hide_glass)
    elif g.cmd == 'pmmdl':
        r = bake_pmmdl(g.bin, g.index, g.pmtex, g.out)
    elif g.cmd == 'acm':
        r = bake_acm(g.acm, g.gim, g.out)
    else:
        z = read_zvrm(g.zvrm)
        print('parts %d textures %s vertices %d triangles %d eye %s bbox %s..%s sha256 %s' % (
            len(z['parts']), [t.shape[:2] for t in z['textures']], len(z['V']), len(z['I']) // 3, z['eye'],
            np.round(z['bbox'][0], 2), np.round(z['bbox'][1], 2), z['sha256'][:16]))
        return 0
    print(g.out, r)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
