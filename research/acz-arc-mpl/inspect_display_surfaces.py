"""Verify cockpit screen candidates against an extracted MPL, without exporting meshes.

The output contains paired position/UV source lanes and geometric measurements.
It does not assert VU execution, texture binding, topology or runtime identity.
"""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path

spec = importlib.util.spec_from_file_location('acz_acm_batches', Path(__file__).with_name('acm_batches.py'))
batch_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(batch_module)


def subtract(a, b):
    return [a[i] - b[i] for i in range(3)]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def describe(batch):
    positions = batch['positions_f32']
    uv = batch['uvs_f32']
    if len(positions) != 4 or len(uv) != 4:
        raise ValueError('display candidate must have four paired source lanes')
    if not all(math.isfinite(x) for row in positions + uv for x in row):
        raise ValueError('non-finite candidate source lane')
    # Sort by UV corner, not VIF primitive order. No triangle assembly is inferred.
    umin, umax = min(x[0] for x in uv), max(x[0] for x in uv)
    vmin, vmax = min(x[1] for x in uv), max(x[1] for x in uv)
    if umax <= umin or vmax <= vmin:
        raise ValueError('degenerate UV rectangle')
    corners = {}
    for p, t in zip(positions, uv):
        name = ('top' if abs(t[1] - vmin) < abs(t[1] - vmax) else 'bottom') + '_' + ('left' if abs(t[0] - umin) < abs(t[0] - umax) else 'right')
        if name in corners:
            raise ValueError('UV values do not identify four distinct corners')
        corners[name] = {'position': p, 'uv': t}
    tl, tr, bl, br = (corners[k]['position'] for k in ('top_left', 'top_right', 'bottom_left', 'bottom_right'))
    horizontal, vertical = subtract(tr, tl), subtract(bl, tl)
    normal = cross(horizontal, vertical)
    length = math.sqrt(dot(normal, normal))
    if length < 1e-12:
        raise ValueError('degenerate position geometry')
    unit_normal = [v / length for v in normal]
    return {'corners_by_uv': corners,
            'centre': [sum(p[i] for p in positions) / 4 for i in range(3)],
            'width_source_units': math.sqrt(dot(horizontal, horizontal)),
            'height_source_units': math.sqrt(dot(vertical, vertical)),
            'fourth_corner_plane_distance_source_units': abs(dot(subtract(br, tl), unit_normal)),
            'uv_bounds': [umin, vmin, umax, vmax]}


def inspect(mpl, candidates):
    data = mpl.read_bytes()
    model = candidates['model']
    if hashlib.sha256(data).hexdigest() != model['mpl_sha256']:
        raise ValueError('MPL SHA-256 mismatch')
    block = batch_module._mpl_part(mpl, model['acm_part'])
    if hashlib.sha256(block).hexdigest() != model['acm_sha256']:
        raise ValueError('ACM SHA-256 mismatch')
    texture = batch_module._mpl_part(mpl, model['texture_part'])
    if hashlib.sha256(texture).hexdigest() != model['texture_sha256']:
        raise ValueError('texture SHA-256 mismatch')
    batches = {b['offset']: b for b in batch_module.acm_batches(block)['batches']}
    results = []
    for candidate in candidates['candidates']:
        offset = int(candidate['acm_batch_offset'], 0)
        if offset not in batches:
            raise ValueError('candidate batch not found: ' + candidate['id'])
        result = describe(batches[offset])
        if any(abs(a - b) > 1e-6 for a, b in zip(result['uv_bounds'], candidate['uv_rect_normalized'])):
            raise ValueError('candidate UV bounds mismatch: ' + candidate['id'])
        result.update(id=candidate['id'], proposed_domain=candidate['proposed_domain'], batch_offset=offset)
        results.append(result)
    return {'schema': 'acz-display-source-lanes/v1', 'model': model, 'displays': results,
            'limitations': ['Source lanes only; VIF/VU execution and primitive topology are unverified.',
                            'Texture binding, coordinate frame, camera and runtime aircraft identity are unverified.',
                            'No game data is modified.']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mpl', type=Path)
    parser.add_argument('--candidates', type=Path, default=Path(__file__).parent / 'cockpit-profiles/itesu-display-surface-candidates.json')
    parser.add_argument('--out', type=Path, required=True, help='new JSON output path')
    args = parser.parse_args()
    try:
        result = inspect(args.mpl, json.loads(args.candidates.read_text(encoding='utf-8')))
        with args.out.open('x', encoding='utf-8', newline='\n') as out:
            json.dump(result, out, indent=2, allow_nan=False)
            out.write('\n')
    except (OSError, ValueError, KeyError) as exc:
        parser.error(str(exc))
    print('Verified %d source-lane display candidates' % len(result['displays']))


if __name__ == '__main__':
    main()
