# AGENTS.md — scripts

## Purpose

Build-time and developer-machine helpers. Nothing here ships inside a kext.

## Ownership

- `fw_gen.sh` — run by the `fw_gen` Xcode target; packs `itlwm/firmware/*` into
  `include/FwBinary.cpp`.
- `zlib_compress_fw.py` — compresses firmware blobs for `fw_gen.sh`.
- `load.sh`, `unload.sh` — local `kextutil` load/unload of a Debug `itlwm.kext`.
- `kextuuid.py` — print a kext binary's `LC_UUID`, optionally gated with `--expect`, to confirm
  which build is actually deployed. Stdlib only and no macOS dependency **on purpose**: the moment
  this question matters is usually while mounting the EFI partition from another OS, where
  `dwarfdump` and `lipo` do not exist. Also note the build tree is on APFS, which Linux typically
  cannot read — a kext must be staged from macOS *before* rebooting, or it will be unreachable.
- `ifpred.c` — userspace probe printing the inputs to IO80211's Wi-Fi-interface test
  (`SIOCGIFMEDIA` / `SIOCGIFTYPE` / `SIOCGIFFUNCTIONALTYPE`), so an adoption failure is
  visible without a reboot. Reports the first gate only, which is necessary but not
  sufficient — see "Interface adoption by airportd" in `AirportItlwm/AGENTS.md`.
- `abi/` — tools for reading Apple's shipped binaries: the kernel collection, to verify
  `include/Airport` against a release, and the dyld shared cache, for the userspace side of
  the Wi-Fi stack.
  **`$ITLWM_KC` selects which collection the disassembly tools read** — `callers.py`,
  `disrange.py`, `kdis.py`, `wclfsm.py`, `findfield.py`, `slotcall.py` — defaulting to the
  running system's. Point it at an archive under `kc/` to ask about a superseded release:
  `export ITLWM_KC=scripts/abi/kc/BootKernelExtensions-26.6-25G72.kc`. This matters most
  immediately after an OS update, when the default silently becomes the *new* release and a tool
  answers confidently about the wrong kernel. The remaining tools take the collection path as
  their first argument instead. **Self-test after switching collections** —
  `wclfsm.py WCLScanManager` must still show `num=237` — because a wrong path yields empty
  results rather than an error.
  - `vtdump.py` — dump a class vtable out of a kernel collection.
  - `cmp_vt.py` — align a clang `-fdump-vtable-layouts` table against a dumped vtable.
  - `ownslots.py` — per class, list the virtuals it introduces, header vs kernel. This is
    the view that maps onto declaration edits.
  - `mapdrv.py` — check every driver override lands on a correctly named slot.
  - `whichkext.py` — map classes to the kext that owns them, to tell shared
    `IO80211Family` infrastructure from vendor-private code.
  - `classsize.py` — recover `OSMetaClass::classSize` from the MetaClass constructor.
  - `kdis.py` — disassemble a function out of a kernel collection with branch targets
    resolved to symbols. Never rename it back to `dis.py`, which shadows the stdlib module
    capstone imports.
  - `disrange.py` — disassemble a byte *range* of a symbol. Use this, not `kdis.py`, when a
    panic frame gives `symbol + 0xNNN` and the code of interest is far into a large function.
  - `findfield.py` — every read and write of a struct offset within one kext, so
    "who populates this member?" is answerable without reading a whole family. Tags accesses
    reached through a chosen base field (default `0xa0`, `IOPCIDevice::reserved`) to separate
    an expansion struct from unrelated types sharing an offset.
    **Reach for this the moment a deadline or predicate reads a field you cannot account for.**
    Chasing the missed-beacon teardown by following function names outward from
    `handleMissedBeacons` produced two confident wrong answers over as many boots; one
    `findfield.py 0x150 0x158` named the sole writer of both timestamps immediately. Filtering the
    write list by the class that owns the read is what makes it decisive — a bare offset hits
    every unrelated type in the kext.
  - `slotcall.py` — virtual call sites for a vtable byte offset (slot index × 8), the counterpart
    to `callers.py` for methods reached through a vtable. **Self-test it against a known call site
    before believing a zero result**: it matches only `call qword ptr [reg + disp]`, so a
    tail-dispatched virtual (`mov rax, [rax+disp]` / `jmp rax`) is invisible, and an earlier
    segment-sweeping version silently covered a fraction of the kernel because `md.disasm()` stops
    at the first undecodable byte. Both failure modes read as authoritative zeros.
  - `callers.py` — direct `call` sites of a symbol, across every kext, with the argument setup
    before each. Use it to check whether Apple's own drivers use an API the way a header
    suggests: `setInterfaceSubType` turned out to have two callers, both wired ethernet, which
    is why it is not the route to a Wi-Fi interface. Finds direct calls only; virtual dispatch
    needs `tahoe-26.6-slots.txt`.
  - `tahoe-26.6-slots.txt` — the authoritative macOS 26.6 (25G72) slot reference. Slot
    numbers are vtable *indices*, so a disassembled `call [rax + 0xNNN]` maps to slot
    `0xNNN / 8`. Hand-curated own-slots view with commentary; read this one to *understand*
    a class.
  - `snapshot.sh` + `abi-<ver>-<build>.txt` — the exhaustive, unannotated companion: every
    slot of every class this repo reconstructs, every class a real Skywalk registration would
    need, and Apple's concrete reference subclasses, one `CLASS slot symbol` line each, plus
    recovered instance sizes. Nothing in it is a judgement, so nothing in it goes stale, and
    two releases' files `diff` directly.
    **Run `snapshot.sh > abi-<new>.txt` before installing any macOS update.** That is step 1 of
    the full procedure in the root AGENTS.md, "Surviving a macOS update" — which is what to
    follow when an update is actually pending, because the baseline is necessary and far from
    sufficient.
    `/System/Library/KernelCollections` is replaced wholesale and no other copy of the old
    layout exists on the machine; without a baseline, porting to the new release means
    re-deriving every layout instead of reading a delta. This is not a hypothetical — the
    separate `AirportItlwm-Sonoma14.0` and `AirportItlwm-Sonoma14.4` targets exist because
    Apple changed this ABI inside a point release.
    The slot dumps answer vtable questions only. `kdis.py`, `disrange.py`, `findfield.py`,
    `slotcall.py`, `callers.py` and `wclfsm.py` all need the collection *binary*, so the text
    baseline alone does not keep a release disassemblable.
  - `kc/` — archived kernel collections, one per supported release, named
    `BootKernelExtensions-<ver>-<build>.kc`. Every `abi/` tool takes a collection path as its
    first argument, so an archived one is a drop-in for the live
    `/System/Library/KernelCollections` copy and keeps a superseded release fully
    disassemblable. Gitignored (tens of megabytes of Apple's binaries); the `.sha256` beside
    each is tracked so a copy can be shown to be the genuine shipped artifact.
    `SystemKernelExtensions.kc` is deliberately **not** archived — every `IO80211*`,
    `IOSkywalk*` and `AppleBCMWLAN*` class lives in the boot collection.
    Copy the collection before installing an update, not after: it is replaced wholesale.
    **This was missed once already**: the machine reached 26.6.2 (25G83) with only 25G72 and 25G76
    archived, so `$ITLWM_KC` had been defaulting to an unarchived kernel and every ABI check in the
    repo was last made against a release the machine no longer runs. 25G83 is archived now and its
    verification record is in the root `AGENTS.md`. The tell is cheap and worth making a habit:
    `sw_vers` against `ls scripts/abi/kc/`.

  - `callcheck.sh` + `callprobe.cpp` — verify the vtable indices of classes the driver **calls
    virtuals on but never subclasses**: `IOSkywalkMemorySegment` and `IOSkywalkPacketQueue`
    (`IOSkywalkPacket` was dropped once its methods became direct calls to exported symbols, which
    the linker checks on every build — see the rule in `include/Airport/AGENTS.md`). `mapdrv.py`
    structurally cannot: it checks slots we override. A wrong index here dispatches to a different
    method of the same class and has no build-time, load-time or `mapdrv` symptom. Needs a prior
    `AirportItlwm-Tahoe` build, because it reuses that target's real compiler arguments. Must end
    `WRONG: 0` (currently 94 slots across 2 classes).
    The `IOSkywalkPacketQueue` probe is there for `enable`/`disable`, which are **inherited**
    `IOEventSource` slots 42/43 — so what it really pins is that MacKernelSDK's `IOEventSource`
    matches Apple's, which nothing else in this repo checks. A shift there would send `enable()`
    somewhere else and leave the whole Skywalk data path silently inert.
    **Self-test by inserting one virtual** into the probed header and confirming the reported
    *cascade* — a single insertion shifts every later slot, so a working checker reports several
    wrong slots, not one. A declaration-only class whose table ends at the insertion point
    correctly reports exactly one; check the slot number is where you inserted it.
  - `wclfsm.py` — a WCL manager's FSM states, events, **state × event transition matrix** and
    **message subscriptions**, from the static descriptor its `initWCL*Manager` passes to
    `WCLFsmManager::initWithOptions`. This is the direct answer to "which `apple80211` message
    number moves this manager's FSM", the question that made both `setWCL_SCAN_REQ` and
    `setWCL_ASSOCIATE` look like mysteries. The driver's message number is **not** translated on
    the way in, so the table is read literally. The transition matrix answers the *second* half —
    which messages a state will actually act on, and in what order they must arrive — without
    which a correctly built message can still be silently dropped.
    Self-test on a known answer before trusting it elsewhere: `wclfsm.py WCLScanManager` must
    show `type=2 num=237 -> scanDoneEventHandler` and `IN_PROGRESS: SCAN_COMPLETE -> IDLE`.
    **That self-test is necessary and was not sufficient.** The transition matrix is located by
    walking backwards from the handler array, which is 16-byte aligned — so any manager whose
    `nst * nev * 2` is not a multiple of 16 has padding in between, and reading the table as if it
    ended at the handler array shifts every entry. `WCLScanManager` (4 × 10 × 2 = 80) is a multiple
    of 16 and passed the self-test throughout, while `WCLNetManager` (7 × 11 × 2 = 154) was rotated
    by three entries and printed a *plausible* table with every handler on the wrong event. The
    start is now rounded down to a 16-byte boundary. **Validate a decoded matrix against a live
    transition** from the WCL log (root **Runtime Debugging**) before acting on it — a matrix that
    passes every internal consistency check can still be uniformly wrong, and this one was.

  Shared-cache side. Apple80211, IO80211 and CoreWiFi exist **only** inside the dyld shared
  cache, so `otool` and `lldb` both refuse them at their nominal
  `/System/Library/PrivateFrameworks/...` paths — these read the on-disk cache instead:
  - `dsc.py` — address <-> file-offset mapping across the cache and its subcaches, plus
    `image_ranges()`/`owner_of()` from the cache `.map`. The userspace `vtdump.load()`.
  - `xref.py` — find RIP-relative references to an address in a VA range. Reference a format
    string by its **start**, not a `grep` hit inside it.
  - `dscdis.py` — disassemble a cache VA range, annotating C strings. `--sync=<va>` finds a
    real instruction boundary, without which capstone decodes nothing.

  The caches ship no `.symbols` subcache, so local symbols are unavailable. Locate a private
  function by anchoring on a log format string: `grep -abo` the literal across subcaches, walk
  back to the preceding NUL for the string start, `dsc.py f2v` it to a VA, `xref.py` the code
  that loads it, then `dscdis.py --sync` that address. This is how `_getIfListCopy` was read.

## Local Contracts

- `fw_gen.sh` exits early when `include/FwBinary.cpp` already exists. Delete that file to
  force a regeneration; it is gitignored and must never be committed.
- `load.sh`/`unload.sh` target `itlwm.kext` only. They do not apply to `AirportItlwm.kext`,
  which the user injects via OpenCore rather than loading live.
- Loading a kext is a user action. Do not run `load.sh` on the user's behalf without
  explicit permission.

## Work Guidance

- `abi/` tools read a kernel collection straight from the running system and are read-only.
  Keep them dependency-free (standard library only) so they run on any macOS host. The six
  disassembly tools — `kdis.py`, `disrange.py`, `findfield.py`, `slotcall.py`, `dscdis.py`,
  `wclfsm.py` — are the deliberate exception: they need capstone, so they run from a venv. Each
  says how in its docstring. `dsc.py` and `xref.py` are stdlib and stay that way.
- **The venv lives at `scripts/abi/.venv`, gitignored.** Not in `/tmp`: this work spans reboots by
  nature — a bring-up cycle *is* a reboot — and anything under `/tmp`, the session scratchpad
  included, is gone afterwards. It was silently wiped three times in one session, and an emptied
  venv fails as *empty tool output*, which reads like "no matches" rather than "no tool". Recreate
  with `python3 -m venv scripts/abi/.venv && scripts/abi/.venv/bin/pip install capstone`. If a
  disassembly command returns nothing at all, check the interpreter exists before believing the
  result.
- Reach for the disassembly before asking for a boot test. "Can this code block?", "who writes
  this field?" and "what does this panic offset dereference?" are all answerable statically,
  and a wrong guess costs the user a reboot on a machine that may not come back.
- Never `git stash` to get a "clean" baseline in this repo. The Tahoe port is large and
  uncommitted, so a stash silently reverts it along with the change under test. Compare
  against a copy instead.

## Verification

`fw_gen.sh` is exercised by any full build:

```bash
rm -f include/FwBinary.cpp
xcodebuild -jobs 8 -target itlwm -configuration Release build
```

The `abi/` tools are self-checking: run `cmp_vt.py` against a class whose ABI is stable
(`IOSkywalkInterface`) and expect a near-exact match.

## Child DOX Index

- No child AGENTS.md files. `abi/` is owned here.
