"""Read-only PE/import/string triage; does not execute or modify game files."""
import argparse
import hashlib
import json
import math
import re
import struct
from collections import Counter
from pathlib import Path


def inspect_pe(path):
    data = path.read_bytes()
    def u16(p): return struct.unpack_from('<H', data, p)[0]
    def u32(p): return struct.unpack_from('<I', data, p)[0]
    def u64(p): return struct.unpack_from('<Q', data, p)[0]
    def cstr(p): return data[p:data.index(b'\0', p)].decode('ascii', 'replace')
    if data[:2] != b'MZ': raise ValueError('Not an MZ image')
    pe = u32(0x3c)
    if data[pe:pe+4] != b'PE\0\0': raise ValueError('Not a PE image')
    opt = pe + 24
    is64 = u16(opt) == 0x20b
    if u16(opt) not in (0x10b, 0x20b): raise ValueError('Unknown optional header')
    base = u64(opt+24) if is64 else u32(opt+28)
    sec_start = opt + u16(pe+20)
    sections = []
    for i in range(u16(pe+6)):
        p = sec_start + i*40
        name = data[p:p+8].split(b'\0')[0].decode('ascii', 'replace')
        vs, va, size, raw = struct.unpack_from('<IIII', data, p+8)
        chunk = data[raw:raw+size]
        counts = Counter(chunk)
        entropy = -sum((n/len(chunk))*math.log2(n/len(chunk)) for n in counts.values()) if chunk else 0
        sections.append(dict(name=name, rva=va, virtual_size=vs, raw_offset=raw, raw_size=size,
                             characteristics=hex(u32(p+36)), entropy=round(entropy, 3)))
    def offset(rva):
        if rva < u32(opt+60): return rva
        for s in sections:
            delta = rva-s['rva']
            if 0 <= delta < s['raw_size']: return s['raw_offset']+delta
        raise ValueError(f'Unmapped RVA {rva:x}')
    dd = opt + (112 if is64 else 96)
    imports = []
    import_rva = u32(dd+8)
    if import_rva:
        p = offset(import_rva)
        while any(data[p:p+20]):
            original, _, _, name_rva, first = struct.unpack_from('<IIIII', data, p)
            entries = []
            q = offset(original or first)
            stride = 8 if is64 else 4
            idx = 0
            while (value := (u64(q) if is64 else u32(q))):
                ordinal_bit = 1 << (63 if is64 else 31)
                name = f'ordinal:{value & 0xffff}' if value & ordinal_bit else cstr(offset(value)+2)
                entries.append(dict(name=name, iat_rva=hex(first+idx*stride)))
                q += stride
                idx += 1
            imports.append(dict(dll=cstr(offset(name_rva)), functions=entries))
            p += 20
    terms = re.compile(r'd3d|direct3d|dxgi|vulkan|opengl|unreal|unity|pcsx|emulat|mips|r5900|ps2|playstation|gsdx|openxr|openvr|camera|projection|perspective|frustum|shader|\.pdb|fov|render|SDL|CreateDevice|GetProcAddress|LoadLibrary', re.I)
    strings = []
    for encoding, pattern in [('ascii', rb'[\x20-\x7e]{5,}'), ('utf-16le', rb'(?:[\x20-\x7e]\x00){5,}')]:
        for match in re.finditer(pattern, data):
            value = match.group().decode(encoding)
            if terms.search(value):
                strings.append(dict(file_offset=hex(match.start()), encoding=encoding, text=value[:500]))
    return dict(path=str(path), size=len(data), sha256=hashlib.sha256(data).hexdigest(),
                machine=hex(u16(pe+4)), pe_type='PE32+' if is64 else 'PE32', image_base=hex(base),
                entry_rva=hex(u32(opt+16)), managed=bool(u32(dd+14*8)),
                sections=sections, imports=imports, selected_strings=strings,
                rtti_classes=[dict(file_offset=hex(m.start()), name=m.group()[:-1].decode('ascii', 'replace'))
                              for m in re.finditer(rb'\.\?AV[^\x00]{3,200}\x00', data)],
                delay_import_directory_rva=hex(u32(dd+13*8)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('game_dir', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = dict(executable=inspect_pe(args.game_dir/'acz.exe'), archives=[])
    for path in sorted(args.game_dir.glob('*.arc')):
        with path.open('rb') as stream: header = stream.read(256)
        result['archives'].append(dict(name=path.name, size=path.stat().st_size,
                                       header_hex=header.hex(),
                                       header_ascii=''.join(chr(b) if 32 <= b < 127 else '.' for b in header)))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(dict(machine=result['executable']['machine'], pe_type=result['executable']['pe_type'],
                         sha256=result['executable']['sha256'],
                         imported_dlls=[i['dll'] for i in result['executable']['imports']],
                         matched_strings=len(result['executable']['selected_strings']), output=str(args.output)), indent=2))


if __name__ == '__main__': main()
