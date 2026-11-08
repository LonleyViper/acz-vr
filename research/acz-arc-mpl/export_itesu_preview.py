"""Export a provisional, textured aircraft part-2 OBJ from a user's extracted MPL.

Triangle strips and the part-9 texture binding are hypotheses. This is a local
visual inspection tool, not a verified game model or a runtime mod. No archive
or game file is modified. Output must be a new directory outside the repository.
"""
import argparse
import base64
import hashlib
import importlib.util
import json
import math
from pathlib import Path


def local_module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


batch_module = local_module('acz_batches', 'acm_batches.py')
gim_module = local_module('acz_gim', 'gim_inspect.py')
MODELS = {
    'itesu': {'mpl': '614f4e683d34348df80a159fd1156aa21827248145efcef14f549804c17f9a9a',
              'acm': 'a01b8e008c1fec1d9e555ae4e58b1ba0d33276da0eff29f18a16c07c5dc577b3',
              'gim': 'db68018cb2232ce7ec567da294426470c1d010afbf1964d8954f5391215243c3',
              'view': {'target': [0, 2.6, -35], 'bounds': [-1.5, 1.5, 2, 3.7, -35.6, -34.4],
                       'mock_screens': [{'offset': 0x210, 'source': [82, 868, 620, 560], 'uv': [0.1715692282, 0.3010668755, 0.2812442482, 0.4107418061], 'inset': 0.0, 'flip_v': False},
                                        {'offset': 0x16c, 'source': [2038, 900, 522, 540], 'uv': [0.5150018930, 0.3017023206, 0.6252503991, 0.4087194204], 'inset': 0.14, 'flip_v': False}]}},
    'f16ck': {'mpl': 'cf58a887649bb83c9d837e8608d619c83ef59d8b32e9366a2428ca1723234436',
              'acm': '8fea96237e8dcde5b050aaebc95d7f8a466f6a1b302e201915f7f4f2dfc83d2f',
              'gim': 'f07f64cb2bb906009f16bdd20bc549338f9244d2b9bab81ad66ae8f69de01acc',
              'view': {'target': [0, 2.6, -35.2], 'bounds': [-1.5, 1.5, 2, 3.7, -36.2, -34.4],
                       'nearby_offsets': [0x2e8c, 0x2f30, 0x2fd4, 0x3078, 0x311c, 0x8ed4,
                                          0x31c0, 0x3264, 0x3308, 0x33ac, 0x3598, 0x363c, 0x36e0, 0x8f78],
                       'mock_screens': [{'offset': 0x191e0, 'source': [82, 868, 620, 560], 'uv': [0.2460940033, 0.9335939884, 0.2851560116, 0.9726560116], 'inset': 0.0, 'flip_v': True},
                                        {'offset': 0x19284, 'source': [2038, 900, 522, 540], 'uv': [0.2505260110, 0.9351190329, 0.2807239890, 0.9711329937], 'inset': 0.14, 'flip_v': True}]}},
}


def validate_input(source, model):
    expected = MODELS[model]
    source_bytes = source.read_bytes()
    if hashlib.sha256(source_bytes).hexdigest() != expected['mpl']:
        raise ValueError('unexpected %s MPL SHA-256' % model)
    mesh = batch_module._mpl_part(source, 2)
    texture = batch_module._mpl_part(source, 9)
    if hashlib.sha256(mesh).hexdigest() != expected['acm'] or hashlib.sha256(texture).hexdigest() != expected['gim']:
        raise ValueError('unexpected mesh or texture part SHA-256')
    batches = batch_module.acm_batches(mesh)
    for batch in batches['batches']:
        if not all(math.isfinite(value) for lane in batch['positions_f32'] + batch['uvs_f32'] for value in lane):
            raise ValueError('non-finite position or UV')
    return batches, gim_module.parse_gim(texture)


def write_preview(source, output, hud_preview=None, model='itesu', experimental_f16_mock=False):
    repo_root = Path(__file__).resolve().parents[2]
    if output.resolve().is_relative_to(repo_root):
        raise ValueError('output must be outside the repository')
    if model != 'itesu' and hud_preview is not None and not experimental_f16_mock:
        raise ValueError('HUD mock placement has not been calibrated for this aircraft')
    batches, texture = validate_input(source, model)
    hud_bytes = None
    if hud_preview is not None:
        hud_bytes = hud_preview.read_bytes()
        if len(hud_bytes) < 24 or hud_bytes[:8] != b'\x89PNG\r\n\x1a\n' or hud_bytes[16:24] != (2560).to_bytes(4, 'big') + (1440).to_bytes(4, 'big'):
            raise ValueError('HUD preview must be the 2560x1440 diagnostic PNG')
    output.mkdir(parents=True, exist_ok=False)
    texture_name = model + '-atlas-csm1-candidate.png'
    mtl_name = model + '-preview.mtl'
    rgba = gim_module.rgba(texture, csm1=True)
    gim_module.write_png(output / texture_name, texture['width'], texture['height'], rgba)
    # A second, explicitly diagnostic PNG preserves palette RGB even when the
    # stored alpha is zero. Browser canvas would discard that RGB on drawing.
    opaque_rgba = bytearray(rgba)
    opaque_rgba[3::4] = b'\xff' * (len(opaque_rgba) // 4)
    opaque_name = model + '-atlas-opaque-diagnostic.png'
    gim_module.write_png(output / opaque_name, texture['width'], texture['height'], opaque_rgba)
    # Embed local observations so the interactive preview works without a server.
    viewer = Path(__file__).with_name('itesu_preview_viewer.html').read_text(encoding='utf-8')
    visual_batches = [{'off': batch['offset'], 'p': batch['positions_f32'], 'uv': batch['uvs_f32']} for batch in batches['batches']]
    texture_url = 'data:image/png;base64,' + base64.b64encode((output / texture_name).read_bytes()).decode('ascii')
    opaque_url = 'data:image/png;base64,' + base64.b64encode((output / opaque_name).read_bytes()).decode('ascii')
    viewer = viewer.replace('__MODEL_JSON__', json.dumps(visual_batches, separators=(',', ':'), allow_nan=False))
    viewer = viewer.replace('__TEXTURE_URL__', json.dumps(texture_url))
    viewer = viewer.replace('__OPAQUE_URL__', json.dumps(opaque_url))
    hud_url = ('data:image/png;base64,' + base64.b64encode(hud_bytes).decode('ascii')) if hud_bytes else None
    viewer = viewer.replace('__HUD_URL__', json.dumps(hud_url))
    viewer = viewer.replace('__VIEW_OPTIONS__', json.dumps(MODELS[model]['view'], separators=(',', ':')))
    viewer = viewer.replace('__MODEL_NAME__', model)
    (output / 'inspect-cockpit.html').write_text(viewer, encoding='utf-8')
    (output / mtl_name).write_text(
        '# Provisional CSM1 palette and part-9 texture association.\n'
        'newmtl atlas_candidate\nKd 1 1 1\nKa 0 0 0\nKs 0 0 0\n'
        'map_Kd ' + texture_name + '\n', encoding='utf-8')
    face_count = 0
    with (output / (model + '-preview.obj')).open('x', encoding='utf-8', newline='\n') as obj:
        obj.write('# PROVISIONAL: triangle strip hypothesis; texture binding unverified.\n')
        obj.write('mtllib ' + mtl_name + '\nusemtl atlas_candidate\n')
        first = 1
        for batch in batches['batches']:
            obj.write('g source_batch_%x\n' % batch['offset'])
            for p in batch['positions_f32']:
                obj.write('v %.9g %.9g %.9g\n' % tuple(p))
            for uv in batch['uvs_f32']:
                # OBJ's conventional origin is bottom-left; GIM preview uses top-left.
                obj.write('vt %.9g %.9g\n' % (uv[0], 1.0 - uv[1]))
            for i in range(len(batch['positions_f32']) - 2):
                indices = (i, i + 1, i + 2) if i % 2 == 0 else (i + 1, i, i + 2)
                vertices = [first + x for x in indices]
                obj.write('f ' + ' '.join('%d/%d' % (x, x) for x in vertices) + '\n')
                face_count += 1
            first += len(batch['positions_f32'])
    expected = MODELS[model]
    report = {'schema': 'acz-provisional-cockpit-preview/v1', 'model': model, 'mpl_sha256': expected['mpl'],
              'acm_sha256': expected['acm'], 'gim_sha256': expected['gim'],
              'batches': batches['batch_count'], 'vertices': batches['vertex_source_count'],
              'hypothetical_triangle_count': face_count,
              'assumptions': ['Each source batch is a triangle strip; VU/GIF execution has not been decoded.',
                              'All part-2 batches use part-9 texture; texture binding is unverified.',
                              'CSM1-style palette and UV vertical convention are visual candidates.',
                              'Normals and game blend state are not exported.',
                              'Optional per-batch HUD placement uses a static opaque diagnostic PNG, not live data or runtime alpha.',
                              'The F-16 mock requires explicit opt-in; its screen functions and left/right remain unverified.',
                              'Camera location, cockpit orientation, and physical scale remain unknown.']}
    (output / 'preview-metadata.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mpl', type=Path, help='user-extracted MPL matching the chosen model')
    parser.add_argument('--model', choices=sorted(MODELS), default='itesu')
    parser.add_argument('--out', type=Path, required=True, help='new directory outside the repository')
    parser.add_argument('--hud-preview', type=Path, help='optional 2560x1440 recorded hud-on-black PNG for a static placement mockup')
    parser.add_argument('--experimental-f16-screen-mock', action='store_true', help='explicitly opt in to the unverified F-16 screen assignment, for offline comparison only')
    args = parser.parse_args()
    try:
        report = write_preview(args.mpl, args.out, args.hud_preview, args.model, args.experimental_f16_screen_mock)
    except (OSError, ValueError, KeyError) as exc:
        parser.error(str(exc))
    print('Wrote provisional preview: {vertices} vertices, {hypothetical_triangle_count} hypothesized triangles'.format(**report))


if __name__ == '__main__':
    main()
