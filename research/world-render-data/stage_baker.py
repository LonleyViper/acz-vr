"""Stage baker: ACZ stage terrain -> compact .zvrs file for the XR terrain renderer (offline, numpy + stdlib only, NO network).

Usage:
  python -I stage_baker.py <stage> [--arc <a.arc>] [--src <dir with <stage>/<stage>.mha ...>] [--out <dir>] [--sea-level auto|height|none|<units>]
  (auto = sea plane at 0 when the .mti sea bit 13 covers the byte-0 tiles, see sea_rule(); height = the old WORLD-2 rule: > 10% byte-0 cells)
  python -I stage_baker.py --selftest

Default --out is %LOCALAPPDATA%\\acz-vr\\stage ; output file <out>\\<stage>.zvrs . Game data is never written to the repository.
Reads (existing reader arcread.py, same folder): data/map/<s>/<s>.{mha,mhd,mta,mti,mtd,mtc,pdl} from a.arc (or a pre-extracted --src tree).

.zvrs format v2 (v1 = same without the water fields; the C++ loader reads both) (little endian; C++ loader: src/acz-vr/terrain_stage.h; docs/world-render/terrain-render-impl.md section 2)
  header, 256 bytes:
    0   char[4]  magic "ZVRS"
    4   u32      version = 1
    8   char[8]  stage id, NUL padded (e.g. "m02a")
    16  u32      flags  bit0 = sea level is a GUESS (flagged unknown), bit1 = sea level present (else none), bit2 = river water present (v2)
    20  u32      hdim   = 513            (height samples per side)
    24  u32      tgrid  = 256            (tile words per side)
    28  u32      ntiles                  (atlas tiles, <= 1024, remapped to the used set)
    32  u32      tilepx = 64
    36  u32      nobj                    (object placements)
    40  f32      cell   = 1024.0         (world units per height cell)
    44  f32      hstep  = 80.0           (world units per height byte)
    48  f32      half   = 262144.0       (terrain spans [-half, +half] in X and Z)
    52  f32      sea_level (world units; meaningful when flags bit1)
    56  u32      off_heights  (absolute file offsets of the 5 sections, 4-byte aligned)
    60  u32      off_words
    64  u32      off_tiles
    68  u32      off_clut
    72  u32      off_objects
    76  u32      file_size
    80  u32      off_tilewater (v2; 0 in v1): u8[ntiles] after the objects, 1 = water tile
    84  u32      water_key (v2; 0 in v1): palette index of the flat river colour inside water tiles
    88..191      zero (reserved)
    192 byte[32] SHA-256 of bytes [256, file_size)
    224 byte[32] SHA-256 of the concatenated source files (informational)
  sections (after the header):
    heights  u8 [513*513]    row = Z index (z = -half + row*cell), col = X index, byte * hstep = y units
    words    u16[256*256]    tile word: low 10 bits = atlas tile index (remapped), bits 10.. = original flag bits (meaning unknown)
                             word(ty,tx): +tx = +X, +ty = +Z, one tile = 2 cells (64 px = 2*32 texels)
    tiles    u8 [ntiles*64*64]  8-bit palette indices (mip 0 only)
    clut     u8 [256*4]      RGBA as stored in .mtc
    objects  {f32 x,y,z; u32 model}[nobj]  from .pdl
    tilewater u8[ntiles] (v2)   1 = every word that uses this tile has .mti flag bit 12 (0x1000); in such a tile texels with palette index == water_key are RIVER WATER
                             (D-299 evidence: m02a: 280043 of 280061 key-colour pixels lie in bit-12 tiles, those tiles have height byte 0; inference for the rendering)
    env (v3 only, flags bit3; header +88 = offset, +92 = byte size 84488) -- WORLD-2 environment, docs/world-render/world2-impl.md:
        'ENV1', u32 1, f32 P[32][4] (parameter texels, see ENV_* below and terrain_stage.h EnvParam), u8 cloud[256*256] (skytex[6] luminance),
        u8 wave[128*128] (<stage>_sea.gim normalised luminance), u8 moon_grey[32*32], u8 moon_alpha[32*32] (skytex[4])
    A stage without the env inputs (or --no-env) is written as v2 exactly as before.
"""
import hashlib, os, struct, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

MAGIC = b'ZVRS'
VERSION = 2
WATER_FLAG = 4
WATER_BIT = 0x1000      # .mti word bit 12
HEADER = 256
CELL, HSTEP, HALF = 1024.0, 80.0, 262144.0
EXTS = ('mha', 'mhd', 'mta', 'mti', 'mtd', 'mtc', 'pdl')
DEFAULT_ARC = r'D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero\a.arc'


def load_from_arc(stage, arc_path):
    import arcread
    a = arcread.Arc(arc_path)
    ver, es = arcread.entries(a)
    a.ver = ver
    by = {e[0]: e for e in es}
    out = {}
    for ext in EXTS:
        name = 'data/map/%s/%s.%s' % (stage, stage, ext)
        if name not in by:
            raise SystemExit('missing in archive: ' + name)
        out[ext] = arcread.extract_bytes(a, by[name])
        assert len(out[ext]) == by[name][2], name
    for k, name in env_paths(stage).items():   # optional WORLD-2 inputs
        if name in by:
            out['env_' + k] = arcread.extract_bytes(a, by[name])
    return out


ENV_FLAG = 8
ENV_PARAMS = 32
ENV_BYTES = 8 + ENV_PARAMS * 16 + 256 * 256 + 128 * 128 + 2 * 32 * 32
# Fitted (NOT data): elevations of the 4 sky stops (horizon, glow, mid, high) against the step56 pre-V game shot (terrain-issues-test16.md section 4)
SKY_STOPS_DEG = (0.0, 1.5, 6.5, 23.0)   # top segment eased out (f*(2-f)) in the renderer
# INFERENCE constants: moon sprite half-size (tan 0.8 deg, from the crescent's ~1.5 deg width in the same shot), cloud map repeat, wave map repeat, cloud shadow strength
MISC = (0.013964, 65536.0, 4096.0, 0.3)


def env_paths(stage):
    return {'mapset': 'data/mapset/map/mapset%s.bin' % stage, 'sph': 'data/spheredata/map/sph%s.sph' % stage,
            'skytex': 'data/map/%s/skytex%s.bin' % (stage, stage), 'sea': 'data/map/%s/%s_sea.gim' % (stage, stage)}


def load_env_from_dir(stage, root, out):
    """root = a tree holding data/... (arcread extract output)."""
    for k, rel in env_paths(stage).items():
        p = os.path.join(root, *rel.split('/'))
        if os.path.exists(p):
            with open(p, 'rb') as f:
                out['env_' + k] = f.read()


def _dir(el, az):
    """game heading convention (D-297): heading a -> forward (-sin a, 0, -cos a); elevation e up."""
    import math
    return (-math.sin(az) * math.cos(el), math.sin(el), -math.cos(az) * math.cos(el))


def build_env(src):
    """-> env section bytes, or None when an input is missing. Offsets: docs/world-render/world2-impl.md section 2 (evidence per field)."""
    import math
    import env_gim
    if not all(('env_' + k) in src for k in ('mapset', 'sph', 'skytex', 'sea')):
        return None
    m, sph = src['env_mapset'], src['env_sph']
    if len(m) != 864 or len(sph) != 352:
        raise ValueError('mapset/sph size')
    f = lambda o: struct.unpack_from('<f', m, o)[0]
    col = lambda b, o: (b[o] / 255.0, b[o + 1] / 255.0, b[o + 2] / 255.0, 0.0)
    P = [(0.0, 0.0, 0.0, 0.0)] * ENV_PARAMS
    P[0] = _dir(f(0xBC), f(0xC0)) + (f(0xC4),)              # moon sprite: elevation, heading, strength
    P[1] = _dir(f(0x58), f(0x5C)) + (1.0,)                   # directional light (sun)
    P[2] = col(m, 0x68)                                      # light colour
    P[3] = col(m, 0x84)                                      # ambient
    for r in range(3):                                       # sph rows 0..2 (+0x10 + 16 r): columns high..horizon -> stops horizon..high
        for k in range(4):
            P[4 + 4 * r + k] = col(sph, 0x10 + 0x10 * r + 4 * (3 - k))
    P[16] = col(m, 0x1F4)                                    # water colour
    P[17] = col(m, 0x1F8)                                    # water highlight
    P[18] = tuple(f(0x210 + 16 * i) for i in range(4))       # cloud layer altitudes
    P[19] = tuple(float(struct.unpack_from('<I', m, 0x21C + 16 * i)[0] != 0) for i in range(4))
    for i in range(4):
        P[20 + i] = col(m, 0x260 + 4 * i)                    # cloud colours
    P[24] = tuple(math.radians(x) for x in SKY_STOPS_DEG)
    P[25] = MISC
    for p in P:
        if not all(math.isfinite(v) for v in p):
            raise ValueError('non-finite env parameter')
    gims = dict(env_gim.gims_in(src['env_skytex']))
    cl = env_gim.parse(gims[6])
    if (cl['w'], cl['h']) != (256, 256):
        raise ValueError('skytex[6] is not 256x256')
    px = np.frombuffer(env_gim.rgba(cl), np.uint8).reshape(256, 256, 4)
    cloud = np.round(px[..., :3].astype(np.float64).mean(2)).astype(np.uint8)
    sea = env_gim.parse(src['env_sea'])
    if (sea['w'], sea['h']) != (128, 128):
        raise ValueError('_sea.gim is not 128x128')
    spx = np.frombuffer(env_gim.rgba(sea), np.uint8).reshape(128, 128, 4)[..., :3].astype(np.float64).mean(2)
    lo, hi = spx.min(), spx.max()
    wave = np.round((spx - lo) / (hi - lo) * 255.0 if hi > lo else spx * 0 + 128).astype(np.uint8)
    mo = env_gim.parse(gims[4])
    if (mo['w'], mo['h']) != (32, 32):
        raise ValueError('skytex[4] is not 32x32')
    mpx = np.frombuffer(env_gim.rgba(mo), np.uint8).reshape(32, 32, 4)
    mgrey = np.round(mpx[..., :3].astype(np.float64).mean(2)).astype(np.uint8)
    malpha = mpx[..., 3].copy()
    body = b'ENV1' + struct.pack('<I', 1) + b''.join(struct.pack('<4f', *p) for p in P)
    body += cloud.tobytes() + wave.tobytes() + mgrey.tobytes() + malpha.tobytes()
    assert len(body) == ENV_BYTES
    return body


SEA_BIT = 0x2000        # .mti word bit 13 = SEA tile (STAGE-SWEEP, docs/world-render/stage-sweep.md)
SEA_MIN_WORDS = 0.01    # sea plane only when >= 1% of the 256x256 tile words carry bit 13 ...
SEA_MIN_AT0 = 0.80      # ... and >= 80% of those tiles have all four corner heights at byte 0


def sea_rule(hf, words):
    """STAGE-SWEEP auto sea rule. Evidence (all 39 bakes): where bit 13 occurs on a real scale (m04, m14, v01a, v03a, v04a, v08a) 90-99% of the
    byte-0 tile cells carry it and 0.0-0.1% of the other cells do; the WORLD-2 rule (> 10% byte-0 cells) also put a sea on the T4 heightfield
    (m03/m10/m16/r01a/v05a: 16% byte 0, NO bit 13, land texture at byte 0) and m06a (16%, no bit 13): false translucent 'sea' rectangles on land.
    -> True = a sea plane at height 0 (still a GUESS: flags bit0)."""
    sea_w = (words & SEA_BIT) != 0
    if sea_w.mean() < SEA_MIN_WORDS:
        return False
    corner = np.maximum(np.maximum(hf[0:512:2, 0:512:2], hf[0:512:2, 2:513:2]), np.maximum(hf[2:513:2, 0:512:2], hf[2:513:2, 2:513:2]))
    return float((corner[sea_w] == 0).mean()) >= SEA_MIN_AT0


def load_from_dir(stage, root):
    out = {}
    for ext in EXTS:
        p = os.path.join(root, stage, '%s.%s' % (stage, ext))
        with open(p, 'rb') as f:
            out[ext] = f.read()
    return out


def assemble(stage, src, sea_mode='auto', env=True):
    """-> bytes of the .zvrs file."""
    chunks = np.frombuffer(src['mhd'], np.uint8)
    if chunks.size % (33 * 33):
        raise ValueError('mhd size')
    chunks = chunks.reshape(-1, 33, 33)
    mha = np.frombuffer(src['mha'], np.uint8)
    mta = np.frombuffer(src['mta'], np.uint8)
    if mha.size != 256 or mta.size != 256:
        raise ValueError('mha/mta size')
    mha = mha.reshape(16, 16)
    mta = mta.reshape(16, 16)
    if int(mha.max()) >= chunks.shape[0]:
        raise ValueError('mha index beyond mhd')
    hf = np.zeros((513, 513), np.uint8)
    for r in range(16):
        for c in range(16):
            hf[r * 32:r * 32 + 33, c * 32:c * 32 + 33] = chunks[mha[r, c]]
    mti = np.frombuffer(src['mti'], '<u2')
    if mti.size % 256:
        raise ValueError('mti size')
    blocks = mti.reshape(-1, 16, 16)
    if int(mta.max()) >= blocks.shape[0]:
        raise ValueError('mta block beyond mti')
    w = np.zeros((16, 16, 16, 16), np.uint16)
    for r in range(16):
        for c in range(16):
            w[r, c] = blocks[mta[r, c]]
    words = w.transpose(0, 2, 1, 3).reshape(256, 256)
    mtd = src['mtd']
    n_src = struct.unpack('<I', mtd[:4])[0]
    tiles_src = np.frombuffer(mtd, np.uint8, n_src * 4096, 0x20).reshape(n_src, 64, 64)
    ids = (words & 0x3ff).astype(np.int64)
    if int(ids.max()) >= n_src:
        raise ValueError('tile id beyond mtd')
    used = np.unique(ids)
    remap = np.zeros(1024, np.uint16)
    remap[used] = np.arange(used.size, dtype=np.uint16)
    words2 = ((words & ~np.uint16(0x3ff)) | remap[ids]).astype(np.uint16)
    tiles = np.ascontiguousarray(tiles_src[used])
    clut = np.frombuffer(src['mtc'], np.uint8)
    if clut.size != 1024:
        raise ValueError('mtc size')
    pdl = src['pdl']
    nobj = 0
    objs = b''
    if len(pdl) >= 4:
        cnt = struct.unpack('<I', pdl[:4])[0]
        if cnt and len(pdl) >= 16 + 16 * cnt:
            nobj = cnt
            objs = pdl[16:16 + 16 * cnt]
    # sea level: the stage water height is NOT understood (docs: .wad/.wbd/.wpd/_sea.gim unknown)
    flags = 0
    sea = 0.0
    zero_frac = float((hf == 0).mean())
    if sea_mode == 'auto':
        if sea_rule(hf, words):
            flags |= 3          # present + guessed: height byte 0 plane
    elif sea_mode == 'height':  # the WORLD-2 rule (kept for comparison): > 10% of the height cells at byte 0
        if zero_frac > 0.10:
            flags |= 3
    elif sea_mode == 'none':
        pass
    else:
        sea = float(sea_mode)
        flags |= 3
    # river water (D-299): tiles used ONLY by words with bit 12; the key colour = the dominant palette index inside them (>= 20% of the water-tile pixels)
    uses = np.zeros(used.size, np.int64); wuses = np.zeros(used.size, np.int64)
    np.add.at(uses, remap[ids].ravel().astype(np.int64), 1)
    np.add.at(wuses, remap[ids].ravel().astype(np.int64), ((words & WATER_BIT) != 0).ravel().astype(np.int64))
    tile_water = ((uses > 0) & (uses == wuses)).astype(np.uint8)
    water_key = 0
    if tile_water.any():
        hist = np.bincount(tiles[tile_water.astype(bool)].ravel(), minlength=256)
        if hist.max() >= 0.2 * hist.sum():
            water_key = int(hist.argmax())
            flags |= WATER_FLAG
    if not (flags & WATER_FLAG):
        tile_water[:] = 0
    secs = [hf.tobytes(), words2.astype('<u2').tobytes(), tiles.tobytes(), clut.tobytes(), objs, tile_water.tobytes()]
    env_body = build_env(src) if env else None
    if env_body is not None:
        secs.append(env_body)
        flags |= ENV_FLAG
    offs = []
    pos = HEADER
    body = bytearray()
    for s in secs:
        pad = (-pos) % 4
        body += b'\0' * pad
        pos += pad
        offs.append(pos)
        body += s
        pos += len(s)
    pad = (-pos) % 4
    body += b'\0' * pad
    pos += pad
    size = pos
    h = bytearray(HEADER)
    sid = stage.encode('ascii')[:8]
    struct.pack_into('<4sI8sIIIIIIffff', h, 0, MAGIC, 3 if env_body is not None else VERSION, sid, flags, 513, 256, used.size, 64, nobj, CELL, HSTEP, HALF, sea)
    struct.pack_into('<IIIIII', h, 56, offs[0], offs[1], offs[2], offs[3], offs[4], size)
    struct.pack_into('<II', h, 80, offs[5], water_key)
    if env_body is not None:
        struct.pack_into('<II', h, 88, offs[6], ENV_BYTES)
    h[192:224] = hashlib.sha256(bytes(body)).digest()
    sh = hashlib.sha256()
    for ext in EXTS:
        sh.update(src[ext])
    if env_body is not None:
        for k in ('mapset', 'sph', 'skytex', 'sea'):
            sh.update(src['env_' + k])
    h[224:256] = sh.digest()
    return bytes(h) + bytes(body), dict(ntiles=int(used.size), nobj=nobj, zero_frac=zero_frac, flags=flags,
                                         hmax=int(hf.max()), size=size, water_tiles=int(tile_water.sum()), water_key=water_key,
                                         env=env_body is not None)


def verify(blob):
    """Re-check a baked file (same rule as the C++ loader). Raises ValueError."""
    if len(blob) < HEADER or blob[:4] != MAGIC:
        raise ValueError('magic')
    ver = struct.unpack_from('<I', blob, 4)[0]
    size = struct.unpack_from('<I', blob, 76)[0]
    if ver not in (1, VERSION, 3) or size != len(blob):
        raise ValueError('version/size')
    if hashlib.sha256(blob[HEADER:]).digest() != blob[192:224]:
        raise ValueError('payload hash')
    return True


def selftest():
    rng = np.random.default_rng(7)
    src = {
        'mhd': rng.integers(0, 40, (3, 33, 33), dtype=np.uint8).tobytes(),
        'mha': rng.integers(0, 3, 256, dtype=np.uint8).tobytes(),
        'mta': rng.integers(0, 2, 256, dtype=np.uint8).tobytes(),
        'mti': (rng.integers(0, 5, 2 * 256) | (rng.integers(0, 2, 2 * 256) << 10)).astype('<u2').tobytes(),
        'mtd': struct.pack('<I', 5) + b'\0' * 28 + rng.integers(0, 256, 5 * 4096, dtype=np.uint8).tobytes(),
        'mtc': rng.integers(0, 256, 1024, dtype=np.uint8).tobytes(),
        'pdl': struct.pack('<4I', 1, 0, 0, 0) + struct.pack('<fffI', 1.0, 2.0, 3.0, 4),
    }
    b1, info = assemble('synth', src)
    b2, _ = assemble('synth', src)
    assert b1 == b2, 'not deterministic'
    verify(b1)
    bad = bytearray(b1)
    bad[HEADER + 5] ^= 1
    try:
        verify(bytes(bad))
        raise AssertionError('corruption not detected')
    except ValueError:
        pass
    # water: tile 2 is used only with flag bit 12 and is mostly palette index 7 -> water tile + key 7; tile 3 used with and without the bit -> land
    mti = np.zeros(2 * 256, '<u2'); mti[:256] = 2 | WATER_BIT; mti[256:] = 3; mti[5] = 3 | WATER_BIT
    mta = np.zeros(256, np.uint8); mta[:128] = 0; mta[128:] = 1
    tiles = rng.integers(0, 256, 5 * 4096, dtype=np.uint8).reshape(5, 4096); tiles[2, :3000] = 7
    src2 = dict(src, mti=mti.tobytes(), mta=mta.tobytes(), mtd=struct.pack('<I', 5) + b'\0' * 28 + tiles.tobytes())
    b3, info3 = assemble('synth', src2)
    verify(b3)
    assert info3['water_tiles'] == 1 and info3['water_key'] == 7 and info3['flags'] & WATER_FLAG, info3
    # env (v3): synthetic mapset/sph/skytex/sea -> section present, version 3, parameters decoded at the documented offsets
    def gim(w, h, bpp, idx, clut):
        hd = bytearray(0x20); hd[:4] = b'GIM\0'; hd[0x13] = 0x13 if bpp == 8 else 0x14; struct.pack_into('<HH', hd, 0x1c, w, h)
        px = bytes(idx) if bpp == 8 else bytes((idx[2 * i] & 15) | ((idx[2 * i + 1] & 15) << 4) for i in range(len(idx) // 2))
        return bytes(hd) + px + b'\0' * 16 + b''.join(bytes(c) for c in clut)
    import math
    m = bytearray(864)
    struct.pack_into('<3f', m, 0xBC, math.radians(10), math.radians(-53), 0.85)
    struct.pack_into('<2f', m, 0x58, math.radians(50), math.radians(-130))
    m[0x68:0x6B] = bytes((83, 83, 68)); m[0x84:0x87] = bytes((110, 121, 105)); m[0x1F4:0x1F7] = bytes((70, 89, 84))
    struct.pack_into('<f', m, 0x210, 16000.0); struct.pack_into('<I', m, 0x21C, 1)
    sph = bytearray(352); sph[0x10:0x20] = bytes((65, 100, 112, 0, 92, 132, 141, 0, 140, 164, 155, 0, 135, 156, 148, 0))
    blank8 = gim(32, 32, 8, [0] * 1024, [(0, 0, 0, 128)] * 256)
    moon = gim(32, 32, 4, [i % 2 for i in range(1024)], [(0, 0, 0, 0), (200, 200, 200, 67)] + [(0, 0, 0, 0)] * 14)
    cloudg = gim(256, 256, 8, [255 if (i // 256) % 64 < 8 else 0 for i in range(65536)], [(c, c, c, 255) for c in range(256)])
    sky = [blank8] * 12; sky[4] = moon; sky[6] = cloudg
    offs = []; pos = 4 + 4 * 12
    for g in sky:
        offs.append(pos); pos += len(g)
    hdr = struct.pack('<13I', 12, *offs)   # u32 count, then the 12 image offsets (as in the game files)
    seag = gim(128, 128, 4, [i % 16 for i in range(16384)], [(c * 16, c * 16, c * 16, 20) for c in range(16)])
    src4 = dict(src, env_mapset=bytes(m), env_sph=bytes(sph), env_skytex=bytes(hdr) + b''.join(sky), env_sea=seag)
    b4, info4 = assemble('synth', src4)
    verify(b4)
    assert info4['env'] and struct.unpack_from('<I', b4, 4)[0] == 3 and struct.unpack_from('<I', b4, 92)[0] == ENV_BYTES, info4
    eo = struct.unpack_from('<I', b4, 88)[0]
    assert b4[eo:eo + 4] == b'ENV1'
    p = struct.unpack_from('<%df' % (ENV_PARAMS * 4), b4, eo + 8)
    assert abs(p[0 * 4 + 1] - math.sin(math.radians(10))) < 1e-6 and abs(p[0 * 4 + 3] - 0.85) < 1e-6      # moon elevation, strength
    assert abs(p[4 * 4 + 1] - 156 / 255.0) < 1e-6 and abs(p[7 * 4 + 1] - 100 / 255.0) < 1e-6           # sky stop 0 = horizon column, stop 3 = high
    assert abs(p[16 * 4 + 1] - 89 / 255.0) < 1e-6 and p[18 * 4] == 16000.0 and p[19 * 4] == 1.0
    assert assemble('synth', src4, env=False)[0] != b4 and struct.unpack_from('<I', assemble('synth', src4, env=False)[0], 4)[0] == 2
    # sea rule (STAGE-SWEEP): byte-0 basin without bit-13 words -> no sea (the WORLD-2 'height' rule said yes); bit 13 on the byte-0 tiles -> sea
    hfz = np.zeros((513, 513), np.uint8); hfz[:, 300:] = 20
    w0 = np.zeros((256, 256), np.uint16)
    assert not sea_rule(hfz, w0)
    w13 = w0.copy(); w13[:, :140] |= SEA_BIT
    assert sea_rule(hfz, w13)
    w13b = w0.copy(); w13b[:, 160:] |= SEA_BIT      # bit 13 on raised land only -> refused
    assert not sea_rule(hfz, w13b)
    print('selftest ok', info, info3, info4)


def main(argv):
    if '--selftest' in argv:
        selftest()
        return 0
    args = [a for a in argv if not a.startswith('--')]
    opt = {}
    i = 0
    flat = []
    while i < len(argv):
        if argv[i].startswith('--') and i + 1 < len(argv):
            opt[argv[i][2:]] = argv[i + 1]
            i += 2
        else:
            flat.append(argv[i])
            i += 1
    if not flat:
        print(__doc__)
        return 2
    stage = flat[0]
    out_dir = opt.get('out') or os.path.join(os.environ.get('LOCALAPPDATA', '.'), 'acz-vr', 'stage')
    src = load_from_dir(stage, opt['src']) if 'src' in opt else load_from_arc(stage, opt.get('arc', DEFAULT_ARC))
    if 'env-src' in opt:   # an extracted tree holding data/mapset, data/spheredata, data/map/<stage>/skytex..., _sea.gim
        load_env_from_dir(stage, opt['env-src'], src)
    blob, info = assemble(stage, src, opt.get('sea-level', 'auto'), env=opt.get('env', '1') != '0')
    verify(blob)
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, stage + '.zvrs')
    tmp = path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(blob)
    os.replace(tmp, path)
    print('wrote', path, info, 'sha256(payload)=' + blob[192:224].hex())
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
