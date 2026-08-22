#!/usr/bin/env python3
"""Find direct `call` sites of a kernelcache symbol, with a few preceding instructions.

    callers.py <mangled symbol> [context-instructions]

Answers "who actually uses this, and what do they pass?" — the question that separates an API
that looks right from one Apple's own drivers use. Scans every executable segment for E8 rel32
landing on the symbol, so it finds cross-kext calls, and prints the setup instructions before
each so argument constants are visible.

Only finds *direct* calls. A virtual dispatch (`call [rax+0xNNN]`) is invisible here — use
`tahoe-26.6-slots.txt` to turn the slot into a name instead.

Needs capstone; see kdis.py's docstring for the venv.
"""
import sys, os, struct, bisect
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vtdump import load, v2f_all
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

KC = os.environ.get('ITLWM_KC',
                    '/System/Library/KernelCollections/BootKernelExtensions.kc')


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    sym = sys.argv[1]
    ctx = int(sys.argv[2]) if len(sys.argv) > 2 else 6

    data, top, symbols, segs = load(KC)
    byname = {}
    for v, n in symbols.items():
        byname.setdefault(n, v)
    target = byname.get(sym)
    if target is None:
        print('symbol not found: %s' % sym)
        return
    print('%s @ 0x%x' % (sym, target))
    addrs = sorted(symbols)

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    # Scan every executable segment for E8 rel32 landing on target.
    sites = []
    for vmaddr, vmsize, fileoff, filesize, name in segs:
        if name not in ('__TEXT_EXEC', '__TEXT'):
            continue
        blob = data[fileoff:fileoff + min(vmsize, filesize)]
        start = 0
        while True:
            i = blob.find(b'\xe8', start)
            if i < 0 or i + 5 > len(blob):
                break
            rel = struct.unpack_from('<i', blob, i + 1)[0]
            if vmaddr + i + 5 + rel == target:
                sites.append(vmaddr + i)
            start = i + 1

    print('%d call site(s)\n' % len(sites))
    for site in sites:
        j = bisect.bisect_right(addrs, site) - 1
        owner = '%s+0x%x' % (symbols[addrs[j]], site - addrs[j]) if j >= 0 else '?'
        print('--- %s ---' % owner)
        lo = site - 0x30
        fo = v2f_all(segs, lo)
        if fo is None:
            continue
        insns = list(md.disasm(data[fo:fo + 0x35], lo))
        # keep the tail that ends at the call
        tail = [x for x in insns if x.address <= site][-ctx:]
        for x in tail:
            mark = '  <== call' if x.address == site else ''
            print('  0x%x  %s %s%s' % (x.address, x.mnemonic, x.op_str, mark))
        print()


if __name__ == '__main__':
    main()
