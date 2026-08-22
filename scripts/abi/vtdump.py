#!/usr/bin/env python3
"""Dump C++ vtables out of a Mach-O (incl. kernel collection filesets)."""
import struct, sys, bisect

MH_MAGIC_64 = 0xfeedfacf
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x02
LC_FILESET_ENTRY = 0x80000035


class MachO:
    def __init__(self, data, off=0):
        self.data = data
        self.off = off
        magic, cputype, cpusub, filetype, ncmds, sizeofcmds, flags, res = \
            struct.unpack_from('<IiiIIIII', data, off)
        assert magic == MH_MAGIC_64, hex(magic)
        self.segments = []   # (vmaddr, vmsize, fileoff, filesize, name)
        self.symbols = []    # (value, name)
        self.filesets = []   # (vmaddr, fileoff, name)
        p = off + 32
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from('<II', data, p)
            if cmd == LC_SEGMENT_64:
                name = data[p+8:p+24].rstrip(b'\0').decode()
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', data, p+24)
                self.segments.append((vmaddr, vmsize, fileoff, filesize, name))
            elif cmd == LC_SYMTAB:
                symoff, nsyms, stroff, strsize = struct.unpack_from('<IIII', data, p+8)
                self._syms(symoff, nsyms, stroff)
            elif cmd == LC_FILESET_ENTRY:
                vmaddr, fileoff, entry_id = struct.unpack_from('<QQI', data, p+8)
                nm = data[p+entry_id:data.index(b'\0', p+entry_id)].decode()
                self.filesets.append((vmaddr, fileoff, nm))
            p += cmdsize
        self.segments.sort()

    def _syms(self, symoff, nsyms, stroff):
        d = self.data
        for i in range(nsyms):
            q = symoff + i*16
            n_strx, n_type, n_sect, n_desc, n_value = struct.unpack_from('<IBBHQ', d, q)
            if n_value == 0:
                continue
            e = d.index(b'\0', stroff + n_strx)
            nm = d[stroff+n_strx:e].decode('utf-8', 'replace')
            if nm:
                self.symbols.append((n_value, nm))

    def v2f(self, vaddr):
        for vmaddr, vmsize, fileoff, filesize, _ in self.segments:
            if vmaddr <= vaddr < vmaddr + vmsize and vaddr - vmaddr < filesize:
                return fileoff + (vaddr - vmaddr)
        return None


def load(path):
    data = open(path, 'rb').read()
    top = MachO(data)
    symbols = dict()          # vaddr -> name (first wins)
    for v, n in top.symbols:
        symbols.setdefault(v, n)
    segs = list(top.segments)
    for vmaddr, fileoff, nm in top.filesets:
        sub = MachO(data, fileoff)
        for v, n in sub.symbols:
            symbols.setdefault(v, n)
        segs.extend(sub.segments)
    return data, top, symbols, segs


def v2f_all(segs, vaddr):
    for vmaddr, vmsize, fileoff, filesize, _ in segs:
        if vmaddr <= vaddr < vmaddr + vmsize and vaddr - vmaddr < filesize:
            return fileoff + (vaddr - vmaddr)
    return None


def main():
    path = sys.argv[1]
    data, top, symbols, segs = load(path)
    byname = {}
    for v, n in symbols.items():
        byname.setdefault(n, v)
    addrs = sorted(symbols)
    base = min(s[0] for s in top.segments)

    for cls in sys.argv[2:]:
        mangled = '__ZTV%d%s' % (len(cls), cls)
        va = byname.get(mangled)
        print('=' * 70)
        if va is None:
            print('%s: NO VTABLE SYMBOL' % cls)
            continue
        print('%s  vtable @ 0x%x' % (cls, va))
        fo = v2f_all(segs, va)
        if fo is None:
            print('  (not mapped)')
            continue
        # skip offset-to-top + typeinfo
        idx = 0
        p = fo + 16
        while True:
            raw, = struct.unpack_from('<Q', data, p)
            if raw == 0:
                break
            # DYLD_CHAINED_PTR_64_KERNEL_CACHE: low 30 bits are a base-relative target
            ptr = base + (raw & 0x3FFFFFFF)
            nm = symbols.get(ptr)
            if nm is None:
                i = bisect.bisect_right(addrs, ptr) - 1
                nm = '0x%x (<%s+0x%x>)' % (ptr, symbols[addrs[i]], ptr - addrs[i]) if i >= 0 else '0x%x' % ptr
            print('  [%3d] %s' % (idx, nm))
            idx += 1
            p += 8
            if idx > 900:
                break


if __name__ == '__main__':
    main()
