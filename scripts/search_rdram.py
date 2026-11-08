"""Search an EE RAM dump for tags and show context.

usage: search_rdram.py <rdram.bin>
"""
import mmap
import struct
import sys

with open(sys.argv[1], "rb") as f:
    m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)


def hits(tag, limit=8):
    out, s = [], 0
    while len(out) < limit:
        p = m.find(tag, s)
        if p < 0:
            break
        out.append(p)
        s = p + 1
    return out


for tag in (b"FSTH", b"HTSF", b"PVRT", b"TRVP", b"GBIX", b"NJTL", b"B Data", b"gaiji.ada", b"gaiji_t.ada"):
    h = hits(tag)
    print("%-12s %s" % (tag.decode("latin1"), [hex(x) for x in h]))

# FONT.BDA was read to 0x173df80: check the first bytes there
base = 0x173DF80
print("RAM@0x%x first 48 bytes: %s" % (base, m[base : base + 48].hex(" ")))
print("expected file head     : 04 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 42 20 44 61 74 61 00 ff ...")

# the two entries' offsets are 0xa0 and 0x1c0 in the archive; show what's at those in RAM
for name, off in (("gaiji.ada", 0xA0), ("gaiji_t.ada", 0x1C0)):
    p = base + off
    print("RAM entry %-12s @0x%x: %s" % (name, p, m[p : p + 32].hex(" ")))
