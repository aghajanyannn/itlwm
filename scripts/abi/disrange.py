#!/usr/bin/env python3
"""Disassemble a byte range of one function, by symbol plus start/end offset.

`kdis.py` counts instructions forward from a function's entry, which is the wrong handle when
a panic points at a large function and the interesting code is far in — a backtrace gives
`symbol + 0xNNN`, so address the block directly:

    disrange.py __ZN11IOPCIDevice6attachEP9IOService 570 640

Offsets are hex and relative to the symbol, so they can be copied straight out of a panic
backtrace frame. Output is offset-prefixed for the same reason.

x86 is variable-length, so a start offset in the middle of the function usually lands
mid-instruction and capstone then decodes nothing at all. Offset 0 is always safe; for
anything else pass `--sync`, which walks the start forward until the stream reaches the
requested end offset.

Needs capstone, which PEP 668 blocks from the system Python. Use a venv:

    python3 -m venv scripts/abi/.venv && scripts/abi/.venv/bin/pip install capstone
    scripts/abi/.venv/bin/python scripts/abi/disrange.py <mangled symbol> <hex start> <hex end>

A `call [rax + 0xNNN]` is a virtual call on vtable slot `0xNNN / 8`; look the index up in
`tahoe-26.6-slots.txt`. Slot 0x28 is `OSObject::release`.
"""
import sys, os, bisect
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vtdump import load, v2f_all
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

KC = os.environ.get('ITLWM_KC',
                    '/System/Library/KernelCollections/BootKernelExtensions.kc')
MAX_SPAN = 0x8000        # furthest a branch target may sit past a symbol and still resolve to it


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    sync = any(a == '--sync' for a in sys.argv[1:])
    if len(args) < 3:
        print(__doc__)
        return
    sym = args[0]
    start_off = int(args[1], 16)
    end_off = int(args[2], 16)

    data, top, symbols, segs = load(KC)
    byname = {}
    for v, n in symbols.items():
        byname.setdefault(n, v)
    addrs = sorted(symbols)

    va = byname.get(sym)
    if va is None:
        print('symbol not found: %s' % sym)
        return
    fo = v2f_all(segs, va)
    if fo is None:
        print('%s not mapped' % sym)
        return

    def resolve(t):
        if t in symbols:
            return symbols[t]
        i = bisect.bisect_right(addrs, t) - 1
        if i >= 0 and t - addrs[i] < MAX_SPAN:
            return '%s+0x%x' % (symbols[addrs[i]], t - addrs[i])
        return '0x%x' % t

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    if sync:
        # Nudge the start forward until the stream actually reaches end_off. A start that
        # lands mid-instruction either decodes nothing or desyncs and stops early.
        for delta in range(0, 16):
            probe = start_off + delta
            reached = [i.address - va for i in
                       md.disasm(data[fo + probe:fo + end_off], va + probe)]
            if reached and max(reached) >= end_off - 8:
                if delta:
                    print('; synced start +0x%x (requested +0x%x)' % (probe, start_off))
                start_off = probe
                break

    n = 0
    for ins in md.disasm(data[fo + start_off:fo + end_off], va + start_off):
        note = ''
        if ins.mnemonic.startswith(('call', 'j')) and ins.op_str.startswith('0x'):
            try:
                note = '   ; ' + resolve(int(ins.op_str, 16))
            except ValueError:
                pass
        print('+0x%-5x %-42s%s' % (ins.address - va, '%s %s' % (ins.mnemonic, ins.op_str), note))
        n += 1
    if n == 0:
        print('; nothing decoded — start offset is mid-instruction, retry with --sync')


if __name__ == '__main__':
    main()
