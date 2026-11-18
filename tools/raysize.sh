#!/bin/sh
# Size in bytes of each function that runs from RAM, from the symbol list.
python3 - <<'PY'
rows = [l.split() for l in open('rom/voxelike.lst') if len(l.split()) == 3]
rows = [(int(a, 16), k, n) for a, k, n in rows if a.startswith('060')]
for (a, k, n), (b, _, _) in zip(rows, rows[1:]):
    if k in 'tT':
        print('%6d %s' % (b - a, n))
PY
