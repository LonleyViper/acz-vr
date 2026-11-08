"""Bounded, loss-aware VIF/GIF inspection. Input is an already-extracted stream.

No archive reader, VU interpreter, engine layout inference or runtime access.
All offsets are byte offsets in the supplied block. See decoder-tooling.md.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

MAX_INPUT = 16 * 1024 * 1024
MAX_RECORDS = 65536
VIF_NAMES = {0x00: 'NOP', 0x01: 'STCYCL', 0x02: 'OFFSET', 0x03: 'BASE',
             0x04: 'ITOP', 0x05: 'STMOD', 0x06: 'MSKPATH3', 0x07: 'MARK',
             0x10: 'FLUSHE', 0x11: 'FLUSH', 0x13: 'FLUSHA', 0x14: 'MSCAL',
             0x15: 'MSCALF', 0x17: 'MSCNT', 0x20: 'STMASK', 0x30: 'STROW',
             0x31: 'STCOL', 0x4a: 'MPG', 0x50: 'DIRECT', 0x51: 'DIRECTHL'}
REG_NAMES = {0: 'PRIM', 1: 'RGBAQ', 2: 'ST', 3: 'UV', 4: 'XYZF2',
             5: 'XYZ2', 6: 'TEX0_1', 7: 'TEX0_2', 8: 'CLAMP_1',
             9: 'CLAMP_2', 10: 'FOG', 12: 'XYZF3', 13: 'XYZ3',
             14: 'A+D', 15: 'NOP'}
PRIMS = ['point', 'line', 'line_strip', 'triangle', 'triangle_strip',
         'triangle_fan', 'sprite', 'reserved']


def span(data, start, end, kind, reason=None):
    item = {'offset': start, 'end': end, 'kind': kind,
            'raw_hex': data[start:end].hex()}
    if reason:
        item['reason'] = reason
    return item


def decode_gif(data, base=0, _budget=None):
    """Decode GIF packet framing and register observations, never a VU mesh.

    BASE relocates observations only; raw byte coverage remains contiguous.
    IMAGE/IMAGE2 data is opaque. EOP ends a packet, not the enclosing transfer.
    """
    if len(data) > MAX_INPUT or type(base) is not int or base < 0:
        raise ValueError('GIF input exceeds limit or invalid base')
    budget = [MAX_RECORDS] if _budget is None else _budget
    out = {'tags': [], 'coverage': [], 'issues': [], 'complete': True}
    pos = 0
    active_prim = None

    def cover(a, b, kind, reason=None):
        if a == b:
            return
        item = span(data, a, b, kind, reason)
        item['offset'] += base
        item['end'] += base
        out['coverage'].append(item)

    while pos < len(data):
        start = pos
        if budget[0] <= 0:
            cover(pos, len(data), 'unknown_tail', 'inspection record limit')
            out['issues'].append({'offset': base + pos, 'reason': 'inspection record limit'})
            out['complete'] = False
            break
        budget[0] -= 1
        if len(data) - pos < 16:
            cover(pos, len(data), 'rejected', 'truncated GIF tag')
            out['issues'].append({'offset': base + pos, 'reason': 'truncated GIF tag'})
            out['complete'] = False
            break
        lo, hi = struct.unpack_from('<QQ', data, pos)
        nloop, eop = lo & 0x7fff, (lo >> 15) & 1
        pre, prim, flg = (lo >> 46) & 1, (lo >> 47) & 0x7ff, (lo >> 58) & 3
        nreg = (lo >> 60) & 15 or 16
        regs = [(hi >> (4 * i)) & 15 for i in range(nreg)]
        tag = {'offset': base + start, 'nloop': nloop, 'eop': eop, 'pre': pre,
               'prim': prim, 'mode': ['PACKED', 'REGLIST', 'IMAGE', 'IMAGE2'][flg],
               'registers': regs, 'observations': []}
        out['tags'].append(tag)
        cover(pos, pos + 16, 'GIFtag')
        pos += 16
        size = nloop * (16 if flg >= 2 else nreg * (16 if flg == 0 else 8))
        padded = (size + 15) & ~15
        if padded > len(data) - pos:
            cover(pos, len(data), 'rejected', 'truncated GIF payload')
            out['issues'].append({'offset': base + pos, 'reason': 'truncated GIF payload',
                                  'expected_bytes': padded, 'available_bytes': len(data) - pos})
            out['complete'] = False
            break
        if flg < 2 and nloop * nreg > budget[0]:
            cover(pos, len(data), 'unknown_tail', 'inspection record limit')
            out['issues'].append({'offset': base + pos, 'reason': 'inspection record limit'})
            out['complete'] = False
            break
        if flg < 2:
            budget[0] -= nloop * nreg
        # PRE applies only to PACKED; PRIM can also be written as a register.
        if pre and flg == 0:
            active_prim = prim
        tag['initial_prim'] = active_prim
        if flg >= 2:
            cover(pos, pos + size, 'opaque_image', 'texture/image semantics unresolved')
        else:
            width = 16 if flg == 0 else 8
            for i in range(nloop * nreg):
                off = pos + i * width
                reg = regs[i % nreg]
                raw = data[off:off + width]
                value = int.from_bytes(raw[:8], 'little')
                packed = flg == 0
                obs = {'offset': base + off, 'register': REG_NAMES.get(reg, 'reserved'),
                       'register_id': reg, 'raw_hex': raw.hex()}
                if packed and reg == 14:
                    reg = raw[8]  # A+D address is the low 8 bits of the upper word.
                    obs['address'] = reg
                    obs['register'] = REG_NAMES.get(reg, 'GS_register_0x%02x' % reg)
                    packed = False  # A+D carries the ordinary 64-bit GS layout.
                elif not packed and reg == 14:
                    obs['unresolved'] = 'A+D is invalid in REGLIST'
                    reg = -1
                if reg == 0:
                    active_prim = value & 0x7ff
                    obs['prim'] = active_prim
                elif reg == 1:
                    if packed:
                        obs['rgba'] = [raw[j] for j in (0, 4, 8, 12)]
                        obs['q'] = 'GIF internal Q; not decoded as an RGBA byte'
                    else:
                        obs['rgba'] = list(raw[:4])
                        obs['q_bits'] = value >> 32
                elif reg == 2:
                    obs['st_bits'] = list(struct.unpack_from('<II', raw))
                    if packed:
                        obs['q_bits'] = struct.unpack_from('<I', raw, 8)[0]
                    obs['unresolved'] = 'ST requires Q and GS texture state; not model UV'
                elif reg == 3:
                    obs['uv_fixed_10_4'] = ([value & 0x3fff, (int.from_bytes(raw[4:8], 'little') & 0x3fff)]
                                                if packed else [value & 0x3fff, (value >> 16) & 0x3fff])
                    obs['unresolved'] = 'texel coordinates, normalization/texture unresolved'
                elif reg in (4, 5, 12, 13):
                    if packed:
                        x, y, z, w = struct.unpack('<IIII', raw)
                        xyz = [x & 0xffff, y & 0xffff,
                               (z >> 4) & 0xffffff if reg in (4, 12) else z]
                        adc = (w >> 15) & 1
                    else:
                        xyz = [value & 0xffff, (value >> 16) & 0xffff,
                               (value >> 32) & (0xffffff if reg in (4, 12) else 0xffffffff)]
                        adc = 0
                    obs.update({'xyz_gs': xyz, 'coordinate_space': 'GS fixed screen XY / integer Z',
                                'draw_kick': reg in (4, 5) and not adc,
                                'adc': adc, 'prim': active_prim,
                                'prim_type_field': None if active_prim is None else PRIMS[active_prim & 7],
                                'topology_evidence': 'PRIM low three bits only; no assembled topology'})
                    obs['unresolved'] = 'GS context/PRMODE state and vertex queue not evaluated; no model topology'
                else:
                    obs.setdefault('unresolved', 'register preserved; GS effects not evaluated')
                tag['observations'].append(obs)
                cover(off, off + width, 'GIFregister')
            if padded != size:
                cover(pos + size, pos + padded, 'GIFpadding')
        pos += padded
        tag['end'] = base + pos
    return out


def decode_vif(data, start=0):
    """Decode VIF1 framing from an explicit byte offset to the end of a block."""
    if len(data) > MAX_INPUT:
        raise ValueError('input exceeds 16 MiB inspection limit')
    if type(start) is not int or not 0 <= start <= len(data) or start % 4:
        raise ValueError('start must be an aligned in-range byte offset')
    result = {'schema': 'acz-vif-gif-1', 'input_size': len(data),
              'sha256': hashlib.sha256(data).hexdigest(), 'stream_offset': start,
              'complete': True, 'commands': [], 'coverage': [], 'issues': [],
              'limitations': ['No ACM header inference; prefix is opaque.',
                              'No VU execution, DMA-chain traversal or engine attribute inference.',
                              'UNPACK lanes are source values, not positions or triangles.',
                              'GIF observations are GS writes, not model vertices.',
                              'GS state before each DIRECT is unknown; no cross-transfer state execution.',
                              'DIRECT requires qword-aligned payload in supplied block coordinates.']}
    cover = result['coverage']
    if start:
        cover.append(span(data, 0, start, 'opaque_prefix', 'caller-selected VIF start'))
    pos = start
    budget = [MAX_RECORDS]
    cl, wl = 1, 1
    state = {'cl': cl, 'wl': wl, 'mode': 0, 'mask': 0, 'row': [0]*4, 'col': [0]*4}

    def reject(off, reason):
        result['complete'] = False
        result['issues'].append({'offset': off, 'reason': reason})

    while pos < len(data):
        off = pos
        if budget[0] <= 0:
            cover.append(span(data, off, len(data), 'unknown_tail', 'inspection record limit'))
            reject(off, 'inspection record limit')
            break
        budget[0] -= 1
        if len(data) - off < 4:
            cover.append(span(data, off, len(data), 'rejected', 'truncated VIFcode'))
            reject(off, 'truncated VIFcode')
            break
        word = struct.unpack_from('<I', data, off)[0]
        imm, num, cmd = word & 0xffff, (word >> 16) & 255, (word >> 24) & 127
        entry = {'offset': off, 'code_hex': data[off:off+4].hex(), 'cmd': cmd,
                 'irq': bool(word >> 31), 'immediate': imm, 'num_field': num,
                 'name': VIF_NAMES.get(cmd, 'UNKNOWN'), 'state_before': dict(state)}
        result['commands'].append(entry)
        cover.append(span(data, off, off+4, 'VIFcode'))
        pos += 4
        size = 0
        unpack = 0x60 <= cmd <= 0x7f
        if unpack:
            vn, vl = ((cmd >> 2) & 3) + 1, cmd & 3
            entry['name'] = 'UNPACK'
            count = num or 256
            if not cl or not wl:
                entry.update(status='rejected', end=len(data),
                             reason='zero STCYCL field unsupported; source length not guessed')
                reject(off, entry['reason'])
                if pos < len(data):
                    cover.append(span(data, pos, len(data), 'unknown_tail', entry['reason']))
                break
            if vl == 3 and vn != 4:
                entry['status'] = 'rejected'
                entry['end'] = len(data)
                entry['reason'] = 'reserved UNPACK format; length unknown'
                reject(off, 'reserved UNPACK format; length unknown')
                if pos < len(data):
                    cover.append(span(data, pos, len(data), 'unknown_tail', 'unsafe to resume after reserved UNPACK'))
                break
            bits = [32, 16, 8, 5][vl]
            source_count = count if wl <= cl else (count // wl) * cl + min(count % wl, cl)
            size = source_count * (2 if vl == 3 else vn * (bits // 8))
            entry['unpack'] = {'components': vn, 'bits': bits, 'count': count,
                               'source_count': source_count, 'unsigned': bool(imm & 0x4000),
                               'masked': bool(cmd & 0x10), 'tops_relative': bool(imm & 0x8000),
                               'address': imm & 0x3ff,
                               'unresolved': 'destination values require VU state, mask, STMOD, row/col and cycle effects'}
        elif cmd not in VIF_NAMES:
            entry['status'] = 'unknown'
            entry['end'] = len(data)
            entry['reason'] = 'unknown VIF command; payload length unknown'
            reject(off, 'unknown VIF command; payload length unknown')
            if pos < len(data):
                cover.append(span(data, pos, len(data), 'unknown_tail', 'no resynchronization guessed'))
            break
        elif cmd in (0x20, 0x30, 0x31):
            size = 4 if cmd == 0x20 else 16
        elif cmd == 0x4a:
            size = (num or 256) * 8
            entry['unresolved'] = 'VU microcode preserved without execution'
        elif cmd in (0x50, 0x51):
            if imm == 0:
                entry.update(status='rejected', end=len(data),
                             reason='zero DIRECT count unsupported; transfer length not guessed')
                reject(off, entry['reason'])
                if pos < len(data):
                    cover.append(span(data, pos, len(data), 'unknown_tail', entry['reason']))
                break
            size = imm * 16
            if pos % 16:
                entry.update(status='rejected', end=len(data),
                             reason='unaligned DIRECT payload; transport padding not guessed')
                reject(off, entry['reason'])
                if pos < len(data):
                    cover.append(span(data, pos, len(data), 'unknown_tail', entry['reason']))
                break
        padded = (size + 3) & ~3
        entry['payload_offset'] = pos
        entry['expected_payload_bytes'] = size
        if padded > len(data) - pos:
            entry['status'] = 'rejected'
            entry['end'] = len(data)
            if pos < len(data):
                cover.append(span(data, pos, len(data), 'rejected', 'truncated VIF payload/padding'))
            reject(off, 'truncated VIF payload/padding')
            break
        payload = data[pos:pos+size]
        if size:
            cover.append(span(data, pos, pos+size, 'VIFpayload'))
            entry['payload_hex'] = payload.hex()
        if padded != size:
            cover.append(span(data, pos+size, pos+padded, 'VIFpadding'))
        if unpack:
            fmt = entry['unpack']
            fmt['lane_encoding'] = ('packed RGB5A1' if fmt['bits'] == 5 else
                                    'raw uint32 bits' if fmt['bits'] == 32 else
                                    'unsigned integer' if fmt['unsigned'] else 'signed integer')
            if fmt['source_count'] > budget[0]:
                entry['unresolved'] = 'source vectors omitted: inspection record limit'
                reject(off, entry['unresolved'])
                pos += padded
                entry.update(end=pos, status='framed_opaque')
                continue
            budget[0] -= fmt['source_count']
            if fmt['bits'] == 5:
                entry['source_vectors'] = [[v & 31, (v >> 5) & 31, (v >> 10) & 31, (v >> 15) & 1]
                                           for (v,) in struct.iter_unpack('<H', payload)]
            else:
                code = {8: 'B', 16: 'H', 32: 'I'}[fmt['bits']]
                if not fmt['unsigned'] and fmt['bits'] != 32:
                    code = code.lower()
                values = [v[0] for v in struct.iter_unpack('<' + code, payload)]
                entry['source_vectors'] = [values[i:i+fmt['components']]
                                           for i in range(0, len(values), fmt['components'])]
        elif cmd in (0x50, 0x51):
            entry['gif'] = decode_gif(payload, pos, budget)
            if not entry['gif']['complete']:
                reject(off, 'invalid nested GIF stream; next VIF boundary retained from DIRECT length')
        elif cmd == 1:
            cl, wl = imm & 255, (imm >> 8) & 255
            state.update(cl=cl, wl=wl)
            if not cl or not wl:
                entry['unresolved'] = 'zero cycle field retained raw; subsequent UNPACK requires validated length semantics'
        elif cmd == 5:
            state['mode'] = imm & 3
            if state['mode'] == 3:
                entry['unresolved'] = 'reserved STMOD mode'
        elif cmd == 0x20:
            state['mask'] = struct.unpack('<I', payload)[0]
        elif cmd in (0x30, 0x31):
            state['row' if cmd == 0x30 else 'col'] = list(struct.unpack('<IIII', payload))
        elif cmd in (0x14, 0x15, 0x17):
            entry['unresolved'] = 'VU execution may change memory/state; not simulated'
        pos += padded
        entry['end'] = pos
        entry['status'] = 'framed'
    result['state_final'] = state
    return result


def declared_attributes(result, declarations):
    """Opt-in source-lane annotations, never inferred engine facts.

    Each declaration identifies an UNPACK by byte offset, role, numeric storage
    and a nonempty evidence label. These are caller assertions, not validation
    of that evidence. Cycle/mask/STMOD transforms deliberately not applied.
    """
    attrs = []
    by_offset = {c['offset']: c for c in result['commands']}
    seen = set()
    for d in declarations:
        if not isinstance(d, dict) or set(d) != {'offset', 'role', 'storage', 'evidence'}:
            raise ValueError('declaration requires exactly offset, role, storage, evidence')
        off, role, storage, evidence = d['offset'], d['role'], d['storage'], d['evidence']
        if type(off) is not int or off in seen or not isinstance(evidence, str) or not evidence.strip():
            raise ValueError('invalid/duplicate declaration offset or missing evidence')
        seen.add(off)
        command = by_offset.get(off, {})
        fmt = command.get('unpack', {})
        required = {'position': (3,), 'normal': (3,), 'uv': (2,), 'colour': (3, 4)}
        if (not isinstance(role, str) or role not in required or fmt.get('components') not in required[role]
                or command.get('status') != 'framed' or storage not in ('integer', 'float32')):
            raise ValueError('declaration does not match a complete supported source vector')
        if storage == 'float32' and fmt['bits'] != 32:
            raise ValueError('float32 declaration requires 32-bit lanes')
        vectors = command['source_vectors']
        if storage == 'float32':
            vectors = [[struct.unpack('<f', struct.pack('<I', v))[0] for v in row] for row in vectors]
        if any(not math.isfinite(v) for row in vectors for v in row):
            raise ValueError('nonfinite declared attribute')
        attrs.append({'offset': off, 'role': role, 'storage': storage, 'evidence': evidence,
                      'authority': 'unverified caller declaration of source lanes only', 'vectors': vectors})
    return attrs


def export_text(result, declarations=()):
    """Return stable OBJ, MTL and complete JSON sidecar; no inferred faces."""
    attrs = declared_attributes(result, declarations)
    obj = ['# Source-lane observations; no inferred vertex associations or faces.', 'mtllib model.mtl']
    for a in attrs:
        obj.append('o %s_at_%08x' % (a['role'], a['offset']))
        token = {'position': 'v', 'normal': 'vn', 'uv': 'vt'}.get(a['role'])
        if token:
            obj.extend(token + ' ' + ' '.join(format(v, '.9g') for v in row) for row in a['vectors'])
        else:
            obj.append('# colours retained in sidecar; no vertex association evidenced')
    mtl = '# No texture/material assignment evidenced.\nnewmtl unresolved\nKd 1 1 1\n'
    sidecar = {'decode': result, 'declared_attributes': attrs,
               'export': {'faces': 0, 'attribute_associations': 'unresolved',
                          'materials': 'unresolved', 'normals': 'caller-declared source lanes only',
                          'losses': ['OBJ omits command semantics, colours and GIF state; preserved here.']}}
    return '\n'.join(obj) + '\n', mtl, json.dumps(sidecar, sort_keys=True, indent=2, allow_nan=False) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('block', type=Path, help='already-extracted block; never an archive')
    parser.add_argument('--start', type=lambda s: int(s, 0), required=True, help='explicit VIF byte offset')
    parser.add_argument('--out', type=Path, required=True, help='existing empty output directory')
    parser.add_argument('--declarations', type=Path, help='optional JSON array of caller source-lane declarations')
    args = parser.parse_args()
    if not args.out.is_dir() or any(args.out.iterdir()):
        parser.error('--out must be an existing empty directory')
    if args.block.stat().st_size > MAX_INPUT:
        parser.error('input exceeds 16 MiB inspection limit')
    with args.block.open('rb') as source:
        data = source.read(MAX_INPUT + 1)
    try:
        result = decode_vif(data, args.start)
        declarations = json.loads(args.declarations.read_text(encoding='utf-8')) if args.declarations else []
        if not isinstance(declarations, list):
            raise ValueError('declarations must be an array')
        obj, mtl, sidecar = export_text(result, declarations)
    except (ValueError, TypeError, KeyError) as exc:
        parser.error(str(exc))
    for name, text in [('model.obj', obj), ('model.mtl', mtl), ('model.json', sidecar)]:
        with (args.out / name).open('x', encoding='utf-8', newline='\n') as target:
            target.write(text)
    return 0 if result['complete'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
