"""Offline: list the frame-11906 draws (list 282) with PSO shaders, depth, VB/IB and decode ps2_v vertices (read-only)."""
import sys, struct, json, os
import xml.etree.ElementTree as ET
import numpy as np

CAP = r'%USERPROFILE%\ACZero-captures\E10'
XML = os.path.join(CAP, 'frame11906.zip.xml')
BLOBS = os.path.join(CAP, 'blobs')


def dxil_hash(blob_id):
    p = os.path.join(BLOBS, '%06d' % int(blob_id))
    if not os.path.exists(p) or os.path.getsize(p) == 0:
        return None
    b = open(p, 'rb').read()
    if b[:4] != b'DXBC':
        return None
    cnt = struct.unpack_from('<I', b, 28)[0]
    for i in range(cnt):
        off = struct.unpack_from('<I', b, 32 + 4 * i)[0]
        if b[off:off + 4] == b'HASH':
            return b[off + 12: off + 28].hex()
    return None


def find(el, name):
    for c in el.iter():
        if c.get('name') == name:
            return c
    return None


psos = {}
draws = []
state = dict(pso=None, vb=None, ib=None, topo=None, rc={}, rsrv={})
ordinal = 0
for ev, ch in ET.iterparse(XML, events=('end',)):
    if ch.tag != 'chunk':
        continue
    name = ch.get('name')
    idx = int(ch.get('chunkIndex'))
    if name in ('ID3D12Device::CreateGraphicsPipeline', 'ID3D12Device2::CreatePipelineState'):
        d = {}
        for st in ('VS', 'PS', 'GS'):
            s = find(ch, st)
            if s is not None:
                b = s.find("buffer")
                d[st] = dxil_hash(b.text) if b is not None and b.get('byteLength') != '0' else None
        ds = find(ch, 'DepthStencilState')
        if ds is not None:
            de = find(ds, 'DepthEnable'); df = find(ds, 'DepthFunc'); dw = find(ds, 'DepthWriteMask')
            d['depth'] = (de.text if de is not None else '?', df.get('string') if df is not None else '?', dw.get('string') if dw is not None else '?')
        out = None
        for c in ch:
            if c.tag == 'ResourceId' and c.get('name') in ('ppPipelineState', 'pPipelineState'):
                out = c.text
        if out:
            psos[out] = d
    else:
        cl = None
        for c in ch:
            if c.tag == 'ResourceId' and c.get('name') == 'pCommandList':
                cl = c.text
        if cl == '282':
            if name.endswith('SetPipelineState'):
                state['pso'] = ch.find("ResourceId[@name='pPipelineState']").text
            elif name.endswith('IASetVertexBuffers'):
                v = find(ch, 'pViews')
                s = v.find('struct') if v is not None else None
                if s is not None:
                    bl = s.find("struct[@name='BufferLocation']")
                    state['vb'] = (bl.find('ResourceId').text, int(bl.find('uint').text), int(s.find("uint[@name='SizeInBytes']").text), int(s.find("uint[@name='StrideInBytes']").text))
            elif name.endswith('IASetIndexBuffer'):
                v = find(ch, 'pView')
                bl = v.find("struct[@name='BufferLocation']")
                state['ib'] = (bl.find('ResourceId').text, int(bl.find('uint').text), int(v.find("uint[@name='SizeInBytes']").text), v.find("enum[@name='Format']").get('string'))
            elif name.endswith('IASetPrimitiveTopology'):
                state['topo'] = ch.find("enum").get('string')
            elif name.endswith('SetGraphicsRoot32BitConstants'):
                p = int(ch.find("uint[@name='RootParameterIndex']").text)
                arr = ch.find("array")
                vals = [int(u.text) for u in arr] if arr is not None else []
                state['rc'][p] = [struct.unpack('<f', struct.pack('<I', v & 0xffffffff))[0] for v in vals]
            elif name.endswith('SetGraphicsRootShaderResourceView'):
                p = int(ch.find("uint[@name='RootParameterIndex']").text)
                bl = ch.find("struct[@name='BufferLocation']")
                state['rsrv'][p] = (bl.find('ResourceId').text, int(bl.find('uint').text))
            elif name.endswith('DrawIndexedInstanced') or name.endswith('DrawInstanced'):
                u = {c.get('name'): int(c.text) for c in ch if c.tag == 'uint' or c.tag == 'int'}
                draws.append(dict(ordinal=ordinal, chunk=idx, call=name.split('::')[1], args=u, pso=state['pso'], vb=state['vb'], ib=state['ib'] if 'Indexed' in name else None,
                                  topo=state['topo'], rc={k: v[:] for k, v in state['rc'].items()}, rsrv=dict(state['rsrv'])))
                ordinal += 1
    ch.clear()

for d in draws:
    d['shaders'] = psos.get(d['pso'], {})
json.dump(draws, open(sys.argv[1], 'w'), indent=0, default=str)
print(len(draws), 'draws;', len(psos), 'psos')
for d in draws:
    s = d['shaders']
    print(d['ordinal'], d['chunk'], d['call'], d['args'].get('IndexCountPerInstance', d['args'].get('VertexCountPerInstance')),
          (s.get('VS') or '-')[:8], (s.get('GS') or '-')[:8], (s.get('PS') or '-')[:8], s.get('depth'), d['topo'], d['vb'], d['ib'])
