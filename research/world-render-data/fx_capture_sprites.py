"""Offline unprojection check on the stored frame 11906 (read-only): the two ps2_v + ps2AtestCA_p (4caf6e2d) draws.
Hypothesis H: a pre-projected ps2_v vertex (x_s, y_s, z_s, w=1) of a world point P satisfies, with the frame's world->PS2-screen matrix A
(DrawACM_v record +0, row vectors): x_s = (P.A0)/(P.A3), y_s = (P.A1)/(P.A3), z_s = (P.A2)/(P.A3) = 2^24/w - 255.0078 (A col2 = Z0 e3 + b A3).
So w = 2^24/(z_s - b) and P solves P.A0 = x_s w, P.A1 = y_s w, P.A3 = w (3 equations, P.xyz unknown)."""
import os, sys, struct, json
import numpy as np

CAP = r'%USERPROFILE%\ACZero-captures\E10\blobs'
b297 = open(os.path.join(CAP, '000334'), 'rb').read()
b329 = open(os.path.join(CAP, '000335'), 'rb').read()
draws = json.load(open(sys.argv[1]))

rec = np.frombuffer(b329, np.float32, 104, 967184).astype(np.float64)
A = rec[0:16].reshape(4, 4)        # row-vector matrix: h = [P,1] @ A
B = rec[64:80].reshape(4, 4)
print('A=\n', A)
Z0 = A[3, 2] - (A[3, 2] - A[3, 3] * (A[0, 2] / A[0, 3]))  # placeholder, recomputed below
b = np.mean(A[0:3, 2] / A[0:3, 3])
Z0 = A[3, 2] - b * A[3, 3]
print('depth model: b=%.4f (col2/col3 rows 0-2: %s)  Z0=%.1f (2^24=%d)' % (b, A[0:3, 2] / A[0:3, 3], Z0, 1 << 24))

# camera eye: the point with P.A0 = P.A1 = P.A3 = 0 ... use the w=0 plane + x,y screen centre rays: eye solves [e,1]@A[:, (0,1,3)] = 0
M = A[0:3][:, [0, 1, 3]]; t = A[3, [0, 1, 3]]
eye = np.linalg.solve(M.T, -t)
print('camera eye (from A) =', eye)


def unproject(xs, ys, zs):
    w = Z0 / (zs - b)
    rhs = np.array([xs * w, ys * w, w]) - t
    return np.linalg.solve(M.T, rhs), w


for d in draws:
    s = d.get('shaders', {})
    if (s.get('PS') or '')[:8] != '4caf6e2d':
        continue
    vb = d['vb']; ib = d['ib']
    nv = vb[2] // vb[3]
    v = np.frombuffer(b297, np.float32, nv * 16, vb[1]).reshape(nv, 16)
    idx = np.frombuffer(b297, np.uint16, ib[2] // 2, ib[1])
    used = sorted(set(int(i) for i in idx if i != 0xFFFF))
    print('\n== draw', d['ordinal'], 'depth', s.get('depth'), 'indices', len(idx), 'verts used', len(used), 'of', nv, 'rc', {k: [round(x, 3) for x in vv] for k, vv in d['rc'].items()})
    P = []
    for i in used:
        x, y, z, w = v[i, 0:4]
        p, wv = unproject(x, y, z)
        P.append(p)
        if len(P) <= 12 or i == used[-1]:
            print('v%-3d xs=%8.2f ys=%8.2f zs=%10.2f w_in=%.3f | uv=%s col1=%s | view-depth=%9.1f  P=(%9.1f, %8.1f, %9.1f)  dist-from-eye=%9.1f' % (
                i, x, y, z, w, np.round(v[i, 8:10], 3), np.round(v[i, 4:8], 3), wv, p[0], p[1], p[2], np.linalg.norm(p - eye)))
    P = np.array(P)
    print('world Y range %.1f .. %.1f  (mean %.1f); eye Y %.1f' % (P[:, 1].min(), P[:, 1].max(), P[:, 1].mean(), eye[1]))
    # strips: quads of 4 consecutive used vertices between restarts
    strips, cur = [], []
    for i in idx:
        if i == 0xFFFF:
            if cur: strips.append(cur); cur = []
        else:
            cur.append(int(i))
    if cur: strips.append(cur)
    for k, st in enumerate(strips[:20]):
        q = np.array([unproject(*v[i, 0:3])[0] for i in st])
        c = q.mean(axis=0)
        e1 = np.linalg.norm(q[1] - q[0]); e2 = np.linalg.norm(q[2] - q[0]) if len(q) > 2 else 0
        # quad plane normal vs view direction (billboard facing check)
        n = np.cross(q[1] - q[0], q[2] - q[0]) if len(q) > 2 else np.zeros(3)
        vd = (c - eye) / np.linalg.norm(c - eye)
        nn = n / (np.linalg.norm(n) + 1e-12)
        zs = [round(float(v[i, 2]), 2) for i in st]
        print('  quad %2d verts %s centre (%9.1f,%8.1f,%9.1f) edges %.0f/%.0f |cos(normal,view)|=%.3f zs=%s' % (k, st, c[0], c[1], c[2], e1, e2, abs(nn @ vd), zs))
