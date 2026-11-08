"""Synthetic (no game data) tests for cockpit_seat_clearance.py / the SEAT-CLEARANCE port: python -I -B test_cockpit_seat_clearance.py"""
import os, re, sys, unittest
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cockpit_seat_clearance as sc
from cockpit_clip_survey import interior, seat_back_max, U
from model_baker import MF_EXTERIOR

HERE = os.path.dirname(os.path.abspath(__file__))
EYE = np.array([0.0, 4.0, -30.0])


def synth(rear_m=1.0, front_m=1.0, half_m=0.4, low_m=0.6, high_m=0.3, game_behind=None):
    """One game cockpit part (a box of 12 triangles spanning the footprint around EYE) + optional game triangle `game_behind` (x,y,z metres rel. the eye)."""
    lo = EYE + np.array([-half_m, -low_m, -front_m]) * U
    hi = EYE + np.array([half_m, high_m, rear_m]) * U
    c = np.array([[x, y, z] for x in (lo[0], hi[0]) for y in (lo[1], hi[1]) for z in (lo[2], hi[2])], np.float32)
    quads = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    pos, idx = [], []
    for q in quads:
        for tri in ((q[0], q[1], q[2]), (q[0], q[2], q[3])):
            for k in tri:
                idx.append(len(pos)); pos.append(c[k])
    parts = [dict(name='syn/p2/n1', mflags=0, fi=0, ic=len(idx), tex=-1)]
    if game_behind is not None:
        g = EYE + np.array(game_behind, float) * U
        for d in ((0, 0, 0), (0.05, 0, 0), (0, 0.05, 0)):
            idx.append(len(pos)); pos.append(np.float32(g + np.array(d) * U))
        parts.append(dict(name='syn/p2/n2', mflags=0, fi=len(idx) - 3, ic=3, tex=-1))
    V = np.zeros(len(pos), dtype=[('pos', '<f4', 3)])
    V['pos'] = np.array(pos)
    return dict(parts=parts, V=V, I=np.array(idx, np.uint32), eye=EYE.tolist(), flags=0)


class SeatClearance(unittest.TestCase):
    def test_seat_back_max(self):
        self.assertAlmostEqual(seat_back_max(0.36, 0.30), 0.45)
        self.assertEqual(seat_back_max(0.36, 0.0), 0.36)         # off
        self.assertEqual(seat_back_max(0.36, 0.10), 0.36)        # never moved forward
        self.assertAlmostEqual(seat_back_max(0.10, 0.30), 0.45)  # a short footprint is pushed back too
        self.assertEqual(seat_back_max(0.36, float('nan')), 0.36)
        self.assertEqual(seat_back_max(0.36, -1.0), 0.36)

    def test_default_matches_cpp(self):
        with open(os.path.join(HERE, '..', '..', 'src', 'acz-vr', 'cockpit_model.h'), encoding='utf-8') as f:
            h = f.read()
        m = re.search(r'float seat_back = ([0-9.]+)f;', h)
        self.assertIsNotNone(m)
        self.assertAlmostEqual(float(m.group(1)), sc.DEFAULT_SEATBACK)
        self.assertIn('inline float seat_back_max(float back_max_old, float seatback_m)', h)

    def headrest_front(self, z, seatback):
        it, _ = interior(z, EYE, seatback=seatback)
        t = dict(it)['headrest']
        return float((t.reshape(-1, 3)[:, 2].min() - EYE[2]) / U)

    def test_headrest_placement(self):
        z = synth()
        self.assertAlmostEqual(self.headrest_front(z, 0.0), 0.21, places=4)    # the COCKPIT-4 placement (0.36 clamp - 0.15)
        self.assertAlmostEqual(self.headrest_front(z, 0.30), 0.30, places=4)
        self.assertAlmostEqual(self.headrest_front(z, 0.40), 0.40, places=4)
        self.assertAlmostEqual(self.headrest_front(z, 0.10), 0.21, places=4)   # never forward of the old placement
        # a short cockpit footprint (ends 0.12 m behind the eye): old = 0.10 - 0.15 = headrest AHEAD of the eye; now behind it
        zs = synth(rear_m=0.12)
        self.assertLess(self.headrest_front(zs, 0.0), 0.0)
        self.assertAlmostEqual(self.headrest_front(zs, 0.30), 0.30, places=4)

    def test_measure_before_after(self):
        z = synth()
        b = sc.measure(z, EYE, 0.0, 1.15, 5.0, None)
        a = sc.measure(z, EYE, 0.30, 1.15, 5.0, None)
        self.assertAlmostEqual(b['aft_d'], 0.21 * 1.15, places=3)               # physical = model metres * cscale
        self.assertAlmostEqual(a['aft_d'], 0.30 * 1.15, places=3)
        self.assertEqual(sc.verdict(b), 'FAIL')
        self.assertEqual(sc.verdict(a), 'pass')
        self.assertLess(b['look'], 0.0)                                         # the head swings into the old headrest/seat back
        self.assertGreaterEqual(a['look'], 0.0)
        self.assertGreaterEqual(sc.clear_of(a), sc.MIN_CLEAR_M)
        self.assertGreater(sc.clear_of(a), sc.clear_of(b))
        # cscale 1.0: the same model distance is 1/1.15 smaller physically
        a1 = sc.measure(z, EYE, 0.30, 1.0, 5.0, None)
        self.assertAlmostEqual(a1['aft_d'], 0.30, places=3)

    def test_game_limited(self):
        z = synth(game_behind=(0.0, 0.0, 0.12))      # a game triangle 0.12 m (model) behind the eye: 0.138 m physical, nearer than the headrest
        a = sc.measure(z, EYE, 0.30, 1.15, 5.0, None)
        self.assertLess(a['game_d'], 0.15)
        self.assertEqual(sc.verdict(a), 'game-limited')   # the procedural seat clears the head; the game's own triangle is the nearer limit
        zf = synth(game_behind=(0.0, 0.0, 0.06))     # closer than 0.10 m aft of the eye plane: ignored as overhead/instrument structure
        self.assertAlmostEqual(sc.measure(zf, EYE, 0.30, 1.15, 5.0, None)['game_d'], sc.measure(synth(), EYE, 0.30, 1.15, 5.0, None)['game_d'], places=6)

    def test_tub_extends_to_seat(self):
        zs = synth(rear_m=0.12)   # footprint ends 0.12 m behind the eye: the moved seat (rails to 0.45 m) must be inside the tub
        for sb, want in ((0.0, False), (0.30, True)):
            it, _ = interior(zs, EYE, seatback=sb)
            rear = max(float(t.reshape(-1, 3)[:, 2].max()) for n, t in it if n == 'tub-floor')
            self.assertEqual(rear >= EYE[2] + 0.45 * U - 1e-3, want)

    def test_clipbox_footprint(self):
        z = synth(rear_m=1.0)
        it, info = interior(z, EYE, seatback=0.30, clipbox=(1.0, 0.5, 1.0, 0.3, 0.4))
        self.assertAlmostEqual(float((info['hi'][2] - EYE[2]) / U), 0.5, places=4)

    def test_head_model_sane(self):
        self.assertAlmostEqual(sc.HEAD_BACK_M, 0.17)
        # the occiput's furthest swing about the neck pivot bounds the look-up reach: |occiput - neck| + neck_z
        v = sc.HEAD_PTS - sc.NECK
        reach = float((np.linalg.norm(v[:, 1:], axis=1) + sc.NECK[2]).max())
        self.assertLess(reach, sc.DEFAULT_SEATBACK * 1.15 + 0.01)    # the default seat clears the sweep at the headset cscale


if __name__ == '__main__':
    unittest.main()
