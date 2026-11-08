"""List the distinct shapes of VU0 arithmetic lines in the generated Shinobi runner (digits normalised)."""
import collections
import os
import re
import sys

runner = sys.argv[1]
names_file = sys.argv[2]
names = set()
for line in open(names_file, encoding="utf-8", errors="ignore"):
    m = re.search(r"([^\\/]+\.cpp):0x", line)
    if m:
        names.add(m.group(1))

shapes = collections.Counter()
example = {}
mn_re = re.compile(r"^\s*// 0x[0-9a-f]+: 0x[0-9a-f]+\s+(v\w+)")
for name in sorted(names):
    path = os.path.join(runner, name)
    if not os.path.exists(path):
        continue
    prev = ""
    for line in open(path, encoding="utf-8", errors="ignore"):
        m = mn_re.match(line)
        if m:
            prev = m.group(1)
            continue
        if line.lstrip().startswith("//") or line.lstrip().startswith("ctx->pc"):
            continue
        if "vu0_" in line and prev:
            shape = re.sub(r"\d+", "N", line.strip())
            key = (prev.split(".")[0], shape[:230])
            shapes[key] += 1
            example.setdefault(key, line.strip()[:300])
        prev = ""
print("files scanned:", len(names))
for (mn, shape), n in shapes.most_common(60):
    print(n, mn)
    print("    ", example[(mn, shape)])
