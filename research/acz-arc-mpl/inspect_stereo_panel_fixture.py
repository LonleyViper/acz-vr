"""Project hash-pinned itesu display candidates through a synthetic stereo rig.

This is an offline geometry fixture, not a recovered game eye position or an
OpenXR projection. The two matrices below are explicit synthetic test inputs.
"""
import argparse
import importlib.util
import json
from pathlib import Path


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(file))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


surfaces = load('surface_inspector', 'inspect_display_surfaces.py')
routing = load('panel_routing', 'panel_routing_reference.py')


def synthetic_clip(eye_x):
    # Source units are not calibrated. This camera aims toward increasing Z;
    # w=z+37 and D3D z=z+36 place both observed panels in front of the camera.
    return [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 1], [-eye_x, -2.598371, 36, 37]]


def fixture(mpl, candidates):
    source = surfaces.inspect(mpl, candidates)
    left_clip, right_clip = synthetic_clip(-0.032), synthetic_clip(0.032)
    panels = []
    for display in source['displays']:
        corners = display['corners_by_uv']
        ordered = [corners[name]['position'] for name in ('top_left', 'top_right', 'bottom_right', 'bottom_left')]
        eyes = routing.project_stereo(ordered, left_clip, right_clip)
        if eyes is None:
            raise ValueError('candidate failed synthetic stereo projection: ' + display['id'])
        panels.append({'id': display['id'], 'source_batch_offset': display['batch_offset'],
                       'left_ndc': eyes[0], 'right_ndc': eyes[1]})
    return {'schema': 'acz-itesu-synthetic-stereo-fixture/v1', 'model_sha256': source['model']['mpl_sha256'],
            'panels': panels, 'matrices': {'left': left_clip, 'right': right_clip},
            'limitations': ['Synthetic, uncalibrated camera and eye offset; not a game or headset pose.',
                            'Candidate VU topology and texture binding remain unverified.',
                            'No depth occlusion, runtime identity or real 6DoF claim.']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mpl', type=Path)
    parser.add_argument('--out', type=Path, required=True, help='new output JSON')
    args = parser.parse_args()
    try:
        candidates = json.loads(Path(__file__).with_name('cockpit-profiles').joinpath('itesu-display-surface-candidates.json').read_text(encoding='utf-8'))
        result = fixture(args.mpl, candidates)
        with args.out.open('x', encoding='utf-8', newline='\n') as out:
            json.dump(result, out, indent=2, allow_nan=False)
            out.write('\n')
    except (OSError, ValueError, KeyError) as exc:
        parser.error(str(exc))
    print('Projected %d candidate surfaces for both synthetic eyes' % len(result['panels']))


if __name__ == '__main__':
    main()
