"""Summarise a [gs:draw] trace (PS2X_GS_DRAW_TRACE / F9 capture): draws per tick, by primitive and rough pixel cost."""
import collections
import re
import sys

draw_re = re.compile(r"\[gs:draw\] t=(\d+) path=(\d+) prim=(\d+) tme=(\d+) abe=(\d+).*?fbp=(\d+) fbw=(\d+) fpsm=(\w+).*?"
                     r"tbp=(\d+) .*?v0=\(([-\d.]+),([-\d.]+).*?vN=\(([-\d.]+),([-\d.]+)")
per_tick = collections.defaultdict(lambda: collections.Counter())
area = collections.defaultdict(float)
big = collections.defaultdict(list)
prim_names = {0: "point", 1: "line", 2: "linestrip", 3: "tri", 4: "tristrip", 5: "trifan", 6: "sprite"}
for line in open(sys.argv[1], encoding="utf-8", errors="ignore"):
    m = draw_re.search(line)
    if not m:
        continue
    tick, path, prim, tme, abe, fbp, fbw, fpsm, tbp, x0, y0, x1, y1 = m.groups()
    tick = int(tick)
    c = per_tick[tick]
    c["draws"] += 1
    c["prim_" + prim_names.get(int(prim), prim)] += 1
    c["textured"] += int(tme)
    c["blended"] += int(abe)
    w = abs(float(x1) - float(x0)) / 16.0
    h = abs(float(y1) - float(y0)) / 16.0
    a = w * h if int(prim) == 6 else 0.0
    area[tick] += a
    if a > 200 * 200:
        big[tick].append((int(a), "tme=%s abe=%s fbp=%s tbp=%s %.0fx%.0f" % (tme, abe, fbp, tbp, w, h)))
print("ticks traced:", len(per_tick))
for tick in sorted(per_tick):
    c = per_tick[tick]
    print("t=%d draws=%d tri=%d strip=%d sprite=%d textured=%d blended=%d sprite_area=%.0f px" % (
        tick, c["draws"], c["prim_tri"], c["prim_tristrip"], c["prim_sprite"], c["textured"], c["blended"], area[tick]))
    for a, desc in sorted(big[tick], reverse=True)[:6]:
        print("      big sprite", a, desc)
