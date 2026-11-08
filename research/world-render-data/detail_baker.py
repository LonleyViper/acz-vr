"""DTEX: bake a stage's DETAIL texture (data/map/<s>/<s>_dt.gim) into <s>.zvdt next to the stage's .zvrs (docs/world-render/detail-texture.md).

Usage:
  python -I detail_baker.py <stage>|all [--arc <a.arc>] [--src <tree holding data/map/...>] [--out <dir>]
  python -I detail_baker.py --selftest
Default --out: %LOCALAPPDATA%\\acz-vr\\stage (the runtime reads <out>\\<stage>.zvdt only with xr_terrain_stage ...;dtex:1).
Read-only on the game archive (arcread.py, same folder). The .zvrs bakes are untouched (a separate file keeps every existing bake valid).

.zvdt v1 (little endian), mirrored by acz::terrain::parse_detail (src/acz-vr/terrain_stage.cpp):
  0 'ZVDT' | 4 u32 version 1 | 8 u32 w = 128 | 12 u32 h = 128 | 16 char id[8] (NUL padded) | 32 sha256(payload) | 64 sha256(source .gim) | 96..127 zero
  128: payload u8 rgba[128*128*4] = the decoded CLUT colour + the RAW PS2 CLUT alpha (0x80 = 1.0; not doubled).
The GIM decode is env_gim.parse (4-bit, low nibble first; the flag byte @0x12 = 1 is the 8-bit CSM1 swap only, not applied to 4-bit).
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # arcread / env_gim next to this file (python -I drops the script dir)
DIM = 128
HEADER = 128


def decode_dt(gim_bytes):
    import env_gim
    g = env_gim.parse(gim_bytes)
    if (g['w'], g['h']) != (DIM, DIM):
        raise ValueError('_dt.gim is %dx%d, expected 128x128' % (g['w'], g['h']))
    swap = g['flag'] == 1 and g['bpp'] == 8
    px = bytearray()
    for i in g['idx']:
        r, gg, b, a = g['clut'][env_gim.csm1(i) if swap else i]
        px += bytes((r, gg, b, a))
    return bytes(px)


def build_zvdt(stage, rgba, source_sha):
    if len(rgba) != DIM * DIM * 4:
        raise ValueError('payload size')
    sid = stage.encode('ascii')
    if not (1 <= len(sid) <= 8):
        raise ValueError('stage id')
    hd = bytearray(HEADER)
    hd[0:4] = b'ZVDT'
    struct.pack_into('<III', hd, 4, 1, DIM, DIM)
    hd[16:16 + len(sid)] = sid
    hd[32:64] = hashlib.sha256(rgba).digest()
    hd[64:96] = source_sha
    return bytes(hd) + rgba


def verify(blob):
    assert blob[:4] == b'ZVDT' and struct.unpack_from('<III', blob, 4) == (1, DIM, DIM), 'header'
    assert len(blob) == HEADER + DIM * DIM * 4, 'size'
    assert blob[96:128] == bytes(32), 'reserved'
    assert hashlib.sha256(blob[HEADER:]).digest() == blob[32:64], 'hash'


def stats(rgba):
    g = [rgba[4 * i] for i in range(DIM * DIM)]
    al = sorted(set(rgba[4 * i + 3] for i in range(DIM * DIM)))
    mean = sum(g) / len(g)
    sd = (sum((x - mean) ** 2 for x in g) / len(g)) ** 0.5
    return 'mean %.1f sd %.2f min %d max %d alpha %s' % (mean, sd, min(g), max(g), al[:3])


def gather(names, opt):
    """-> {stage: gim bytes}"""
    out = {}
    if 'src' in opt:
        for s in names:
            p = os.path.join(opt['src'], 'data', 'map', s, s + '_dt.gim')
            if os.path.exists(p):
                out[s] = open(p, 'rb').read()
        return out
    import arcread
    a = arcread.Arc(opt.get('arc', r'D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero\a.arc'))
    ver, es = arcread.entries(a)
    a.ver = ver
    for e in es:
        n = e[0]
        if n.startswith('data/map/') and n.endswith('_dt.gim'):
            s = n.split('/')[-1][:-7]
            if names is None or s in names:
                out[s] = arcread.extract_bytes(a, e)
    return out


def selftest():
    # a 4-bit 128x128 GIM with a 16-entry grey CLUT (alpha 0x40), the layout env_gim.parse reads
    hd = bytearray(0x20); hd[:4] = b'GIM\0'; hd[0x12] = 1; hd[0x13] = 0x14; struct.pack_into('<HH', hd, 0x1c, DIM, DIM)
    idx = [(x + y) % 16 for y in range(DIM) for x in range(DIM)]
    px = bytes((idx[2 * k] & 15) | ((idx[2 * k + 1] & 15) << 4) for k in range(DIM * DIM // 2))
    clut = b''.join(bytes((c, c, c, 0x40)) for c in range(16))
    gim = bytes(hd) + px + bytes(16) + clut
    rgba = decode_dt(gim)
    assert rgba[0:4] == bytes((0, 0, 0, 0x40)) and rgba[4:8] == bytes((1, 1, 1, 0x40)), rgba[:8]
    assert rgba[4 * (DIM + 1)] == 2   # (x+y) % 16 at (1,1)
    blob = build_zvdt('k00a', rgba, hashlib.sha256(gim).digest())
    verify(blob)
    assert blob[16:24] == b'k00a\0\0\0\0'
    try:
        build_zvdt('k00a', rgba[:-4], bytes(32)); raise AssertionError('short payload accepted')
    except ValueError:
        pass
    print('detail_baker selftest ok')


def main(argv):
    if argv[1:] == ['--selftest']:
        selftest(); return 0
    if len(argv) < 2:
        print(__doc__); return 2
    stage = argv[1]
    opt = {}
    i = 2
    while i < len(argv):
        k = argv[i].lstrip('-'); opt[k] = argv[i + 1]; i += 2
    out = opt.get('out') or os.path.join(os.environ.get('LOCALAPPDATA', '.'), 'acz-vr', 'stage')
    os.makedirs(out, exist_ok=True)
    names = None if stage == 'all' else [stage]
    if names is None and 'src' in opt:
        names = sorted(os.listdir(os.path.join(opt['src'], 'data', 'map')))
    gims = gather(names, opt)
    if not gims:
        raise SystemExit('no _dt.gim found for ' + stage)
    for s in sorted(gims):
        rgba = decode_dt(gims[s])
        blob = build_zvdt(s, rgba, hashlib.sha256(gims[s]).digest())
        verify(blob)
        p = os.path.join(out, s + '.zvdt')
        open(p, 'wb').write(blob)
        print('%s -> %s (%d bytes) %s' % (s, p, len(blob), stats(rgba)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
