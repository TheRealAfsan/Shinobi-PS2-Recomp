"""Measure flicker in a folder of consecutive presented frames (PS2X_FRAME_DUMP_INTERVAL=1).

usage: flicker_analyze.py <frames_dir> [--skip N]
For each screen region (static world patch, HUD ring, kunai stack, player area, whole frame) prints the frame-to-frame
change and whether it alternates with frame parity (even/odd) or period 3-4, which separates "drawn every other frame"
and "sometimes missing at display time" from steady motion.
"""
import glob
import os
import re
import sys

import numpy as np
from PIL import Image

folder = sys.argv[1]
skip = int(sys.argv[sys.argv.index("--skip") + 1]) if "--skip" in sys.argv else 0
files = sorted(glob.glob(os.path.join(folder, "frame_*.png")) + glob.glob(os.path.join(folder, "frame_*.ppm")),
               key=lambda p: int(re.search(r"frame_(\d+)_", os.path.basename(p)).group(1)))[skip:]
print("frames:", len(files))
if len(files) < 12:
    raise SystemExit("need at least 12 frames")

first = Image.open(files[0]).convert("RGB")
W, H = first.size
# regions as fractions of the image: (x0, y0, x1, y1)
regions = {
    "world (top-left, static)": (0.02, 0.10, 0.20, 0.30),
    "HUD ring (bottom-left)": (0.03, 0.70, 0.20, 0.97),
    "kunai stack (right)": (0.88, 0.55, 0.99, 0.90),
    "player (centre)": (0.38, 0.40, 0.62, 0.90),
    "whole frame": (0.0, 0.0, 1.0, 1.0),
}
series = {name: [] for name in regions}
prev = None
for path in files:
    img = np.asarray(Image.open(path).convert("L"), dtype=np.float32)
    h, w = img.shape
    if prev is not None:
        for name, (x0, y0, x1, y1) in regions.items():
            a = img[int(y0 * h):int(y1 * h), int(x0 * w):int(x1 * w)]
            b = prev[int(y0 * h):int(y1 * h), int(x0 * w):int(x1 * w)]
            series[name].append(float(np.abs(a - b).mean()))
    prev = img

print("%-26s %8s %8s %8s %10s %10s" % ("region", "mean|d|", "max|d|", "ev/odd", "period2 r", "period3-4 r"))
for name, values in series.items():
    v = np.array(values)
    even, odd = v[0::2].mean(), v[1::2].mean()
    vc = v - v.mean()
    denom = float((vc * vc).sum()) or 1.0
    r2 = float((vc[:-2] * vc[2:]).sum() / denom)          # autocorrelation at lag 2 (period-2 alternation)
    r1 = float((vc[:-1] * vc[1:]).sum() / denom)          # lag 1: negative = alternating high/low
    print("%-26s %8.2f %8.2f %4.1f/%-4.1f %+10.2f %+10.2f" % (name, v.mean(), v.max(), even, odd, r1, r2))
print()
print("reading: lag-1 correlation strongly NEGATIVE (e.g. < -0.5) = the region alternates every frame (flicker).")
print("         ev/odd means differ a lot = one parity of frames is different from the other.")
# frame-by-frame trace of the two suspects
for name in ("HUD ring (bottom-left)", "player (centre)"):
    print(name, "per-frame change:", " ".join("%.0f" % x for x in series[name][:40]))
