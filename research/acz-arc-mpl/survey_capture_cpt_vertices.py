"""Survey pinned saved E10 DrawCPT vertex-buffer views offline.

This reads the captured XML command stream and one saved upload resource. It
does not replay the capture, attach to a process, or infer aircraft identity.
Only the four pinned E10 DrawCPT PSOs and resource 329 are included.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

XML_SHA256 = 'df00ecfb65bd8eec75d22152be92829b90fb3879ef068ca171f39e492e4a609a'
BLOB_SHA256 = '02474f0403795c16f9e25fd9777d1a54137bd4b206819f738af329b4aa10ecb4'
PSOS = frozenset(('198101', '198105', '198109', '198121'))


def named(element, tag, name):
    found = element.find('./%s[@name="%s"]' % (tag, name))
    return None if found is None else found.text


def vertex_view(chunk):
    start = named(chunk, 'uint', 'StartSlot')
    views = chunk.find('./array[@name="pViews"]')
    if start is None or views is None or int(start) != 0 or len(views) == 0:
        return None
    view = views[0]
    location = view.find('./struct[@name="BufferLocation"]')
    if location is None:
        return None
    resource = named(location, 'ResourceId', 'Buffer')
    offset = named(location, 'uint', 'Offset')
    size = named(view, 'uint', 'SizeInBytes')
    stride = named(view, 'uint', 'StrideInBytes')
    if None in (resource, offset, size, stride):
        return None
    return (resource, int(offset), int(size), int(stride))


def selected_draws(chunks):
    states = {}
    draws = []
    for chunk in chunks:
        operation = chunk.get('name', '').rsplit('::', 1)[-1]
        command = named(chunk, 'ResourceId', 'pCommandList')
        if operation == 'Reset':
            command = named(chunk, 'ResourceId', 'CommandList')
            if command is not None:
                states[command] = {'pso': named(chunk, 'ResourceId', 'pInitialState'), 'view': None}
            continue
        if command is None:
            continue
        state = states.setdefault(command, {'pso': None, 'view': None})
        if operation == 'SetPipelineState':
            state['pso'] = named(chunk, 'ResourceId', 'pPipelineState')
        elif operation == 'IASetVertexBuffers':
            start = named(chunk, 'uint', 'StartSlot')
            if start is None or int(start) == 0:
                state['view'] = vertex_view(chunk)
        elif operation in ('DrawIndexedInstanced', 'DrawInstanced') and state['pso'] in PSOS:
            draws.append({'chunk_index': int(chunk.get('chunkIndex')), 'command_list': command,
                          'pso': state['pso'], 'operation': operation, 'view': state['view']})
    return draws


def summarize_view(blob, view):
    resource, offset, size, stride = view
    if resource != '329' or stride != 64 or offset < 0 or size <= 0 or size % stride or offset + size > len(blob):
        return None
    points = [struct.unpack_from('<3f', blob, offset + step) for step in range(0, size, stride)]
    if not all(math.isfinite(value) for point in points for value in point):
        return None
    return {'resource_id': resource, 'offset': offset, 'size_bytes': size, 'stride': stride,
            'vertices': len(points), 'position_bounds': [[min(p[i] for p in points), max(p[i] for p in points)] for i in range(3)],
            'view_sha256': hashlib.sha256(blob[offset:offset + size]).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xml', type=Path, required=True)
    parser.add_argument('--blob', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    try:
        xml = args.xml.read_bytes()
        blob = args.blob.read_bytes()
        if hashlib.sha256(xml).hexdigest() != XML_SHA256 or hashlib.sha256(blob).hexdigest() != BLOB_SHA256:
            raise ValueError('input hash does not match pinned E10 evidence')
        root = ET.fromstring(xml)
        chunks = root.find('chunks')
        if chunks is None:
            raise ValueError('missing capture chunks')
        draws = selected_draws(chunks)
        for draw in draws:
            draw['view'] = summarize_view(blob, draw['view']) if draw['view'] else None
        report = {'schema': 'acz-e10-cpt-buffer-survey/v1', 'xml_sha256': XML_SHA256, 'blob_sha256': BLOB_SHA256,
                  'pso_ids': sorted(PSOS), 'draws': draws,
                  'limitations': ['The bound-view range can contain vertices not referenced by a particular draw.',
                    'The saved upload is an end-of-recording snapshot; draw-time byte lifetime is not established.',
                    'These PSOs are a subset, not proof that all cockpit draws were surveyed.',
                    'No asset, aircraft, texture binding, or runtime identity is inferred.']}
        with args.out.open('x', encoding='utf-8', newline='\n') as output:
            json.dump(report, output, indent=2, allow_nan=False)
            output.write('\n')
    except (OSError, ValueError, ET.ParseError) as exc:
        parser.error(str(exc))
    print('Surveyed %d matching E10 draw records' % len(draws))


if __name__ == '__main__':
    main()
