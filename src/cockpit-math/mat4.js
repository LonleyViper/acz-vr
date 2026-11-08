// 4x4 helpers. Matrices are Float64Array(16), row-major (index = row * 4 + col),
// row-vector convention: p' = [x, y, z, 1] * M, translation in the last row.
// multiply(a, b) applies a first, then b.

export function identity() {
  const m = new Float64Array(16);
  m[0] = m[5] = m[10] = m[15] = 1;
  return m;
}

export function fromRows(rows) {
  if (!Array.isArray(rows) || rows.length !== 4) throw new TypeError('fromRows expects 4 rows');
  const m = new Float64Array(16);
  for (let r = 0; r < 4; r++) {
    if (!Array.isArray(rows[r]) || rows[r].length !== 4) throw new TypeError('fromRows expects 4 columns per row');
    for (let c = 0; c < 4; c++) m[r * 4 + c] = rows[r][c];
  }
  return m;
}

export function toRows(m) {
  const rows = [];
  for (let r = 0; r < 4; r++) rows.push([m[r * 4], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]]);
  return rows;
}

export function multiply(a, b) {
  const out = new Float64Array(16);
  for (let r = 0; r < 4; r++) {
    for (let c = 0; c < 4; c++) {
      let s = 0;
      for (let k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
      out[r * 4 + c] = s;
    }
  }
  return out;
}

export function transpose(m) {
  const out = new Float64Array(16);
  for (let r = 0; r < 4; r++) for (let c = 0; c < 4; c++) out[c * 4 + r] = m[r * 4 + c];
  return out;
}

export function translation(x, y, z) {
  const m = identity();
  m[12] = x;
  m[13] = y;
  m[14] = z;
  return m;
}

// General inverse (Gauss-Jordan, partial pivoting). Throws on a singular matrix.
export function inverse(m) {
  const a = Float64Array.from(m);
  const inv = identity();
  let scale = 0;
  for (let i = 0; i < 16; i++) scale = Math.max(scale, Math.abs(a[i]));
  if (!(scale > 0) || !Number.isFinite(scale)) throw new RangeError('matrix is zero or non-finite');
  for (let col = 0; col < 4; col++) {
    let piv = col;
    for (let r = col + 1; r < 4; r++) if (Math.abs(a[r * 4 + col]) > Math.abs(a[piv * 4 + col])) piv = r;
    if (Math.abs(a[piv * 4 + col]) <= scale * 1e-14) throw new RangeError('matrix is singular');
    if (piv !== col) {
      for (let c = 0; c < 4; c++) {
        let t = a[col * 4 + c]; a[col * 4 + c] = a[piv * 4 + c]; a[piv * 4 + c] = t;
        t = inv[col * 4 + c]; inv[col * 4 + c] = inv[piv * 4 + c]; inv[piv * 4 + c] = t;
      }
    }
    const d = a[col * 4 + col];
    for (let c = 0; c < 4; c++) { a[col * 4 + c] /= d; inv[col * 4 + c] /= d; }
    for (let r = 0; r < 4; r++) {
      if (r === col) continue;
      const f = a[r * 4 + col];
      if (f === 0) continue;
      for (let c = 0; c < 4; c++) { a[r * 4 + c] -= f * a[col * 4 + c]; inv[r * 4 + c] -= f * inv[col * 4 + c]; }
    }
  }
  return inv;
}

// Row-vector transform of [x, y, z, w]; returns [x, y, z, w] (no divide).
export function transformVec4(m, v) {
  const out = [0, 0, 0, 0];
  for (let c = 0; c < 4; c++) {
    out[c] = v[0] * m[c] + v[1] * m[4 + c] + v[2] * m[8 + c] + v[3] * m[12 + c];
  }
  return out;
}

export function transformPoint(m, p) {
  return transformVec4(m, [p[0], p[1], p[2], 1]);
}

// Unit quaternion (x, y, z, w) -> rotation as a row-vector matrix (v_out = v_in * M),
// i.e. the transpose of the usual column-vector matrix. Rejects zero/non-finite input.
export function quatToMat4(q) {
  if (!q || q.length !== 4 || !q.every(Number.isFinite)) throw new TypeError('quaternion must be 4 finite numbers');
  const n = Math.hypot(q[0], q[1], q[2], q[3]);
  if (!(n > 0)) throw new RangeError('zero-length quaternion');
  const x = q[0] / n, y = q[1] / n, z = q[2] / n, w = q[3] / n;
  // Column-vector rotation R:
  const r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w), r02 = 2 * (x * z + y * w);
  const r10 = 2 * (x * y + z * w), r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
  const r20 = 2 * (x * z - y * w), r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y);
  // Row-vector matrix = R transposed.
  return Float64Array.from([
    r00, r10, r20, 0,
    r01, r11, r21, 0,
    r02, r12, r22, 0,
    0, 0, 0, 1,
  ]);
}
