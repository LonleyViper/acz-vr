"""Colour fit of the model cockpit against the game window (step62 T6.75, client crop 10,31, game lens), display domain:
game ~= a * F + b * Lm  (F = tex*vcol, render amb:1 gain:1; Lm = tex*vcol*lambert, render amb:0 gain:1) over interior cockpit pixels (HUD green and glass
excluded) -> gain = a + b, ambient share = a / (a + b). Also reports per-region medians before (COCKPIT-2 path) / after."""
import sys, os, subprocess, numpy as np
from scipy import ndimage
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_io import read_png, write_png
EXE = r'D:\b-c3\cockpit_model_offline_tests.exe'
Z = os.environ.get('ZVRM', r'D:\acz-extract\models\j35jk_vrcockpit.zvrm')   # the COCKPIT-3 (RGBA CLUT) bake
TMP = os.environ.get('OUT', r'D:\acz-extract\out\cockpit3')   # game-derived outputs stay outside the repo
G = r'<repo>\research\experiments\step62-integrated-run1\shots\acz-20261027-004711-116-T6.75.png'
LIGHT = ['0.640', '0.766', '-0.056']   # m02a sun (elev 50, heading -130) in the cockpit frame at E6 heading -45, level (see cockpit-render-impl.md 10)
game = np.asarray(read_png(G))[31:31 + 1350, 10:10 + 2400, :3].astype(float)

def render(opts, z=Z, light=True, name='c.png', tint=None):
    out = TMP + '\\' + name
    cmd = [EXE, '--load', z, '--png', out, '0', '0', '0', '--tan', '0.6152', '0.6152', '0.3460', '0.3460', '--size', '2400', '1350', '--opts', opts]
    if light: cmd += ['--light', *LIGHT]
    if tint: cmd += ['--tint', *tint]
    subprocess.run(cmd, check=True, capture_output=True)
    return np.asarray(read_png(out))[..., :3].astype(float)

bg = np.array([round(255 * (1.055 * 0.6 ** (1 / 2.4) - 0.055))] * 2 + [round(255 * (1.055 * 0.75 ** (1 / 2.4) - 0.055))])
F = render('glass:0;floor:0;amb:1;gain:1')
L = render('glass:0;floor:0;amb:0;gain:1')
cov = np.abs(F - bg).sum(2) > 6
cov = ndimage.binary_erosion(cov, iterations=3)
hud = (game[..., 1] > game[..., 0] + 30) | (game.max(2) > 120)   # HUD symbology / bright text
hud = ndimage.binary_dilation(hud, iterations=4)
m = cov & ~hud
print('pixels used %d (%.1f%% of the frame)' % (m.sum(), 100 * m.mean()))
res = {}
for c, nm in enumerate('RGB'):
    A = np.stack([F[..., c][m], L[..., c][m]], 1); y = game[..., c][m]
    ab, *_ = np.linalg.lstsq(A, y, rcond=None)
    res[nm] = ab
    print('%s: a=%.3f b=%.3f gain=%.3f amb=%.3f' % (nm, ab[0], ab[1], ab.sum(), ab[0] / ab.sum()))
A = np.concatenate([np.stack([F[..., c][m], L[..., c][m]], 1) for c in range(3)]); y = np.concatenate([game[..., c][m] for c in range(3)])
ab, *_ = np.linalg.lstsq(A, y, rcond=None)
gain, amb = ab.sum(), ab[0] / ab.sum()
print('ALL: gain=%.3f amb=%.3f' % (gain, amb))
fit = ab[0] * F + ab[1] * L
print('residual mean abs (display levels): fit %.2f' % np.abs(fit[m] - game[m]).mean())
new = render('glass:0;floor:0', name='new.png', tint=['1.0158', '1.0737', '0.9105'])                                  # the shipped defaults (gain 1.0, amb 0.6)
print('mean abs diff vs game: new defaults %.2f' % np.abs(new[m] - game[m]).mean())
print('mean game %s  new %s  fit %s' % (game[m].mean(0).round(1), new[m].mean(0).round(1), fit[m].mean(0).round(1)))
# landmark regions (client px of the 2400x1350 lens image): sight housing face, alpha gauge bezel, canopy bow upper left, strut left, right console
regs = {'sight-face': (980, 1150, 1150, 1250), 'alpha-bezel': (530, 1050, 560, 1080), 'bow-ul': (150, 60, 250, 120), 'strut-L': (430, 330, 450, 350),
        'console-R': (1700, 1180, 1800, 1250)}
for k, (x0, y0, x1, y1) in regs.items():
    sl = (slice(y0, y1), slice(x0, x1))
    mm = m[sl]
    if mm.sum() < 10: print(k, 'no pixels'); continue
    print('%-12s game %s new %s' % (k, np.median(game[sl][mm], 0).astype(int), np.median(new[sl][mm], 0).astype(int)))
o = np.concatenate([game, new], 0)[::3, ::3]
write_png(TMP + r'\game_vs_new.png', o.clip(0, 255).astype(np.uint8).copy())
