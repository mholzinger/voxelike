#!/usr/bin/env python3
"""Where the SH-2s spend their time, by function.

    ares-headless --frames N --profile prof.csv rom/voxelike.32x
    tools/profile.py prof.csv [function]

Reads the emulator's per-PC profile and the ROM's symbol list. With a
function's name, lists that function's hottest instructions instead.
"""
import bisect, csv, sys

syms = sorted((int(a, 16), n) for a, k, n in
              (l.split() for l in open('rom/voxelike.lst') if len(l.split()) == 3)
              if k in 'tT')
addrs = [a for a, _ in syms]

def owner(pc):
    pc &= 0x07FFFFFF | 0x06000000
    i = bisect.bisect_right(addrs, pc) - 1
    return syms[i] if i >= 0 else (0, '?')

rows = [(r['cpu'], int(r['pc'], 16), int(r['instructions']), int(r['cycles']))
        for r in csv.DictReader(open(sys.argv[1]))]
total = {}
for cpu, pc, n, c in rows:
    total[cpu] = total.get(cpu, 0) + c

if len(sys.argv) > 2:
    want = '_' + sys.argv[2].lstrip('_')
    for cpu, pc, n, c in sorted(rows, key=lambda r: -r[3]):
        base, name = owner(pc)
        if name == want and c * 500 > total[cpu]:
            print('%s %08x +%-5d %10d instr %10d cycles %5.1f%%' %
                  (cpu, pc, (pc & 0x07FFFFFF) - (base & 0x07FFFFFF), n, c, 100.0 * c / total[cpu]))
else:
    by = {}
    for cpu, pc, n, c in rows:
        k = (cpu, owner(pc)[1])
        a = by.setdefault(k, [0, 0])
        a[0] += n
        a[1] += c
    for (cpu, name), (n, c) in sorted(by.items(), key=lambda kv: -kv[1][1])[:24]:
        print('%-4s %-28s %11d instr %11d cycles %5.1f%%  %.2f cycles/instr' %
              (cpu, name, n, c, 100.0 * c / total[cpu], c / max(n, 1)))
