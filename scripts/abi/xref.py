#!/usr/bin/env python3
"""Find RIP-relative references to an address inside a VA range of the dyld shared cache.

The second half of locating an unsymbolised function: `dsc.py` turns a string's file offset
into a VA, this finds the code that loads it.

    xref.py <target-va> <range-start> <range-end>

Ranges come from the cache's .map (see `dsc.image_ranges()`), e.g. IO80211's __TEXT.

Stdlib only — it pattern-matches `lea reg, [rip+disp32]` (REX.W/REX.WR + 0x8D with mod=00,
rm=101) rather than disassembling, which is enough to find string loads and avoids needing a
valid instruction boundary to start from.

Note a format string is referenced by its START, so xref the beginning of the literal, not
the middle of it. Walk back from a `grep -abo` hit to the preceding NUL first.
"""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dsc import mappings, va_to_file

REGS = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi']


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return
    target = int(sys.argv[1], 0)
    lo = int(sys.argv[2], 0)
    hi = int(sys.argv[3], 0)

    maps = mappings()
    path, off = va_to_file(maps, lo)
    if path is None:
        print('range start 0x%x is not mapped' % lo)
        return
    with open(path, 'rb') as fh:
        fh.seek(off)
        buf = fh.read(hi - lo)

    hits = 0
    for i in range(len(buf) - 7):
        b0, b1, b2 = buf[i], buf[i + 1], buf[i + 2]
        if b0 in (0x48, 0x4C) and b1 == 0x8D and (b2 & 0xC7) == 0x05:
            disp = struct.unpack_from('<i', buf, i + 3)[0]
            if lo + i + 7 + disp == target:
                reg = REGS[(b2 >> 3) & 7]
                if b0 == 0x4C:
                    reg = 'r%d' % (8 + ((b2 >> 3) & 7))
                print('0x%x  lea %s, [rip + ...]  -> 0x%x' % (lo + i, reg, target))
                hits += 1
    if not hits:
        print('no rip-relative lea targeting 0x%x in 0x%x-0x%x' % (target, lo, hi))


if __name__ == '__main__':
    main()
