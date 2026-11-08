"""Read the game's GameSystem / SoundCont state out of an EE RAM dump."""
import struct
import sys

ram = open(sys.argv[1], "rb").read()


def u32(a):
    return struct.unpack_from("<I", ram, a & 0x1FFFFFFF)[0]


gs_ptr_addr = 0x482DF4
gs = u32(gs_ptr_addr)
print("gameSystem variable @0x%x -> object 0x%x" % (gs_ptr_addr, gs))
if 0x100000 < gs < 0x2000000:
    print("  +0xaa8 (exit flag A) =", u32(gs + 0xAA8))
    print("  +0xaac (exit flag B) =", u32(gs + 0xAAC))
    sc = u32(gs + 0x69C)
    print("  +0x69c (SoundCont*)  = 0x%x" % sc)
    if 0x100000 < sc < 0x2000000:
        for off in (0x0, 0x4, 0x8, 0xC, 0x10, 0x14):
            print("    SoundCont+0x%x = 0x%x" % (off, u32(sc + off)))
else:
    print("  (not a plausible pointer; showing a few words around the variable)")
    for off in range(-16, 32, 4):
        print("    @0x%x = 0x%x" % (gs_ptr_addr + off, u32(gs_ptr_addr + off)))
