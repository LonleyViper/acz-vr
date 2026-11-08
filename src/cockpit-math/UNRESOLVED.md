# cockpit-math: unresolved quantities

Offline maths only. Nothing here establishes runtime parity with the game.
Source data: `research/experiments/fixtures/cockpit-math-fixture.json` (E2 capture 11906, E3 lens, E4 camera block).

## 1. PS2 screen depth term (z_s) — UNRESOLVED, not fitted

x_s, y_s and w are reproduced (`x_s = 258.048*x_c + 2048*w`, `y_s = 225.792*y_c + 2048*w`, `w_s = w`).
The z column of `world_to_ps2_screen` / `local_to_ps2_screen` is **not** reproduced. `ps2ViewportMatrix()` therefore
defaults to a zero z column (`depth = {fromClipZ: 0, fromW: 0}`); callers must pass a depth term deliberately.

Observation only: a two-term least-squares through rows 0 and 3 gives z_s ~ kz*z_c + kw*w with kz ~ -8.39e6 and
kw ~ +8.39e6 (about -/+ 2^23). It is exported as `PS2_DEPTH_HYPOTHESIS = {fromClipZ: -2^23, fromW: 2^23}`
(z_s ~ 2^23 * (w - z_c)) purely so the residual can be inspected. It does **not** match:

Residual (hypothesis minus measured z_s column, rows 0..3 of the matrix):

| Source | row 0 | row 1 | row 2 | row 3 (measured ~1.7e7) |
|---|---|---|---|---|
| camera A1 | -0.68 | 0.00 | 169.7 | 630 |
| camera B1 | 0.97 | 0.02 | -44.7 | -331 |
| camera A2 | -0.59 | 0.00 | 187.6 | 630 |
| camera B2 | 1.19 | 0.01 | -65.4 | -587 |
| draw 2025/2035 | 1913.9 | -50.7 | 1834.3 | 342 |
| draw 2045/2055 | -316.1 | -87.7 | -2643.2 | -28016 |
| draw 2065 | 2630.8 | 263.4 | 214.5 | -2058 |

Why it is unresolved: z_c and w differ only by `(A-1)*w - 2` with `A-1 ~ 3e-5`, so `w - z_c` suffers float32
cancellation; the measured z_s may also include depth scaling, bias or an integer conversion (the fixture lists
`depth_scale = 5.96e-08 = 2^-24`) that the matrix form cannot show. The hypothesis is also much worse on draw
matrices than on the camera block. No fit was applied. Resolving it needs a capture that records the depth
output (or the shader constant derivation), not more algebra.

The pose-contract note that conventional VP substitution alone does not fix an integer-depth path is consistent with this.

## 2. Clip depth constants: near/far are "consistent with", not measured

Across the four camera states `z_c = A*w + C` with `A = 1.0000305143 .. 1.0000305407` and `C = -2.000022 .. -2.000042`.
`projectionFromFov` with `near = 2`, `far = 65536`, `depthRange = [0, 1]` gives `A = 65536/65534 = 1.0000305185`,
`B = 2.0000610`: a D3D-style [0,1] depth. The match is within float32 noise on A; C differs from -B by ~6e-5.
Whether the engine's near plane is really 2 and far 65536 (versus another pair giving the same A and B) is not known.

## 3. Handedness / flipY of the measured camera basis

The measured clip y column is negative (`y_c = -u / tanY`). `decomposeCameraVP(vp, {flipY: true})` (default) treats that as
a flip and reports `handedness = -1` (right x up = -forward, i.e. left-handed in x-right/y-up/z-forward) for all four
states. With `flipY: false` the sign inverts. Whether the engine's world axes are left-handed or the sign is a screen-space
y-down convention is not determined by these matrices.

## 4. Off-centre (asymmetric) lenses in measured data

`decomposeCameraVP` assumes a symmetric lens (no off-centre terms); it reports `axesOrthogonalityResidual` (all four states
< 1e-4). Fixture lens recovered: tan(half FOV) x 0.61517, y 0.34603 (E3: 0.6152 x 0.3460). The asymmetric case is
exercised only for `projectionFromFov` (synthetic), not for decomposition or against measured data.

## 5. Data limits

- Draw eid 1534's `local_to_world` is singular (rows are not a transform: rows 0..2 are collinear offsets of one point), so no
  VP can be recovered from it; it is excluded from recombination tests.
- Per-draw recombination uses the VP recovered from eid 2045 and reproduces eids 2025, 2035, 2045, 2055, 2065 on the x/y/w
  columns only; all those draws share one VP in capture 11906. Camera-block states A1/B1/A2/B2 were captured at different
  instants (E4 phase 1) and are not the same VP as capture 11906; the two sets were not cross-compared.
- Tolerances are float32-scale (`1e-5..2e-5` relative to term magnitudes, `2e-2` absolute on the 2048-scale
  inverse test). The inverse test bounds rows 2/3 at 0.2 because z_c is nearly parallel to w (conditioning ~1/3e-5).
- Native units, axis conventions, and the canonical->native transform are not handled here (see pose-contract C_W_N).
