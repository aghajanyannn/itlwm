#!/usr/bin/env python3
"""Find every access to a struct offset inside one kext of a kernel collection.

Answers "who populates this member?" — the recurring question when a reconstructed class in
include/Airport has a field whose owner is unknown, and when a panic names an offset but not
the code that should have filled it.

Needs capstone, which PEP 668 blocks from the system Python. Use a venv:

    python3 -m venv scripts/abi/.venv && scripts/abi/.venv/bin/pip install capstone
    scripts/abi/.venv/bin/python scripts/abi/findfield.py <bundle-id> <hex off> [<hex off> ...]

Example — the three fields IOPCIDevice::attach publishes, and every reader of them:

    findfield.py com.apple.iokit.IOPCIFamily 198 1a0 1a8

Writes are reported as W, reads as R. An access whose base register was loaded from
[X + <base-offset>] earlier in the same function is tagged "[via base]", which separates real
accesses to an expansion struct from unrelated structures sharing the same offset. Pass
--base=<hex> to change that offset; it defaults to 0xa0, IOPCIDevice::reserved.

Pass a bundle id that does not exist to list the available ones.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vtdump import MachO, load, v2f_all
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_OP_REG

KC = os.environ.get('ITLWM_KC',
                    '/System/Library/KernelCollections/BootKernelExtensions.kc')
DEFAULT_BASE_OFF = 0xa0          # IOPCIDevice::reserved
MAX_FUNC = 0x8000                # cap on an unbounded trailing symbol

# Mnemonic prefixes whose first operand is written.
WRITERS = ('mov', 'and', 'or', 'xor', 'add', 'sub', 'inc', 'dec', 'lock',
           'cmpxchg', 'xchg', 'btr', 'bts', 'set')


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    base_off = DEFAULT_BASE_OFF
    for a in sys.argv[1:]:
        if a.startswith('--base='):
            base_off = int(a.split('=', 1)[1], 16)
    if not args:
        print(__doc__)
        return
    bundle = args[0]
    want = [int(x, 16) for x in args[1:]]
    if not want:
        print('no offsets given')
        return

    data = open(KC, 'rb').read()
    top = MachO(data)
    fs = [f for f in top.filesets if f[2] == bundle]
    if not fs:
        print('kext not found: %s\navailable:' % bundle)
        for f in sorted(top.filesets, key=lambda x: x[2]):
            print('   ', f[2])
        return
    sub = MachO(data, fs[0][1])
    _, _, symbols, segs = load(KC)
    addrs = sorted(symbols)

    code = [s for s in sub.segments if s[4] in ('__TEXT_EXEC', '__TEXT')]
    lo = min(s[0] for s in code)
    hi = max(s[0] + s[1] for s in code)
    print('%s  code 0x%x - 0x%x  (base offset 0x%x)' % (bundle, lo, hi, base_off))

    funcs = [a for a in addrs if lo <= a < hi]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    hits = {o: [] for o in want}
    for i, start in enumerate(funcs):
        end = min(funcs[i + 1] if i + 1 < len(funcs) else hi, start + MAX_FUNC)
        fo = v2f_all(segs, start)
        if fo is None:
            continue
        name = symbols.get(start, '0x%x' % start)
        base_regs = set()

        for ins in md.disasm(data[fo:fo + (end - start)], start):
            ops = ins.operands
            is_base_load = (ins.mnemonic == 'mov' and len(ops) == 2 and
                            ops[0].type == X86_OP_REG and ops[1].type == X86_OP_MEM and
                            ops[1].mem.disp == base_off)
            if is_base_load:
                base_regs.add(ops[0].reg)
            elif ops and ops[0].type == X86_OP_REG and ops[0].reg in base_regs:
                base_regs.discard(ops[0].reg)   # clobbered, no longer the base pointer

            if is_base_load:
                continue
            for idx, op in enumerate(ops):
                if op.type != X86_OP_MEM or op.mem.disp not in want:
                    continue
                is_write = idx == 0 and ins.mnemonic.startswith(WRITERS)
                hits[op.mem.disp].append(
                    (name, ins.address - start, 'W' if is_write else 'R',
                     '%s %s' % (ins.mnemonic, ins.op_str),
                     ' [via base]' if op.mem.base in base_regs else ''))

    for o in want:
        print('\n' + '=' * 78)
        print('offset 0x%x  --  %d access(es), %d write(s)'
              % (o, len(hits[o]), sum(1 for h in hits[o] if h[2] == 'W')))
        print('=' * 78)
        for name, off, kind, text, via in hits[o]:
            print('  %s  %-44s +0x%-5x %-38s%s' % (kind, name, off, text, via))


if __name__ == '__main__':
    main()
