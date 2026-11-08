# cockpit-math

Dependency-free ES modules; pure computation (no fs/net/process). Matrices are `Float64Array(16)`, row-major,
**row-vector** convention (`p' = [x,y,z,1] * M`, translation in the last row, `multiply(a, b)` applies `a` first).

| Module | Provides |
|---|---|
| `mat4.js` | `identity`, `fromRows`, `toRows`, `multiply`, `inverse`, `transpose`, `translation`, `transformPoint`, `transformVec4`, `quatToMat4` |
| `pose.js` | `poseToWorld(pose)`, `viewMatrixFromPose(pose)` — OpenXR pose `{position, orientation:[x,y,z,w]}`; canonical +X right, +Y up, -Z forward |
| `projection.js` | `projectionFromFov(fov, {near, far, depthRange, flipY})`, `depthTerms` — measured form `w = -z_view`, `z_c = A*w - B` |
| `ps2.js` | `ps2ViewportMatrix` — `x_s=258.048*x_c+2048*w`, `y_s=225.792*y_c+2048*w`, `w_s=w`, depth term parameterised (default none) |
| `camera.js` | `recoverCameraVP` (= inverse(local_to_world)*local_to_clip), `decomposeCameraVP` (eye, axes, tan half FOV), `substituteEye` (local_to_clip + local_to_ps2_screen) |

Typical per-eye flow: `eyeVP = multiply(viewMatrixFromPose(pose), projectionFromFov(fov, {...}))`, then
`substituteEye(draw.local_to_world, eyeVP)`. Axis/unit conversion from the native frame (pose-contract `C_W_N`) is the
caller's job and is not implemented or measured here.

Tests: `node --test tests/cockpit-math`. They validate algebra against the measured fixture and analytic cases only.
They do not show that the game or any runtime behaves this way. See `UNRESOLVED.md` for what is not reproduced
(notably the PS2 z/depth term).
