// Pure numeric checks of the PROPOSED per-eye DrawCPT matrix substitution.
// Every matrix here is synthetic. Nothing is read from the game, no file, network
// or process is touched. These tests validate algebra only, not engine behaviour.
import test from 'node:test';
import assert from 'node:assert/strict';

const EPS = 1e-9;
const close = (a, b, eps = EPS, msg = '') => {
  assert.equal(a.length, b.length, msg);
  a.forEach((v, i) => assert.ok(Math.abs(v - b[i]) <= eps, `${msg} [${i}] ${v} vs ${b[i]}`));
};

// Row-vector convention: p' = p * M, 4x4 stored as 16 scalars m[4*r+c], translation in the last row.
const identity = () => [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
const mul = (a, b) => {
  const o = new Array(16).fill(0);
  for (let r = 0; r < 4; r++) for (let c = 0; c < 4; c++) for (let k = 0; k < 4; k++) o[4 * r + c] += a[4 * r + k] * b[4 * k + c];
  return o;
};
const transpose = m => m.map((_, i) => m[4 * (i % 4) + Math.floor(i / 4)]);
const apply = (p, m) => [0, 1, 2, 3].map(c => p[0] * m[c] + p[1] * m[4 + c] + p[2] * m[8 + c] + p[3] * m[12 + c]);

function inverse(m) { // Gauss-Jordan with partial pivoting
  const a = [0, 1, 2, 3].map(r => [...m.slice(4 * r, 4 * r + 4), ...[0, 1, 2, 3].map(c => (r === c ? 1 : 0))]);
  for (let i = 0; i < 4; i++) {
    let p = i;
    for (let r = i + 1; r < 4; r++) if (Math.abs(a[r][i]) > Math.abs(a[p][i])) p = r;
    assert.ok(Math.abs(a[p][i]) > 1e-12, 'singular');
    [a[i], a[p]] = [a[p], a[i]];
    const d = a[i][i];
    a[i] = a[i].map(v => v / d);
    for (let r = 0; r < 4; r++) if (r !== i) { const f = a[r][i]; a[r] = a[r].map((v, c) => v - f * a[i][c]); }
  }
  return a.flatMap(row => row.slice(4));
}

// Rotation helpers as row-vector matrices (active rotation of a row point), translation last row.
const ry = t => [Math.cos(t), 0, -Math.sin(t), 0, 0, 1, 0, 0, Math.sin(t), 0, Math.cos(t), 0, 0, 0, 0, 1];
const rx = t => [1, 0, 0, 0, 0, Math.cos(t), Math.sin(t), 0, 0, -Math.sin(t), Math.cos(t), 0, 0, 0, 0, 1];
const trans = (x, y, z) => { const m = identity(); m[12] = x; m[13] = y; m[14] = z; return m; };
const scale3 = s => { const m = identity(); m[0] = m[5] = m[10] = s; return m; };

// Canonical D3D projection (pose-contract.md, column form), transposed once for row vectors.
function projectionRow(l, r, b, t, n, f) {
  const P = [2 / (r - l), 0, (r + l) / (r - l), 0, 0, 2 / (t - b), (t + b) / (t - b), 0,
    0, 0, -f / (f - n), -f * n / (f - n), 0, 0, -1, 0];
  return transpose(P);
}

// Eye-to-world row matrix for head pose composition applied exactly ONCE:
// T_W_E = T_W_A * T_A_S * T_R_X * T_X_E in column form equals, in row form,
// T_X_E_row * T_R_X_row * T_A_S_row * T_W_A_row (the order reverses).
const eyeToWorldRow = ({aircraft, seat, recenter, xrEye}) => mul(mul(mul(xrEye, recenter), seat), aircraft);
const viewRow = eyeToWorld => inverse(eyeToWorld);

// Synthetic calibration (NOT native): N = 100 * W so C_W_N is scale 1/100, origin at W origin.
const C_W_N_row = mul(scale3(1 / 100), identity());

// PROPOSED per-eye replacement for the +64 matrix: M64' = M256 * (C * V * P) in row order.
const vpNRow = (V, P) => mul(mul(C_W_N_row, V), P);

// Synthetic per-draw model: local->N with the recorded x16 rotation scale plus a translation.
const model256 = (rot, tx, ty, tz) => mul(mul(scale3(16), rot), trans(tx, ty, tz));

// Synthetic desktop camera in N space; M64 = M256 * VP_desktop. Recovery must invert this.
const vpDesktop = mul(mul(trans(-410.8, -55.2, 410.8), ry(Math.PI / 4)), projectionRow(-0.6152, 0.6152, -0.346, 0.346, 1, 10000));

test('VP is recovered as inverse(M+256) * M+64 for every draw with its own model matrix', () => {
  for (const M256 of [model256(ry(0.3), 5, -2, 7), model256(mul(rx(-0.4), ry(1.1)), -80, 12, 3)]) {
    const M64 = mul(M256, vpDesktop);
    close(mul(inverse(M256), M64), vpDesktop, 1e-6, 'recovered VP');
  }
});

test('substituted +64 matrix equals independent column-form clip computation', () => {
  const M256 = model256(ry(0.7), 4, 1, -6);
  const eye = eyeToWorldRow({
    aircraft: mul(ry(Math.PI / 2), trans(10, 2, 3)),
    seat: trans(0, 1, -2),
    recenter: identity(),
    xrEye: trans(0.03, 0, -0.1),
  });
  const V = viewRow(eye);
  const P = projectionRow(-1, 2, -0.5, 1.5, 0.1, 100);
  const M64eye = mul(M256, vpNRow(V, P));
  const local = [0.2, -0.1, 0.5, 1];
  const viaSubstitution = apply(local, M64eye);
  const stepwise = apply(apply(apply(apply(local, M256), C_W_N_row), V), P);
  close(viaSubstitution, stepwise, 1e-9, 'row product equals stepwise');
  // Independent column-form check: transpose everything and apply right to left.
  const col = [M256, C_W_N_row, V, P].map(transpose);
  let p = local;
  for (const m of col) p = [0, 1, 2, 3].map(r => m.slice(4 * r, 4 * r + 4).reduce((s, v, c) => s + v * p[c], 0));
  close(viaSubstitution, p, 1e-9, 'column-form agreement');
});

test('eye pose is applied once: the eye position has clip x, y, w all zero', () => {
  const head = mul(ry(0.4), trans(0.1, 0.2, -0.3)); // tracked head pose (fixture)
  const eyeOffset = trans(-0.032, 0, 0);
  const xrEye = mul(eyeOffset, head); // raw OpenXR-style view pose, built once
  const eye = eyeToWorldRow({aircraft: trans(10, 2, 3), seat: trans(0, 1, -2), recenter: identity(), xrEye});
  const V = viewRow(eye);
  const P = projectionRow(-0.9, 0.7, -0.6, 0.8, 0.1, 100);
  const eyeWorld = apply([0, 0, 0, 1], eye);
  const clip = apply(apply(eyeWorld, V), P);
  close([clip[0], clip[1], clip[3]], [0, 0, 0], 1e-9, 'eye maps to clip (0,0,*,0)');
  // Applying the head a second time (the contract's forbidden double application) moves the eye.
  const doubled = eyeToWorldRow({aircraft: trans(10, 2, 3), seat: trans(0, 1, -2), recenter: identity(), xrEye: mul(xrEye, head)});
  const doubledClip = apply(apply(eyeWorld, viewRow(doubled)), P);
  assert.ok(Math.abs(doubledClip[3]) > 0.01, 'double application must be detectable');
});

test('both eyes share one model matrix and differ only by VP; parallax and asymmetric FOV hold', () => {
  const M256 = model256(identity(), 0, 0, 0); // cockpit fixed in the aircraft frame
  const mk = (dx, fov) => {
    const eye = eyeToWorldRow({aircraft: identity(), seat: identity(), recenter: identity(), xrEye: trans(0.1 + dx, 0.2, -0.3)});
    return mul(M256, vpNRow(viewRow(eye), projectionRow(...fov, 1, 10)));
  };
  const left = mk(-0.032, [-1, 0.8, -0.7, 0.9]);
  const right = mk(0.032, [-0.8, 1, -0.7, 0.9]);
  // Local point that M256 (x16 scale) and C (1/100) bring to 1 m ahead of the aircraft origin.
  const local = [0, 0, -100 / 16, 1];
  const ndcX = (m, p) => { const c = apply(p, m); return c[0] / c[3]; };
  assert.notEqual(ndcX(left, local).toFixed(6), ndcX(right, local).toFixed(6), 'parallax gives distinct NDC x');
  // Frustum edges at depth d map to +/-1 for the left eye's own asymmetric FOV.
  const d = 2;
  const eyeL = eyeToWorldRow({aircraft: identity(), seat: identity(), recenter: identity(), xrEye: trans(0.068, 0.2, -0.3)});
  const PL = projectionRow(-1, 0.8, -0.7, 0.9, 1, 10);
  for (const [x, y, nx, ny] of [[-1, 0, -1, null], [0.8, 0, 1, null], [0, -0.7, null, -1], [0, 0.9, null, 1]]) {
    const worldPoint = apply([x * d, y * d, -d, 1], eyeL);
    const c = apply(apply(worldPoint, viewRow(eyeL)), PL);
    if (nx !== null) assert.ok(Math.abs(c[0] / c[3] - nx) < 1e-9);
    if (ny !== null) assert.ok(Math.abs(c[1] / c[3] - ny) < 1e-9);
  }
});

test('re-multiplying the recovered desktop VP by a headset projection keeps the desktop view', () => {
  const M256 = model256(ry(0.2), 3, 0, -4);
  const M64 = mul(M256, vpDesktop);
  const recovered = mul(inverse(M256), M64);
  const headset = projectionRow(-0.9, 0.9, -0.8, 0.8, 0.1, 100);
  const wrong = mul(M256, mul(recovered, headset));
  const eye = eyeToWorldRow({aircraft: identity(), seat: identity(), recenter: identity(), xrEye: trans(0.5, 0.1, 0.2)});
  const right = mul(M256, vpNRow(viewRow(eye), headset));
  const local = [0.3, 0.2, -0.5, 1];
  const a = apply(local, wrong);
  const b = apply(local, right);
  assert.ok(Math.abs(a[0] / a[3] - b[0] / b[3]) > 1e-3, 'must not be interchangeable');
});
