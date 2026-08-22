#!/usr/bin/env python3
"""Print a Mach-O kext binary's LC_UUID, to confirm which build is actually deployed.

Stdlib only, and deliberately no macOS dependency: the usual reason to ask this question is from
another OS while mounting the EFI partition, where dwarfdump and lipo do not exist.

    scripts/kextuuid.py /mnt/efi/EFI/OC/Kexts/AirportItlwm.kext/Contents/MacOS/AirportItlwm
    scripts/kextuuid.py <path> --expect 4D27207E-AE53-389E-A4B6-CDF3BC888FCC

With --expect it exits non-zero on a mismatch, so it can gate a copy. Deploying a stale kext is a
failure this port has already had: a build reported success while silently skipping a target, and
the binary on disk still held code that had just been deleted from the source.
"""
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
MH_CIGAM_64 = 0xCFFAEDFE
FAT_MAGIC = 0xCAFEBABE
FAT_CIGAM = 0xBEBAFECA
LC_UUID = 0x1B


def uuid_of_thin(data, off, little):
    end = '<' if little else '>'
    # mach_header_64: magic cputype cpusubtype filetype ncmds sizeofcmds flags reserved
    ncmds = struct.unpack_from(end + 'I', data, off + 16)[0]
    pos = off + 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from(end + 'II', data, pos)
        if cmdsize == 0:
            break
        if cmd == LC_UUID:
            raw = data[pos + 8:pos + 24]
            h = raw.hex().upper()
            return '%s-%s-%s-%s-%s' % (h[0:8], h[8:12], h[12:16], h[16:20], h[20:32])
        pos += cmdsize
    return None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    expect = None
    if '--expect' in sys.argv:
        expect = sys.argv[sys.argv.index('--expect') + 1].strip().upper()

    with open(path, 'rb') as f:
        data = f.read()
    if len(data) < 32:
        print('not a Mach-O: too short')
        return 3

    magic = struct.unpack_from('>I', data, 0)[0]
    found = []
    if magic in (FAT_MAGIC, FAT_CIGAM):
        end = '>' if magic == FAT_MAGIC else '<'
        nfat = struct.unpack_from(end + 'I', data, 4)[0]
        for i in range(nfat):
            # fat_arch: cputype cpusubtype offset size align
            off = struct.unpack_from(end + 'I', data, 8 + i * 20 + 8)[0]
            m = struct.unpack_from('>I', data, off)[0]
            u = uuid_of_thin(data, off, m == MH_CIGAM_64)
            if u:
                found.append(u)
    else:
        m = struct.unpack_from('>I', data, 0)[0]
        if m not in (MH_MAGIC_64, MH_CIGAM_64):
            print('not a 64-bit Mach-O (magic %#x)' % m)
            return 3
        u = uuid_of_thin(data, 0, m == MH_CIGAM_64)
        if u:
            found.append(u)

    if not found:
        print('no LC_UUID found')
        return 3
    for u in found:
        print(u)

    if expect is not None:
        if expect in found:
            print('OK: matches expected build')
            return 0
        print('MISMATCH: expected %s' % expect)
        print('          the deployed kext is NOT the build you think it is')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
