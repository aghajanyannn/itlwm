#!/usr/bin/env python3
"""Map symbols to the kext (fileset entry) that owns them, in a kernel collection."""
import re, struct, sys, bisect
from vtdump import MachO


def build(path):
    data = open(path, 'rb').read()
    top = MachO(data)
    ranges = []          # (start, end, kext)
    syms = {}            # name -> vaddr
    for v, n in top.symbols:
        syms.setdefault(n, v)
    for vmaddr, fileoff, nm in top.filesets:
        sub = MachO(data, fileoff)
        for s in sub.segments:
            if s[1]:
                ranges.append((s[0], s[0] + s[1], nm))
        for v, n in sub.symbols:
            syms.setdefault(n, v)
    ranges.sort()
    return data, ranges, syms


def owner(ranges, starts, va):
    i = bisect.bisect_right(starts, va) - 1
    while i >= 0:
        s, e, nm = ranges[i]
        if s <= va < e:
            return nm
        i -= 1
    return '<kernel/unknown>'


def main():
    path = sys.argv[1]
    pattern = re.compile(sys.argv[2])
    data, ranges, syms = build(path)
    starts = [r[0] for r in ranges]
    from collections import Counter, defaultdict
    per = defaultdict(list)
    for n, v in syms.items():
        m = re.match(r'__ZTV(\d+)(.*)', n)
        if not m:
            continue
        cls = m.group(2)[:int(m.group(1))]
        if not pattern.search(cls):
            continue
        per[owner(ranges, starts, v)].append(cls)
    for k in sorted(per):
        cls = sorted(set(per[k]))
        print('%-55s %3d classes' % (k, len(cls)))
        for c in cls[:60]:
            print('      %s' % c)


if __name__ == '__main__':
    main()
