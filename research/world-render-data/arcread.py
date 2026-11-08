"""Minimal READ-ONLY reader for the ACZ PC .arc archives (independent re-derivation).

Written from scratch for the offline terrain-data inventory (no third-party code):
  * whole file is XORed word-wise (u32 LE) with key_i = (A + B*i) mod 2^32, i = absolute word index
  * A,B are per-archive; recovered here from zero-plaintext runs (key progression step == B)
  * header (16 bytes) is not covered by that scheme; word0 magic 0x454758c3
  * decrypted index: u32 count, u32 name_table_bytes, u32 version, names, entries
      v0: plain, names NUL-terminated, entries = (offset,size) pairs        (e.arc verified)
      v1: all index fields/names are bitwise-NOT, entries = 3 words         (d.arc, see doc)
No game files are ever written.  Usage: python -I arcread.py list <arc>
"""
import numpy as np, struct, sys

M = 0xffffffff


def findkey(path, maxw=40_000_000):
    w = np.fromfile(path, dtype='<u4', count=maxw)
    d = (w[1:] - w[:-1]).astype(np.uint32)
    vals, c = np.unique(d[4:], return_counts=True)
    B = int(vals[np.argmax(c)])
    idx = np.nonzero(d == B)[0]
    i = int(idx[len(idx) // 2]) + 1
    A = (int(w[i]) - B * i) & M
    return A, B


class Arc:
    def __init__(self, path):
        self.path = path
        self.A, self.B = findkey(path)
        self.f = open(path, 'rb')

    def read(self, off, n):
        """decrypted bytes [off, off+n)"""
        a = off & ~3
        e = (off + n + 3) & ~3
        self.f.seek(a)
        raw = np.frombuffer(self.f.read(e - a), dtype='<u4')
        i = np.arange(a // 4, a // 4 + len(raw), dtype=np.uint64)
        ks = ((np.uint64(self.A) + np.uint64(self.B) * i) & np.uint64(M)).astype(np.uint32)
        out = (raw ^ ks).astype('<u4').tobytes()
        return out[off - a: off - a + n]


def lz4_block(src, usize):
    out = bytearray(); i = 0; n = len(src)
    while i < n:
        t = src[i]; i += 1
        l = t >> 4
        if l == 15:
            while True:
                b = src[i]; i += 1; l += b
                if b != 255: break
        out += src[i:i + l]; i += l
        if i >= n: break
        o = src[i] | (src[i + 1] << 8); i += 2
        m = t & 15
        if m == 15:
            while True:
                b = src[i]; i += 1; m += b
                if b != 255: break
        m += 4
        s = len(out) - o
        for k in range(m): out.append(out[s + k])
    return bytes(out)


def entries(a):
    """-> list of (name, file_offset, usize, csize). file_offset already includes the +12 bias."""
    cnt, nt, ver = struct.unpack('<3I', a.read(16, 12))
    if ver >> 31:
        cnt ^= M; nt ^= M; ver ^= M
    names = a.read(28, nt)
    if ver == 1:
        names = bytes(x ^ 0xff for x in names)
    nl = names.split(b'\0')[:cnt]
    k = 3 if ver == 1 else 2
    w = struct.unpack('<%dI' % (k * cnt), a.read(28 + nt, 4 * k * cnt))
    if ver == 1:
        w = [x ^ M for x in w]
    out = []
    for i in range(cnt):
        if k == 3:
            o, u, c = w[3 * i:3 * i + 3]
        else:
            o, u = w[2 * i:2 * i + 2]; c = u
        out.append((nl[i].decode('latin1'), o + 12, u, c))
    return ver, out


def extract_bytes(a, e):
    name, off, u, c = e
    raw = a.read(off, c)
    if getattr(a, 'ver', 0) == 1:  # v1 payloads are stored bitwise-NOT after the XOR layer
        raw = bytes(np.bitwise_not(np.frombuffer(raw, dtype=np.uint8)))
    return lz4_block(raw, u) if c != u else raw


if __name__ == '__main__':
    cmd, path = sys.argv[1], sys.argv[2]
    a = Arc(path)
    ver, es = entries(a)
    a.ver = ver
    if cmd == 'list':
        print('# version', ver, 'files', len(es))
        for n, o, u, c in es:
            print('%s\t%d\t%d\t%d' % (n, o, u, c))
    elif cmd == 'extract':
        import os
        out = sys.argv[3]
        pats = sys.argv[4:]
        for e in es:
            if pats and not any(p in e[0] for p in pats):
                continue
            d = extract_bytes(a, e)
            assert len(d) == e[2], e
            p = os.path.join(out, e[0])
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, 'wb').write(d)
            print('wrote', p, len(d))
