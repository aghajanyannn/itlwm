# AGENTS.md — include/Airport

## Purpose

Hand-written reconstructions of Apple's private `IO80211Family` / `IOSkywalkFamily` C++
classes. `AirportItlwm` inherits from these, so these files define the binary interface
between the driver and the kernel.

## Ownership

- Owns every reconstructed Apple class: `IO80211Controller*`, `IO80211*Interface*`,
  `IOSkywalk*`, `CC*`, and the `apple80211_*` request/struct definitions.
- Does not own driver logic. Implementations live in `AirportItlwm/`.
- `IOSkywalkDataPath.h` — the Skywalk data path: the four queue factories, `IOSkywalkPacketQueue`,
  and the packet types the queue callbacks work on. Declaration-only apart from
  `IOSkywalkMemorySegment`, which is the last class here still declaring virtuals and is inert —
  nothing calls one. `IOSkywalkPacket`, `IOSkywalk{Tx,Rx}CompletionQueue::enqueuePackets` and
  `IOSkywalkRxSubmissionQueue::requestDequeue` are reached by direct calls to exported symbols
  instead, so the linker checks them; see the rule above.
  **`IOSkywalkMemorySegment` and `IOSkywalkPacketQueue` are the classes in this repo verified by
  `scripts/abi/callcheck.sh` rather than `mapdrv.py`** — see Verification. The queue probe exists
  because the driver calls `enable()`/`disable()`, which are **inherited** `IOEventSource` slots
  42/43: it is the only check in the repo that MacKernelSDK's `IOEventSource` matches Apple's, and
  a shift there leaves the entire data path silently inert rather than panicking.
  **Getting the base class right matters here even though nothing is virtual.** This class was
  declared `: public IOService` for weeks; with the correct `IOEventSource` base, `addEventSource`
  and `enable()` are the obvious calls and their absence is conspicuous. Both were missing, and
  both cost a boot cycle each to find.

## Local Contracts

- These headers are ABI declarations. The order and count of `virtual` methods, and the
  size of the ivar blobs (`char _data[...]`), must match Apple's shipped binary for the
  targeted release **exactly**. One wrong slot panics the kernel at boot.
- Every header that participates in the ABI must `#error` when `__IO80211_TARGET` is
  undefined. Keep that guard.
- Express release differences with `#if __IO80211_TARGET >= __MAC_xx_y` around the
  affected declarations only. Never reorder existing declarations to make a new release
  fit — that silently breaks the older targets built from the same file.
- Add new `__MAC_*` fallbacks to `itlwm/PrivateSPI.pch` when targeting a release whose
  constant an older SDK would not define.
- A method the driver never overrides or calls still occupies a slot. Placeholder
  declarations are acceptable for those, but the slot must exist and be in the right place.
- **Declaration-only headers are a separate category and must say so.** When the driver only
  passes a pointer through — never allocates the class, never calls a virtual on it, never
  takes its size — declare nothing but the static factory and no virtual at all. That creates
  no vtable and no ABI obligation, so there is nothing to keep in sync with a future release.
  `CCDataStream.h`, `CCFaultReporter.h` and `IO80211FaultReporter.h` are the examples; each
  carries a comment saying it is declaration-only and why. Do not "complete" them by adding
  virtuals nobody calls — that converts a zero-risk header into an ABI liability.
- **A method that is virtual in Apple's binary can still be declared non-virtual here, and
  usually should be.** If the out-of-line definition is exported — check with `nm` on the
  collection — declaring it non-virtual makes the compiler emit a direct call to that symbol
  instead of an indexed load from the vtable. The two are equivalent whenever nothing overrides
  the method, and they differ only in how they fail:

  |          | wrong/shifted vtable slot                              | missing or renamed symbol          |
  | -------- | ------------------------------------------------------ | ---------------------------------- |
  | detected | at runtime, on the target machine                      | at link time, on the build machine |
  | symptom  | calls a different method silently; panic or corruption | build fails                        |

  For a driver injected by OpenCore, where the recovery path for a bad boot is editing an EFI
  partition, trading the first failure mode for the second is close to free. It also removes the
  method from the per-release porting surface: the linker re-checks it on every build, so no
  `callcheck.sh` probe and no slot table is needed.

  **Two things to verify before converting one**, both cheap and both in the ABI baseline:
  1. the symbol is exported — `nm <kc> | grep <mangled>`;
  2. *nothing overrides it*, or the direct call would bypass the override and silently read the
     wrong object. `grep -E "^[A-Za-z0-9_]+ <slot> __ZNK?<mangled-base>" abi-<ver>-<build>.txt`
     must list only the base class and subclasses that inherit it unchanged.

  `IOSkywalkDataPath.h` is the worked example: `IOSkywalkPacket` used to declare slots 35..42 as
  virtual and was one of two classes `callcheck.sh` existed to guard. It now declares none, and
  `IOSkywalkTxCompletionQueue::enqueuePackets` — genuinely virtual at slot 85 — is called the same
  way, because our own factory call returns exactly that class and nothing subclasses it.
- **A link error against a reconstructed signature is a question, not a verdict, and the answer is
  sometimes that the other spelling names a DIFFERENT function.** Overload sets are invisible in a
  reconstruction: the header declares one entry point, so a symbol that fails to resolve reads as
  "the declaration is wrong" and the natural fix is to change it back. Enumerate first:

  ```sh
  nm -a <kc> | grep -E "<ClassName>(8withPool|12initWithPool|<method>)" | c++filt | sort -u
  ```

  `IOSkywalkTxSubmissionQueue::withPool` has **three** overloads on 26.6. Two differ only in the
  `const` on the callback's `IOSkywalkPacket *const *` / `IOSkywalkPacket **`, and the non-const one
  is a shim that ORs a mode bit into `options` and tails into the const one — so that qualifier
  *selects between two incompatible meanings of a different argument*. This repo recorded the
  opposite conclusion for weeks ("keep Apple's declaration exactly and `const_cast` at the call
  site") and panicked the machine on the first transmitted frame because of it.
  **A `const` in a reconstructed signature can be load-bearing semantics.** When one exists, prefer
  binding the overload whose contract you implement over passing a magic constant: the mangled name
  then carries the choice, and the linker checks it on every build. That is the direct-call trade
  applied to a *contract* rather than to a slot index.
- **Instrumentation must sample before the teardown it is trying to explain, and the failure path
  usually *is* the teardown.** A snapshot taken where the driver *notices* a failure often runs
  after the cleanup that erased the evidence, so it reads a plausible zero and says nothing. The
  Tahoe join snapshot captured `ic_rsnprotos`/`akms`/`ciphers` at the point failure was reported,
  which is the branch that observes `ic_des_esslen == 0` — reachable only *after*
  `ieee80211_deselect_ess` has called `ieee80211_disable_rsn` and zeroed all four. The reading was
  guaranteed by construction and briefly "proved" that the driver advertised no RSN at all.
  Latch state at the last point it is known good — here, the first transition past `SCAN` — and
  before trusting any zero, check what the failure path runs on its way to your capture. This is
  the same lesson `ITLWM_PREINIT_SNAP` records; it has now cost two separate investigations.
- **A parameter can select behaviour, not just carry data — check before passing a literal.**
  A reconstructed signature is only the shape; it does not say what the family does with the
  values. `postMessage`'s trailing `bool` chooses between an async queue and a synchronous send
  that asserts the caller's thread and gate. The cheap check is two disassemblies: Apple's
  implementation, to see what the value selects, and the same call in a shipping Apple driver, to
  see what a real caller passes (`AppleBCMWLANCore::postMessageInfra` sets it to 1). Document the
  meaning at the declaration so the next caller cannot pass a bare literal by accident.
- **A placeholder for an unnamed slot must return a plausible status, not `void`.** An empty
  `virtual void _RESERVEDx() {}` body is *not* neutral: it leaves the return register undefined, so
  any caller that error-checks the slot reads whatever was last in `%eax`. Slot 468 on
  `IO80211InfraInterface` is unnamed and pure even in Apple's own `AppleBCMWLANInfraProtocol`, which
  made a `void` placeholder look obviously safe — yet `IO80211MacAddressAgent::setMacAddress` calls it
  and treats non-zero as fatal, so a leftover *pointer* was read as a status and blocked every
  association. The symptom is diagnostic: a large value that is stable across attempts within a boot
  and whose high byte tracks the KASLR slide between boots. Placeholders now return
  `kIOReturnSuccess`. Occupying the slot is necessary but not sufficient — the *register* has to be
  defined too.
- **Convert vtable byte offsets with a calculator, then convert back to check.** `call [rax + 0xea0]`
  is slot **468** (0xea0 / 8), not 452; reading it as 452 cost three boots, two pointless slot pins
  and two retracted conclusions in one session. Cross-check against a known-good conversion in the
  same session if one is available.
  **The last hex digit gives the answer's parity for free, so use it as the independent check.**
  Even slots sit at byte offsets ending in `0`, odd slots at offsets ending in `8` — nothing else
  is possible, because the stride is 8. `0xea0` → even (468 ✓), `0xcf8` → odd (415, not the 414 a
  slip produces), `0xd08` → odd (417). A quotient whose parity disagrees with the last digit is
  wrong before you check anything else, which is precisely the error that cost those three boots.
  An odd slot number is therefore *normal*, not a symptom — half of them are.
- **A struct crossing a slot needs its flag word decoded, not just its layout.** Getting every
  offset, width and signedness right still loses data if the family gates a field on a validity
  bit: `BeaconMetaData`'s RSSI at `0x30` was correct in every respect and discarded because flags
  bit 14 was clear, and *nothing failed* — scan results appeared, correctly named, all reporting
  0 dBm. Assume every bit gates something until the disassembly says otherwise, and recover the
  meanings from **both** sides: the consumer shows which bit guards which read, and Apple's
  producer shows what it derives each bit from, which is usually what names it (here, Broadcom's
  `WL_BSS_FLAGS_RSSI_INVALID`). Plain-data payload contracts — `BeaconMetaData.h`,
  `AssocCandidates.h`, `JoinCompleteEvents.h`, `LqmEventData.h` — declare no class and so carry no vtable obligation,
  but they are ABI all the same: pin them with `_Static_assert` on `sizeof` and on every offset the
  disassembly established. **Which direction a payload travels changes the stakes.** Apple writes
  `AssocCandidates.h` and we read it, so an unread field is merely inert; we *write*
  `JoinCompleteEvents.h`, so a wrong field is one the family acts on. Note also that a struct whose
  64-bit members sit at offsets that are not 8-aligned, or whose total is not a multiple of 8, must
  be `__attribute__((packed))` — both join-completion events are — otherwise the compiler pads,
  every later offset shifts, and a consumer that checks the length rejects the message.
  `JoinCompleteEvents.h` now holds three: 211 and 213 for `JOIN_MANAGER`, and **216
  (`apple80211_link_status_ind`, `0x10`) for `WCLNetManager`**, whose whole switch is byte 6. When a
  payload struct is recovered from a consumer, recover it from *both* of Apple's producers too if
  there is more than one — here `AppleBCMWLANNetAdapter::sendInternalLinkDownInd` zeroes the whole
  struct and so pins every field that `::handleLink` leaves to a conditional branch.
- **A validity-gated payload is the one case where a partial reconstruction is legitimate.**
  `LqmEventData.h` (`apple80211_lqm_event_data`, `0x1dc`, driver message 39) is validity-gated
  end to end: `WCLNetManager::handleLqmUpdate` tests a flag byte before reading each group, so a
  zeroed buffer carrying only the flags this driver can honestly back is *skipped* group by group
  rather than read as zeros. That is the `BeaconMetaData` bit-14 shape pointing the other way, and
  it holds only because the flags exist — do not generalise it to a struct without them, where an
  unfilled field is read as a real value. The same header shows why size still comes first: the
  consumer opens with `cmp qword ptr [rsi+8], 0x1dc; jne out`, so a struct short by one byte is
  discarded in silence and looks exactly like a message that was never posted.
- **Size a struct from its allocation, not from the last field a producer writes, and check for a
  second producer.** `apple80211_assoc_candidates` was reconstructed at `0x3dc` — the last offset
  `WCLJoinRequest::fillAssocCandidatesList` touches — and is really `0x6f8`, because
  `WCLJoinManager::getVendorSpeificIes` runs afterwards and writes another 260 bytes higher up. The
  error is invisible by construction: the buffer is `IOMallocZeroData`'d, so every untouched field
  reads as a plausible zero. Get the size from the allocation and the calls that carry it — here
  `IOMallocZeroData(0x6f8)`, the `cmdIouc` length argument and `IOFreeData(p, 0x6f8)` agree three
  ways — and grep for every function taking the struct as a parameter before concluding a field is
  unused. Where a producer's own arithmetic bounds an array, derive the bound from it rather
  than choosing one (`AssocCandidates.h` gets its `[10]` that way) and say so at the declaration.
  Name a field you cannot explain `_unkNN` and size it: for a struct Apple writes and we read, an
  unknown field is inert, while a wrongly named one invites a wrong use.
  **But "inert" describes the struct, not the driver.** An unread field is an unread *instruction*:
  `apple80211_assoc_candidates._unk40[0x94]` is a whole `apple80211_key`, and not reading it meant
  every Tahoe association was attempted with no PSK at all, which `ieee80211_match_bss` then
  refused on every scan. It had been sitting behind a `memmove` whose length matched a struct this
  repo already defines. **When a producer copies a fixed-size blob into a field you cannot name,
  match that length against every struct the same code path already handles before settling for
  `_unkNN`** — here `sizeof(apple80211_key) == 0x94`, and `WCLJoinRequest::getKeyCipherType`
  reading offset 8 of the source confirmed it independently.
- **Disassemble the override that is actually bound, not the class that declares the method.**
  Reading a base-class implementation and assuming it runs is how a verified-looking conclusion
  turns out wrong: `postMessage`'s flag really does select a queued route in
  `IO80211SkywalkInterface::postMessageInternal`, but the bound implementation is
  `IO80211InfraInterface::postMessage`, which ignores the flag for some message types. Get the
  binding from `tahoe-26.6-slots.txt` first — it names the owning class per slot — then
  disassemble that symbol. Panic backtraces name the bound one; believe them over the hierarchy.
- **Some ioctls never reach the driver's override at all — check before implementing one.** An
  `apple80211<name>` dispatcher may call the family's own base implementation **non-virtually**, so
  the driver's override of that slot is unreachable dead code no matter how correct it is.
  `apple80211setSET_MAC_ADDRESS` is the worked example: it calls slot 409 (`isCommandProhibited`),
  then `safeMetaCast`s to `IO80211SkywalkInterface` and `jmp`s **directly** to
  `IO80211SkywalkInterface::setSET_MAC_ADDRESS`, which then drives `IO80211MacAddressAgent` itself.
  `AirportItlwmSkywalkInterface::setSET_MAC_ADDRESS` can therefore never run, and "implement the
  stub" would be wasted work pointed at the wrong layer entirely. The tell in a log is a return
  value the driver could not have produced — ours return `kIOReturnUnsupported`, so anything else
  means the driver was not in the path. Disassemble the `apple80211<name>` dispatcher before
  implementing any `set*`/`get*` you are blaming for a failure.
- **A WCL FSM waits on a message number, and that number is data, not something to deduce.** Every
  `WCL*Manager` declares its subscriptions in a static table; `scripts/abi/wclfsm.py <class>` prints
  them with the FSM state and event names. Read it *before* implementing any `setWCL_*`, because the
  setter is only half the contract — the manager stays in its in-progress state until the driver posts
  the completion message, and every later request then fails with `EBUSY` while looking like a bug in
  the setter. This is what made `setWCL_SCAN_REQ` (message 237) and `setWCL_ASSOCIATE` (211 and 213)
  each look like a mystery. The numbers these establish live in `apple80211_var.h` as
  `APPLE80211_M_WCL_*`, deliberately outside the `APPLE80211_M_MAX` bitmap, which is Apple's
  pre-Tahoe subscription map and must keep its original size. Note also that each handler validates
  the payload **length** exactly and raises no event when it is wrong — so a length mistake presents
  as a message that was never posted.
- **Posting the right message is still not enough: the state has to accept it.** `wclfsm.py` also
  prints the **state × event transition matrix**, and it decides three things no subscription table
  shows — whether a state acts on an event at all, what ordering the driver's messages must follow,
  and which failure report actually advances the FSM. All three mattered for the join: `JOIN_MANAGER`
  accepts `JOIN_CONNECT_COMPLETE` in `IN_PROGRESS` as well as in `ASSOC_DONE` (so a join that fails
  before associating can still be reported), but `CONNECT_COMPLETE` does **not** accept
  `JOIN_ASSOC_COMPLETE` (so 211 must precede 213 and never follow it), and a failure has to be
  reported as a non-zero *connect* status because `handleJoinAssocComplete` raises no event for a
  non-zero assoc status other than the literal 1000. An event a state does not accept is dropped in
  silence, which is indistinguishable from never posting it.
  The matrix is not in `__const` where the names and subscriptions are: the config the FSM reads
  (`cfg[0] = table`, `cfg[8] = handlers`, `cfg[0x29] = event count`) is a `__bss` global named
  `gfsm<MGR>Configuration`, filled by `__GLOBAL__sub_I_<Mgr>.cpp`, so **it reads as zeros in the
  kernel collection**. The four arrays it points at do sit contiguously in `__const`, ending at the
  state-name array, which is what `wclfsm.py` walks back from.
- **A non-virtual member added to a reconstructed class is vtable-neutral** and is the cheap way
  to reach an exported Apple accessor: it consumes no slot, so `mapdrv.py`'s slot counts do not
  move. `IO80211SkywalkInterface::getWorkQueue()` is declared that way. Confirm the method really
  is non-virtual before doing this — absence from `tahoe-26.6-slots.txt` is the check — because
  declaring a virtual as non-virtual silently drops a slot and shifts everything after it.

## Work Guidance

### Recovering a release's true layout

Ground truth is the shipping kernel for that release, not documentation. On a machine
running the target release, the classes live in the kernel collection:

```bash
python3 scripts/abi/vtdump.py \
    /System/Library/KernelCollections/BootKernelExtensions.kc \
    IO80211Controller IO80211InfraProtocol > /tmp/os_vt.txt
```

Abstract classes show `___cxa_pure_virtual` for slots whose names you need. Dump a
*concrete* Apple subclass instead — it names almost every slot:

- `AppleBCMWLANInfraProtocol` resolves the `IO80211InfraProtocol` chain.
- `AppleBCMWLANCore` resolves the `IO80211Controller` chain.

Then dump what the current headers produce, using the real build's compiler arguments so
the two are comparable:

```bash
clang -x c++ "@<target>-common-args.resp" \
    -Xclang -fdump-vtable-layouts -S -emit-llvm -o /dev/null \
    -include itlwm/PrivateSPI.pch AirportItlwm/AirportItlwmV2.cpp > /tmp/hdr_vt.txt
```

`-fdump-vtable-layouts` only prints vtables that are actually emitted, so dump from a real
`.cpp` that defines the class — a header-only probe prints nothing.

Compare, and map the driver's own overrides onto the release's real slots:

```bash
python3 scripts/abi/cmp_vt.py /tmp/hdr_vt.txt /tmp/os_vt.txt IO80211Controller
python3 scripts/abi/mapdrv.py /tmp/hdr_vt.txt /tmp/os_bcmcore.txt AirportItlwm IO80211Controller
```

`mapdrv.py` is the acceptance check: every driver override must land on a slot whose name
matches. Anything else is a panic waiting to happen.

Sanity-check the method itself against a class whose ABI did not change (e.g.
`IOSkywalkInterface`); it should come back as a near-exact match. If a supposedly stable
class shows large drift, the tooling is misaligned, not Apple.

### Notes on the tooling

- Kernel collections use chained fixups; `vtdump.py` decodes the
  `DYLD_CHAINED_PTR_64_KERNEL_CACHE` form (low 30 bits are a base-relative target).
- Instance sizes are **not** recoverable from the kernel collection: `OSMetaClass::classSize`
  is written by the runtime constructor, so the on-disk field reads 0. Recover sizes from
  the metaclass constructor's immediate operand, or from a live kernel.

## Verification

Building proves nothing here — the compiler cannot see Apple's real layout. A change to
these headers is verified when `mapdrv.py` reports zero overrides on a wrong slot for both
`AirportItlwm`/`IO80211Controller` and `AirportItlwmSkywalkInterface`/`IO80211InfraProtocol`
against the targeted release, and every other `AirportItlwm-*` target still builds.

Full check against a running Tahoe machine:

```bash
S=/tmp/abi; mkdir -p $S
python3 scripts/abi/vtdump.py /System/Library/KernelCollections/BootKernelExtensions.kc \
    AppleBCMWLANInfraProtocol > $S/os_infra.txt
python3 scripts/abi/vtdump.py /System/Library/KernelCollections/BootKernelExtensions.kc \
    AppleBCMWLANCore > $S/os_core.txt

xcodebuild -jobs 8 -target "AirportItlwm-Tahoe" -configuration Release build
RESP=$(ls build/itlwm.build/Release/AirportItlwm-Tahoe.build/Objects-normal/x86_64/*common-args.resp | head -1)
for f in AirportItlwm/AirportItlwmSkywalkInterface.cpp AirportItlwm/AirportItlwmV2.cpp; do
  clang -x c++ -w -isysroot "$(xcrun --sdk macosx --show-sdk-path)" \
      -target x86_64-apple-macos10.15 "@$RESP" \
      -Xclang -fdump-vtable-layouts -S -emit-llvm -o /dev/null \
      -include itlwm/PrivateSPI.pch "$f"
done > $S/hdr.txt 2>/dev/null

python3 scripts/abi/mapdrv.py $S/hdr.txt $S/os_infra.txt \
    AirportItlwmSkywalkInterface IO80211InfraProtocol
python3 scripts/abi/mapdrv.py $S/hdr.txt $S/os_core.txt \
    AirportItlwm IO80211Controller
```

Both must end in `WRONG Tahoe slot: 0`. `cmp_vt.py` reports a spurious `delta=-1` on these
classes because it counts clang's trailing `vcall_offset` pseudo-entry; `ownslots.py`
filters it and is the one to trust for slot counts.

`mapdrv.py` only checks the slots we *override*. To verify a class end to end, compare every
slot against Apple's table by name — that is what caught nothing wrong in `AirportItlwm`
except the two deliberate `_RESERVED` stubs, and it is worth running before blaming the
headers for anything.

**And it cannot see a class we only CALL into.** `IOSkywalkMemorySegment` is never subclassed
here, so it has no overrides for `mapdrv.py` to check — yet a wrong index on a virtual we invoke
dispatches to a different method of the same class with no build-time, load-time or `mapdrv`
symptom. `scripts/abi/callcheck.sh` is the check for that case; it compiles `callprobe.cpp` and
compares the emitted tables to the release baseline by name. It must end `WRONG: 0`.

**The better fix, where it is available, is to stop calling through the vtable at all** — see the
direct-call rule near the top of this file. `IOSkywalkPacket` used to be checked here and no longer
needs checking, because every method the driver calls on it is an exported symbol. A class only
belongs in `callprobe.cpp` when a virtual call is genuinely unavoidable — which is the case for
`IOSkywalkPacketQueue`'s `enable`/`disable`: they are declared `virtual` by `IOEventSource`, so a
derived redeclaration stays virtual no matter how it is written, and a qualified call would need an
override this repo has no definition for.

```bash
scripts/abi/callcheck.sh          # after building AirportItlwm-Tahoe
```

**Self-test it the way its own docstring demands**, because a checker that silently passes is
worse than none: insert one extra virtual into `IOSkywalkDataPath.h` and confirm it reports the
shift, then remove it. Verified doing exactly that on both probed classes — 7 wrong from a single
insertion into `IOSkywalkMemorySegment`, 0 after restoring; and 1 wrong at slot 55 from an
insertion into `IOSkywalkPacketQueue`, which is the *correct* answer for a declaration-only class
whose table ends at the insertion point. **Check the reported slot number is where you inserted
it** — for a class with later slots expect a cascade, for one without expect exactly one hit.
Current baseline: 2 classes, 94 slots, `WRONG: 0`.

**And none of it validates the loaded image — see below.**

### The vtable in the file is not the vtable in memory

`mapdrv.py`, `ownslots.py` and every slot-by-slot comparison read the *built kext*. In that
file only our own overrides carry an address; **every slot inherited from an Apple class is
zero**, and the kext loader patches them in from the parent vtable at load time. Check with:

```bash
# non-zero slots in a class's vtable in the built kext == the overrides we define
python3 - <<'PY'
import struct,sys; sys.path.insert(0,'scripts/abi')
from vtdump import MachO
d=open('build/Release/Tahoe/AirportItlwm.kext/Contents/MacOS/AirportItlwm','rb').read()
m=MachO(d); by={}
for v,n in m.symbols: by.setdefault(n,v)
vt=by['__ZTV29AirportItlwmEthernetInterface']; fo=m.v2f(vt)
for s in range(345):
    raw=struct.unpack_from('<Q',d,fo+0x10+s*8)[0]
    if raw: print(s, hex(raw))
PY
```

That patching **can bind the wrong function.** Measured on Tahoe:
`AirportItlwmEthernetInterface` slot 241 should be `IOService::errnoFromReturn`; at runtime
it held `IO80211SkywalkInterface::errnoFromReturn`, because our kext also contains
`IO80211SkywalkInterface`, which overrides the same method name. The wrong implementation
treats `this` as an `IO80211SkywalkInterface` and dispatches *its* slot 373 — 28 past the end
of our 345-entry table — into adjacent `__DATA`, landing on
`IO80211InfraProtocol::gMetaClass`. That was the NX panic on `SIOCSIFFLAGS`.

This is a **loader defect, not a header error**. Eight rounds of layout verification kept
coming back clean because the table was right on disk and wrong in memory.

**Fix:** define the method and forward to `super`, so the slot is non-zero at link time and
there is no hole to patch. Same remedy as `_RESERVEDIONetworkController6/7`.

**Read a suspect slot from the live object** rather than guessing — that is what closed this:
from one of our own overrides on the same object, `((void *const *)*(void *const *)this)[N]`,
delivered in a `panic()` string.

Enumerate what else is exposed — any slot we inherit whose method name is *also* overridden
by an `IO80211*` / `IOSkywalk*` class in this kext:

```bash
# for each slot of Apple's IOEthernetInterface vtable, split the mangled name into
# (const-ness, class, method-signature) and look for the same signature on a class our kext
# also carries; those slots are the ones the loader can mis-bind
```

20 slots of `AirportItlwmEthernetInterface` qualify. Only 241 is proven to fire and only
241/240 are pinned; the rest — `free`, `init`, `getWorkLoop`, `newUserClient`,
`willTerminate`, `handleOpen`/`handleClose`/`handleIsOpen`, `setAggressiveness`,
`setMaxTransferUnit`, `configureReport`/`updateReport`, `maxCapabilityForDomainState`,
`initialPowerStateForDomainState` — are latent. A panic inside any of them is this bug
again, not a new one.

**`-include itlwm/PrivateSPI.pch` is mandatory in that recipe.** The `common-args.resp`
does not carry the prefix header — Xcode passes it separately — so a hand-run clang that
omits it leaves `__MAC_26_0` undefined. `__IO80211_TARGET` then evaluates to 0, which makes
every `>= __MAC_26_0` test accidentally true (`0 >= 0`) while every `>= __MAC_<older>` test
turns false, silently dropping declarations such as `setInfraSpecificFrameStats`. The dump
comes out one slot short and mapdrv reports a bogus mass shift. Also confirm the header and
Tahoe slot *counts* match in the mapdrv banner — a count mismatch means the dump itself is
wrong, not the headers.

## Known ABI Gaps

### macOS 26 (Tahoe) — ABI port validated, HAL bring-up in progress

**The ABI reconstruction is confirmed working against the shipping kernel.**
`super::start()` returns **true**: Apple's entire `IO80211Controller::start()` — post
office, IOReporters, control-path logging, fault reporter, `IO80211RangingManager`,
command gate, timer source, timer factory, `IO80211RNGAgent`, frame pool — runs to
completion through these headers. Observed live: `IO80211Family (1200.13.1)` and
`AirportItlwm (2.4.0)` both loaded and linked, `IO80211RangingManager` and
`IO80211RNGAgent` instantiated, `AirportItlwmStage = 9`, provider `busy 0`.

**`fHalService->attach(pciNub)` now succeeds** on an Intel AX200 (`pci8086,2723`, rev 0x1a,
`iwx` HAL). The blocker was the ICT, not ABI — see `itlwm/AGENTS.md`.

Bring-up has since walked forward through, in order: the `mExpansionData` offset bug, the
`RegistrationInfo` size/allocator pair, the two `fRegistrationInfo` buffers
`prepareBSDInterface` requires, and finally the loader mis-binding `AirportItlwmEthernetInterface`
vtable slot 241, which killed the `SIOCSIFFLAGS` that `attachNetworkInterfaceToBSD` issues.
Each is written up below or in its owning doc. The history is kept because it explains the
marker encodings and because every one of these is a trap a future release can re-set.

Not one of them was a header error. The reconstruction has been correct throughout; the
failures were an allocator zone, a null field Apple never checks, and a loader bug.

The attach failure was localised to **`iwx_preinit`**, the last call in `iwx_attach`. (That
console evidence predates the deferred publish; with the defer in place `start()` ran after the GUI
takes the console and nothing driver-side reaches a screen — see the root AGENTS.md.) With `debug=0x8`
the console shows `ieee80211_ifattach`, a successful `ifnet_attach`, and the taskq
creation that follow the ring setup — so PCI mapping, interrupt establishment, the
device-config match and ring allocation all succeeded. The provider's busy time is
**10,031 ms**, i.e. `attach()` stalls for ten seconds before failing: a timeout, not a
rejection.

Measured with `ITLWM_PREINIT_MARK` and read from ioreg on a boot that survived:

```text
ItlwmPreinitMark = 6      iwx_run_init_mvm_ucode failed (the silent exit)
ItlwmPreinitErr  = 35     EAGAIN / EWOULDBLOCK
```

**Those two values together prove the firmware DID come alive.** This is a proof, not a
hypothesis, and it inverts the obvious reading:

- 35 appears nowhere in the HAL — `grep -E 'EAGAIN|EWOULDBLOCK' itlwm/hal_iwx/ItlIwx.cpp`
  is empty. It can only be `msleep()` timing out inside `ItlHalService::tsleep_nsec`
  (`include/HAL/ItlHalService.cpp:62-72`).
- Every `tsleep` reachable between mark 5 and mark 6 requires `sc->sc_uc.uc_ok == 1`.
  `iwx_load_firmware` returns **EINVAL (22)** if the firmware never came alive
  (`ItlIwx.cpp:4715-4716`), and the later waits are only reached once it returned 0.
- `uc_ok` is set in exactly one place — the `IWX_ALIVE` case of `iwx_rx_pkt`
  (`ItlIwx.cpp:10935`, `:10951`), reached only through `iwx_intr` → `iwx_notif_intr` →
  `iwx_rx_pkt`, i.e. the `IOFilterInterruptEventSource` action on the work-loop thread.

So **MSI delivery, the DMA firmware image, the context-info load, the RX ring and the
`IO80211WorkQueue` thread all work.** Do not re-investigate "the interrupt never arrived";
had that been true the errno would be 22. The failure is a *later* wait that expired.

Five `tsleep` sites are reachable between the two marks. Any one could be the timeout:

| # | site | waits for |
| --- | --- | --- |
| 1 | `ItlIwx.cpp:4693` | `sc_uc`, 1 s, in `iwx_load_firmware` |
| 2 | `ItlIwx.cpp:2125` | `sc_init_complete`, 2 s, in `iwx_load_pnvm` |
| 3 | `ItlIwx.cpp:6426` | command completion, 1 s, in `iwx_send_cmd` |
| 4 | `ItlIwx.cpp:4875` | `sc_init_complete`, 2 s, in `iwx_run_init_mvm_ucode` |
| 5 | `ItlIwx.cpp:6426` | again, via `iwx_nvm_get` (`:4584`) |

Leading explanation is a **lost wakeup**: `ItlHalService::wakeupOn`
(`include/HAL/ItlHalService.cpp:55-58`) calls `wakeup(ident)` *without* holding the
`inner_lock` that `tsleep_nsec` sleeps on (`:69-70`), and the OpenBSD predicate loops that
made this safe were replaced by single unconditional sleeps — the originals are still
there, commented out, at `ItlIwx.cpp:4690-4693` and `:4869-4875`. A wakeup landing before
the sleeper reaches `msleep()` is dropped and the caller runs the full timeout.

Strong runner-up, deterministic rather than racy: **`iwx_load_pnvm` (site 2)**. It guards
only on `sku_id` (`ItlIwx.cpp:2110`), not device family. `sku_id` is filled only in the
ALIVE-v5 branch, which is the `else` of a bare payload-size comparison (`:10924`). An
AX200 is `IWX_DEVICE_FAMILY_22000` (`:11853-11854`); if its ALIVE matches neither v4 nor
v5, `sku_id` is populated from garbage, PNVM is rung, and the driver waits 2 s for a
`PNVM_INIT_COMPLETE_NTFY` a 22000-family part never sends.

**The fault is environmental, not in the HAL.** Confirmed by the user: `itlwm.kext` runs
this same AX200 on this same Tahoe install and boots every time. The iwx HAL is
byte-for-byte upstream apart from the markers (`git diff itlwm/hal_iwx/ItlIwx.cpp` is
instrumentation only), so `iwx_preinit` — including all five waits — *succeeds* under
`itlwm.kext`. Whatever breaks it is in the AirportItlwm layer around it. The material
differences to weigh:

- `_fWorkloop` is Apple's `IO80211WorkQueue` (created in `IOPCIEDeviceWrapper::start()`),
  not `IOWorkLoop::workLoop()` as in `itlwm/itlwm.cpp`. The HAL's interrupt event source
  and the command gate both live on it.
- `AirportItlwm::start()` runs *after* Apple's entire `IO80211Controller::start()`, so
  `iwx_preinit` executes in a different thread/lock context and much later in the sequence.
- Only AirportItlwm interposes `IOPCIEDeviceWrapper` between the PCI device and the driver.

**Per-wait-site markers are in the tree** (`ITLWM_PREINIT_WAIT` / `ITLWM_PREINIT_CMD`,
defined at the top of `ItlIwx.cpp` so they are visible at `tsleep` sites thousands of lines
above `iwx_preinit`). `ItlwmPreinitFail` is the value that matters — sticky on the *first*
failing wait, so cleanup paths cannot overwrite it — encoded `site*100 + 10 + predicate`:

| value | meaning |
| --- | --- |
| 111 | site 1, `iwx_load_firmware` — ALIVE arrived, **wakeup lost** |
| 110 | site 1 — firmware never came alive (would also give errno 22, so unlikely) |
| 210 | site 2, `iwx_load_pnvm` — PNVM notification never arrived |
| 310 | site 3, `iwx_send_cmd` — a command timed out; `ItlwmPreinitCmd` holds its opcode |
| 411 | site 4, `iwx_run_init_mvm_ucode` — `IWX_INIT_COMPLETE` already set, **wakeup lost** |
| 410 | site 4 — `IWX_INIT_COMPLETE_NOTIF` really never came |

Trailing **1 = lost wakeup** (condition already true when the sleep returned); trailing
**0 = the notification never arrived**. Leading digit picks the site.

**Do not instrument the HAL's wait sites. It has been tried three times and hangs the boot
every time.** The final test was decisive because it used **one unchanged binary**, with the
stores gated behind `-itlwaitmark`: booted without the flag, hung with it, booted again
without it. So the markers are harmless to *ship* and fatal to *execute* — this is not a
link or layout effect, and `__DATA,__common` is not the explanation (`itlwm.kext` has 17
such symbols and boots every time).

No mechanism was ever found. They are plain `int` stores on a path that cannot run before
STAGE 9, yet enabling them moved the hang to two different points deep inside Darwin
Ignition. The honest reading is that something in this driver is corrupt or racy enough
that a handful of extra stores changes the outcome — which is itself a finding worth
chasing, but not by adding more stores.

**Get the same answer by reading the softc afterwards instead.** `publishPreinitMark()` in
`AirportItlwmV2.cpp` runs once, on our own `start()` thread, after `attach()` has already
returned, and writes no HAL state at all:

| property | source | meaning |
| --- | --- | --- |
| `ItlwmUcOk` | `sc_uc.uc_ok` | the `IWX_ALIVE` notification was processed |
| `ItlwmUcIntr` | `sc_uc.uc_intr` | the firmware-load interrupt was seen |
| `ItlwmInitComplete` | `sc_init_complete` | bit 0 = `IWX_INIT_COMPLETE_NOTIF` was processed |
| `ItlwmGeneration` | `sc_generation` | device generation, to spot a reset mid-sequence |

Measured: `UcOk = 1`, `UcIntr = 1`, `InitComplete = 0`, `Generation = 0`. So the firmware
came alive and its interrupt was processed, **and the notification genuinely never
arrived** — this is *not* a lost wakeup, which retires that hypothesis.

**PNVM is exonerated.** Measured `ItlwmSkuId0/1/2 = 0`, `ItlwmDeviceFamily = 1`, so
`iwx_load_pnvm` takes its "if the SKU_ID is empty, there's nothing to do" early return and
never kicks the doorbell. `iwx_load_ucode_wait_alive` therefore completed in full
(`read_firmware` → `start_fw`, which is where ALIVE set `uc_ok` → `iwx_load_pnvm` →
`iwx_post_alive`).

Three hypotheses are now dead. Do not revisit any of them:

1. **Lost wakeup.** Refuted by `InitComplete = 0` — the notification never arrived, so
   there was nothing to lose.
2. **PNVM kicked on hardware that never answers.** Refuted by `sku_id = 0`. (The theory was
   that `sku_id` is populated only in the ALIVE-*v5* branch, the `else` of a bare
   `payload_len == sizeof(*resp4)` test, so a payload matching neither version would fill it
   with garbage and start a 2 s wait an AX200 never satisfies. Plausible, but it does not
   happen here.)
3. **Apple's no-op `enableAllInterrupts`/`disableAllInterrupts` breaking interrupt re-arm.**
   Refuted by disassembly: `IOWorkLoop::runEventSources` dispatches only `closeGate`
   (slot 48) and each event source's `checkForWork`; it never calls slots 44/45.

**What remains** is the tail of `iwx_run_init_mvm_ucode`: two `iwx_send_cmd_pdu` calls
(`INIT_EXTENDED_CFG_CMD`, then `NVM_ACCESS_COMPLETE`, 1 s each) followed by the 2 s
`INIT_COMPLETE` wait. `ItlwmCmdQueued` separates them, because `iwx_send_cmd` does
`ring->queued++` before sleeping and `iwx_cmd_done` does `ring->queued--` on completion:

- `ItlwmCmdQueued > 0` → a command never completed; the RX/notification path stopped working
  after ALIVE.
- `ItlwmCmdQueued = 0` → both commands completed and only `IWX_INIT_COMPLETE_NOTIF` is
  missing, which is a firmware-behaviour question rather than a plumbing one.

Note `uc_ok = 1` already proves the RX path processed at least one notification, so a
non-zero `CmdQueued` would mean interrupts worked once and then stopped — a very different
bug from "never worked".

**Tahoe `IO80211WorkQueue` stubs that will bite something eventually.** All confirmed by
disassembly, none currently implicated:

- `enableAllInterrupts()` and `disableAllInterrupts()` (slots 44, 45) are no-ops
  (`push rbp; pop rbp; ret`), overriding `IOWorkLoop`'s versions which normally walk the
  event sources and enable each interrupt. The `iwx` HAL does not depend on them — it
  calls `sc->sc_ih->enable()` directly on its `IOFilterInterruptEventSource` — but any
  code expecting them to act gets nothing.
- `getThread()` (slot 37) is `xor eax, eax; ret` — it always returns NULL. Nothing in this
  repo calls it (only the declaration in `IO80211WorkQueue.h`), but Skywalk code above us
  might.

**Latent hazard in `IOPCIEDeviceWrapper::probe()`** (`AirportItlwm/IOPCIEDeviceWrapper.cpp:85-90`):
it clears MSI-X enable and sets the MSI Enable bit *directly in config space*, behind
IOPCIFamily's back, during early-boot matching; and `msiCap`/`msixCap` are read into
uninitialised `UInt8`s when `findPCICapability` fails. On a warm boot the device may still
hold the previous OS's MSI address/data, so this can briefly arm an interrupt pointing at
an address this kernel never programmed. Verified as *not* the cause of the attach failure
(IOPCIFamily reprograms the vector before it goes live), but worth fixing on its own.

### Boot stability: judge configurations by their record, not by single boots

**The boot-args below no longer exist.** `-itlnocc`, `-itlmincc`, `-itlccowner`, `itlccsize`,
`-itlnostart` and `-itlnohal` are deleted; the bisection they served is closed and `-itlmincc` is on
record as a mode that invalidated every result taken under it. The record is kept because it is what
stops the refuted theories being re-followed, not because the flags can be re-run. Apple's own
`ccpipe:<PipeName>` boot-arg (below) is the way to vary pipe size now, and it is better than
`itlccsize` was because it needs no rebuild. **`itldefer`, `-itlnodefer` and `-itlwaitpub` are gone
too** — the deferral they controlled was measured, then deleted after `-itlnodefer` booted 5/5, and
`IOPCIEDeviceWrapper` is back to upstream. See root AGENTS.md mechanism 7, which is CLOSED.


The hangs land at **four different boot phases** — USB enumeration, root mount, Ignition's
config dump, and Ignition's cryptex graft. A specific defect hangs in a specific place; a
hang that wanders like this is memory corruption or a broad race, where each build lands
somewhere random on a distribution. **A single boot is therefore roughly a coin flip and
proves very little.** Several conclusions in this file's history were drawn from n=1 and had
to be withdrawn; the wait-site markers in particular were reverted on evidence that no
longer looks causal.

Records so far, which is the only trustworthy view:

| configuration | pipes actually created | boots | hangs | **what has since been found to confound this row** |
| --- | --- | --- | --- | --- |
| `-itlnocc` (no CoreCapture at all) | 0 | **5** | 0 | proves only that *not running Apple's `start()`* is safe — a NULL logger fails at `createIOReporters` before anything interesting |
| `-itlmincc itlccsize=64` | **0 — rejected** | 1 | 0 | the pipe was refused by the `min <= size` gate; nothing was tested |
| `-itlmincc` (one pipe + one stream) | 1 | 0 | 2 | **panicked unconditionally** — no fault reporter, `findAndAttachToFaultReporter+0x10f` |
| `-itlccowner` | 1 | 0 | 2 | same era: slot 432 returned a bare `CCStream`, so the family dispatched slot 36 on the wrong class |
| deferral, full CoreCapture | 3 | 2 | 3 | same fault-reporter era, **and** carried the slot instrumentation this file records as hanging roughly half of all boots on its own |

**READ THE LAST COLUMN BEFORE USING THIS TABLE. Every row is confounded by a fault that was
found later and has since been fixed, so there is currently NO surviving evidence that creating
a CCPipe early hangs anything.** The conclusion this table used to carry — "creating CoreCapture
pipes at all is the destabiliser, and deferral only lowers the odds" — does not follow from data
gathered under an unconditional panic plus an instrumentation bug.

What that does **not** license is deleting the deferral on the spot: nobody has yet booted the
*current* code — correct fault-reporter chain, slot instrumentation gone — without it. It moves
`itldefer` from "required, mechanism unknown" to a fossil. It was disproved and deleted:
`-itlnodefer` booted 5/5 on 26.6.2 (25G83), with all three `CCPipe`s created at the earliest point
in boot. Root AGENTS.md mechanism 7 has the full closure.
**Rule: a record table is only evidence if each row was gathered under one variable.** These were
not, and the table read as independent evidence for months because the confounds were discovered
one at a time and never propagated back into it.

**Leading suspect: the pipe size.** `-itlmincc` creates exactly ONE pipe, with the upstream
`pipe_size = 0x200000`, and still hangs — so a single 2 MB allocation this early is enough.
`itlccsize=<KB>` makes that tunable (default 2048, i.e. unchanged). Still untested: every
pipe ever created on this machine has been 2 MB.

#### A clean boot is not evidence unless the pipe was actually created

Two different configurations boot reliably *because `start()` gave up early*, not because
they fixed anything:

- `-itlnocc` skips `initCCLogs()` and fails at `createIOReporters`.
- `-itlmincc itlccsize=64` was **rejected by the kernel**. `CCLogPipe::init...` requires
  `min_log_size_notify <= pipe_size`, and `initCCLogs()` used to hardcode
  `min_log_size_notify = 0xccccc` (819 KB), so every `itlccsize` under 820 KB returned NULL
  and aborted `start()`. The knob only worked upward. Fixed: `ITLWM_CC_NOTIFY(size)` is
  `size * 2 / 5`, which reproduces `0xccccc` exactly at the stock 2 MB and scales below it.
  Full validation rules are documented at the `CCPipeOptions` declaration in `CCPipe.h`.

**`AirportItlwmStage` alone cannot answer "was a pipe created".** `STAGE(2)` sits after
`super::start()`, so Stage 1 is ambiguous between a rejected pipe and a good pipe followed
by a failed `super::start()` — and `-itlmincc` builds no fault reporter, which is its own
reason for `super::start()` to fail. `ItlwmCCPipeOK` is published between the two and is
the unambiguous answer. Stage 9 still means `super::start()` succeeded, and neither
`ItlwmPreinit*` nor `ItlwmCmd*` can exist below it.

#### Apple's `ccpipe:` boot-arg — resize a pipe without rebuilding

`CCPipe::initWithOwnerNameCapacity` parses `PE_parse_boot_argn("ccpipe:<PipeName>", ...)`,
where `<PipeName>` is the third argument to `withOwnerNameCapacity` (`DriverLogs`,
`DatapathEvents`, `StateSnapshots`) — not the owner name. The value is a comma-separated
positional list, recovered from the 7-entry jump table at `0xffffff80032e5800`:

```text
ccpipe:<PipeName> = LogType, LogPolicy, LogSize, MinLogSizeToNotify, NotifyTimeout, FileSize, NumberOfFiles
```

The override is applied **inside `CCPipe::init`, which `CCLogPipe::init` calls only after
validating the caller's raw struct, and before it reads `getPipeSize()` for the
allocation**. So a `LogSize` override changes the real allocation while the driver's own
struct still passes the `min <= size` gate at 2 MB. That makes it the right way to test
pipe size: the kext binary under test stays byte-identical to the one that produced the
existing hang record, so the experiment has exactly one variable.

Success is visible afterwards in `log show`: `Boot-Arg Key: %s, Value=%s` followed by
`LogSize is overriden %s`.

#### `-itlmincc` was an invalid experiment — it guarantees a panic

`IO80211Controller::start()` panics `"No ivars->_faultReporter" @IO80211Controller.cpp:2877`
when the driver has no CoreCapture fault reporter. The panic is *inside*
`findAndAttachToFaultReporter`, at `+0x10f`: slot 432 returning NULL takes the `+0x92` branch,
which logs `has no faultReporter`, re-tests `ivars+0x58`, emits one `logDebug`, tests a third
time and panics. (An earlier version of this note said the function stored NULL and returned
success, with the failure surfacing in a later `logDebug`. It does not return.)

`-itlmincc` used to skip `driverSnapshotsPipe` and `driverFaultReporter` by design, so it
panicked every time. Every result gathered under it is void: the `ccpipe:` pipe-size test, the
`wlan.lqm.logging=0` test, and `-itlnohal` (which panicked before reaching its own check, so
the HAL is untested, not exonerated). Do not reintroduce a diagnostic mode that omits
something Apple's `start()` requires.

`initCCLogs()` returns false unless the fault reporter exists, so the driver either has one or
never reaches `super::start()`. That guard is now unconditional: `-itlmincc` builds the
snapshots pipe and the whole reporter chain and only skips the datapath pipe, so no boot-arg
combination can reach Apple's `start()` without a reporter.

#### A pre-root-mount panic looks exactly like a hang

Panic reports are written to `/Library/Logs/DiagnosticReports/*.panic` only once the
filesystem is up. A panic before root mount leaves **no report and no panic UI** — the verbose
console simply stops. That is why every early "hang" produced nothing on disk while the same
same panic under the (since-deleted) 30 s deferral, firing after login, wrote a full report.

It also explains the wandering hang site. The panic is at a fixed point in the driver's own
timeline, but `start()` runs asynchronously against boot progress, so it lands wherever boot
happens to be — USB enumeration, root mount, Ignition. **A wandering stop site is evidence of
an asynchronous fixed fault, not of memory corruption.** Always check for `.panic` files
before theorising, and treat their absence as evidence about timing, not about the fault.

#### RESOLVED: the Tahoe boot hang was the `-itlmincc` panic

A 30 s deferral with full CoreCapture booted reliably and reached `AirportItlwmStage = 9` with
`ItlwmCCPipeOK = 1`. The boot problem is closed — and the deferral has since been deleted as well,
because `-itlnodefer` booted 5/5 once the faults below were fixed.

The whole investigation chased phantoms because the diagnostic flag itself was the fault.
Successively blamed and cleared: CoreCapture pipe size, CoreCapture pipe creation, the
`CCPipe` registry attach, class-size/heap corruption, vtable truncation, `IO80211Controller::
start()`'s LQM path, and the HAL firmware attach. **None of them was ever the cause.** Each
"correlation" was really a correlation with `-itlmincc`, which panics unconditionally.

Sequence that works: build full CoreCapture and let Apple's `start()` run. Publishing the nub
late was part of this sequence and turned out to be unnecessary once the fault reporter was
correct; it is deleted. The remaining defect is the HAL attach failing cleanly — mark 6 / errno 35 —
which is a timeout, not a crash, and is diagnosable from a running machine.

Method notes worth more than the findings:

- A diagnostic mode that omits something the code under test requires is worse than no
  diagnostic. `-itlmincc` cost four boots and three withdrawn conclusions.
- Check `/Library/Logs/DiagnosticReports/*.panic` FIRST. It is in the root runbook and would
  have ended this in one step.
- A stop site that wanders between boots means an asynchronous fixed fault, not corruption.
- Instrumentation that samples state after a teardown path measures the teardown. See
  `ITLWM_PREINIT_SNAP` in `ItlIwx.cpp`.

#### IO80211Controller::start() — the map, and Apple's own boot-args

Order of operations (26.6, `__ZN17IO80211Controller5startEP9IOService` @ `0xffffff8002290f30`):

```text
IOEthernetController::start        via a global vtable, not our chain
store provider -> state+0x368      state block is this+0x120
makePlane("IO80211") + attachToParent(registryRoot)
setPropertyHelper x3
getLogger()                        slot 424 -> stored into a GLOBAL
createIOReporters()                -> IO80211ControllerMonitor::withControllerAndProvider
PE_parse_boot_argn("wlan.lqm.logging", &state+0x4cc, 4)    default 3, >=4 is rejected to 0
setupControlPathLogging()          -> setupIoctlAndEventLogging() + setupLQMLogging()
findAndAttachToFaultReporter()     slot 432 = getFaultReporterFromDriver
new IO80211RangingManager + initWithController
getWorkQueue()                     slot 397
IO80211CommandGate::allocWithParams
PE_parse_boot_argn("wlan.disableIOCTL", &state+0x37c, 4)
```

Two things here matter more than they look:

- **Apple's own `start()` creates two more `CCPipe`s**, inside `setupIoctlAndEventLogging()`
  and `setupLQMLogging()`. So a boot that reaches `setupControlPathLogging` has our pipes
  *plus* Apple's. Any experiment that counts pipes must account for these.
- **`setupLQMLogging` only runs when `state+0x4cc == 3`**, and `wlan.lqm.logging` sets that
  field before `setupControlPathLogging` runs. `wlan.lqm.logging=0` therefore removes
  exactly one of Apple's two pipes with no code change — a bisect *inside* Apple's start().
  `wlan.disableIOCTL` is parsed too late to gate anything in that path.

`findAndAttachToFaultReporter()` allocates a 0x358 scratch, calls slot 432, and **panics at
`+0x10f` if the result is NULL**. It returns 0 only when a reporter exists. See "The fault
reporter is three objects" below for what it must be handed.

#### The fault reporter is three objects, and its type is load-bearing

Slot 432 `getFaultReporterFromDriver()` must return an **`IO80211FaultReporter *`**. Nothing
checks it. `findAndAttachToFaultReporter+0x50` stores the return value straight into controller
`ivars+0x58` — the field `IO80211Controller::getCommonFaultReporter()` reads as
`*(void **)(*(void **)(this + 0x120) + 0x58)` — and `IO80211PeerManager::initWithInterface`
later calls **vtable slot 36** on it, expecting
`IO80211FaultReporter::registerCallbacks(CCFaultReporter::register_callback_t *, unsigned int,
OSObject *, char const *)`.

This port returned a bare `CCStream` named `"FaultReporter"`, which is an `IOService`. Slot 36
of any `IORegistryEntry` subclass is
`IORegistryEntry::copyProperty(OSString const *, IORegistryPlane const *, unsigned int)`, so
Apple passed the callback struct where an `OSString *` was expected:

```text
IO80211PeerManager::initWithInterface
  +0x1024  ivars+0x520 -> +0x120 -> +0x58   getCommonFaultReporter(), inlined
  +0x104b  NULL -> +0x1434 -> panic "no ivars->_faultReporter" @IO80211PeerManager.cpp:1558
  +0x1059  build a 0x28 callback struct at rbp-0x70; [0] = 0
  +0x10b5  call [reporter_vtable + 0x120]   slot 36
           -> IORegistryEntry::copyProperty -> getProperty -> OSSymbol::withString(&struct)
           -> loads the vptr from [struct+0] = 0, calls [0 + 0x38]
           -> page fault, CR2 = 0x38, RAX = 0
```

So there are **two** panic sites and neither is escapable: NULL panics at
`IO80211Controller.cpp:2877`, a wrong type faults at `OSSymbol::withString+0x18`. Both are
mandatory-reporter panics wearing different clothes.

`IO80211Family` never builds one — the only `IO80211FaultReporter::allocWithParams` call site
in the whole kernel collection is `AppleBCMWLANBusInterfacePCIe::deferredStart`. The vendor
driver owns it, so this port has to build the chain bottom-up:

```c
CCDataStream    *s = CCDataStream::withPipeAndName(snapshotsPipe, "FaultReporter", &opts);
CCFaultReporter *c = CCFaultReporter::withStreamWorkloop(s, ownWorkLoop);
IO80211FaultReporter *r = IO80211FaultReporter::allocWithParams(c);   /* slot 432 returns r */
```

Each step returns NULL if its input is NULL, so each needs checking:

- `CCFaultReporter::initWithStreamWorkloop+0x24` requires **both** arguments non-NULL, then
  stores and retains the stream (`ivars+0x48`) and the workloop (`ivars+0x50`).
- `IO80211FaultReporter::init+0x1d` requires a non-NULL `CCFaultReporter`.
- The workloop must be a real `IOWorkLoop`. `getWorkLoop()` is NULL at this point —
  `initCCLogs()` runs before `super::start()`, so `IONetworkController::_workLoop` does not
  exist — and `_fWorkloop` is an `IO80211WorkQueue`, not an `IOWorkLoop`, and is itself only
  created inside Apple's `start()`. The driver owns a dedicated one.

All three factories are `n_type 0x0f` (defined + external): the two `CC*` in
`com.apple.driver.corecapture`, `allocWithParams` in `com.apple.iokit.IO80211Family`. Only the
Tahoe target uses this chain; the pre-Tahoe targets still return a `CCStream`, which is the same
type confusion but latent there — those targets work, and switching them would add three export
dependencies verified only on 26.6, where a missing one is a load failure rather than a bug.

#### The controller ABI is verified sound

Checked against 26.6 with the `scripts/abi/` tooling: instance sizes match Apple exactly for
all nine reconstructed classes; `IO80211Controller` owns vtable slots 0..461 (AppleBCMWLANCore's
own begin at 462) and our `AirportItlwm` table is 463 — 462 inherited plus one we add; and
`mapdrv.py` reports 45/45 overrides on correctly-named slots, 0 wrong. The `uint8_t[16] filler`
at offset 280 covers Apple's state pointer at `this+0x120`, which `IO80211Controller::init`
allocates via `IOMallocTypeImpl` — we do call it, so those bytes are Apple-managed and must
stay untouched. Layout is therefore not the explanation for the Tahoe hang.

`AirportItlwmEthernetInterface` is verified too, and was not covered by the `mapdrv.py` pair
above. Against Tahoe's real `IOEthernetInterface` (340 slots) all 338 comparable slots match
by name, and its two own virtuals — `initWithSkywalkInterfaceAndProvider`, `setLinkState` —
sit at 340 and 341. Clang's dump reports 345 because it counts `offset_to_top`, RTTI, and a
trailing `vcall_offset`; subtract those three before comparing.

#### Reading a panic backtrace through IONetworkingFamily

**Tail `jmp`s make this backtrace lie, twice over.**
`IOEthernetInterface::performGatedCommand` reaches every `syncSIOC*` handler by tail `jmp`,
and `syncSIOCSIFFLAGS` in turn *ends* with one:

```text
+0x106  mov rax, [rbx]              ; rbx = the interface — ours
+0x118  call qword ptr [rax + 0x918]  ; slot 291 IONetworkInterface::setFlags
+0x121  mov rax, [rax + 0x788]        ; slot 241 IOService::errnoFromReturn
+0x13c  jmp rax                       ; pushes NO return address
```

A `jmp` leaves whatever was already at `[rsp]`, so the target inherits
`executeCommandAction+0x43` — two frames up — and the walker reports the fault there. Every
panic in this path named `executeCommandAction`; none of them was in it.

**Reconcile the register dump with the candidate's prologue before believing any frame.**
That is what eventually cracked it:

- `RSP = RBP - 0x18` means the faulting call came from a function that pushed exactly two
  registers. `executeCommandAction` pushes four, `performGatedCommand` six, and the four
  `syncSIOC*` handlers 6/4/4/6 — so none of them. The real caller was
  `IO80211SkywalkInterface::errnoFromReturn` (`push rbp; push r14; push rbx`), reached
  through the mis-bound slot 241.
- `RBX = 0` was the giveaway all along: `executeCommandAction` sets `rbx = rdi` after
  testing it non-NULL, so it can never be 0 there. In `errnoFromReturn` it is `mov ebx, esi`
  with `esi` = the status code, which was 0.

An NX fault on instruction fetch (`error code 0x11`, `CR2 == RIP`) means the callee never
executed a single instruction, so `RBP` still belongs to whoever called it and the whole
frame chain below is suspect.

#### What a CCLogPipe actually allocates

```text
CCLogPipe::init -> getPipeSize() -> round_page()
  -> IOBufferMemoryDescriptor::inTaskWithGuardPages(kernel_task, 0x10003, size)
  -> kmem_alloc_guard(kernel_map, size + 2 pages, ...)      ; non-pageable
  -> memset_s(bytes, len, 0, len)                           ; every page faulted in
```

Eager, wired, guard-paged, and fully touched — all inside `AirportItlwm::start()`. The
stock configuration therefore wires and zeroes 2 x 2 MB before `super::start()` runs. The
`StateSnapshots` CCDataPipe allocates no ring at all (its "size" is a blob count), so only
the two CCLogPipes matter. This is what makes the size hypothesis physically plausible;
it is not yet proof of a deadlock.

Read them with `ioreg -r -n IOPCIEDeviceWrapper -l -w0 | grep -i itlwm`. Without `-r`,
ioreg also prints the root node's `IOKitDiagnostics` dictionary, a single line long enough
to bury the properties and truncate the output.

### The warm/cold reboot correlation was coincidence

Track it separately from the attach failure. It does not correlate with the attach failure
(which happens on cold and warm boots alike), a failed `start()` returns cleanly rather
than hanging (`AirportItlwmV2.cpp:331-342`), and the user reports the split follows warm
reboot vs cold power cycle — the classic signature of device or platform state that
survives a warm reset and is cleared only by removing power.

Note the user runs `itlwm.kext` daily on this machine and it boots every time, so if the
hang is ours it is specific to the AirportItlwm/IO80211Family path, not the shared HAL.

**CAUSE FOUND: creating any CoreCapture pipe before `super::start()` hangs the boot.**
Established by boot-arg bisection, all in a single unchanged binary (important — rebuilding
appears to perturb the hang, so the configurations must be compared without recompiling):

| boot-arg | CoreCapture work before `super::start()` | result |
| --- | --- | --- |
| *(none)* | 3 `CCPipe` + 2 `CCStream` | hangs |
| `-itlmincc` | 1 `CCPipe` + 1 `CCStream` | **hangs** (warm and cold) |
| `-itlnocc` | none | **boots** (4/4, warm and cold) |

One pipe is enough, so this is not contention that scales with the number of pipes — it is
the act of creating a `CCPipe` that early. Warm vs cold makes no difference, which also
retires the earlier warm-reboot correlation as coincidence.

Tahoe is the **only** target that runs `initCCLogs()` before `super::start()`; every other
target runs it late, after `super::start()` and `attachInterface`, and none has this
problem. Tahoe forces the early call because `IO80211ControllerMonitor::initWithControllerAndProvider`
fails on a NULL `getLogger()`, and Apple exports **no** `IO80211Controller::getLogger`
implementation to fall back on (it does export `getLogPipes`, but that does not help).

Ruled out, do not re-investigate:

- **Struct layout.** `CCPipe::initWithOwnerNameCapacity` reads options offsets 0x04, 0x08,
  0x0c, 0x10, 0x18, 0x20, 0x224, 0x228, 0x22c and `CCStream::initWithPipeAndName` reads
  offset 0x00 plus a pointer at 0x58 — every one matches `CCPipe.h` / `CCStream.h`. (The
  64-bit `pipe_type` is really two 32-bit fields, which the existing `0x200000001`
  assignment already reflects.)
- **A blocking wait inside CoreCapture.** `CCPipe::initWithOwnerNameCapacity` has an
  `IOSleep` retry loop, but it is bounded at 10 × 5 ms and then gives up.
- **corecapture not being loaded.** It is a hard `OSBundleLibraries` dependency and is
  present in `kextstat` well before we run.

**"Creating any CoreCapture pipe before `super::start()` hangs the boot" is the claim the table
above no longer supports** — every configuration behind it also carried an unconditional panic.
Treat the paragraph below as the historical reasoning, not as a current finding.

**FIXED by deferring publication — AND THAT FIX IS NOW DELETED, because the problem it fixed was
not the problem.** `IOPCIEDeviceWrapper::start()` delayed its `registerService()` for a
configurable number of seconds, which pushed `AirportItlwm::probe`/`start`, `initCCLogs()` and
Apple's `IO80211Controller::start()` past the boot-time matching storm and the root mount. It was
believed required. It is not: with the fault-reporter chain correct and the slot instrumentation
gone, `-itlnodefer` boots — 5/5 on 26.6.2 (25G83) — and the whole apparatus has been removed.
Before deleting it, it was made to measure what it had only ever been assumed to be waiting for:
**`boot-uuid-media` at 66 ms, registry quiescence at 17.7 s, publishing at 30 s.** A wall clock
that matches neither milestone is a race, not a precondition. Root AGENTS.md mechanism 7.

While the deferral existed it changed where a *remaining* failure showed up — boot reached root
mount, `launchd` and `Darwin Ignition` before anything of ours ran, so a hang there was ours rather
than the storage stack. **That tell is gone with the deferral**: the driver now starts early again,
so a stop site early in boot no longer rules itself out.

Also refuted along the way: `-itlccowner`, which passes the fully-started provider as the
`CCPipe` owner instead of the not-yet-started `this`, **hangs** exactly like the default.
So the fault is not about the owner being unstarted; it is purely about *when* in boot the
pipe is created. The flag is retained only as a control.

This also retires the "unexplained regression" recorded above: the finer per-wait-site
instrumentation never caused anything: the boot hang was always present and merely
timing-sensitive, so any code change shifted its odds. With the deferral in place that
instrumentation is back in the tree.

The deferral and its `ItlwmPublish*` measurements are deleted; there is nothing left to read here.
What the measurement established, kept because it is the reason the timer could go: `boot-uuid-media`
is published 66 ms after `IOPCIEDeviceWrapper::start()`, and `IOService::getServiceRoot()->waitQuiet()`
returns at ~17.7 s, against a 30 s publish. Root AGENTS.md mechanism 7.

Ruled out: `IO80211WorkQueue` inherits `addEventSource`, `removeEventSource`,
`runEventSources` and `threadMain` from `IOWorkLoop` unchanged (slots 40, 41, 53, 36), so
handing Apple's work queue to the HAL for its interrupt source is legitimate.

**`CreatePostOffice` must be bound to Apple's implementation.** With it stubbed to NULL,
`IO80211Controller::start()` logs `fPostOffice NULL !!!` and fails. `CreatePostOffice` and
`getPostOffice` are therefore declared body-less so the vtable binds to IO80211Family's
exported code.

Binding it makes `start()` continue into a region that has never executed before, and the
boot then **hangs** — no panic, no panic report. Do not "fix" it by re-stubbing
`CreatePostOffice`; that only hides the next failure.

**The hang is not inside Apple's `start()`.** The whole tail after the post-office check is
disassembled and accounted for, and every step is a plain allocation with no lock, sleep or
wait:

| after `CreatePostOffice` | notes |
| --- | --- |
| `IO80211TimerSource::allocWithParams` + one virtual on it | same path `IO80211RangingManager` already survives |
| `IO80211TimerFactory::allocWithParams` | |
| `IO80211RNGAgent::withOptions` | calls back into `getWorkQueue` (397) and `getLogger` (424); a null logger fails cleanly, it does not hang |
| `allocFramePool` | calls `getActionFramePoolCapacity` (438) — our stub returns 0, which is **harmless**: `IO80211BufferPool::withOptions(..., true)` routes to `IO80211MallocBufferPool::withOptions`, which never reads the count |
| set flag, `os_log`, `return true` | `os_log`, not `IOLog`, so this success is invisible on the boot screen |

So a hang here means execution reached `AirportItlwm::start()`'s own continuation, where
the `STAGE(n)` numbers name the exact line. `STAGE 2` on screen means `super::start()`
finally returned true.

Disassembly is the cheap way to answer "can this block?" — it costs no boot cycle, and each
boot cycle on the user's machine risks an unbootable system.

`allocIO80211RecursiveLock`, `getActionFramePoolCapacity` and `getPLATFORM_CONFIG` are
left stubbed on purpose — they are not required yet, and keeping them stubbed limits how
much untested kernel code a single bring-up step turns on.

**Bring-up instrumentation** (temporary, gated on `__MAC_26_0`, remove when done):
`STAGE(n)` in `AirportItlwm::start()` writes `AirportItlwmStage` to the *provider*, which
stays registered even when we fail to attach. `trace()` writes `ItlwmTrace_*` onto the
controller, readable via `IO80211Plane` even though the controller never reaches the
service plane. Read both with:

```bash
ioreg -l -w0 | grep AirportItlwmStage
ioreg -p IO80211Plane -l -w0 | grep ItlwmTrace
```

Note `getWorkQueue` (slot 397) is called from several Apple call sites, so it is counted
rather than flagged — a bare "was it called" bit misled the first diagnosis. Observed call
order is recorded at the function in `AirportItlwmV2.cpp`.

**Never instrument a slot Apple invokes.** Not `setProperty`, not `IOLog`, not `IODelay`.
`getWorkQueue` (397), `getLogger` (424) and `getFaultReporterFromDriver` are called from
inside `IO80211Controller::start()` and `IO80211ControllerMonitor`, in lock contexts the
driver does not control:

- `IORegistryEntry::setProperty` takes the *global* `gPropertiesLock` for write and wedged
  `start()` outright.
- `IOLog` also takes locks and can block; `IODelay` spins for milliseconds inside whatever
  region Apple is holding. With those left in, roughly **half of all boots hung** right
  after one of these slots was called. The intermittency made it look like unrelated
  changes were at fault, and two innocent changes were reverted before the pattern was
  spotted: three separate builds hung in the same narrow window, including one whose only
  delta was two integer stores in code that never ran.

Instrument only our own `start()` thread (`STAGE()`, which is safe and keeps its
`setProperty` on the provider), or record plain integers from the HAL and publish them
later (`ITLWM_PREINIT_MARK` in `ItlIwx.cpp`). When a hang is intermittent, suspect the
instrumentation before suspecting the change under test.

Removing it helped but did not fully cure the intermittency. The first boot without any
slot instrumentation got materially further than any before it — past `createIOReporters`,
`setupControlPathLogging`, `findAndAttachToFaultReporter`, `IO80211RangingManager` and the
command-gate allocation, stopping after `IO80211Controller::start, disabling IOCTL logic` —
but still hung. Nothing between that log and the next one (`controller is %p, provider is
%p`) can block: only `PE_parse_boot_argn`, `vm_kernel_addrhash`, and a `logDebug` that is
conditional on a boot-arg. So either the console tail is truncated again and the real stall
is later, or the remaining fault is elsewhere. Treat the intermittent early hang as a
**separate open bug** from the `iwx_attach` failure; boots do still succeed, so retrying is
a legitimate way to collect `ItlwmPreinitMark` while it is unsolved.

Ruled out as the cause: `CCPipe::initWithOwnerNameCapacity` does contain an `IOSleep` wait
loop — relevant because Tahoe runs `initCCLogs()` *before* `super::start()`, much earlier in
boot than other targets — but it is bounded at 10 iterations of 5 ms and then gives up, so
it cannot hang indefinitely.

That deadlock is worth recognising by its *symptom*, which looks nothing like a Wi-Fi bug:
a `start()` that never returns leaves the provider permanently busy, so the IOKit registry
never quiesces, and the boot dies at root mount — `hfs_mountroot failed: 13`,
`apfs: mountroot called!`, then silence. If a boot hangs there, suspect a driver `start()`
that never returned, not the storage stack.

**Do not trust the last line on the boot screen.** `IOLog` only queues into the kernel log
buffer; the framebuffer console drains it separately, so on a hard hang the final line or
two never render. Twice this made execution look like it stopped somewhere it provably
could not — once inside `IO80211CommandGate::allocWithParams`, whose `initWithParams` is
`OSObject::init` plus one `IOMallocTypeImpl` and cannot block. Both `trace()` and `STAGE()`
now spin briefly after logging so the console drains (20 ms in `STAGE`, 2 ms in `trace` —
long spins inside an Apple-called slot are their own hazard). The reliable tell that the
tail is *not* truncated is unrelated kernel output continuing after our last line: that
means the console kept draining and our thread genuinely stopped. When the visible tail
contradicts the disassembly, believe the disassembly.

**Kernel logging is invisible on this machine.** `XYLog` uses `kprintf`, which never
reaches the kernel message buffer, and the unified log contains zero kernel messages
(`log show --last boot --predicate 'process == "kernel"'` returns 0 lines), so `log show`
is useless. `IOLog` *does* appear on the verbose boot screen and in the message buffer, but
that buffer is 128 KB and wraps within about three minutes of uptime — by the time anyone
can log in and run `sudo dmesg`, the boot-time output is gone.

**Do not try to capture `XYLog` by mirroring it into another sink.** This was tried — a
`vsnprintf`-based collector called from the `XYLog` macro, published as an ioreg property —
and it made the machine unbootable, non-deterministically, hanging at two *different*
points in Apple's `IO80211Controller::start()` on two boots, both long before the HAL ran.
It was reverted. Why it is a bad idea:

- `XYLog` expands at thousands of call sites across the HAL and the 802.11 stack, in every
  context including interrupt context.
- `kprintf` is effectively a no-op on a machine with no serial sink, so those call sites
  have never actually formatted their arguments. A collector that really formats them is
  running brand-new work everywhere at once, including on any call site whose format string
  and arguments disagree.

The safe alternatives, in order of preference:

1. **`debug=0x8` (`DB_KPRT`) in boot-args** — routes `kprintf` to the console with no code
   change at all. Zero risk of changing driver behaviour.
2. **A few explicit markers at the specific failure exits under investigation**, recording
   an integer rather than formatting arbitrary arguments, published with `setProperty` on
   the provider from our own start thread (the pattern `STAGE()` already uses safely).

Whatever the instrument, change one thing at a time and keep a known-booting build to fall
back to.

**Every vtable slot now resolves.** An unresolved slot links as a *null pointer*, so it is
not a cosmetic warning — it is a guaranteed fault the moment the OS dispatches through it.
Keep this at zero:

```bash
nm -g /System/Library/KernelCollections/BootKernelExtensions.kc \
    | awk '$2!="U"{print $3}' | sed 's/^ *//' | sort -u > /tmp/kcdef.txt
nm -u build/Release/Tahoe/AirportItlwm.kext/Contents/MacOS/AirportItlwm \
    | sed 's/^ *//' | sort | comm -23 - /tmp/kcdef.txt | c++filt
```

Use `nm -g` on the whole collection, not a `T`/`S` filter over one architecture — a
narrower filter drops real exports and reports false unresolved symbols such as `_bzero`.

The eleven that were unresolved had four distinct causes, all worth recognising again:

- **Placeholder typedefs leak into mangled names.** `IO80211FlowQueueHash` was
  `typedef UInt64` and `if_link_status` was `typedef UInt`, so six slots mangled as
  `y`/`Py`/`PKy`/`PKj` and matched nothing. Both are now class types
  (`IO80211FlowQueueHash.h`; `if_link_status` is an opaque `struct`, XNU-private and in no
  SDK). A typedef to a builtin is never safe in a signature that has to bind.
- **Overrides declared on the wrong class.** `get/setHardwareAddress(ether_addr*)` sat on
  `IO80211InfraInterface`, but Apple implements them on `IO80211SkywalkInterface`; the
  vtable entry must name the class Apple attributes the code to. Declaring an override on
  a derived class does not move the slot, but it does change the symbol.
- **Overrides Apple does not actually make.** Tahoe's `IO80211Controller` does not override
  `setHardwareAddress(IOEthernetAddress const*)` — slot 357 keeps
  `IOEthernetController`'s. Declaring the override emitted a symbol nothing exports.
- **MacKernelSDK is stale for Tahoe.** The kernel no longer exports
  `_RESERVEDIONetworkController6`/`7`: Tahoe spent those slots on
  `allocatePacketNoWait(unsigned int)` and `setHardwareAssists(unsigned int, unsigned int)`,
  while the SDK header still declares both `OSMetaClassDeclareReservedUnused`.
  `AirportItlwmV2.cpp` defines the two stubs so the slots are inert rather than null. This
  is containment, not a fix — the slots still cannot carry Apple's behaviour. The real fix
  belongs in the SDK header. Re-check after every MacKernelSDK update.

### Layout reference

The layout port is **done and machine-verified**: `mapdrv.py` reports zero overrides on a
wrong slot for both chains (202/202 on `IO80211InfraProtocol`, 44/44 on
`IO80211Controller`), and `ownslots.py` shows every class in the chain matching Tahoe's
slot counts exactly. Instance sizes are pinned by `static_assert`.

It has still never been booted. Two things remain unverified by any check available here:

- The semantics of the new `IO80211SkywalkInterface` peer/data-path methods
  (`createPeer`, `attachPeer`, `findPeer`, `syncDPSStats`, `setRxFlowSteering`) — the slots
  are right, the behaviour is a guess.
- ~~`setWCL_ASSOCIATE`'s struct layout~~ — **resolved.** It is a rename, not a new type, and the
  layout is reconstructed in `AssocCandidates.h` from the producer pair
  `WCLJoinRequest::fillAssocCandidatesList` / `::addAssocCandidates`. Nothing calls it yet; the
  layout being right is necessary but not sufficient, and the open questions are listed under
  mechanism 15 in the root `AGENTS.md`.

The 68 Tahoe-only accessors are `kIOReturnUnsupported` stubs, and slot 468 on
`IO80211InfraInterface` is an unnamed pure virtual filled with a no-op placeholder.

Historical detail of the port follows.

### What changed between 14.4 and 26

These headers describe macOS 14.4. The `AirportItlwm-Tahoe` target compiles but **must not
be booted** until the layout is ported. Established against Tahoe 26.6's shipping
`IO80211Family` with `scripts/abi/`, using `AppleBCMWLANInfraProtocol` and
`AppleBCMWLANCore` as ground truth (they name 667/668 and 635/635 slots respectively).

**Vtable layout drift.** 232 of 233 `AirportItlwmSkywalkInterface` overrides and 22 of 49
`AirportItlwm` overrides land on the wrong slot; 14 driver slots run past the end of
Tahoe's vtable. Per class, own-slot counts move `IOSkywalkNetworkInterface` 50→51,
`IOSkywalkEthernetInterface` 21→20, `IO80211SkywalkInterface` 93→108,
`IO80211InfraInterface` 5→8, `IO80211InfraProtocol` 231→199. `IOSkywalkInterface` is
unchanged.

**The driver contract itself is intact.** Tahoe's required surface is small and largely
already implemented:

- `IO80211Controller` — 69 own slots, only **12 pure**: `isCommandProhibited`,
  `handleCardSpecific`, `get/HARDWARE/DRIVER_VERSION`, `getCARD_CAPABILITIES`,
  `get/setPOWER`, `get/setCOUNTRY_CODE`, `setGET_DEBUG_INFO`, `getLogger`,
  `getFaultReporterFromDriver`. All exist in AirportItlwm today. The old
  `apple80211_ioctl*`, `apple80211VirtualRequest`, `enable/disable(IO80211SkywalkInterface*)`
  and `postMessage` virtuals became `_RESERVEDIO80211Controller0..15` padding — the family
  no longer calls them.
- `IO80211InfraProtocol` — 199 own slots, all pure. **AirportItlwm already implements 131.**
  The 68 new ones are config/offload/telemetry (`setMWS_*_WIFI_ENH`, `setTIMESYNC_*`,
  `setIPV4_PARAMS`/`setIPV6_PARAMS`, `setDEVICE_ORIENTATION`, `setPOWER_PROFILE`,
  `setBTCOEX_EXT_PROFILE`, …); none is a control-path primitive, so they can be honest
  `kIOReturnUnsupported` stubs.

**Every control-path verb survives, with the signature these headers already declare**:
`setWCL_ASSOCIATE`, `setWCL_SCAN_REQ(apple80211ScanRequest *)`, `setWCL_LEAVE_NETWORK`,
`setWCL_REASSOC`, `setWCL_JOIN_ABORT`, `setWCL_SCAN_ABORT`, `setCIPHER_KEY`, `setCHANNEL`,
`setIE`, `setWCL_ACTION_FRAME`, `setRSN_XE`. The only spelling delta is
`setWCL_ASSOCIATE`, declared here as `apple80211_assoc_candidates *` and named
`apple80211AssocCandidates` in Tahoe — **a rename, confirmed**: the reconstruction in
`AssocCandidates.h` matches what `WCLJoinRequest` fills, including a candidate array whose
10-entry bound falls out of the producer's own arithmetic.

What Tahoe *did* remove is BSS bookkeeping — `getSSID`, `getBSSID`, `getSTATE`,
`getSCAN_RESULT`, `getPHY_MODE`, and the legacy non-`WCL_` duplicates (`setASSOCIATE`,
`setSCAN_REQ`, `setDISASSOCIATE`). `IO80211Family` now tracks that itself in a 45-class
`WCL*` subsystem (`WCLBssManager`, `WCLJoinManager`, `WCLScanManager`, `WCLFsmManager`, …)
that lives in **IO80211Family, not the Broadcom driver** — so it is shared infrastructure,
not a vendor-private path.

Instance sizes, recovered with `scripts/abi/classsize.py` and pinned by `static_assert`:
`IOSkywalkInterface` 0xb0 (unchanged), `IOSkywalkNetworkInterface` 0xe0,
`IOSkywalkEthernetInterface` 0x120, `IO80211SkywalkInterface` 0x128,
`IO80211InfraInterface` 0x130, `IO80211InfraProtocol` 0x130, `IO80211Controller` 0x128.
`IOSkywalkNetworkInterface` was **undersized by 16 bytes** before the port, which corrupts
memory silently rather than panicking — trust the asserts over hand arithmetic, and note
that `IOSkywalkEthernetInterface` needed no padding change even though its size grew,
because the growth came entirely from its base.

**A correct `sizeof` is not evidence that a member offset is correct.** Those 16 bytes were
first modelled as trailing padding, which left `IOSkywalkNetworkInterface::mExpansionData` at
0xb8 while Tahoe puts it at 0xc0 — 8 of the 16 land *ahead* of it. `AirportItlwm::start`
then read a NULL neighbour and panicked writing through it (`CR2 = 0`, page fault on write,
`AirportItlwmV2.cpp:663`). Any member the driver dereferences needs its own
`__offsetof` assert recovered from the binary, not just a class-size assert.

Recover a member offset by disassembling the accessors that Apple compiled against the real
layout — `init` and `free` are the reliable pair, since one allocates the field and the other
frees it:

```bash
python3 -m venv scripts/abi/.venv && scripts/abi/.venv/bin/pip install capstone
scripts/abi/.venv/bin/python scripts/abi/kdis.py __ZN25IOSkywalkNetworkInterface4initEP12OSDictionary 45
scripts/abi/.venv/bin/python scripts/abi/kdis.py __ZN25IOSkywalkNetworkInterface4freeEv 60
```

Verified this way and pinned by `__offsetof` asserts: `IOSkywalkNetworkInterface`
`mExpansionData` at 0xc0, `IOSkywalkEthernetInterface` `mExpansionData2` at 0x118, and
`fRegistrationInfo` at offset 0 within `ExpansionData` (`registerNetworkInterface` stores the
`IOMallocType` result straight to `[mExpansionData]`).

**Never hand-allocate a buffer that Apple frees, and never leave one NULL because the free
path tolerates it.** Both halves were learned the hard way, one panic each.

Hand-allocating is fatal on Tahoe even at the right size: `IOMalloc` lands in
`kalloc.type.var4.*`, `IOMallocData` in `data.kalloc.*`, and `IOFreeType` demands
`early.kalloc.288` — a *boot-ordering* property, not a type property, so no allocator
reachable from an injected kext can produce it (`not in the expected zone early.kalloc.288,
but found in kalloc.type.var4.384`). `zprint | grep kalloc` shows the families side by side.

But NULL is not safe either, and **a null-tolerant free path proves nothing about the
readers**. `IOSkywalkNetworkInterface::prepareBSDInterface` dereferences
`mExpansionData->fRegistrationInfo` with no check at all, to read the MTU at offset 0x4c —
`CR2 = 0x4c`, page fault on read. Before leaving any Apple-owned field NULL, grep the whole
family for readers, not just `free`/`deregister`.

There are two such buffers and they need different answers. Both are installed from
`AirportItlwmSkywalkInterface::prepareBSDInterface`, which is virtual (slot 285 of
`IOSkywalkNetworkInterface`) and the outermost hook in the chain —
`IO80211InfraInterface`, `IO80211SkywalkInterface` and `IOSkywalkEthernetInterface` all
delegate to super before doing their own work.

`fRegistrationInfo` is not the only such field. `IO80211SkywalkInterface::createEventPipe`
does `mov rax,[this+0x120]; mov rdi,[rax+0xa8]; mov rax,[rdi]` with no check anywhere, and
`state[0xa8]` is written in exactly one place — `IO80211SkywalkInterface::start`. `wifip2pd`
opens an `IO80211APIUserClient` and asks for that pipe roughly 20 s after login, so if `start`
left the field unset the first P2P client to appear takes the machine down.
`AirportItlwmSkywalkInterface` overrides `createEventPipe` to refuse when the field is NULL,
and its `start` override records `ItlwmSkyIfStarted` / `HasState` / `HasEvtSrc` so the reason
is readable in ioreg on a boot that now survives. Losing AWDL/P2P beats a panic.

**The pattern generalises: Apple's private classes assume state that only their own
registration/start paths create, and check almost none of it.** Where this driver skips such a
path, the omission surfaces later as a NULL dereference deep inside Apple code, on a thread we
do not control. Expect more. The remedy is the same each time — find the single writer with a
scan over the family, then either call it or refuse the operation that needs it.

**Ethernet (`mExpansionData2`, 304 bytes, written at 0x20): let Apple allocate it.**
`IOSkywalkEthernetInterface::copyRegistrationInfo` is the standalone allocator that
`registerEthernetInterface` itself calls first — `IOMallocType`, `memmove` 0x130 bytes, then
fill the MAC at +0x108. It validates `info->[0] == 1` and `info->[4] >= 0x130`, exactly what
`initRegistrationInfo` writes, so the pair is the whole recipe. It needs no pool, queue or
logical link, and it is not virtual, so declaring it binds straight to the exported symbol.
This is the sanctioned path; prefer it over any hack.

**Network (`mExpansionData`, 264 bytes, MTU read at 0x4c): lent from a static.** Nothing
short of `registerNetworkInterface` allocates it, and that needs a logical link → queue set →
real `IOSkywalkPacketQueue` objects.

A loan is only safe if **every** path that frees the field takes it back first, so enumerate
them by scanning the kernel collection for direct `call`/`jmp` sites targeting
`deregisterNetworkInterface`. There are four, resolving to three reachable entry points, all
virtual and all overridden in `AirportItlwmSkywalkInterface`: `free`, `stop`, and
`deregisterLogicalLink` (reached by tail `jmp`, which is why it does not show up in
backtraces). The fourth, `deregisterEthernetInterface`, is not virtual but is only called
from `registerEthernetInterface` — never called here — and from `AppleEthernetRL`. The
ethernet buffer is freed inline by `IOSkywalkEthernetInterface::free`, covered by the same
`free` override. Reclaiming is identity-checked, so an Apple-allocated buffer is left alone
and still freed normally.

**Rerun that caller scan when porting to a new release.** A new caller turns every unload
into a zone panic, and a tail `jmp` caller will not appear in any panic backtrace.

**A loan must be permanent, not scoped to one call.** Scoping it to `prepareBSDInterface`
only moves the fault: `ioctl_sifcap`, `ioctl_gifmedia` and `ioctl_sifmedia` read the same
field, in the `ifnet_ioctl` that `attachNetworkInterfaceToBSD` issues moments later.

Enumerate the readers before assuming any Apple-owned field is safe to leave NULL — scan for
the `[this+<expansion-offset>]` load followed by a dereference of that register, across
`IOSkywalk*Interface`, `IO80211SkywalkInterface`, `IO80211InfraInterface` and
`IO80211InfraProtocol`. That scan turns "one boot per discovered reader" into one pass; it
found ~15 for `fRegistrationInfo`, only one of which was in `prepareBSDInterface`.

### Full Skywalk registration IS reachable — the earlier "not reachable" reading was wrong

**Retained as a worked example of a specific mistake: a conclusion drawn from one overload of an
overloaded function.** The old text said registration was unreachable because
"both `registerNetworkInterface` overloads bottom out in the logical-link one", which
dereferences its `IOSkywalkLogicalLink*` and rejects NULL — and the only way to build a link is
`createWithQueueSet` → `createWithQueues` → real `IOSkywalkPacketQueue` objects. Every one of
those statements is true. The conclusion does not follow, because **Apple does that construction
for you**:

```text
IOSkywalkNetworkInterface::registerNetworkInterface(info, IOSkywalkPacketQueue **, uint n, pool, pool, uint)
    IOSkywalkQueueSet::isWMMSchedModelCapable(queues, n)
    IOSkywalkQueueSet::createWithQueues(queues, n, &QueueSetInfo_s)          <- built internally
    IOSkywalkLogicalLink::createWithQueueSet(&queueSet, 1, &LogicalLinkInfo_s)  <- built internally
    IOSkywalkNetworkInterface::registerNetworkInterface(info, logicalLink, pool, pool, uint)
```

So `IOSkywalkQueueSet`, `IOSkywalkLogicalLink`, `QueueSetInfo_s` and `LogicalLinkInfo_s` are
**never touched by the driver** and need no reconstruction. `IOSkywalkLogicalLink.h` can stay an
empty stub. The reachable entry point is
`IO80211InfraInterface::registerInfraEthernetInterface` → `registerEthernetInterface` →
`copyRegistrationInfo` + the queue-array `registerNetworkInterface` above.

**The rule: when a function is overloaded, disassemble the overload you would actually call.**
The mangled names differ only deep in the suffix (`...EPP20IOSkywalkPacketQueuej...` versus
`...EP20IOSkywalkLogicalLink...`), which is exactly how the wrong one gets read. This single
misreading shelved the work as a "feature project" and left mechanisms 1, 10 and 12 in place.

What the driver does need is small, and **none of it is a vtable**: `withName` /
`withPool` are exported static factories, and the data-path callbacks are plain C function
pointers (Apple passes non-virtual member functions, with the owner as the `OSObject *target`
argument). That is the declaration-only pattern above — no vtable, no per-release obligation.
Still to pin: `IOSkywalkPacketBufferPool::PoolOptions` (see below) and whatever `IOSkywalkPacket`
accessors our callbacks must call — that last one is the only remaining ABI question, since it is
the one Skywalk class we would call methods *on*.

**`PoolOptions` is `>= 0x28` and the existing header's is wrong.** From
`AppleEthernetRL::startInterface`, which sets every field:

| off | field | value used |
| --- | --- | --- |
| 0x00 | u32 packet count | `Tx/RxSubmissionQueue::getEffectiveCapacity(0x100) + …CompletionQueue::…` |
| 0x04 | u32 buffer count | same |
| 0x08 | u32 buffer size | `0x10000` tx pool, `0x4000` rx pool |
| 0x0c | u32 | 1 |
| 0x10 | u32 | 0 |
| 0x14 | u32 | 1 |
| 0x18 | ptr | a memory/DMA spec struct (its `+0x30` is the driver's `IOPCIDevice`-ish object) |
| 0x20 | u64 | 0 |

The header in the tree declares `pad` as a `uint64_t` at 0x18 — that slot is a **pointer**, and
the struct is short by at least 8 bytes. Nothing uses it yet, so this has cost nothing so far;
fix it before the first caller. `initWithName` validates `mode < 3` and `options != NULL`.

**Registration and the data path are NOT separable, and an earlier draft of this section said they
were.** The ifnet on the Skywalk path is created by **`IOSkywalkNetworkBSDClient`**, whose
`gatedPrepareNexus(__ifnet *)` is the sole Skywalk-side caller of `prepareBSDInterface` (slot 285).
It prepares a *nexus*, so the interface it publishes is backed by the queues — traffic through that
ifnet flows through the pool and the four queues, not through the legacy path. Registering for real
therefore means the Skywalk interface owns the ifnet and
`AirportItlwmEthernetInterface`'s BSD attach must be retired at the same time, or the machine ends
up with two network interfaces. **Size this as: no new vtables, but a real TX/RX data path.**

That also corrects the caller named in `AirportItlwm/AGENTS.md` mechanism 12. Apple does hold the
gate — the `gated` in `gatedPrepareNexus` says so — but it reaches the hook from the BSD client,
**not** from `registerNetworkInterfaceWithLogicalLink`, which touches steering rules and UUIDs and
never calls it. The conclusion stood; the named function did not.

**What the callbacks must call, which is the only remaining ABI obligation.** Recovered from
`AppleEthernetRL::dequeueTx4DWLegacyPackets`. Three virtuals on the packet, one on the segment, and
three exported non-virtual accessors:

| call | kind |
| --- | --- |
| `IOSkywalkPacket::getPacketBuffers(IOSkywalkPacketBuffer **, uint) const` | virtual, slot 36 |
| `IOSkywalkPacket::getDataLength() const` | virtual, slot 40 |
| `IOSkywalkPacket::getDataOffset() const` | virtual, slot 42 |
| `IOSkywalkMemorySegment` slot 42 | virtual |
| `IOSkywalkPacketBuffer::getMemorySegment() const` | exported, non-virtual |
| `IOSkywalkPacketBuffer::getMemorySegmentOffset() const` | exported, non-virtual |
| `IOSkywalkNetworkPacket::getVlanTag(uint16_t *) const` | exported, non-virtual |

So `IOSkywalkPacket` (61 slots) and `IOSkywalkMemorySegment` (43) do need enough reconstruction for
those indices to land correctly — we call virtuals *on* them, even though we never subclass them.
That is transcription rather than discovery: both tables are already in
`scripts/abi/abi-26.6-25G72.txt`. Verify by dumping the header's vtable and comparing slot names
against the baseline; `mapdrv.py` does **not** cover this case, because it checks the slots we
override, not the slots we call.

The two `RegistrationInfo` structs are **different sizes** and must not be used
interchangeably: `IOSkywalkEthernetInterface::RegistrationInfo` is 304 (`initRegistrationInfo`
rejects anything else with `cmp rcx, 0x130`), `IOSkywalkNetworkInterface::RegistrationInfo`
is 264 (`kt_size` 0x108 in the `kalloc_type_view`, and `mov edx, 0x108` before the fill
`memmove`). Sizes for releases before Tahoe were never checked against a binary.

To read a type's true size out of a `kalloc_type_view`, take the `lea rdi, [rip + N]` feeding
`IOMallocTypeImpl`/`IOFreeTypeImpl` and decode `kt_size` at offset 44 (`zone_view` is 32
bytes, then `kt_signature`, `kt_flags`, `kt_size`).

## Child DOX Index

- No child AGENTS.md files. All files in this folder are owned here.
