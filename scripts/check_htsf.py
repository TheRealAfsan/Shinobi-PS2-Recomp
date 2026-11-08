"""Is the HTSF texture header found in RAM part of the ELF image, or built at run time?"""
import mmap

with open(r"X:\HAPPYNESS\shinobi_port\rdram1.bin", "rb") as f:
    ram = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
elf = open(r"X:\HAPPYNESS\shinobi_port\game\SLUS_204.59", "rb").read()

va = 0x46F430
fo = 0x100 + (va - 0x180000)  # segment 1: file off 0x100 -> vaddr 0x180000
print("RAM @0x%x : %s" % (va, ram[va : va + 48].hex(" ")))
print("ELF file  : %s" % elf[fo : fo + 48].hex(" "))
print("identical to the ELF file image:", ram[va : va + 0x400] == elf[fo : fo + 0x400])

# where do 4-byte pointers to this block appear (little-endian)?
import struct

needle = struct.pack("<I", va)
hits, s = [], 0
while len(hits) < 8:
    p = ram.find(needle, s)
    if p < 0:
        break
    hits.append(p)
    s = p + 1
print("pointers to 0x%x in RAM at:" % va, [hex(h) for h in hits])
