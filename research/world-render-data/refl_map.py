"""SKIN-REFL: bake the per-stage reflection map (data/map/<stage>/<stage>_ref.gim) into the .zvrt sidecar the runtime loads next to the .zvrs.

Evidence and design: docs/world-render/aircraft-shading-reflection.md section 7. The map is a 32x32 hemisphere map of the stage sky indexed by the WORLD
normal (u = 0.5 + 0.5 n.x, v = 0.5 + 0.5 n.z, game axes; the sun highlight sits on the stage sun's azimuth in every stage), drawn by the engine's
cb10-bit-0 pass of the aircraft parts.  Read-only on extracted game files (arcread.py extract); writes <out_dir>/<stage>_ref.zvrt.

Format (src/acz-vr/terrain_stage.h parse_refl_map): 48-byte header "ZVRT", u32 version 1, u16 w, u16 h, u32 kind 1, SHA-256 of the payload;
payload = w*h RGBA8 (row 0 = v 0; CLUT colour as stored, alpha 0x80 -> 255 like env_gim.rgba).

  python -I refl_map.py <data_root> <out_dir> [stage ...]     (data_root holds map/<stage>/<stage>_ref.gim; no stages = every stage found)
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import env_gim

MAGIC = b'ZVRT'
VERSION = 1
KIND = 1
HEADER = 48
MAX_DIM = 64


def encode(w, h, rgba):
    if not (0 < w <= MAX_DIM and 0 < h <= MAX_DIM) or len(rgba) != w * h * 4:
        raise ValueError('bad reflection map size')
    return MAGIC + struct.pack('<IHHI', VERSION, w, h, KIND) + hashlib.sha256(rgba).digest() + bytes(rgba)


def decode(b):
    if len(b) < HEADER or b[:4] != MAGIC:
        raise ValueError('not ZVRT')
    ver, w, h, kind = struct.unpack_from('<IHHI', b, 4)
    if ver != VERSION or kind != KIND or not (0 < w <= MAX_DIM and 0 < h <= MAX_DIM) or len(b) != HEADER + w * h * 4:
        raise ValueError('bad ZVRT header/size')
    px = b[HEADER:]
    if hashlib.sha256(px).digest() != b[16:48]:
        raise ValueError('bad ZVRT hash')
    return w, h, px


def from_gim(data):
    g = env_gim.parse(data)
    return g['w'], g['h'], env_gim.rgba(g)


def bake(data_root, out_dir, stages=None):
    mroot = os.path.join(data_root, 'map')
    if not stages:
        stages = sorted(s for s in os.listdir(mroot) if os.path.exists(os.path.join(mroot, s, s + '_ref.gim')))
    os.makedirs(out_dir, exist_ok=True)
    done = []
    for s in stages:
        p = os.path.join(mroot, s, s + '_ref.gim')
        w, h, px = from_gim(open(p, 'rb').read())
        out = os.path.join(out_dir, s + '_ref.zvrt')
        with open(out, 'wb') as f:
            f.write(encode(w, h, px))
        done.append((s, w, h, out))
    return done


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    for s, w, h, out in bake(sys.argv[1], sys.argv[2], sys.argv[3:]):
        print('%s %dx%d -> %s' % (s, w, h, out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
