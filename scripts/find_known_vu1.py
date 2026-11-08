"""Where does the game keep a VU1 program that is known to be resident? Search an EE RAM dump for chunks of its code.

usage: find_known_vu1.py <rdram.bin> <vu1code.bin>
"""
import struct
import sys

ram = open(sys.argv[1], "rb").read()
code = open(sys.argv[2], "rb").read()
# the program starts at pair 0; use a few 32-byte windows from its start, skipping blank leading words
used = len(code.rstrip(b"\x00"))
print("resident image: %d bytes of code (%d pairs)" % (used, used // 8))
for offset in (0, 64, 128, 256, 512, 1024, 2048, 3000):
    if offset + 32 > used:
        break
    needle = code[offset:offset + 32]
    hits = []
    start = 0
    while len(hits) < 6:
        pos = ram.find(needle, start)
        if pos < 0:
            break
        hits.append(pos)
        start = pos + 1
    print("code[%d:%d] found at EE:" % (offset, offset + 32), ["0x%07x" % h for h in hits])
    for h in hits[:2]:
        before = struct.unpack_from("<4I", ram, h - 16) if h >= 16 else ()
        print("      the 16 bytes before: ", ["%08x" % x for x in before])
