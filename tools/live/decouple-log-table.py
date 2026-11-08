# DECOUPLE (docs/world-render/decouple.md section 7): one markdown row per 5 s summary window of an xr-stereo.log (read-only), so a run with
# decouple:1 can be compared with a run without it. Usage:
#   python -I tools/live/decouple-log-table.py "label=<dir with xr-stereo.log>" ["label2=<dir>" ...]
#   python -I tools/live/decouple-log-table.py --selftest
# Columns per window: composites/s and fresh (= new game frames)/s from summary-decouple (decouple:1 only), new/s from summary-selection (both),
# re-composed, reuse fallbacks (old re-submit path), slot releases by reason, re-projection counts, camera dead reckoning (extrapolated / held,
# mean horizon), held-frame age max, plus summary/summary-timing: submitted, reused, xr-period mean/max, end-frame max, completion mean/max,
# frame-cpu max, gpu-blit ms. The last row per run is the mean over the windows after the first 15 s (the skip-intro clock).
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


def num(s):
    try:
        return float(re.match(r'-?[\d.]+', s).group(0))
    except Exception:
        return None


def windows(lines):
    """Groups the summary lines into windows: a 'event=summary;' line opens a window; the following summary-* lines belong to it."""
    out, cur = [], None
    for ln in lines:
        m = re.search(r'event=([a-z0-9-]+)', ln)
        if not m:
            continue
        ev = m.group(1)
        if ev == 'summary':
            cur = {'summary': kv(ln[m.start():])}
            out.append(cur)
        elif cur is not None and ev in ('summary-decouple', 'summary-selection', 'summary-timing'):
            d = kv(ln[m.start():])
            key = ev
            if ev == 'summary-timing' and 'gpu-blit-ms' in d:
                key = 'summary-timing2'
            cur[key] = d
    return out


COLS = ['t-s', 'comp/s', 'fresh/s', 'new/s', 'recomp', 'fallback', 'rel(sup/stall/space)', 'reproj(src/win)', 'cam(ext/held)', 'hor-ms', 'age-max',
        'submitted', 'reused', 'xr-period', 'end-max', 'compl(mean/max)', 'cpu-max', 'gpu-blit']


def row(w, t0):
    s, dc, sel, t1, t2 = w.get('summary', {}), w.get('summary-decouple', {}), w.get('summary-selection', {}), w.get('summary-timing', {}), w.get('summary-timing2', {})
    t = num(s.get('t-ms', '0')) or 0
    rel = dc.get('released', '')
    m = re.match(r'(\d+)\(superseded:(\d+),stall:(\d+),space:(\d+)', rel)
    return [
        '%.0f' % ((t - t0) / 1000.0), dc.get('composites-per-s', '-'), dc.get('new-per-s', '-'), sel.get('new-per-s', '-'), dc.get('recomposed', '-'),
        dc.get('reuse-fallback', '-'), ('%s/%s/%s' % (m.group(2), m.group(3), m.group(4))) if m else '-',
        ('%s/%s' % (dc.get('reproj-source', '-'), dc.get('reproj-window', '-'))) if dc else '-',
        ('%s/%s' % (dc.get('cam-extrap', '-'), dc.get('cam-held', '-'))) if dc else '-', dc.get('horizon-ms-mean', '-'), dc.get('held-age-ms-max', '-'),
        s.get('submitted', '-'), s.get('reused', '-'), ('%s/%s' % (t2.get('xr-period-ms-mean', '-'), t2.get('xr-period-ms-max', '-'))),
        t1.get('end-frame-ms-max', '-'), ('%s/%s' % (t1.get('completion-ms-mean', '-'), t1.get('completion-ms-max', '-'))), t2.get('frame-cpu-ms-max', '-'),
        t2.get('gpu-blit-ms', '-')]


def table(label, lines):
    ws = windows(lines)
    out = ['### ' + label, '', '| ' + ' | '.join(COLS) + ' |', '|' + '---|' * len(COLS)]
    if not ws:
        out.append('| (no summary lines) |')
        return out
    t0 = num(ws[0]['summary'].get('t-ms', '0')) or 0
    body = [row(w, t0) for w in ws]
    out += ['| ' + ' | '.join(r) + ' |' for r in body]
    late = [r for r in body if num(r[0]) is not None and num(r[0]) >= 15]
    if late:
        def mean_col(i):
            xs = [num(r[i]) for r in late if num(r[i]) is not None]
            return ('%.2f' % (sum(xs) / len(xs))) if xs else '-'
        out.append('| mean>=15s | ' + ' | '.join(mean_col(i) for i in range(1, len(COLS))) + ' |')
    return out


def selftest():
    sample = [
        '1|xr-stereo;event=summary;t-ms=1000;frames=450;submitted=450;new=300;reused=0;reuse-age-max-ms=0',
        '1|xr-stereo;event=summary-selection;select-lag=0;select-age-ms=12.0;new-per-s=60.0',
        '1|xr-stereo;event=summary-decouple;t-ms=1000;window-ms=5000;composites=450;composites-per-s=90.0;fresh=300;new-per-s=60.0;recomposed=150;'
        'reuse-fallback=0;retained=300;not-retained=0;released=300(superseded:300,stall:0,space:0,retire:0);untracked=0;no-views=0;reproj-source=450;'
        'reproj-window=150;reproj-identity=0;reproj-refused=0;cam-extrap=440;cam-held=10;cam-none=0;horizon-ms-mean=18.5;held-age-ms-max=22.0;'
        'cam-delta-deg-max=0.40;holding=yes',
        '1|xr-stereo;event=summary-timing;t-ms=1000;gpu-waits=0;cpu-presync=300;fence-lag-max=1;completion-ms-max=3;completion-ms-mean=2;'
        'wait-frame-ms-max=11;end-frame-ms-max=4;end-failed=0',
        '1|xr-stereo;event=summary-timing;gpu-start-ms=0.5;gpu-blit-ms=0.9;end-late-ms-max=unavailable;xr-period-ms-mean=11.1;xr-period-ms-max=11.2;'
        'frame-cpu-ms-max=6;blocked-ms-max=3',
        '2|xr-stereo;event=summary;t-ms=21000;frames=450;submitted=450;new=300;reused=0',
        '2|xr-stereo;event=summary-decouple;t-ms=21000;composites=450;composites-per-s=90.0;fresh=300;new-per-s=60.0;recomposed=150;reuse-fallback=0;'
        'released=300(superseded:300,stall:0,space:0,retire:0)',
    ]
    t = table('selftest', sample)
    assert any('| 0 | 90.0 | 60.0 | 60.0 | 150 | 0 | 300/0/0 | 450/150 | 440/10 | 18.5 | 22.0 | 450 | 0 | 11.1/11.2 | 4 | 2/3 | 6 | 0.9 |' in x for x in t), t
    assert any(x.startswith('| mean>=15s | 90.00 | 60.00 |') for x in t), t
    print('decouple-log-table selftest: OK')


def main(argv):
    if len(argv) == 2 and argv[1] == '--selftest':
        selftest()
        return 0
    if len(argv) < 2:
        print(__doc__ or 'usage: decouple-log-table.py label=<dir> ...')
        return 2
    for a in argv[1:]:
        label, _, d = a.partition('=')
        path = os.path.join(d, 'xr-stereo.log')
        try:
            with open(path, 'r', encoding='utf-8', errors='replace') as fh:
                lines = fh.readlines()
        except OSError as e:
            print('### %s: cannot read %s (%s)' % (label, path, e))
            continue
        print('\n'.join(table(label, lines)))
        print()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
