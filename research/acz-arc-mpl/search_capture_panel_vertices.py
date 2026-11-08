"""Search saved capture blobs for exact candidate-panel source position bytes.

The scan reads only caller-supplied, SHA-256-pinned files. A byte match is not
a draw or resource binding; a miss does not exclude transformed, quantized or
otherwise re-encoded geometry, another capture, or another resource.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

spec = importlib.util.spec_from_file_location('display_inspector', Path(__file__).with_name('inspect_display_surfaces.py'))
surfaces = importlib.util.module_from_spec(spec)
spec.loader.exec_module(surfaces)

MAX_BLOB = 256 * 1024 * 1024
MAX_HITS = 1024


def search(data, position):
    needle = struct.pack('<3f', *position)
    hits = []
    start = 0
    while len(hits) < MAX_HITS:
        offset = data.find(needle, start)
        if offset < 0:
            return {'offsets': hits, 'truncated': False}
        hits.append(offset)
        start = offset + 1
    return {'offsets': hits, 'truncated': data.find(needle, start) >= 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mpl', type=Path)
    parser.add_argument('--candidates', required=True, type=Path)
    parser.add_argument('--blob', action='append', nargs=2, metavar=('PATH', 'SHA256'), required=True)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    try:
        candidate_data = json.loads(args.candidates.read_text(encoding='utf-8'))
        panels = surfaces.inspect(args.mpl, candidate_data)['displays']
        captures = []
        for path_string, digest in args.blob:
            path = Path(path_string)
            if path.stat().st_size > MAX_BLOB:
                raise ValueError('blob exceeds 256 MiB limit')
            data = path.read_bytes()
            observed = hashlib.sha256(data).hexdigest()
            if len(digest) != 64 or observed != digest.lower():
                raise ValueError('capture blob SHA-256 mismatch: ' + path.name)
            matches = []
            for panel in panels:
                for corner_name, corner in panel['corners_by_uv'].items():
                    matches.append({'panel_id': panel['id'], 'corner': corner_name,
                                    'position_float32': corner['position'], **search(data, corner['position'])})
            captures.append({'name': path.name, 'sha256': observed, 'bytes': len(data), 'corner_results': matches})
        report = {'schema': 'acz-capture-panel-exact-search/v1', 'model': candidate_data['model'],
                  'captures': captures, 'limitations': ['Exact little-endian float32 XYZ byte sequence search only.',
                    'Byte hits are candidates, not associated with a draw, vertex stride or resource lifetime.',
                    'Negative results do not exclude transformations, float conversion, hidden/culled models or unsurveyed resources.']}
        with args.out.open('x', encoding='utf-8', newline='\n') as out:
            json.dump(report, out, indent=2, allow_nan=False)
            out.write('\n')
    except (OSError, ValueError, KeyError) as exc:
        parser.error(str(exc))
    print('Searched %d capture blobs against %d candidate panels' % (len(captures), len(panels)))


if __name__ == '__main__':
    main()
