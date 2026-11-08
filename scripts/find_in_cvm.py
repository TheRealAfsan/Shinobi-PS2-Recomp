"""Find where a file's first bytes sit inside the CVM and compare with a game read position.

usage: find_in_cvm.py <file> <cvm> <game_lbn_hex> <pseudo_base_hex>
"""
import mmap
import sys

file_path, cvm_path, game_lbn, base = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4], 16)

with open(file_path, "rb") as f:
    head = f.read(64)
print("file first 32 bytes:", head[:32].hex(), head[:4])

with open(cvm_path, "rb") as cvm:
    m = mmap.mmap(cvm.fileno(), 0, access=mmap.ACCESS_READ)
    pos = m.find(head)
    # every occurrence, in case of duplicates
    hits = []
    start = 0
    while len(hits) < 5:
        p = m.find(head, start)
        if p < 0:
            break
        hits.append(p)
        start = p + 1

print("occurrences at CVM byte offsets:", [hex(h) for h in hits])
if hits:
    p = hits[0]
    print("first hit => sector %d (0x%x), remainder %d bytes" % (p // 2048, p // 2048, p % 2048))
rel = game_lbn - base
print("game read lbn 0x%x - pseudo base 0x%x = sector 0x%x => byte offset 0x%x" % (game_lbn, base, rel, rel * 2048))
if hits:
    print("difference (file position - game read position) in sectors:", hits[0] / 2048 - rel)
