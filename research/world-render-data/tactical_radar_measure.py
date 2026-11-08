"""TACTICAL: measure the game's radar widget (box, centre, rings, wedge) in game-window screenshots.

Usage: python -I tactical_radar_measure.py <shot.png> [...]
Prints, per shot, the client area found (title bar / borders removed), the radar box rect, the centre (the cross
intersection), the ring radii along the four half-axes, all in client pixels and normalised to the client (0..1).
Pure numpy (png_io.read_png). Only reads the PNGs.
"""
import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_io import read_png


def load(path):
    a = read_png(path)
    if a.ndim == 2:
        a = np.stack([a] * 3, -1)
    return a[..., :3].astype(np.int32)


def client_rect(img):
    # Window chrome: a bright title bar (rows with mean > 200) on top, near-uniform border columns left/right. The game client is 16:9
    # (scene RT 2560x1440 output 2405x1352 in the step62/step71 runs), so the bottom is derived from the width.
    h, w, _ = img.shape
    m = img.mean(2)
    top = 0
    while top < 80 and not (m[top].mean() > 200):
        top += 1
    while top < 80 and m[top].mean() > 200:
        top += 1
    left = 0
    while left < 40 and m[top:, left].std() < 10:
        left += 1
    # Bottom: the first (near-)black border row below the client (step62/71: row 1383 = 0); the width follows from 16:9.
    bottom = top + 1
    while bottom < h and m[bottom, left:].mean() > 3:
        bottom += 1
    right = min(w, left + int(round((bottom - top) * 2560 / 1440)))
    return left, top, right, bottom


def radar_green(img):
    r, g, b = img[..., 0], img[..., 1], img[..., 2]
    return (g > 150) & (g > r + 60) & (g > b + 40)


def measure(path):
    img = load(path)
    L, T, R, B = client_rect(img)
    cw, ch = R - L, B - T
    sub = img[T:B, L:R]
    gm = radar_green(sub)
    # Search the lower-left quarter for the radar box: long vertical / horizontal green runs.
    y0, x1 = int(ch * 0.55), int(cw * 0.30)
    reg = gm[y0:, :x1]
    cols = reg.sum(0)
    rows = reg.sum(1)
    vx = [x for x in range(x1) if cols[x] > 0.6 * (ch - y0) * 0.4]
    hy = [y for y in range(ch - y0) if rows[y] > 0.6 * x1 * 0.4]
    if not vx or not hy:
        return dict(path=path, client=(L, T, R, B), found=False)
    bx0, bx1 = min(vx), max(vx)
    by0, by1 = min(hy) + y0, max(hy) + y0
    # Cross: the vertical and horizontal faint lines inside the box: the column/row (strictly inside) with the most bright pixels.
    box = sub[by0:by1 + 1, bx0:bx1 + 1].mean(2)
    inner = box[4:-4, 4:-4]
    cx = int(np.argmax(inner.sum(0))) + 4 + bx0
    cy = int(np.argmax(inner.sum(1))) + 4 + by0
    # Rings: along the half-axes from the centre, local maxima of greenness (g - (r+b)/2) away from the cross lines.
    gg = (sub[..., 1] - (sub[..., 0] + sub[..., 2]) / 2.0)
    rings = {}
    for name, dx, dy in (('right', 1, 0), ('left', -1, 0), ('up', 0, -1), ('down', 0, 1)):
        prof = []
        for t in range(6, 400):
            x, y = cx + dx * t, cy + dy * t
            if not (bx0 + 3 < x < bx1 - 3 and by0 + 3 < y < by1 - 3):
                break
            # sample perpendicular offset 3 px so the cross line itself does not dominate
            px, py = x + dy * 3, y + dx * 3
            prof.append((t, gg[py, px]))
        if not prof:
            continue
        v = np.array([p[1] for p in prof])
        base = np.median(v)
        peaks = [prof[i][0] for i in range(1, len(v) - 1) if v[i] >= v[i - 1] and v[i] > v[i + 1] and v[i] > base + 12]
        # merge peaks within 3 px
        merged = []
        for p in peaks:
            if merged and p - merged[-1][-1] <= 3:
                merged[-1].append(p)
            else:
                merged.append([p])
        rings[name] = [round(float(np.mean(m)), 1) for m in merged]
    # Centre refinement: the box centre and the ring symmetry (left/right and up/down radii of the same ring must agree).
    bcx, bcy = (bx0 + bx1) / 2.0, (by0 + by1) / 2.0
    sym = {}
    for a, b, k in (('right', 'left', 'x'), ('down', 'up', 'y')):
        ra, rb = rings.get(a, []), rings.get(b, [])
        n = min(len(ra), len(rb), 4)
        if n:
            sym[k] = float(np.mean([(ra[i] - rb[i]) / 2.0 for i in range(n)]))
    ccx = cx + sym.get('x', 0.0)
    ccy = cy + sym.get('y', 0.0)
    radii = []
    for i in range(4):
        vals = [rings[k][i] + (-1 if k in ('right', 'down') else 1) * sym.get('x' if k in ('right', 'left') else 'y', 0.0)
                for k in rings if len(rings[k]) > i]
        if vals:
            radii.append(round(float(np.mean(vals)), 1))
    return dict(path=os.path.basename(path), client=(L, T, R, B), size=(cw, ch), box=(bx0, by0, bx1, by1),
                box_centre=(bcx, bcy), ring_centre=(round(ccx, 1), round(ccy, 1)), radii_px=radii,
                radii_over_height=[round(r / ch, 5) for r in radii],
                norm_box=(round(bx0 / cw, 5), round(by0 / ch, 5), round(bx1 / cw, 5), round(by1 / ch, 5)),
                norm_centre=(round(ccx / cw, 5), round(ccy / ch, 5)),
                centre=(cx, cy), rings=rings,
                )


def bearing_check(path, tanx=0.6152):
    """Blue blobs: FRND boxes/labels in the upper half and blue arrows inside the radar box. Prints marker azimuths (deg, from the lens) and
    blip bearings (deg clockwise from the radar's up) relative to the measured radar centre: heading-up => the two must agree."""
    r0 = measure(path)
    L, T, R, B = r0['client']
    cx, cy = r0['ring_centre']
    img = load(path)
    sub = img[T:B, L:R]
    r, g, b = sub[..., 0], sub[..., 1], sub[..., 2]
    blue = (b > 150) & (b > r + 60) & (b > g + 40)
    h, w = blue.shape
    seen = np.zeros(blue.shape, bool)
    out = []
    ys, xs = np.nonzero(blue)
    for y0, x0 in zip(ys, xs):
        if seen[y0, x0]:
            continue
        stack = [(y0, x0)]; seen[y0, x0] = True; pts = []
        while stack:
            y, x = stack.pop(); pts.append((y, x))
            for dy in (-2, -1, 0, 1, 2):
                for dx in (-2, -1, 0, 1, 2):
                    yy, xx = y + dy, x + dx
                    if 0 <= yy < h and 0 <= xx < w and blue[yy, xx] and not seen[yy, xx]:
                        seen[yy, xx] = True; stack.append((yy, xx))
        if len(pts) < 20:
            continue
        a = np.array(pts)
        bx, by = a[:, 1].mean(), a[:, 0].mean()
        if by < h * 0.5:
            az = np.degrees(np.arctan((bx - w / 2) / (w / 2) * tanx))
            out.append(('marker-or-label', round(float(bx), 1), round(float(by), 1), len(pts), 'azimuth-deg', round(float(az), 1)))
        elif abs(bx - cx) < 200 and abs(by - cy) < 200:
            br = np.degrees(np.arctan2(bx - cx, -(by - cy)))
            out.append(('radar-blip', round(float(bx), 1), round(float(by), 1), len(pts), 'bearing-deg', round(float(br), 1),
                        'radius-px', round(float(np.hypot(bx - cx, by - cy)), 1)))
    return out


if __name__ == '__main__':
    args = sys.argv[1:]
    if args and args[0] == '--bearings':
        for p in args[1:]:
            print(os.path.basename(p))
            for o in bearing_check(p):
                print('  ', o)
    else:
        for p in args:
            print(measure(p))
