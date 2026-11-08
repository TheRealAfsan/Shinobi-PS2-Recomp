"""Read the Ninja store buffer state out of an EE RAM dump."""
import struct
import sys

ram = open(sys.argv[1], "rb").read()


def u32(a):
    return struct.unpack_from("<I", ram, a & 0x1FFFFFFF)[0]


def s32(a):
    return struct.unpack_from("<i", ram, a & 0x1FFFFFFF)[0]


print("dma channel   (_nj_storebufer_dma_ch_  @0x4829d8) =", u32(0x4829D8))
print("buffer count  (_nj_storebuffer_max_    @0x4829e4) =", s32(0x4829E4))
print("chain max     (_nj_storebuffer_chain_max_ @0x4829d4) =", s32(0x4829D4))
print("chain index   (_nj_storebuffer_chain_index_ @0x4829dc) =", s32(0x4829DC))
print("debug flag    (_nj_storebuffer_debug_  @0x4829e0) =", s32(0x4829E0))
sb = u32(0x4829E8)
print("store buffer array pointer (_nj_storebuffer_ @0x4829e8) = 0x%x" % sb)
print("current index [0],[1] (_nj_storebuffer_current_ @0x481fe8) =", s32(0x481FE8), s32(0x481FEC))
if 0x100000 < sb < 0x2000000:
    print("header words:", " ".join("%x" % u32(sb + o) for o in range(0, 0x40, 4)))
    n = s32(0x4829E4)
    for i in range(min(max(n, 0), 4)):
        # entry layout used by _hySystemBufferKick: entry = base + index*0x20 ... fields at +0x30..+0x4c
        base = sb + i * 0x70
        print("entry %d @0x%x:" % (i, base), " ".join("%x" % u32(base + o) for o in range(0, 0x70, 4)))
