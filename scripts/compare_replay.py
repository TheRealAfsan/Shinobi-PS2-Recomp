"""Compare two replay outputs (see PS2X_PCSX2_REPLAY in gs_pcsx2.cpp).

usage: python -I compare_replay.py <prefixA> <prefixB> [outdir]

<prefix>_<frame>_<w>x<h>.rgba files are paired by frame number. Prints, per frame: share of pixels that differ by more
than 8 in any channel, and the mean absolute difference. Writes PNGs of A, B and an amplified difference for the worst
pairs into outdir (default: compare_out next to prefixA). Needs nothing outside the standard library.
"""
import glob, os, re, struct, sys, zlib


def load(prefix):
    frames = {}
    for path in glob.glob(prefix + "_*.rgba"):
        m = re.search(r"_(\d+)_(\d+)x(\d+)\.rgba$", path)
        if m:
            frames[int(m.group(1))] = (path, int(m.group(2)), int(m.group(3)))
    return frames


def png(path, w, h, rgb):
    raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 3)) + chunk(b"IEND", b""))


def rgb_of(data):
    out = bytearray(len(data) // 4 * 3)
    out[0::3] = data[0::4]
    out[1::3] = data[1::4]
    out[2::3] = data[2::4]
    return bytes(out)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    a, b = load(sys.argv[1]), load(sys.argv[2])
    outdir = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(os.path.abspath(sys.argv[1])), "compare_out")
    os.makedirs(outdir, exist_ok=True)
    common = sorted(set(a) & set(b))
    if not common:
        print("no frames in common (A has %d, B has %d)" % (len(a), len(b)))
        return 1
    rows = []
    for n in common:
        (pa, wa, ha), (pb, wb, hb) = a[n], b[n]
        if (wa, ha) != (wb, hb):
            rows.append((n, 1.0, 255.0, "size differs %dx%d vs %dx%d" % (wa, ha, wb, hb)))
            continue
        da, db = open(pa, "rb").read(), open(pb, "rb").read()
        count = wa * ha
        differing = 0
        total = 0
        for i in range(0, len(da) - 3, 4):
            d = max(abs(da[i] - db[i]), abs(da[i + 1] - db[i + 1]), abs(da[i + 2] - db[i + 2]))
            total += d
            if d > 8:
                differing += 1
        rows.append((n, differing / count, total / count, ""))
    rows.sort()
    print("frame  differing  mean|diff|")
    for n, share, mean, note in rows:
        print("%5d  %8.2f%%  %9.2f  %s" % (n, share * 100, mean, note))
    worst = sorted(rows, key=lambda r: -r[1])[:3]
    for n, share, mean, note in worst:
        if note:
            continue
        (pa, w, h), (pb, _, _) = a[n], b[n]
        da, db = open(pa, "rb").read(), open(pb, "rb").read()
        png(os.path.join(outdir, "f%05d_A.png" % n), w, h, rgb_of(da))
        png(os.path.join(outdir, "f%05d_B.png" % n), w, h, rgb_of(db))
        diff = bytearray(len(da) // 4 * 3)
        for i in range(w * h):
            for c in range(3):
                diff[i * 3 + c] = min(255, abs(da[i * 4 + c] - db[i * 4 + c]) * 4)
        png(os.path.join(outdir, "f%05d_diff.png" % n), w, h, bytes(diff))
    print("worst frames written to", outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
