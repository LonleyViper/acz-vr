// OpenXR-style pose -> matrices. Canonical frame: right-handed, +X right, +Y up, -Z forward
// (docs/proposals/pose-contract.md). Native-engine unit/axis conversion is NOT done here.
import { quatToMat4, translation, multiply, transpose } from './mat4.js';

// pose = { position: [x,y,z], orientation: [x,y,z,w] } (eye -> world). Row-vector matrix.
export function poseToWorld(pose) {
  const p = pose.position;
  if (!p || p.length !== 3 || !p.every(Number.isFinite)) throw new TypeError('position must be 3 finite numbers');
  const rot = quatToMat4(pose.orientation);
  return multiply(rot, translation(p[0], p[1], p[2]));
}

// View matrix (world -> eye) in closed form for a rigid pose: T(-p) * R^T.
export function viewMatrixFromPose(pose) {
  const p = pose.position;
  if (!p || p.length !== 3 || !p.every(Number.isFinite)) throw new TypeError('position must be 3 finite numbers');
  const rot = quatToMat4(pose.orientation);
  return multiply(translation(-p[0], -p[1], -p[2]), transpose(rot));
}
