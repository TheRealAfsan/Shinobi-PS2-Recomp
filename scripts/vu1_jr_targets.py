"""Where do a VU1 program's JR/JALR targets come from? Lists JR instructions, how their register is set up, and whether
the constants loaded with IADDIU/ISUBIU (vis=vi0) cover the addresses seen as run exits.

usage: vu1_jr_targets.py <image.bin> [exit_pc_hex ...]
"""
import collections
import struct
import sys

image = open(sys.argv[1], "rb").read()
exits = [int(x, 16) for x in sys.argv[2:]]
used = len(image.rstrip(b"\x00"))
pairs = [(struct.unpack_from("<I", image, pc)[0], struct.unpack_from("<I", image, pc + 4)[0]) for pc in range(0, used, 8)]

consts = collections.defaultdict(list)   # target pc -> [(pc, vit)]
jrs = []
for i, (lower, upper) in enumerate(pairs):
    if upper & 0x80000000:               # I bit: lower word is an immediate, not an instruction
        continue
    op = (lower >> 25) & 0x7F
    pc = i * 8
    if op in (0x08, 0x09):               # IADDIU / ISUBIU
        it = (lower >> 16) & 0xF
        is_ = (lower >> 11) & 0xF
        imm = ((lower >> 21) & 0xF) << 11 | (lower & 0x7FF)
        if is_ == 0:
            value = imm if op == 0x08 else -imm
            consts[(value * 8) & 0x3FFF].append((pc, it))
    elif op in (0x24, 0x25):             # JR / JALR
        jrs.append((pc, (lower >> 11) & 0xF, op))
print("program: %d pairs; JR/JALR sites: %d" % (len(pairs), len(jrs)))
for pc, is_, op in jrs[:12]:
    print("   %s at 0x%04x uses vi%d" % ("JR" if op == 0x24 else "JALR", pc, is_))
valid = {t for t in consts if t < used}
print("IADDIU/ISUBIU constants that are valid code addresses: %d" % len(valid))
for e in exits:
    print("run exit 0x%04x: %s" % (e, ("loaded as a constant at " + ", ".join("0x%04x(vi%d)" % x for x in consts[e])) if e in consts else "NOT a constant"))
