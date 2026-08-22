#!/bin/sh
# Verify the vtable indices of classes the driver CALLS virtuals on but never subclasses.
#
#   scripts/abi/callcheck.sh [abi-baseline.txt]
#
# `mapdrv.py` checks the slots we override. This checks the slots we invoke, which no other tool
# covers and which fail silently: a wrong index dispatches to a different method of the same class,
# so the kext builds, loads, and misbehaves. Compares scripts/abi/callprobe.cpp's emitted vtables
# against the release baseline slot-for-slot, by name.
#
# Needs a prior `xcodebuild -target AirportItlwm-Tahoe -configuration Release build`, because it
# reuses that target's real compiler arguments — a hand-rolled clang line gets __IO80211_TARGET
# wrong and silently drops declarations.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BASE=${1:-$HERE/abi-26.6.1-25G76.txt}
PY=$HERE/.venv/bin/python3
[ -x "$PY" ] || PY=python3

RESP=$(ls "$ROOT/build/itlwm.build/Release/AirportItlwm-Tahoe.build/Objects-normal/x86_64/"*common-args.resp 2>/dev/null | head -1)
if [ -z "$RESP" ]; then
    echo "no common-args.resp; build AirportItlwm-Tahoe first" >&2
    exit 2
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# -include the pch explicitly: common-args.resp does not carry it (Xcode passes it separately),
# and without it __IO80211_TARGET evaluates to 0 and every >= test flips.
clang -x c++ -w -isysroot "$(xcrun --sdk macosx --show-sdk-path)" \
      -target x86_64-apple-macos10.15 "@$RESP" -I"$ROOT/include" \
      -Xclang -fdump-vtable-layouts -S -emit-llvm -o /dev/null \
      -include "$ROOT/itlwm/PrivateSPI.pch" "$HERE/callprobe.cpp" > "$TMP/vt.txt" 2>&1

BASE="$BASE" VT="$TMP/vt.txt" "$PY" - <<'PY'
import os, re, sys, subprocess

# probe class -> the Apple class it stands in for.
# IOSkywalkPacket is intentionally absent: the driver reaches it through direct calls to exported
# symbols, which the linker verifies on every build. See the note at the top of callprobe.cpp.
PROBES = {'ProbeSegment': 'IOSkywalkMemorySegment',
          'ProbeQueue':   'IOSkywalkPacketQueue'}

# The baseline stores MANGLED names; clang's dump is demangled. Demangle in one c++filt pass
# rather than pattern-matching the mangling, which is how this check first "failed" on six
# correct slots.
raw = {}
for line in open(os.environ['BASE']):
    p = line.split(None, 2)
    if len(p) == 3 and p[1].isdigit() and p[0] in PROBES.values():
        raw.setdefault(p[0], {})[int(p[1])] = p[2].strip()
syms = sorted({s for d in raw.values() for s in d.values()})
out = subprocess.run(['c++filt'] + syms, capture_output=True, text=True).stdout.splitlines()
dem = dict(zip(syms, out))
base = {c: {s: dem.get(v, v) for s, v in d.items()} for c, d in raw.items()}


def method(sig):
    """'IOReturn Cls::foo(int) const' / 'Cls::foo()' -> 'foo'."""
    sig = re.sub(r'\(.*', '', sig).strip()
    return sig.split('::')[-1].split()[-1] if sig else ''


txt = open(os.environ['VT']).read().splitlines()
bad = total = 0
for probe, cls in PROBES.items():
    hits = [i for i, l in enumerate(txt) if l.startswith("Vtable for '%s'" % probe)]
    if not hits:
        print('FAIL %s: no vtable emitted (needs a key function)' % probe); bad += 1; continue
    for l in txt[hits[-1] + 1:]:
        m = re.match(r'\s*(\d+) \| (.*)', l)
        if not m:
            if l.strip() == '' or l.startswith('Vtable'):
                break
            continue
        # clang counts offset_to_top and RTTI ahead of slot 0
        slot, entry = int(m.group(1)) - 2, m.group(2).strip()
        # Destructors and the trailing vcall_offset carry the probe's own name, never Apple's.
        if 'vcall_offset' in entry or '~' in entry:
            continue
        want = base.get(cls, {}).get(slot)
        if want is None:
            continue
        got, exp = method(entry), method(want)
        total += 1
        if got != exp:
            print('WRONG %s slot %d: header=%s kernel=%s' % (cls, slot, got, exp)); bad += 1

print('checked %d called-slot classes, %d slots, WRONG: %d' % (len(PROBES), total, bad))
sys.exit(1 if bad else 0)
PY
