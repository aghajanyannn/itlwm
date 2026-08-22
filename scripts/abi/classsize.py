#!/usr/bin/env python3
"""Recover OSMetaClass classSize for classes in a kernel collection.

The metaclass object's classSize field is written at runtime, so it reads 0 on
disk. The value is instead recovered from the MetaClass constructor, which passes
it as OSMetaClass()'s 3rd argument:

    OSMetaClass::OSMetaClass(const char *name, const OSMetaClass *super, unsigned size)
      rdi=this  rsi=name  rdx=super  ecx=size

so the constructor contains `mov ecx, imm32` (B9 imm32) immediately followed by the
`call` (E8) into OSMetaClass::OSMetaClass. Anchoring on the call matters: a bare 0xB9
scan also matches bytes inside RIP-relative displacements.

usage: classsize.py <kernel collection> <class> [<class> ...]
"""
import struct, sys
from vtdump import load, v2f_all


def main():
    path = sys.argv[1]
    data, top, symbols, segs = load(path)
    byname = {}
    for v, n in symbols.items():
        byname.setdefault(n, v)

    for cls in sys.argv[2:]:
        va = (byname.get('__ZN%d%s9MetaClassC2Ev' % (len(cls), cls))
              or byname.get('__ZN%d%s9MetaClassC1Ev' % (len(cls), cls)))
        if va is None:
            print('%-30s no MetaClass ctor symbol' % cls)
            continue
        fo = v2f_all(segs, va)
        if fo is None:
            print('%-30s ctor not mapped' % cls)
            continue
        blob = data[fo:fo + 160]
        hits = []
        for i in range(len(blob) - 10):
            if blob[i] == 0xB9 and blob[i + 5] == 0xE8:
                imm, = struct.unpack_from('<I', blob, i + 1)
                if 0 < imm < 0x100000:
                    hits.append(imm)
        if not hits:
            print('%-30s no mov ecx,imm32 + call found' % cls)
        else:
            print('%-30s size = %6d (0x%x)%s'
                  % (cls, hits[0], hits[0],
                     '   [also saw %s]' % hits[1:] if len(hits) > 1 else ''))


if __name__ == '__main__':
    main()
