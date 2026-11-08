# FPS investigation (docs/world-render/fps-investigation.md): tabulate the producer's summary-producer-cost lines (perf_counters.h, step 80+)
# from one or more step5-producer.log files, next to the consumer's new frames/s from the same folder's xr-stereo.log (read-only). Usage:
#   python -I tools/live/fps-cost-table.py "A=<log dir of the current-config run>" "B=<log dir of the -Lean run>"
# One row per 5 s window: t (s from the first cost line), game fps (frames/window), hooks us/frame (sum of the hook categories), then
# us/frame per category (setter addr draw step5 draw-x native exec descr ee-pres ee-seam log flush; addr is nested in setter/descr, flush in
# exec, and native is the game's own driver cost: none of the three is in the sum), registry resources/heaps. A last row per run gives the means.
# FPS growth (docs/world-render/fps-investigation.md section 7): rel (releaseMetadata), cert (Execute list re-certification) and pscan (remaining
# HookState::pipelines walks) are nested/other-thread categories, also not in the sum; `objects` = live PSO records and tracked lists.
# Older logs without these fields show nan / an empty objects column.
import os
import re
import sys

CATS = ['setter', 'addr', 'draw', 'step5', 'draw-x', 'native', 'exec', 'descr', 'ee-pres', 'ee-seam', 'log', 'flush', 'rel', 'cert', 'pscan']


def parse(line):
    d = {}
    for part in line.rstrip('\n').split(';'):
        if '=' in part:
            k, v = part.split('=', 1)
            d[k] = v
    return d


def rows(d):
    p = os.path.join(d, 'step5-producer.log')
    out = []
    if not os.path.exists(p):
        return out
    t0 = None
    for line in open(p, errors='replace'):
        if 'event=summary-producer-cost' not in line:
            continue
        m = re.match(r'^(\d+)\|', line)
        t = int(m.group(1)) if m else 0
        k = parse(line)
        if k.get('part', '1') == '2':   # the second line of the same window: merge its categories
            if out:
                for c in CATS:
                    if c in k:
                        out[-1][c] = float(k[c].split('/')[0])
                if 'objects' in k:
                    out[-1]['registry'] += ';' + k['objects']
            continue
        if t0 is None:
            t0 = t
        r = {'t': (t - t0) / 1000.0, 'fps': float(k.get('fps', 'nan')), 'hooks': float(k.get('hooks-us-per-frame', 'nan')),
             'registry': k.get('registry', '')}
        for c in CATS:
            v = k.get(c, '')
            r[c] = float(v.split('/')[0]) if v else float('nan')
        out.append(r)
    return out


if __name__ == '__main__':
    print('| run | t s | fps | hooks us/frame | ' + ' | '.join(CATS) + ' | registry |')
    print('|---|' + '---|' * (len(CATS) + 4))
    for arg in sys.argv[1:]:
        label, d = arg.split('=', 1)
        rs = rows(d)
        for r in rs:
            print('| %s | %.0f | %.1f | %.0f | ' % (label, r['t'], r['fps'], r['hooks']) + ' | '.join('%.0f' % r[c] for c in CATS) + ' | %s |' % r['registry'])
        if rs:
            n = len(rs)
            print('| %s mean | - | %.1f | %.0f | ' % (label, sum(r['fps'] for r in rs) / n, sum(r['hooks'] for r in rs) / n) +
                  ' | '.join('%.0f' % (sum(r[c] for r in rs) / n) for c in CATS) + ' | - |')
