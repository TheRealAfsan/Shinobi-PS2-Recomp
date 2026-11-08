"""Disassemble a range of an IRX (MIPS R3000 LE, relocatable ELF) by module offset.

usage: disasm_irx.py <irx> <start_hex> <end_hex>
"""
import struct
import sys

from capstone import CS_ARCH_MIPS, CS_MODE_32, CS_MODE_LITTLE_ENDIAN, Cs

path, start, end = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
data = open(path, "rb").read()

assert data[:4] == b"\x7fELF", "not ELF"
phoff = struct.unpack_from("<I", data, 0x1C)[0]
phentsize, phnum = struct.unpack_from("<HH", data, 0x2A)
shoff = struct.unpack_from("<I", data, 0x20)[0]
shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)

print(f"ELF type=0x{struct.unpack_from('<H', data, 0x10)[0]:x} phnum={phnum} shnum={shnum}")
segs = []
for i in range(phnum):
    p_type, p_off, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from(
        "<IIIIIIII", data, phoff + i * phentsize
    )
    print(f"  seg{i}: type=0x{p_type:x} off=0x{p_off:x} vaddr=0x{p_vaddr:x} filesz=0x{p_filesz:x} memsz=0x{p_memsz:x}")
    segs.append((p_type, p_off, p_vaddr, p_filesz))

# the module offset is relative to the load base; take the first loadable segment as base 0
load = [s for s in segs if s[0] == 1 and s[3] > 0] or segs
base_off, base_va = load[0][1], load[0][2]
fo = base_off + (start - base_va)
code = data[fo : fo + (end - start)]

md = Cs(CS_ARCH_MIPS, CS_MODE_32 | CS_MODE_LITTLE_ENDIAN)
for ins in md.disasm(code, start):
    print(f"{ins.address:06x}: {ins.bytes.hex():8s} {ins.mnemonic:8s} {ins.op_str}")
