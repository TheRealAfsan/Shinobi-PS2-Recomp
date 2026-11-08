"""Find live MlTask objects in an EE RAM dump by vtable pointer at +0x30 and print their state."""
import mmap
import struct
import sys

with open(sys.argv[1], "rb") as f:
    ram = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)


def u32(a):
    return struct.unpack_from("<I", ram, a)[0]


vts = {
    "Autoload": 0x479F00,
    "MainTask": 0x475CD0,
    "LogoDisp": 0x4772A0,
    "MainTitle": 0x475F10,
    "TitleMovie": 0x475E50,
    "SetUp": 0x47B660,
    "MemoryCardAccessor": 0x479F80,
}
for name, vt in vts.items():
    needle = struct.pack("<I", vt)
    s, hits = 0, []
    while len(hits) < 4:
        p = ram.find(needle, s)
        if p < 0:
            break
        s = p + 1
        # a task object has the vtable at +0x30; accept hits aligned to 4 bytes
        if p % 4 == 0 and p >= 0x30:
            hits.append(p - 0x30)
    print("%-20s objects at: %s" % (name, [hex(h) for h in hits]))
    for obj in hits[:2]:
        words = [u32(obj + o) for o in range(0, 0x60, 4)]
        print("    obj 0x%x:" % obj, " ".join("%x" % w for w in words))
        if name in ("Autoload", "MemoryCardAccessor"):
            print("      MemoryCardAccessor mainMode(+0x44)=0x%x subStep(+0x48)=0x%x" % (u32(obj + 0x44), u32(obj + 0x48)))
        if name == "MainTask":
            print("      MainTask state(+0x34)=%d saved(+0x38)=%d sub-task-flag(+0x48)=%d sub-task-id(+0x4c)=0x%x"
                  % (u32(obj + 0x34), u32(obj + 0x38), u32(obj + 0x48), u32(obj + 0x4C)))
