"""For one traced tick of a [gs:draw] log: how many distinct render states, texture switches and tiny draws."""
import collections
import re
import sys

log, tick = sys.argv[1], sys.argv[2]
state_re = re.compile(r"\[gs:draw\] t=%s path=(\d+) prim=(\d+) tme=(\d+) abe=(\d+) fst=(\d+) fbp=(\d+) fbw=(\d+) fpsm=(\w+) "
                      r"fbmsk=(\w+) zbp=(\d+) zpsm=(\w+) zmsk=(\d+) test=(\w+) alpha=(\w+) tbp=(\d+) tbw=(\d+) tpsm=(\w+) "
                      r"tw=(\d+) th=(\d+) tfx=(\d+) tcc=(\d+)" % tick)
draws = []
for line in open(log, encoding="utf-8", errors="ignore"):
    m = state_re.search(line)
    if m:
        draws.append(m.groups())
print("draws:", len(draws))
tex = collections.Counter()
state = collections.Counter()
switches = 0
state_switches = 0
prev_tex = prev_state = None
for d in draws:
    t = (d[14], d[16], d[17], d[18])      # tbp, tpsm, tw, th
    s = d[:2] + d[3:4] + d[10:14] + d[19:21]
    tex[t] += 1
    state[s] += 1
    if t != prev_tex:
        switches += 1
    if s != prev_state:
        state_switches += 1
    prev_tex, prev_state = t, s
print("distinct textures:", len(tex), " texture switches between consecutive draws:", switches)
print("distinct render states:", len(state), " state switches:", state_switches)
print("top textures (tbp, psm, tw, th) by draw count:")
for t, n in tex.most_common(8):
    print("   ", t, n)
print("draws per texture switch: %.1f" % (len(draws) / max(1, switches)))
