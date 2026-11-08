#!/usr/bin/env python
"""png_diff.py - where does scene-end.png differ from world.png, outside the cockpit?

Offline helper for the producer PNG burst sets (J-effects-content-audit.md section 5):
  world.png     the scene render target at the FIRST cockpit member (the world snapshot)
  scene-end.png the scene render target at the START OF THE HUD REGION (after all scene draws)
  eye0.png      the cockpit eye image (used only as a cockpit mask)

Pixels that are lit in scene-end but not in world and lie OUTSIDE the cockpit were drawn between the
first member and the HUD region (smoke, tracers, explosions, ...). Pixels INSIDE the cockpit mask
are expected to differ (the cockpit itself is drawn there) and are reported separately.

Cockpit mask: pixels where eye0.png alpha > 0. The producer's eye0.png is written with alpha 255
everywhere (the depth coverage alpha is not read back); when the alpha channel is unusable
(covers >= 99.9 percent of the image) the mask falls back to eye0 pixels with a non-zero RGB.
Without --eye0 there is no mask (every changed pixel counts as outside).

Usage (run with isolation: python -I):
  python -I tools/analysis/png_diff.py world.png scene-end.png [--eye0 eye0.png] [--out diff.png]
        [--threshold 8] [--cell 16] [--top 20] [--json]
  python -I tools/analysis/png_diff.py --self-test

Output: a difference image (red = changed outside the mask, scaled by the difference; blue = changed
inside the mask; grey = unchanged, the scene-end image darkened) and the bounding boxes of the
changed regions outside the mask (8-connected cells of --cell pixels, biggest first).
Only the Python standard library is used. Exit status 0 on success, 2 on a usage or file error.
"""
import json
import struct
import sys
import zlib

PNG_SIG = b"\x89PNG\r\n\x1a\n"


# --------------------------------------------------------------------------- PNG io
def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(data):
    """Decode a non-interlaced 8-bit PNG (colour type 0, 2, 3 without palette alpha, 4 or 6) to
    (width, height, rgba bytes, row-major, 4 bytes per pixel)."""
    if data[:8] != PNG_SIG:
        raise ValueError("not a PNG")
    pos = 8
    ihdr = None
    idat = []
    plte = None
    while pos + 8 <= len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if typ == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif typ == b"PLTE":
            plte = body
        elif typ == b"IDAT":
            idat.append(body)
        elif typ == b"IEND":
            break
    if ihdr is None or not idat:
        raise ValueError("missing IHDR/IDAT")
    w, h, depth, ctype, _comp, _filt, interlace = ihdr
    if depth != 8 or interlace != 0 or ctype not in (0, 2, 3, 4, 6):
        raise ValueError("unsupported PNG (need 8-bit, non-interlaced, colour type 0/2/3/4/6)")
    if ctype == 3 and plte is None:
        raise ValueError("palette PNG without PLTE")
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(b"".join(idat))
    stride = w * bpp
    if len(raw) < h * (stride + 1):
        raise ValueError("truncated image data")
    rows = []
    prev = bytearray(stride)
    at = 0
    for _y in range(h):
        f = raw[at]
        cur = bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        if f == 1:
            for i in range(bpp, stride):
                cur[i] = (cur[i] + cur[i - bpp]) & 255
        elif f == 2:
            for i in range(stride):
                cur[i] = (cur[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                left = cur[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + ((left + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                left = cur[i - bpp] if i >= bpp else 0
                ul = prev[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + _paeth(left, prev[i], ul)) & 255
        elif f != 0:
            raise ValueError("bad filter type %d" % f)
        rows.append(cur)
        prev = cur
    out = bytearray(w * h * 4)
    o = 0
    for cur in rows:
        if ctype == 6:
            out[o:o + w * 4] = cur
        else:
            for x in range(w):
                if ctype == 2:
                    out[o + 4 * x:o + 4 * x + 4] = bytes((cur[3 * x], cur[3 * x + 1], cur[3 * x + 2], 255))
                elif ctype == 0:
                    g = cur[x]
                    out[o + 4 * x:o + 4 * x + 4] = bytes((g, g, g, 255))
                elif ctype == 4:
                    g = cur[2 * x]
                    out[o + 4 * x:o + 4 * x + 4] = bytes((g, g, g, cur[2 * x + 1]))
                else:
                    i = cur[x] * 3
                    out[o + 4 * x:o + 4 * x + 4] = bytes((plte[i], plte[i + 1], plte[i + 2], 255))
        o += w * 4
    return w, h, bytes(out)


def write_png(w, h, rgba, filter_type=0):
    """Encode an RGBA8 image (filter 0 = None unless a test asks for another)."""
    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    stride = w * 4
    raw = bytearray()
    prev = bytes(stride)
    for y in range(h):
        row = rgba[y * stride:(y + 1) * stride]
        if filter_type == 0:
            raw += b"\x00" + row
        else:
            f = bytearray(stride)
            for i in range(stride):
                left = row[i - 4] if i >= 4 else 0
                up = prev[i]
                ul = prev[i - 4] if i >= 4 else 0
                pred = {1: left, 2: up, 3: (left + up) >> 1, 4: _paeth(left, up, ul)}[filter_type]
                f[i] = (row[i] - pred) & 255
            raw += bytes((filter_type,)) + bytes(f)
        prev = row
    return PNG_SIG + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")


# --------------------------------------------------------------------------- analysis
def cockpit_mask(eye_w, eye_h, eye_rgba, w, h):
    """(mask bytearray w*h of 0/1, description). Mask = eye0 alpha > 0; if that covers >= 99.9 percent
    (the producer writes alpha 255) the mask is eye0 RGB != 0 instead."""
    if (eye_w, eye_h) != (w, h):
        raise ValueError("eye0.png size %dx%d differs from %dx%d" % (eye_w, eye_h, w, h))
    n = w * h
    alpha = eye_rgba[3::4]
    mask = bytearray(1 if a else 0 for a in alpha)
    covered = sum(mask)
    desc = "eye0 alpha>0"
    if covered >= 0.999 * n:
        desc = "eye0 alpha is constant (>= 99.9 percent opaque): fell back to eye0 rgb != 0"
        mask = bytearray(n)
        for i in range(n):
            j = 4 * i
            if eye_rgba[j] or eye_rgba[j + 1] or eye_rgba[j + 2]:
                mask[i] = 1
    return mask, desc


def analyse(w, h, world, scene, mask=None, threshold=8, cell=16, top=20, want_image=True):
    """Compare two RGBA8 images. Returns (report dict, diff image rgba bytes or None)."""
    if cell < 1:
        raise ValueError("cell must be >= 1")
    cw, ch = (w + cell - 1) // cell, (h + cell - 1) // cell
    cells = {}  # (cx, cy) -> [x0, y0, x1, y1, count]
    changed_out = changed_in = 0
    img = bytearray(w * h * 4) if want_image else None
    for y in range(h):
        base = y * w * 4
        ra, rb = world[base:base + w * 4], scene[base:base + w * 4]
        same = ra == rb
        for x in range(w):
            j = 4 * x
            if same:
                d = 0
            else:
                d = max(abs(ra[j] - rb[j]), abs(ra[j + 1] - rb[j + 1]), abs(ra[j + 2] - rb[j + 2]))
            inside = bool(mask[y * w + x]) if mask is not None else False
            if d > threshold:
                if inside:
                    changed_in += 1
                else:
                    changed_out += 1
                    c = cells.get((x // cell, y // cell))
                    if c is None:
                        cells[(x // cell, y // cell)] = [x, y, x, y, 1]
                    else:
                        if x < c[0]: c[0] = x
                        if y < c[1]: c[1] = y
                        if x > c[2]: c[2] = x
                        if y > c[3]: c[3] = y
                        c[4] += 1
            if img is not None:
                o = base + j
                if d > threshold and not inside:
                    img[o:o + 4] = bytes((min(255, 128 + d // 2), 0, 0, 255))
                elif d > threshold:
                    img[o:o + 4] = bytes((0, 0, min(255, 96 + d // 2), 255))
                else:
                    img[o:o + 4] = bytes((rb[j] // 4 + 24, rb[j + 1] // 4 + 24, rb[j + 2] // 4 + 24, 255))
    # 8-connected components over the marked cells
    seen = set()
    regions = []
    for start in cells:
        if start in seen:
            continue
        stack = [start]
        seen.add(start)
        box = None
        pixels = 0
        ncells = 0
        while stack:
            cx, cy = stack.pop()
            c = cells[(cx, cy)]
            ncells += 1
            pixels += c[4]
            if box is None:
                box = [c[0], c[1], c[2], c[3]]
            else:
                box = [min(box[0], c[0]), min(box[1], c[1]), max(box[2], c[2]), max(box[3], c[3])]
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    nb = (cx + dx, cy + dy)
                    if nb in cells and nb not in seen:
                        seen.add(nb)
                        stack.append(nb)
        regions.append({"bbox": box, "pixels": pixels, "cells": ncells})
    regions.sort(key=lambda r: -r["pixels"])
    report = {
        "size": [w, h],
        "threshold": threshold,
        "cell": cell,
        "changed_outside_mask": changed_out,
        "changed_inside_mask": changed_in,
        "regions_outside_mask": len(regions),
        "regions": regions[:top],
        "bbox_format": "x0,y0,x1,y1 inclusive",
    }
    return report, (bytes(img) if img is not None else None)


# --------------------------------------------------------------------------- self test
def _check(cond, what):
    if not cond:
        raise AssertionError(what)


def self_test():
    # 1. PNG round trip with every filter type
    w, h = 13, 7
    px = bytes((x * 19 + y * 7 + c * 31) & 255 for y in range(h) for x in range(w) for c in range(4))
    for f in (0, 1, 2, 3, 4):
        rw, rh, back = read_png(write_png(w, h, px, f))
        _check((rw, rh, back) == (w, h, px), "png round trip filter %d" % f)
    # 2. scene: world = dark gradient; scene-end = world + a cockpit patch + a smoke blob + a tiny sub-threshold change
    w, h = 96, 64
    world = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            o = 4 * (y * w + x)
            world[o:o + 4] = bytes((x, y, 40, 255))
    scene = bytearray(world)
    eye = bytearray(w * h * 4)
    def fill(img, x0, y0, x1, y1, rgb):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                o = 4 * (y * w + x)
                img[o:o + 3] = bytes(rgb)
    fill(scene, 60, 40, 90, 60, (200, 200, 200))        # the cockpit: changed, inside the mask
    for y in range(40, 61):
        for x in range(60, 91):
            eye[4 * (y * w + x) + 3] = 255              # eye0 alpha > 0 only there
            eye[4 * (y * w + x)] = 77
    fill(scene, 10, 8, 25, 14, (230, 230, 230))         # smoke: changed, outside the mask
    fill(scene, 70, 5, 72, 6, (255, 255, 255))          # a second, small effect, far away
    scene[4 * (30 * w + 40):4 * (30 * w + 40) + 3] = bytes((world[4 * (30 * w + 40)] + 3, world[4 * (30 * w + 40) + 1], 40))  # below threshold
    mask, desc = cockpit_mask(w, h, bytes(eye), w, h)
    _check(desc == "eye0 alpha>0" and sum(mask) == 31 * 21, "mask from alpha")
    rep, img = analyse(w, h, bytes(world), bytes(scene), mask, threshold=8, cell=8)
    _check(rep["changed_outside_mask"] == 16 * 7 + 3 * 2, "outside count %r" % rep["changed_outside_mask"])
    _check(rep["changed_inside_mask"] == 31 * 21, "inside count %r" % rep["changed_inside_mask"])
    _check(rep["regions_outside_mask"] == 2, "two regions %r" % rep["regions"])
    _check(rep["regions"][0]["bbox"] == [10, 8, 25, 14] and rep["regions"][1]["bbox"] == [70, 5, 72, 6], "bboxes %r" % rep["regions"])
    _check(img is not None and img[4 * (10 * w + 12):4 * (10 * w + 12) + 4][0] > 128 and img[4 * (10 * w + 12) + 2] == 0, "red outside")
    _check(img[4 * (50 * w + 70) + 2] > 96 and img[4 * (50 * w + 70)] == 0, "blue inside")
    _check(img[4 * (30 * w + 40) + 3] == 255, "alpha opaque")
    # 3. without a mask every change is outside; a threshold above the contrast finds nothing
    rep2, _ = analyse(w, h, bytes(world), bytes(scene), None, threshold=8, cell=8, want_image=False)
    _check(rep2["changed_inside_mask"] == 0 and rep2["changed_outside_mask"] == 16 * 7 + 3 * 2 + 31 * 21, "no mask")
    rep3, _ = analyse(w, h, bytes(world), bytes(scene), mask, threshold=250, cell=8, want_image=False)
    _check(rep3["changed_outside_mask"] == 0 and rep3["regions"] == [], "high threshold")
    # 4. eye0 with constant alpha 255 falls back to rgb != 0
    eye2 = bytearray(eye)
    for i in range(w * h):
        eye2[4 * i + 3] = 255
    mask2, desc2 = cockpit_mask(w, h, bytes(eye2), w, h)
    _check("fell back" in desc2 and sum(mask2) == 31 * 21, "alpha fallback")
    # 5. size mismatch is an error
    try:
        cockpit_mask(w, h, bytes(eye), w + 1, h)
    except ValueError:
        pass
    else:
        _check(False, "size mismatch not detected")
    # 6. the file level API on PNG bytes
    rw, rh, wr = read_png(write_png(w, h, bytes(world), 4))
    _check(wr == bytes(world), "world png")
    print("png_diff self-test OK")
    return 0


# --------------------------------------------------------------------------- cli
def main(argv):
    args = list(argv)
    if "--self-test" in args:
        return self_test()
    opts = {"--eye0": None, "--out": None, "--threshold": "8", "--cell": "16", "--top": "20"}
    flags = {"--json": False}
    pos = []
    i = 0
    while i < len(args):
        a = args[i]
        if a in opts:
            if i + 1 >= len(args):
                print("missing value for " + a, file=sys.stderr)
                return 2
            opts[a] = args[i + 1]
            i += 2
        elif a in flags:
            flags[a] = True
            i += 1
        elif a.startswith("--"):
            print("unknown option " + a, file=sys.stderr)
            return 2
        else:
            pos.append(a)
            i += 1
    if len(pos) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        threshold, cell, top = int(opts["--threshold"]), int(opts["--cell"]), int(opts["--top"])
        with open(pos[0], "rb") as f:
            w, h, world = read_png(f.read())
        with open(pos[1], "rb") as f:
            w2, h2, scene = read_png(f.read())
        if (w, h) != (w2, h2):
            print("size mismatch: %dx%d vs %dx%d" % (w, h, w2, h2), file=sys.stderr)
            return 2
        mask, desc = None, "none (no --eye0: every change counts as outside)"
        if opts["--eye0"]:
            with open(opts["--eye0"], "rb") as f:
                ew, eh, eye = read_png(f.read())
            mask, desc = cockpit_mask(ew, eh, eye, w, h)
        out = opts["--out"] or "png_diff.png"
        rep, img = analyse(w, h, world, scene, mask, threshold, cell, top)
        with open(out, "wb") as f:
            f.write(write_png(w, h, img))
    except (OSError, ValueError, zlib.error) as e:
        print("png_diff: %s" % e, file=sys.stderr)
        return 2
    rep["mask"] = desc
    rep["diff_image"] = out
    if flags["--json"]:
        print(json.dumps(rep, indent=2))
    else:
        print("size %dx%d threshold %d cell %d mask: %s" % (w, h, threshold, cell, desc))
        print("changed outside the cockpit mask: %d pixels in %d regions; inside: %d pixels"
              % (rep["changed_outside_mask"], rep["regions_outside_mask"], rep["changed_inside_mask"]))
        for r in rep["regions"]:
            b = r["bbox"]
            print("  bbox x0=%d y0=%d x1=%d y1=%d  pixels=%d" % (b[0], b[1], b[2], b[3], r["pixels"]))
        print("difference image: " + out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
