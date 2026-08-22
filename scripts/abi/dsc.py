#!/usr/bin/env python3
"""Address <-> file-offset mapping for the dyld shared cache and its subcaches.

The userspace counterpart to `vtdump.load()`. Apple's private frameworks — Apple80211,
IO80211, CoreWiFi — exist only inside the shared cache, so `otool` and `lldb` both refuse
them when given their nominal /System/Library/PrivateFrameworks path. Everything here works
off the on-disk cache instead.

Library use:

    from dsc import mappings, va_to_file, file_to_va, read_va
    maps = mappings()
    path, off = va_to_file(maps, 0x7ffc1887f158)

As a script, dumps the mapping table and converts single addresses:

    dsc.py                                        # list every mapping
    dsc.py v2f 0x7ffc1887f158                     # VA -> subcache + offset
    dsc.py f2v dyld_shared_cache_x86_64h.04 0x188c0aa0

Stdlib only. Finding a function without local symbols: the caches ship no `.symbols`
subcache, so anchor on a log format string instead — `grep -abo` the literal across the
subcaches, walk back to the preceding NUL for the string start, convert to a VA with `f2v`,
then find the code referencing it with `xref.py`.
"""
import struct, glob, sys, os

# Sequoia and later keep the cache under the Preboot cryptex; older systems used the
# /System/Library path. First one that exists wins.
DIRS = [
    '/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld',
    '/System/Library/dyld',
]
ARCHES = ['x86_64h', 'x86_64', 'arm64e']


def cache_dir():
    for d in DIRS:
        if os.path.isdir(d) and glob.glob(os.path.join(d, 'dyld_shared_cache_*')):
            return d
    raise RuntimeError('no dyld shared cache found in %s' % DIRS)


def base_name(d=None):
    d = d or cache_dir()
    for a in ARCHES:
        p = os.path.join(d, 'dyld_shared_cache_' + a)
        if os.path.isfile(p):
            return 'dyld_shared_cache_' + a
    raise RuntimeError('no primary cache file in %s' % d)


def caches():
    """Primary cache plus its numbered subcaches, in order."""
    d = cache_dir()
    base = os.path.join(d, base_name(d))
    return [base] + sorted(glob.glob(base + '.0*'))


def mappings():
    """[(address, size, fileOffset, path)] across every subcache."""
    out = []
    for f in caches():
        with open(f, 'rb') as fh:
            hdr = fh.read(0x100)
            if hdr[:8] != b'dyld_v1 ':
                continue
            moff, mcnt = struct.unpack_from('<II', hdr, 0x10)
            fh.seek(moff)
            raw = fh.read(32 * mcnt)
        for i in range(mcnt):
            addr, size, foff, _maxp, _inip = struct.unpack_from('<QQQII', raw, i * 32)
            out.append((addr, size, foff, f))
    return out


def va_to_file(maps, va):
    for addr, size, foff, path in maps:
        if addr <= va < addr + size:
            return path, foff + (va - addr)
    return None, None


def file_to_va(maps, path, off):
    for addr, size, foff, p in maps:
        if p == path and foff <= off < foff + size:
            return addr + (off - foff)
    return None


def read_va(maps, va, n):
    path, off = va_to_file(maps, va)
    if path is None:
        return None
    with open(path, 'rb') as fh:
        fh.seek(off)
        return fh.read(n)


def image_ranges():
    """{path: [(segname, start, end)]} parsed from the cache's .map text file."""
    d = cache_dir()
    mp = os.path.join(d, base_name(d) + '.map')
    if not os.path.isfile(mp):
        return {}
    out, cur = {}, None
    for line in open(mp, 'r', errors='replace'):
        if line.startswith('/'):
            cur = line.strip()
            out[cur] = []
        elif cur and '->' in line:
            parts = line.split()
            try:
                out[cur].append((parts[0], int(parts[1], 16), int(parts[3], 16)))
            except (IndexError, ValueError):
                pass
    return out


def owner_of(va, ranges=None):
    """Which cached dylib a VA belongs to, and which of its segments."""
    for path, segs in (ranges or image_ranges()).items():
        for name, lo, hi in segs:
            if lo <= va < hi:
                return path, name
    return None, None


def main():
    maps = mappings()
    if len(sys.argv) >= 3 and sys.argv[1] == 'v2f':
        va = int(sys.argv[2], 0)
        p, o = va_to_file(maps, va)
        img, seg = owner_of(va)
        print('VA 0x%x -> %s +0x%x' % (va, os.path.basename(p or '?'), o or 0))
        if img:
            print('   in %s (%s)' % (img, seg))
        return
    if len(sys.argv) >= 4 and sys.argv[1] == 'f2v':
        path = os.path.join(cache_dir(), sys.argv[2])
        print('0x%x' % (file_to_va(maps, path, int(sys.argv[3], 0)) or 0))
        return
    print('cache dir: %s' % cache_dir())
    print('%d mappings across %d files' % (len(maps), len(caches())))
    for addr, size, foff, path in maps:
        print('  0x%012x +0x%-10x file=0x%-10x %s'
              % (addr, size, foff, os.path.basename(path)))


if __name__ == '__main__':
    main()
