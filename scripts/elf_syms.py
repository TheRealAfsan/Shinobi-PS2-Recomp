"""Print addresses of named symbols from an ELF32 little-endian file.

usage: elf_syms.py <elf> <name> [<name> ...]     (substring match, case-sensitive)
"""
import struct
import sys

path, wanted = sys.argv[1], sys.argv[2:]
d = open(path, "rb").read()
shoff = struct.unpack_from("<I", d, 0x20)[0]
shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)

sections = []
for i in range(shnum):
    name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(
        "<IIIIIIIIII", d, shoff + i * shentsize
    )
    sections.append((name, typ, flags, addr, off, size, link, info, align, entsize))

for (name, typ, flags, addr, off, size, link, info, align, entsize) in sections:
    if typ != 2:  # SHT_SYMTAB
        continue
    stroff = sections[link][4]
    for i in range(size // 16):
        st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from("<IIIBBH", d, off + i * 16)
        end = d.index(b"\0", stroff + st_name)
        nm = d[stroff + st_name : end].decode("latin1")
        if any(w in nm for w in wanted):
            print("%08x size=%-6d %s" % (st_value, st_size, nm))
