#!/usr/bin/env python3
"""Dump a WCL manager's FSM states, events, and driver-message subscriptions.

Every `WCL*Manager::initWCL*Manager` passes a static descriptor to
`WCLFsmManager::initWithOptions`. That descriptor is the authoritative answer to "which
`apple80211` message number do we post to move this manager's FSM, and what does the handler
expect?" — the question that cost a long hunt for the scan path before message 237 was found.

    scripts/abi/.venv/bin/python scripts/abi/wclfsm.py WCLJoinManager

Descriptor layout, recovered on 26.6:

    state-handler function pointers   (pairs, 16 bytes each)
    state-name  char*[]               "<MGR>_STATE_IDLE" first  -> state 0
    event-name  char*[]               "<MGR>_EVENT_..."  first  -> event 0
    char*  manager name               "JOIN_MANAGER"
    0
    subscription table                24-byte entries, all-zero entry terminates

A subscription entry is `{u16 msgType; u16 msgnum; u32 pad; handler_fn; 0}`. msgType comes from
`WCLGlue::receiveMessageInternal`, which packs `(msgnum << 16) | msgType` and passes the driver's
message number through **untranslated**:

    0 = apple80211 GET ioctl      3 = WCL-internal notification
    1 = apple80211 SET ioctl      4 = msgnum 0x17f only
    2 = message posted by the driver (postMessage)  <-- the ones a driver owes

Pointers in the descriptor are chained fixups, so the stored value is a file offset; the VA is
`fileoff + 0xffffff8000100000` (see include/Airport/AGENTS.md).

Sanity-check the decoding against a known answer before trusting it on a new manager:
`wclfsm.py WCLScanManager` must show `type=2 num=237 -> scanDoneEventHandler`.

Needs capstone, which PEP 668 blocks from the system Python. Use the repo venv:

    python3 -m venv scripts/abi/.venv && scripts/abi/.venv/bin/pip install capstone
"""
import sys, os, bisect, struct

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vtdump import load, v2f_all
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

KC = os.environ.get('ITLWM_KC',
                    '/System/Library/KernelCollections/BootKernelExtensions.kc')
BASE = 0xffffff8000100000
INIT_WITH_OPTIONS = '__ZN13WCLFsmManager15initWithOptionsER20WCLFsmManagerOptions'

MSGTYPE = {
    0: 'GET ioctl',
    1: 'SET ioctl',
    2: 'DRIVER postMessage',
    3: 'WCL-internal notify',
    4: 'msg 0x17f',
}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    mgr = sys.argv[1]

    data, top, symbols, segs = load(KC)
    addrs = sorted(symbols)
    byname = {}
    for v, nm in symbols.items():
        byname.setdefault(nm, v)

    def resolve(t):
        if t in symbols:
            return symbols[t]
        i = bisect.bisect_right(addrs, t) - 1
        if i >= 0 and t - addrs[i] < 0x8000:
            return '%s+0x%x' % (symbols[addrs[i]], t - addrs[i])
        return '0x%x' % t

    def cstr(fo, limit=160):
        if not (0 < fo < len(data)):
            return None
        e = data.find(b'\0', fo, fo + limit)
        if e <= fo:
            return None
        try:
            s = data[fo:e].decode('ascii')
        except UnicodeDecodeError:
            return None
        return s if s.isprintable() else None

    # Most managers name it after themselves (WCLJoinManager -> initWCLJoinManager), but some just
    # use initWithOptions, so match any member of the class whose method name starts with "init".
    prefix = '__ZN%d%s' % (len(mgr), mgr)
    va, init = None, None
    for nm, v in sorted(byname.items(), key=lambda kv: kv[1]):
        if not nm.startswith(prefix):
            continue
        rest = nm[len(prefix):]
        i = 0
        while i < len(rest) and rest[i].isdigit():
            i += 1
        if i and rest[i:i + 4] == 'init':
            va, init = v, nm
            break
    if va is None:
        print('no init* symbol on class %s' % mgr)
        return 1

    # Collect `lea reg, [rip + X]` targets up to the initWithOptions call.
    fo = v2f_all(segs, va)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    leas = []
    for ins in md.disasm(data[fo:fo + 0x1200], va):
        if ins.mnemonic == 'lea' and 'rip +' in ins.op_str:
            disp = int(ins.op_str.split('rip +')[1].split(']')[0].strip(), 16)
            leas.append(ins.address + ins.size + disp)
        if ins.mnemonic.startswith('call'):
            op = ins.op_str.strip()
            if op.startswith('0x') and symbols.get(int(op, 16)) == INIT_WITH_OPTIONS:
                break
        if ins.mnemonic == 'ret':
            break

    def owning_class(nm):
        """'__ZN14WCLScanManager18scanRequest...' -> 'WCLScanManager'."""
        i = 4
        while i < len(nm) and nm[i].isdigit():
            i += 1
        try:
            return nm[i:i + int(nm[4:i])]
        except ValueError:
            return ''

    def decode_table(tbl_fo):
        """Decode 24-byte subscription entries at a file offset; [] if this is not a table.

        The tables of adjacent managers sit back to back in __const with no terminator between
        them, and the entry count lives in the options struct rather than in the data. So the
        boundary is drawn where the handler's owning class stops being this manager — without
        that, WCLScanManager's table runs straight on into WCLConfigManager's.
        """
        out, n = [], 0
        while True:
            w0, w1, w2 = struct.unpack_from('<QQQ', data, tbl_fo + n * 24)
            if (w0 == 0 and w1 == 0) or w2 != 0 or (w0 >> 32) != 0:
                break
            mtype, mnum = w0 & 0xffff, (w0 >> 16) & 0xffff
            if mtype > 4 or mnum > 0x1000:
                break
            hva = (w1 & 0xffffffffff) + BASE
            nm = resolve(hva)
            if not nm.startswith('__ZN'):
                break                       # handler must be a real function symbol
            if owning_class(nm) != mgr:
                break                       # next manager's table starts here
            out.append((mtype, mnum, nm))
            n += 1
        return out

    # The lea before initWithOptions points at the *subscription table*; the state- and
    # event-name arrays precede it, so find the table first and walk backwards for the names.
    tbl_fo, subs = None, []
    for t in reversed(leas):
        try:
            cand = v2f_all(segs, t)
        except Exception:
            continue
        if cand is None:
            continue
        got = decode_table(cand)
        if got:
            tbl_fo, subs = cand, got
            break
    if tbl_fo is None:
        print('%s: no subscription table among %d lea targets' % (mgr, len(leas)))
        return 1

    # Backwards: zero terminator, manager name, then the event and state name arrays.
    names, off, names_fo = [], tbl_fo - 8, tbl_fo
    while off > tbl_fo - 0x600:
        q = struct.unpack_from('<Q', data, off)[0]
        s = cstr(q & 0xffffffffff)
        if s:
            names.append(s)
            names_fo = off                  # lowest string pointer seen == array start
        elif q != 0:
            break                           # hit the state-handler pointers
        off -= 8
    names.reverse()
    states = [s for s in names if '_STATE_' in s]
    events = [s for s in names if '_EVENT_' in s]

    def decode_fsm(nst, nev):
        """(handlers, transition rows) laid out immediately before the name arrays, or None.

        CommonFsmManager::processEvent reads the config as
            table   = cfg[0x00]     nst*nev entries of {u8 nextState, u8 handlerIndex}
            handler = cfg[0x08]     16-byte member-function-pointer pairs
            names   = cfg[0x10] / cfg[0x18]
        and dispatches `table[curState*cfg[0x29] + event]`, where 0xff for nextState means
        "stay". The config itself is a __bss global filled by __GLOBAL__sub_I_<Mgr>.cpp, so it
        cannot be read from the file — but the four arrays it points at sit contiguously in
        __const, ending at the name arrays, which is enough to walk backwards from.
        """
        if not nst or not nev:
            return None
        hi = names_fo                       # handler array ends where the names begin
        lo = hi
        while lo - 16 > hi - 0x400:
            raw = struct.unpack_from('<Q', data, lo - 16)[0]
            if not raw:
                break
            nm = resolve((raw & 0xffffffffff) + BASE)
            if owning_class(nm) != mgr:
                break                       # walked off the front of the handler array
            lo -= 16
        nh = (hi - lo) // 16
        if nh < 2:
            return None
        handlers = [resolve((struct.unpack_from('<Q', data, lo + i * 16)[0] & 0xffffffffff) + BASE)
                    for i in range(nh)]
        # The table does *not* end where the handler array begins. The handler array is 16-byte
        # aligned, so anything between the table's true end and it is padding, and the table's own
        # start is 16-byte aligned too — round down to recover it. Only managers whose nst*nev*2 is
        # a multiple of 16 escape this: WCLScanManager (4*10*2 = 80) decoded correctly for months
        # while WCLNetManager (7*11*2 = 154, 6 bytes of pad) was silently rotated by three entries,
        # which reads as a plausible table with every handler attached to the wrong event.
        tfo = lo - nst * nev * 2
        tfo -= (tfo + BASE) % 16
        tbl = data[tfo:lo]
        rows = []
        for s in range(nst):
            row = []
            for e in range(nev):
                ns, h = tbl[(s * nev + e) * 2], tbl[(s * nev + e) * 2 + 1]
                if ns != 0xff and ns >= nst:
                    return None             # not the transition table; refuse to guess
                if h >= nh:
                    return None
                row.append((ns, h))
            rows.append(row)
        return handlers, rows, tfo, lo

    print('%s' % init)
    print('\nstates:')
    for n, s in enumerate(states):
        print('  %2d  %s' % (n, s))
    print('\nevents:')
    for n, s in enumerate(events):
        print('  %2d  %s' % (n, s))

    fsm = decode_fsm(len(states), len(events))
    if fsm is None:
        print('\ntransitions: could not locate the table')
    else:
        handlers, rows, tfo, hfo = fsm
        print('\nstate handlers (0x%x):' % (hfo + BASE))
        for n, h in enumerate(handlers):
            print('  %2d  %s' % (n, h))
        # Only the meaningful (state, event) pairs are printed. An entry that both stays in the
        # current state (0xff) and runs `ignore` is the unset one — the FSM accepts the event and
        # does nothing — and that is also what a driver posting the wrong message, or the right
        # messages in the wrong order, looks like from outside: nothing happens and nothing is
        # logged. Pairs that change state under `ignore` are kept; they are real transitions.
        def short(n):
            return n.split('_STATE_', 1)[-1].split('_EVENT_', 1)[-1]

        print('\ntransitions (0x%x), no-op pairs omitted:' % (tfo + BASE))
        for s, row in enumerate(rows):
            print('  %s:' % short(states[s]))
            for e, (ns, h) in enumerate(row):
                if ns == 0xff and h == 0:
                    continue
                nxt = states[s] if ns == 0xff else states[ns]
                print('    %-28s -> %-18s %s' % (short(events[e]), short(nxt), handlers[h]))

    print('\nsubscriptions (table 0x%x):' % (tbl_fo + BASE))
    for mtype, mnum, nm in subs:
        print('  type=%d %-20s num=%-4d (0x%03x)  %s'
              % (mtype, MSGTYPE.get(mtype, '?'), mnum, mnum, nm))
    return 0


if __name__ == '__main__':
    sys.exit(main())
