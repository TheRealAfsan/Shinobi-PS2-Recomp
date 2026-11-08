"""Look inside a BDA archive: header words and where known tags appear."""
import struct
import sys

path = sys.argv[1]
d = open(path, "rb").read()
print(path, len(d), "bytes")
print("header (first 0x60 bytes):")
for i in range(0, 0x60, 16):
    print("  %04x: %s" % (i, d[i : i + 16].hex(" ")))

print("u32 words 0..23:", [hex(struct.unpack_from("<I", d, i * 4)[0]) for i in range(24)])

for tag in (b"FSTH", b"HTSF", b"PVRT", b"TRVP", b"IZCA", b"NJTL", b"GBIX", b"B Data"):
    hits = []
    s = 0
    while len(hits) < 6:
        p = d.find(tag, s)
        if p < 0:
            break
        hits.append(p)
        s = p + 1
    print("tag %-7s at: %s" % (tag.decode("latin1"), [hex(h) for h in hits]))
