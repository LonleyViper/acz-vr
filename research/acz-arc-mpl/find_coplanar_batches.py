"""Find source batches close to hash-verified candidate panel planes.

The output identifies geometric coincidences only. It does not establish draw
order, texture binding, primitive topology, occlusion or semantic labels.
"""
import argparse
import importlib.util
import json
import math
from pathlib import Path


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


surfaces = load('display_surface_inspector', 'inspect_display_surfaces.py')


def plane_coordinates(points, corners):
    top_left = corners['top_left']['position']
    horizontal = surfaces.subtract(corners['top_right']['position'], top_left)
    vertical = surfaces.subtract(corners['bottom_left']['position'], top_left)
    normal = surfaces.cross(horizontal, vertical)
    normal_length = math.sqrt(surfaces.dot(normal, normal))
    hh, vv, hv = surfaces.dot(horizontal, horizontal), surfaces.dot(vertical, vertical), surfaces.dot(horizontal, vertical)
    determinant = hh * vv - hv * hv
    if normal_length <= 1e-12 or determinant <= 1e-20:
        raise ValueError('degenerate candidate plane')
    coordinates = []
    for point in points:
        delta = surfaces.subtract(point, top_left)
        dh, dv = surfaces.dot(delta, horizontal), surfaces.dot(delta, vertical)
        u, v = (dh * vv - dv * hv) / determinant, (dv * hh - dh * hv) / determinant
        distance = surfaces.dot(delta, normal) / normal_length
        if not all(math.isfinite(x) for x in (u, v, distance)):
            raise ValueError('non-finite plane coordinate')
        coordinates.append((u, v, distance))
    return coordinates


def find_nearby(display, batches, distance, margin):
    found = []
    for batch in batches:
        if batch['offset'] == display['batch_offset']:
            continue
        coords = plane_coordinates(batch['positions_f32'], display['corners_by_uv'])
        if not coords:
            continue
        if all(-margin <= u <= 1 + margin and -margin <= v <= 1 + margin and abs(d) <= distance for u, v, d in coords):
            found.append({'batch_offset': batch['offset'], 'source_vertex_count': len(coords),
                          'local_u': [min(p[0] for p in coords), max(p[0] for p in coords)],
                          'local_v': [min(p[1] for p in coords), max(p[1] for p in coords)],
                          'signed_distance_source_units': [min(p[2] for p in coords), max(p[2] for p in coords)],
                          'uv_bounds': [min(p[0] for p in batch['uvs_f32']), min(p[1] for p in batch['uvs_f32']),
                                        max(p[0] for p in batch['uvs_f32']), max(p[1] for p in batch['uvs_f32'])]})
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mpl', type=Path)
    parser.add_argument('--candidates', type=Path, required=True)
    parser.add_argument('--distance', type=float, default=0.004, help='maximum absolute source-unit plane distance')
    parser.add_argument('--margin', type=float, default=0.02, help='allowed edge margin as fraction of panel')
    parser.add_argument('--out', type=Path, required=True, help='new output JSON path')
    args = parser.parse_args()
    if not math.isfinite(args.distance) or not math.isfinite(args.margin) or not 0 <= args.distance <= 1 or not 0 <= args.margin <= 1:
        parser.error('distance and margin must be finite values from 0 to 1')
    try:
        candidate_data = json.loads(args.candidates.read_text(encoding='utf-8'))
        report = surfaces.inspect(args.mpl, candidate_data)
        acm = surfaces.batch_module._mpl_part(args.mpl, candidate_data['model']['acm_part'])
        batches = surfaces.batch_module.acm_batches(acm)['batches']
        panels = [{'id': display['id'], 'source_batch_offset': display['batch_offset'],
                   'nearby': find_nearby(display, batches, args.distance, args.margin)} for display in report['displays']]
        result = {'schema': 'acz-panel-coplanar-source-survey/v1', 'model': candidate_data['model'],
                  'distance_tolerance_source_units': args.distance, 'edge_margin_fraction': args.margin,
                  'panels': panels, 'limitations': ['Proximity of source lanes is not proof of a displayed or overlaid primitive.',
                      'VU execution, material identity, depth test and draw order are unknown.',
                      'Partial intersections are deliberately not reported; these are not a complete coverage list.']}
        with args.out.open('x', encoding='utf-8', newline='\n') as output:
            json.dump(result, output, indent=2, allow_nan=False)
            output.write('\n')
    except (OSError, ValueError, KeyError) as exc:
        parser.error(str(exc))
    print('Surveyed %d panels: %s nearby batches' % (len(panels), ', '.join(str(len(p['nearby'])) for p in panels)))


if __name__ == '__main__':
    main()
