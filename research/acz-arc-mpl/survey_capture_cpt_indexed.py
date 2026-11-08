"""Inspect index-referenced positions in a pinned, saved E10 capture snapshot.

No replay is performed. Byte lifetime and aircraft identity are not established.
"""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

from survey_capture_cpt_vertices import BLOB_SHA256, PSOS, XML_SHA256, named, vertex_view

INDEX_SHA256 = '4c8e4f5698edc575682378f0dbf8985d1753ee4e274e65743b456792e92021a1'


def index_view(chunk):
    view = chunk.find('./struct[@name="pView"]')
    if view is None:
        return None
    location = view.find('./struct[@name="BufferLocation"]')
    if location is None:
        return None
    fields = (named(location, 'ResourceId', 'Buffer'), named(location, 'uint', 'Offset'),
              named(view, 'uint', 'SizeInBytes'), named(view, 'enum', 'Format'))
    if None in fields:
        return None
    return (fields[0], *(int(x) for x in fields[1:]))


def draws_with_state(chunks):
    states = {}
    for chunk in chunks:
        op = chunk.get('name', '').rsplit('::', 1)[-1]
        command = named(chunk, 'ResourceId', 'pCommandList')
        if op == 'Reset':
            command = named(chunk, 'ResourceId', 'CommandList')
            if command is not None:
                states[command] = {'pso': named(chunk, 'ResourceId', 'pInitialState'), 'vb': None, 'ib': None, 'topology': None}
            continue
        if command is None:
            continue
        state = states.setdefault(command, {'pso': None, 'vb': None, 'ib': None, 'topology': None})
        if op == 'SetPipelineState':
            state['pso'] = named(chunk, 'ResourceId', 'pPipelineState')
        elif op == 'IASetVertexBuffers':
            start = named(chunk, 'uint', 'StartSlot')
            if start is None or int(start) == 0:
                state['vb'] = vertex_view(chunk)
        elif op == 'IASetIndexBuffer':
            state['ib'] = index_view(chunk)
        elif op == 'IASetPrimitiveTopology':
            state['topology'] = named(chunk, 'enum', 'PrimitiveTopology')
        elif op == 'DrawIndexedInstanced' and state['pso'] in PSOS:
            fields = (named(chunk, 'uint', 'IndexCountPerInstance'),
                      named(chunk, 'uint', 'StartIndexLocation'), named(chunk, 'int', 'BaseVertexLocation'))
            if None not in fields:
                yield {'chunk': int(chunk.get('chunkIndex')), **state,
                       'count': int(fields[0]), 'start': int(fields[1]), 'base': int(fields[2])}


def referenced_positions(draw, vertices, indices):
    vb, ib = draw['vb'], draw['ib']
    if vb is None or ib is None:
        return {'error': 'missing buffer view'}
    resource, offset, size, stride = vb
    ir, io, insize, fmt = ib
    if (resource != '329' or ir != '331' or stride != 64 or fmt != 57 or
            min(offset, size, io, insize, draw['start'], draw['count']) < 0 or
            offset + size > len(vertices) or io + insize > len(indices) or
            2 * (draw['start'] + draw['count']) > insize):
        return {'error': 'unsupported or out-of-bounds view'}
    raw = struct.unpack_from('<%dH' % draw['count'], indices, io + 2 * draw['start'])
    if draw.get('topology') != '5' or draw.get('cut_mode') != '1':
        return {'error': 'triangle-strip topology and 0xffff cut mode not established'}
    lengths = []
    length = 0
    for index in raw:
        if index == 0xffff:
            if length:
                lengths.append(length)
                length = 0
        else:
            length += 1
    if length:
        lengths.append(length)
    ordinary = sorted(set(i + draw['base'] for i in raw if i != 0xffff))
    if any(i < 0 or i * stride + 12 > size for i in ordinary):
        return {'error': 'referenced vertex outside bound view'}
    points = [struct.unpack_from('<3f', vertices, offset + i * stride) for i in ordinary]
    if not points or not all(math.isfinite(x) for point in points for x in point):
        return {'error': 'empty or non-finite referenced positions'}
    return {'unique_referenced_vertices': len(points), 'index_sentinel_count': raw.count(0xffff),
            'strip_length_counts': dict(sorted(Counter(lengths).items())),
            'bounds': [[min(p[a] for p in points), max(p[a] for p in points)] for a in range(3)]}


def pipeline_cut_modes(chunks):
    modes = {}
    for chunk in chunks:
        if chunk.get('name', '').endswith('::CreateGraphicsPipeline'):
            pso = named(chunk, 'ResourceId', 'pPipelineState')
            mode = chunk.find('.//enum[@name="IBStripCutValue"]')
            if pso in PSOS and mode is not None:
                modes[pso] = mode.text
    return modes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ('xml', 'vertices', 'indices', 'out'):
        parser.add_argument('--' + arg, type=Path, required=True)
    args = parser.parse_args()
    try:
        data = [args.xml.read_bytes(), args.vertices.read_bytes(), args.indices.read_bytes()]
        if [hashlib.sha256(x).hexdigest() for x in data] != [XML_SHA256, BLOB_SHA256, INDEX_SHA256]:
            raise ValueError('input hash does not match pinned E10 evidence')
        chunks = ET.fromstring(data[0]).find('chunks')
        if chunks is None:
            raise ValueError('missing chunks')
        draws = list(draws_with_state(chunks))
        cut_modes = pipeline_cut_modes(chunks)
        for draw in draws:
            draw['cut_mode'] = cut_modes.get(draw['pso'])
            draw['result'] = referenced_positions(draw, data[1], data[2])
        report = {'schema': 'acz-e10-cpt-indexed-survey/v1', 'draws': draws, 'pipeline_strip_cut_modes': cut_modes,
                  'limitations': ['The saved upload buffers are end-of-recording snapshots; draw-time bytes are not established.',
                                  'Only verified triangle-strip topology and 0xffff strip-cut mode are resolved.',
                                  'These PSOs are a subset; aircraft, asset, surface and actual rasterization are unproven.']}
        with args.out.open('x', encoding='utf-8', newline='\n') as output:
            json.dump(report, output, indent=2, allow_nan=False)
            output.write('\n')
        print('Surveyed %d indexed draw records; %d resolved' %
              (len(draws), sum('error' not in d['result'] for d in draws)))
    except (OSError, ValueError, ET.ParseError, struct.error) as exc:
        parser.error(str(exc))


if __name__ == '__main__':
    main()
