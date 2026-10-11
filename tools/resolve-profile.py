"""Resolve a --bully_profile CSV (module-relative function addresses) to names using the linker map.

usage: python -I resolve-profile.py <BullyRecomp.map> <name.csv.profile.csv> [top_n]
"""
import bisect
import re
import sys

map_path, profile_path = sys.argv[1], sys.argv[2]
top_n = int(sys.argv[3]) if len(sys.argv) > 3 else 40

symbols = {}
pattern = re.compile(r"^\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s")
base = 0x140000000
with open(map_path, errors="replace") as handle:
    for line in handle:
        match = pattern.match(line)
        if not match:
            continue
        name, va = match.group(1), int(match.group(2), 16)
        rva = va - base
        # prefer the plain name over the __imp__ alias or a mangled host name
        if rva not in symbols or (symbols[rva].startswith("__imp__") and not name.startswith("__imp__")):
            symbols[rva] = name
starts = sorted(symbols)

def resolve(rva):
    i = bisect.bisect_right(starts, rva) - 1
    if i < 0:
        return f"?{rva:x}"
    return symbols[starts[i]] + ("" if starts[i] == rva else f"+{rva - starts[i]:#x}")

rows, total = [], 0
with open(profile_path) as handle:
    header = handle.readline()
    total = int(header.strip().split("total=")[1])
    for line in handle:
        rva, self_count, inclusive = line.strip().split(",")
        rows.append((int(rva, 16), int(self_count), int(inclusive)))

print(f"samples: {total}")
print(f"\nTop {top_n} by self time")
print(f"{'self%':>6} {'incl%':>6}  function")
for rva, s, i in sorted(rows, key=lambda r: -r[1])[:top_n]:
    print(f"{100*s/total:6.2f} {100*i/total:6.2f}  {resolve(rva)}")
print(f"\nTop {top_n} by inclusive time")
for rva, s, i in sorted(rows, key=lambda r: -r[2])[:top_n]:
    print(f"{100*s/total:6.2f} {100*i/total:6.2f}  {resolve(rva)}")
