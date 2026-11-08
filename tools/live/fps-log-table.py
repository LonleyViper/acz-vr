# FPS investigation (docs/world-render/fps-investigation.md): one markdown table row per run from its xr-stereo.log, step5-producer.log
# and ee-camera.log (read-only). Usage:
#   python -I tools/live/fps-log-table.py "label=<dir with the logs>" ["label2=<dir>" ...]
# Columns: XR seconds, consumer new frames/s (first 20 s / last 30 s), first time new/s < 40, producer published/s (= game frames, since
# skipped stays ~0) first/last, cumulative avg-commands, skipped, ee-camera size and rate, step5+xr log MB, consumer blit ms and frame-cpu-max.
# The per-window cost breakdown (summary-producer-cost, step 80+) is tabulated by tools/live/fps-cost-table.py.
import os
import re
import sys


def kv(line):
    d = {}
    for part in line.rstrip('\n').split(';'):
        if '=' in part:
            k, v = part.split('=', 1)
            d[k] = v
    return d


def ts(line):
    m = re.match(r'^(\d+)(?:\.\d+)?\|', line)
    return int(m.group(1)) if m else None


def f(s):
    try:
        return float(re.match(r'-?[\d.]+', s).group(0))
    except Exception:
        return None


def mean(xs):
    xs = [x for x in xs if x is not None]
    return sum(xs) / len(xs) if xs else float('nan')


def run(label, d):
    xr = os.path.join(d, 'xr-stereo.log')
    sp = os.path.join(d, 'step5-producer.log')
    ee = os.path.join(d, 'ee-camera.log')
    if not os.path.exists(xr):
        return
    sel, tim = [], []
    for line in open(xr, errors='replace'):
        if 'event=summary-selection' in line:
            sel.append((ts(line), f(kv(line).get('new-per-s', '0'))))
        elif 'event=summary-timing;gpu-start' in line:
            k = kv(line)
            tim.append((f(k.get('gpu-blit-ms', '')), f(k.get('frame-cpu-ms-max', '')), f(k.get('blocked-ms-max', ''))))
    sel = [s for s in sel if s[1] and s[1] > 1]
    if len(sel) < 3:
        return
    prod = []
    if os.path.exists(sp):
        for line in open(sp, errors='replace'):
            if 'event=continuous;' in line:
                k = kv(line)
                prod.append((ts(line), int(k.get('published', 0)), f(k.get('avg-commands', '0')), f(k.get('avg-members', '0')),
                             int(k.get('member-refused', '0')), int(k.get('skipped', '0'))))
    dur = (sel[-1][0] - sel[0][0]) / 1000.0 + 5
    first = mean([s[1] for s in sel[1:5]])
    last = mean([s[1] for s in sel[-6:]])

    def pubrate(a, b):
        dt = (b[0] - a[0]) / 1000.0
        return (b[1] - a[1]) / dt if dt > 0 else float('nan')
    pf = pl = cm = float('nan')
    skipped = 0
    if len(prod) >= 6:
        pf = pubrate(prod[1], prod[4])
        pl = pubrate(prod[-5], prod[-1])
        cm = prod[-1][2]
        skipped = prod[-1][5]
    eesz = os.path.getsize(ee) / 1e6 if os.path.exists(ee) else 0
    spsz = os.path.getsize(sp) / 1e6 if os.path.exists(sp) else 0
    xrsz = os.path.getsize(xr) / 1e6
    blit = mean([t[0] for t in tim[-6:]]) if tim else float('nan')
    cpu = mean([t[1] for t in tim[-6:]]) if tim else float('nan')
    t40 = next((round((s[0] - sel[0][0]) / 1000.0) for s in sel[2:] if s[1] < 40), None)
    print('| %s | %.0f | %.0f | %.0f | %s | %.0f | %.0f | %.0f | %d | %.2f | %.2f | %.2f | %.2f | %.0f |' % (
        label, dur, first, last, t40 if t40 is not None else '-', pf, pl, cm, skipped, eesz, eesz / dur * 1000 if dur else 0,
        spsz + xrsz, blit, cpu))


if __name__ == '__main__':
    print('| run | xr s | new/s first 20 s | new/s last 30 s | t(<40/s) s | prod pub/s first | prod pub/s last | avg-cmds | skipped |'
          ' ee-camera MB | ee KB/s | step5+xr MB | blit ms (last) | xr frame-cpu-max ms (last) |')
    print('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    for arg in sys.argv[1:]:
        label, d = arg.split('=', 1)
        run(label, d)
