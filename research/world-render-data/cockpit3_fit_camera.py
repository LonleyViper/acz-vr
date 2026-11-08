"""Fit the step62 eye-0 camera of the model cockpit by chamfer matching: silhouette edges of CPU renders (null FOV +-45 deg, glass off, 2400x1350/S)
against the edge map of the XR dump. Score = mean truncated distance (px at 1/S scale) of the render's silhouette edges to the nearest XR edge."""
import sys, os, subprocess, itertools, numpy as np
from scipy import ndimage
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_io import read_png, write_png
EXE = r'D:\b-c3\cockpit_model_offline_tests.exe'
Z = os.environ.get('ZVRM', r'D:\acz-extract\models\j35jk_vrcockpit.zvrm')   # the COCKPIT-3 (RGBA CLUT) bake
TMP = os.environ.get('OUT', r'D:\acz-extract\out\cockpit3')   # game-derived outputs stay outside the repo
os.makedirs(TMP, exist_ok=True)
S = 3
W, H = 2400 // S, 1350 // S
TAN = os.environ.get('TAN', '1 1 1 1').split()
REF = os.environ.get('REF', r'<repo>\research\experiments\step62-integrated-run1\xr-f403-final-upright-2400.png')
CROP = [int(v) for v in os.environ.get('CROP', '0 0').split()]
fin = np.asarray(read_png(REF))[..., :3].astype(float)
fin = fin[CROP[1]:CROP[1] + 1350, CROP[0]:CROP[0] + 2400]
g = fin.mean(2)[:H * S, :W * S].reshape(H, S, W, S).mean((1, 3))
mag = np.hypot(ndimage.sobel(g, 0), ndimage.sobel(g, 1))
xedge = mag > float(os.environ.get('ETH', '60'))
dist = ndimage.distance_transform_edt(~xedge)
write_png(TMP + r'\xredge.png', (np.stack([xedge] * 3, 2) * 255).astype(np.uint8))

def render(dx, dy, dz, pitch, yaw=0.0, extra=''):
    out = TMP + r'\r.png'
    e = 'eye:%.3f,%.3f,%.3f;glass:0;floor:0%s' % (dx, 3.575 + dy, -36.3 + dz, extra)
    subprocess.run([EXE, '--load', Z, '--png', out, str(yaw), str(pitch), '0', '--tan', *TAN, '--size', str(W), str(H), '--opts', e],
                   check=True, capture_output=True)
    r = np.asarray(read_png(out))[..., :3].astype(int)
    bg = np.array([round(255 * (1.055 * 0.6 ** (1 / 2.4) - 0.055))] * 2 + [round(255 * (1.055 * 0.75 ** (1 / 2.4) - 0.055))])
    cov = np.abs(r - bg).sum(2) > 6
    edge = cov ^ ndimage.binary_erosion(cov)
    edge[:2] = edge[-2:] = False; edge[:, :2] = edge[:, -2:] = False
    return cov, edge

def score(edge):
    d = np.minimum(dist[edge], 8.0)
    return d.mean()

grid = dict(dx=[0.0], dy=[0.0], dz=[0.0], pitch=[0.0], yaw=[0.0])
for a in sys.argv[1:]:
    k, v = a.split('=')
    grid[k] = [float(t) for t in v.split(',')]
res = []
for dx, dy, dz, p, yw in itertools.product(grid['dx'], grid['dy'], grid['dz'], grid['pitch'], grid['yaw']):
    cov, e = render(dx, dy, dz, p, yw)
    res.append((score(e), dx, dy, dz, p, yw))
res.sort()
for b in res[:6]:
    print('score %.3f  dx %.3f dy %.3f dz %.3f pitch %.2f yaw %.2f' % b)
if os.environ.get('SAVE'):
    b = res[0]; cov, e = render(*b[1:5], b[5])
    o = np.stack([g] * 3, 2) * 0.7; o[xedge] = [0, 200, 255]; o[e] = [255, 0, 0]
    write_png(os.environ['SAVE'], o.clip(0, 255).astype(np.uint8))
