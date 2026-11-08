"""Synthetic (no game data) tests for acm_lib + model_baker: python -I -B test_model_baker.py"""
import os, struct, sys, tempfile, unittest
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from acm_lib import decode_acm, node_parents, NODE, csm1, fit_rest_angles
from model_baker import cockpit_alpha_test, Builder, read_zvrm, gim_image, MF_ALPHA_TEST, SRC_ACM, SRC_MPL, PF_HINGE_FITTED, add_stores, weapon_model, MF_EXTERIOR, MF_HIDDEN, STORES_SRC_PART, add_tank, find_missile_index, choose_stations, station_candidates, weapon_shape, slot_stations, ms_weapon_model


def vif(cmd, num, imm):
    return struct.pack('<I', cmd << 24 | num << 16 | imm)


def strip_batch(pos, uv):
    n = len(pos)
    b = vif(0x01, 0, 0x0105) + vif(0x64, 1, 0x8000) + struct.pack('<2I', n, 1)
    b += vif(0x68, n, 0x8001) + np.asarray(pos, '<f4').tobytes()
    nb = np.tile(np.array([0, 0, 4096], '<i2'), (n, 1)).tobytes()
    b += vif(0x69, n, 0x8002) + nb + b'\0' * ((-len(nb)) % 4)
    b += vif(0x6e, n, 0xc003) + bytes([128, 128, 128, 128] * n)
    b += vif(0x64, n, 0x8004) + np.asarray(uv, '<f4').tobytes()
    b += vif(0x11, 0, 0) + vif(0x17, 0, 0) + vif(0x01, 0, 0x0404)
    return b


def make_acm(nodes, meshes):
    nn, nm = len(nodes), len(meshes)
    mt = 80 + 64 * nn
    base = (mt + 4 * nm + 15) & ~15
    blobs, offs, cur = [], [], 0
    for m in meshes:
        body = vif(0, 0, 0) + m
        body += b'\0' * ((-(len(body) + 4)) % 16)
        qwc = (len(body) + 4 - 8) // 16 if False else (len(body) - 4) // 16
        blob = struct.pack('<I', 0x60000000 | qwc) + body
        offs.append(cur); blobs.append(blob); cur += len(blob)
    hdr = struct.pack('<4sI2f4I', b'ACM\0', 5, 1.0, 0.0, nn, 80, nm, mt) + b'\0' * 48
    nb = b''.join(np.array([n], NODE).tobytes() for n in nodes)
    table = struct.pack('<%dI' % nm, *offs)
    pad = b'\0' * (base - mt - len(table))
    return hdr + nb + table + pad + b''.join(blobs)


def node(nchild, mesh, t=(0, 0, 0), rot=(0, 0, 0), chan=-1, flags=8):
    return (flags, nchild, mesh, 0, 0, -1, chan, -1, t, 1.0, rot, 0.0)


class T(unittest.TestCase):
    def setUp(self):
        pos = [(0, 0, 0), (0, 1, 0), (1, 0, 0), (1, 1, 0)]
        uv = [(0, 0), (0, 1), (1, 0), (1, 1)]
        self.acm = make_acm([node(1, -1, (0, 0, -5)), node(0, 0, (1, 0, 0), (0.349, 0, np.pi / 2), chan=0)],
                            [strip_batch(pos, uv)])

    def test_tree(self):
        n = np.array([node(2, -1), node(1, 0), node(0, 0), node(0, 0)], NODE)
        self.assertEqual(node_parents(n), [-1, 0, 1, 0])

    def test_decode_and_transform(self):
        m = decode_acm(self.acm)
        self.assertEqual(len(m['meshes']), 1)
        me = m['meshes'][0]
        self.assertEqual(me['tri'].tolist(), [[0, 1, 2], [2, 1, 3]])
        W = m['world'][1]
        # rot[0] (hinge range) ignored for an animated node; Rz(90) maps +X -> +Y; translations chain
        p = W[:3, :3] @ np.array([1, 0, 0]) + W[:3, 3]
        np.testing.assert_allclose(p, [1, 1, -5], atol=1e-6)

    def test_csm1(self):
        self.assertEqual([csm1(i) for i in (0, 8, 16, 24, 255)], [0, 16, 8, 24, 255])

    def test_gim_ct32_and_indexed(self):
        g = b'GIM\0' + b'\0' * 15 + b'\x00' + b'\0' * 8 + struct.pack('<HH', 1, 1) + bytes([10, 20, 30, 128])
        np.testing.assert_array_equal(gim_image(g)[0, 0], [10, 20, 30, 255])
        pal = np.zeros((256, 4), np.uint8); pal[csm1(5)] = [10, 20, 30, 64]          # RGBA (PS2 CT32 CLUT), stored CSM1
        g8 = b'GIM\0' + b'\0' * 15 + b'\x13' + b'\0' * 8 + struct.pack('<HH', 1, 1) + bytes([5]) + b'\0' * 16 + pal.tobytes()
        np.testing.assert_array_equal(gim_image(g8)[0, 0], [10, 20, 30, 127])

    def test_zvrm_roundtrip(self):
        B = Builder('synthetic', SRC_ACM)
        t = B.add_texture(np.full((2, 2, 4), 200, np.uint8))
        B.add_acm(self.acm, 'syn', lambda nd: t, lambda nd: MF_ALPHA_TEST)
        B.eye = (0.0, 1.0, 2.0)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'x.zvrm')
            r = B.write(p)
            z = read_zvrm(p)
            self.assertEqual((r['triangles'], len(z['parts']), len(z['V'])), (2, 2, 4))
            self.assertEqual(z['parts'][1]['parent'], 0)
            self.assertEqual(z['flags'], 15)                         # CCW | rest pose | RGBA CLUT (COCKPIT-3) | anim rest (COCKPIT-4)
            self.assertEqual((z['parts'][1]['rest'], z['parts'][1]['aflags']), (0.0, 0))   # SRC_ACM: no hinge fit
            self.assertEqual(z['parts'][1]['name'], 'syn/n1')
            # emitted CCW-front: geometric normal agrees with the vertex normal after transform
            I = z['I'].reshape(-1, 3); P = z['V']['pos'][I]
            gn = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0])
            vn = z['V']['nrm'][I].sum(1)
            self.assertTrue(np.all((gn * vn).sum(1) < 0) or np.all((gn * vn).sum(1) > 0))
            b = bytearray(open(p, 'rb').read()); b[-1] ^= 1; open(p, 'wb').write(bytes(b))
            with self.assertRaises(ValueError):
                read_zvrm(p)

    def test_fit_rest_angles(self):
        # COCKPIT-4: a static 'wing' with vertex rows at z -2, 0 and +1 (the cut-out aft of the hinge) and a mirrored 'elevon' hinged at z 0 whose
        # stored rot[0] is pi - 20 deg: its vertices (local z -1..0) land on the rows z 0 / +1 only at rest = pi (stored rounded to 90 deg); rest 0 puts
        # them at z -1 (median 0.5 away), the stored angle ~0.35 off the rows.
        wing = [(0, 0, -2), (4, 0, -2), (0, 0, 0), (4, 0, 0), (0, 0, 1), (4, 0, 1)]
        surf = [(0, 0, -1), (4, 0, -1), (0, 0, 0), (4, 0, 0)]
        uv = [(0, 0)] * 6
        acm = make_acm([node(2, -1), node(0, 0), node(0, 1, (0, 0, 0), (np.pi - 0.349, 0, 0), chan=3)],
                       [strip_batch(wing, uv), strip_batch(surf, uv[:4])])
        m = decode_acm(acm)
        r = fit_rest_angles(m['nodes'], m['meshes'])
        self.assertIn(2, r)
        self.assertAlmostEqual(r[2][0], np.pi, places=5)
        self.assertTrue(r[2][1])
        m2 = decode_acm(acm, fit_rest=True)
        p = m2['world'][2][:3, :3] @ np.array([0, 0, -1.0]) + m2['world'][2][:3, 3]
        np.testing.assert_allclose(p, [0, 0, 1], atol=1e-5)   # local z -1 -> aft of the hinge
        # unfitted: a part far from everything keeps rest 0 and is not located
        acm2 = make_acm([node(2, -1), node(0, 0), node(0, 1, (50, 50, 50), (0.349, 0, 0), chan=0)], [strip_batch(wing, uv), strip_batch(surf, uv[:4])])
        m3 = decode_acm(acm2)
        r3 = fit_rest_angles(m3['nodes'], m3['meshes'])
        self.assertEqual((r3[2][0], r3[2][1]), (0.0, False))
        # baked: part record carries rest + the fitted flag (SRC_MPL only)
        B = Builder('fit', SRC_MPL)
        B.add_acm(acm, 'fit', lambda nd: -1, lambda nd: 0)
        with tempfile.TemporaryDirectory() as d:
            pth = os.path.join(d, 'f.zvrm'); B.write(pth); z = read_zvrm(pth)
            self.assertAlmostEqual(z['parts'][2]['rest'], np.pi, places=5)
            self.assertEqual(z['parts'][2]['aflags'], PF_HINGE_FITTED)
            self.assertAlmostEqual(z['parts'][2]['hinge'], np.pi - 0.349, places=5)


def container(parts):
    """u32 n, u32 offsets[n], payloads (the .mpl / weapon pack container)"""
    off = 4 + 4 * len(parts); offs = []
    for b in parts:
        offs.append(off); off += len(b)
    return struct.pack('<I%dI' % len(parts), len(parts), *offs) + b''.join(parts)


class Stores(unittest.TestCase):
    """STORES (headset test 7, F-5E 'the missiles aren't showing on the racks'): the game's own weapon mesh is added at the weapon-station locators."""
    def setUp(self):
        loc = lambda g, t, rot=(0, 0, 0): (8, 0, -1, 0, g, -1, -1, -1, t, 1.0, rot, 0.0)
        # exterior: root (no mesh) with two weapon-station locators (groups 67, 68) at x -10 / +10, y -4, z -3 (one pitched 0.1 rad)
        self.ex = make_acm([node(2, -1), loc(67, (-10, -4, -3), (0.1, 0, 0)), loc(68, (10, -4, -3))], [])
        # weapon, authored like the game's (wz?1_ms.bin, ex.bin): a long thin plate along z, nose (z -7) .. tail (z +7); a second articulated node (the nozzle) at the tail
        plate = [(0, 0, -7), (0, 0, 7), (1, 0, -7), (1, 0, 7)]; uv = [(0, 0), (0, 1), (1, 0), (1, 1)]
        self.weapon = make_acm([node(1, 0), node(0, 1, (0, 0, 7), flags=0xa)], [strip_batch(plate, uv), strip_batch(plate[:3], uv[:3])])
        self.tmp = tempfile.TemporaryDirectory()
        self.pack = os.path.join(self.tmp.name, 'f05eex.bin')
        open(self.pack, 'wb').write(container([container([b'\0' * 16, self.weapon, b'\0' * 16, b'\0' * 16])]))
        self.gim = os.path.join(self.tmp.name, 'weapon.gim')
        open(self.gim, 'wb').write(b'GIM\0' + b'\0' * 15 + b'\0' + b'\0' * 8 + struct.pack('<HH', 1, 1) + bytes([10, 20, 30, 128]))

    def tearDown(self):
        self.tmp.cleanup()

    def test_weapon_model_is_the_high_detail_acm(self):
        self.assertEqual(weapon_model(self.pack, 0), self.weapon)

    def test_one_instance_per_station_at_the_locator_nose_forward(self):
        B = Builder('t', SRC_MPL)
        t_ex = B.add_texture(np.full((2, 2, 4), 9, np.uint8))
        ex = decode_acm(self.ex)
        n0 = len(B.parts)
        r = add_stores(B, ex, dict(pack=self.pack, gim=self.gim, index=0, stations=[67, 68]))
        self.assertEqual(r, [(67, 2), (68, 2)])
        self.assertEqual(len(B.parts) - n0, 4)
        self.assertEqual(len(B.textures), 2)                                       # the weapon atlas is its own texture
        for p in B.parts[n0:]:
            self.assertEqual(p['src'], STORES_SRC_PART)
            self.assertTrue(p['mflags'] & MF_EXTERIOR and not p['mflags'] & MF_HIDDEN)   # drawn, hidden with the airframe by ext:0
            self.assertEqual(p['tex'], 1)
        # world position of the body vertices: NO turn (headset test 8: the old 180 deg turn put the missiles backwards): the nose (authored -7) stays at z-7 relative to the station, the tail at z+7
        with tempfile.TemporaryDirectory() as d:
            pth = os.path.join(d, 's.zvrm'); B.eye = (0, 0, 0); B.write(pth); z = read_zvrm(pth)
            I = z['I']; V = z['V']['pos']
            for g, cx, pitch in ((67, -10.0, 0.1), (68, 10.0, 0.0)):
                body = [p for p in z['parts'] if p['name'].endswith('store%d/n0' % g)][0]
                q = V[np.unique(I[body['fi']:body['fi'] + body['ic']])]
                self.assertAlmostEqual(float(q[:, 0].mean()), cx + 0.5, delta=0.01)   # plate x 0..1 is not mirrored
                nose_z = float(q[:, 2].min()); tail_z = float(q[:, 2].max())
                self.assertLess(nose_z, -3 - 6.0 + 1.5 * abs(pitch) * 10)                  # nose forward (-z) of the station z -3
                self.assertGreater(tail_z, -3 + 6.0 - 1.5 * abs(pitch) * 10)               # tail aft
            noz = [p for p in z['parts'] if p['name'].endswith('store68/n1')][0]
            qz = V[np.unique(I[noz['fi']:noz['fi'] + noz['ic']])][:, 2]
            self.assertGreater(float(qz.mean()), -3 + 1.0)                                 # the nozzle node (weapon z +7) sits at the aircraft-aft end

    def test_real_missiles_are_authored_nose_at_minus_z(self):
        # headset test 8: the real wz?1_ms.bin and ex.bin missiles have the narrow seeker end at -z and the fins at +z = the aircraft frame, so no turn is applied
        data = os.environ.get('ACZ_DATA', r'D:/b-stmsl/data')
        files = [os.path.join(data, 'lodweapon', 'wz%s1_ms.bin' % L) for L in 'afjr']
        files = [f for f in files if os.path.exists(f)]
        if not files:
            self.skipTest('no game data under %s (set ACZ_DATA)' % data)
        for f in files:
            m = decode_acm(ms_weapon_model(f))
            a = np.concatenate([me['pos'] for me in m['meshes'] if len(me['pos'])])
            r = np.abs(a[:, :2]).max(1)
            z0, z1 = float(a[:, 2].min()), float(a[:, 2].max()); k = 0.1 * (z1 - z0)
            self.assertLess(float(r[a[:, 2] < z0 + k].max()), float(r[a[:, 2] > z1 - k].max()), f)   # pointed tip at -z (the end slice), fins at +z

    def test_missing_station_is_an_error(self):
        B = Builder('t', SRC_MPL); B.add_texture(np.full((2, 2, 4), 9, np.uint8))
        with self.assertRaises(Exception):
            add_stores(B, decode_acm(self.ex), dict(pack=self.pack, gim=self.gim, index=0, stations=[99]))


class StoresSelection(unittest.TestCase):
    """STORES for the other aircraft: which pack entry is the missile, which mirrored station pairs are proposed, and the F-5E tank (mpl part 7)."""
    def _weapon(self, w, h, length):
        # a box-less 'weapon': one strip whose bounding box is w x h x length (triangle count 2)
        pos = [(0, 0, -length / 2), (0, 0, length / 2), (w, h, -length / 2), (w, h, length / 2)]
        return make_acm([node(0, 0)], [strip_batch(pos, [(0, 0), (0, 1), (1, 0), (1, 1)])])

    def test_missile_entry_is_the_first_long_thin_one_and_skips_empty_entries(self):
        with tempfile.TemporaryDirectory() as d:
            pk = os.path.join(d, 't.bin')
            bomb = self._weapon(4.0, 4.0, 10.0)      # aspect 2.5: not a missile
            wide = self._weapon(6.0, 6.0, 40.0)      # long but 6 u wide: not accepted (max width 4.5)
            missile = self._weapon(2.0, 2.0, 16.0)   # aspect 8
            open(pk, 'wb').write(container([b'\0' * 16, container([b'\0' * 16, bomb, b'\0' * 16]), container([b'\0' * 16, wide, b'\0' * 16]),
                                            container([b'\0' * 16, missile, b'\0' * 16])]))
            self.assertEqual(find_missile_index(pk, min_tris=1), 3)           # entry 0 is empty (decode error) and skipped; bomb and wide store rejected
            self.assertIsNone(find_missile_index(pk, min_tris=100))           # a triangle-count window nothing fits
            sh = weapon_shape(missile)
            self.assertEqual(sh[0], 2); self.assertAlmostEqual(float(sh[1][2]), 16.0, places=3)

    def _cand(self, g, x, plate, y=-4.0, z=-4.0, w=2.0):
        return dict(group=g, pos=[x, y, z], lo=np.array([x - w / 2, y - 1, z - 8]), hi=np.array([x + w / 2, y + 1, z + 8]), plate=plate)

    def test_choose_stations_pairs_overlap_and_racks(self):
        c = [self._cand(67, -15, 40), self._cand(68, 15, 40),                       # inside a rack mesh (40 plate vertices): rejected
             self._cand(71, -15, 5), self._cand(72, 15, 5),                         # free outer pair
             self._cand(73, -12, 8), self._cand(74, 12, 8),                         # free inner pair, 3 u inboard: no overlap with the outer pair (2 u wide)
             self._cand(75, -12, 9, z=-4.5), self._cand(76, 12, 9, z=-4.5),         # the same place as 73/74: dropped by the overlap rule
             self._cand(79, 0, 0)]                                                  # centre line: never proposed
        self.assertEqual(choose_stations(c), [71, 72, 73, 74])
        self.assertEqual(choose_stations(c, max_n=2), [71, 72])
        # a station without its mirror is dropped; a lone centre-line one never counts
        self.assertEqual(choose_stations([self._cand(71, -15, 0), self._cand(72, 15, 99)]), [])
        self.assertEqual(choose_stations([self._cand(71, -15, 0), self._cand(72, 15, 0)]), [71, 72])
        self.assertEqual(choose_stations([]), [])

    def test_station_candidates_count_plate_vertices_inside_the_weapon(self):
        loc = lambda g, t: (8, 0, -1, 0, g, -1, -1, -1, t, 1.0, (0, 0, 0), 0.0)
        # exterior: root, a rack plate (group 54) with a mesh around (-10, -4, -3), locators 71 at (-10, -4, -3) and 72 at (10, -4, -3)
        plate = [(-9.8, -3.5, -4), (-9.3, -3.5, -4), (-9.8, -2.5, -2), (-9.3, -2.5, -2)]   # the weapon box starts at the locator (no mirror)
        ex = decode_acm(make_acm([node(3, -1), node(0, 0, flags=0xa), loc(71, (-10, -4, -3)), loc(72, (10, -4, -3))], [strip_batch(plate, [(0, 0), (0, 1), (1, 0), (1, 1)])]))
        ex['nodes'] = ex['nodes'].copy(); ex['nodes']['group'][1] = 54
        weapon = self._weapon(2.0, 2.0, 10.0)
        c = {k['group']: k for k in station_candidates(ex, weapon)}
        self.assertEqual(sorted(c), [71, 72])
        self.assertEqual(c[71]['plate'], 4)        # the whole rack plate lies inside the weapon box
        self.assertEqual(c[72]['plate'], 0)

    def test_tank_is_part_7_at_the_centre_line_locator_with_the_skin_texture(self):
        loc = lambda g, t: (8, 0, -1, 0, g, -1, -1, -1, t, 1.0, (0, 0, 0), 0.0)
        ex = decode_acm(make_acm([node(1, -1), loc(79, (0, -4, -8))], []))
        tankpos = [(-2, -1, -10), (-2, -1, 10), (2, 1, -10), (2, 1, 10)]
        p7 = make_acm([node(0, 0, flags=0xa)], [strip_batch(tankpos, [(0, 0), (0, 1), (1, 0), (1, 1)])])
        P = [b'\0' * 16] * 10; P[7] = p7
        B = Builder('t', SRC_MPL)
        t_skin = B.add_texture(np.full((2, 2, 4), 5, np.uint8))
        n0 = len(B.parts)
        self.assertEqual(add_tank(B, ex, P, t_skin, dict(station=79, part=7)), (79, 1))
        self.assertEqual(len(B.parts) - n0, 1)
        q = B.parts[-1]
        self.assertTrue(q['name'] and q['mflags'] & MF_EXTERIOR and not q['mflags'] & MF_HIDDEN)
        self.assertEqual((q['src'], q['tex']), (STORES_SRC_PART, t_skin))
        self.assertIn(b'/tank79/n0', bytes(B.strings))           # named tank, never 'store': the weapon mask cannot hide it
        self.assertNotIn(b'/store79/', bytes(B.strings))
        with tempfile.TemporaryDirectory() as d:
            pth = os.path.join(d, 'k.zvrm'); B.eye = (0, 0, 0); B.write(pth); z = read_zvrm(pth)
            v = z['V']['pos'][np.unique(z['I'][z['parts'][-1]['fi']:z['parts'][-1]['fi'] + z['parts'][-1]['ic']])]
            # authored frame kept (nose -z like the aircraft, no 180 degree turn): z -10..10 about the locator z -8 -> -18..2, y about -4
            self.assertAlmostEqual(float(v[:, 2].min()), -18.0, places=3); self.assertAlmostEqual(float(v[:, 2].max()), 2.0, places=3)
            self.assertAlmostEqual(float(v[:, 1].mean()), -4.0, delta=1.01)
        with self.assertRaises(Exception):
            add_tank(B, ex, P, t_skin, dict(station=99, part=7))        # no such locator
        with self.assertRaises(Exception):
            add_tank(B, ex, P, t_skin, dict(station=79, part=6))        # part 6 is not an ACM here


class SpStores(unittest.TestCase):
    """SP-STORES: the special weapon is its own station group ('<model>/sp<station>/n<node>'), its stations are the locators under the weapon slot node
    (group 53/54/55 = slot 0/1/2) and it shares the weapon atlas with the standard missile."""
    def setUp(self):
        loc = lambda g, t: (8, 0, -1, 0, g, -1, -1, -1, t, 1.0, (0, 0, 0), 0.0)
        slot = lambda g, n: (8, n, -1, 0, g, -1, -1, -1, (0, 0, 0), 1.0, (0, 0, 0), 0.0)
        # root; slot 53 -> 67,68 ; slot 54 -> 71,72,73,74 ; slot 55 -> 75,76 ; a free locator 79 (centre line) under the root
        nodes = [node(4, -1), slot(53, 2), loc(67, (-10, -4, -3)), loc(68, (10, -4, -3)),
                 slot(54, 4), loc(71, (-10, -4, -5)), loc(72, (10, -4, -5)), loc(73, (-6, -4, -4)), loc(74, (6, -4, -4)),
                 slot(55, 2), loc(75, (-6, -4, -4)), loc(76, (6, -4, -4)), loc(79, (0, -5, -8))]
        self.ex = make_acm(nodes, [])
        plate = [(0, 0, -7), (0, 0, 7), (1, 0, -7), (1, 0, 7)]; uv = [(0, 0), (0, 1), (1, 0), (1, 1)]
        self.weapon = make_acm([node(0, 0)], [strip_batch(plate, uv)])
        self.tmp = tempfile.TemporaryDirectory()
        self.pack = os.path.join(self.tmp.name, 'f05eex.bin')
        w = lambda: container([b'\0' * 16, self.weapon, b'\0' * 16, b'\0' * 16])
        open(self.pack, 'wb').write(container([w(), w(), w()]))
        self.gim = os.path.join(self.tmp.name, 'weapon.gim')
        open(self.gim, 'wb').write(b'GIM\0' + b'\0' * 15 + b'\0' + b'\0' * 8 + struct.pack('<HH', 1, 1) + bytes([10, 20, 30, 128]))

    def tearDown(self):
        self.tmp.cleanup()

    def test_slot_stations_are_the_locator_children_of_the_slot_node(self):
        ex = decode_acm(self.ex)
        self.assertEqual(slot_stations(ex, 0), [67, 68])
        self.assertEqual(slot_stations(ex, 1), [71, 72, 73, 74])
        self.assertEqual(slot_stations(ex, 2), [75, 76])
        self.assertEqual(slot_stations(ex, 3), [])         # no group 56 node
        self.assertNotIn(79, sum((slot_stations(ex, s) for s in range(3)), []))   # a locator outside the slots is nobody's

    def test_sp_group_is_named_sp_and_shares_the_weapon_atlas(self):
        B = Builder('t', SRC_MPL)
        B.add_texture(np.full((2, 2, 4), 9, np.uint8))
        ex = decode_acm(self.ex)
        r0 = add_stores(B, ex, dict(pack=self.pack, gim=self.gim, index=0, stations=[67, 68]))
        ntex = len(B.textures)
        r1 = add_stores(B, ex, dict(pack=self.pack, gim=self.gim, index=1, stations=[71, 72, 73, 74]), prefix='sp')
        self.assertEqual(r0, [(67, 1), (68, 1)]); self.assertEqual(r1, [(71, 1), (72, 1), (73, 1), (74, 1)])
        self.assertEqual(len(B.textures), ntex)            # one weapon atlas for both groups (the GPU has 4 slots)
        names = bytes(B.strings)
        for g in (71, 72, 73, 74):
            self.assertIn(('/sp%d/n0' % g).encode(), names)
            self.assertNotIn(('/store%d/' % g).encode(), names)   # the standard-missile mask never claims an SP part
        for g in (67, 68):
            self.assertIn(('/store%d/n0' % g).encode(), names)
        sp = B.parts[-4:]
        self.assertTrue(all(p['src'] == STORES_SRC_PART and p['mflags'] & MF_EXTERIOR and not p['mflags'] & MF_HIDDEN for p in sp))
        self.assertTrue(all(p['tex'] == B.parts[-5]['tex'] for p in sp))

    def test_default_bake_table_and_override(self):
        import bake_all_aircraft as ba
        ex = decode_acm(self.ex)
        d = os.path.join(self.tmp.name, 'data');
        spec = ba.sp_spec('f05e', d, ex)
        self.assertEqual((spec['index'], spec['stations']), (0, [67, 68]))      # SAAM = the first special = slot 0
        self.assertTrue(spec['pack'].endswith('f05eex.bin') and spec['gim'].endswith('weapon.gim'))
        self.assertEqual(ba.sp_spec('f05e', d, ex, '2')['stations'], [75, 76])
        self.assertEqual(ba.sp_spec('f05e', d, ex, '1')['index'], 1)
        # the standard group skips the stations the SP group takes (one locator, one weapon)
        std = dict(pack='p', index=0, stations=[67, 68, 75, 76])
        self.assertEqual(ba.dedupe_std(std, spec)['stations'], [75, 76])
        self.assertEqual(std['stations'], [67, 68, 75, 76])                      # the table entry itself is untouched
        self.assertIsNone(ba.dedupe_std(dict(std, stations=[67, 68]), spec))       # nothing left: no std group
        self.assertIs(ba.dedupe_std(std, None), std)
        self.assertIsNone(ba.sp_spec('f05e', d, ex, 'none'))
        self.assertIsNone(ba.sp_spec('j35j', d, ex))         # no table entry: the file stays the STORES bake
        self.assertIsNone(ba.sp_spec('f05e', d, ex, '3'))     # no such slot in the mpl: nothing is baked


class StdMs(unittest.TestCase):
    """STD-MS: the STANDARD missile (HUD MSSL) is data/lodweapon/wz<L>1_ms.bin, never an ex.bin entry (entry i = special weapon i, so entry 0 is the SAAM)."""
    def setUp(self):
        loc = lambda g, t: (8, 0, -1, 0, g, -1, -1, -1, t, 1.0, (0, 0, 0), 0.0)
        self.ex = make_acm([node(2, -1), loc(75, (-6, -4, -4)), loc(76, (6, -4, -4))], [])
        uv = [(0, 0), (0, 1), (1, 0), (1, 1)]
        # the ex.bin entry 0 (SAAM stand-in): a 14 u plate (z -7..7); the standard missile stand-in: a 6 u plate (z -3..3) with 2 nodes
        saam = make_acm([node(0, 0)], [strip_batch([(0, 0, -7), (0, 0, 7), (1, 0, -7), (1, 0, 7)], uv)])
        std = make_acm([node(1, 0), node(0, 1, (0, 0, -3), flags=0xa)],
                       [strip_batch([(0, 0, -3), (0, 0, 3), (1, 0, -3), (1, 0, 3)], uv), strip_batch([(0, 0, -3), (0, 0, 3), (1, 0, -3)], uv[:3])])
        self.saam, self.std = saam, std
        self.tmp = tempfile.TemporaryDirectory()
        d = self.tmp.name
        self.pack = os.path.join(d, 'f05eex.bin')
        open(self.pack, 'wb').write(container([container([b'\0' * 16, saam, b'\0' * 16, b'\0' * 16])] * 3))
        self.ms = os.path.join(d, 'wzf1_ms.bin')
        open(self.ms, 'wb').write(container([b'\0' * 16, std, b'\0' * 16, b'\0' * 16]))
        self.gim = os.path.join(d, 'weapon.gim')
        open(self.gim, 'wb').write(b'GIM\0' + b'\0' * 15 + b'\0' + b'\0' * 8 + struct.pack('<HH', 1, 1) + bytes([10, 20, 30, 128]))

    def tearDown(self):
        self.tmp.cleanup()

    def _bake(self, **stores):
        B = Builder('t', SRC_MPL)
        B.add_texture(np.full((2, 2, 4), 9, np.uint8))
        r = add_stores(B, decode_acm(self.ex), dict(gim=self.gim, stations=[75, 76], **stores))
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 's.zvrm'); B.eye = (0, 0, 0); B.write(p); z = read_zvrm(p)
        return r, z

    def test_ms_weapon_model_is_part_1_of_the_file(self):
        self.assertEqual(ms_weapon_model(self.ms), self.std)
        bad = os.path.join(self.tmp.name, 'bad_ms.bin')
        open(bad, 'wb').write(container([b'\0' * 16, b'\0' * 16]))
        with self.assertRaises(Exception):
            ms_weapon_model(bad)

    def test_standard_group_uses_the_ms_mesh_not_the_ex_bin_entry_0(self):
        r, z = self._bake(pack=self.pack, index=0, ms=self.ms)
        self.assertEqual(r, [(75, 2), (76, 2)])                  # the 2-node standard missile, not the 1-node ex.bin entry 0
        for g in (75, 76):
            names = [p['name'] for p in z['parts'] if ('store%d/' % g) in p['name']]
            self.assertEqual(len(names), 2)
            body = [p for p in z['parts'] if p['name'].endswith('store%d/n0' % g)][0]
            q = z['V']['pos'][np.unique(z['I'][body['fi']:body['fi'] + body['ic']])]
            self.assertLess(float(q[:, 2].max() - q[:, 2].min()), 6.5)   # the 6 u standard missile, never the 14 u ex.bin entry 0
        _, z0 = self._bake(pack=self.pack, index=0)              # the old path (no ms) still draws the ex.bin entry
        body0 = [p for p in z0['parts'] if p['name'].endswith('store75/n0')][0]
        q0 = z0['V']['pos'][np.unique(z0['I'][body0['fi']:body0['fi'] + body0['ic']])]
        self.assertGreater(float(q0[:, 2].max() - q0[:, 2].min()), 13.0)

    def test_ms_file_needs_no_pack(self):
        r, _ = self._bake(ms=self.ms)
        self.assertEqual(r, [(75, 2), (76, 2)])

    def test_standard_file_by_first_letter_and_fallback(self):
        import bake_all_aircraft as ba
        for t, f in (('f05e', 'wzf1_ms.bin'), ('f15c', 'wzf1_ms.bin'), ('fa22', 'wzf1_ms.bin'), ('a10a', 'wza1_ms.bin'), ('j35j', 'wzj1_ms.bin'), ('j39c', 'wzj1_ms.bin'),
                     ('rm01', 'wzr1_ms.bin'), ('su27', ba.std_ms_name('f05e')), ('m29a', ba.std_ms_name('f05e')), ('yf23', ba.std_ms_name('f05e'))):
            self.assertEqual(ba.std_ms_name(t), f)
        self.assertTrue(all(ba.std_ms_name(t) in ('wza1_ms.bin', 'wzf1_ms.bin', 'wzj1_ms.bin', 'wzr1_ms.bin') for t in ba.STORES))

    def test_f05e_standard_group_is_the_ms_file_and_sp_keeps_the_ex_bin_entry(self):
        import bake_all_aircraft as ba
        d = os.path.join(self.tmp.name, 'data')
        st = dict(ba.STORES['f05e']); st['pack'] = self.pack; st['gim'] = self.gim; st['ms'] = os.path.join(d, 'lodweapon', ba.std_ms_name('f05e'))
        self.assertTrue(st['ms'].endswith('wzf1_ms.bin') and st['ms'] != self.pack)
        self.assertNotIn('ms', ba.STORES['f05e'])               # the table itself stays free of absolute paths
        self.assertEqual(ba.SP_STORES['f05e']['slot'], 0)         # the special group still reads ex.bin entry 0 (SAAM), through sp_spec


class CockpitAlphaRule(unittest.TestCase):
    def test_alpha_is_coverage_only_where_the_game_blends(self):
        self.assertFalse(cockpit_alpha_test(0x0008))   # lit opaque panel (F-5E main panels): texel alpha is a mask, must stay opaque
        self.assertTrue(cockpit_alpha_test(0x0009))    # blend
        self.assertTrue(cockpit_alpha_test(0x2009))    # glass overlay
        self.assertTrue(cockpit_alpha_test(0x2001))
        self.assertTrue(cockpit_alpha_test(0x0000))    # unlit decal-like: unchanged (unverified)


if __name__ == '__main__':
    unittest.main()
