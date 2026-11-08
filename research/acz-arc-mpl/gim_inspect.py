"""Inspect the bounded indexed-GIM layout used by extracted ACZ MPL parts.

The tool reads an already-extracted GIM block and writes candidate RGBA PNGs.
It does not open archives, modify game data, infer UVs, or claim a candidate
preview is the texture's in-game orientation.  The two previews differ only in
the documented PS2 CSM1-style CLUT ordering hypothesis.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

MAX_INPUT = 2 * 1024 * 1024
HEADER_BYTES = 0x20
PALETTE_HEADER_BYTES = 0x10
PALETTE_BYTES = 256 * 4


def parse_gim(data):
    if len(data) < HEADER_BYTES + PALETTE_HEADER_BYTES + PALETTE_BYTES:
        raise ValueError('GIM is too short for indexed pixels and a 256-entry palette')
    if data[:4] != b'GIM\0':
        raise ValueError('missing GIM magic')
    width, height = struct.unpack_from('<HH', data, 0x1c)
    if not width or not height or width * height > MAX_INPUT:
        raise ValueError('invalid or oversized GIM dimensions')
    pixels_end = HEADER_BYTES + width * height
    palette_header = pixels_end
    palette = palette_header + PALETTE_HEADER_BYTES
    expected = palette + PALETTE_BYTES
    if len(data) != expected:
        raise ValueError('unsupported GIM layout: expected 8-bit pixels plus one 256-entry palette')
    entries = [tuple(data[palette + n * 4:palette + n * 4 + 4]) for n in range(256)]
    return {
        'width': width,
        'height': height,
        'pixels': data[HEADER_BYTES:pixels_end],
        'palette_header': data[palette_header:palette],
        'palette_bgra': entries,
        'pixel_offset': HEADER_BYTES,
        'palette_header_offset': palette_header,
        'palette_offset': palette,
    }


def csm1_index(index):
    """PS2 8-bit CLUT group exchange candidate; this is not a UV swizzle."""
    return (index & 0xe7) | ((index & 0x08) << 1) | ((index & 0x10) >> 1)


def rgba(image, csm1=False):
    out = bytearray()
    for index in image['pixels']:
        blue, green, red, alpha = image['palette_bgra'][csm1_index(index) if csm1 else index]
        out.extend((red, green, blue, alpha))
    return bytes(out)


def png_chunk(name, value):
    return (struct.pack('>I', len(value)) + name + value +
            struct.pack('>I', zlib.crc32(name + value) & 0xffffffff))


def write_png(path, width, height, pixels):
    scanlines = b''.join(b'\0' + pixels[row * width * 4:(row + 1) * width * 4]
                         for row in range(height))
    encoded = b'\x89PNG\r\n\x1a\n'
    encoded += png_chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
    encoded += png_chunk(b'IDAT', zlib.compress(scanlines, 9))
    encoded += png_chunk(b'IEND', b'')
    path.write_bytes(encoded)


def inspect_bytes(data, source_name, output, source_context=None):
    if len(data) > MAX_INPUT:
        raise ValueError('input exceeds inspection limit')
    image = parse_gim(data)
    output.mkdir(parents=True, exist_ok=False)
    metadata = {
        'schema': 'acz-indexed-gim-inspection/v1',
        'source_name': source_name,
        'source_sha256': hashlib.sha256(data).hexdigest(),
        'size': len(data),
        'width': image['width'],
        'height': image['height'],
        'indexed_bits': 8,
        'pixel_offset': image['pixel_offset'],
        'pixel_bytes': len(image['pixels']),
        'palette_header_offset': image['palette_header_offset'],
        'palette_header_hex': image['palette_header'].hex(),
        'palette_offset': image['palette_offset'],
        'palette_entries': len(image['palette_bgra']),
        'pixel_layout': 'linear candidate only; GS texture-memory swizzle not evaluated',
        'previews': {
            'linear_palette.png': 'pixel index directly selects BGRA palette entry',
            'csm1_palette_candidate.png': 'pixel index selects group-exchanged CSM1-style palette entry',
        },
        'limitations': [
            'No UV/material/surface association.',
            'No GS texture-memory swizzle or in-game orientation inference.',
            'Palette alpha is preserved as stored; its game blend semantics are unresolved.',
        ],
    }
    if source_context:
        metadata['source_context'] = source_context
    (output / 'metadata.json').write_text(json.dumps(metadata, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    write_png(output / 'linear_palette.png', image['width'], image['height'], rgba(image))
    write_png(output / 'csm1_palette_candidate.png', image['width'], image['height'], rgba(image, csm1=True))
    return metadata


def inspect_file(source, output):
    return inspect_bytes(source.read_bytes(), source.name, output)


def inspect_mpl_part(source, part, output):
    container = source.read_bytes()
    if len(container) > MAX_INPUT or len(container) < 8:
        raise ValueError('MPL input is too short or exceeds inspection limit')
    count, = struct.unpack_from('<I', container)
    if not count or count > 4096 or 4 + count * 4 > len(container):
        raise ValueError('invalid MPL part table')
    if not 0 <= part < count:
        raise ValueError('MPL part index is outside the part table')
    offsets = struct.unpack_from('<' + 'I' * count, container, 4)
    if any(offsets[index] < 4 + count * 4 or offsets[index] >= len(container) for index in range(count)):
        raise ValueError('MPL part table has an out-of-range offset')
    if any(offsets[index] >= offsets[index + 1] for index in range(count - 1)):
        raise ValueError('MPL part table is not strictly ascending')
    start = offsets[part]
    end = offsets[part + 1] if part + 1 < count else len(container)
    return inspect_bytes(container[start:end], source.name + '#part' + str(part), output, {
        'container_name': source.name,
        'container_sha256': hashlib.sha256(container).hexdigest(),
        'part_index': part,
        'part_offset': start,
        'part_length': end - start,
    })


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='already-extracted standalone GIM block or MPL container')
    parser.add_argument('--mpl-part', type=int, help='read this GIM-bearing MPL part directly, without extracting it')
    parser.add_argument('--out', type=Path, required=True, help='new output directory')
    args = parser.parse_args()
    if args.out.exists():
        parser.error('--out must not exist')
    try:
        result = (inspect_mpl_part(args.source, args.mpl_part, args.out)
                  if args.mpl_part is not None else inspect_file(args.source, args.out))
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print('INSPECTED: {width}x{height}, 8-bit indexed, {palette_entries} palette entries'.format(**result))


if __name__ == '__main__':
    main()
