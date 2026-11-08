"""Patch the generated Shinobi code for the PEXEH / PEXCH / PEXCW / PROT3W translation bugs.

Each generated instruction is preceded by a comment like
    // 0x346bd4: 0x700216c9  prevh       $v0, $v0
and the translation is on one of the next few lines. The old translations used the same shuffle text for
PEXEH and PEXCH, so the comment decides which one it is. (PEXEW goes through the PS2_PEXEW macro, fixed in
ps2_runtime_macros.h; PREVH was patched separately.)

usage: patch_mmi_shuffles.py <runner_dir>
"""
import os
import re
import sys

runner = sys.argv[1]
mn_re = re.compile(rb"^\s*// 0x[0-9a-f]+: 0x[0-9a-f]+\s+(pexeh|pexch|pexcw|prot3w)\s")

OLD_HALF = b"_MM_SHUFFLE(2,3,0,1)"
NEW = {
    b"pexeh": (OLD_HALF, b"_MM_SHUFFLE(3,0,1,2)"),   # halfword shuffles (lo and hi): 0<->2
    b"pexch": (OLD_HALF, b"_MM_SHUFFLE(3,1,2,0)"),   # halfword shuffles: 1<->2
    b"pexcw": (b"_MM_SHUFFLE(1,0,3,2)", b"_MM_SHUFFLE(3,1,2,0)"),  # word shuffle: 1<->2
    b"prot3w": (b"_MM_SHUFFLE(0,3,2,1)", b"_MM_SHUFFLE(3,0,2,1)"),  # rotate low three words
}

files = patched_files = patched_lines = 0
for name in os.listdir(runner):
    if not name.endswith(".cpp"):
        continue
    path = os.path.join(runner, name)
    with open(path, "rb") as f:
        data = f.read()
    files += 1
    if not any(m in data for m in (b"pexeh ", b"pexch ", b"pexcw ", b"prot3w ")):
        continue
    lines = data.split(b"\n")
    changed = False
    for i, line in enumerate(lines):
        m = mn_re.match(line)
        if not m:
            continue
        mnem = m.group(1)
        old, new = NEW[mnem]
        for j in range(i + 1, min(i + 6, len(lines))):
            if b"_mm_shuffle" in lines[j]:
                if old in lines[j]:
                    lines[j] = lines[j].replace(old, new)
                    patched_lines += 1
                    changed = True
                break
    if changed:
        with open(path, "wb") as f:
            f.write(b"\n".join(lines))
        patched_files += 1

print("scanned %d files, patched %d lines in %d files" % (files, patched_lines, patched_files))
