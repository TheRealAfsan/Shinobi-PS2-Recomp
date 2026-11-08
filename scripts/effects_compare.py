"""Compares the replays written by effects_check.bat (A = PCSX2 software renderer at native resolution, the reference;
B, C, D = Direct3D 11 at 4x with different settings, E = Vulkan at 4x). Every frame is scaled to 640x448 and compared with A:

  diff%   share of pixels differing by more than 24 (of 255) in any channel - missing or extra effect layers show up here,
          plain sharpness differences mostly do not
  dark    mean brightness difference against A (fog, glow and colour washes change brightness; negative = darker)

Writes side-by-side pictures A | B | C | D | E of the frames where the setups disagree most (effects_<frame>.png) and a summary.
usage: python effects_compare.py <effects_check folder>
"""
import glob
import os
import re
import sys

import numpy as np
from PIL import Image

SIZE = (640, 448)


def frames(folder, prefix):
    out = {}
    for path in glob.glob(os.path.join(folder, prefix + '_*.rgba')):
        m = re.search(r'_(\d+)_(\d+)x(\d+)\.rgba$', path)
        if m:
            out[int(m.group(1))] = (path, int(m.group(2)), int(m.group(3)))
    return out


def load(entry):
    path, w, h = entry
    raw = np.fromfile(path, dtype=np.uint8)
    if raw.size < w * h * 4:
        return None
    img = Image.fromarray(raw[:w * h * 4].reshape(h, w, 4)[:, :, :3])
    return np.asarray(img.resize(SIZE, Image.BOX), dtype=np.int16)


def main():
    folder = sys.argv[1]
    sets = {k: frames(folder, k) for k in 'ABCDE'}
    if not sets['A']:
        print('no reference (A) frames in', folder)
        return 1
    common = sorted(set(sets['A']).intersection(*[set(v) for k, v in sets.items() if k != 'A' and v]))
    print('%d frames compared (scaled to %dx%d); reference A = PCSX2 software renderer, native' % (len(common), *SIZE))
    print('frame    B diff% dark   C diff% dark   D diff% dark   E diff% dark')
    rows = []
    totals = {k: [] for k in 'BCDE'}
    for n in common:
        a = load(sets['A'][n])
        if a is None:
            continue
        line = '%6d' % n
        imgs = {'A': a}
        score = 0.0
        for k in 'BCDE':
            if n not in sets[k]:
                line += '        -     - '
                continue
            b = load(sets[k][n])
            if b is None or b.shape != a.shape:
                line += '        ?     ? '
                continue
            imgs[k] = b
            diff = np.abs(b - a).max(axis=2)
            pct = 100.0 * float((diff > 24).mean())
            dark = float(b.mean() - a.mean())
            totals[k].append((pct, dark))
            score = max(score, pct)
            line += '   %6.1f %5.1f ' % (pct, dark)
        print(line)
        rows.append((score, n, imgs))
    print('\naverage over all frames (lower diff% = closer to the reference):')
    names = {'B': 'Shinobi settings (now)', 'C': 'Ridge Racer fixes (before)', 'D': 'fixes off, no native scaling',
             'E': 'Vulkan, Shinobi settings'}
    for k in 'BCDE':
        if totals[k]:
            print('  %s %-30s diff %5.1f%%  brightness %+5.1f' % (k, names[k], np.mean([t[0] for t in totals[k]]),
                                                                 np.mean([t[1] for t in totals[k]])))
    rows.sort(key=lambda r: -r[0])
    for score, n, imgs in rows[:12]:
        tiles = [imgs.get(k, np.zeros_like(imgs['A'])) for k in 'ABCDE']
        Image.fromarray(np.concatenate(tiles, axis=1).astype(np.uint8)).save(os.path.join(folder, 'effects_%06d.png' % n))
    print('\nside-by-side pictures (A | B | C | D | E) of the %d most different frames: effects_<frame>.png' % min(12, len(rows)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
