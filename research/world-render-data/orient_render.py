"""Render m02a at the capture pose for candidate conventions + the screenshot crop, as one contact sheet (offline).
python -I orient_render.py <out.png> [shift_u=0]"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np, terrain_render as tr, terrain_calibrate as tc
from terrain_lib import Stage, write_png
SHOT = os.environ.get('ACZ_SHOT', '<repo>/research/experiments/step54-terrain-run3/game-window-shots/acz-20261026-222415.png')
POSE = np.array([-6987.0, 114648.0]); Y = 3879.5
out = sys.argv[1]; shift = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
st = Stage('m02a'); W = int(os.environ.get("W","640"))
eye = POSE + shift * np.array([0.7071, -0.7071])
shot = tc.read_png(SHOT)[31:]                      # drop title bar -> 2421x1361
tiles = []
def crop(img):                                     # lower half (ground strip) of an upright 16:9 image
    return img[int(img.shape[0]*0.46):int(img.shape[0]*0.92)]
sh = tc.resize(shot, W, W*9//16); tiles.append(crop(sh))
for name, off, hand in (('RH+0 (shipped default)', 0, -1), ('mirror+0 (mirror:1)', 0, 1), ('RH+90', 90, -1), ('mirror+90', 90, 1),
                        ('RH+180', 180, -1), ('mirror+180', 180, 1), ('RH+270', 270, -1), ('mirror+270', 270, 1)):
    tc.patch_camera(1, 135 + off, hand)
    img, _ = tr.render(st, (eye[0], Y, eye[1]), 0.0, 0.0, W, 63.2, 38.1, 64, False)
    tiles.append(crop(img)); print(name, flush=True)
N = int(os.environ.get('COLS', '3'))
h0 = min(t.shape[0] for t in tiles); tiles = [t[:h0] for t in tiles]
tiles += [np.zeros_like(tiles[0])] * (-len(tiles) % N)
rows = [np.concatenate(tiles[i:i + N], 1) for i in range(0, len(tiles), N)]
write_png(out, np.concatenate(rows, 0))
