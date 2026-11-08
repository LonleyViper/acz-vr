"""Offline check of the E6 pose convention from an ee-camera.log (timeline slot with src=best2 / E6-like rows).

campos = air + F*fwd + K*up_actual holds for the cockpit camera (F ~36.3, K ~3.57 units). Project (campos-air) on the camera_frame axes
(roll 0 axes: fwd from pitch/heading, right0 = fwd x worldUp, up0 = right0 x fwd) and fit  right = kR*sin(roll), up = kU*cos(roll).
kR < 0 => positive E6 roll tilts the aircraft's up toward the LEFT, i.e. terrain_stage.cpp camera_frame needs roll_sign = -1.
Usage: python -I e6_roll_sign_check.py <ee-camera.log>
"""
import math, re, sys
rows = []
for l in open(sys.argv[1], errors='ignore'):
    if 'event=timeline;slot=2;' not in l:
        continue
    m = re.search(r'air=([^;]+);cam=([^;]+);rot=([^;]+);', l)
    hh = re.search(r'H=([^;]+);', l)
    a, c, r = ([float(x) for x in g.split(',')] for g in m.groups())
    if abs(a[0]) > 4e4 or a[0] > 0 or abs(r[1] - float(hh.group(1))) > 1e-3:   # mission samples only, heading == H (no game look)
        continue
    rows.append((a, c, r))
S = []
for a, c, (p, h, ro) in rows:
    f = (-math.sin(h) * math.cos(p), math.sin(p), -math.cos(h) * math.cos(p))
    rx, rz = -f[2], f[0]; n = math.hypot(rx, rz); r0 = (rx / n, 0.0, rz / n)
    u0 = (r0[1] * f[2] - r0[2] * f[1], r0[2] * f[0] - r0[0] * f[2], r0[0] * f[1] - r0[1] * f[0])
    o = [c[i] - a[i] for i in range(3)]
    d = lambda x, y: sum(x[i] * y[i] for i in range(3))
    S.append((ro, d(o, f), d(o, r0), d(o, u0)))
kR = sum(s[2] * math.sin(s[0]) for s in S) / sum(math.sin(s[0]) ** 2 for s in S)
kU = sum(s[3] * math.cos(s[0]) for s in S) / sum(math.cos(s[0]) ** 2 for s in S)
print("samples %d  F range %.2f..%.2f  kR(right per sin roll) %.3f  kU(up per cos roll) %.3f" % (len(S), min(s[1] for s in S), max(s[1] for s in S), kR, kU))
print("=> roll_sign must be %s" % ("-1 (E6 roll>0 tilts up toward the left)" if kR < 0 else "+1"))
