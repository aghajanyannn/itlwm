"""Find virtual call sites for a vtable byte offset, across every kext, with enclosing symbol.

Answers "who calls this slot?", which `callers.py` cannot: that finds direct calls only.
Slot index N is byte offset N*8, so slot 432 is `slotcall.py d80`.

    slotcall.py d80        # -> IO80211Controller::findAndAttachToFaultReporter +0x26

TWO WAYS THIS LIES, both of which have already produced a wrong conclusion in this repo:

1. A zero result does NOT mean "never called virtually". Only `call qword ptr [reg + disp]` is
   matched. A tail-dispatched virtual compiles to `mov rax, [rax + disp]` then `jmp rax` and is
   invisible here — that is exactly how `apple80211setWCL_SCAN_REQ` reaches slot 601, and how
   Apple reaches `prepareBSDInterface` (slot 285).
2. The same displacement means different methods in unrelated class hierarchies, so most hits
   for a given offset are noise. Filter by the family you care about.

**Always self-test before trusting a zero.** Pick a slot with a known call site and confirm it
comes back. An earlier version swept whole segments in one `md.disasm()` call, which stops at the
first undecodable byte and silently covered a fraction of the kernel; it reported zero for
everything and looked authoritative. Disassembling per symbol range is what fixed it.

Needs capstone, which PEP 668 blocks from the system Python. Use a venv:

    python3 -m venv scripts/abi/.venv && scripts/abi/.venv/bin/pip install capstone
    scripts/abi/.venv/bin/python scripts/abi/slotcall.py <hex byte offset>
"""
import sys, os, bisect
sys.path.insert(0,'scripts/abi')
from vtdump import load, v2f_all
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
disp = int(sys.argv[1], 16)
KC = os.environ.get('ITLWM_KC',
                    '/System/Library/KernelCollections/BootKernelExtensions.kc')
data, top, symbols, segs = load(KC)
addrs = sorted(symbols)
md = Cs(CS_ARCH_X86, CS_MODE_64)
needle = '+ 0x%x]' % disp
hits = []
MAXFN = 0x20000
for i, va in enumerate(addrs):
    end = addrs[i+1] if i+1 < len(addrs) else va + 0x40
    n = min(end - va, MAXFN)
    if n <= 0:
        continue
    fo = v2f_all(segs, va)
    if fo is None:
        continue
    for ins in md.disasm(data[fo:fo+n], va):
        if ins.mnemonic != 'call' or not ins.op_str.startswith('qword ptr ['):
            continue
        if needle in ins.op_str:
            hits.append((symbols[va], ins.address - va))
seen = {}
for s, o in hits:
    seen.setdefault(s, o)
for s, o in seen.items():
    print('  %-95s +0x%x' % (s, o))
print('total %d call sites, %d distinct functions' % (len(hits), len(seen)))
