"""Summarise one or more perf_*.csv files written by the game (see PerfRecorder in ps2_runtime.cpp).

usage: python -I perf_summary.py <perf.csv> [<perf2.csv> ...] [--all]

Only seconds spent in a stage (stage=1) are counted unless --all is given. With two or more files the results are printed side by
side (first file is the left column). Everything here is the game's real behaviour: game_fps counts frames that changed the
picture, vsync_per_s is emulated vblanks per real second (60 = the emulated machine is at full speed).
"""
import csv, sys, collections, re


LABELS = {
    "FFFF0001": "kick of a DMA (processPendingTransfers)", "FFFF0002": "submit GIF packet",
    "FFFF0003": "GIF image-upload chain", "FFFF0004": "GIF packed chain",
    "FFFF0021": "sceGsSwapDBuffDc", "FFFF0022": "sceGsSwapDBuff", "FFFF0023": "GsSetCrt", "FFFF0024": "GsPutIMR", "FFFF0025": "GS stub register write (PutDraw/DispEnv)",
    "FFFF0030": "window thread latching a frame", "FFFF0031": "syncCoreSubsystems (GS helper)", "FFFF0032": "DMAC register stub",
    "10009000": "VIF1 DMA control (CHCR)", "1000A000": "GIF DMA control (CHCR)", "1000E010": "DMA status (D_STAT)",
    "10003C00": "VIF1 status", "10003020": "GIF status", "10005000": "VIF1 FIFO write", "00000000": "no reason given (untagged site)",
}


def load(path, stage_only):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            try:
                if stage_only and r["stage"] != "1":
                    continue
                rows.append(r)
            except KeyError:
                pass
    return rows


def num(r, k):
    try:
        return float(r[k])
    except (KeyError, ValueError):
        return 0.0


def pct(vals, q):
    if not vals:
        return 0.0
    vals = sorted(vals)
    return vals[min(len(vals) - 1, int(q * len(vals)))]


def summarise(path, stage_only):
    rows = load(path, stage_only)
    n = len(rows)
    out = collections.OrderedDict()
    out["seconds counted"] = "%d" % n
    if not n:
        return out
    g = [num(r, "game_fps") for r in rows]
    out["renderer"] = rows[0].get("renderer", "?")
    out["game fps avg / p10 / min"] = "%.1f / %.1f / %.1f" % (sum(g) / n, pct(g, 0.10), min(g))
    for t in (57, 50, 40, 30):
        out["seconds with game fps < %d" % t] = "%d (%.0f%%)" % (sum(1 for x in g if x < t), 100.0 * sum(1 for x in g if x < t) / n)
    v = [num(r, "vsync_per_s") for r in rows if num(r, "vsync_per_s") > 0]
    out["emulated vblanks/s avg / min"] = "%.1f / %.1f" % ((sum(v) / len(v), min(v)) if v else (0, 0))
    b = [sum(num(r, k) for r in rows) for k in ("v1", "v2", "v3", "v4p")]
    tot = sum(b) or 1
    out["new frames taking 1/2/3/4+ vblanks"] = " / ".join("%.0f%%" % (100 * x / tot) for x in b)
    out["frame gaps >25 ms / >50 ms (total)"] = "%d / %d" % (sum(num(r, "slow25") for r in rows), sum(num(r, "slow50") for r in rows))
    out["worst frame gap (ms)"] = "%.0f" % max(num(r, "f_max_ms") for r in rows)
    out["game CPU (EE) waits for VIF1 (ms/s avg)"] = "%.0f" % (sum(num(r, "ee_wait_ms") for r in rows) / n)
    out["window thread waits for VIF1 (ms/s avg)"] = "%.0f" % (sum(num(r, "other_wait_ms") for r in rows) / n)
    out["VIF1 thread idle (ms/s avg)"] = "%.0f" % (sum(num(r, "vif1_idle_ms") for r in rows) / n)
    out["IOP emulation on game thread (ms/s avg)"] = "%.0f" % (sum(num(r, "iop_ms") for r in rows) / n)
    calls = sum(num(r, "aud_calls") for r in rows) or 1
    out["audio underruns / drops per s"] = "%.2f (%.1f%% of callbacks) / %.1f" % (
        sum(num(r, "aud_under") for r in rows) / n, 100.0 * sum(num(r, "aud_under") for r in rows) / calls,
        sum(num(r, "aud_drop") for r in rows) / n)
    out["audio buffered ms avg"] = "%.0f" % (sum(num(r, "aud_buf_ms") for r in rows) / n)
    waits = collections.Counter()
    counts = collections.Counter()
    for r in rows:
        for i in (1, 2, 3):
            a = r.get("w%d_addr" % i, "")
            if a:
                waits[a] += num(r, "w%d_ms" % i)
                counts[a] += num(r, "w%d_n" % i)
    for i, (a, ms) in enumerate(waits.most_common(4), 1):
        label = LABELS.get(a, "")
        if not label:
            v = int(a, 16) if a else 0
            label = ("VU1 memory" if 0x1100C000 <= v < 0x11010000 or 0x11008000 <= v < 0x1100C000 else
                     "GS privileged register" if 0x12000000 <= v < 0x12002000 else
                     "VIF1 register" if 0x10003C00 <= v < 0x10003E00 else "")
        out["wait #%d: %s %s" % (i, a, label)] = "%.0f ms/s, %d waits" % (ms / n, counts[a])
    # what the guest's threads waited for (frequent waits): total ms/s and waits/s per thread+kind, averaged over the seconds
    blk = collections.defaultdict(lambda: [0.0, 0.0, 0.0])
    for r in rows:
        for part in (r.get("blocked", "") or "").split(" / "):
            m = re.match(r"(t\d+@[0-9A-F]+) (\S+) n=(\d+) ms=(\d+) max=(\d+)", part.strip())
            if m:
                e = blk[(m.group(1), m.group(2))]
                e[0] += float(m.group(4)); e[1] += float(m.group(3)); e[2] = max(e[2], float(m.group(5)))
    for i, ((who, kind), (ms, cnt, mx)) in enumerate(sorted(blk.items(), key=lambda kv: -kv[1][0])[:4], 1):
        out["guest wait #%d: %s %s" % (i, who, kind)] = "%.0f ms/s, %.0f waits/s, longest %.0f ms" % (ms / n, cnt / n, mx)
    worst = sorted(rows, key=lambda r: num(r, "game_fps"))[:3]
    out["worst seconds (t, game fps, vblanks/s)"] = "; ".join("%.0f s: %.0f fps, %.0f" % (num(r, "time_s"), num(r, "game_fps"), num(r, "vsync_per_s")) for r in worst)
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    stage_only = "--all" not in sys.argv
    if not args:
        print(__doc__)
        return 2
    cols = [summarise(p, stage_only) for p in args]
    keys = []
    for c in cols:
        for k in c:
            if k not in keys and not k.startswith(("wait #", "guest wait #")):
                keys.append(k)
    width = max(len(k) for k in keys) + 2
    print(("%-" + str(width) + "s") % "" + "".join("%-34s" % p.replace("\\", "/").split("/")[-1][:32] for p in args))
    for k in keys:
        print(("%-" + str(width) + "s") % k + "".join("%-34s" % c.get(k, "-") for c in cols))
    for i, c in enumerate(cols):
        w = [(k, v) for k, v in c.items() if k.startswith(("wait #", "guest wait #"))]
        if w:
            print("\n%s - what the game's CPU waited on:" % args[i].replace("\\", "/").split("/")[-1])
            for k, v in w:
                print("   %-60s %s" % (k, v))
    return 0


if __name__ == "__main__":
    sys.exit(main())
