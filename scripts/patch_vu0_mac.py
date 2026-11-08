"""Patch the generated Shinobi code for the VU0 macro-mode bugs (same fixes as ps2xRecomp/src/lib/vu_translator.cpp
addVu0MacUpdate + vu_translation_helpers.cpp):
  * arithmetic never updated the MAC/status flags that `cfc2 $vi17` reads (the game's clip tests) -> PS2_VU0_MAC(...)
  * vdiv gave 0 for x/0 (VU: +-max float); vsqrt/vrsqrt ignore sign; vrsqrt ignored fs (Q = fs / sqrt(|ft|)).

usage: patch_vu0_mac.py <runner_dir>      (idempotent)
"""
import os
import re
import sys

runner = sys.argv[1]

WITH_MASK = re.compile(rb"(__m128 res = PS2_V(?:ADD|SUB|MUL)\([^;]*\); __m128i mask = _mm_set_epi32\([^)]*\);)")
TO_ACC = re.compile(
    rb"(__m128 res = PS2_V(?:ADD|SUB|MUL)\([^;]*\); )(ctx->vu0_acc = _mm_blendv_ps\(ctx->vu0_acc, res, "
    rb"_mm_castsi128_ps\((_mm_set_epi32\([^)]*\))\)\);)")
OLD_DIV = b"ctx->vu0_q = (ft != 0.0f) ? (fs / ft) : 0.0f;"
NEW_DIV = b"ctx->vu0_q = ps2_vu0_div(fs, ft);"
OLD_SQRT = b"ctx->vu0_q = sqrtf(std::max(0.0f, ft));"
NEW_SQRT = b"ctx->vu0_q = ps2_vu0_sqrt(ft);"
OLD_RSQRT = b"ctx->vu0_q = (ft > 0.0f) ? (1.0f / sqrtf(ft)) : 0.0f; }"
RSQRT_OPERANDS = re.compile(rb"vrsqrt\s+\$?Q,\s*\$vf(\d+)([xyzw]),\s*\$vf(\d+)([xyzw])")
LANE = {b"x": 0, b"y": 1, b"z": 2, b"w": 3}

files = patched_files = mac = div = sqrt_n = rsqrt = 0
leftover = []
for name in os.listdir(runner):
    if not name.endswith(".cpp"):
        continue
    path = os.path.join(runner, name)
    with open(path, "rb") as f:
        data = f.read()
    files += 1
    if b"vu0_vf" not in data:
        continue
    lines = data.split(b"\n")
    changed = False
    last_rsqrt_comment = None
    for i, line in enumerate(lines):
        if b"vrsqrt" in line and line.lstrip().startswith(b"//"):
            last_rsqrt_comment = line
        if b"PS2_VU0_MAC" in line:
            continue
        new = line
        new, n1 = WITH_MASK.subn(rb"\1 PS2_VU0_MAC(ctx, res, mask);", new)
        new, n2 = TO_ACC.subn(rb"\1PS2_VU0_MAC(ctx, res, \3); \2", new)
        mac += n1 + n2
        if OLD_DIV in new:
            new = new.replace(OLD_DIV, NEW_DIV)
            div += 1
        if OLD_SQRT in new:
            new = new.replace(OLD_SQRT, NEW_SQRT)
            sqrt_n += 1
        if OLD_RSQRT in new and last_rsqrt_comment:
            m = RSQRT_OPERANDS.search(last_rsqrt_comment)
            if m:
                fs, fsf, ft, ftf = int(m.group(1)), LANE[m.group(2)], int(m.group(3)), LANE[m.group(4)]
                # keep the existing `float ft = ...;` prefix of the line, add fs, replace the Q assignment
                head = new.split(b"ctx->vu0_q =")[0]
                fs_expr = ("float fs = _mm_cvtss_f32(_mm_shuffle_ps(ctx->vu0_vf[%d], ctx->vu0_vf[%d], _MM_SHUFFLE(0,0,0,%d))); "
                           % (fs, fs, fsf)).encode()
                # the original head is "{ float ft = ...; " -> insert fs after the opening brace
                head = head.replace(b"{ float ft", b"{ " + fs_expr + b"float ft", 1)
                new = head + b"ctx->vu0_q = ps2_vu0_rsqrt(fs, ft); }" + new.split(OLD_RSQRT)[1]
                rsqrt += 1
        if new != line:
            lines[i] = new
            changed = True
        elif b"__m128 res = PS2_V" in line and b"PS2_VU0_MAC" not in line and b"vu0_acc, mul_res" not in line:
            leftover.append((name, line.strip()[:200]))
    if changed:
        with open(path, "wb") as f:
            f.write(b"\n".join(lines))
        patched_files += 1

print("scanned %d files, patched %d; MAC inserts=%d vdiv=%d vsqrt=%d vrsqrt=%d" % (files, patched_files, mac, div, sqrt_n, rsqrt))
print("arithmetic lines left without a MAC update: %d" % len(leftover))
seen = set()
for name, line in leftover:
    shape = re.sub(rb"\d+", b"N", line)
    if shape in seen:
        continue
    seen.add(shape)
    print("  ", name, line.decode("utf-8", "replace"))
    if len(seen) >= 12:
        break
