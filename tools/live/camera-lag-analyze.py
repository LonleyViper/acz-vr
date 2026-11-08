#!/usr/bin/env python3
"""camera-lag-analyze.py -- OFFLINE analysis of the CAM-EPOCH 'camlag' evidence lines (xr-stereo.log / diagnostics log).

The DLL (xr_stereo_output.cpp camlag_frame) logs, in bounded bursts, one line per composed frame with the inputs it ACTUALLY used:
  xr-stereo;event=camlag;f=<csv>   game (draw) camera basis + eye, the E6 sample used (seam-B 'frame' or Present 'present-*'), its sequence,
                                   read-start/publication times, lock/stage epochs, camera mode, lens
  xr-stereo;event=camlag;p=<csv>   every Present-tick publication (history), same clock
  xr-stereo;event=camlag;r=<csv>   a re-submitted (reused) image with its ORIGINAL snapshot identity
  xr-stereo;event=camlag;s=...     burst summary (counts of refused/missing/dropped)
Column names: the 'event=camlag-columns' lines (defaults below if absent).

For every frame with a valid draw camera it compares the draw rotation with E6 rotations at several offsets:
  * seam-B E6 samples of frames fid+k (k = -3..+3, by producer frame id; missing pairs counted),
  * Present-tick publications nearest to (recording start + d), d = -64..+64 ms in 8 ms steps (within +-4 ms; else missing).
Rotations are compared as the relative rotation Q = G * E^T (rows right/up/fwd; right = fwd x up), reported as the rotation angle and the
SIGNED rotation-vector components about the draw camera's right (pitch), up (yaw) and forward (roll) axes - never Euler subtraction.
Per offset: pairs, missing, median / p95 / max angle, mean signed residuals. Best lag = the smallest median among offsets with enough pairs;
'ambiguous' when the runner-up is within max(0.05 deg, 10%); 'unresolved' when no offset has enough pairs. Also per turn direction and in the
frames around yaw-rate reversals (a constant transform shows the same residual both ways; a lag flips sign with the turn).

A nonzero best lag is a DIAGNOSIS of skew, not permission to compensate by a constant frame offset.

Usage: camera-lag-analyze.py <log> [--json]      camera-lag-analyze.py --selftest
"""
import json
import math
import re
import sys

F_COLS = ("fid,gen,slot,members,mode,src,why,frame_why,e6seq,e6_read_us,e6_pub_us,e6_lock,e6_stage,cur_lock,cur_stage,start_us,publish_us,game,game_eye,"
          "gu0,gu1,gu2,gf0,gf1,gf2,ge0,ge1,ge2,e6,eu0,eu1,eu2,ef0,ef1,ef2,ee0,ee1,ee2,lens_l,lens_r,lens_u,lens_d").split(",")
P_COLS = "seq,usable,read_us,pub_us,lock,stage,eu0,eu1,eu2,ef0,ef1,ef2,ee0,ee1,ee2".split(",")
FRAME_OFFSETS = list(range(-3, 4))
PRESENT_OFFSETS_MS = list(range(-64, 65, 8))
PRESENT_TOL_US = 4000


# ------------------------------------------------------------------------------------------------ geometry
def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def basis(up, fwd):
    """Rows right, up, fwd (world -> camera). right = fwd x up (the game convention right x up = -fwd)."""
    return (cross(fwd, up), tuple(up), tuple(fwd))


def rel(g, e):
    """Q = G * E^T: E-camera coordinates -> G-camera coordinates. Returns (angle_deg, (rx, ry, rz) deg about G's right, up, fwd)."""
    q = [[sum(g[i][k] * e[j][k] for k in range(3)) for j in range(3)] for i in range(3)]
    w = (q[2][1] - q[1][2], q[0][2] - q[2][0], q[1][0] - q[0][1])
    # atan2 of the skew and symmetric parts: well conditioned near 0 (acos of the trace loses ~0.1 deg to 6-decimal rounding)
    ang = math.atan2(0.5 * math.sqrt(sum(x * x for x in w)), (q[0][0] + q[1][1] + q[2][2] - 1.0) / 2.0)
    s = math.sin(ang)
    k = ang / (2.0 * s) if s > 1e-9 else 0.5
    return math.degrees(ang), tuple(math.degrees(k * x) for x in w)


# ------------------------------------------------------------------------------------------------ parsing
def parse(lines):
    fcols, pcols = list(F_COLS), list(P_COLS)
    frames, present, reused, bursts = [], [], [], []
    for line in lines:
        i = line.find("event=camlag")
        if i < 0:
            continue
        body = line[i:].strip()
        if body.startswith("event=camlag-columns"):
            for tok in body.split(";"):
                if tok.startswith("f="):
                    fcols = tok[2:].split(",")
                elif tok.startswith("p="):
                    pcols = tok[2:].split(",")
            continue
        m = re.match(r"event=camlag;([fprs])=(.*)$", body)
        if not m:
            continue
        kind, payload = m.group(1), m.group(2)
        if kind == "s":
            bursts.append(payload)
            continue
        vals = payload.split(",")
        if kind == "f" and len(vals) >= len(fcols):
            frames.append(dict(zip(fcols, vals)))
        elif kind == "p" and len(vals) >= len(pcols):
            present.append(dict(zip(pcols, vals)))
        elif kind == "r":
            reused.append(vals)
    return frames, present, reused, bursts


def vec(d, prefix):
    return tuple(float(d[prefix + str(i)]) for i in range(3))


# ------------------------------------------------------------------------------------------------ analysis
def pct(xs, p):
    if not xs:
        return float("nan")
    s = sorted(xs)
    k = (len(s) - 1) * p
    lo, hi = int(math.floor(k)), int(math.ceil(k))
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


class Acc:
    def __init__(self):
        self.ang, self.res, self.missing = [], [], 0

    def add(self, a, r):
        self.ang.append(a)
        self.res.append(r)

    def summary(self):
        n = len(self.ang)
        out = {"pairs": n, "missing": self.missing}
        if n:
            out.update(median=pct(self.ang, 0.5), p95=pct(self.ang, 0.95), max=max(self.ang),
                       mean_signed_deg=[sum(r[i] for r in self.res) / n for i in range(3)])
        return out


def best_of(table, min_frac=0.5, min_pairs=5):
    """table: {offset: summary}. Returns (verdict, best, runner_up)."""
    top = max((s["pairs"] for s in table.values()), default=0)
    need = max(min_pairs, int(top * min_frac))
    cand = sorted(((s["median"], off) for off, s in table.items() if s["pairs"] >= need and s["pairs"] > 0), key=lambda x: x[0])
    if not cand:
        return "unresolved", None, None
    best = cand[0]
    if len(cand) > 1:
        second = cand[1]
        if second[0] - best[0] <= max(0.05, 0.1 * best[0]):
            return "ambiguous", best[1], second[1]
        return "resolved", best[1], second[1]
    return "resolved", best[1], None


def analyse(frames, present, reused=(), bursts=()):
    seam = {}
    for f in frames:
        if f.get("src") == "frame" and f.get("e6") == "1":
            seam[int(f["fid"])] = basis(vec(f, "eu"), vec(f, "ef"))
    pres = []
    for p in present:
        if p.get("usable") == "1":
            pres.append((int(p["pub_us"]), basis(vec(p, "eu"), vec(p, "ef"))))
    pres.sort(key=lambda x: x[0])
    ptimes = [t for t, _ in pres]

    def nearest_present(t):
        import bisect
        i = bisect.bisect_left(ptimes, t)
        best = None
        for j in (i - 1, i):
            if 0 <= j < len(ptimes) and abs(ptimes[j] - t) <= PRESENT_TOL_US and (best is None or abs(ptimes[j] - t) < abs(ptimes[best] - t)):
                best = j
        return pres[best][1] if best is not None else None

    # yaw rate sign per frame from neighbouring seam-B samples (rotation about the camera up axis)
    def yaw_sign(fid):
        a, b = seam.get(fid - 1), seam.get(fid + 1)
        if a is None or b is None:
            return 0
        _, r = rel(b, a)
        return 1 if r[1] > 1e-4 else -1 if r[1] < -1e-4 else 0

    signs = {fid: yaw_sign(fid) for fid in seam}
    reversal = set()
    ordered = sorted(fid for fid, s in signs.items() if s)
    for a, b in zip(ordered, ordered[1:]):
        if signs[a] != signs[b]:
            for k in range(a - 10, b + 11):
                reversal.add(k)

    groups = {"all": None, "turn+": 1, "turn-": -1, "reversal": "rev"}
    fr = {g: {k: Acc() for k in FRAME_OFFSETS} for g in groups}
    pr = {g: {d: Acc() for d in PRESENT_OFFSETS_MS} for g in groups}
    modes, used = {}, 0
    for f in frames:
        modes[f.get("mode", "?")] = modes.get(f.get("mode", "?"), 0) + 1
        if f.get("game") != "1":
            continue
        used += 1
        fid = int(f["fid"])
        g = basis(vec(f, "gu"), vec(f, "gf"))
        sel = [gname for gname, cond in groups.items()
               if cond is None or (cond == "rev" and fid in reversal) or (cond in (1, -1) and signs.get(fid, 0) == cond)]
        for k in FRAME_OFFSETS:
            e = seam.get(fid + k)
            for gname in sel:
                if e is None:
                    fr[gname][k].missing += 1
                else:
                    fr[gname][k].add(*rel(g, e))
        start = int(f["start_us"])
        for d in PRESENT_OFFSETS_MS:
            e = nearest_present(start + d * 1000)
            for gname in sel:
                if e is None:
                    pr[gname][d].missing += 1
                else:
                    pr[gname][d].add(*rel(g, e))
    report = {"frames": len(frames), "frames_with_draw_camera": used, "present_samples": len(pres), "reused_images": len(reused),
              "modes": modes, "bursts": list(bursts), "groups": {}}
    for gname in groups:
        ft = {k: a.summary() for k, a in fr[gname].items()}
        pt = {d: a.summary() for d, a in pr[gname].items()}
        fv = best_of(ft)
        pv = best_of(pt)
        report["groups"][gname] = {"frame_offsets": ft, "present_offsets_ms": pt,
                                   "best_frame_offset": {"verdict": fv[0], "best": fv[1], "runner_up": fv[2]},
                                   "best_present_offset_ms": {"verdict": pv[0], "best": pv[1], "runner_up": pv[2]}}
    report["note"] = "a nonzero best lag is a diagnosis of skew, not permission to compensate by a constant offset"
    return report


def text(report):
    out = ["frames %d (with draw camera %d), present samples %d, reused images %d, modes %s" % (
        report["frames"], report["frames_with_draw_camera"], report["present_samples"], report["reused_images"], report["modes"])]
    for gname, g in report["groups"].items():
        bf, bp = g["best_frame_offset"], g["best_present_offset_ms"]
        out.append("[%s] best frame offset: %s %s (runner-up %s); best Present offset: %s %s ms (runner-up %s)" % (
            gname, bf["verdict"], bf["best"], bf["runner_up"], bp["verdict"], bp["best"], bp["runner_up"]))
        for label, tab in (("frame", g["frame_offsets"]), ("present-ms", g["present_offsets_ms"])):
            for off, s in tab.items():
                if s["pairs"]:
                    out.append("  %s %+4d: pairs %5d missing %5d median %.3f p95 %.3f max %.3f deg; signed (right/up/fwd) %+.3f %+.3f %+.3f" % (
                        label, off, s["pairs"], s["missing"], s["median"], s["p95"], s["max"], *s["mean_signed_deg"]))
                else:
                    out.append("  %s %+4d: pairs 0 missing %d" % (label, off, s["missing"]))
    out.append(report["note"])
    return "\n".join(out)


# ------------------------------------------------------------------------------------------------ synthetic self-test
def _att(t, moving=True):
    """A turning, rolling, pitching attitude (yaw reverses every 2 s). Returns (up, fwd)."""
    yaw = math.radians(30.0 * math.sin(2 * math.pi * t / 4.0)) if moving else 0.3
    pitch = math.radians(5.0 * math.sin(2 * math.pi * t / 3.0)) if moving else 0.05
    roll = math.radians(20.0 * math.sin(2 * math.pi * t / 5.0)) if moving else -0.1
    fwd0 = (-math.sin(yaw) * math.cos(pitch), math.sin(pitch), -math.cos(yaw) * math.cos(pitch))
    right0 = (math.cos(yaw), 0.0, -math.sin(yaw))
    up0 = cross(right0, fwd0)   # (right, up, -fwd) right-handed: up = (-fwd) x right = right x fwd
    cr, sr = math.cos(roll), math.sin(roll)
    up = tuple(up0[i] * cr + right0[i] * sr for i in range(3))
    return up, fwd0


def _synth(lag_frames=1, present_phase_ms=0.0, moving=True, n=600, noise=0.0, game=True):
    import random
    rnd = random.Random(5)
    dt = 1.0 / 60.0
    lines = []
    for k in range(1, n + 1):
        t = k * dt
        start_us = int(t * 1e6)
        eu, ef = _att(t, moving)
        gu, gf = _att(t + lag_frames * dt, moving)
        if noise:
            gu = tuple(x + rnd.gauss(0, noise) for x in gu)
        vals = [k, 5000 + k, 0, 7, "correlated-hybrid", "frame", "ok", "ok", k, start_us - 100, start_us, 1, 1, 1, 1, start_us, start_us + 12000,
                1 if game else 0, 1, *("%.6f" % x for x in gu), *("%.6f" % x for x in gf), 0, 0, 0, 1, *("%.6f" % x for x in eu), *("%.6f" % x for x in ef),
                0, 0, 0, 0.6152, 0.6152, 0.346, 0.346]
        lines.append("xr-stereo;event=camlag;f=" + ",".join(str(v) for v in vals))
        pt = t + present_phase_ms / 1000.0
        pu, pf = _att(pt, moving)
        pv = [k, 1, int(pt * 1e6) - 100, int(pt * 1e6), 1, 1, *("%.6f" % x for x in pu), *("%.6f" % x for x in pf), 0, 0, 0]
        lines.append("xr-stereo;event=camlag;p=" + ",".join(str(v) for v in pv))
    return lines


def selftest():
    fails = []

    def check(c, what):
        if not c:
            fails.append(what)

    # orthonormality of the synthetic basis and the relative-rotation decomposition
    u, f = _att(0.7)
    b = basis(u, f)
    for i in range(3):
        for j in range(3):
            d = sum(b[i][k] * b[j][k] for k in range(3))
            check(abs(d - (1.0 if i == j else 0.0)) < 1e-9, "orthonormal basis")
    check(abs(rel(b, b)[0]) < 1e-6, "identity angle 0")
    # a pure yaw of 2 deg about the camera up axis shows up as a signed 'up' residual of magnitude 2
    u2, f2 = _att(0.0, moving=False)
    yaw = math.radians(2.0)
    bb = basis(u2, f2)
    r, up_, fw = bb
    f_rot = tuple(fw[i] * math.cos(yaw) + r[i] * math.sin(yaw) for i in range(3))
    ang, res = rel(basis(u2, f_rot), bb)
    check(abs(ang - 2.0) < 1e-6 and abs(abs(res[1]) - 2.0) < 1e-6 and abs(res[0]) < 1e-6 and abs(res[2]) < 1e-6, "yaw residual on the up axis")
    # draws one frame AHEAD of their seam-B sample: best frame offset +1; Present grid aligned with the frames -> best Present offset +16 ms (1 frame = 16.7 ms)
    rep = analyse(*parse(_synth(lag_frames=1))[:2])
    a = rep["groups"]["all"]
    check(a["best_frame_offset"]["verdict"] == "resolved" and a["best_frame_offset"]["best"] == 1, "best frame offset +1: %s" % a["best_frame_offset"])
    check(a["best_present_offset_ms"]["best"] == 16, "best present +16: %s" % a["best_present_offset_ms"])
    check(rep["groups"]["reversal"]["best_frame_offset"]["best"] == 1, "reversal subset agrees")
    check(a["frame_offsets"][0]["median"] > 0.1 and a["frame_offsets"][1]["median"] < 1e-3,
          "residual medians %.4f %.4f" % (a["frame_offsets"][0]["median"], a["frame_offsets"][1]["median"]))
    # signed residuals flip with the turn direction for a lag (not for a fixed transform)
    tp, tm = rep["groups"]["turn+"]["frame_offsets"][0], rep["groups"]["turn-"]["frame_offsets"][0]
    check(tp["pairs"] > 0 and tm["pairs"] > 0 and tp["mean_signed_deg"][1] * tm["mean_signed_deg"][1] < 0, "lag residual flips sign with the turn")
    # no lag: offset 0
    rep0 = analyse(*parse(_synth(lag_frames=0, noise=1e-4))[:2])
    check(rep0["groups"]["all"]["best_frame_offset"]["best"] == 0, "no lag -> 0")
    # static attitude: every offset equal -> ambiguous (motion is required to separate timing from a fixed transform)
    reps = analyse(*parse(_synth(lag_frames=1, moving=False))[:2])
    check(reps["groups"]["all"]["best_frame_offset"]["verdict"] == "ambiguous", "static -> ambiguous: %s" % reps["groups"]["all"]["best_frame_offset"])
    # no draw cameras -> unresolved, missing pairs counted when frames skip ids
    repn = analyse(*parse(_synth(game=False))[:2])
    check(repn["groups"]["all"]["best_frame_offset"]["verdict"] == "unresolved", "no draw camera -> unresolved")
    sk = [l for l in _synth(lag_frames=1) if ";f=" not in l or int(l.split("f=")[1].split(",")[0]) % 3 == 0]
    reps2 = analyse(*parse(sk)[:2])
    check(reps2["groups"]["all"]["frame_offsets"][1]["missing"] > 0 and reps2["groups"]["all"]["frame_offsets"][3]["pairs"] > 0, "skipped ids -> missing pairs counted")
    # column header lines are honoured and the text report renders
    hdr = ["xr-stereo;event=camlag-columns;v=1;f=" + ",".join(F_COLS), "xr-stereo;event=camlag-columns;v=1;p=" + ",".join(P_COLS) + ";r=fid,e6seq,mode,age_ms"]
    fr, pr, _, _ = parse(hdr + _synth(n=60) + ["t xr-stereo;event=camlag;r=5,5,correlated-hybrid,12", "xr-stereo;event=camlag;s=burst:1,frames:60"])
    check(len(fr) == 60 and len(pr) == 60, "parse with header")
    check("best frame offset" in text(analyse(fr, pr)), "text report")
    if fails:
        print("camera-lag-analyze selftest FAILED:\n  " + "\n  ".join(fails))
        return 1
    print("camera-lag-analyze selftest: all checks passed")
    return 0


def main(argv):
    if len(argv) > 1 and argv[1] == "--selftest":
        return selftest()
    if len(argv) < 2:
        print(__doc__)
        return 2
    with open(argv[1], encoding="utf-8", errors="replace") as fh:
        frames, present, reused, bursts = parse(fh)
    rep = analyse(frames, present, reused, bursts)
    print(json.dumps(rep, indent=1) if "--json" in argv else text(rep))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
