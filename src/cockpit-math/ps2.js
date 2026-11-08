// PS2 clip -> screen mapping (measured, E4/E2):
//   x_s = 258.048 * x_c + 2048 * w
//   y_s = 225.792 * y_c + 2048 * w
//   w_s = w
//   z_s = depth.fromClipZ * z_c + depth.fromW * w     (UNRESOLVED - see UNRESOLVED.md)
// Row-vector matrix: [x_c, y_c, z_c, w] * M = [x_s, y_s, z_s, w].

export const PS2_CENTRE = 2048;
export const PS2_SCALE_X = 258.048;
export const PS2_SCALE_Y = 225.792;

// NOT used by default and NOT validated: a two-term hypothesis from the fixture
// (z_s ~ 2^23 * (w - z_c)). Observed residuals are far above float32 noise; do not rely on it.
export const PS2_DEPTH_HYPOTHESIS = Object.freeze({ fromClipZ: -(2 ** 23), fromW: 2 ** 23 });

// depth defaults to a zero z column on purpose: the z_s output is then visibly meaningless
// rather than silently fitted. Pass depth explicitly to experiment.
export function ps2ViewportMatrix({
  scaleX = PS2_SCALE_X,
  scaleY = PS2_SCALE_Y,
  centre = PS2_CENTRE,
  depth = { fromClipZ: 0, fromW: 0 },
} = {}) {
  const m = new Float64Array(16);
  m[0] = scaleX;
  m[3 * 4 + 0] = centre;
  m[5] = scaleY;
  m[3 * 4 + 1] = centre;
  m[10] = depth.fromClipZ;
  m[3 * 4 + 2] = depth.fromW;
  m[15] = 1;
  return m;
}
