"""Reassemble VU1 micro programs (chains of VIF MPG uploads) from EE RAM dumps and write each as a 16 KB image for vu1rec.py.

usage: find_mpg.py <out_dir> <rdram.bin> [<rdram.bin> ...]

MPG = VIF command 0x4A: word = 0x4A000000 | num << 16 | addr (addr in 64-bit units, num 0 = 256 pairs); followed by
num instruction pairs (lower word first). Games upload a big program as consecutive chunks (addr 0, 0x100, ...). A chain
starts at an MPG with addr 0 and continues while the next header (up to 4 filler words later) continues the address.
Images are de-duplicated by content.
"""
import hashlib
import os
import struct
import sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
images = {}

for dump in sys.argv[2:]:
    ram = open(dump, "rb").read()
    n = len(ram) // 4
    words = struct.unpack("<%dI" % n, ram[: n * 4])
    for i in range(n):
        w = words[i]
        if (w >> 24) != 0x4A or (w & 0xFFFF) != 0:
            continue
        image = bytearray(16384)
        pos = i
        expect = 0
        chunks = 0
        total = 0
        e_seen = False
        while True:
            w = words[pos]
            num = ((w >> 16) & 0xFF) or 256
            addr = w & 0xFFFF
            if (w >> 24) != 0x4A or addr != expect or (addr + num) * 8 > 16384 or pos + 1 + num * 2 > n:
                break
            data = ram[(pos + 1) * 4:(pos + 1 + num * 2) * 4]
            image[addr * 8:addr * 8 + num * 8] = data
            for k in range(num):
                if struct.unpack_from("<I", data, k * 8 + 4)[0] & 0x40000000:
                    e_seen = True
            chunks += 1
            total += num
            expect = addr + num
            pos += 1 + num * 2
            skipped = 0
            while pos < n and words[pos] == 0 and skipped < 4:
                pos += 1
                skipped += 1
        if total >= 16 and e_seen:
            key = hashlib.md5(bytes(image)).hexdigest()[:12]
            images.setdefault(key, (os.path.basename(dump), i * 4, total, chunks, bytes(image)))

print("distinct complete programs:", len(images))
for key, (dump, pos, total, chunks, image) in sorted(images.items(), key=lambda kv: -kv[1][2]):
    name = "vu1code_%s.bin" % key
    open(os.path.join(out, name), "wb").write(image)
    print("  %s  %4d pairs in %d chunks  (first seen %s @ EE 0x%07x)" % (name, total, chunks, dump, pos))
