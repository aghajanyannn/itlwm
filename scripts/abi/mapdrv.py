#!/usr/bin/env python3
"""Map the driver's overridden vtable slots (built against the 14.4 headers)
onto the real Tahoe slot at the same index, and report mismatches."""
import re, subprocess, sys


def parse_clang(path, cls):
    lines = open(path).read().splitlines()
    st = [i for i, l in enumerate(lines) if l.startswith("Vtable for '%s' (" % cls)][-1]
    out = []
    for l in lines[st + 1:]:
        if l.startswith('Vtable for') or l.startswith('VTable indices'):
            break
        m = re.match(r'\s*(\d+) \| (.*)', l)
        if not m:
            continue
        idx, txt = int(m.group(1)), m.group(2).strip()
        if txt.startswith('offset_to_top') or txt.endswith('RTTI') or txt.startswith('vcall_offset'):
            continue
        out.append(txt)
    return out


def parse_os(path):
    lines = open(path).read().splitlines()
    i = [k for k, l in enumerate(lines) if 'vtable @' in l][0]
    out = []
    for l in lines[i + 1:]:
        m = re.match(r'\s*\[\s*(\d+)\] (.*)', l)
        if not m:
            break
        out.append(m.group(2))
    p = subprocess.run(['c++filt'], input='\n'.join(out), capture_output=True, text=True)
    return p.stdout.splitlines()


def name_of(t, strip_cls=True):
    t = re.sub(r'\s*\[.*$', '', t).split('(')[0]
    if '::' in t:
        t = t.rsplit('::', 1)[1]
    else:
        t = t.split()[-1]
    return t.strip('*& ')


def main():
    clang_file, os_file, drv_cls, os_cls = sys.argv[1:5]
    hdr = parse_clang(clang_file, drv_cls)
    os_v = parse_os(os_file)
    ok = mism = 0
    print('=' * 90)
    print('%s (built vs 14.4 headers) vs Tahoe %s' % (drv_cls, os_cls))
    print('header slots=%d  Tahoe slots=%d' % (len(hdr), len(os_v)))
    print('=' * 90)
    for i, txt in enumerate(hdr):
        if drv_cls + '::' not in txt:
            continue          # not a driver override
        hn = name_of(txt)
        if hn.startswith('~'):
            continue
        on = name_of(os_v[i]) if i < len(os_v) else '<OUT OF RANGE>'
        if hn == on:
            ok += 1
        else:
            mism += 1
            print('slot %4d  driver:%-42s Tahoe:%s' % (i, hn, on))
    print('-' * 90)
    print('driver overrides landing on the CORRECT Tahoe slot: %d' % ok)
    print('driver overrides landing on the WRONG   Tahoe slot: %d' % mism)


if __name__ == '__main__':
    main()
