// Asymmetric clip projection in the *measured* world_to_clip form (E4 camera block):
//   w   = forward distance (column 3 is the depth axis)
//   z_c = A * w - B           (z ~ w minus a constant)
// Input view space is the OpenXR/canonical one: +X right, +Y up, -Z forward, so w = -z_view.
// Row-vector matrix, translation in the last row.

// Resolve z_c = A*w - B from explicit near/far and NDC depth range [zNdcNear, zNdcFar]:
//   ndc(w) = A - B / w;  ndc(near) = zNdcNear;  ndc(far) = zNdcFar.
// far may be Infinity (reverse-infinite style: A = zNdcFar, B = (zNdcFar - zNdcNear) * near).
export function depthTerms({ near, far, depthRange = [0, 1] }) {
  const [zn, zf] = depthRange;
  if (!(near > 0) || !Number.isFinite(near)) throw new RangeError('near must be a finite number > 0');
  if (!(far > near)) throw new RangeError('far must be > near');
  if (![zn, zf].every(Number.isFinite) || zn === zf) throw new RangeError('depthRange must be two distinct finite numbers');
  if (far === Infinity) return { A: zf, B: (zf - zn) * near };
  const B = ((zf - zn) * far * near) / (far - near);
  const A = zf + B / far;
  return { A, B };
}

// fov = { angleLeft, angleRight, angleUp, angleDown } radians (OpenXR: left/down negative).
// opts: { near, far, depthRange = [0,1], flipY = true }
// flipY = true matches the measured form (y_c = -u / tan(half FOV y)); set false for +Y-up clip.
export function projectionFromFov(fov, opts) {
  const { angleLeft, angleRight, angleUp, angleDown } = fov;
  if (![angleLeft, angleRight, angleUp, angleDown].every(Number.isFinite)) throw new TypeError('fov angles must be finite');
  if (!(angleRight > angleLeft) || !(angleUp > angleDown)) throw new RangeError('fov must satisfy right > left and up > down');
  if (Math.max(Math.abs(angleLeft), Math.abs(angleRight), Math.abs(angleUp), Math.abs(angleDown)) >= Math.PI / 2) {
    throw new RangeError('fov angles must be within (-pi/2, pi/2)');
  }
  const { flipY = true } = opts;
  const { A, B } = depthTerms(opts);
  const tl = Math.tan(angleLeft), tr = Math.tan(angleRight);
  const tu = Math.tan(angleUp), td = Math.tan(angleDown);
  const sy = flipY ? -1 : 1;
  const m = new Float64Array(16);
  // x_c = (2 x + (tr+tl) z) / (tr-tl),  w = -z
  m[0] = 2 / (tr - tl);
  m[8] = (tr + tl) / (tr - tl);
  m[5] = (sy * 2) / (tu - td);
  m[9] = (sy * (tu + td)) / (tu - td);
  // z_c = A*w - B = -A*z - B
  m[10] = -A;
  m[14] = -B;
  m[11] = -1;
  return m;
}
