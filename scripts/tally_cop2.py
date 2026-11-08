"""Tally COP2/VU0-macro mnemonics from a ripgrep -o dump of the generated runner (file:0xINSN  mnemonic  per line)."""
import collections
import re
import sys

counts = collections.Counter()
files = collections.defaultdict(set)
for line in open(sys.argv[1], encoding="utf-8", errors="ignore"):
    m = re.search(r"([^\\/]+\.cpp):0x[0-9a-f]{8}\s+(\w+)\s", line)
    if m:
        counts[m.group(2)] += 1
        files[m.group(2)].add(m.group(1))
for name, n in counts.most_common():
    print(name, n, len(files[name]))
