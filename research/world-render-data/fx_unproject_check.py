"""Second check on frame 11906: (1) cloud sprites (w_in = q = 1/view-depth, z = 0): unproject with depth = 1/w_in -> world altitude;
(2) the GREATER streak draw: z model 2^24/w - 255.0078 vs alternatives (linear z), segment-length consistency."""
import os, numpy as np
CAP = r'%USERPROFILE%\ACZero-captures\E10\blobs'
b297 = open(os.path.join(CAP, '000334'), 'rb').read(); b329 = open(os.path.join(CAP, '000335'), 'rb').read()
A = np.frombuffer(b329, np.float32, 16, 967184).astype(np.float64).reshape(4, 4)
b = np.mean(A[0:3, 2] / A[0:3, 3]); Z0 = A[3, 2] - b * A[3, 3]
M = A[0:3][:, [0, 1, 3]]; t = A[3, [0, 1, 3]]
eye = np.linalg.solve(M.T, -t)
fwd = A[0:3, 3] / np.linalg.norm(A[0:3, 3])   # w column = view depth direction


def unproj_depth(xs, ys, d):
    return np.linalg.solve(M.T, np.array([xs * d, ys * d, d]) - t)


def project(P):
    h = np.append(P, 1.0) @ A
    return h[0] / h[3], h[1] / h[3], h[2] / h[3], h[3]


v = np.frombuffer(b297, np.float32, 48 * 16, 65728).reshape(48, 16).astype(np.float64)
P = np.array([unproj_depth(r[0], r[1], 1.0 / r[3]) for r in v])
print('eye', eye.round(1), 'forward', fwd.round(4))
print('clouds (depth = 1/w_in): view depth %.0f..%.0f ; world Y %.0f..%.0f mean %.0f sd %.0f' % ((1 / v[:, 3]).min(), (1 / v[:, 3]).max(), P[:, 1].min(), P[:, 1].max(), P[:, 1].mean(), P[:, 1].std()))
for q in range(12):
    Q = P[4 * q:4 * q + 4]
    n = np.cross(Q[1] - Q[0], Q[2] - Q[0]); n /= np.linalg.norm(n)
    vd = Q.mean(0) - eye; vd /= np.linalg.norm(vd)
    print('  quad %2d centre (%8.0f %6.0f %8.0f) Y per vertex %s  normal.up=%.3f normal.view=%.3f  size %.0f x %.0f' % (
        q, *Q.mean(0), Q[:, 1].round(0), abs(n[1]), abs(n @ vd), np.linalg.norm(Q[1] - Q[0]), np.linalg.norm(Q[2] - Q[0])))
# round trip
err = max(abs(project(P[i])[0] - v[i, 0]) + abs(project(P[i])[1] - v[i, 1]) for i in range(48))
print('round-trip screen error (GS px) %.2e' % err)

s = np.frombuffer(b297, np.float32, 8 * 16, 113984).reshape(8, 16).astype(np.float64)
print('\nstreak draw: z, w_in', s[:, 2], s[:, 3])
for name, depth in (('H: 2^24/(z-b)', lambda z: Z0 / (z - b)), ('alt: linear z/2^24*far(65536)', lambda z: (1 - z / 2 ** 24) * 65536.0),
                    ('alt: 2^24/z', lambda z: 2 ** 24 / z)):
    Q = np.array([unproj_depth(r[0], r[1], depth(r[2])) for r in s])
    L0 = np.linalg.norm(Q[2] - Q[0]); L1 = np.linalg.norm(Q[6] - Q[4])
    W0 = np.linalg.norm(Q[1] - Q[0]); W1 = np.linalg.norm(Q[3] - Q[2])
    # collinearity of the 4 edge midpoints
    m = np.array([(Q[0] + Q[1]) / 2, (Q[2] + Q[3]) / 2, (Q[4] + Q[5]) / 2, (Q[6] + Q[7]) / 2])
    u = m[-1] - m[0]; u /= np.linalg.norm(u)
    off = max(np.linalg.norm((x - m[0]) - ((x - m[0]) @ u) * u) for x in m)
    gap = np.linalg.norm(m[2] - m[1])
    print('  %-32s depths %s  segment lengths %.1f / %.1f (ratio %.3f) gap %.1f ; widths near/far %.1f/%.1f ; off-line %.2f ; dir %s' % (
        name, np.round([depth(z) for z in s[::2, 2]], 1), L0, L1, L1 / L0, gap, W0, W1, off, u.round(3)))
