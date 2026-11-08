"""Pure offline contract fixtures; no engine, files, network or child processes.

Run with Python -B. Expected values are analytic, not snapshots of the helpers.
"""
import math
import unittest


def identity():
    return [[float(r == c) for c in range(4)] for r in range(4)]


def multiply(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(4))
             for c in range(4)] for r in range(4)]


def apply(m, p):
    return [sum(m[r][c] * p[c] for c in range(4)) for r in range(4)]


def pose(position=(0, 0, 0), axis='y', angle=0):
    m = identity()
    a, b = {'x': (1, 2), 'y': (2, 0), 'z': (0, 1)}[axis]
    m[a][a] = m[b][b] = math.cos(angle)
    m[a][b], m[b][a] = -math.sin(angle), math.sin(angle)
    for i in range(3):
        m[i][3] = position[i]
    return m


def inverse_rigid(m):
    inv = identity()
    for r in range(3):
        for c in range(3):
            inv[r][c] = m[c][r]
        inv[r][3] = -sum(m[c][r] * m[c][3] for c in range(3))
    return inv


def recenter(position, yaw):
    rotation = pose(angle=-yaw)
    translation = apply(rotation, [*position, 0])
    for i in range(3):
        rotation[i][3] = -translation[i]
    return rotation


HEADING_THRESHOLD = 1e-6  # Dimensionless horizontal length of unit forward.


def heading(orientation):
    forward = apply(orientation, [0, 0, -1, 0])
    if math.hypot(forward[0], forward[2]) <= HEADING_THRESHOLD:
        return None
    return math.atan2(-forward[0], -forward[2])


def recenter_from_head(head):
    yaw = heading(head)
    if yaw is None:
        return None  # Defer: caller retains prior valid reset, never uses identity.
    return recenter([head[i][3] for i in range(3)], yaw)


def quaternion_pose(x, y, z, w):
    if not all(math.isfinite(v) for v in (x, y, z, w)):
        raise ValueError('Invalid quaternion')
    length = math.hypot(x, y, z, w)
    if length == 0:
        raise ValueError('Invalid quaternion')
    x, y, z, w = (v / length for v in (x, y, z, w))
    return [[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w), 0],
            [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w), 0],
            [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y), 0],
            [0, 0, 0, 1]]


def compose(aircraft, seat, reset, tracked_eye):
    return multiply(multiply(multiply(aircraft, seat), reset), tracked_eye)


def projection(l, r, b, t, near, far):
    if (not all(math.isfinite(v) for v in (l, r, b, t, near, far))
            or not (r > l and t > b and 0 < near < far)):
        raise ValueError('Invalid frustum')
    return [[2 / (r-l), 0, (r+l) / (r-l), 0],
            [0, 2 / (t-b), (t+b) / (t-b), 0],
            [0, 0, -far / (far-near), -far*near / (far-near)],
            [0, 0, -1, 0]]


def ndc(proj, point):
    clip = apply(proj, [*point, 1])
    return [v / clip[3] for v in clip[:3]]


def projection_from_xrfov(fov, near, far):
    # Named fields avoid mistaking XrFovf's up/down order for contract order.
    angles = [fov[name] for name in
              ('angleLeft', 'angleRight', 'angleDown', 'angleUp')]
    if not all(math.isfinite(a) and -math.pi/2 < a < math.pi/2
               for a in angles):
        raise ValueError('Invalid FOV angles')
    return projection(*(math.tan(a) for a in angles), near, far)


class ContractTests(unittest.TestCase):
    def assert_vector(self, actual, expected):
        self.assertEqual(len(actual), len(expected))
        for a, e in zip(actual, expected):
            self.assertTrue(math.isfinite(a))
            self.assertLessEqual(abs(a-e), 1e-9, (actual, expected))

    def test_order_and_inverse(self):
        eye = compose(pose((10, 2, 3), angle=math.pi/2),
                      pose((0, 1, -2)), identity(), pose((0.03, 0, -0.1)))
        self.assert_vector(apply(eye, [0, 0, 0, 1]), [7.9, 3, 2.97, 1])
        self.assert_vector(apply(eye, [0, 0, -1, 0]), [-1, 0, 0, 0])
        self.assert_vector(apply(inverse_rigid(eye), [7.9, 3, 2.97, 1]),
                           [0, 0, 0, 1])

    def test_identity_recenter(self):
        eye = compose(identity(), identity(), recenter((0, 0, 0), 0),
                      pose((-0.032, 0, 0)))
        self.assert_vector(apply(eye, [0, 0, 0, 1]), [-0.032, 0, 0, 1])
        self.assert_vector(apply(eye, [0, 0, -1, 0]), [0, 0, -1, 0])

    def test_nonidentity_recenter_preserves_eye_offset_and_tilt(self):
        reset = recenter((1, 2, 3), math.pi/2)
        self.assert_vector(apply(reset, [0, 0, 0, 1]), [3, -2, -1, 1])
        head = pose((1, 2, 3), angle=math.pi/2)
        centered = multiply(reset, head)
        for actual, expected in zip(centered, identity()):
            self.assert_vector(actual, expected)
        eye = multiply(head, pose((0.032, 0, 0)))
        self.assert_vector(apply(multiply(reset, eye), [0, 0, 0, 1]),
                           [0.032, 0, 0, 1])
        tilted = multiply(head, pose(axis='x', angle=math.pi/6))
        self.assert_vector(apply(multiply(reset, tilted), [0, 0, -1, 0]),
                           [0, 0.5, -math.sqrt(3)/2, 0])

    def test_translation_once_and_fixed_cockpit_parallax(self):
        head = pose((0.1, 0.2, -0.3))
        eyes = [multiply(head, pose((offset, 0, 0)))
                for offset in (-0.032, 0.032)]
        left = compose(identity(), identity(), identity(), eyes[0])
        right = compose(identity(), identity(), identity(), eyes[1])
        self.assert_vector(apply(left, [0, 0, 0, 1]), [0.068, 0.2, -0.3, 1])
        self.assert_vector(apply(right, [0, 0, 0, 1]), [0.132, 0.2, -0.3, 1])
        self.assert_vector(apply(inverse_rigid(left), [0, 0, -1, 1]),
                           [-0.068, -0.2, -0.7, 1])
        doubled = multiply(head, left)
        self.assert_vector(apply(doubled, [0, 0, 0, 1]), [0.168, 0.4, -0.6, 1])

    def test_heading_extraction_and_recenter_from_orientation(self):
        tilted = multiply(pose((1, 2, 3), angle=math.pi/2),
                          pose(axis='x', angle=math.pi/6))
        self.assert_vector([heading(tilted)], [math.pi/2])
        reset = recenter_from_head(tilted)
        self.assert_vector(apply(reset, [0, 0, 0, 1]), [3, -2, -1, 1])
        centered = multiply(reset, tilted)
        self.assert_vector(apply(centered, [0, 0, 0, 1]), [0, 0, 0, 1])
        self.assert_vector(apply(centered, [0, 0, -1, 0]),
                           [0, 0.5, -math.sqrt(3)/2, 0])
        negative = pose((1, 0, 0), angle=-math.pi/3)
        self.assert_vector([heading(negative)], [-math.pi/3])
        centered = multiply(recenter_from_head(negative), negative)
        self.assert_vector(apply(centered, [0, 0, -1, 0]), [0, 0, -1, 0])
        self.assert_vector(apply(centered, [0, 0, 0, 1]), [0, 0, 0, 1])

    def test_heading_singularity_and_overturned_policy(self):
        for pitch in (math.pi/2, math.pi/2-0.5e-6, math.pi/2+0.5e-6):
            head = multiply(pose((1, 2, 3), angle=math.pi/3),
                            pose(axis='x', angle=pitch))
            self.assertIsNone(heading(head))
            self.assertIsNone(recenter_from_head(head))
        # Just outside the deferral band, the projected heading is available.
        head = multiply(pose(angle=math.pi/3),
                        pose(axis='x', angle=math.pi/2-2e-6))
        self.assert_vector([heading(head)], [math.pi/3])
        for pitch in (2*math.pi/3, -2*math.pi/3):
            head = multiply(pose(angle=math.pi/3), pose(axis='x', angle=pitch))
            self.assert_vector([heading(head)], [-2*math.pi/3])
            centered = multiply(recenter_from_head(head), head)
            self.assert_vector(apply(centered, [0, 0, -1, 0]),
                               [0, math.copysign(math.sqrt(3)/2, pitch), -0.5, 0])

    def test_xyzw_quaternion_active_rotation(self):
        expected = [[0, 0, 1, 0], [0, 1, 0, 0],
                    [-1, 0, 0, 0], [0, 0, 0, 1]]
        q = (0, math.sin(math.pi/4), 0, math.cos(math.pi/4))
        for scale in (1, 3, -1):
            rotation = quaternion_pose(*(scale*v for v in q))
            for actual, reference in zip(rotation, expected):
                self.assert_vector(actual, reference)
            self.assert_vector(apply(rotation, [0, 0, -1, 0]), [-1, 0, 0, 0])
            self.assert_vector(apply(rotation, [1, 0, 0, 0]), [0, 0, -1, 0])
        for invalid in ((0, 0, 0, 0), (math.nan, 0, 0, 1), (0, math.inf, 0, 1)):
            with self.assertRaises(ValueError):
                quaternion_pose(*invalid)

    def test_rotation_rotates_eye_baseline(self):
        head = pose((0.1, 0.2, -0.3), angle=math.pi/2)
        left = multiply(head, pose((-0.032, 0, 0)))
        right = multiply(head, pose((0.032, 0, 0)))
        lp, rp = apply(left, [0, 0, 0, 1]), apply(right, [0, 0, 0, 1])
        self.assert_vector(lp, [0.1, 0.2, -0.268, 1])
        self.assert_vector(rp, [0.1, 0.2, -0.332, 1])
        self.assert_vector([math.dist(lp[:3], rp[:3])], [0.064])
        for eye in (left, right):
            self.assert_vector(apply(eye, [0, 0, -1, 0]), [-1, 0, 0, 0])

    def test_pitch_and_roll(self):
        self.assert_vector(apply(pose(axis='x', angle=math.pi/2), [0, 0, -1, 0]),
                           [0, 1, 0, 0])
        self.assert_vector(apply(pose(axis='z', angle=math.pi/2), [1, 0, 0, 0]),
                           [0, 1, 0, 0])

    def test_asymmetric_matrix_and_center_ray(self):
        proj = projection(-1, 2, -0.5, 1.5, 0.1, 100)
        expected = [[2/3, 0, 1/3, 0], [0, 1, 0.5, 0],
                    [0, 0, -1000/999, -100/999], [0, 0, -1, 0]]
        for row, reference in zip(proj, expected):
            self.assert_vector(row, reference)
        self.assert_vector(ndc(proj, (0, 0, -1)), [-1/3, -0.5, 100/111])
        right = projection(-2, 1, -1.5, 0.5, 0.1, 100)
        self.assert_vector(ndc(right, (0, 0, -1)), [1/3, 0.5, 100/111])

    def test_frustum_edges_and_depth(self):
        proj = projection(-1, 2, -0.5, 1.5, 0.1, 100)
        # Derive boundary expectations geometrically, independently of coefficients.
        self.assert_vector([ndc(proj, (-2, 0, -2))[0],
                            ndc(proj, (4, 0, -2))[0]], [-1, 1])
        self.assert_vector([ndc(proj, (0, -1, -2))[1],
                            ndc(proj, (0, 3, -2))[1]], [-1, 1])
        self.assert_vector([ndc(proj, (0, 0, -0.1))[2],
                            ndc(proj, (0, 0, -100))[2]], [0, 1])

    def test_named_xrfov_angles_to_asymmetric_projection(self):
        fov = dict(angleLeft=-math.pi/4, angleRight=math.pi/6,
                   angleUp=math.pi/3, angleDown=-math.pi/6)
        proj = projection_from_xrfov(fov, 1, 10)
        # Analytic tangents: (-1, 1/sqrt(3), -1/sqrt(3), sqrt(3)).
        # Horizontal coefficients simplify to 3-sqrt(3), sqrt(3)-2.
        root3 = math.sqrt(3)
        expected = [[3-root3, 0, root3-2, 0], [0, root3/2, 0.5, 0],
                    [0, 0, -10/9, -10/9], [0, 0, -1, 0]]
        for actual, reference in zip(proj, expected):
            self.assert_vector(actual, reference)
        self.assert_vector(ndc(proj, (0, 0, -2)), [2-root3, -0.5, 5/9])
        self.assert_vector([ndc(proj, (-2, 0, -2))[0],
                            ndc(proj, (2/root3, 0, -2))[0]], [-1, 1])
        self.assert_vector([ndc(proj, (0, -2/root3, -2))[1],
                            ndc(proj, (0, 2*root3, -2))[1]], [-1, 1])
        self.assert_vector([ndc(proj, (0, 0, -1))[2],
                            ndc(proj, (0, 0, -10))[2]], [0, 1])

    def test_xrfov_angle_domain_and_order_rejected(self):
        valid = dict(angleLeft=-math.pi/4, angleRight=math.pi/6,
                     angleUp=math.pi/3, angleDown=-math.pi/6)
        for name in valid:
            for bad in (math.nan, math.inf, -math.inf, math.pi/2, -math.pi/2,
                        math.pi/2+0.01, -math.pi/2-0.01, math.pi):
                with self.subTest(field=name, angle=bad):
                    fov = dict(valid, **{name: bad})
                    with self.assertRaises(ValueError):
                        projection_from_xrfov(fov, 1, 10)
        for changes in (dict(angleRight=-math.pi/4),
                        dict(angleLeft=math.pi/3),
                        dict(angleUp=-math.pi/6),
                        dict(angleDown=math.pi/3+0.01)):
            with self.assertRaises(ValueError):
                projection_from_xrfov(dict(valid, **changes), 1, 10)

    def test_invalid_frusta_rejected(self):
        for values in ((1, 1, -1, 1, 0.1, 100), (-1, 1, 1, -1, 0.1, 100),
                       (-1, 1, -1, 1, 0, 100), (-1, 1, -1, 1, 1, 1),
                       (-1, math.inf, -1, 1, 0.1, 100)):
            with self.assertRaises(ValueError):
                projection(*values)

    def test_hypothetical_units_and_reflected_basis(self):
        # D includes 100 native units/metre and reflects Z. This is no engine claim.
        d = [[100, 0, 0, 0], [0, 100, 0, 0], [0, 0, -100, 0], [0, 0, 0, 1]]
        di = [[0.01, 0, 0, 0], [0, 0.01, 0, 0],
              [0, 0, -0.01, 0], [0, 0, 0, 1]]
        native = multiply(multiply(d, pose((0.1, 0.2, -0.3), angle=math.pi/2)), di)
        self.assert_vector(apply(native, [0, 0, 0, 1]), [10, 20, 30, 1])
        # T_native maps native-local to native-world, not native to canonical.
        self.assert_vector(apply(native, [0, 0, 1, 0]), [-1, 0, 0, 0])
        # Basis alone maps native +Z to canonical -Z (normalized direction).
        basis_inverse = [[1, 0, 0, 0], [0, 1, 0, 0],
                         [0, 0, -1, 0], [0, 0, 0, 1]]
        self.assert_vector(apply(basis_inverse, [0, 0, 1, 0]), [0, 0, -1, 0])


if __name__ == '__main__':
    unittest.main()
