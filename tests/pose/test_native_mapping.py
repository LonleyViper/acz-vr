"""Pure numeric J-006 fixtures. Run with python -B; no runtime parity claim."""
import math
import unittest
from test_pose_contract import (identity, multiply, apply, pose, inverse_rigid,
                                projection, projection_from_xrfov, ndc)


def transpose(m):
    return [list(row) for row in zip(*m)]


def apply_row(point, matrix):
    return [sum(point[r]*matrix[r][c] for r in range(4)) for c in range(4)]


def row_vp(proj, eye, canonical_from_native):
    return transpose(multiply(multiply(proj, inverse_rigid(eye)),
                              canonical_from_native))


class NativeMappingTests(unittest.TestCase):
    H, V = 0.6152, 0.3460  # Rounded E3 measurements, not headset FOV.

    def assert_values(self, actual, expected, tolerance=1e-9):
        for a, e in zip(actual, expected):
            self.assertTrue(math.isfinite(a))
            self.assertLessEqual(abs(a-e), tolerance, (actual, expected))

    def lens(self):
        return projection_from_xrfov(dict(angleLeft=-math.atan(self.H),
            angleRight=math.atan(self.H), angleDown=-math.atan(self.V),
            angleUp=math.atan(self.V)), 1, 10)  # Synthetic near/far metres.

    def test_rounded_lens_coefficients_and_angles(self):
        p = self.lens()
        # Independent rounded expectations; tolerance reflects tangent rounding.
        self.assert_values([p[0][0], p[1][1]], [1.625488, 2.890173], 0.0002)
        self.assert_values([math.degrees(2*math.atan(self.H)),
                            math.degrees(2*math.atan(self.V))],
                           [63.1997, 38.1712], 0.01)

    def test_lens_edges_and_center(self):
        p = self.lens()
        self.assert_values([ndc(p, (-1.2304, 0, -2))[0],
                            ndc(p, (1.2304, 0, -2))[0],
                            ndc(p, (0, -0.692, -2))[1],
                            ndc(p, (0, 0.692, -2))[1]], [-1, 1, -1, 1])
        self.assert_values(ndc(p, (0, 0, -2)), [0, 0, 5/9])

    def test_nonidentity_local_eye_and_row_order(self):
        eye = pose((1, 2, 3), angle=math.pi/2)
        local = pose((-1, 2, 3), axis='z', angle=math.pi/2)
        p = self.lens()
        vp = row_vp(p, eye, identity())
        m64 = multiply(transpose(local), vp)
        expected = [0, 1/self.V, 10/9, 2]
        self.assert_values(apply_row([1, 0, 0, 1], m64), expected)
        column = multiply(multiply(p, inverse_rigid(eye)), local)
        self.assert_values(apply(column, [1, 0, 0, 1]), expected)
        for row, reference in zip(m64, transpose(column)):
            self.assert_values(row, reference)
        wrong = apply_row([1, 0, 0, 1], multiply(vp, transpose(local)))
        self.assertGreater(math.dist(wrong, expected), 1)
        # Translation moves to last row, not last column.
        self.assert_values(transpose(local)[3], [-1, 2, 3, 1])

    def test_one_sided_basis_units_and_preserved_fixed_scale(self):
        # All calibration values synthetic. 16 in L is separate from s=100.
        c = [[0.01, 0, 0, 0], [0, 0.01, 0, 0],
             [0, 0, -0.01, 0], [0, 0, 0, 1]]
        local_row = [[16, 0, 0, 0], [0, 16, 0, 0],
                     [0, 0, 16, 0], [100, 200, 300, 1]]
        local_point = [1, 0, 0, 1]
        self.assert_values(apply(c, apply_row(local_point, local_row)),
                           [1.16, 2, -3, 1])
        eye = pose((1, 2, -1), angle=math.pi/2)
        m64 = multiply(local_row, row_vp(self.lens(), eye, c))
        expected = [2/self.H, 0, -58/45, -0.16]
        self.assert_values(apply_row(local_point, m64), expected)
        column = multiply(multiply(multiply(self.lens(), inverse_rigid(eye)), c),
                          transpose(local_row))
        self.assert_values(apply(column, local_point), expected)
        # A nonzero origin is also one-sided: N origin maps to W origin offset.
        c[0][3], c[1][3], c[2][3] = 5, -2, 1
        self.assert_values(apply_row([0, 0, 0, 1], row_vp(identity(), identity(), c)),
                           [5, -2, 1, 1])

    def test_distinct_eyes_parallax_and_independent_fov(self):
        # Located-eye equivalents contain head translation and IPD exactly once.
        eyes = [pose((0.1+offset, 0, 0)) for offset in (-0.032, 0.032)]
        xs = []
        for eye in eyes:
            m = row_vp(self.lens(), eye, identity())
            clip = apply_row([0, 0, -1, 1], m)
            xs.append(clip[0]/clip[3])
            farther = apply_row([0, 0, -2, 1], m)
            self.assert_values([farther[0]/farther[3]], [xs[-1]/2])
        self.assert_values(xs, [-0.110533, -0.214564], 0.00002)
        self.assertGreater(abs(xs[1]-xs[0]), 0.1)
        right_p = projection(-self.H, 2*self.H, -self.V, self.V, 1, 10)
        # Each eye's own center ray isolates asymmetric lens offset.
        self.assert_values(ndc(right_p, (0, 0, -2)), [-1/3, 0, 5/9])
        right_clip = apply_row([0.132, 0, -2, 1],
                              row_vp(right_p, eyes[1], identity()))
        self.assert_values([v/right_clip[3] for v in right_clip[:3]],
                           [-1/3, 0, 5/9])
        self.assert_values(ndc(self.lens(), (0, 0, -2)), [0, 0, 5/9])

    def test_normalized_gs_screen_and_integer_depth_algebra(self):
        # Only inverse of the reported ps2_v formula for input w=1.
        for x, y, expected in ((0, 0, (2048, 2048)),
                               (-1, 1, (1792, 1824)),
                               (1, -1, (2304, 2272))):
            self.assert_values([2048+256*x, 2048-224*y], expected)
        self.assert_values([(2**24-1)*2**-24], [0.9999999403953552])


if __name__ == '__main__':
    unittest.main()
