"""Synthetic binary fixtures only; no archive/game inputs or third-party modules."""
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import decoder as d


def vif(cmd, imm=0, num=0, irq=False):
    return struct.pack('<I', imm | (num << 16) | (cmd << 24) | (int(irq) << 31))


def gif(regs=(5,), payload=b'', nloop=1, mode=0, pre=1, prim=3, eop=1):
    lo = nloop | (eop << 15) | (pre << 46) | (prim << 47) | (mode << 58) | ((len(regs) % 16) << 60)
    hi = sum(reg << (4 * i) for i, reg in enumerate(regs))
    return struct.pack('<QQ', lo, hi) + payload


def direct(payload, cmd=0x50):
    assert len(payload) % 16 == 0
    return vif(0) * 3 + vif(cmd, len(payload) // 16) + payload


def declaration(offset=0, role='position', storage='integer'):
    return dict(offset=offset, role=role, storage=storage,
                evidence='synthetic fixture explicitly defines source lane role')


class DecoderTests(unittest.TestCase):
    def coverage(self, data, records, base=0):
        pos = base
        reconstructed = bytearray()
        for s in records:
            self.assertEqual(s['offset'], pos)
            self.assertGreater(s['end'], pos)
            raw = bytes.fromhex(s['raw_hex'])
            self.assertEqual(len(raw), s['end'] - pos)
            reconstructed.extend(raw)
            pos = s['end']
        self.assertEqual(pos, base + len(data))
        self.assertEqual(bytes(reconstructed), data)

    def check(self, data, start=0):
        r = d.decode_vif(data, start)
        self.coverage(data, r['coverage'])
        self.assertEqual(r['sha256'], hashlib.sha256(data).hexdigest())
        for c in r['commands']:
            if 'gif' in c:
                self.coverage(bytes.fromhex(c['payload_hex']), c['gif']['coverage'], c['payload_offset'])
        json.dumps(r, allow_nan=False)
        return r

    def test_empty_and_prefix(self):
        self.assertTrue(self.check(b'')['complete'])
        r = self.check(b'ACM\0' + vif(0), 4)
        self.assertEqual(r['coverage'][0]['kind'], 'opaque_prefix')
        self.assertEqual(r['commands'][0]['offset'], 4)
        self.assertEqual(self.check(b'ACM\0', 4)['commands'], [])

    def test_offset_validation(self):
        for start in (-1, 1, 8, True, 0.0):
            with self.subTest(start=start), self.assertRaises(ValueError):
                d.decode_vif(b'1234', start)

    def test_size_limits(self):
        with patch.object(d, 'MAX_INPUT', 3):
            for fn in (d.decode_vif, d.decode_gif):
                with self.assertRaises(ValueError):
                    fn(b'1234')
        with self.assertRaises(ValueError):
            d.decode_gif(b'', -1)

    def test_all_fixed_commands_and_irq(self):
        cmds = [0, 1, 2, 3, 4, 5, 6, 7, 0x10, 0x11, 0x13, 0x14, 0x15, 0x17]
        r = self.check(b''.join(vif(c, irq=True) for c in cmds))
        self.assertTrue(r['complete'])
        self.assertEqual([c['cmd'] for c in r['commands']], cmds)
        self.assertTrue(all(c['irq'] for c in r['commands']))
        self.assertTrue(all(c['status'] == 'framed' for c in r['commands']))

    def test_state_and_snapshot(self):
        raw = (vif(1, 0x0201) + vif(5, 2) + vif(0x20) + struct.pack('<I', 123)
               + vif(0x30) + struct.pack('<4I', 1, 2, 3, 4)
               + vif(0x31) + struct.pack('<4I', 5, 6, 7, 8) + vif(0))
        r = self.check(raw)
        self.assertEqual(r['state_final'], dict(cl=1, wl=2, mode=2, mask=123, row=[1, 2, 3, 4], col=[5, 6, 7, 8]))
        self.assertEqual(r['commands'][0]['state_before']['row'], [0] * 4)
        self.assertEqual(r['commands'][-1]['state_before'], r['state_final'])
        self.assertIn('reserved', self.check(vif(5, 3))['commands'][0]['unresolved'])

    def test_cycle_zero_fields(self):
        r = self.check(vif(1))
        self.assertEqual((r['state_final']['cl'], r['state_final']['wl']), (0, 0))
        for cycle in (0, 1, 0x100):
            r = self.check(vif(1, cycle) + vif(0x60, num=1) + vif(7))
            self.assertFalse(r['complete'])
            self.assertEqual(len(r['commands']), 2)
            self.assertIn('zero STCYCL', r['issues'][0]['reason'])

    def test_all_unpack_formats_and_masks(self):
        for components in range(1, 5):
            for vl, bits in enumerate((32, 16, 8, 5)):
                if vl == 3 and components != 4:
                    continue
                for masked in (0, 0x10):
                    cmd = 0x60 | ((components - 1) << 2) | vl | masked
                    size = 2 if vl == 3 else components * bits // 8
                    payload = bytes(range(1, size + 1))
                    raw = vif(cmd, 0xc123, 1) + payload + bytes((-size) % 4) + vif(7)
                    with self.subTest(cmd=cmd):
                        r = self.check(raw)
                        c = r['commands'][0]
                        self.assertTrue(r['complete'])
                        self.assertEqual(c['unpack']['components'], components)
                        self.assertEqual(c['unpack']['masked'], bool(masked))
                        self.assertTrue(c['unpack']['unsigned'])
                        self.assertTrue(c['unpack']['tops_relative'])
                        self.assertEqual(c['unpack']['address'], 0x123)
                        self.assertEqual(len(c['source_vectors'][0]), components)
                        self.assertEqual(r['commands'][-1]['name'], 'MARK')

    def test_signed_and_raw_32_lanes(self):
        for cmd, payload, expected in ((0x62, b'\xff', -1), (0x61, b'\xff\xff', -1),
                                        (0x60, b'\xff'*4, 0xffffffff)):
            raw = vif(cmd, num=1) + payload + bytes((-len(payload)) % 4)
            self.assertEqual(self.check(raw)['commands'][0]['source_vectors'], [[expected]])
        raw = vif(0x6f, num=1) + struct.pack('<H', 0xffff) + b'\xa5\xa5'
        self.assertEqual(self.check(raw)['commands'][0]['source_vectors'], [[31, 31, 31, 1]])

    def test_zero_num_and_mpg(self):
        r = self.check(vif(0x60) + bytes(256*4) + vif(0x4a) + bytes(256*8) + vif(7))
        self.assertEqual(len(r['commands'][0]['source_vectors']), 256)
        self.assertEqual(r['commands'][1]['expected_payload_bytes'], 2048)
        self.assertEqual(r['commands'][-1]['name'], 'MARK')

    def test_fill_and_skip_source_lengths(self):
        for cl, wl, count, source in ((2, 4, 7, 4), (2, 4, 8, 4), (4, 2, 7, 7), (1, 3, 1, 1)):
            raw = vif(1, cl | wl << 8) + vif(0x60, num=count) + bytes(source*4) + vif(7)
            r = self.check(raw)
            self.assertTrue(r['complete'])
            self.assertEqual(r['commands'][1]['unpack']['source_count'], source)
            self.assertEqual(r['commands'][-1]['name'], 'MARK')

    def test_unknown_command_stops_without_resync(self):
        raw = vif(8) + vif(0) * 3
        r = self.check(raw)
        self.assertFalse(r['complete'])
        self.assertEqual(len(r['commands']), 1)
        self.assertEqual(r['coverage'][-1]['kind'], 'unknown_tail')

    def test_reserved_unpack_stops(self):
        for cmd in (0x63, 0x67, 0x6b, 0x73, 0x77, 0x7b):
            r = self.check(vif(cmd, num=1) + bytes(16))
            self.assertFalse(r['complete'])
            self.assertEqual(len(r['commands']), 1)

    def test_truncations_and_padding(self):
        for raw in (b'\0', b'\0\0', b'\0\0\0', vif(0x20), vif(0x30)+bytes(15),
                    vif(0x31)+bytes(12), vif(0x4a, num=1)+bytes(7), vif(0x62, num=1)+b'\xff'):
            with self.subTest(raw=raw):
                self.assertFalse(self.check(raw)['complete'])

    def test_direct_and_directhl(self):
        packet = gif(payload=struct.pack('<4I', 16, 32, 99, 0))
        for cmd in (0x50, 0x51):
            r = self.check(direct(packet, cmd) + vif(7))
            c = r['commands'][3]
            self.assertTrue(r['complete'])
            self.assertEqual(c['gif']['tags'][0]['offset'], 16)
            self.assertEqual(c['gif']['tags'][0]['observations'][0]['xyz_gs'], [16, 32, 99])
            self.assertEqual(r['commands'][-1]['name'], 'MARK')

    def test_unaligned_direct_rejected_without_guess(self):
        r = self.check(vif(0x50, 1) + bytes(28))
        self.assertFalse(r['complete'])
        self.assertEqual(len(r['commands']), 1)
        self.assertIn('unaligned', r['issues'][0]['reason'])

    def test_direct_zero_count_is_explicitly_unsupported(self):
        r = self.check(vif(0)*3 + vif(0x50) + vif(7))
        self.assertFalse(r['complete'])
        self.assertEqual(len(r['commands']), 4)
        self.assertIn('zero DIRECT count', r['issues'][0]['reason'])

    def test_bad_nested_gif_retains_known_vif_boundary(self):
        r = self.check(direct(gif(nloop=100)) + vif(7))
        self.assertFalse(r['complete'])
        self.assertEqual(r['commands'][-1]['name'], 'MARK')

    def gifcheck(self, raw):
        r = d.decode_gif(raw, 32)
        self.coverage(raw, r['coverage'], 32)
        json.dumps(r, allow_nan=False)
        return r

    def test_gif_all_modes_nreg_zero_and_eop(self):
        packets = [gif(regs=tuple(range(16)), payload=bytes(256)),
                   gif(regs=(15,), payload=bytes(16), mode=1),
                   gif(payload=bytes(16), mode=2), gif(payload=bytes(16), mode=3)]
        r = self.gifcheck(b''.join(packets))
        self.assertTrue(r['complete'])
        self.assertEqual(len(r['tags']), 4)  # EOP does not end DIRECT.
        self.assertEqual(len(r['tags'][0]['registers']), 16)
        self.assertEqual(r['tags'][1]['observations'][0]['register'], 'NOP')
        self.assertEqual(sum(s['kind'] == 'opaque_image' for s in r['coverage']), 2)
        self.assertIn('GIFpadding', [s['kind'] for s in r['coverage']])

    def test_gif_packed_attributes_and_adc(self):
        raw = gif(regs=(1, 2, 3, 4, 5, 12, 13), payload=(
            struct.pack('<4I', 10, 20, 30, 40) + struct.pack('<4I', 1, 2, 3, 0)
            + struct.pack('<4I', 123, 456, 0, 0)
            + struct.pack('<4I', 16, 32, 99 << 4, 1 << 15)
            + struct.pack('<4I', 16, 32, 99, 0)
            + struct.pack('<4I', 16, 32, 99 << 4, 0)
            + struct.pack('<4I', 16, 32, 99, 0)))
        obs = self.gifcheck(raw)['tags'][0]['observations']
        self.assertEqual(obs[0]['rgba'], [10, 20, 30, 40])
        self.assertEqual(obs[1]['q_bits'], 3)
        self.assertEqual(obs[2]['uv_fixed_10_4'], [123, 456])
        self.assertFalse(obs[3]['draw_kick'])
        self.assertTrue(obs[4]['draw_kick'])
        self.assertEqual(obs[3]['xyz_gs'], obs[4]['xyz_gs'])
        self.assertFalse(obs[5]['draw_kick'])
        self.assertFalse(obs[6]['draw_kick'])
        self.assertEqual(obs[4]['prim_type_field'], 'triangle')
        self.assertNotIn('faces', obs[4])

    def test_ad_and_reglist_layouts(self):
        # A+D data is ordinary GS layout, not PACKED component layout.
        value = 0x44332211 | (0x3f800000 << 32)
        raw = gif(regs=(14, 14, 14), payload=(struct.pack('<QQ', value, 1)
                 + struct.pack('<QQ', 123 | 456 << 16, 3)
                 + struct.pack('<QQ', 4, 0)))
        obs = self.gifcheck(raw)['tags'][0]['observations']
        self.assertEqual(obs[0]['rgba'], [17, 34, 51, 68])
        self.assertEqual(obs[0]['q_bits'], 0x3f800000)
        self.assertEqual(obs[1]['uv_fixed_10_4'], [123, 456])
        self.assertEqual(obs[2]['prim'], 4)
        raw = gif(regs=(1, 3, 5, 14), mode=1, payload=struct.pack('<4Q', value, 123 | 456 << 16, 16 | 32 << 16 | 99 << 32, 0))
        obs = self.gifcheck(raw)['tags'][0]['observations']
        self.assertEqual(obs[0]['rgba'], [17, 34, 51, 68])
        self.assertEqual(obs[1]['uv_fixed_10_4'], [123, 456])
        self.assertEqual(obs[2]['xyz_gs'], [16, 32, 99])
        self.assertIn('invalid', obs[3]['unresolved'])

    def test_prim_evidence_and_pre_modes(self):
        xyz = struct.pack('<4I', 16, 32, 99, 0)
        r = self.gifcheck(gif(regs=(0, 5), pre=0, payload=struct.pack('<QQ', 6, 0)+xyz))
        self.assertEqual(r['tags'][0]['observations'][1]['prim_type_field'], 'sprite')
        r = self.gifcheck(gif(pre=0, payload=xyz))
        self.assertIsNone(r['tags'][0]['observations'][0]['prim'])
        r = self.gifcheck(gif(mode=1, payload=bytes(16)))
        self.assertIsNone(r['tags'][0]['initial_prim'])  # REGLIST PRE is ignored.
        # Preserve state across tags inside a transfer, but not across DIRECTs.
        r = self.gifcheck(gif(payload=xyz, prim=4) + gif(payload=xyz, pre=0))
        self.assertEqual(r['tags'][1]['observations'][0]['prim_type_field'], 'triangle_strip')

    def test_gif_truncation_and_unknown_registers(self):
        for raw in (bytes(15), gif(payload=bytes(15)), gif(mode=1, payload=bytes(7))):
            self.assertFalse(self.gifcheck(raw)['complete'])
        r = self.gifcheck(gif(regs=(11, 14), payload=bytes(16)+struct.pack('<QQ', 0, 0x7f)))
        self.assertTrue(r['complete'])
        self.assertTrue(all('unresolved' in o for o in r['tags'][0]['observations']))

    def test_shared_record_limit(self):
        with patch.object(d, 'MAX_RECORDS', 5):
            r = self.check(vif(0)*8)
            self.assertFalse(r['complete'])
            self.assertEqual(len(r['commands']), 5)
            r = self.check(vif(0x60, num=6)+bytes(24)+vif(7))
            self.assertFalse(r['complete'])
            self.assertNotIn('source_vectors', r['commands'][0])
            self.assertEqual(r['commands'][-1]['name'], 'MARK')
            r = self.check(direct(gif(payload=bytes(16))))
            self.assertFalse(r['complete'])
            self.assertEqual(r['commands'][3]['gif']['coverage'][-1]['kind'], 'unknown_tail')

    def test_export_no_inferred_geometry(self):
        r = self.check(vif(0x68, num=3)+bytes(36))
        obj, mtl, sidecar = d.export_text(r)
        self.assertFalse(any(line.startswith(('v ', 'f ')) for line in obj.splitlines()))
        self.assertIn('newmtl unresolved', mtl)
        self.assertEqual(json.loads(sidecar)['decode'], r)
        self.assertEqual(d.export_text(r), (obj, mtl, sidecar))

    def test_declared_attributes_and_stable_export(self):
        chunks = [(0x68, struct.pack('<3f', 1, -2, 3)), (0x68, struct.pack('<3f', 0, 1, 0)),
                  (0x64, struct.pack('<2f', .25, .5)), (0x6e, bytes([10, 20, 30, 255]))]
        raw = b''
        decls = []
        for (cmd, payload), role in zip(chunks, ('position', 'normal', 'uv', 'colour')):
            decls.append(declaration(len(raw), role, 'integer' if role == 'colour' else 'float32'))
            raw += vif(cmd, imm=0x4000 if role == 'colour' else 0, num=1)+payload
        r = self.check(raw)
        obj, mtl, sidecar = d.export_text(r, decls)
        self.assertIn('v 1 -2 3\n', obj)
        self.assertIn('vn 0 1 0\n', obj)
        self.assertIn('vt 0.25 0.5\n', obj)
        self.assertFalse(any(s.startswith('f ') for s in obj.splitlines()))
        meta = json.loads(sidecar)
        self.assertEqual(meta['declared_attributes'][-1]['vectors'], [[10, 20, 30, 255]])
        self.assertTrue(all(a['evidence'] for a in meta['declared_attributes']))
        self.assertEqual(d.export_text(r, decls), (obj, mtl, sidecar))

    def test_invalid_declarations(self):
        r = self.check(vif(0x68, num=1)+bytes(12))
        invalid = [None, {}, [], dict(declaration(), evidence=''), dict(declaration(), offset=True),
                   dict(declaration(), offset=4), dict(declaration(), role=[]),
                   dict(declaration(), role='uv'), dict(declaration(), storage='guess')]
        for decl in invalid:
            with self.subTest(decl=decl), self.assertRaises(ValueError):
                d.export_text(r, [decl])
        with self.assertRaises(ValueError):
            d.export_text(r, [declaration(), declaration()])
        r = self.check(vif(0x68, num=1)+struct.pack('<3I', 0x7fc00000, 0, 0))
        with self.assertRaises(ValueError):
            d.export_text(r, [declaration(storage='float32')])
        r = self.check(vif(0x69, num=1)+bytes(8))
        with self.assertRaises(ValueError):
            d.export_text(r, [declaration(storage='float32')])

    def test_deterministic_mutation_and_truncation_sweep(self):
        raw = vif(0x68, num=2)+bytes(24)+vif(0)
        packet = direct(gif(payload=struct.pack('<4I', 16, 32, 99, 0)))
        for fixture in (raw, packet):
            for end in range(len(fixture)+1):
                self.check(fixture[:end])
            for index in range(len(fixture)):
                for value in (0, 0x7f, 0xff):
                    self.check(fixture[:index]+bytes([value])+fixture[index+1:])
        rng = random.Random(183)
        for size in range(128):
            sample = bytes(rng.randrange(256) for _ in range(size))
            self.check(sample)
            self.gifcheck(sample)

    def test_cli_stable_outputs_and_exit_codes(self):
        script = str(Path(d.__file__).resolve())
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parent) as scratch:
            root = Path(scratch)
            block = root/'synthetic.bin'
            out = root/'out'
            out.mkdir()
            block.write_bytes(vif(0x68, num=1)+bytes(12))
            args = [sys.executable, '-I', '-B', script, str(block), '--start', '0', '--out', str(out)]
            run = subprocess.run(args, capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(sorted(p.name for p in out.iterdir()), ['model.json', 'model.mtl', 'model.obj'])
            self.assertEqual((out/'model.json').read_bytes(), d.export_text(d.decode_vif(block.read_bytes()))[2].encode())
            before = (out/'model.json').read_bytes()
            run = subprocess.run(args, capture_output=True, text=True, timeout=10)
            self.assertNotEqual(run.returncode, 0)
            self.assertEqual((out/'model.json').read_bytes(), before)
            badout = root/'badout'
            badout.mkdir()
            block.write_bytes(vif(8)+vif(0))
            args[-1] = str(badout)
            run = subprocess.run(args, capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 2, run.stderr)
            self.assertFalse(json.loads((badout/'model.json').read_text())['decode']['complete'])


if __name__ == '__main__':
    unittest.main()
