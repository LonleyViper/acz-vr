"""Extract repeated ACM VIF source batches without inferring triangles.

The input must already be an extracted ``.mpl`` file.  This parser never reads
an ARC archive and only writes its requested JSON output.  It recognizes the
specific five-UNPACK source pattern observed in the PC files:

* V2-32 destination 0: two u32 batch fields;
* V3-32 destination 1: position source lanes;
* V3-16 destination 2: normal source lanes;
* V4-8 destination 3: colour source lanes; and
* V2-32 destination 4: UV source lanes.

The equal per-batch counts make the arrays useful for inspection, but VIF/VU
execution and the meaning of the two leading fields remain unproven.  In
particular, no faces or primitive topology are generated.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct


class DecodeError(ValueError):
    """The supplied MPL/ACM data does not meet the bounded source pattern."""


def _mpl_part(path: Path, index: int) -> bytes:
    data = path.read_bytes()
    if len(data) < 4:
        raise DecodeError("truncated MPL part table")
    (count,) = struct.unpack_from("<I", data)
    if not 0 <= index < count or len(data) < 4 + count * 4:
        raise DecodeError("part index/table is invalid")
    offsets = struct.unpack_from(f"<{count}I", data, 4) + (len(data),)
    start, end = offsets[index], offsets[index + 1]
    if not 0 <= start <= end <= len(data):
        raise DecodeError("part offsets are invalid")
    return data[start:end]


def _command(data: bytes, offset: int, code: int, count: int, immediate: int) -> int:
    if offset + 4 > len(data):
        raise DecodeError("truncated VIF code")
    (word,) = struct.unpack_from("<I", data, offset)
    if word != (code << 24 | count << 16 | immediate):
        raise DecodeError(f"unexpected VIF code at 0x{offset:x}")
    return offset + 4


def _take(data: bytes, offset: int, size: int) -> tuple[bytes, int]:
    end = offset + size
    if end > len(data):
        raise DecodeError("truncated VIF payload")
    return data[offset:end], (end + 3) & ~3


def acm_batches(acm: bytes) -> dict:
    """Return every complete observed five-UNPACK batch in one ACM block."""
    if len(acm) < 80 or acm[:4] != b"ACM\0":
        raise DecodeError("not an ACM block")
    (version,) = struct.unpack_from("<I", acm, 4)
    (record_count,) = struct.unpack_from("<I", acm, 16)
    (data_start,) = struct.unpack_from("<I", acm, 28)
    record_end = 80 + record_count * 64
    if version != 5 or data_start != record_end or data_start > len(acm):
        raise DecodeError("unsupported ACM header/record span")

    batches = []
    # A five-code signature prevents float payloads from being treated as VIF.
    for start in range(data_start, len(acm) - 12, 4):
        try:
            at = _command(acm, start, 0x64, 1, 0x8000)
            meta_bytes, at = _take(acm, at, 8)
            vertices, flag = struct.unpack("<II", meta_bytes)
            if not 3 <= vertices <= 255:
                continue
            at = _command(acm, at, 0x68, vertices, 0x8001)
            raw, at = _take(acm, at, vertices * 12)
            positions = [list(row) for row in struct.iter_unpack("<3f", raw)]
            at = _command(acm, at, 0x69, vertices, 0x8002)
            raw, at = _take(acm, at, vertices * 6)
            normals = [list(row) for row in struct.iter_unpack("<3h", raw)]
            at = _command(acm, at, 0x6E, vertices, 0xC003)
            raw, at = _take(acm, at, vertices * 4)
            colours = [list(row) for row in struct.iter_unpack("<4B", raw)]
            at = _command(acm, at, 0x64, vertices, 0x8004)
            raw, at = _take(acm, at, vertices * 8)
            uvs = [list(row) for row in struct.iter_unpack("<2f", raw)]
        except DecodeError:
            continue
        batches.append({
            "offset": start,
            "end": at,
            "source_metadata_u32": [vertices, flag],
            "positions_f32": positions,
            "normals_i16": normals,
            "colours_rgba_u8": colours,
            "uvs_f32": uvs,
            "topology": "unresolved: VIF/VU execution and primitive assembly not decoded",
        })
    return {
        "schema": "acz-acm-vif-batches-1",
        "acm_sha256": hashlib.sha256(acm).hexdigest(),
        "acm_size": len(acm),
        "vif_data_start": data_start,
        "batch_count": len(batches),
        "vertex_source_count": sum(batch["source_metadata_u32"][0] for batch in batches),
        "batches": batches,
        "limitations": [
            "Source-lane association is observed from the five adjacent UNPACK commands.",
            "No VIF/VU destination execution, GIF state, primitive assembly or faces are inferred.",
            "The metadata pair and normal scale/frame remain unresolved.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mpl", type=Path)
    parser.add_argument("part", type=int)
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    if args.out.exists():
        parser.error("output must not already exist")
    try:
        result = acm_batches(_mpl_part(args.mpl, args.part))
    except (DecodeError, OSError) as error:
        parser.error(str(error))
    args.out.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
