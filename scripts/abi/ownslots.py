#!/usr/bin/env python3
"""For each class in the chain, list the virtuals it INTRODUCES (slots beyond its
base's vtable length), for both the 14.4 headers and the Tahoe kernel, side by side.
That is exactly the declaration list that has to be edited in each header."""
import re, subprocess, sys, difflib

CHAIN = [
    ('IOSkywalkInterface',        None),
    ('IOSkywalkNetworkInterface', 'IOSkywalkInterface'),
    ('IOSkywalkEthernetInterface','IOSkywalkNetworkInterface'),
    ('IO80211SkywalkInterface',   'IOSkywalkEthernetInterface'),
    ('IO80211InfraInterface',     'IO80211SkywalkInterface'),
    ('IO80211InfraProtocol',      'IO80211InfraInterface'),
]


def parse_clang(path, cls):
    lines = open(path).read().splitlines()
    hits = [i for i, l in enumerate(lines) if l.startswith("Vtable for '%s' (" % cls)]
    if not hits:
        return None
    out = []
    for l in lines[hits[-1] + 1:]:
        if l.startswith('Vtable for') or l.startswith('VTable indices') or l.strip() == '':
            break
        m = re.match(r'\s*(\d+) \| (.*)', l)
        if not m:
            continue
        txt = m.group(2).strip()
        if (txt.startswith('offset_to_top') or txt.endswith('RTTI')
                or txt.startswith('vcall_offset') or txt.startswith('vbase_offset')):
            continue
        out.append(txt)
    return out


def parse_os(path, cls):
    lines = open(path).read().splitlines()
    hits = [i for i, l in enumerate(lines) if l.startswith(cls + '  vtable @')]
    if not hits:
        return None
    out = []
    for l in lines[hits[0] + 1:]:
        m = re.match(r'\s*\[\s*(\d+)\] (.*)', l)
        if not m:
            break
        out.append(m.group(2).strip())
    p = subprocess.run(['c++filt'], input='\n'.join(out), capture_output=True, text=True)
    return p.stdout.splitlines()


def sig(t):
    """normalise a display signature down to name+arity for comparison"""
    t = re.sub(r'\s*\[.*$', '', t)
    name = t.split('(')[0]
    name = name.rsplit('::', 1)[1] if '::' in name else name.split()[-1]
    args = t[t.find('('):] if '(' in t else ''
    return name.strip('*& '), args.count(',') + (0 if args in ('', '()') else 1)


def main():
    hdr_file, os_file = sys.argv[1], sys.argv[2]
    for cls, base in CHAIN:
        h = parse_clang(hdr_file, cls)
        o = parse_os(os_file, cls)
        if h is None or o is None:
            print('!! %s missing (hdr=%s os=%s)' % (cls, h is not None, o is not None))
            continue
        hb = len(parse_clang(hdr_file, base)) if base else 0
        ob = len(parse_os(os_file, base)) if base else 0
        print('=' * 100)
        print('%s   header total=%d (base %d, own %d)   TAHOE total=%d (base %d, own %d)'
              % (cls, len(h), hb, len(h) - hb, len(o), ob, len(o) - ob))
        print('=' * 100)
        hown, oown = h[hb:], o[ob:]
        sm = difflib.SequenceMatcher(a=[sig(x) for x in hown],
                                     b=[sig(x) for x in oown], autojunk=False)
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag == 'equal':
                for k in range(i1, i2):
                    print('   =  %4d  %s' % (hb + k, hown[k]))
                continue
            for k in range(i1, i2):
                print('   -  %4d  %s' % (hb + k, hown[k]))
            for k in range(j1, j2):
                print('   +  %4d  %s' % (ob + k, oown[k]))
        print()


if __name__ == '__main__':
    main()
