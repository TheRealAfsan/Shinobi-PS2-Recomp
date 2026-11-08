"""Summarise a Shinobi run log (stderr of ps2EntryRunner with PS2X_STATS_LOG=1 PS2X_VU1_STATS=1).

usage: analyze_run.py <log> [--stage-only]
Prints fps over time, busiest threads, VU1 native-vs-interpreter share, handoff addresses, verify results and the
"pairs table" (cycles per second vs the 60 fps target of ~380 million).
"""
import collections
import re
import statistics
import sys

log = sys.argv[1]
stats = []        # (stage, fps, game, cpu, ram, vram, threads)
vu1 = []          # dicts from [vu1-stats]
why = []          # dicts from [vu1-why] counters
handoffs = collections.Counter()
verify = None
mismatches = 0
pads = []

stats_re = re.compile(r"\[stats\] stage=(\d) .*?\| ([\d.]+) fps \(game ([\d.]+)\) \| CPU (\d+)% \| RAM ([\d.]+) GB(?: \| VRAM ([\d.]+) GB)?(?: \| (.*))?$")
vu1_re = re.compile(r"\[vu1-stats\] runs/s=(\d+) pairs/s=(\d+) cycles/s=(\d+) busy=(\d+)% ns/pair=([\d.]+) ns/cycle=([\d.]+)")
why_re = re.compile(r"\[vu1-why\] execute=(\d+) resume=(\d+) run\(\)=(\d+) compiled_ok=(\d+) skipped\(pending=(\d+) branch/ebit=(\d+)\) no_program=(\d+) compiled_failed=(\d+)")
hand_re = re.compile(r"\[vu1-why\] handoffs at:(.*)")
time_re = re.compile(r"\[vu1-time\] compiled_run=(\d+) ms/s  of which gif_submit=(\d+) ms/s \((\d+) packets, (\d+) KB\)")
times = []
verify_re = re.compile(r"\[vu1-verify\] verified=(\d+) mismatches=(\d+)")

for line in open(log, encoding="utf-8", errors="ignore"):
    m = stats_re.search(line)
    if m:
        stats.append((int(m.group(1)), float(m.group(2)), float(m.group(3)), int(m.group(4)), float(m.group(5)),
                      float(m.group(6) or 0), m.group(7) or ""))
        continue
    m = vu1_re.search(line)
    if m:
        vu1.append(dict(runs=int(m.group(1)), pairs=int(m.group(2)), cycles=int(m.group(3)), busy=int(m.group(4))))
        continue
    m = why_re.search(line)
    if m:
        why.append(dict(execute=int(m.group(1)), resume=int(m.group(2)), runs=int(m.group(3)), ok=int(m.group(4)),
                        failed=int(m.group(8))))
        continue
    m = hand_re.search(line)
    if m:
        for item in m.group(1).split():
            pc, _, n = item.partition(":")
            handoffs[pc] += int(n)
        continue
    m = time_re.search(line)
    if m:
        times.append(tuple(int(m.group(i)) for i in range(1, 5)))
        continue
    m = verify_re.search(line)
    if m:
        verify = (int(m.group(1)), int(m.group(2)))
        continue
    if "MISMATCH" in line:
        mismatches += 1
    if line.startswith("[pad]"):
        pads.append(line.strip())

print("== run summary:", log)
print("pad:", pads[-1] if pads else "(none)")
stage = [s for s in stats if s[0] == 1]
print("seconds logged: %d   in stage: %d (stage flag = VU1 load > 5000 programs/s)" % (len(stats), len(stage)))
use = stage if stage else stats
if use:
    fps = [s[1] for s in use]
    print("FPS (%s): min %.0f  median %.0f  max %.0f   CPU median %d%%   RAM %.2f GB   VRAM %.2f GB" % (
        "stage" if stage else "all", min(fps), statistics.median(fps), max(fps), statistics.median([s[3] for s in use]),
        use[-1][4], use[-1][5]))
    threads = collections.defaultdict(list)
    for s in use:
        for part in s[6].split(", "):
            m = re.match(r"(.+) (\d+)%$", part.strip())
            if m:
                threads[m.group(1)].append(int(m.group(2)))
    if threads:
        print("busiest threads (avg % of one core over the seconds they were busy):")
        for name, values in sorted(threads.items(), key=lambda kv: -sum(kv[1]))[:5]:
            print("   %-28s %3.0f%%  (busy in %d of %d seconds)" % (name, sum(values) / len(values), len(values), len(use)))
    if len(fps) >= 6:
        quarter = len(fps) // 4
        print("fps early/late: %.1f -> %.1f (a drop over time would show here)" % (
            statistics.mean(fps[:quarter]), statistics.mean(fps[-quarter:])))
if vu1:
    tail = vu1[len(vu1) // 3:] if len(vu1) > 9 else vu1
    cycles = statistics.mean(v["cycles"] for v in tail)
    pairs = statistics.mean(v["pairs"] for v in tail)
    runs = statistics.mean(v["runs"] for v in tail)
    busy = statistics.mean(v["busy"] for v in tail)
    print("VU1: %.0f runs/s, %.1f M cycles/s, busy %.0f%% of wall; interpreted pairs %.1f M/s = %.0f%% of all cycles" % (
        runs, cycles / 1e6, busy, pairs / 1e6, 100.0 * pairs / cycles if cycles else 0))
if why:
    tail = why[len(why) // 3:] if len(why) > 9 else why
    ok = sum(w["ok"] for w in tail)
    failed = sum(w["failed"] for w in tail)
    total = sum(w["runs"] for w in tail)
    print("runs that started native-and-finished: %.0f%%   handed off to the interpreter: %.0f%%   (of %d runs)" % (
        100.0 * ok / max(1, total), 100.0 * failed / max(1, total), total))
if times:
    tail = times[len(times) // 3:] if len(times) > 9 else times
    comp = statistics.mean(t[0] for t in tail)
    gif = statistics.mean(t[1] for t in tail)
    packets = statistics.mean(t[2] for t in tail)
    kb = statistics.mean(t[3] for t in tail)
    print("VU1 thread time per second: compiled runs %.0f ms, of which GIF hand-over to the GS %.0f ms  (%.0f packets, %.0f KB per second)" % (comp, gif, packets, kb))
    print("   => VU programs themselves ~%.0f ms/s, GIF hand-over ~%.0f ms/s, per packet %.1f us" % (comp - gif, gif, 1000.0 * gif / max(1.0, packets)))
if handoffs:
    print("top handoff addresses:", ", ".join("%s:%d" % kv for kv in handoffs.most_common(8)))
if verify:
    print("VERIFY: %d runs checked, %d mismatches (log lines with MISMATCH: %d)" % (verify[0], verify[1], mismatches))
if vu1:
    cyc = statistics.mean(v["cycles"] for v in (vu1[len(vu1) // 3:] if len(vu1) > 9 else vu1))
    print()
    print("PAIRS TABLE            cycles/s        fps-equivalent (6.3 M cycles/frame)")
    print("  this run (VU1 load)  %8.1f M      %.1f fps worth of VU1 work done per second" % (cyc / 1e6, cyc / 6.3e6))
    print("  60 fps target        ~380 M          60 fps")
