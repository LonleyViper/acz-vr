// Offline synthetic/fixture checks only. Passing these does NOT establish runtime parity.
import test from 'node:test';
import assert from 'node:assert/strict';
import fixture from '../../research/experiments/fixtures/cockpit-math-fixture.json' with { type: 'json' };
import {
  fromRows, toRows, identity, multiply, inverse, transpose, transformPoint, transformVec4, quatToMat4,
  poseToWorld, viewMatrixFromPose, projectionFromFov, depthTerms,
  ps2ViewportMatrix, PS2_DEPTH_HYPOTHESIS,
  recoverCameraVP, decomposeCameraVP, substituteEye,
} from '../../src/cockpit-math/index.js';

// Magnitude-aware float32-style tolerance: |a-b| <= rel * (sum of |terms|) + abs.
function near(actual, expected, mag, { rel = 1e-5, abs = 1e-3 } = {}, label = '') {
  const tol = rel * Math.max(mag, Math.abs(expected)) + abs;
  assert.ok(Math.abs(actual - expected) <= tol, `${label} actual=${actual} expected=${expected} tol=${tol}`);
}
function approx(a, b, eps = 1e-12, label = '') {
  assert.ok(Math.abs(a - b) <= eps, `${label} ${a} vs ${b}`);
}
function matApprox(m, rows, eps = 1e-12, label = '') {
  for (let r = 0; r < 4; r++) for (let c = 0; c < 4; c++) approx(m[r * 4 + c], rows[r][c], eps, `${label}[${r}][${c}]`);
}
// x (0), y (1) and w (3) output columns of a product a*b; z column (2) is unresolved and skipped.
function compareXYW(actual, expected, magOf, label) {
  for (let r = 0; r < 4; r++) {
    for (const c of [0, 1, 3]) near(actual[r * 4 + c], expected[r][c], magOf(r, c), {}, `${label}[${r}][${c}]`);
  }
}

const STATES = Object.keys(fixture.camera_block);
const viewport = ps2ViewportMatrix();

test('fixture has the four states', () => {
  assert.deepEqual(STATES, ['A1', 'B1', 'A2', 'B2']);
});

// ---- (6) helpers ----
test('mat4 helpers: multiply order, inverse, transpose, transform', () => {
  const t = fromRows([[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [5, 6, 7, 1]]);
  assert.deepEqual(transformPoint(t, [1, 2, 3]), [6, 8, 10, 1]);
  const s = fromRows([[2, 0, 0, 0], [0, 2, 0, 0], [0, 0, 2, 0], [0, 0, 0, 1]]);
  // multiply(a,b) applies a first: translate then scale.
  assert.deepEqual(transformPoint(multiply(t, s), [0, 0, 0]), [10, 12, 14, 1]);
  assert.deepEqual(transformPoint(multiply(s, t), [0, 0, 0]), [5, 6, 7, 1]);
  matApprox(multiply(multiply(t, s), inverse(multiply(t, s))), toRows(identity()));
  assert.deepEqual(toRows(transpose(t))[0], [1, 0, 0, 5]);
  assert.deepEqual(transformVec4(identity(), [1, 2, 3, 4]), [1, 2, 3, 4]);
  assert.throws(() => inverse(new Float64Array(16)), RangeError);
  assert.throws(() => inverse(fromRows([[1, 2, 3, 4], [2, 4, 6, 8], [0, 0, 1, 0], [0, 0, 0, 1]])), RangeError);
});

test('quaternion -> row-vector matrix: yaw +90 about +Y, scaled and negated inputs', () => {
  const s = Math.sin(Math.PI / 4), c = Math.cos(Math.PI / 4);
  for (const k of [1, 3, -1]) {
    const m = quatToMat4([0, k * s, 0, k * c]);
    const fwd = transformVec4(m, [0, 0, -1, 0]);
    approx(fwd[0], -1, 1e-12); approx(fwd[1], 0, 1e-12); approx(fwd[2], 0, 1e-12);
    const right = transformVec4(m, [1, 0, 0, 0]);
    approx(right[0], 0, 1e-12); approx(right[2], -1, 1e-12);
  }
  matApprox(quatToMat4([0, 0, 0, 1]), toRows(identity()));
  assert.throws(() => quatToMat4([0, 0, 0, 0]), RangeError);
  assert.throws(() => quatToMat4([NaN, 0, 0, 1]), TypeError);
});

// ---- (1) view matrix from pose ----
test('view matrix from pose: inverse of pose-to-world, analytic points', () => {
  const s = Math.sin(Math.PI / 4), c = Math.cos(Math.PI / 4);
  const pose = { position: [1, 2, 3], orientation: [0, s, 0, c] };
  const view = viewMatrixFromPose(pose);
  matApprox(multiply(poseToWorld(pose), view), toRows(identity()), 1e-12);
  matApprox(view, toRows(inverse(poseToWorld(pose))), 1e-12);
  // The eye's own position maps to the origin.
  const o = transformPoint(view, [1, 2, 3]);
  approx(o[0], 0); approx(o[1], 0); approx(o[2], 0);
  // A point 5 units ahead of the eye (yaw 90 deg: forward is world -X) lands at view -Z.
  const p = transformPoint(view, [1 - 5, 2, 3]);
  approx(p[0], 0); approx(p[1], 0); approx(p[2], -5);
  // Identity orientation: pure translation.
  const v2 = viewMatrixFromPose({ position: [10, 0, -4], orientation: [0, 0, 0, 1] });
  assert.deepEqual(transformPoint(v2, [10, 0, -4]).slice(0, 3).map((x) => x + 0), [0, 0, 0]);
});

// ---- (2) projection ----
test('projection: symmetric 90 degree FOV analytic values', () => {
  const a = Math.PI / 4;
  const fov = { angleLeft: -a, angleRight: a, angleUp: a, angleDown: -a };
  const P = projectionFromFov(fov, { near: 1, far: 101, depthRange: [0, 1], flipY: false });
  // Point at x=1,y=0.5 at depth 2 (view z=-2): w=2, x_ndc=0.5, y_ndc=0.25
  const clip = transformPoint(P, [1, 0.5, -2]);
  approx(clip[3], 2, 1e-12);
  approx(clip[0] / clip[3], 0.5, 1e-12);
  approx(clip[1] / clip[3], 0.25, 1e-12);
  // depth NDC 0 at near, 1 at far; z_c = A*w - B
  const { A, B } = depthTerms({ near: 1, far: 101 });
  approx(A, 101 / 100, 1e-12); approx(B, 101 / 100, 1e-12);
  const zn = transformPoint(P, [0, 0, -1]), zf = transformPoint(P, [0, 0, -101]);
  approx(zn[2] / zn[3], 0, 1e-12); approx(zf[2] / zf[3], 1, 1e-12);
  // flipY mirrors only y
  const Pf = projectionFromFov(fov, { near: 1, far: 101 });
  approx(transformPoint(Pf, [1, 0.5, -2])[1], -clip[1], 1e-12);
  approx(transformPoint(Pf, [1, 0.5, -2])[0], clip[0], 1e-12);
});

test('projection: asymmetric OpenXR FOV edges map to +-1, off-centre terms', () => {
  const fov = { angleLeft: -0.5, angleRight: 0.8, angleUp: 0.6, angleDown: -0.4 };
  const tl = Math.tan(-0.5), tr = Math.tan(0.8), tu = Math.tan(0.6), td = Math.tan(-0.4);
  for (const flipY of [false, true]) {
    const P = projectionFromFov(fov, { near: 0.1, far: 50, flipY });
    const d = 7;
    const ndc = (x, y) => { const c = transformPoint(P, [x, y, -d]); return [c[0] / c[3], c[1] / c[3]]; };
    const sy = flipY ? -1 : 1;
    approx(ndc(tl * d, 0)[0], -1, 1e-12); approx(ndc(tr * d, 0)[0], 1, 1e-12);
    approx(ndc(0, tu * d)[1], sy, 1e-12); approx(ndc(0, td * d)[1], -sy, 1e-12);
    // Optical axis is off-centre: x_ndc = -(tr+tl)/(tr-tl)
    approx(ndc(0, 0)[0], -(tr + tl) / (tr - tl), 1e-12);
    approx(ndc(0, 0)[1], -sy * (tu + td) / (tu - td), 1e-12);
  }
  assert.throws(() => projectionFromFov({ angleLeft: 0.5, angleRight: -0.5, angleUp: 1, angleDown: -1 }, { near: 1, far: 2 }), RangeError);
  assert.throws(() => projectionFromFov(fov, { near: 0, far: 2 }), RangeError);
  assert.throws(() => projectionFromFov(fov, { near: 2, far: 1 }), RangeError);
});

test('projection: depth range [-1,1] and infinite far', () => {
  const { A, B } = depthTerms({ near: 2, far: 10, depthRange: [-1, 1] });
  approx(A - B / 2, -1, 1e-12); approx(A - B / 10, 1, 1e-12);
  const inf = depthTerms({ near: 2, far: Infinity });
  assert.equal(inf.A, 1); assert.equal(inf.B, 2);
});

test('projection: near=2 / far=65536 reproduces the measured z_c = A*w - 2 form (consistent-with, not proof)', () => {
  const { A, B } = depthTerms({ near: 2, far: 65536 });
  near(A, 65536 / 65534, 1, { rel: 1e-12, abs: 1e-12 });
  approx(B, (65536 * 2) / 65534, 1e-12); // ~2.00006; fixture offset is -2 within float32 noise
  for (const s of STATES) {
    const f = decomposeCameraVP(fromRows(fixture.camera_block[s].world_to_clip));
    near(f.depthSlopeA, A, 1, { rel: 0, abs: 5e-7 }, `${s} A`);
    near(f.depthOffsetC, -2, 1, { rel: 0, abs: 5e-3 }, `${s} offset`);
  }
});

// ---- (3) viewport mapping ----
test('PS2 viewport matrix entries and analytic mapping', () => {
  const v = ps2ViewportMatrix();
  const out = transformVec4(v, [0.5, -0.25, 3, 2]);
  approx(out[0], 258.048 * 0.5 + 2048 * 2, 1e-9);
  approx(out[1], 225.792 * -0.25 + 2048 * 2, 1e-9);
  assert.equal(out[3], 2);
  assert.equal(out[2], 0, 'depth is not fitted by default');
  const d = ps2ViewportMatrix({ depth: { fromClipZ: 2, fromW: 3 } });
  assert.equal(transformVec4(d, [0, 0, 5, 7])[2], 2 * 5 + 3 * 7);
});

test('inverse(world_to_clip) * world_to_ps2_screen equals the viewport mapping (x/y/w) for A1,B1,A2,B2', () => {
  for (const s of STATES) {
    const w2c = fromRows(fixture.camera_block[s].world_to_clip);
    const got = multiply(inverse(w2c), fromRows(fixture.camera_block[s].world_to_ps2_screen));
    // z_c = A*w - 2 with A = 1 + 3e-5, so z_c and w are nearly parallel and the float32 inverse
    // amplifies noise by ~1/3e-5 on the z- and w-input rows (rows 2, 3). Those rows get a looser bound (0.2 on
    // values up to 2048, i.e. ~1e-4 relative); x/y input rows are within 2e-2.
    for (const c of [0, 1, 3]) {
      for (let r = 0; r < 4; r++) near(got[r * 4 + c], viewport[r * 4 + c], 1, { rel: 0, abs: r >= 2 ? 0.2 : 2e-2 }, `${s}[${r}][${c}]`);
    }
  }
});

test('measured world_to_ps2_screen x/y/w columns equal world_to_clip * viewport per state', () => {
  for (const s of STATES) {
    const w2c = fromRows(fixture.camera_block[s].world_to_clip);
    const got = multiply(w2c, viewport);
    const exp = fixture.camera_block[s].world_to_ps2_screen;
    compareXYW(got, exp, (r, c) => {
      let m = 0;
      for (let k = 0; k < 4; k++) m += Math.abs(w2c[r * 4 + k] * viewport[k * 4 + c]);
      return m;
    }, s);
  }
});

// ---- (4) VP decomposition ----
test('E3 lens and eye position recovered from measured world_to_clip', () => {
  for (const s of STATES) {
    const m = fromRows(fixture.camera_block[s].world_to_clip);
    const d = decomposeCameraVP(m);
    near(d.tanHalfFovX, fixture.lens.tan_half_fov_x, 1, { rel: 0, abs: 5e-4 }, `${s} tanX`);
    near(d.tanHalfFovY, fixture.lens.tan_half_fov_y, 1, { rel: 0, abs: 5e-4 }, `${s} tanY`);
    near(d.forwardNorm, 1, 1, { rel: 0, abs: 1e-4 }, `${s} fwd norm`);
    assert.ok(d.axesOrthogonalityResidual < 1e-4, `${s} axes orthogonal ${d.axesOrthogonalityResidual}`);
    // The eye is the point whose clip x, y and w all vanish.
    const c = transformPoint(m, d.eye);
    for (const i of [0, 1, 3]) near(c[i], 0, 1, { rel: 0, abs: 1e-6 }, `${s} eye clip[${i}]`);
    // Axes are consistent with the matrix: moving one unit along forward adds 1 to w.
    const ahead = transformPoint(m, d.eye.map((v, i) => v + d.forward[i]));
    near(ahead[3], 1, 1, { rel: 0, abs: 1e-6 }, `${s} unit ahead`);
    // Basis handedness is reported, not assumed.
    assert.ok(d.handedness === 1 || d.handedness === -1);
    assert.equal(d.handedness, decomposeCameraVP(fromRows(fixture.camera_block.A1.world_to_clip)).handedness);
  }
  // Eye position is shared by A1/A2 and by B1/B2 (same ps2 translation rows) only if the
  // fixture says so; report-only here: print is avoided, so just require finite values.
  for (const s of STATES) assert.ok(decomposeCameraVP(fromRows(fixture.camera_block[s].world_to_clip)).eye.every(Number.isFinite));
});

test('decomposition round-trips a synthetic pose + FOV through the measured form', () => {
  // Build world_to_clip in the measured axis convention from a known eye, then decompose it.
  const eye = [100, 20, -30];
  const yaw = 0.7;
  const fwd = [Math.sin(yaw), 0, Math.cos(yaw)];
  const right = [Math.cos(yaw), 0, -Math.sin(yaw)];
  const up = [0, 1, 0];
  const tx = 0.6152, ty = 0.346;
  const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const cols = [right.map((v) => v / tx), up.map((v) => -v / ty), [0, 0, 0], fwd];
  const rows = [0, 1, 2].map((r) => [cols[0][r], cols[1][r], cols[2][r], cols[3][r]]);
  rows.push([-dot(eye, cols[0]), -dot(eye, cols[1]), 0, -dot(eye, cols[3])]);
  const d = decomposeCameraVP(fromRows(rows));
  for (let i = 0; i < 3; i++) {
    approx(d.eye[i], eye[i], 1e-9); approx(d.right[i], right[i], 1e-12);
    approx(d.up[i], up[i], 1e-12); approx(d.forward[i], fwd[i], 1e-12);
  }
  approx(d.tanHalfFovX, tx, 1e-12); approx(d.tanHalfFovY, ty, 1e-12);
  assert.equal(d.handedness, 1, 'right x up = +forward for this basis');
  // Handedness depends on the flipY assumption (the sign of the measured y column).
  assert.equal(decomposeCameraVP(fromRows(rows), { flipY: false }).handedness, -1);
});

// ---- (4,5) per-draw recovery and per-eye substitution ----
const USABLE = fixture.drawcpt_draws.filter((d) => d.eid !== 1534);

test('eid 1534 local_to_world is singular (not a transform) and is excluded', () => {
  const d = fixture.drawcpt_draws.find((x) => x.eid === 1534);
  assert.throws(() => recoverCameraVP(fromRows(d.local_to_world), fromRows(d.local_to_clip)), RangeError);
});

test('recombining each draw local_to_world with the recovered VP reproduces local_to_clip and local_to_ps2_screen (x/y/w)', () => {
  const ref = USABLE.find((d) => d.eid === 2045);
  const vp = recoverCameraVP(fromRows(ref.local_to_world), fromRows(ref.local_to_clip));
  for (const d of USABLE) {
    const l2w = fromRows(d.local_to_world);
    const { localToClip, localToPs2Screen } = substituteEye(l2w, vp);
    const mag = (a, b, ar, ac, bc) => { let m = 0; for (let k = 0; k < 4; k++) m += Math.abs(a[ar * 4 + k] * b[k * 4 + bc]); return m; };
    for (let r = 0; r < 4; r++) {
      for (const c of [0, 1, 3]) {
        near(localToClip[r * 4 + c], d.local_to_clip[r][c], mag(l2w, vp, r, 0, c), { rel: 2e-5, abs: 1e-3 }, `eid ${d.eid} clip[${r}][${c}]`);
        near(localToPs2Screen[r * 4 + c], d.local_to_ps2_screen[r][c], mag(localToClip, viewport, r, 0, c), { rel: 2e-5, abs: 1e-2 }, `eid ${d.eid} ps2[${r}][${c}]`);
      }
    }
  }
});

test('per-eye substitution: a shifted eye VP changes the draw output consistently', () => {
  const d = USABLE.find((x) => x.eid === 2045);
  const l2w = fromRows(d.local_to_world);
  const vp = recoverCameraVP(l2w, fromRows(d.local_to_clip));
  const dec = decomposeCameraVP(vp);
  // Eye moved 0.032 along the camera's right axis (camera at the same orientation).
  const shift = dec.right.map((v) => v * 0.032);
  const T = fromRows([[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [-shift[0], -shift[1], -shift[2], 1]]);
  const vpR = multiply(T, vp);
  const base = substituteEye(l2w, vp);
  const eyeR = substituteEye(l2w, vpR);
  // Same depth (w), different x: x_c differs by shift . right / tanX * (nothing in w).
  for (let r = 0; r < 4; r++) near(eyeR.localToClip[r * 4 + 3], base.localToClip[r * 4 + 3], 1e3, {}, 'w');
  const w = transformPoint(multiply(l2w, vp), [0, 0, 0]);
  const wR = transformPoint(multiply(l2w, vpR), [0, 0, 0]);
  near(wR[3], w[3], 1, { rel: 1e-9, abs: 1e-6 }, 'origin w unchanged');
  near(wR[0] - w[0], -0.032 / dec.tanHalfFovX, 1, { rel: 0, abs: 1e-6 }, 'origin x shift');
  // ps2 screen x moves by 258.048 * dx_c
  const sx = transformPoint(eyeR.localToPs2Screen, [0, 0, 0]);
  const s0 = transformPoint(base.localToPs2Screen, [0, 0, 0]);
  near(sx[0] - s0[0], 258.048 * (wR[0] - w[0]), 1, { rel: 1e-6, abs: 1e-3 }, 'ps2 x shift');
});

// ---- unresolved depth: document, do not fit ----
test('UNRESOLVED depth: z_s hypothesis 2^23*(w - z_c) is NOT validated; observed residual is recorded, not asserted away', () => {
  const h = PS2_DEPTH_HYPOTHESIS;
  assert.equal(h.fromClipZ, -(2 ** 23));
  assert.equal(h.fromW, 2 ** 23);
  const resid = [];
  for (const s of STATES) {
    const w2c = fromRows(fixture.camera_block[s].world_to_clip);
    const pred = multiply(w2c, ps2ViewportMatrix({ depth: h }));
    const meas = fixture.camera_block[s].world_to_ps2_screen;
    let worst = 0;
    for (let r = 0; r < 4; r++) worst = Math.max(worst, Math.abs(pred[r * 4 + 2] - meas[r][2]));
    resid.push(worst);
  }
  // Float32 cancellation in (w - z_c) makes some rows disagree by far more than the x/y/w terms do.
  // We only assert that the residual is NOT negligible, so nobody mistakes the hypothesis for a fit.
  assert.ok(Math.max(...resid) > 1, `max depth residuals ${resid.join(', ')}`);
});
