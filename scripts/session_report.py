"""After a profile session: one table from the game's log (VU1, exact maths, GS thread, profile) next to a baseline log,
then perf_summary.py on the two perf CSVs.

usage: python -I session_report.py <new last_run.log> <baseline last_run.log> [<new perf.csv> <baseline perf.csv>]
"""
import collections
import os
import re
import subprocess
import sys


def avg(values):
    return sum(values) / len(values) if values else float('nan')


def parse(path):
    text = open(path, encoding='utf-8', errors='replace').read()
    out = collections.OrderedDict()
    f = lambda pattern: [float(x) for x in re.findall(pattern, text)]
    busy, nspc = f(r'\[vu1-stats\].*?busy=(\d+)%'), f(r'\[vu1-stats\].*?ns/cycle=([\d.]+)')
    runs, cycles = f(r'\[vu1-stats\] runs/s=(\d+)'), f(r'\[vu1-stats\].*?cycles/s=(\d+)')
    # Only seconds with real VU1 load (menus/loading screens dilute the averages).
    loaded = [i for i, c in enumerate(cycles) if c > 20e6]
    pick = lambda values: [values[i] for i in loaded if i < len(values)]
    out['seconds with VU1 load (>20M cycles/s)'] = '%d' % len(loaded)
    out['VU1 thread busy %'] = '%.0f' % avg(pick(busy))
    out['VU1 ns per cycle'] = '%.2f' % avg(pick(nspc))
    out['VU1 cycles/s (millions)'] = '%.0f' % (avg(pick(cycles)) / 1e6)
    out['VU1 runs/s'] = '%.0f' % avg(pick(runs))
    comp, gif = f(r'\[vu1-time\] compiled_run=(\d+) ms/s'), f(r'gif_submit=(\d+) ms/s')
    out['translated VU1 time ms/s'] = '%.0f' % avg(pick(comp))
    out['  of which GIF packet hand-off ms/s'] = '%.0f' % avg(pick(gif))
    exact = f(r'\[vu1-exact\] calls=(\d+)')
    out['exact-maths calls/s'] = ('%.0f' % avg(pick(exact))) if exact else 'n/a (older build)'
    kinds = collections.Counter()
    for line in re.findall(r'\[vu1-exact\].*kind: (.*)', text):
        for name, value in re.findall(r'(\w+)=(\d+)', line):
            kinds[name] += int(value)
    if kinds and sum(kinds.values()):
        out['exact-maths by kind (total)'] = ' '.join('%s=%d' % kv for kv in kinds.most_common() if kv[1])
    bridge = re.findall(r'bridge time per frame: (.*)', text)
    out['PCSX2 bridge time per frame (last)'] = bridge[-1].strip() if bridge else '-'
    queue = re.findall(r'GS thread queue: (.*)', text)
    out['GS thread queue (last)'] = queue[-1].strip() if queue else 'n/a (older build)'
    lag = re.findall(r'\[swap-lag\] swaps=(\d+) avg=([\d.]+) ms max=([\d.]+) ms >16.7ms=(\d+) >33.3ms=(\d+)', text)
    if lag:
        swaps = sum(int(x[0]) for x in lag) or 1
        out['swap wait in VIF1 queue avg / max'] = '%.1f / %.0f ms' % (avg([float(x[1]) for x in lag]), max(float(x[2]) for x in lag))
        out['  swaps waiting > 1 frame / > 2 frames'] = '%.0f%% / %.0f%%' % (100.0 * sum(int(x[3]) for x in lag) / swaps,
                                                                           100.0 * sum(int(x[4]) for x in lag) / swaps)
    else:
        out['swap wait in VIF1 queue avg / max'] = 'n/a (older build)'
    spu = re.findall(r'\[spu2\] t=\d+s keyons=(\d+) rekeys<50ms=(\d+) adma-starts=(\d+) adma-leftover=(\d+)', text)
    if spu:
        out['SPU2 key-ons / re-keys<50ms per s'] = '%.1f / %.2f' % (avg([float(x[0]) for x in spu]), avg([float(x[1]) for x in spu]))
        out['SPU2 stream transfers / leftover samples per s'] = '%.1f / %.0f' % (avg([float(x[2]) for x in spu]), avg([float(x[3]) for x in spu]))
        routing = re.findall(r'\| (core\d MMIX=\S+ ADMAS=\S+ AVOL=\S+ BVOL=\S+ MVOL=\S+)', text)
        if routing:
            out['SPU2 routing (last)'] = ' '.join(routing[-2:])
    selfhitch = [float(x) for x in re.findall(r'\[hitch-self\] [^\n]*?took ([\d.]+) ms', text)]
    out['window-thread stats update >4 ms (count, max)'] = ('%d, %.0f ms' % (len(selfhitch), max(selfhitch))) if selfhitch else '0'
    marks = re.findall(r'\[MARK (\d+)\] t=([\d.]+) s .*?audio=([\d.]+) s .*?game fps (\d+), slowest frame (\d+) ms', text)
    out['problems you marked (F8 / L3+R3)'] = ('; '.join('#%s at %ss (audio %ss, %s fps, slowest %s ms)' % m for m in marks)
                                               if marks else 'none')
    # Last profile report per thread: top 8 functions.
    reports = collections.OrderedDict()
    current = None
    for line in text.splitlines():
        m = re.match(r'\[prof ([^\]]+?)\] (\d+) samples', line)
        if m:
            current = m.group(1)
            reports[current] = []
            continue
        m = re.match(r'\[prof ([^\]]+?)\]\s+([\d.]+)%\s+(.*)', line)
        if m and m.group(1) == current and len(reports[current]) < 8:
            reports[current].append((float(m.group(2)), m.group(3)))
    return out, reports


def main():
    args = [a for a in sys.argv[1:]]
    if len(args) < 2:
        print(__doc__)
        return 2
    new, base = parse(args[0]), parse(args[1])
    keys = list(new[0].keys()) + [k for k in base[0] if k not in new[0]]
    width = max(len(k) for k in keys) + 2
    print(('%-' + str(width) + 's%-40s%s') % ('', 'THIS SESSION', 'BASELINE'))
    for k in keys:
        print(('%-' + str(width) + 's%-40s%s') % (k, new[0].get(k, '-')[:38], base[0].get(k, '-')))
    for label, (_, reports) in (('THIS SESSION', new), ('BASELINE', base)):
        print('\n%s - where the busiest threads spend time (last profile report):' % label)
        for thread in ('VIF1+VU1', 'GameThread', 'PCSX2 GS'):
            rows = [r for name, r in reports.items() if name == thread]
            if rows and rows[0]:
                print('  %s: %s' % (thread, '; '.join('%.1f%% %s' % (p, n[:60]) for p, n in rows[0])))
    if len(args) >= 4:
        here = os.path.dirname(os.path.abspath(__file__))
        print()
        subprocess.run([sys.executable, '-I', os.path.join(here, 'perf_summary.py'), args[2], args[3]])
    return 0


if __name__ == '__main__':
    sys.exit(main())
