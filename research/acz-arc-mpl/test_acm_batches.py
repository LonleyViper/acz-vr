import struct
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).parent))
import acm_batches


def code(command, count, immediate):
    return struct.pack("<I", command << 24 | count << 16 | immediate)


def fixture():
    records = 1
    data_start = 80 + records * 64
    header = bytearray(data_start)
    header[:4] = b"ACM\0"
    struct.pack_into("<I", header, 4, 5)
    struct.pack_into("<I", header, 16, records)
    struct.pack_into("<I", header, 28, data_start)
    n = 3
    stream = b"".join((
        code(0x64, 1, 0x8000), struct.pack("<2I", n, 1),
        code(0x68, n, 0x8001), struct.pack("<9f", *range(9)),
        code(0x69, n, 0x8002), struct.pack("<9h", *range(-4, 5)), b"\0\0",
        code(0x6E, n, 0xC003), bytes(range(12)),
        code(0x64, n, 0x8004), struct.pack("<6f", *range(6)),
    ))
    return bytes(header) + stream


class BatchesTests(unittest.TestCase):
    def test_preserves_complete_source_lanes_without_faces(self):
        result = acm_batches.acm_batches(fixture())
        self.assertEqual(result["batch_count"], 1)
        self.assertEqual(result["vertex_source_count"], 3)
        batch = result["batches"][0]
        self.assertEqual(batch["source_metadata_u32"], [3, 1])
        self.assertEqual(batch["positions_f32"][2], [6.0, 7.0, 8.0])
        self.assertEqual(batch["normals_i16"][0], [-4, -3, -2])
        self.assertEqual(batch["colours_rgba_u8"][2], [8, 9, 10, 11])
        self.assertEqual(batch["uvs_f32"][1], [2.0, 3.0])
        self.assertIn("unresolved", batch["topology"])

    def test_wrong_signature_does_not_become_a_batch(self):
        data = bytearray(fixture())
        # The V3-32 position command begins after its code plus one V2 tuple.
        data[80 + 64 + 12] ^= 1
        self.assertEqual(acm_batches.acm_batches(bytes(data))["batch_count"], 0)


if __name__ == "__main__":
    unittest.main()
