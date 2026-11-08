// Camera VP recovery / decomposition / per-eye substitution. Row-vector matrices.
import { multiply, inverse } from './mat4.js';
import { ps2ViewportMatrix } from './ps2.js';

// VP = inverse(local_to_world) * local_to_clip. Throws if local_to_world is singular
// (e.g. the E2 eid 1534 data, which is not an invertible transform).
export function recoverCameraVP(localToWorld, localToClip) {
  return multiply(inverse(localToWorld), localToClip);
}

function col(m, c) {
  return [m[c], m[4 + c], m[8 + c], m[12 + c]];
}

function norm3(v) {
  return Math.hypot(v[0], v[1], v[2]);
}

// Decompose a world_to_clip of the measured form (symmetric lens, no off-centre terms):
//   col0 = right / tanX, col1 = (flipY ? -1 : 1) * up / tanY, col3 = forward (w = forward distance),
//   translation row = -(eye . columns).
// Returns eye, unit right/up/forward axes, tan(half FOV), the depth relation of column 2 against
// column 3 (z_c = A*w + C, from rows 0 and 3), and handedness = sign det[right, up, forward].
// Off-centre (asymmetric) lenses are NOT decomposed; offCentre reports the residual so callers can tell.
export function decomposeCameraVP(vp, { flipY = true } = {}) {
  const c0 = col(vp, 0), c1 = col(vp, 1), c2 = col(vp, 2), c3 = col(vp, 3);
  const n0 = norm3(c0), n1 = norm3(c1), n3 = norm3(c3);
  if (!(n0 > 0) || !(n1 > 0) || !(n3 > 0)) throw new RangeError('degenerate camera VP');
  const right = [c0[0] / n0, c0[1] / n0, c0[2] / n0];
  const sy = flipY ? -1 : 1;
  const up = [sy * c1[0] / n1, sy * c1[1] / n1, sy * c1[2] / n1];
  const forward = [c3[0] / n3, c3[1] / n3, c3[2] / n3];
  // eye * C = -t for the x, y, w columns (3x3 solve by Cramer's rule on the transposed system).
  const C = [
    [c0[0], c1[0], c3[0]],
    [c0[1], c1[1], c3[1]],
    [c0[2], c1[2], c3[2]],
  ];
  const t = [c0[3], c1[3], c3[3]];
  const eye = solveRowVec3(C, [-t[0], -t[1], -t[2]]);
  const det = right[0] * (up[1] * forward[2] - up[2] * forward[1])
    - right[1] * (up[0] * forward[2] - up[2] * forward[0])
    + right[2] * (up[0] * forward[1] - up[1] * forward[0]);
  const A = dot3(c2, c3) / (n3 * n3); // z_c xyz part = A * forward (1st-3rd rows only)
  const offCentre = Math.max(
    Math.abs(dot3(right, forward)), Math.abs(dot3(up, forward)), Math.abs(dot3(right, up)),
  );
  return {
    eye,
    right, up, forward,
    tanHalfFovX: 1 / n0,
    tanHalfFovY: 1 / n1,
    forwardNorm: n3,
    depthSlopeA: A, // z_c = A * w + depthOffsetC
    depthOffsetC: c2[3] - A * c3[3],
    handedness: Math.sign(det), // +1: right-handed (r x u = f); -1: left-handed
    axesOrthogonalityResidual: offCentre,
  };
}

function dot3(a, b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Solve e * C = b for row vector e (3x3 C): e = b * C^-1. Cofactor inverse.
function solveRowVec3(C, b) {
  const [[a, bb, c], [d, e, f], [g, h, i]] = C;
  const A = e * i - f * h, B = -(d * i - f * g), Cc = d * h - e * g;
  const det = a * A + bb * B + c * Cc;
  if (!(Math.abs(det) > 0)) throw new RangeError('camera VP columns are linearly dependent');
  const inv = [
    [A, -(bb * i - c * h), bb * f - c * e],
    [B, a * i - c * g, -(a * f - c * d)],
    [Cc, -(a * h - bb * g), a * e - bb * d],
  ].map((r) => r.map((v) => v / det));
  return [0, 1, 2].map((j) => b[0] * inv[0][j] + b[1] * inv[1][j] + b[2] * inv[2][j]);
}

// Per-eye substitution: given a draw's local_to_world and an eye world_to_clip (VP),
// return local_to_clip and local_to_ps2_screen. Viewport options as in ps2ViewportMatrix.
export function substituteEye(localToWorld, eyeVP, viewportOpts = {}) {
  const localToClip = multiply(localToWorld, eyeVP);
  const localToPs2Screen = multiply(localToClip, ps2ViewportMatrix(viewportOpts));
  return { localToClip, localToPs2Screen };
}
