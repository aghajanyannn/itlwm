#!/usr/bin/env python3
"""Disassemble a function out of a kernel collection, resolving branch targets to symbols.

Needs capstone, which PEP 668 blocks from the system Python. Use a venv:

    python3 -m venv scripts/abi/.venv && scripts/abi/.venv/bin/pip install capstone
    scripts/abi/.venv/bin/python scripts/abi/kdis.py <mangled symbol> [instruction count]

Do not name this file dis.py — that shadows the stdlib module capstone imports.
"""
import sys, bisect, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vtdump import load, v2f_all
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

KC = os.environ.get('ITLWM_KC',
                    '/System/Library/KernelCollections/BootKernelExtensions.kc')


def main():
    sym = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 120
    data, top, symbols, segs = load(KC)
    byname = {}
    for v, nm in symbols.items():
        byname.setdefault(nm, v)
    addrs = sorted(symbols)

    va = byname.get(sym)
    if va is None:
        print('symbol not found: %s' % sym)
        return
    fo = v2f_all(segs, va)
    code = data[fo:fo + n * 8]

    def resolve(t):
        if t in symbols:
            return symbols[t]
        i = bisect.bisect_right(addrs, t) - 1
        if i >= 0 and t - addrs[i] < 0x4000:
            return '%s+0x%x' % (symbols[addrs[i]], t - addrs[i])
        return None

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    count = 0
    for ins in md.disasm(code, va):
        note = ''
        if ins.mnemonic.startswith(('call', 'j')):
            op = ins.op_str.strip()
            if op.startswith('0x'):
                nm = resolve(int(op, 16))
                if nm:
                    note = '   ; %s' % nm
        print('0x%x  %-8s %s%s' % (ins.address, ins.mnemonic, ins.op_str, note))
        count += 1
        if ins.mnemonic == 'ret' or count >= n:
            break


if __name__ == '__main__':
    main()
