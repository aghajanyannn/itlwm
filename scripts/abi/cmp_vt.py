#!/usr/bin/env python3
"""Align a clang -fdump-vtable-layouts table against a real OS vtable dump."""
import re, subprocess, sys, difflib


def parse_clang(path, cls):
    """Return list of (slot, display, key) for the LAST 'Vtable for cls' block."""
    want = "Vtable for '%s' (" % cls
    lines = open(path).read().splitlines()
    starts = [i for i, l in enumerate(lines) if l.startswith(want)]
    if not starts:
        return None
    out = []
    for l in lines[starts[-1] + 1:]:
        m = re.match(r'\s*(\d+) \| (.*)', l)
        if not m:
            if out:
                break
            continue
        idx, txt = int(m.group(1)), m.group(2).strip()
        if txt.startswith('offset_to_top') or txt.endswith('RTTI'):
            continue
        out.append((idx, txt))
    return out


def key_clang(txt):
    """Reduce 'void IO80211X::foo(int)' -> 'foo'."""
    t = txt
    t = re.sub(r'\s*\[.*$', '', t)          # drop [deleted], [pure] markers
    t = t.split('(')[0]
    if '::' in t:
        t = t.rsplit('::', 1)[1]
    else:
        t = t.split()[-1]
    return t.strip('*& ')


def parse_os(path, cls):
    lines = open(path).read().splitlines()
    hdr = [i for i, l in enumerate(lines) if l.startswith(cls + '  vtable @')]
    if not hdr:
        return None
    out = []
    for l in lines[hdr[0] + 1:]:
        m = re.match(r'\s*\[\s*(\d+)\] (.*)', l)
        if not m:
            break
        out.append((int(m.group(1)), m.group(2).strip()))
    return out


def demangle(names):
    p = subprocess.run(['c++filt'], input='\n'.join(names), capture_output=True, text=True)
    return p.stdout.splitlines()


def key_os(dem):
    t = dem.split('(')[0]
    if '::' in t:
        t = t.rsplit('::', 1)[1]
    return t.strip('*& ')


def main():
    clang_file, os_file, cls = sys.argv[1], sys.argv[2], sys.argv[3]
    c = parse_clang(clang_file, cls)
    o = parse_os(os_file, cls)
    if c is None:
        print('%-30s clang: MISSING' % cls); return
    if o is None:
        print('%-30s OS: MISSING (class absent from kernelcache)' % cls); return
    od = demangle([n for _, n in o])
    ck = [key_clang(t) for _, t in c]
    ok = [key_os(d) for d in od]
    print('=' * 78)
    print('%s: header=%d slots  OS(Tahoe)=%d slots  delta=%+d'
          % (cls, len(ck), len(ok), len(ok) - len(ck)))
    print('=' * 78)
    sm = difflib.SequenceMatcher(a=ck, b=ok, autojunk=False)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == 'equal':
            continue
        print('--- %s  header[%d:%d] vs OS[%d:%d]' % (tag.upper(), i1, i2, j1, j2))
        for i in range(i1, i2):
            print('    header %4d  %s' % (i, c[i][1]))
        for j in range(j1, j2):
            print('    OS     %4d  %s' % (j, od[j]))


if __name__ == '__main__':
    main()
