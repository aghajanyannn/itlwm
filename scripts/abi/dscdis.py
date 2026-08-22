#!/usr/bin/env python3
"""Disassemble a VA range of the dyld shared cache, annotating C strings.

    dscdis.py <start-va> <end-va> [--sync=<va>]

x86 is variable-length, so an arbitrary start VA usually lands mid-instruction and capstone
yields nothing at all. `--sync=<va>` brute-forces a start offset whose instruction stream
lands exactly on that address — pass the known-good address found with `xref.py` and the
whole enclosing function decodes cleanly. It picks the earliest such start, which maximises
context but often begins inside the previous function's tail, so expect a few lines of
nonsense before the stream settles; judge the output from the first recognisable prologue or
from the target address backwards.

Every rip-relative operand pointing at printable, NUL-terminated bytes is annotated with the
string, which is how anonymous functions get identified in a cache that ships no local
symbol table: the log format strings name the function they sit in.

Needs capstone, which PEP 668 blocks from the system Python. Use a venv:

    python3 -m venv scripts/abi/.venv && scripts/abi/.venv/bin/pip install capstone
    scripts/abi/.venv/bin/python scripts/abi/dscdis.py 0x7ffc1887df58 0x7ffc1887f160
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dsc import mappings, va_to_file, read_va
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_MEM

MAX_SYNC_BACK = 0x1200   # how far back to hunt for an instruction boundary


def cstr(maps, va, limit=120):
    b = read_va(maps, va, limit)
    if not b:
        return None
    end = b.find(b'\x00')
    if end <= 0:
        return None
    s = b[:end]
    if all(32 <= c < 127 or c in (9, 10) for c in s):
        return s.decode('ascii')
    return None


def fetch(maps, lo, hi):
    path, off = va_to_file(maps, lo)
    if path is None:
        return None
    with open(path, 'rb') as fh:
        fh.seek(off)
        return fh.read(hi - lo)


def find_sync(maps, target, hi):
    """Earliest start VA whose instruction stream lands exactly on `target`."""
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    for back in range(MAX_SYNC_BACK, 0x10, -1):
        lo = target - back
        buf = fetch(maps, lo, max(hi, target + 16))
        if not buf:
            continue
        if target in (i.address for i in md.disasm(buf, lo)):
            return lo
    return None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    sync = None
    for a in sys.argv[1:]:
        if a.startswith('--sync='):
            sync = int(a.split('=', 1)[1], 0)
    if len(args) < 2:
        print(__doc__)
        return
    lo, hi = int(args[0], 0), int(args[1], 0)

    maps = mappings()
    if sync is not None:
        s = find_sync(maps, sync, hi)
        if s is None:
            print('could not sync to 0x%x' % sync)
            return
        print('; synced start 0x%x (target 0x%x)' % (s, sync))
        lo = s

    buf = fetch(maps, lo, hi)
    if not buf:
        print('0x%x is not mapped' % lo)
        return
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    n = 0
    for ins in md.disasm(buf, lo):
        note = ''
        if 'rip' in ins.op_str:
            for op in ins.operands:
                if op.type == CS_OP_MEM:
                    tgt = ins.address + ins.size + op.mem.disp
                    s = cstr(maps, tgt)
                    note = '   ; 0x%x' % tgt
                    if s:
                        note += ' "%s"' % s.replace('\n', '\\n')
        print('0x%x  %-46s%s' % (ins.address, '%s %s' % (ins.mnemonic, ins.op_str), note))
        n += 1
    if n == 0:
        print('; nothing decoded — start VA is probably mid-instruction, use --sync=<va>')


if __name__ == '__main__':
    main()
