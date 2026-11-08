"""Offline reference for atomic cockpit-panel routing and premultiplied blending.

No GPU, game, OpenXR or files are accessed. A runtime implementation must also
authenticate aircraft identity, frame lifetime, depth and per-eye placement.
"""
import math


def valid_rect(rect, width, height):
    if not isinstance(rect, (list, tuple)) or len(rect) != 4:
        return False
    if any(type(x) is not int for x in rect):
        return False
    x, y, w, h = rect
    return x >= 0 and y >= 0 and w > 0 and h > 0 and x + w <= width and y + h <= height


def overlap(a, b):
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    return ax < bx + bw and bx < ax + aw and ay < by + bh and by < ay + ah


def routing_plan(width, height, displays, protected, *, matching_aircraft, frame_valid,
                 hud_valid, stereo_valid, depth_valid, markers_separated, radar_complete,
                 required_kinds=('radar', 'stores_status')):
    """Return crops to mask only if every required console panel can be drawn.

    An empty result means preserve the entire original canopy HUD. The caller
    must validate the real panel geometry and finish the GPU work atomically.
    """
    if (type(width) is not int or type(height) is not int or width <= 0 or height <= 0
            or not all(value is True for value in (matching_aircraft, frame_valid, hud_valid, stereo_valid, depth_valid,
                                                   markers_separated, radar_complete))
            or not displays or not protected):
        return []
    if (not isinstance(required_kinds, (list, tuple, set, frozenset)) or not required_kinds
            or any(kind not in ('radar', 'stores_status') for kind in required_kinds)):
        return []
    if any(not valid_rect(rect, width, height) for rect in protected):
        return []
    crops = []
    ids = set()
    kinds = set()
    for panel in displays:
        if not isinstance(panel, dict):
            return []
        name, kind, crop = panel.get('id'), panel.get('kind'), panel.get('crop')
        if (not isinstance(name, str) or not name or name in ids
                or kind not in ('radar', 'stores_status') or panel.get('surface_valid') is not True
                or not valid_rect(crop, width, height)
                or any(overlap(crop, rect) for rect in protected)
                or any(overlap(crop, rect) for rect in crops)):
            return []
        ids.add(name)
        kinds.add(kind)
        crops.append(tuple(crop))
    return crops if set(required_kinds).issubset(kinds) else []


def premultiplied_over(src, dst):
    """Linear premultiplied RGBA source-over; reject invalid nonfinite input."""
    if len(src) != 4 or len(dst) != 4 or not all(math.isfinite(x) for x in (*src, *dst)):
        raise ValueError('RGBA must contain four finite values')
    if not 0 <= src[3] <= 1 or not 0 <= dst[3] <= 1:
        raise ValueError('alpha must be within [0,1]')
    keep = 1.0 - src[3]
    return tuple(src[i] + dst[i] * keep for i in range(4))


def consumer_hud_shade(src):
    """Mirror the existing clamp/exposure=1 HUD consumer's coverage policy.

    Alpha is raised to at least the maximum shaded RGB channel. This is not
    equivalent to trusting the stored FP16 alpha, and is not necessarily the
    correct future cockpit-display artistic treatment.
    """
    if len(src) != 4 or not all(math.isfinite(x) for x in src):
        raise ValueError('RGBA must contain four finite values')
    rgb = tuple(min(1.0, max(0.0, v)) for v in src[:3])
    alpha = min(1.0, max(0.0, src[3], *rgb))
    return (*rgb, alpha)


def project_quad(corners, clip_matrix):
    """Project four ordered 3D corners with a row-vector clip matrix.

    This is a conservative full-frustum check, not a clipping algorithm. None
    means refuse the entire surface; it never clamps a missing corner to screen.
    Coordinates use D3D clip depth 0 <= z <= w and return NDC pairs.
    """
    if len(corners) != 4 or len(clip_matrix) != 4 or any(len(row) != 4 for row in clip_matrix):
        return None
    if not all(math.isfinite(v) for row in clip_matrix for v in row):
        return None
    projected = []
    for corner in corners:
        if len(corner) != 3 or not all(math.isfinite(v) for v in corner):
            return None
        p = (*corner, 1.0)
        clip = [sum(p[row] * clip_matrix[row][column] for row in range(4)) for column in range(4)]
        x, y, z, w = clip
        if not all(math.isfinite(v) for v in clip) or w <= 1e-6 or not (-w <= x <= w and -w <= y <= w and 0 <= z <= w):
            return None
        projected.append((x / w, y / w))
    # Reject flat, crossed, or concave projected quads, rather than displaying
    # an ambiguous or inverted projection. Caller supplies perimeter order.
    turns = []
    for i in range(4):
        a, b, c = projected[i], projected[(i + 1) % 4], projected[(i + 2) % 4]
        turns.append((b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0]))
    if not (all(v > 1e-10 for v in turns) or all(v < -1e-10 for v in turns)):
        return None
    return projected


def project_stereo(corners, left_clip, right_clip):
    """Both eyes must project successfully in the same frame."""
    left, right = project_quad(corners, left_clip), project_quad(corners, right_clip)
    return (left, right) if left is not None and right is not None else None
