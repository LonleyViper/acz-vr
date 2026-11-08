#!/usr/bin/env python3
"""Offline analysis of look-input-trace.bin (D-269 follow-up, src/acz-vr/ee_look_input_trace.h).

The probe records, for windows around look events, every 4-byte word that changed inside the 64 KiB around the look yaw
float (base+0xFDD410).  This tool reconstructs the per-tick series of each changed word and ranks them against the look
value L (the word at offset 0 of the reference frame, or the L column of ee-camera.log):

  (a) DIGITAL  words with few distinct values that step to a constant while a key is held (kappa-like gain of the best
               value -> {released, held+, held-} mapping, searched over lags);
  (b) FILTER   words that move like L: linear in L at some lag (|r| >= 0.9) and, where the decay can be measured, with
               the same exponential time constant as L;
  (c) LEAD     the candidates of (a)/(b) whose best lag is >= 1 tick, i.e. which change BEFORE L does.

Offsets are printed relative to base+0xFDD410 (the look yaw), e.g. -0x200.  A lead of k means word(t) ~ f(L(t+k)).
Unverified: it has only ever been run on synthetic traces (see --selftest); real-game behaviour is not established.

Usage:
  python -I tools/analysis/look_input_trace.py TRACE.bin [--log ee-camera.log] [--l-source trace|log] [--top 15]
                                                          [--max-lag 8] [--window N] [--verbose]
  python -I tools/analysis/look_input_trace.py --selftest
  python -I tools/analysis/look_input_trace.py --generate OUT.bin [--seed 1]
"""
import argparse
import math
import os
import random
import re
import struct
import sys
import tempfile

MAGIC = b"ACZLIT1\0"
HDR = struct.Struct("<8sIIIIQQIIIIIIIIddII")      # 96 bytes
TICK = struct.Struct("<IIdQII")                    # 32 bytes
DELTA = struct.Struct("<III")                      # 12 bytes
assert HDR.size == 96 and TICK.size == 32 and DELTA.size == 12
CLOSE = {0: "-", 1: "timeout", 2: "budget", 3: "candidate-lost", 4: "identity-changed", 5: "read-failure",
         6: "process-exit", 7: "region-outside-block"}
OPEN = {1: "look-value", 2: "look-event"}


# ------------------------------------------------------------------ parsing
class Window:
    pass


def parse_trace(data):
    """Returns a list of Window objects. Raises ValueError on a malformed file (a truncated LAST window is dropped)."""
    out, p, n = [], 0, len(data)
    while p < n:
        if n - p < HDR.size:
            break                                   # truncated header at the end: ignore
        f = HDR.unpack_from(data, p)
        if f[0] != MAGIC:
            raise ValueError("bad magic at byte %d" % p)
        w = Window()
        (_, w.version, hdr_bytes, w.index, w.open_reason, w.base, w.region, w.region_bytes, w.l_offset, w.tick_count,
         w.preroll, w.delta_count, w.data_bytes, w.flags, w.truncated, w.open_ms, w.close_ms, w.close_reason,
         w.trigger_tick) = f
        if w.version != 1 or hdr_bytes != HDR.size:
            raise ValueError("unsupported header (version %d, %d bytes)" % (w.version, hdr_bytes))
        body = p + hdr_bytes
        if body + w.data_bytes > n:
            break                                   # truncated window at the end: ignore
        q, end, w.ticks = body, body + w.data_bytes, []
        while q < end:
            idx, tid, ms, frame, ns, nt = TICK.unpack_from(data, q)
            q += TICK.size
            if q + ns * DELTA.size > end:
                raise ValueError("tick record overruns its window")
            d = [DELTA.unpack_from(data, q + i * DELTA.size) for i in range(ns)]
            q += ns * DELTA.size
            w.ticks.append((idx, tid, ms, frame, nt, d))
        if q != end:
            raise ValueError("window data size mismatch")
        out.append(w)
        p = end
    return out


def load_trace(path):
    with open(path, "rb") as fh:
        return parse_trace(fh.read())


_L_RE = re.compile(r"^([0-9.]+)\|ee-camera;event=timeline;slot=\d+;base=([0-9a-f]+);.*?;L=([^,;]+),([^,;]+),([^,;]+);")


def load_log_L(path, base):
    """[(ms, yaw)] from the timeline lines of ee-camera.log for the given base (zero-order hold between lines)."""
    pts = []
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            m = _L_RE.match(line)
            if m and int(m.group(2), 16) == base:
                try:
                    pts.append((float(m.group(1)), float(m.group(4))))
                except ValueError:
                    pass
    return pts


# ------------------------------------------------------------------ series reconstruction
def u2f(u):
    return struct.unpack("<f", struct.pack("<I", u & 0xFFFFFFFF))[0]


def u2i(u):
    return u - (1 << 32) if u & 0x80000000 else u


def s16(x):
    return x - 65536 if x & 0x8000 else x


def view_fn(name):
    if name == "f32":
        return lambda u: u2f(u)
    if name == "i32":
        return lambda u: float(u2i(u))
    if name == "i16lo":
        return lambda u: float(s16(u & 0xFFFF))
    return lambda u: float(s16((u >> 16) & 0xFFFF))      # i16hi


VIEWS = ("f32", "i32", "i16lo", "i16hi")


def build_series(w):
    """{word_offset_bytes: [u32 value per tick]} for every word that changed in the window."""
    n = len(w.ticks)
    first = {}
    changes = {}
    for t, rec in enumerate(w.ticks):
        for off, old, new in rec[5]:
            if off not in first:
                first[off] = old
            changes.setdefault(off, []).append((t, new))
    series = {}
    for off, ch in changes.items():
        s, cur, k = [0] * n, first[off], 0
        for t in range(n):
            while k < len(ch) and ch[k][0] == t:
                cur = ch[k][1]
                k += 1
            s[t] = cur
        series[off] = s
    return series


def l_series(w, series, log_pts, source):
    """(L list, label). Trace's own word at l_offset unless the log is requested / the trace has no L motion."""
    n = len(w.ticks)
    own = None
    if w.l_offset in series:
        own = [u2f(u) for u in series[w.l_offset]]
        if any(not math.isfinite(x) for x in own):
            own = [x if math.isfinite(x) else 0.0 for x in own]
    if source == "trace" and own is not None:
        return own, "trace"
    if log_pts and source in ("log", "trace"):
        out, k, cur = [], 0, 0.0
        for rec in w.ticks:
            while k < len(log_pts) and log_pts[k][0] <= rec[2]:
                cur = log_pts[k][1]
                k += 1
            out.append(cur)
        return out, "log"
    return ([0.0] * n if own is None else own), "trace" if own is not None else "none"


# ------------------------------------------------------------------ statistics
def pearson(a, b):
    n = len(a)
    if n < 4:
        return 0.0
    ma, mb = sum(a) / n, sum(b) / n
    sa = sb = sab = 0.0
    for x, y in zip(a, b):
        dx, dy = x - ma, y - mb
        sa += dx * dx
        sb += dy * dy
        sab += dx * dy
    if sa <= 1e-300 or sb <= 1e-300:
        return 0.0
    return sab / math.sqrt(sa * sb)


def shifted(x, y, k):
    """pairs (x[t], y[t+k])"""
    n = len(x)
    lo, hi = max(0, -k), min(n, n - k)
    return x[lo:hi], y[lo + k:hi + k]


def key_classes(L):
    """Per-tick class from L alone, using BACKWARD differences so the proxy is not ahead of L:
    +1/-1 = L is moving away from zero or saturated (key held, direction = sign of L), 0 = released."""
    n = len(L)
    peak = max((abs(x) for x in L), default=0.0)
    cls = [0] * n
    if peak < 0.05:
        return cls
    for t in range(n):
        a = abs(L[t])
        prev = abs(L[t - 1]) if t else a
        rising = a > prev * 1.0005 + 1e-6
        sat = a >= 0.97 * peak and a > 0.5
        if a > 0.01 and (rising or sat):
            cls[t] = 1 if L[t] > 0 else -1
    return cls


def decay_tau(ms, x, floor):
    """Exponential time constant (ms) and fit R^2 of |x - x_end| over the decay after the last peak of |L|-like data.
    x is the series to fit, reference peak index is given by the caller through seg (see decay_segment)."""
    pts = [(t, v) for t, v in zip(ms, x) if v > floor]
    if len(pts) < 6:
        return None
    ts = [p[0] for p in pts]
    ys = [math.log(p[1]) for p in pts]
    n = len(pts)
    mt, my = sum(ts) / n, sum(ys) / n
    stt = sum((a - mt) ** 2 for a in ts)
    if stt <= 0:
        return None
    sty = sum((a - mt) * (b - my) for a, b in zip(ts, ys))
    slope = sty / stt
    if slope >= -1e-9:
        return None
    syy = sum((b - my) ** 2 for b in ys)
    r2 = (sty * sty) / (stt * syy) if syy > 0 else 0.0
    return -1.0 / slope, r2


def decay_segment(L):
    """(start, end) tick indices of the monotone decay after the last peak of |L|, or None."""
    n = len(L)
    a = [abs(x) for x in L]
    peak = max(a, default=0.0)
    if peak < 0.2:
        return None
    ipk = max(i for i in range(n) if a[i] >= 0.97 * peak)
    j = ipk + 1
    while j < n and a[j] < a[j - 1] and a[j] > 1e-4 * peak:
        j += 1
    return (ipk + 1, j) if j - ipk >= 7 else None


def word_tau(ms, vals, seg):
    s, e = seg
    xs = vals[s:e]
    if len(xs) < 6:
        return None
    end = vals[e - 1]
    dev = [abs(v - end) for v in xs[:-1]]
    mx = max(dev, default=0.0)
    if mx <= 0:
        return None
    return decay_tau(ms[s:e - 1], dev, 1e-4 * mx)


# ------------------------------------------------------------------ analysis of one window
def analyse_window(w, log_pts=None, source="trace", max_lag=8):
    n = len(w.ticks)
    res = {"window": w.index, "ticks": n, "digital": [], "filter": [], "lead": [], "note": ""}
    if n < 12:
        res["note"] = "too few ticks"
        return res
    series = build_series(w)
    L, label = l_series(w, series, log_pts, source)
    res["l_source"] = label
    ms = [rec[2] for rec in w.ticks]
    cls = key_classes(L)
    held = sum(1 for c in cls if c)
    if held < 3:
        res["note"] = "no look episode in this window (L never moved)"
        return res
    seg = decay_segment(L)
    tauL = word_tau(ms, L, seg) if seg else None
    res["tau_L_ms"] = tauL[0] if tauL else None
    frac_held = held / float(n)
    for off, raw in series.items():
        rel = off - w.l_offset
        if rel == 0:
            continue                                 # L itself
        distinct = len(set(raw))
        # ---- (a) digital: few distinct raw values, best value->class mapping over lags
        if 2 <= distinct <= 8:
            best = None
            for k in range(-max_lag, max_lag + 1):
                lo, hi = max(0, -k), min(n, n - k)
                if hi - lo < 8:
                    continue
                table = {}
                for t in range(lo, hi):
                    table.setdefault(raw[t], {0: 0, 1: 0, -1: 0})[cls[t + k]] += 1
                tot = hi - lo
                purity = sum(max(c.values()) for c in table.values()) / float(tot)
                counts = {0: 0, 1: 0, -1: 0}
                for c in table.values():
                    for kk, vv in c.items():
                        counts[kk] += vv
                base = max(counts.values()) / float(tot)
                gain = (purity - base) / (1 - base) if base < 1 else 0.0
                if best is None or gain > best[0] + 1e-9 or (abs(gain - best[0]) <= 1e-9 and abs(k) < abs(best[1])):
                    best = (gain, k, table)
            if best and best[0] > 0.3:
                vals = sorted(set(raw))
                res["digital"].append({"rel": rel, "view": "raw", "score": best[0], "lead": best[1],
                                       "lo": vals[0], "hi": vals[-1], "distinct": distinct, "f": frac_held})
            continue
        # ---- (b) filter-like: linear in L at some lag, per view
        for vn in VIEWS:
            fn = view_fn(vn)
            v = [fn(u) for u in raw]
            if any(not math.isfinite(x) or abs(x) > 1e30 for x in v):
                continue
            if max(v) - min(v) <= 0:
                continue
            bestr = (0.0, 0)
            for k in range(-max_lag, max_lag + 1):
                xa, ya = shifted(v, L, k)
                r = pearson(xa, ya)
                if abs(r) > abs(bestr[0]) + 1e-12 or (abs(abs(r) - abs(bestr[0])) <= 1e-12 and abs(k) < abs(bestr[1])):
                    bestr = (r, k)
            if abs(bestr[0]) < 0.9:
                continue
            tau = word_tau(ms, v, seg) if seg else None
            match = None
            if tau and tauL:
                match = math.exp(-3.0 * abs(math.log(tau[0] / tauL[0])))
            same = all(abs(a - b) <= 1e-12 for a, b in zip(v, L)) if vn == "f32" else False
            score = abs(bestr[0]) * (match if match is not None else 0.7)
            res["filter"].append({"rel": rel, "view": vn, "score": score, "r": bestr[0], "lead": bestr[1], "lo": min(v), "hi": max(v),
                                  "tau_ms": tau[0] if tau else None, "tau_r2": tau[1] if tau else None, "tau_match": match,
                                  "copy": same})
    # one entry per word in the filter list: keep the best view
    best_by = {}
    for c in res["filter"]:
        if c["rel"] not in best_by or c["score"] > best_by[c["rel"]]["score"]:
            best_by[c["rel"]] = c
    res["filter"] = sorted(best_by.values(), key=lambda c: -c["score"])
    res["digital"].sort(key=lambda c: -c["score"])
    lead = [dict(c, kind="digital") for c in res["digital"] if c["lead"] >= 1]
    lead += [dict(c, kind="filter") for c in res["filter"] if c["lead"] >= 1]
    res["lead"] = sorted(lead, key=lambda c: (-c["score"], -c["lead"]))
    return res


def aggregate(results):
    """Mean score over the windows in which a word qualified, plus how many windows saw it."""
    agg = {}
    for r in results:
        for kind in ("digital", "filter"):
            for c in r[kind]:
                a = agg.setdefault((kind, c["rel"]), {"kind": kind, "rel": c["rel"], "n": 0, "score": 0.0, "lead": [], "c": c})
                a["n"] += 1
                a["score"] += c["score"]
                a["lead"].append(c["lead"])
                if c["score"] > a["c"]["score"]:
                    a["c"] = c
    out = []
    for a in agg.values():
        a["score"] /= a["n"]
        a["lead_med"] = sorted(a["lead"])[len(a["lead"]) // 2]
        out.append(a)
    return out


# ------------------------------------------------------------------ report
def rel_str(rel):
    return ("-0x%X" % -rel) if rel < 0 else ("+0x%X" % rel)


def fmt_val(c):
    if c.get("view") == "f32":
        return "%.6g..%.6g" % (c["lo"], c["hi"])
    return "%d..%d (0x%X..0x%X)" % (c["lo"], c["hi"], int(c["lo"]) & 0xFFFFFFFF, int(c["hi"]) & 0xFFFFFFFF)


def print_report(wins, results, top, verbose, out=sys.stdout):
    P = lambda s="": out.write(s + "\n")
    P("windows: %d" % len(wins))
    for w, r in zip(wins, results):
        P("  window %d: open=%s close=%s ticks=%d preroll=%d deltas=%d span=%.0f ms%s%s  [%s]" % (
            w.index, OPEN.get(w.open_reason, "?"), CLOSE.get(w.close_reason, "?"), w.tick_count, w.preroll, w.delta_count,
            w.close_ms - w.open_ms, " TRUNCATED" if w.flags & 1 else "", " L=%s" % r.get("l_source", "-"),
            r["note"] or "ok"))
        if verbose and not r["note"]:
            P("    tau(L)=%s ms" % ("%.0f" % r["tau_L_ms"] if r.get("tau_L_ms") else "n/a"))
    ok = [r for r in results if not r["note"]]
    if not ok:
        P("no analysable window (no look episode in any window)")
        return
    agg = aggregate(ok)
    P("")
    P("(a) DIGITAL: step to a constant while a key is held (gain 1 = perfect; lead k>0 = changes k ticks before L)")
    dig = sorted([a for a in agg if a["kind"] == "digital"], key=lambda a: (-a["score"], -a["n"]))[:top]
    P("  %-9s %-5s %-6s %-5s %-9s %s" % ("offset", "gain", "windows", "lead", "distinct", "values"))
    for a in dig:
        c = a["c"]
        P("  %-9s %-5.2f %2d/%-4d %-5d %-9d %s" % (rel_str(a["rel"]), a["score"], a["n"], len(ok), a["lead_med"], c["distinct"],
                                                  "0x%X..0x%X" % (int(c["lo"]) & 0xFFFFFFFF, int(c["hi"]) & 0xFFFFFFFF)))
    if not dig:
        P("  (none)")
    P("")
    P("(b) FILTER STATE: linear in L and, where measurable, the same decay time constant")
    flt = sorted([a for a in agg if a["kind"] == "filter"], key=lambda a: (-a["score"], -a["n"]))[:top]
    P("  %-9s %-5s %-6s %-5s %-6s %-7s %-9s %-9s %s" % ("offset", "score", "windows", "lead", "view", "r", "tau(ms)", "tauL(ms)", "range"))
    tauLs = [r["tau_L_ms"] for r in ok if r.get("tau_L_ms")]
    tauL = sum(tauLs) / len(tauLs) if tauLs else None
    for a in flt:
        c = a["c"]
        P("  %-9s %-5.2f %2d/%-4d %-5d %-6s %-7.3f %-9s %-9s %s%s" % (
            rel_str(a["rel"]), a["score"], a["n"], len(ok), a["lead_med"], c["view"], c["r"],
            "%.0f" % c["tau_ms"] if c["tau_ms"] else "n/a", "%.0f" % tauL if tauL else "n/a", fmt_val(c),
            "  [identical to L: an output copy]" if c["copy"] else ""))
    if not flt:
        P("  (none)")
    P("")
    P("(c) LEADING L by >= 1 tick (digital or filter candidates)")
    lead = sorted([a for a in agg if a["lead_med"] >= 1], key=lambda a: (-a["score"], -a["lead_med"]))[:top]
    for a in lead:
        P("  %-9s %-8s lead=%d ticks  score=%.2f  windows=%d/%d  %s" % (
            rel_str(a["rel"]), a["kind"], a["lead_med"], a["score"], a["n"], len(ok), fmt_val(dict(a["c"], view=a["c"].get("view", "raw")))))
    if not lead:
        P("  (none)")
    P("")
    P("note: words that are exact copies of L (lag 0, e.g. the composed-camera copy at +0x484) are outputs, not inputs.")


# ------------------------------------------------------------------ synthetic generator (also used by --selftest)
class Synth:
    """Fake game: digital key flag + target word -> first-order filter state -> L (one tick later), plus noise words.
    Planted offsets (relative to the look yaw): FLAG +0x100 (u32 0/1/2 = none/left/right), TARGET +0x104 (float),
    STATE -0x200 (float), LCOPY +0x484 (float, = L), TIMER +0x400 (counter), NOISE +0x800..+0x83C (random u32),
    SINE +0x900 (slow float), CONST +0xA00 (never changes)."""
    FLAG, TARGET, STATE, LCOPY, TIMER, SINE, CONST = 0x100, 0x104, -0x200, 0x484, 0x400, 0x900, 0xA00

    def __init__(self, seed=1):
        self.rng = random.Random(seed)

    def windows(self, holds, hz=60.0, pre_ms=1000, win_ms=3000, noisy=16):
        """holds: list of (start_tick, hold_ticks, direction). Returns a list of windows' raw tick lists + a log."""
        total = max(s + h for s, h, _ in holds) + int(win_ms * hz / 1000) + 120
        alpha, sat = 0.3, 2.48
        state = 0.0
        mem = {}
        key = [0] * total
        for s, h, d in holds:
            for t in range(s, min(total, s + h)):
                key[t] = d
        ticks = []
        timer = 0
        L = 0.0
        for t in range(total):
            tgt = {0: 0.0, 1: sat, -1: -sat}[key[t]]
            Lprev_state = state
            state += (tgt - state) * alpha
            L = u2f(struct.unpack("<I", struct.pack("<f", Lprev_state))[0])   # L = state one tick earlier
            timer += 1
            cur = {self.FLAG: {0: 0, 1: 1, -1: 2}[key[t]], self.TARGET: struct.unpack("<I", struct.pack("<f", tgt))[0],
                   self.STATE: struct.unpack("<I", struct.pack("<f", state))[0], 0: struct.unpack("<I", struct.pack("<f", L))[0],
                   self.LCOPY: struct.unpack("<I", struct.pack("<f", L))[0], self.TIMER: timer,
                   self.SINE: struct.unpack("<I", struct.pack("<f", math.sin(t / 37.0)))[0], self.CONST: 0xC0FFEE}
            for i in range(noisy):
                cur[0x800 + 4 * i] = self.rng.getrandbits(32)
            ticks.append((t, 1000.0 + t * 1000.0 / hz, cur))
        return ticks, key

    def build(self, holds, hz=60.0, pre_ms=1000, win_ms=3000):
        """Bytes of a trace file with one window per hold (+pre-roll), plus the log text of the L series."""
        ticks, key = self.windows(holds, hz)
        out, log = bytearray(), []
        idx = 0
        for wi, (s, h, d) in enumerate(holds):
            t0 = s - 1                                  # the tick before the key press is the trigger candidate
            first = max(1, int(round(t0 - pre_ms * hz / 1000.0)))
            last = min(len(ticks) - 1, t0 + int(round(win_ms * hz / 1000.0)))
            body, deltas = bytearray(), 0
            for t in range(first, last + 1):
                prev, cur = ticks[t - 1][2], ticks[t][2]
                ds = [(off, prev[off], cur[off]) for off in sorted(cur) if cur[off] != prev[off]]
                body += TICK.pack(t, 4242, ticks[t][1], t + 1, len(ds), len(ds))
                for off, o, n in ds:
                    body += DELTA.pack(off + 0x8000, o, n)
                deltas += len(ds)
            out += HDR.pack(MAGIC, 1, 96, wi, 1, 0x7000000000, 0x7000000000 + 0xFDD410 - 0x8000, 65536, 0x8000, last - first + 1,
                            t0 - first, deltas, len(body), 0, 0, ticks[t0][1], ticks[last][1], 1, t0)
            out += body
        for t, (_, ms, cur) in enumerate(ticks):
            if t % 15 == 0:                              # the real log samples every ~250 ms
                log.append("%.3f|ee-camera;event=timeline;slot=0;base=7000000000;src=best0;chg=0000;ev=-;L=0,%.6g,0;H=-0.785398;air=0,0,0" % (
                    ms, u2f(cur[0])))
        return bytes(out), "\n".join(log) + "\n"


def generate(path, seed=1):
    data, log = Synth(seed).build([(120, 42, 1), (420, 42, -1), (720, 42, 1)])
    with open(path, "wb") as fh:
        fh.write(data)
    with open(path + ".log", "w") as fh:
        fh.write(log)
    return path


# ------------------------------------------------------------------ self test
def selftest():
    fails = []

    def check(c, msg):
        if not c:
            fails.append(msg)

    S = Synth(7)
    data, log = S.build([(120, 42, 1), (420, 42, -1), (720, 42, 1)])
    wins = parse_trace(data)
    check(len(wins) == 3, "3 windows parsed")
    check(all(w.tick_count == len(w.ticks) for w in wins), "tick counts")
    check(all(w.preroll > 50 for w in wins), "pre-roll present")
    # words that never change are absent; planted ones present
    offs = set()
    for w in wins:
        for rec in w.ticks:
            for off, _, _ in rec[5]:
                offs.add(off - w.l_offset)
    check(Synth.CONST not in offs, "constant word absent")
    check({Synth.FLAG, Synth.TARGET, Synth.STATE, 0, Synth.TIMER} <= offs, "planted words present")
    # truncated tail tolerated
    check(len(parse_trace(data[:-20])) == 2, "truncated last window dropped")
    try:
        parse_trace(b"XXXXXXXX" + data[8:])
        check(False, "bad magic rejected")
    except ValueError:
        pass
    results = [analyse_window(w) for w in wins]
    check(all(not r["note"] for r in results), "all windows have a look episode")
    agg = aggregate(results)
    dig = sorted([a for a in agg if a["kind"] == "digital"], key=lambda a: -a["score"])
    flt = sorted([a for a in agg if a["kind"] == "filter"], key=lambda a: -a["score"])
    dig_offs = [a["rel"] for a in dig[:3]]
    check(Synth.FLAG in dig_offs, "digital flag ranked top-3: %s" % [rel_str(x) for x in dig_offs])
    fa = [a for a in dig if a["rel"] == Synth.FLAG]
    check(fa and fa[0]["score"] > 0.9 and fa[0]["lead_med"] == 1, "flag gain>0.9 and leads L by 1 tick: %s" % str(fa and (fa[0]["score"], fa[0]["lead_med"])))
    flt_offs = [a["rel"] for a in flt[:3]]
    check(Synth.STATE in flt_offs, "filter state in top-3: %s" % [rel_str(x) for x in flt_offs])
    st = [a for a in flt if a["rel"] == Synth.STATE]
    check(st and st[0]["lead_med"] == 1 and abs(st[0]["c"]["r"]) > 0.99, "state leads L by 1 with r~1")
    check(st and st[0]["c"]["tau_ms"], "state tau measured")
    if st and st[0]["c"]["tau_ms"]:
        check(abs(st[0]["c"]["tau_match"] or 0) > 0.8, "state tau matches L")
    cp = [a for a in flt if a["rel"] == Synth.LCOPY]
    check(cp and cp[0]["c"]["copy"] and cp[0]["lead_med"] == 0, "L copy flagged as identical output")
    bad = {Synth.TIMER, Synth.CONST, Synth.SINE} | {0x800 + 4 * i for i in range(16)}
    check(not [a for a in dig[:3] if a["rel"] in bad], "no noise word in the digital top-3")
    check(not [a for a in flt[:2] if a["rel"] in bad], "no noise word in the filter top-2")
    lead_offs = [a["rel"] for a in agg if a["lead_med"] >= 1]
    check(Synth.FLAG in lead_offs and Synth.STATE in lead_offs, "(c) lists flag and state")
    # direction coding: the 0/1/2 flag separates left from right (class mapping is value -> {0,+1,-1})
    # log-sourced L gives the same top digital candidate (coarse 250 ms samples)
    with tempfile.TemporaryDirectory() as d:
        lp = os.path.join(d, "ee-camera.log")
        with open(lp, "w") as fh:
            fh.write(log)
        pts = load_log_L(lp, 0x7000000000)
        check(len(pts) > 20, "log L parsed")
        r2 = [analyse_window(w, pts, "log") for w in wins]
        check(all(r.get("l_source") == "log" for r in r2), "log source used")
        ag2 = aggregate([r for r in r2 if not r["note"]])
        d2 = sorted([a for a in ag2 if a["kind"] == "digital"], key=lambda a: -a["score"])
        check(d2 and Synth.FLAG in [a["rel"] for a in d2[:3]], "flag found with the log L series too")
        # generator + CLI round trip
        gp = os.path.join(d, "t.bin")
        generate(gp, 3)
        check(len(load_trace(gp)) == 3, "generate/load round trip")
    # a window without any L movement is reported, not crashed on
    flat, _ = Synth(2).build([(120, 0, 1)])
    r3 = analyse_window(parse_trace(flat)[0])
    check(r3["note"] != "", "idle window reported")
    # report prints
    import io
    buf = io.StringIO()
    print_report(wins, results, 5, True, buf)
    check("(a) DIGITAL" in buf.getvalue() and "-0x200" in buf.getvalue() and "+0x100" in buf.getvalue(), "report text")
    if fails:
        for f in fails:
            print("SELFTEST FAIL: " + f)
        return 1
    print("look_input_trace selftest: ok")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description="Rank words of a look-input trace against the look value L.")
    ap.add_argument("trace", nargs="?")
    ap.add_argument("--log", help="ee-camera.log (timeline lines carry L)")
    ap.add_argument("--l-source", choices=("trace", "log"), default="trace")
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--max-lag", type=int, default=8)
    ap.add_argument("--window", type=int, help="analyse only this window index")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--generate", metavar="OUT", help="write a synthetic trace (and OUT.log)")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if a.generate:
        print(generate(a.generate))
        return 0
    if not a.trace:
        ap.error("TRACE.bin required")
    wins = load_trace(a.trace)
    if a.window is not None:
        wins = [w for w in wins if w.index == a.window]
    if not wins:
        print("no windows in the trace")
        return 1
    results = []
    for w in wins:
        pts = load_log_L(a.log, w.base) if a.log else None
        results.append(analyse_window(w, pts, a.l_source, a.max_lag))
    print_report(wins, results, a.top, a.verbose)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
