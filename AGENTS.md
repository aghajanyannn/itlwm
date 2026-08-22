# AGENTS.md

## Access

- Never write to files outside this repo without explicit permission.
- Never read gitignored files in a way that exposes the secrets within them.

## Project

itlwm is an Intel Wi-Fi driver for macOS, ported from the OpenBSD `iwm`/`iwn`/`iwx`
drivers. It builds two different kinds of kext from one shared 802.11 stack:

- **itlwm.kext** — a plain `IOEthernetController`. The system sees a wired Ethernet
  interface; Wi-Fi is configured out-of-band by HeliPort. Works on any macOS version
  because it depends on no private Apple API.
- **AirportItlwm.kext** — a real Wi-Fi driver that subclasses Apple's *private*
  `IO80211Family` classes, so native macOS Wi-Fi UI, preferred networks, and Handoff work.

`AirportItlwm` is built once per macOS release, because it inherits from private Apple
C++ classes whose vtable layout and instance size change between releases. Each target
pins `__IO80211_TARGET` to the release it was reverse-engineered against.

Deployment target 10.13 (`itlwm`) / 10.15 (`AirportItlwm` V2 targets); x86_64 only.

### The `__IO80211_TARGET` contract

`include/Airport/*.h` are hand-written reconstructions of Apple's private IO80211 classes.
They are **ABI declarations, not ordinary headers**: the order and count of `virtual`
methods must match Apple's shipped binary for the targeted release exactly. A single
inserted, removed, or reordered virtual makes every later slot resolve to the wrong
function and panics the kernel on load.

Correct headers are necessary but not sufficient. Six failure modes survive a clean vtable
diff, and all six have bitten this port. Two are about the table:

- a member offset can be wrong while `sizeof` is right;
- the kext loader can bind an *inherited* slot to the wrong function entirely — the table is
  correct on disk and wrong in memory.

Four are about the *values crossing* a correctly-bound slot, in either direction, and no build
or `mapdrv` check can see any of them:

- a slot stubbed to return NULL where the family requires a real object (`allocIO80211RecursiveLock`);
- a slot returning an object of the wrong class, which the family then dispatches a virtual on
  (`getFaultReporterFromDriver`);
- an argument whose value selects behaviour rather than describing data — `postMessage`'s
  trailing `bool` picks between an asynchronous queue and a synchronous send that asserts its
  caller's thread and gate;
- a field the family **discards unless a companion validity bit is set**. `BeaconMetaData`'s RSSI
  is at the right offset in the right units and was thrown away for a whole boot cycle because
  flags bit 14 was clear. This one is the most dangerous shape, because nothing fails: scan
  results appeared, correctly named, every one of them reporting 0 dBm. A struct crossing a slot
  needs its *flag* word decoded as carefully as its layout — treat every bit as gating something
  until the disassembly says otherwise.

A correct signature says nothing about what the family does with what crosses it. Establish that
first — `scripts/abi/kdis.py` on Apple's own implementation, and on the corresponding call in a
shipping Apple driver to see what a real caller passes. Disassemble the override that is *bound*,
which `scripts/abi/tahoe-26.6-slots.txt` names per slot, not the base class that declares the
method. See `include/Airport/AGENTS.md`.

Some slots also carry a **calling-context** contract that no signature expresses: `postMessage`
ends in a send that panics unless the caller holds the interface work queue's gate and is not its
thread. See `AirportItlwm/AGENTS.md`.

Version differences are expressed with `#if __IO80211_TARGET >= __MAC_xx_y` around the
affected declarations. `__MAC_*` constants come from the SDK, with fallbacks in
`itlwm/PrivateSPI.pch` so older Xcode still compiles.

Adding a macOS release therefore means two separate jobs, and the second is the real one:

1. Add an Xcode target that defines `__IO80211_TARGET=__MAC_<new>` (mechanical).
2. Port `include/Airport/*.h` to the new release's actual class layout (reverse
   engineering). Step 1 alone produces a kext that compiles and panics.

See `include/Airport/AGENTS.md` for how to recover a release's true layout.

## Repository Layout

- `itlwm/` — the `itlwm.kext` driver and the Intel hardware HALs (`hal_iwm`, `hal_iwn`,
  `hal_iwx`), plus bundled firmware. Owns `PrivateSPI.pch`.
- `itl80211/` — the OpenBSD `net80211` stack and the compatibility shims that let it
  build in the macOS kernel. Shared by both kexts.
- `include/Airport/` — reconstructed private Apple IO80211/IOSkywalk headers. The
  ABI-critical part of the repo.
- `include/HAL/`, `include/ClientKit/` — the HAL abstraction both kexts drive, and the
  userspace ioctl interface (HeliPort).
- `AirportItlwm/` — the `AirportItlwm.kext` sources and per-release `Info.plist` files.
- `scripts/` — firmware packing, local load/unload helpers, and `scripts/abi/` vtable
  tooling for verifying the reconstructed headers against a shipping macOS release.
- `itlwm.xcodeproj` — all targets. There is one project; never pass `-project`.

## Build Pipeline

The repo does not build from a clean clone. One gitignored dependency must be placed at
the repo root first:

```bash
git clone --depth 1 https://github.com/acidanthera/MacKernelSDK
```

Then build. `FwBinary.cpp` is generated by the `fw_gen` target, which every kext target
depends on, so no separate step is needed.

**Always pass `-target`.** A bare `xcodebuild -configuration Release` reports
`** BUILD SUCCEEDED **` while silently skipping targets — it has left `AirportItlwm-Tahoe`
untouched, so the kext on disk still contained code that had just been deleted from the
source. Never treat that command's success as evidence a target was rebuilt; check the
binary's mtime against the source, or build each target by name:

```bash
for t in itlwm "AirportItlwm-High Sierra" "AirportItlwm-Mojave" "AirportItlwm-Catalina" \
         "AirportItlwm-Big Sur" "AirportItlwm-Monterey" "AirportItlwm-Ventura" \
         "AirportItlwm-Sonoma14.0" "AirportItlwm-Sonoma14.4" "AirportItlwm-Tahoe"; do
    xcodebuild -jobs 8 -target "$t" -configuration Release build \
        | grep -qE "BUILD SUCCEEDED" && echo "$t OK" || echo "$t FAIL"
done
```

Single target, one configuration:

```bash
xcodebuild -jobs 8 -target "AirportItlwm-Tahoe" -configuration Release build
```

This project does **not** use Lilu. Do not add Lilu headers, `plugin_start.cpp`, or a
Lilu `OSBundleLibraries` entry.

## Verification

There is no test suite. A change is verified by building both configurations and checking
the artifacts:

```bash
lipo -info build/Release/<Target>/AirportItlwm.kext/Contents/MacOS/AirportItlwm   # expect x86_64
/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" \
    build/Release/<Target>/AirportItlwm.kext/Contents/Info.plist
```

**Build clean after changing a struct shared between the stack and a HAL**, above all
`struct ieee80211com`. It is the first member of every `*_softc`, so its size sets every HAL field's
offset, and an incremental build that reuses one stale object file produces a kext that links,
loads, and then corrupts memory. The failure surfaces far from the cause — a general-protection
fault deep inside net80211 on a pointer made of instruction bytes — and reads as a bug in the
vendored stack. `ItlwmIcSizeNet` and `ItlwmIcSizeHal` in `ioreg` are each translation unit's view
of that struct and must be equal; see `itl80211/AGENTS.md`.

A successful build proves nothing about ABI correctness — the compiler cannot see Apple's
real class layout. For any change under `include/Airport/`, verification means diffing the
generated vtable against the shipping kernel; see `include/Airport/AGENTS.md`.

Booting the result is the user's decision, not the agent's — never install a kext into an
EFI partition without explicit permission.

## Deployment

The user runs OpenCore. Kexts are injected before the kernel boots, so a bad vtable panics
the machine rather than failing gracefully, and the panic often lands before anything can
be logged. Treat every ABI change as unrecoverable-by-default and prefer shipping nothing
over shipping a kext whose layout has not been verified against the target release.

To deploy: mount the EFI partition, copy the kext to `EFI/OC/Kexts/`, and add it under
`Kernel -> Add` in `config.plist` with `Enabled` set.

## Surviving a macOS update

**A minor update changes no preprocessor condition in this repo.** The only Tahoe constant is
`__MAC_26_0`, so 26.x → 26.y keeps building the same `AirportItlwm-Tahoe` target with the same
sources. Nothing about an update can be caught at compile time; every risk lands at load or run
time. And Apple has moved this ABI **inside a point release** before — the separate
`AirportItlwm-Sonoma14.0` / `AirportItlwm-Sonoma14.4` targets exist for exactly that, and the
whole difference is two `#if __IO80211_TARGET >= __MAC_14_4` blocks around inserted virtuals.
If a Tahoe point release shifts a slot, that is the template: a new target, a `__MAC_26_y`
fallback in `itlwm/PrivateSPI.pch`, and `#if` guards around the affected declarations only.

**There is no verification window.** OpenCore injects the kext before the kernel boots, so the
first boot of the new OS is also the first boot of an unverified kext, and a shifted vtable
panics before anything can log. The whole procedure below exists to manufacture that window.

### Before updating

1. `scripts/abi/snapshot.sh > scripts/abi/abi-<ver>-<build>.txt` and copy
   `/System/Library/KernelCollections/BootKernelExtensions.kc` into `scripts/abi/kc/`, with a
   tracked `.sha256`. Both are replaced wholesale by the update and exist nowhere else.
   Validated: `snapshot.sh` run against an archived collection reproduces the tracked baseline
   exactly apart from its two header lines, so the archive genuinely carries the layout.
2. Keep the last known-good `AirportItlwm.kext` on the EFI. It is the only recovery path — the
   staging boot-args are gone. `scripts/kextuuid.py --expect` says which one booted.
3. **Set `Enabled = false` for AirportItlwm under `Kernel -> Add`.** Wi-Fi is down for one boot;
   that buys the window the rest of this procedure needs.

### After updating, before re-enabling the kext

4. Snapshot and archive the new release exactly as in step 1.
4a. **Compare the two collections segment by segment first — it can answer steps 5–8 outright.**
   The archived `.kc` files are static images, so a plain byte comparison is meaningful. If
   `__TEXT_EXEC` and `__DATA_CONST` are byte-identical, then no code, no vtable, no const FSM
   descriptor and no const string changed, and **every** raw offset, flag bit, message number and
   ioctl number in the table below is unchanged *by construction* rather than by sampling. The
   kernel itself is inside this collection (`_ifnet_attach` resolves `T` there), so the BSD `ifnet`
   offsets are covered too.

   ```sh
   scripts/abi/.venv/bin/python3 - <<'PY'
   import sys; sys.path.insert(0,'scripts/abi')
   from vtdump import MachO
   a=open('scripts/abi/kc/BootKernelExtensions-<old>.kc','rb').read()
   b=open('scripts/abi/kc/BootKernelExtensions-<new>.kc','rb').read()
   for vm,vs,fo,fs,nm in sorted(MachO(b).segments, key=lambda s:s[2]):
       if fs: print("%-16s %s" % (nm, "IDENTICAL" if a[fo:fo+fs]==b[fo:fo+fs] else "*** DIFFERS ***"))
   PY
   ```

   `__TEXT` (the mach header page, carrying the UUID) and `__PRELINK_INFO` differ on any rebuild
   and mean nothing on their own — diff the prelink plist to confirm it is only build-root paths
   and cdhashes. A difference in `__TEXT_EXEC`, `__DATA_CONST` or `__DATA` means the full
   procedure applies; run steps 5–8 in full and treat the offset table as unverified.
5. `diff scripts/abi/abi-<old>.txt scripts/abi/abi-<new>.txt` — the go/no-go for vtable layout.
   Slot lines for any class in `include/Airport/` are fatal; instance-size changes are fatal;
   classes appearing or disappearing need reading before anything else.
6. Re-resolve every external symbol. A miss here is a load failure, not a panic, so it is the
   *benign* failure mode — but it is silent from the user's side and worth knowing first:

   ```sh
   K=build/Release/Tahoe/AirportItlwm.kext/Contents/MacOS/AirportItlwm
   KC=scripts/abi/kc/BootKernelExtensions-<new>.kc
   comm -23 <(nm -u "$K" | sed 's/^ *//' | sort -u) \
            <(nm -g "$KC" | awk '$2 ~ /^[TDSBRC]$/ {print $3}' | sort -u)
   ```

   Must print nothing. Baseline on 26.6: 949 undefined, 0 missing.
7. `mapdrv.py` and `callcheck.sh` against the new collection, per
   `include/Airport/AGENTS.md`. Baselines: 212 + 45 correct / 0 wrong, and 94 slots / 0 wrong.
8. Re-verify the raw offsets below. **This is the step a clean vtable diff does not cover**, and
   the one whose failures are silent in both directions.
9. Re-enable the kext, boot, and read the counter order in mechanism 1 ("What to read after any
   change to this path").

### What is pinned to a specific release, by failure mode

Ordered by how hard the failure is to notice, worst last.

| pinned thing | where | fails as |
| --- | --- | --- |
| vtable slot order and count | all of `include/Airport/*.h` | panic at boot |
| 949 external symbols | link surface; the Skywalk factories and the fault-reporter chain are the newest and least proven | kext does not load |
| `RegistrationInfo` +0x0c subfamily, +0x38 BSD unit, +0x4c MTU, +0x108 MAC | `AirportItlwmSkywalkInterface.cpp` | **silent**: wired interface, colliding unit, zero MAC |
| `IO80211SkywalkInterface` state +0x120, event source +0xa8 | `AirportItlwmSkywalkInterface.cpp` | **silent**: the event-pipe guards misjudge and refuse or fault |
| payload structs — `AssocCandidates` 0x6f8, `JoinCompleteEvents` 211/213/216, `LqmEventData` 0x1dc, `BeaconMetaData`, `ExtendedBssInfo` 0x214, `CCPipeOptions` | `include/Airport/*.h` | **silent**: consumers check length exactly and drop a mismatch without a word |
| WCL message numbers 211/212/213/214/216/237 and ioctls 425/433/446/454/460/502, slot 602 | `apple80211_var.h`, `AirportItlwmSkywalkInterface` | **silent**: an FSM never leaves its in-progress state |
| `ifnet` +0x228/+0x22c | `AirportItlwmEthernetInterface.cpp` | **safe** — guarded by a family-word check that skips the write if the layout moved |
| `ifnet` +0x280 | `AirportItlwmV2.cpp` | inert unless `itlifnettrap=1` |

The `_Static_assert`s on the payload structs prove our reconstruction is *self*-consistent. They
say nothing about the new kernel, and cannot: both sides of the comparison are ours.

### Record: 26.6 (25G72) → 26.6.1 (25G76) — no ABI movement

The first run of this procedure. Every check green, and the segment comparison made it cheap:

| check | result |
| --- | --- |
| segment comparison | `__TEXT_EXEC`, `__DATA_CONST`, `__DATA`, `__HIB` and all 600+ `__REGION*` **byte-identical**; only `__TEXT` (header/UUID) and `__PRELINK_INFO` differ — 21 of 16,500 pages |
| `__PRELINK_INFO` delta | 40 build-root path strings + 40 cdhashes for unrelated kexts. No version changed; no `IO80211*`/`IOSkywalk*`/corecapture/`IONetworking*`/`IOPCIFamily` entry appears in it |
| `abi-*.txt` diff | identical apart from the `# system:` header line |
| external symbols | 949 undefined, **0** missing; export surface identical, 93,745 symbols, 0 added, 0 removed |
| `mapdrv.py` | 213 + 45 correct / **0** wrong |
| `callcheck.sh` | 2 classes, 94 slots, **WRONG: 0** |
| `wclfsm.py WCLScanManager` | `num=237`, `SCAN_COMPLETE -> IDLE` |

So the raw-offset table needed no re-derivation: identical `__TEXT_EXEC` and `__DATA_CONST` prove
those bytes did not move. **This is the shape a minor update is expected to have** — a metadata-only
rebuild — and it is worth knowing that the expensive half of the procedure collapses to one
comparison when it holds. It will not always hold; Sonoma 14.4 is the counter-example.

### The tooling lies immediately after an update

`$ITLWM_KC` defaults to the running system, so `kdis.py`, `callers.py`, `findfield.py`,
`slotcall.py` and `wclfsm.py` silently start answering about the **new** kernel the moment it
boots. Export it explicitly when asking about the release a finding was made on:

```sh
export ITLWM_KC=scripts/abi/kc/BootKernelExtensions-26.6-25G72.kc
```

Self-test after switching: `wclfsm.py WCLScanManager` must still show `num=237`. A wrong path
yields empty output, not an error.

## Runtime Debugging

```bash
kextstat | grep -i itlwm
ls -lat /Library/Logs/DiagnosticReports/*.panic
```

`XYLog` has no sink on the target machine, and `itldefer` starts the driver after the GUI has
taken over the console, so neither `log show` nor the verbose screen will show *driver* output.
Diagnostics reach the developer by three routes:

- **boot survives** — integer markers published on the provider, read with
  `ioreg -r -n IOPCIEDeviceWrapper -l -w0 | grep -i itlwm`. Also compare property values
  between the `AirportItlwmSkywalkInterface` and `AirportItlwmEthernetInterface` nodes
  (`ioreg -r -c <class> -l -w0`); a field correct on one and wrong on the other localises a
  bug that no marker was placed for.
- **boot panics** — the panic string, and nothing else. The report is collected on the next
  boot with the kext disabled. Anything that must survive a panic has to be *in the panic
  string*; see `AirportItlwm/AGENTS.md`.
- **the IOKit class census** — `ioreg -l -w0 | grep -o '"IO80211[A-Za-z]*"=[0-9]*'` prints
  `IOKitDiagnostics`' live instance count per class. It answers "does the family actually build
  this object?" in one command, and has already overturned a confident conclusion reached by
  reading disassembly: an EBUSY was pinned on the AWDL/NAN scan gate, and the census showed
  `IO80211AWDLPeerManager = 0` / `IO80211NANPeerManager = 0`, so that gate could not fire. Reach
  for it before tracing a predicate whose inputs depend on objects that may not exist.

- **`skywalkctl`** — `/usr/sbin/skywalkctl`, Apple's own live view of the Skywalk layer, and the
  answer to "is the nexus real and is anything driving it?" without a reboot. Nothing else shows
  the family's side of the data path.

  ```bash
  sudo skywalkctl interface     # per-netif stat block; only NON-ZERO counters are printed
  sudo skywalkctl provider      # net-if / flow-switch provider + instance UUIDs, by name
  sudo skywalkctl channel       # who has a channel open on each nexus instance
  sudo skywalkctl tree          # ring/slot geometry per provider
  ```

  Read `interface` against a *working* interface on the same machine, because the shape is the
  signal: a healthy netif shows `RxPackets`/`RxIRQ`/`RxSYNC`, while `LLinkAdd: 1` and `TxCopyMbuf`
  alone means the logical link registered and traffic was handed to the nexus which then never
  reached the driver. That one line separated "the nexus was never built" from "the queues are
  inert" in a single command, after four mechanisms had been refuted a boot at a time.
  Note the printer omits zero counters, so an *absent* stat means zero, not unsupported.

- **the WCL's own FSM transitions** — the single highest-value channel on Tahoe, and the one to
  reach for first on anything involving scan, join, link or roam. `IO80211Family` logs every
  `CommonFsmManager` transition through CoreCapture, and CoreCapture logs to the unified log under
  `process == "kernel"`, subsystem `corecapture`:

  ```bash
  /usr/bin/log show --last 20m --style compact --predicate 'process == "kernel"' \
      | grep -E 'logTransition|\[wcl\]|IOC DEBUG'
  ```

  Every line reads `FSM <MGR>: in state: <S> got event: <E> moved into: <S'>`, so the FSMs whose
  tables `scripts/abi/wclfsm.py` prints statically can be watched *live* — which manager received
  which event, and whether the state actually moved. `[IOC DEBUG]` lines beside them name every
  ioctl in and out with its result, separated into `EXTERNAL:` (userspace) and `INTERNAL:` (the
  WCL), which is how a `kIOReturnUnsupported` from a stubbed getter is caught at the moment the FSM
  chokes on it. This is a *complete* record of the driver/family conversation and it costs one
  command; it should have been the first thing tried on this bring-up rather than the last.

  **`log` is shadowed by a shell function in this user's zsh** — always spell it `/usr/bin/log`, or
  it fails with `too many arguments` and looks like an empty log.

- **the userspace Wi-Fi stack** — `airportd`, `wifid` and `configd` log to the unified log
  normally, and `IOLog` from the kext reaches it too (unlike `XYLog`):

  ```bash
  log stream --predicate 'process == "airportd" OR process == "wifid" OR process == "configd"'
  log show --last 30m --predicate 'eventMessage CONTAINS[c] "itlwm"'
  ```

  This says nothing about the kext's internals, but everything about whether macOS *accepts*
  the interface — which is a distinct failure domain from firmware bring-up, and the one that
  survives once the driver attaches cleanly. `Apple80211GetIfListCopy` / `_getIfListCopy`
  logging its `ifCount` is the gate on whether a Wi-Fi device exists at all as far as
  `airportd` is concerned.

## Tahoe bring-up: temporary mechanisms

Everything here is deliberately unfinished — either a stopgap standing in for a real mechanism, or
a gap left open on purpose. Each entry says what is in place, what would replace it, and how you
know it is done. Detail lives in the owning doc; this list exists so none of it silently becomes
permanent, and so an unfinished thing is never mistaken for a working one.

**The driver connects, holds a connection, and carries traffic over a real Skywalk data path with a
DHCP lease. That path is now the only one on Tahoe — the staging boot-args are gone.** 15 and 21
are resolved and mechanism 1's data path is measured working, so what is left divides into three
kinds, and the distinction is what should drive priority:

- **Functional gaps on a working connection** — 17 (`networksetup` reports no association),
  18 (no roaming), 20 (no link rate in the UI), 22 (Private Wi-Fi Address = Off fails to join, a
  macOS-side refusal with a working default). These are what a user would notice.
- **Unmeasured, and cheap to settle before it is built** — 23 (a link address assigned mid-session
  never reaches the air). One counter says whether it happens at all; read it before spending
  anything on it.
- **Stopgaps that work but should not be permanent** — 1 (the legacy attach still exists beside the
  Skywalk one, and the RX tee still lives in it; subsumes 10 and 12), 7 and 13 (timers standing in
  for preconditions), 14, 19 (a 1 Hz retry carrying every association), 3, 4, 6, 8, 11.
- **Cleanup owed once the above settle** — 9 (remove all instrumentation), 2 (delete the dead
  event-pipe guards), 16 (finish decoding `apple80211_assoc_candidates`).

Note 16 is *not* cosmetic even though 15 is closed: an unread flag byte in a struct Apple writes is
an unread instruction, and the last one found there was the PSK.

1. **Skywalk registration is faked.** `AirportItlwmSkywalkInterface::prepareBSDInterface` has
   Apple allocate the ethernet `RegistrationInfo` via `copyRegistrationInfo`, and permanently
   lends a kext static for the network one; `free`/`stop`/`deregisterLogicalLink` take the loan
   back. Faking it also means nobody establishes the *caller context* Apple's registration would,
   which is what 12 exists to paper over.

   **This entry used to call it a feature project needing four new ABI classes. That was wrong,
   and the correction is the useful part: real registration needs no new vtable at all.** Every
   object in the chain has an exported *static factory*, and every data-path callback is a plain
   C function pointer — so the driver never subclasses a Skywalk class, never calls a virtual on
   one, and never takes its size. That is the declaration-only header pattern in
   `include/Airport/AGENTS.md`, which carries no ABI obligation and so nothing to re-port per
   release. The old estimate counted vtables that are never involved. **Before sizing a port by
   the classes named in a signature, check whether any of them is ever anything but a pointer.**

   Recovered from the 26.6 collection, and enough to write against:

   - Entry point is **`IO80211InfraInterface::registerInfraEthernetInterface(RegistrationInfo *,
     IOSkywalkPacketQueue **, uint nQueues, pool, pool)`** — non-virtual and exported, so
     declaring it costs no slot. It stamps the MAC from `getSelfMacAddr()` (slot 415) into
     `info+0x108`, then tails into `registerEthernetInterface(..., 0)`. Its only Apple caller is
     `AppleBCMWLANSkywalkInterface::start`.
   - **Transcribe `AppleEthernetRL::startInterface`, not Broadcom's version.** It builds pools,
     all four queues and the registration in one readable function, where
     `AppleBCMWLANSkywalkInterface` spreads the same work across an ivar bundle at `this+0x138`:

     ```text
     pool = IOSkywalkPacketBufferPool::withName(name, owner, 1, &opts)      x2, tx and rx
            opts: +0x00 packets  +0x04 buffers  +0x08 bufsize (0x4000)  +0x0c 1  +0x14 …
            sized from IOSkywalkRx{Submission,Completion}Queue::getEffectiveCapacity(0x100)
     q[]  = IOSkywalk{Tx,Rx}{Submission,Completion}Queue::withPool(
                pool, 0x100, 0, owner, callback, refcon, 0)
            each queue added to the work queue, then the array passed straight to registration
     ```

     `initWithName` validates `mode < 3` and `options != NULL`; that is the literal `1`.
   - The queue array Apple passes is `nTx` TX queues followed by two more, i.e. `nQueues = nTx+2`,
     with `nTx < 7` enforced.

   **Open before coding, and it fails quietly:** `registerInfraEthernetInterface` skips the MAC
   stamp unless `[this+0x128]->byte[0x3c50] & 1` **and** `[this+0x120]->dword[0x58] == 1`.
   Registration proceeds either way, so failing these ships an interface with a **zero MAC** and
   no error anywhere — the `BeaconMetaData` bit-14 shape again. Establish what sets both before
   trusting a successful registration.

   **Scope correction, and it is the one that decides the size of this.** Registration and the
   data path are *not* separable. The ifnet on the Skywalk path is created by
   `IOSkywalkNetworkBSDClient::gatedPrepareNexus`, which prepares a **nexus** — so the interface it
   publishes is backed by the queues, and traffic through it flows through the pool and the four
   queues rather than the legacy path. Registering for real therefore means the Skywalk interface
   owns the ifnet and `AirportItlwmEthernetInterface`'s BSD attach is retired with it, or the
   machine ends up with two network interfaces. **Size this as: no new vtables, but a real TX/RX
   data path.** The ABI risk is low and the driver work is not.
   **And the ABI obligation that used to survive here is now gone entirely.** This once read that
   the callbacks call three virtuals on `IOSkywalkPacket` and one on `IOSkywalkMemorySegment`, so
   both classes needed reconstruction accurate enough for the slot indices to land, checked by
   `callcheck.sh` because `mapdrv.py` covers only slots we override. Every one of those methods
   turned out to be an exported symbol as well as a vtable entry, so they are now direct
   non-virtual calls that the **linker** verifies on every build. `IOSkywalkPacket` declares no
   virtuals at all and is no longer probed. See the rule and its two preconditions in
   `include/Airport/AGENTS.md`.

   **LANDED and booted.** `AirportItlwmSkywalkInterface::start` builds two pools and four queues and
   calls `registerInfraEthernetInterface`. This was staged behind `-itlskywalk`/`-itlskywalkreg`
   while it was unproven; both are now deleted (see "the staging boot-args are gone" below).
   Verified statically, all against 26.6: every one of the 10 new
   external symbols resolves against the collection, `IOSkywalkFamily` was already in
   `OSBundleLibraries`, all ten targets build, `mapdrv.py` 212 correct / 0 wrong, and
   `scripts/abi/callcheck.sh` 82 slots / 0 wrong.
   - `include/Airport/IOSkywalkDataPath.h` — the queue factories and packet types.
   - `IOSkywalkPacketBufferPool.h` — now declaration-only, and `PoolOptions` corrected from `0x20`
     to `0x28` (`+0x18` was declared `uint64_t pad` and is a **pointer**; it would have corrupted
     the first caller's frame).
   - `IO80211InfraInterface::registerInfraEthernetInterface` declared non-virtual, vtable-neutral.

   **The factory chain and registration are PROVEN on 26.6.** Measured across two boots:
   `ItlwmSkywalkStage = 8`, `ItlwmSkywalkRegRet = 0` — both pools, all four queue factories and
   `registerInfraEthernetInterface` succeed, and the two state guards did not refuse. That was the
   open question this staging existed to answer, and it confirms the scope correction above: **no
   new vtable is needed.** What remains is driver work, not reverse engineering.

   **THE SKYWALK DATA PATH CARRIES TRAFFIC. Measured on 26.6 with `-itlskywalkbsd` and a join:**

   ```text
   Stage = 11  RegRet = 0  QueuesAdded = 4  QueuesEnabled = 4
   TxDequeue = 105  TxFrames = 105        RxDequeue = 3  RxFrames = 510  RxFree = 255
   TxListShort = RxListShort = 0          TxComplFail = RxComplFail = 0
   TxDrops = RxDrops = TxNoMbuf = RxOversize = 0
   nexus TxCopyMbuf = 80 against 72 dequeued at the same sample
   ```

   Every frame consumed was delivered, in both directions, with nothing short and nothing refused.
   That is the question this whole mechanism existed to answer.

   **AND THE INTERFACE NOW COMPLETES DHCP.** With mechanism 21 fixed, the next boot came up
   `inet 192.168.87.149`, default route via `en3`, `ItlwmLlAddrCalls = Synced = 2`, `Late = 0`,
   `TxDequeue = 187 / TxFrames = 189`, `RxFrames = 245`, every drop and failure counter still 0.
   **A nexus-backed Skywalk interface, owning the BSD ifnet, carrying real traffic on a leased
   address.** The remaining work in this entry is cleanup (1's stopgaps, 10, 12) rather than
   bring-up.

   **One real bug found by reading `RxFree` on that boot, and it is the kind that only shows up
   under load: the RX free list was refilled only AFTER it emptied.** RX is driver-pulled — nothing
   tops the list up unless the driver asks — so waiting for zero guarantees that the frame which
   discovers the shortage is dropped, once per drain cycle, forever. The session had `RxFree = 10`
   left of one 255-buffer prime with `RxDequeue = 1`: ten frames from the first drop, and no boot so
   far had run long enough to reach it. `skywalkRxInput` now refills at a low-water mark
   (`ITLWM_SKYWALK_RXLOWAT`, a quarter of the ring) with an `fRxRefillPending` flag cleared when
   buffers actually land, so it asks once per drain rather than once per frame. The empty-list path
   is kept as a backstop and still counts `RxNoBuf`.
   **Rule: a counter that is merely low is a prediction.** `RxFree` was not zero and nothing had
   failed yet; the drop was arithmetic, not a symptom, and would have arrived on the first long
   session as unexplained packet loss.
   - `txSubmissionDequeue` copies each frame into an mbuf, enqueues it on `ifp->if_snd`, returns the
     packets through `IOSkywalkTxCompletionQueue::enqueuePackets` and kicks `if_start`.
   - `rxSubmissionDequeue` takes the empty buffers the stack lends and parks them on a driver free
     list; `AirportItlwmEthernetInterface::inputPacket` tees each received frame into one and
     delivers it with `IOSkywalkRxCompletionQueue::enqueuePackets`.
   - `txCompletionEnqueue`/`rxCompletionEnqueue` are still inert stubs. They are *notification*
     callbacks, not the delivery path, so returning 0 costs nothing today — but that is an
     assumption nobody has tested, and it is the first thing to revisit if completions misbehave.
   Verified statically: all ten targets build, `mapdrv.py` 213 + 45 correct / 0 wrong,
   `callcheck.sh` 94 slots / 0 wrong, and all 949 undefined symbols resolve against 26.6.

   **The RX tee lives in the legacy interface, which is the thing scheduled for deletion.** It sits
   in `AirportItlwmEthernetInterface::inputPacket` because that is where a finished ethernet frame
   surfaces today. It returns false — falling back to the BSD path — for *every* reason it cannot
   deliver, including a momentarily starved RX ring, because while both interfaces exist a frame
   delivered twice slowly beats a frame dropped. **When that BSD attach is retired the tee must move
   with it, and the false return must become a drop**; leaving it as a fallback once there is
   nothing to fall back to would silently discard frames.

   **The TX contract, recovered from `IOSkywalkTxSubmissionQueue::listDequeue` on 26.6.** None of
   it follows from the callback signature and three parts contradict it:
   - `packets` is **not** an array of `count` pointers. It is `&queue[0x108]` — the address of the
     head of a singly-linked list chained with `setNextPacket`. Only `packets[0]` is valid; the
     rest come from `getNextPacket()`. Reading `packets[1]` reads the list *tail*.
   - the return value is the number of packets **consumed**; `listDequeue` subtracts it and calls
     back until the pending count reaches zero or the driver returns 0.
   - returning **0 is not "nothing to do", it is a refusal.** `listDequeue` sets a stall flag,
     timestamps it, and keeps the list for a later retry. That is precisely what the inert stub
     did, and it is the mechanism behind the lost networking: every frame queued and nothing drained.
   The callback runs with the submission queue's gate closed (`packetSubmissionForKPipe` wraps it
   in `IOEventSource::closeGate`), so it must not block. `enqueuePackets` closes the *completion*
   queue's gate, so the path takes submission → completion; nothing observed takes them the other
   way, but that is not proven. If it ever deadlocks, defer completion to the watchdog.
   **Rule: a callback's array-and-count signature does not mean it is an array.** Read the caller
   before indexing anything a family hands you — this one is a list head, and the second element
   is a different field entirely.

   **RX has the same three-part contract and the opposite meaning**, re-derived from
   `IOSkywalkRxSubmissionQueue::listDequeue` rather than assumed symmetrical: `packets` is
   `&queue[0xe8]`, `count` is `queue[0x120]`, the return is the consumed count, and 0 is a refusal.
   What differs is what a packet *is*. On RX they are **empty buffers the stack is lending the
   driver to fill**, so consuming one is an acquisition, not a delivery; the frame travels back up
   later through the RX *completion* queue. The stub returning 0 therefore meant the driver never
   obtained a buffer to receive into — a different failure from the TX stall, reached through an
   identical-looking piece of code.
   **Rule: two callbacks with the same shape can still mean opposite things.** Establish the
   direction of ownership before reusing a contract, not just the shape of the arguments.
   `setDataLength` works only on a single-buffer packet — it forwards to the buffer's setter only
   when `packet[0x64] == 1` — which is why both pools are built with `maxBuffersPerPacket = 1`.

   **AND THE CONTRACT ABOVE IS ONE OF TWO. THE DRIVER CHOOSES WHICH, AND CHOOSING WRONG PANICS THE
   MACHINE ON THE FIRST FRAME.** Both submission queue classes ship two dequeue implementations:

   | implementation | what `packets` is |
   | -------------- | ----------------- |
   | `legacyDequeue` | a **real array** of `count` packet pointers, read straight out of the ring slot table. The packets' next-pointers are stale ring state and mean nothing. |
   | `listDequeue` | the **address of the head pointer** of a `setNextPacket`-chained list — the contract documented above. |

   **THE SELECTOR IS THE `const` ON THE HANDLER'S `packets` ARGUMENT, NOT A CONSTANT THE DRIVER
   PASSES.** Every submission-queue factory ships **two overloads that differ only in that
   qualifier**, and the non-const one is a six-instruction shim that ORs the mode bit into `options`
   and tails into the const one:

   ```text
   withPool(..., IOSkywalkPacket *const *, ...)   -> legacyDequeue   (array)
   withPool(..., IOSkywalkPacket **,       ...)   -> or <bit>; tail  -> listDequeue (list)
   ```

   So declare the overload whose contract the handler implements, pass **0** for `options`, and let
   the shim supply the bit. Doing it by hand is how this cost two panics, because the bit is a
   **different value per queue** and neither is guessable:

   | queue | list bit in `options` | stored as |
   | ----- | --------------------- | --------- |
   | `IOSkywalkTxSubmissionQueue` | **8** | bit 1 of `[q+0x142]` |
   | `IOSkywalkRxSubmissionQueue` | **2** | bit 20 of `[q+0x12c]` |

   TX's `options & 2` is a *different feature* — a notification-mode bit (bit 2 of `[q+0x142]`) read by
   `enable()` and `checkForWork`, which suppresses `enable()`'s initial `packetSubmission(false)`.
   TX also validates `options <= 0x1f` and capacity `>= 8`.

   Both panics were the same mistake seen from two sides. First the driver passed 0 to both and
   implemented the list contract, so RX followed garbage after the first element: `Kernel trap ...
   IOSkywalkPacketBuffer::getMemorySegment + 0x4`, `CR2 = 0x58`, `RDI = 0`. Then it passed 2 to
   both — which is list mode on RX and the unrelated notification bit on TX, leaving TX on
   `legacyDequeue` — and the *first frame transmitted* trapped in
   `IOSkywalkTxCompletionQueue::enqueuePackets + 0x9d`, `CR2 = 0x140`: a virtual call on a packet
   whose vtable pointer read 0, because the handler had walked a ring array with `getNextPacket()`.
   The fix that ends this class of bug is to stop naming the bit: both submission queues now bind
   the non-const overload and pass `options = 0`.

   **The tell was in our own header and was inverted into a rule.** RX's `DequeueHandler` had been
   declared `IOSkywalkPacket **` and TX's `IOSkywalkPacket *const *` — so RX was already binding the
   list-mode shim and TX the legacy one, and the explicit `2` was redundant on RX and wrong on TX.
   That asymmetry *is* the entire bug, and it was written down as a rule to preserve: an earlier
   entry here recorded that "correcting" TX's `const` broke the link and concluded **"keep Apple's
   declaration exactly and `const_cast` at the call site."** The link error was the API refusing a
   contract mismatch, and it was read as a naming quirk.
   **Rule: when a reconstructed signature refuses to change, ask whether the other spelling names a
   DIFFERENT function before declaring the first one canonical.** `nm | c++filt | sort` over the
   class shows every overload in one command; two of the three TX `withPool`s were sitting there
   unexamined for the whole bring-up.
   **Corollary, and it is the general form: a `const` in a reconstructed signature can be
   load-bearing semantics, not decoration.** Here it selects between two incompatible meanings of a
   different argument. `const_cast`ing it away compiles, links, and panics.

   **Apple's own drivers take the const/legacy overload with `options = 0` for all four queues** —
   `AppleEthernetRL::startInterface` and `AppleConvergedIPCSkywalkInterface::start`; the list
   overloads have **no in-kernel caller at all**. Both modes are fully implemented, so this is a
   choice rather than a supported-vs-unsupported split, but note we are the only user of the list
   path on this machine. If list mode ever misbehaves in a way that resists diagnosis, switching
   both handlers to the array contract puts the driver on the code path Apple actually ships.

   **Rule: an `options`/`flags` argument passed as 0 is a choice, not a default.** Every bit of a
   flags word a family hands a factory has to be decoded before it is left clear.
   **Rule: two queues in one family can encode the same concept with different bit values.** Nothing
   warns you; the second one is simply a different feature.

   **Second bug found with it, latent in both handlers: `listDequeue` never advances the head.** It
   re-passes the *same* slot address on every iteration and only clears it once the pending count
   reaches zero, so a partial consume must store the remaining head back into `*packets`. Both
   handlers now do. It stayed invisible because they always drained the whole list; `TxListShort`
   and `RxListShort` are the counters that say they did not.
   **Rule: a callback's parameter types are part of the factory's symbol.** A constness "fix" in a
   reconstructed signature is an ABI change.

   **THE STAGING BOOT-ARGS ARE GONE. The Skywalk path is unconditional on Tahoe.** `-itlskywalk`,
   `-itlskywalkreg` and `-itlskywalkbsd` are all deleted, along with the legacy no-flag path; there
   is no runtime alternative and no `itlwmSkywalkOwnsBSD()`. Everything is inside the existing
   `#if __IO80211_TARGET >= __MAC_26_0`, so no earlier release is affected.

   Retired because the path is measured working end to end — registration, the legacy-ethernet
   bridge, SC visibility, association, a live TX/RX data path and a DHCP lease — and **an alternate
   path nothing exercises is not a failsafe, it is untested code that will have rotted by the time
   anyone reaches for it.** It also made every change here get reasoned about twice.

   **The recovery path for a bad boot is now the previous kext, not a boot-arg.** Keep the last
   known-good `AirportItlwm.kext` on the EFI beside the new one; `scripts/kextuuid.py --expect`
   says which booted. If a future change needs staging, give *that* change its own boot-arg and
   delete it when it lands.

   **Rule: when a bring-up switch's two halves differ in blast radius, give them separate
   boot-args** — while it is a bring-up switch. One flag whose meaning grows as the feature lands is
   a trap for the next boot. The split here did its job (registration was measured harmless
   separately from owning the ifnet) and was then removed rather than left as decoration.
   **Corollary: retire the flags when the feature lands.** A permanent opt-in is a permanent second
   configuration to test, and the one nobody boots is the one that breaks.

   **REFUTED: "registration with stub callbacks is destructive" was never true, and the evidence
   against it was on the page when it was written.** That claim said TX was being submitted to a
   nexus-backed queue whose handler returned 0 forever. It cannot have been: **`ItlwmSkywalkTxDequeue`
   and `RxDequeue` read 0 on every boot that has ever registered**, including both boots that lost
   networking. The callbacks were never called even once, so what they returned could not matter.
   The counter that refutes it sits three lines above `Stage = 8` in the same `ioreg` output that
   was used to claim it.
   **Rule: when a mechanism is proposed for a failure, name the counter that would be non-zero if
   it were true, and read it.** "Registration succeeded" was treated as implying "the data path is
   live"; the counters distinguishing those two were already published and simply not looked at.
   **The cause of the two lost-networking boots is therefore UNKNOWN and still open.** Do not
   re-adopt the queue-stall story. A later boot with `-itlskywalkreg` and a join held a working
   connection — `en3` active with an address, traffic flowing, 0% ping loss — so the failure is not
   a deterministic consequence of registering.

   **What registration actually does today: it builds the objects and stops.** Measured on 26.6
   with `-itlskywalkreg`, via the IOKit class census (`ioreg -l -w0 | grep -oE '"IOSkywalk[A-Za-z]*"=[0-9]+'`):

   ```text
   IOSkywalkQueueSet=1  IOSkywalkLogicalLink=1  Tx/RxSubmissionQueue=1  Tx/RxCompletionQueue=1
   IOSkywalkPacketBufferPool=2            <- the full graph registration is supposed to build
   IOSkywalkNetworkBSDClient=0            <- and the one object that would make it live
   ```

   No BSD client means no netif nexus (the kernel log shows `nx_netif` nexuses for `en0` and `en1`
   only), which means no channel, which means nothing ever drives the submission queues. So
   **`-itlskywalkreg` is currently inert, not destructive**, and the TX/RX code is entirely
   untested — 0 callbacks, `RxFree = 0`.

   **The gate is ours: `AirportItlwmV2.cpp` calls `fNetIf->deferBSDAttach(true)` when
   `getInterfaceRole() == 1`, just before `fNetIf->start()`, and never sets it back.** It is not a
   bug — it is what stops a second ifnet appearing beside the legacy one — which makes clearing it
   the *same edit* as retiring `AirportItlwmEthernetInterface`'s BSD attach, not a step that can be
   taken before it.

   **The whole mechanism, recovered on 26.6, and it is smaller than it sounds:**
   - `IOSkywalkNetworkInterface::deferBSDAttach(bool)` does nothing but set an IOKit property:
     `true` → `setProperty("IODeferBSDAttach", true)`, `false` → `removeProperty("IODeferBSDAttach")`.
     No state, no notification, no side effect.
   - A scan of the *entire* boot collection for references to that string finds **exactly three**:
     two inside `deferBSDAttach` itself, and one reader — `IOSkywalkNetworkBSDClient::start`.
   - That reader fetches the property from its provider and, if present and equal to the global
     boolean singleton, returns false. So the client *is* matched and constructed and then thrown
     away, which is why the census reads `IOSkywalkNetworkBSDClient = 0` rather than showing an
     object in a stalled state. (The compared singleton is `kOSBooleanTrue` by every indication but
     cannot be named from the on-disk collection — it is a chained-fixup bind. It does not matter:
     `deferBSDAttach(false)` *removes* the property, which takes the unambiguous absent-→-proceed
     branch.)

   **The operational consequence, and it rules out the obvious approach: the property is read
   exactly once, in `start()`, with no notification registered and no retry.** Calling
   `deferBSDAttach(false)` later therefore does *nothing* on its own — nothing re-reads it. The
   defer must be gone before the BSD client matches, or the interface must be re-registered
   afterwards to force re-matching.
   **Rule: before planning to "flip it later", find out whether anything re-reads it.** A property
   consulted once at `start()` is a construction-time argument wearing a mutable interface.

   **Housekeeping owed on the EFI:** `-itlskywalk`, `-itlskywalkreg` and `-itlskywalkbsd` no longer
   do anything and should be removed from `config.plist`. They are inert, not harmful — an
   unrecognised boot-arg is ignored — but a leftover arg that reads like a mode switch and is not
   one is exactly the trap the split was meant to avoid.

   **Rule: a counter that is set but not published is not instrumentation, and it fails silently in
   the one direction that matters** — the boot looks fine and answers nothing, which is
   indistinguishable from the feature not running. When adding a counter, add the `setProperty`
   in the same edit.
   **`IOLog` from this kext does NOT reach the unified log — measured, and the opposite of what
   this entry used to claim.** The reasoning was that `itldefer` puts `start()` long after the GUI,
   so an `IOLog` would be readable live without a reboot cycle. On the `-itlskywalkreg` boot the
   `itlskywalk: register ret=… stage=…` line is absent from `log show` entirely, under any
   predicate, while the ioreg counters for the same code path read correctly. Grepping the unified
   log for `itlskywalk` matches only the *boot-args* line, which contains the string and is easily
   misread as the driver's own output. **The ioreg counters are the reliable channel here; treat a
   log line as a bonus and never as the only record of something.**
   Note Apple's own `registerInfraEthernetInterface` logs `Override mac address for infra interface`
   **only** when it takes the MAC-stamp branch. Absence of that line next to ours therefore means
   the two state guards refused, not that registration never ran — do not read it as the latter.

   *Next steps, in order.* The first boot with `-itlskywalkreg` has now happened and is described
   above, so the open question is no longer "does registration work" but "what makes the data path
   live":
   1. **Done — `IOSkywalkNetworkBSDClient::start`'s preconditions are enumerated and two of the
      three checkable ones already pass on the running machine.** In order, it requires:

      | # | precondition                                                                      | status on 26.6                                  |
      | - | --------------------------------------------------------------------------------- | ----------------------------------------------- |
      | 1 | `IOSkywalkNetworkNexusDomainProvider::registerNexusDomainProvider()` returns true | unknown                                         |
      | 2 | provider casts to `IOSkywalkNetworkInterface`                                     | passes                                          |
      | 3 | **`IODeferBSDAttach` absent on the provider**                                     | **FAILS — this is the blocker, and it is ours** |
      | 4 | `IOInterfaceUnit` present as an OSNumber; its `uint32` is read and kept           | passes, value **0**                             |
      | 5 | `mExpansionData->fRegistrationInfo` (`[iface+0xc0]` then `[0]`) non-NULL          | passes — `AirportItlwmV2` sets it               |
      | 6 | two further provider virtuals, `[vt+0x9b8]` and others past that point            | unknown                                         |

      3, 4 and 5 are all readable without a reboot, which is how 4 and 5 were confirmed:
      `ioreg -r -c AirportItlwmSkywalkInterface -l -w0` shows `IODeferBSDAttach = Yes` and
      `IOInterfaceUnit = 0`.

      **Hazard found while checking 4, now fully attributed, and it must be fixed before the defer
      comes out.** The Skywalk node reports `IOInterfaceUnit = 0` while the legacy node reports
      **3** (`BSD Name = en3`) — and the Skywalk node's own `IOInterfaceName` still reads **"en3"**.
      Those two properties are written by *adjacent lines* of
      `AirportItlwmEthernetInterface::attachToDataLinkLayer`, both from `ifnet_unit(getIfnet())`,
      so the unit was unambiguously 3 when they ran. Something overwrote one of them afterwards.

      That something is registration itself. `IOSkywalkNetworkInterface::registerNetworkInterface`
      (the logical-link overload our path ends in) calls slot 317 `getBSDNamePrefix()` and slot 318
      **`getBSDUnitNumber()`** on the interface and stamps both into the registry as
      `IOInterfaceNamePrefix` and `IOInterfaceUnit`. Our Skywalk interface has never been assigned
      a BSD unit, so `getBSDUnitNumber()` yields **0**, and registration publishes that over the
      correct value — leaving `IOInterfaceName = "en3"` beside `IOInterfaceUnit = 0` as the visible
      tell.

      **Why it matters, and this is proven rather than inferred.** Gate 4 above is
      `IOSkywalkNetworkBSDClient::start` reading that same property, and what it does with the
      value is build the interface's name out of it:

      ```c
      prefix = iface->getBSDNamePrefix();        // slot 317 -> "en"
      snprintf(buf, 16, "%s%u", prefix, unit);   // unit = the IOInterfaceUnit just read
      iface->setBSDName(buf);                    // slot 288
      ```

      The unit is **absolute** — formatted straight into the name, with no search, no allocation
      and no base-plus-offset. So with the defer removed the Skywalk interface would attach as
      literally **`en0`**, which already exists.
      **This is the Skywalk path only, and the distinction matters** because the machine's existing
      `en0`–`en3` look like counter-evidence and are not: those are `IONetworkInterface`s, where
      `IONetworkStack`/`nameBSDInterface` *does* allocate the unit (that is how this driver's own
      legacy `en3` got 3, via `IONetworkInterface::setUnitNumber`). Nothing on this machine has ever
      attached through `IOSkywalkNetworkBSDClient`, so those interfaces say nothing about it.
      **Rule: "the system obviously assigns these" is a claim about one code path.** Two families
      publish the same `IOInterfaceUnit` property and only one of them allocates it.
      **Removing the defer is therefore not sufficient and not safe on its own**: either
      `getBSDUnitNumber()` must return the right unit before registration, or `IOInterfaceUnit` must
      be corrected after it. A unit collision presents as a broken or renamed interface, not as an
      error.
      **Rule: when two properties written from the same expression disagree, the later writer is
      the story.** The disagreement is the evidence — `IOInterfaceName` was the witness that fixed
      the value at 3 and turned "which is right?" into "who wrote it last?".

      **Where the 0 is stored, and why it never becomes anything else.** `getBSDUnitNumber()` is
      not computed — it returns `mExpansionData->fRegistrationInfo->[0x38]`, a plain `s32` in the
      `RegistrationInfo` this driver hands to registration (field map in
      `include/Airport/IOSkywalkEthernetInterface.h`). And `initRegistrationInfo`, the Apple
      initialiser we call to build that struct, **seeds the new struct's +0x38 by calling
      `getBSDUnitNumber()` — i.e. from the currently installed struct's +0x38.** It propagates a
      unit; it never allocates one. Nothing in this driver's path ever seeds a real value, so it is
      0 at every hop, and registration publishes that 0.
      A **negative** unit is the "unassigned" marker: `registerNetworkInterface` skips publishing
      `IOInterfaceUnit` at all when the value is `< 0`. That is not a way out either — gate 4 above
      *requires* the property to exist, so -1 trades a colliding unit for a failed `start`.
      **The fix is therefore to seed `RegistrationInfo + 0x38` with the unit this interface should
      own, before registering** — which is a decision that only makes sense together with retiring
      the legacy attach, because until then unit 3 belongs to `en3`. Do not pick a value now.
      **Rule: a getter that reads a struct the caller supplies is not a source of truth.** Chasing
      "who assigns the unit" through the family was the wrong question; the answer was that nobody
      does, and the value had been sitting in a field we own the whole time.

      **The BSD name prefix is `"en"`, established rather than assumed.** `IOSkywalkEthernetInterface::
      getBSDNamePrefix` (slot 317) calls the base implementation, which returns
      `RegistrationInfo + 0x30`, and falls back to the literal `"en"` if that is NULL — and the
      field is not NULL, because `IOSkywalkEthernetInterface::initRegistrationInfo`, the function
      this driver calls to build that struct, is itself one of only two referrers of that same
      `"en"` literal. The whole Wi-Fi chain — `IO80211SkywalkInterface`, `IO80211InfraInterface`,
      `IO80211InfraProtocol` and Apple's own `AppleBCMWLANSkywalkInterface` — binds slot 317 to that
      one implementation, and this driver does not override it.
      **There is no `wlanN` on macOS.** The literal `wlan` appears three times in the whole boot
      collection: twice inside `AppleBCMWLANLogger::initWithDriverAndOptions` (a *logger* name) and
      once in an `IO80211Hexdump` format string. None is an interface prefix. The IO80211 prefix
      table holds `en`, `awdl`, `ap`, `llw` as adjacent literals — those are the real Wi-Fi-family
      prefixes, and the infrastructure interface is `en`.
   2. **LANDED behind `-itlskywalkbsd`, UNBOOTED.** All four parts shipped together, because each
      is unsafe alone:
      - **the defer is not set**, so `IOSkywalkNetworkBSDClient::start` proceeds. It is read once at
        `start()`, so it had to be absent before matching rather than cleared after.
      - **a free BSD unit is seeded** into `RegistrationInfo + 0x38` before registration.
        `itlwmPickFreeBSDUnit` walks `en0..en15` with `ifnet_find_by_name` and takes the first name
        nothing owns — mirroring what `IONetworkStack` does for the legacy path, rather than
        hardcoding a number correct on one machine. Registration is refused outright if none is
        free, because attaching as `en0` is worse than not attaching.
      - **`AirportItlwmEthernetInterface` is attached but not registered**
        (`attachInterface(..., false)`), so no second ifnet is published. The object stays alive
        deliberately: the HAL, the ioctl paths and the RX tee all still run through it. Retiring the
        class outright is a later step.
      - **the RX tee's fallback becomes a drop.** RX still reaches it — `_if_input` calls
        `iface->inputPacket()` directly from the driver, not from BSD, so an unregistered interface
        still receives — but there is no longer anything to fall back *to*, and handing the frame to
        `super` would queue it into an interface that never flushes anywhere.

      Verified statically: all ten targets build, `mapdrv.py` 212 + 45 correct / 0 wrong,
      `callcheck.sh` 41 slots / 0 wrong, `ifnet_find_by_name`/`ifnet_release` resolve against 26.6.

      **BOOTED ON 26.6 AND IT FAILED: no Wi-Fi interface at all.** `ifconfig -l` showed `en0 en1`
      and nothing else — the legacy `en3` correctly gone, and no Skywalk interface to replace it.
      Measured in that state:

      ```text
      IOInterfaceUnit = 2          <- the seed WORKS; itlwmPickFreeBSDUnit picked the free unit
      IOInterfaceNamePrefix = "en" IODeferBSDAttach absent   <- both intended, both correct
      IOMACAddress = <000000000000>                          <- ZERO
      IOSkywalkNetworkBSDClient = 0                          <- still refuses to start
      only 6 Itlwm* properties on the provider, all set by IOPCIEDeviceWrapper before
      AirportItlwmV2::start — no ItlwmSkywalk*, no ItlwmSkyIf*, so publishRuntimeCounters
      never ran, so the watchdog never ran
      ```

      **The mistake was scoping, not mechanics.** Every part of the change did what it was designed
      to do — the unit seeding in particular is confirmed working. What was wrong is the assumption
      that the legacy BSD registration only supplied *an ifnet*. It also supplied, through
      `AirportItlwmEthernetInterface::attachToDataLinkLayer`, the whole identity copy onto the
      Skywalk node (`IOMACAddress`, `IOInterfaceName`, the Wi-Fi subfamily) **and the enable
      trigger**: the driver's `enable()` is driven by `IFF_UP`/`SIOCSIFFLAGS` arriving on a
      registered BSD interface. Unregister it and nothing ever enables the adapter — no scan, no
      network list, and no watchdog, which is why even the counters stopped being published.
      So the change removed a *load-bearing* path and replaced only one of its three jobs.
      **Rule: before retiring a path, enumerate what it does besides the thing you are replacing.**
      "Attach the interface" was one of three effects, and the other two had no other producer.
      The zero `IOMACAddress` is the visible tell and is worth keeping: with the copy gone, nothing
      sets it, and `registerInfraEthernetInterface`'s own MAC stamp is gated behind two state checks
      that fail silently — the hazard flagged at the top of this entry, now observed.

      **ROOT CAUSE, found on the failing machine without rebooting it, and it is exact.** The
      Skywalk node's IOKit state line reads **`!registered, !matched`**. So IOKit never ran matching
      on it, `IOSkywalkNetworkBSDClient` was never *considered*, and every precondition inside its
      `start()` was irrelevant because `start()` was never called.
      The only code in this driver that ever published that interface for matching is
      `interface->registerService()` at `AirportItlwmEthernetInterface.cpp:152` — inside
      `attachToDataLinkLayer`, i.e. inside the path this change disabled. **Apple's registration
      does not do it for you:** `registerNetworkInterface` only sets properties (the call that looks
      like a candidate, slot 80, is `IORegistryEntry::setProperty`).
      So `attachToDataLinkLayer` had **four** jobs, not one: publish the BSD ifnet, copy the
      identity onto the Skywalk node, call `registerService()` on it, and call
      `prepareBSDInterface`. Only the first was being replaced.
      **Rule: `!registered, !matched` on a node you expected a client to attach to means matching
      never ran — stop analysing the client's `start()`.** One `ioreg` state line, readable on the
      broken machine, answered what several rounds of disassembling gate conditions could not.

      **FIXED AND BOOTED — the Skywalk interface now attaches.** `AirportItlwmV2::start` calls
      `fNetIf->registerService()` and copies `IOMACAddress` itself when `skywalkOwnsBSD`. Measured:

      ```text
      AirportItlwmSkywalkInterface  ... registered, matched      <- was !registered, !matched
      BSD Name = "en2"   IOMACAddress = <4c445b8476cd>   IOInterfaceUnit = 2
      IOSkywalkNetworkBSDClient = 1                              <- was 0
      en2 in ifconfig -l, agent domain:Skywalk type:NetIf desc:"Userspace Networking"
      ```

      So gates 1 and 6 pass, the BSD client starts, and the ifnet it publishes is **nexus-backed**.
      That is the whole registration/attach chain working end to end.

      **Two things still wrong on that boot, one now fixed:**
      - `en2` reported **`type: Ethernet`**, so macOS does not treat it as Wi-Fi. Cause is the same
        propagate-never-allocate circularity as the unit: `getInterfaceSubFamily()` returns
        `RegistrationInfo + 0x0c`, and `initRegistrationInfo` seeds `+0x0c` from
        `getInterfaceSubFamily()`, so it stays 0 = WIRED. **Now seeded to 3
        (`IFNET_SUBFAMILY_WIFI`) beside the unit — this is mechanism 10's documented real fix**,
        replacing `forceWiFiSubfamily`'s raw poke at `ifnet+0x22c` with the struct field Apple's own
        drivers use. The family field `+0x08` correctly stays 2 (`IFNET_FAMILY_ETHERNET`);
        `_if_functional_type` wants ETHERNET *and* subfamily WIFI. **UNBOOTED.**
      - the adapter is never **enabled**. This is now the only thing left, and the boot after the
        subfamily fix isolated it precisely.

      **The subfamily fix worked completely, measured on 26.6:**

      ```text
      ifconfig -v en2   type: Wi-Fi   agent domain:WiFiManager type:CallInProgress desc:"WiFi"
      scripts/ifpred.c en2 -> SIOCGIFFUNCTIONALTYPE = 3 (WIFI_INFRA)
                              SIOCGIFMEDIA = 0x80 (IFM_IEEE80211)   first gate: PASSES
      airportd _getIfListCopy: getifaddrs nInterfaces[24], count[  1]      <- was count[0]
      ```

      So macOS adopts the interface as Wi-Fi and `airportd` enumerates it. `forceWiFiSubfamily` and
      its `ITLWM_IFNET_*_OFFSET` constants (mechanism 10) are superseded on this path.

      **What remains: `enable()` is never called, and everything else is downstream of it.**
      Still 6 `Itlwm*` properties, so `publishRuntimeCounters` and the watchdog never ran; every
      `[IOC DEBUG]` line reads **`isDriverAvailable=<0>`** (276 of them), and `networksetup` reports
      `en2 is not a Wi-Fi interface`. `IO80211ControllerMonitor::getDriverAvailable` returns
      `[monitor+0x10]->byte[0xae8] & 1`, set by `setDriverAvailable(msg, 0xf8)` from `msg[8]` — a
      driver message, which this driver never sends because it never enables.
      In the legacy path `enable()` arrives as `IFF_UP`/`SIOCSIFFLAGS` on a *registered*
      `IONetworkInterface`. Under `-itlskywalkbsd` there is no such interface, and nothing brings
      `en2` up. **Do not add an enable message before establishing whether the ordinary trigger
      works** — the cheap test is one command on a machine already in this state:

      ```bash
      sudo ifconfig en2 up      # then re-read the Itlwm* property count
      ```

      **ANSWERED: the enable path is intact; only the trigger was missing.** `sudo ifconfig en2 up`
      took the property count from 6 to 95 and the driver came fully to life:

      ```text
      en2  flags=8863<UP,BROADCAST,SMART,RUNNING,...>
      ItlwmScanBeacons = 671 (climbing)   ItlwmScanReqCalls = 2   ScanReqStarted = 2
      system_profiler SPAirPortDataType -> en2, Card Type: Wi-Fi, fw 68.01d30b0c.0,
                                           Country DE/ETSI, 38 channels
      ```

      So `enable()`, the HAL, the scan path and the WCL all work under `-itlskywalkbsd`. **No driver
      code is missing.** What is missing is a *macOS network service*: `networksetup
      -listnetworkserviceorder` reports `(3) Wi-Fi  (Hardware Port: Wi-Fi, Device: en3)` — bound to
      the interface this mode retires. That is why `networksetup -getairportpower en2` answers
      "not a Wi-Fi interface" while `system_profiler` describes the card perfectly: the former
      resolves through SystemConfiguration services, the latter queries the driver directly.
      Fix on the machine, not in the driver: `sudo networksetup -detectnewhardware`, or add Wi-Fi
      for the new interface in System Settings.
      **Rule: "macOS does not see the interface" is at least three different questions** — does the
      ifnet pass `_getIfListCopy` (`scripts/ifpred.c`), does the driver answer Apple80211
      (`system_profiler SPAirPortDataType`), and is there a SystemConfiguration service bound to
      that BSD name (`networksetup -listnetworkserviceorder`). They fail independently and only the
      first two are the driver's problem.

      **BLOCKED, and it is architectural: SystemConfiguration cannot see a Skywalk-only interface.**
      `networksetup -detectnewhardware` does not surface `en2`; `-listallhardwareports` lists only
      `en0`/`en1`. Diffing our node against a working `en1` shows what is missing, and it is not a
      property:

      ```text
      en1 (works):  IOClass = IONetworkStack   IOMatchCategory = "IONetworkStack"
                    IOBuiltin, IOPrimaryInterface, IOControllerEnabled, IOInterfaceFlags/State
      en2 (ours):   IOClass = IOSkywalkNetworkBSDClient   IOProviderClass = IOSkywalkNetworkInterface
                    BSD Name/IOInterfaceType/Unit/NamePrefix/MACAddress all present and correct
      ```

      SC enumerates **`IONetworkInterface`** subclasses. `AirportItlwmSkywalkInterface` is not one —
      it descends from `IOSkywalkNetworkInterface` — so no set of extra properties will make SC
      classify it as a hardware port. The interface works at every layer below SC (ifnet, ifpred,
      `_getIfListCopy`, Apple80211/`system_profiler`) and is invisible at SC.

      **What supplied that before: the legacy `AirportItlwmEthernetInterface`, which IS an
      `IOEthernetInterface` → `IONetworkInterface`.** Retiring its registration removed the only
      `IONetworkInterface` in the driver, and with it SC's view of the device. That is the *fifth*
      job of `attachToDataLinkLayer`, after the ifnet, the identity copy, `registerService` and
      `prepareBSDInterface`.

      **RESOLVED, and the family decides it with a single property.** `IOSkywalkLegacyEthernet::probe`
      is four instructions of substance:

      ```c
      if (provider->getProperty("IOInterfaceUnit") != NULL) return NULL;   // refuse
      return this;                                                        // match
      ```

      So `IOInterfaceUnit` is a **mutually exclusive fork**, not a data field:

      | `IOInterfaceUnit` on the Skywalk interface | who attaches                                     | consequence                                                                                         |
      | ------------------------------------------ | ------------------------------------------------ | --------------------------------------------------------------------------------------------------- |
      | present                                    | `IOSkywalkNetworkBSDClient` (*requires* it)      | nexus ifnet, **no** `IONetworkInterface`, invisible to SC                                           |
      | absent                                     | `IOSkywalkLegacyEthernet` (*refuses* if present) | `IOSkywalkLegacyEthernetInterface`, an `IOEthernetInterface` -> `IONetworkInterface`, visible to SC |

      and `registerNetworkInterface` publishes the property only when `RegistrationInfo + 0x38 >= 0`.
      **A negative unit is therefore the selector for the legacy-ethernet bridge**, which corrects
      what this entry said earlier: -1 is not "an escape that trades a colliding unit for a failed
      start", it is how the other path is chosen.

      **CONFIRMED ON A GENUINE MAC — this is what Apple's Wi-Fi does.** Read off an Apple Silicon
      Mac running Tahoe 26.6.1 with working Wi-Fi:

      ```text
      +-o en0  <class IOSkywalkLegacyEthernetInterface, registered, matched, ...>
             IOPrimaryInterface = Yes   IOBuiltin = Yes   IOInterfaceType = 6
             IOInterfaceUnit = 0   IOMediaAddressLength = 6   IONetworkData = {...}
      networksetup -listallhardwareports -> Hardware Port: Wi-Fi, Device: en0
      census: IOSkywalkLegacyEthernet = 1  IOSkywalkLegacyEthernetInterface = 1
              IOSkywalkNetworkBSDClient = 4
      ```

      So Apple's Wi-Fi BSD interface **is** an `IOSkywalkLegacyEthernetInterface`, i.e. path B, and
      that is why `networksetup` can configure it. Note its `IOInterfaceUnit = 0` sits on the
      *bridge* (an `IONetworkInterface`, assigned by `IONetworkStack`), not on the Skywalk interface
      that `IOSkywalkLegacyEthernet::probe` inspects — consistent with the fork, not a contradiction.
      The four `IOSkywalkNetworkBSDClient`s on that machine belong to its other interfaces; a count
      alone identifies nothing, which is why the BSD name had to be read.
      **Rule: an instance count answers "does this exist", never "is this the one".** Two rounds of
      this entry were spent inferring ownership from a census before asking which BSD name the
      object actually held.

      **Design B taken (`RegistrationInfo + 0x38 = -1`), UNBOOTED.** A Wi-Fi device macOS cannot
      configure is not useful, and letting `IONetworkStack` allocate the unit is also what lets the
      interface keep the name the legacy path produced instead of orphaning the user's Wi-Fi
      service. `itlwmPickFreeBSDUnit` is still called, for its log line only, so the value to use
      is on record if this ever moves back to the positive branch.

      **Rule: a property a driver sets can be a mode selector read by someone else's `probe`.**
      Nothing about `IOInterfaceUnit`'s name suggests it arbitrates between two whole BSD attach
      architectures. Before setting a property the family also reads, grep for its *other* readers —
      here one of them refuses to match on its mere presence.

      **BOOTED: path B attaches and macOS configures it — but the data path is still not driven.**
      Measured on 26.6 with `-itlskywalkbsd` and a join:

      ```text
      IOSkywalkLegacyEthernet = 1   IOSkywalkLegacyEthernetInterface = 1   <- the bridge attached
      networksetup -listallhardwareports -> Hardware Port: Wi-Fi, Device: en3
      en3 UP/RUNNING/active, and it kept the ORIGINAL name, so the user's Wi-Fi service survived
      ItlwmAssocCalls=1 JoinAssocDone=1 JoinConnectDone=1 LinkIndUp=1 LqmPosts=25 (climbing)
      -- but --
      ItlwmSkywalkRxFree=0  TxDequeue=0  RxDequeue=0  TxFrames=0  RxFrames=0
      ItlwmSkywalkRxNoBuf=103  RxFallbackDrops=103
      en3 inet 169.254.x (DHCP failed), ping over en3 100% loss, default route via en1
      ```

      So the fork, the bridge, SC visibility, naming and the whole association path all work. What
      does not is packet movement: **nothing ever drives the Skywalk submission queues**, and
      because this mode turns the RX tee's fallback into a drop, the 103 frames that did arrive
      were discarded rather than delivered. That is the direct cause of the failed DHCP.

      **`IOSkywalkLegacyEthernet` is a property/identity shim, not a data path** — its entire method
      list is `probe`/`start`/`stop`/`createInterface`/`getHardwareAddress`/`getIfnet`/
      `setInterfaceUnitNumber` plus three `redirect*Property` calls. There is **no** input, output or
      queue method on it. So it supplies the `IONetworkInterface` that IONetworkStack and
      SystemConfiguration need, and traffic is still expected to flow through the Skywalk nexus
      queues. Do not look to the bridge to move packets.

      **The nexus exists and is ours — so the gap is narrower than "the data path is not wired".**
      From the same boot:

      ```text
      nx_netif_prov_nx_ctor  create new netif for nexus (IOSkywalkNetworkNexusDomainProvider)
      nx_netif_na_find       name "netif:0:AirportItlwmSkywalkInterface.en3" ... create 1
      IOSkywalkNetworkBSDClient IOSkywalkNexusUUID = 824F68D8-... (the same nexus)
      ifconfig -v en3: agent domain:Skywalk type:NetIf + type:FlowSwitch, type: Wi-Fi
      census: IOSkywalkPacketQueue = 4  QueueSet = 1  LogicalLink = 1   <- exactly our four queues
      ```

      So the netif nexus, the flowswitch, the queue set and the logical link all exist and hold
      *our* queues, and the callbacks still never fire.

      **REFUTED before it cost a boot: `IO80211SkywalkInterface::enableDatapath()` is a stub.**
      It was the obvious candidate — declared in our headers, never called by this driver — and it
      disassembles to `xor eax, eax; ret`. Nothing else in the family is named for starting a data
      path, so absence of a caller proves nothing about it.
      **Rule: check that a candidate function does anything before building a theory on its name.**

      **STILL DARK AFTER FOUR REFUTED MECHANISMS. Read this list before proposing a fifth.**
      Measured on 26.6, `-itlskywalkbsd`, with a join: `ItlwmSkywalkQueuesAdded = 4`,
      `SkyLinkReportCalls = 1`, `SkyLinkReportRet = 0`, and every data-path counter still 0
      (`RxFree`, `TxDequeue`, `RxDequeue`, `TxFrames`, `RxFrames`), `RxFallbackDrops = 133`.
      What is *known working*: registration, the legacy-ethernet bridge, SC visibility, `en3`
      naming, association, enable, and the family's own `setInterfaceEnable isEnable 1`.

      | candidate | verdict |
      | --------- | ------- |
      | `IO80211SkywalkInterface::enableDatapath()` | **stub** — `xor eax,eax; ret`. Cannot be it. |
      | `IOSkywalkNetworkInterface::reportLinkStatus` | now called, returns **0** (precondition passed, event raised). No effect. |
      | queues never added to a work loop | real bug, fixed, `QueuesAdded = 4`. No effect. |
      | queue array order / `nQueues = nTx + 2` | **refuted**: `IOSkywalkQueueSet::initWithQueues` classifies each queue by querying slot 67 for its type and counts it into one of four buckets. Position is irrelevant; our `[txSub, txCompl, rxSub, rxCompl]` is fine. |

      **THE BINDING CHAIN IS NOW RECOVERED END TO END, statically, on 26.6 — and it corrects the
      central assumption of this whole entry: paths A and B are not a fork, they are a SEQUENCE.**
      `setKernNetifQueue` was the right lead and it leads all the way back to the BSD client:

      ```text
      IOSkywalkPacketQueue::setKernNetifQueue(netif_queue *)      <- binds a queue to a netif ring
        ^ exactly 2 callers, both IOSkywalkFamily statics: nxp_queue_init / nxp_queue_fini
      nxp_queue_init(prov, nx, IOSkywalkQueueSet *, qid, isTx, netif_queue *, out)
        ^ a callback in the kern_nexus_provider_init built by
          IOSkywalkNetworkBSDClient::registerNexusControllerProvider   (+0x38 of that struct)
      nxp_qset_init(prov, nx, IOSkywalkLogicalLink *, idx, ..., netif_qset *, out)  (+0x28)
        ^ driven by the netif's logical links
      kern_nexus_controller_alloc_net_provider_instance(..., net_init)
        ^ net_init +0x30 = interface->llinks[0]->getLogicalLinkNexusInfo()   <- OUR logical link
        ^ SOLE caller in the whole boot collection: IOSkywalkNetworkBSDClient::start+0x653
      ```

      **So `IOSkywalkNetworkBSDClient` is the only thing in the system that creates the netif nexus,
      and therefore the only thing that can ever bind our four queues.** Nothing else calls
      `alloc_net_provider_instance` except loopback, utun and ipsec.

      And the legacy-ethernet bridge is what *produces* the BSD client rather than replacing it:

      1. register with `RegistrationInfo + 0x38 < 0` → `registerNetworkInterface` does not publish
         `IOInterfaceUnit` → `IOSkywalkLegacyEthernet::probe` matches (it refuses when present).
      2. it creates an `IOSkywalkLegacyEthernetInterface`; `IONetworkStack` names it `enN` and
         assigns the unit.
      3. **`IOSkywalkLegacyEthernetInterface::attachToDataLinkLayer` writes that unit onto the
         *Skywalk* interface and re-registers it** — literally
         `iface->setProperty("IOInterfaceUnit", getUnitNumber(), 32); iface->registerService(0);`
         (this is `IOSkywalkLegacyEthernet::setInterfaceUnitNumber`, inlined). It deliberately does
         **not** call super, so the bridge never attaches a BSD ifnet of its own.
      4. re-matching runs: LegacyEthernet's probe now refuses, `IOSkywalkNetworkBSDClient` matches,
         `start()` creates the nexus with the default llink taken from our `IOSkywalkLogicalLink`,
         and `nxp_qset_init`/`nxp_queue_init` bind the queues.

      That is why a genuine Mac shows `IOSkywalkLegacyEthernet = 1` **and**
      `IOSkywalkNetworkBSDClient` non-zero at the same time — not a contradiction, two steps of one
      sequence. Design B was the right choice; what was wrong is the belief that it *ends* there.
      **Rule: when two IOKit personalities look mutually exclusive because one `probe` refuses what
      the other requires, check whether something in between writes the property.** The exclusion
      is real at any instant and says nothing about the order they run in.

      **`IOSkywalkNetworkInterface::addLogicalLink` is a red herring — it has ZERO callers in the
      entire boot collection**, Apple's own drivers included. It sends message `0xE0060105` to the
      BSD client, which calls `kern_nexus_netif_llink_add` for *additional* links; the first one
      arrives through `alloc_net_provider_instance`'s default-llink config instead. Do not build on
      it.

      **MEASURED ON 26.6 — every step of that chain runs.** On an `-itlskywalkbsd` boot with a join:

      ```text
      AirportItlwmSkywalkInterface   IOInterfaceUnit = 3   registered, matched
        +-o IOSkywalkLegacyEthernet  -> en3 (IOSkywalkLegacyEthernetInterface, IOInterfaceUnit = 3)
        +-o IOSkywalkNetworkBSDClient   registered, matched          <- step 4 ran
      census: NetworkBSDClient=1 LegacyEthernet=1 LegacyEthernetInterface=1 QueueSet=1
              LogicalLink=1 PacketQueue=4 Tx/RxSubmissionQueue=1 Tx/RxCompletionQueue=1
              PacketBufferPool=2 NetworkPacket=12544 PacketBuffer=70 MemorySegment=8848
      ```

      So the bridge wrote the unit back, re-matching ran, the BSD client started, the nexus was
      allocated **with our logical link**, and the pools are fully populated — and the data-path
      counters are still 0. The failure is therefore inside `nxp_qset_init`/`nxp_queue_init` or one
      step past them, not anywhere earlier.

      **The netif TX path, recovered so the remaining gap is one link wide:**

      ```text
      netif has TX work
        -> nxp_tx_qset_notify(qset)     walks ((IOSkywalkPacketQueue **)qset[0x20])[i], i < qset[0x60]
        -> queue->packetSubmission(true)              slot 59, Apple's own TxSubmissionQueue impl
        -> refillTxRingForNetworking / getPacketListForNetworking
        -> kern_netif_queue_tx_dequeue               needs the netif_queue nxp_queue_init bound
        -> OUR dequeue callback                       ItlwmSkywalkTxDequeue
      ```

      RX is the mirror: `IOSkywalkRxCompletionQueue::enqueuePackets` → `kern_netif_queue_rx_enqueue`.
      `nxp_queue_tx_push` is the per-queue variant and calls slot 58,
      `packetSubmission(uint64 *, uint *, uint64 *)`.
      So our callbacks *are* in the netif path, reached through Apple's `packetSubmission`, and
      `TxDequeue = 0` means the chain breaks at or before the `netif_queue` binding.

      **ROOT CAUSE FOUND, and it was ours again: every queue is born DISABLED and this driver never
      enabled them.** `IOSkywalkPacketQueue::initWithPool` writes `byte[this + 0x28] = 0` — that
      byte *is* `IOEventSource::enabled` (`isEnabled()` disassembles to `byte[this+0x28] & 1`) —
      immediately after `IOEventSource::init` set it to 1. Nothing in the family ever sets it back.
      Every entry point checks it first and returns silently:

      | entry point | behaviour while disabled |
      | ----------- | ------------------------ |
      | `IOSkywalkTxSubmissionQueue::packetSubmission(bool)` — what the netif's `nxp_tx_qset_notify` calls | `test al,1 / je bail`, returns without doing anything |
      | `IOSkywalkRxSubmissionQueue::requestDequeue` | returns `0xe00002d7` instead of pulling buffers |
      | `IOSkywalk{Tx,Rx}CompletionQueue::enqueuePackets` | returns `0xe00002d7` instead of delivering |

      So the data path assembles perfectly and moves nothing, with no error raised anywhere — the
      exact signature of every measurement above.

      **Apple's own driver names the step: `AppleBCMWLANPCIeSkywalk::enableAllSubmissionQueue()`,
      called from `AppleBCMWLANBusInterfacePCIe::FWSetupDone()`** — i.e. after firmware bring-up,
      not at registration. It enables each TX submission queue, enables the RX submission queue,
      then calls **`requestDequeue`** on it. That last call is not optional and is a second bug of
      the same kind: **RX is driver-pulled.** The stack never pushes buffers at the driver, so
      without it `RxFree` stays 0 forever even with the queues enabled.

      **BOOTED, AND IT PANICKED — the enable was right and exposed a second bug.** With the queues
      finally live the first RX frame trapped in `IOSkywalkPacketBuffer::getMemorySegment` on a NULL
      buffer: the submission queues had been built with `options = 0`, which selects `legacyDequeue`
      (an array) while both handlers implement `listDequeue` (a list). See the two-contract table
      under the TX section above. The head write-back landed with the fix. **That fix was itself
      wrong on TX and panicked again on the first frame *sent*; both are now resolved by binding
      Apple's non-const factory overload instead of naming a bit — see the table above.**

      **BOOTED AND THE RX SUBMISSION PATH WORKS — first time in this bring-up.** Measured on 26.6
      with `-itlskywalkbsd`, no panic:

      ```text
      ItlwmSkywalkStage = 11   RegRet = 0   QueuesAdded = 4   QueuesEnabled = 4
      ItlwmSkywalkRxPrimeCalls = 1   RxPrimeRet = 0   RxDequeue = 1   RxFree = 255
      ItlwmSkywalkRxNoBuf = 0   RxFallbackDrops = 0   TxDequeue = 0   Tx/RxFrames = 0
      ```

      One `requestDequeue` pulled **255 buffers in a single callback** and the list contract walked
      them correctly. So enable, prime and RX list mode are all confirmed. `TxDequeue` and both frame
      counters stay 0 only because that boot never associated — there was no traffic to carry.
      **That "no traffic" is also why the TX half of the mode bug survived this boot**: RX's list
      selection was right by accident of its header declaration, TX's was wrong, and nothing sent a
      frame to find out. A green RX column says nothing about TX; the two queues select the mode with
      different bits and were, at that moment, in different modes.

      **LANDED.** `enableSkywalkQueues()` enables all four after registration and primes
      RX; `skywalkRxInput` re-arms the pull asynchronously whenever the free list runs dry (the
      initial prime can legitimately come up empty if the netif has not bound the queues yet);
      teardown disables before `removeEventSource`. New counters `ItlwmSkywalkQueuesEnabled`
      (must be 4), `ItlwmSkywalkRxPrimeCalls`, `ItlwmSkywalkRxPrimeRet` (`0xe00002d7` = still
      disabled). `ItlwmSkywalkStage` is monotonic again and now reaches **11**: 10 = registered,
      11 = queues enabled and RX primed, so a stop at 10 localises a fault inside the new step.
      Verified statically against 26.6: all ten targets build, `mapdrv.py` 212 + 45 correct / 0
      wrong, `callcheck.sh` **94** slots / 0 wrong (was 41 — a `ProbeQueue` for
      `IOSkywalkPacketQueue` now covers the inherited `IOEventSource` range, self-tested by
      insertion), and all **303** external symbols resolve against the collection.

      **`mapdrv.py` must be pointed at a CONCRETE Apple class, not the abstract base the driver
      declares.** `AirportItlwmSkywalkInterface` derives from `IO80211InfraProtocol` and
      `AirportItlwm` from `IO80211Controller`, but both of those are abstract, so most slots read
      `___cxa_pure_virtual` and the tool reports a wall of false mismatches. The pairings that
      reproduce the 212 / 45 baseline are the classes Apple actually instantiates:

      ```sh
      mapdrv.py <drv>.vt <apple>.vt AirportItlwmSkywalkInterface AppleBCMWLANSkywalkInterface  # 212
      mapdrv.py <drv>.vt <apple>.vt AirportItlwm                 AppleBCMWLANCore              #  45
      ```

      Matching slot *counts* (668 = 668) with mismatched *names* is the signature of this mistake,
      not of a real regression.

      **Rule: a family object handed back by a factory may be constructed in a refusing state.**
      Twice now the same class has needed a driver-side call that no family code makes and no error
      reports: `addEventSource` to give it a work loop, and `enable()` to make it accept work. Both
      were invisible because the object exists, is correctly wired, and answers every structural
      query. When a hand-built object is inert, enumerate the *state* transitions its class exposes
      (`enable`/`disable`/`start`/`initialize`) and check who is supposed to drive each — the
      answer "nobody, so it must be you" is the common case for an exported KPI factory.
      **Corollary: `callers.py` returning zero for an exported, overridden method is a signal, not
      noise.** `IOSkywalkTxSubmissionQueue::enable` having no caller anywhere in the collection was
      readable weeks ago and is what pinpointed this.

      **Separate hazard found while reading the registry, tracked as mechanism 21 and NOT part of
      this chain:** the Skywalk interface publishes `IOMACAddress = <eea614c70955>` —
      locally-administered — while `IOSkywalkLegacyEthernet` below it holds the real
      `<4c445b8476cd>`. Two addresses on one device is the visible tell of the lladdr split
      described in 21. Do not conflate it with the queue problem; it has its own path to a failed
      DHCP and will still be there after the queues move packets.

      **ROOT CAUSE of one real bug, and it was ours: the queues were never added to a work loop.**
      `IOSkywalkPacketQueue` derives from **`IOEventSource`**, not `IOService` — slot 36 is
      `checkForWork`, slot 37 `setWorkLoop`. An event source that belongs to no work loop is never
      polled, so `buildSkywalkDataPath` produced four queues that were handed to registration and
      then sat inert. Everything downstream looked perfect — netif nexus, flowswitch, queue set and
      logical link all present and holding *these* queues — and not one callback could ever fire.
      Fixed: `getWorkQueue()->addEventSource()` on all four between construction and registration,
      with `removeEventSource` in teardown (an event source outliving its last reference on a live
      work loop is a use-after-free). `ItlwmSkywalkQueuesAdded` must read **4**, and
      `ItlwmSkywalkStage` now reaches **9**.

      **This was recoverable from our own notes.** The recipe transcribed from
      `AppleEthernetRL::startInterface` says it outright — "each queue added to the work queue, then
      the array passed straight to registration" — and the implementation skipped the clause.
      `IOSkywalkDataPath.h` also declared the class `: public IOService`, which is what made the
      omission invisible: with the correct base, `addEventSource` is the obvious call and its
      absence is conspicuous.
      **Rule: when a recovered recipe lists steps, diff the implementation against it line by line
      before hunting for a family-side trigger.** Three mechanisms were investigated and two
      refuted (`enableDatapath` is a stub; `reportLinkStatus` returns 0 and changes nothing) at a
      boot apiece, to find a missing clause that was already written down.
      **Corollary: a declaration-only header still has to get the base class right.** Nothing here
      is virtual and nothing is instantiated, so `IOService` vs `IOEventSource` cost no ABI
      correctness — it cost the ability to notice a missing call.

      **LANDED, UNBOOTED: `IOSkywalkNetworkInterface::reportLinkStatus` was never called.** It is
      the only link-state notification into the Skywalk layer: it stores the state under a lock and,
      on an actual change, tail-calls `reportEventType` with `0xE0060102` (up) / `0xE0060100` (down).
      It bails with `0xe00002d8` when `getBSDInterface()` is NULL, so it is inert unless this
      interface owns the ifnet — which under `-itlskywalkbsd` it now does. Called from
      `AirportItlwm::postLinkStatusInd`, the single point where this driver signals link state, and
      gated on `skywalkOwnsBSD` so the working legacy path is untouched. The header declared it
      `void`; it returns an `IOReturn`, and that was the one signal saying the precondition failed,
      so it is now typed and recorded in `ItlwmSkyLinkReportRet` (`ItlwmSkyLinkReportCalls` counts
      calls). **`0xe00002d8` there means `getBSDInterface()` is NULL and this is not the answer.**

      **Still unverified on path B**, and the reason this is unbooted rather than claimed working:
      whether `IOSkywalkLegacyEthernet` actually matches our interface (its `probe` passing is
      necessary, not sufficient), whether the bridge drives the same TX/RX queue callbacks this
      driver implements, and how RX reaches it — `_if_input` delivers to
      `AirportItlwmEthernetInterface`, which under `-itlskywalkbsd` is attached but unregistered, so
      the tee still runs but its output must now reach the bridge's ifnet rather than a BSD client.

      **Unit choice: DECIDED — keep the lowest free unit.** Retiring the legacy attach renames the
      interface (here `en3` -> `en2`), which orphans the existing Wi-Fi service and its
      preferred-network list. The alternative was to prefer whatever unit the legacy interface used
      to hold, so existing configuration survives. Rejected: the old unit only looks "right" because
      of the interface this mode deletes, `en3` left a gap at `en2`, and the migration is a one-time
      `networksetup -detectnewhardware` plus re-adding the service. Keeping a gap permanently to
      avoid a one-off reconfiguration is the worse trade.
      The orphan is visible as a service whose device no longer exists:
      `networksetup -listnetworkserviceorder` showing `Device: en3` with no `en3` in `ifconfig -l`,
      and `-listallhardwareports` listing neither. Removing it drops that service's saved settings;
      the passphrases are in the keychain and survive.

      **Still untested on this path: the Skywalk data path itself.** `RxFree`, `TxDequeue`,
      `RxDequeue`, `TxFrames`, `RxFrames` all read 0 — no association has happened yet, so nothing
      has driven the queues. `RxNoBuf` and `RxFallbackDrops` also read 0, which is consistent:
      without an association there are no data frames to tee.

      **Rule: a field an initialiser seeds from its own getter is one the caller must supply.**
      This has now bitten **three** times in the same struct — `+0x38` unit, `+0x0c` subfamily and
      `+0x108` MAC — with identical symptoms every time: plausible value, no error, wrong behaviour.
      The third was found *after* this rule was written and after it told the reader to audit the
      rest of the field map. The audit was not done, and it cost a boot plus a wrong diagnosis that
      blamed macOS for randomising the address. **Do the audit.**

      **The MAC field, `RegistrationInfo + 0x108`, and why its failure is invisible.**
      `registerInfraEthernetInterface` stamps it from `getSelfMacAddr()` (slot 415) — but only when
      **both** `[this+0x128]->byte[0x3c50] & 1` and `[this+0x120]->dword[0x58] == 1`. Neither is
      observable from the driver, registration proceeds and returns success either way, and Apple's
      `Override mac address for infra interface` log line does not reach us. So a failed stamp
      leaves the field at whatever the caller put there — zero, for a `memset` struct — and the
      ifnet attaches with no address. Something downstream then synthesises a **random
      locally-administered** one, regenerated on every boot.
      Measured on 26.6: `en3` came up `ee:a6:14:c7:09:55`, then `6a:88:fb:80:b5:71` on the next
      boot, while `IOSkywalkLegacyEthernet` correctly held the card's `4c:44:5b:84:76:cd`.
      **That reads exactly like a macOS Private Wi-Fi Address and is not one** — it persisted with
      the setting confirmed Off, which is what finally ruled macOS out. Now seeded from `ic_myaddr`
      unconditionally, beside the unit and subfamily.
      **AND SEEDING `+0x108` WAS NOT SUFFICIENT — measured, `en3` still came up
      `d2:d6:47:e2:0e:1c` with the seed in place.** The field the ifnet is built from is not the
      caller's struct but the **installed** one, `mExpansionData2->fRegistrationInfo`
      (`[iface+0x118]`), and `IOSkywalkEthernetInterface::initBSDInterfaceParameters` is what reads
      it:

      ```c
      info = *(RegistrationInfo **)[this + 0x118];
      if (memcmp(&info[0x108], zeros, 6) == 0)      // MAC still unset?
          this->vtable[333](this, &info[0x108]);    // getHardwareAddress(ether_addr *)
      *sdl_out      = &info[0x1e];                  // the sockaddr_dl
      eparams[0x10] = &info[0x108];                 // and the ifnet's lladdr
      ```

      So **slot 333 `getHardwareAddress` is the real hook**, and Apple's implementation of it
      (`IO80211SkywalkInterface::getHardwareAddress`) just returns `getSelfMacAddr()` — slot 415,
      which reads `[[this+0x120]+0x50]+0x10`, a WCL-side chain nothing in this driver populates. It
      yields 0, the ifnet is built with a zero lladdr, and an address is synthesised upstream.
      It was briefly overridden on `AirportItlwmSkywalkInterface`, answering from `ic_myaddr`, with
      counters `ItlwmHwAddrCalls`/`Ok`/`Fail`. **That override and those counters no longer exist —
      the experiment was reverted once `Calls = 0` proved the family never asks** (see below), and
      nothing has needed it since. Do not go looking for them.
      **Its return type was declared `void` and is an `IOReturn`** — the caller does `test eax,eax`
      and decides the link address on it, so a void override would have handed the decision to an
      undefined register. Corrected on both `IOSkywalkEthernetInterface` and
      `IO80211SkywalkInterface`, and the same correction has since been applied to slots 334 and 335
      for the same reason; return type is not mangled, so no slot moved.
      **Rule: when a struct field the caller supplies has a "if still empty, ask the driver"
      fallback, the fallback is the real contract.** Seeding the field is an optimisation; the
      virtual is what has to be right.

      **AND THAT OVERRIDE IS NEVER CALLED EITHER — measured: `ItlwmHwAddrCalls = 0`, `en3` came up
      `72:d7:e7:05:4f:de`.** Which is itself the answer: `getHardwareAddress` only runs when the
      installed MAC is zero, so a zero was never the problem. The chain, traced instead of guessed:

      ```text
      copyRegistrationInfo:  memmove(installed, caller, 0x130)
                             then  getProperty("IOMACAddress", &installed[0x108], 6)   (slot 81)
      initBSDInterfaceParameters: installed[0x108] non-zero -> straight into the ifnet's lladdr
      ```

      So `RegistrationInfo + 0x108` is populated from the **`IOMACAddress` registry property**, and
      that property already held the random address. A scan for every reference to the
      `"IOMACAddress"` literal in the boot collection finds **five**, and only two of them write it:
      `IOSkywalkEthernetInterface::setLinkLayerAddress` (slot 335) and
      `IOSkywalkEthernetInterface::ioctl_lladdr` — **the `SIOCSIFLLADDR` handler**.

      **RESOLVED — the writer is `IO80211MacAddressAgent`, it is INSIDE the family, and the driver
      seeded it with zeros.** The CoreCapture interface log names it outright and needs no reboot:

      ```text
      updateMacAddress@288 role<Infrastructure> init<1> informSkywalk=<0> isInterfaceEnabled=<0>
                           client<4>  mac address changed = <00:00:00:00:00:00> -> <E6:D3:46:D3:CF:FE>
      updateMacAddress@288 role<Infrastructure> init<0> informSkywalk=<1> isInterfaceEnabled=<1>
                           client<2>  mac address changed = <E6:D3:46:D3:CF:FE> -> <DE:41:9A:28:ED:75>
      ```

      Two changes per boot, and the first starts from **all zeros** — so the family had no address
      to work from and minted a random locally-administered one, which the per-network Private
      Wi-Fi Address then replaced. The card's real MAC never entered the chain at all.

      Where the zeros come from, traced through three exported functions:

      ```text
      AirportItlwmV2::start:            fNetIf->init(this, NULL)        <- OURS, and the bug
      IO80211SkywalkInterface::init(IOService *, ether_addr *):
                                        if (mac) state[0xe4..0xe9] = *mac    (skipped on NULL)
      IO80211SkywalkInterface::start:   IO80211MacAddressAgent::withOptions(this, state[0xe4], log)
      ```

      `init`'s second argument is the family's **only** source for the hardware address, and this
      driver passed NULL from the day the Tahoe target was added. `IO80211SkywalkInterface::
      setInitMacAddress(ether_addr &)` writes the same field and is the alternative if it ever has
      to be set after `init`. Fixed by seeding from `ic_myaddr` — `fHalService->attach()` has
      already run at that point, so the value is available.

      **This also refutes what this entry said about the agent.** `IO80211SkywalkInterface::
      setHardwareAddress` (slot 334) reaches `[[this+0x120]+0x50]` and was written off here as "a
      WCL-side chain nothing in this driver populates"; the log proves the agent exists and is
      running. It was never NULL — it was correctly built around an address of zero.
      **Rule: an object reached through a pointer chain the driver does not populate is not
      therefore absent.** The family builds its own; check for its log output or its instance count
      before concluding a slot is inert.

      *Two theories were spent before this* (an unseeded `+0x108`, then a missing slot-333
      override); both were refuted by their own counters in one boot each, which is the
      instrumentation working, but neither was justified before it was shipped. The third — "the
      address is assigned from outside through `SIOCSIFLLADDR`" — was also wrong, and wrong in a way
      worth naming: `ioctl_lladdr` *is* a writer of the `IOMACAddress` property, so a scan for
      writers found it and it fit. It simply was not the one that fired.
      **Rule: enumerating the writers of a field tells you who CAN write it, not who DID.** One
      `log show` for the family's own logging distinguished them in a single command, after four
      theories and three boots had not.

      **SCOPE, measured: the synthesised address appears ONLY when registration runs.** A boot with
      plain `-itlskywalk` (build the queues, tear them down, hand the family nothing) brings `en3`
      up with the factory `4c:44:5b:84:76:cd`, completes DHCP and passes traffic at 0% loss. So the
      bug lives in the `registerInfraEthernetInterface` path, not in the driver generally, and it is
      not macOS privacy — `setSET_MAC_ADDRESS` is stubbed, so macOS cannot apply a private address
      at all and falls back to the hardware one even on a network configured "Fixed".

      **Rule: a symptom that appears under one boot-arg and not another is scoped by that arg — use
      it before disassembling.** Four MAC theories were pursued before anyone booted the *other*
      flag, which would have localised it in one boot.

      **Rule: a locally-administered MAC on an interface is not proof the OS randomised it.** Check
      whether it *changes across boots* and whether a second node on the same device holds the real
      one; a synthesised fallback address looks identical to a privacy address from `ifconfig`.

      **THE TX PATH HAS NOW RUN, AND IT PANICKED — which is progress, because it means the netif
      finally drove our submission queue.** Measured on 26.6, `-itlskywalkbsd`, joining a network
      with Private Wi-Fi Address = Fixed: the machine connected, then trapped on the first frame
      *sent*:

      ```text
      configd -> bpfwrite -> dlil_output -> nx_netif_host_output -> nxp_tx_qset_notify
        -> IOSkywalkTxSubmissionQueue::packetSubmission(bool)
        -> IOSkywalkTxSubmissionQueue::legacyDequeue          <- NOT listDequeue
        -> AirportItlwmSkywalkInterface::handleTxDequeue
        -> IOSkywalkTxCompletionQueue::enqueuePackets + 0x9d  <- CR2 = 0x140
      ```

      Cause and fix are the mode-selector correction above: TX was in legacy/array mode while the
      handler walked a list. **Everything else in that backtrace is the data path working** — the
      nexus notified the queue set, `packetSubmission` ran, and our callback was reached with real
      packets, which no earlier boot achieved. `TxDequeue` had read 0 on every previous boot.
      Two counters make the next boot readable: `ItlwmSkywalkTxDequeue` must now leave 0, and
      `TxFrames` must follow it. `TxDequeue` climbing with `TxFrames` flat is frames consumed and
      discarded — healthy from the family's side, dead from the user's.

      **What to read after any change to this path**, in order. This IS the machine's Wi-Fi now, so
      two failure modes matter and both are unforgiving: with no fallback a broken RX path is silent
      data loss rather than a slow path, and with the queues enabled a wrong data-path contract
      **panics on the first frame** rather than doing nothing. **Recovery is the previous kext** —
      there is no boot-arg to drop — so keep the last known-good build on the EFI and confirm which
      one booted with `scripts/kextuuid.py --expect`.
      1. `ItlwmSkywalkBsdUnit` — the unit chosen. `~0` means the seed never ran. (On the path-B
         branch this is advisory only; the seeded unit is -1.) `ifconfig -l` should show exactly one
         new `enN` and **no** stray extra.
      2. `ItlwmSkywalkStage` = **11**, `RegRet` = 0, `QueuesAdded` = 4, **`QueuesEnabled` = 4**.
         A queue that is added but not enabled refuses everything in silence. Stage 10 means
         registration succeeded and `enableSkywalkQueues` did not return.
      3. `ItlwmSkywalkRxPrimeRet` = 0. `0xe00002d7` means the queue was still disabled when the
         first pull ran; anything else non-zero and `RxFree` will stay 0.
      4. `ItlwmSkywalkRxFree` > 0 — the stack is lending buffers. It should now oscillate rather
         than fall monotonically: the low-water refill tops the list up at a quarter of the ring, so
         a value that keeps dropping towards 0 means `primeSkywalkRx` is not being re-armed.
      5. `TxFrames`/`RxFrames` climbing, and **`ItlwmSkywalkRxFallbackDrops` at 0**. Non-zero there
         is frames thrown away for want of a buffer, which is now real loss.
      6. `ItlwmLlAddrCalls` > 0 with `Synced` matching it and **`Late` = 0**. Calls = 0 means the
         family never told net80211 about the link address and DHCP will fail even though every
         queue counter looks perfect — the exact shape of the boot that found mechanism 21.
      7. `ifconfig en3 | grep "inet "` — the one check that separates "the queues work" from "the
         interface works". A boot has already produced flawless data-path counters with no address.
      Beside the counters, `sudo skywalkctl interface` is the family's own view and needs no reboot:
      a healthy en3 should show the same shape as a working `en1` (`RxPackets`, `RxSYNC`, …).
      `LLinkAdd: 1` with only `TxCopyMbuf` beside it is the inert-queue signature.
   3. Only then is TX/RX exercised at all. Read `ItlwmSkywalkRxFree` first — while it is 0 the
      stack has lent nothing and neither path has run.
   4. Retire 10 and 12 with the loan.
   Do **not** retire any of the three before the data path is *measured* carrying traffic; they are
   what makes the driver work today.

   **What to read on that first boot, in this order** — each answers a different question and the
   first one that is wrong makes the rest meaningless:
   1. `ItlwmSkywalkStage` = 11, `RegRet` = 0 — registration and queue enable both ran.
   2. `ItlwmSkywalkRxFree` > 0 — the stack is lending buffers, i.e. the RX submission path runs.
      If this is 0, RX never starts and `RxNoBuf` will be climbing.
   3. `ItlwmSkywalkTxFrames` and `RxFrames` both climbing — traffic is actually moving. `TxDequeue`
      climbing with `TxFrames` flat is frames consumed and discarded, which looks healthy from the
      family's side and dead from the user's.
   4. `TxComplFail`/`RxComplFail` = 0 — non-zero means packets are not being recycled and the pool
      drains; `TxListShort`/`RxListShort` = 0 — non-zero means the family's count disagreed with its
      own list, which should be impossible and invalidates the run.
   `ifconfig -l` showing **one** interface remains the cheap sanity check that no stray
   nexus-backed ifnet was published.

   **Done when** no `gItlwmLent*RegInfo` static remains, and 10 and 12 are gone with it.
   → `include/Airport/AGENTS.md`, `AirportItlwm/AGENTS.md`

2. **RESOLVED — the event-pipe guards are now dead code.** `createEventPipe` /
   `destroyEventPipe` refuse when `state[0xa8]` is NULL, because Apple dereferences that field
   unchecked in both. The real cause was `IO80211InfraInterface::start` returning false, and that
   is fixed: measured on 26.6, `ItlwmSkyIfStarted = 1`, `ItlwmSkyIfHasEvtSrc = 1`,
   `ItlwmEventPipeCalls = 17` and `Destroys = 13` with `Refused = 0` and `DestroysRefused = 0` —
   the pipes are being created and destroyed for real, and the guards never fire. The stated
   "Done when" is therefore met. *Remaining work:* delete both guards, `itlwmSkyIfEvtSrc`, the
   `ITLWM_SKYIF_*_OFFSET` constants and the four `ItlwmEventPipe*` counters; they are raw offsets
   valid for 26.6 only, so leaving them is a liability, not insurance. Keep them until a second
   clean boot confirms the zeros. → `AirportItlwm/AGENTS.md`

3. **Latent mis-bindable vtable slots — and this one is no longer hypothetical.** Only 240/241 of
   `AirportItlwmEthernetInterface` are pinned; the other 18 inherited slots whose method name an
   `IO80211*`/`IOSkywalk*` class also overrides remain holes the loader can fill wrong.
   Slot 452 (`getLastRxUnicastLinkActivityTime`) has now been pinned with an override returning 0 —
   `mapdrv.py` 210 correct / 0 wrong — because it was inherited, unpinned, and error-checked by
   `IO80211MacAddressAgent::setMacAddress`. **It did not fix the failure it was suspected of**, so
   this remains a hardening measure rather than a confirmed instance; the association bug it was
   meant to explain turned out to involve a pointer-shaped value that cannot be coming from the
   pinned override. See `AirportItlwm/AGENTS.md`.
   **The general hazard still stands and does not announce itself as a panic:** the shape to watch
   for is a slot whose correct implementation *cannot* fail, failing. Any unpinned inherited slot the
   family error-checks is a live hazard, not just the ones that could crash.
   **Pinning may not be enough.** Slot 452 is now pinned and verified by name, and a counter inside
   the override proves it is *still* never called during a join — so the loaded table does not
   dispatch to it. If that holds up, "pin the slot" does not defeat a loader mis-binding and
   understanding the loader's binding rule is the only real fix, not the fallback.
   *Real fix:* establish why the loader mis-binds. **Done when** the binding rule is understood, or
   a pinned slot is demonstrated to be reached. → `include/Airport/AGENTS.md`,
   `AirportItlwm/AGENTS.md`

4. **ICT is disabled on `iwx`.** `iwx_post_alive` no longer enables it, because the table was
   never written — not one of 203 interrupts. *Real fix:* root-cause the dead table.
   `ItlwmIctPaddrLo & 0xfff` (a misaligned IOVA truncated by `paddr >> 12`) and `ItlwmIctTblReg`
   are published and need one surviving boot to read. **Done when** the cause is known and ICT
   either works or the divergence is justified in place. → `itlwm/AGENTS.md`

5. **CLOSED — `enable`/`disable(IO80211SkywalkInterface *)` compiled out for `__MAC_26_0` is
   harmless.** Tahoe dropped those overloads; `setPOWER` is pure virtual in Tahoe's
   `IO80211Controller`, so its slot is necessarily bound, and it reaches `enableAdapter` →
   `fHalService->enable()`. That route is now measured sufficient: `networksetup
   -getairportpower en3` reports **On** and `system_profiler SPAirPortDataType` reports the
   firmware, country (DE/ETSI) and full channel list. No Tahoe replacement entry point is needed.
   Nothing to do; retained only so the absence is not mistaken for an oversight.

6. **`getProvider()` lies about the provider.** Returns the Skywalk interface to any IOKit
   caller once `isAttach` is set. Exonerated for the `SIOCSIFFLAGS` panic — the trap proved it
   is never called there — but it still hands a foreign object to generic machinery. *Real fix:*
   narrow it to the caller that needs it, or remove it and address the IOSkywalkFamily cast
   panic it was written for. → `AirportItlwm/AGENTS.md`

7. **`itldefer` is a timer standing in for a precondition.** `IOPCIEDeviceWrapper::start`
   defers `registerService()` via a `thread_call`, which delays everything downstream past the
   boot-time IOKit matching storm and the root filesystem mount. It is required on Tahoe — a
   single CCPipe created before `super::start()` hangs the boot reliably — but *why* it works
   was never established; the comment says so outright. **A timer is a race.** The documented
   default is 1 s and bring-up is being run at `itldefer=30`, which is itself evidence that 1 s
   is not enough and that nobody knows what the real margin is. *Leading hypothesis:* what it
   actually waits for is the root filesystem being mounted, because that is what CoreCapture
   needs before a CCPipe can be created. *Real fix:* identify the true precondition and wait on
   it — a matching notification or an explicit dependency — instead of a wall-clock delay.
   **Done when** `-itlnodefer` boots, or the wait is on something deterministic.
   → `AirportItlwm/AGENTS.md`, `include/Airport/AGENTS.md` (the bisection)

8. **`iwx_run_init_mvm_ucode` dropped upstream's wait loop.** OpenBSD retests the condition:
   `while ((sc->sc_init_complete & wait_flags) != wait_flags) { tsleep_nsec(...); }`. This port
   has that commented out and a single unconditional `tsleep_nsec` in its place, so it cannot
   observe a notification that arrived *before* the sleep, and cannot distinguish a spurious
   wakeup from a real one. `wait_flags` is now an unused variable — the compiler warns on it,
   which is the tell. `ItlwmPreSleepInitComplete` exists solely to detect the resulting lost
   wakeup. Nobody knows why the loop was removed; it predates this work and is exactly the kind
   of silent divergence `itlwm/AGENTS.md` warns about. *Real fix:* establish whether the loop
   can be restored given this port's `tsleep`/`wakeupOn` locking discipline (the shim does not
   hold the lock the sleeper sleeps on, which may be why it was dropped), then restore it or
   justify the divergence in place. **Done when** the loop is back or the reason it cannot be
   is written at the site. → `itlwm/AGENTS.md`

9. **All bring-up instrumentation comes out.** HAL markers, the `ItlwmTrace` ring and its four
   traps, `ItlwmMarkRef`, and the ioreg properties. Remove wholesale once Tahoe boots reliably.
   → `itlwm/AGENTS.md`, `AirportItlwm/AGENTS.md`

10. **The ifnet's Wi-Fi subfamily is written by hand.**
    `AirportItlwmEthernetInterface::forceWiFiSubfamily` pokes `IFNET_SUBFAMILY_WIFI` straight
    into `ifnet+0x22c` after `attachToDataLinkLayer`, because no IONetworkingFamily API reaches
    that field and without it `_if_functional_type` reports the interface as `WIRED`. Raw
    offsets, valid for 26.6 only, guarded by a family-word check that skips the write if the
    layout does not match. *Real fix:* subsumed by 1 — Apple's Wi-Fi drivers get the subfamily
    from the Skywalk path's virtual `getInterfaceSubFamily()`, which needs a real registration.
    **Done when** no `ITLWM_IFNET_*_OFFSET` constant remains.
    → `AirportItlwm/AGENTS.md`

11. **`AirportItlwmV2::start` ignores `fNetIf->start()`'s return value.** It now *succeeds*
    (`ItlwmSkyIfStarted = 1`), so this is no longer masking a live failure — but it masked the
    single most consequential failure of this bring-up for weeks, while 2 and the scan path were
    investigated as separate problems. Check the return and treat a false as fatal, or at minimum
    record it, so the next regression is attributed on the first boot instead of the tenth. It
    also calls `fNetIf->initRegistrationInfo` twice with identical arguments.
    → `AirportItlwm/AGENTS.md`

12. **`prepareBSDInterface` is gated by hand.**
    `AirportItlwmSkywalkInterface::gatedSuperPrepareBSDInterface` wraps the super call in
    `getWorkQueue()->runAction()` purely to close the interface work queue's gate, because
    `IO80211Glue::sendIOUCToWcl` panics `"trying to send on thread panic"` unless
    `inGate()` is true and `onThread()` is false. *Real fix:* subsumed by 1 — Apple never needs
    this, because it reaches `prepareBSDInterface` (slot 285) only from
    **`IOSkywalkNetworkBSDClient::gatedPrepareNexus(__ifnet *)`**, which already holds the gate;
    the `gated` in the name is the contract. (This entry previously named
    `registerNetworkInterfaceWithLogicalLink` as the caller. That function deals in steering rules
    and UUIDs and never calls the hook — the conclusion was right and the function was wrong.)
    This driver calls the hook directly from
    `AirportItlwmEthernetInterface::attachToDataLinkLayer`, on configd's thread, so it has to
    establish that context itself. Once registration is real, Apple's own path calls the hook
    gated and this wrapper is dead weight — a recursive close that still works and no longer means
    anything, which is the dangerous kind of leftover.
    **Done when** `attachToDataLinkLayer` no longer calls `prepareBSDInterface` itself; at that
    point delete the wrapper, `superPrepareBSDInterface`, `gatedPrepareBSDAction` and
    `ItlwmPrepareBSDUngated` together with the `RegistrationInfo` loan they sit beside.
    → `AirportItlwm/AGENTS.md`

13. **Scan completion is a fixed 100 ms timer.** `AirportItlwmSkywalkInterface::beginScanGated`
    starts a net80211 background scan, arms `scanSource` for 100 ms, and then reports the scan
    complete regardless of what the scan is doing. So a "scan" is really "wait 100 ms, then report
    whatever the node cache already held". Inherited from the pre-Tahoe `setSCAN_REQ`, not
    introduced for Tahoe. **A timer is a race**, the same shape as 7. *Real fix:* net80211 already
    raises `IEEE80211_EVT_SCAN_DONE` (`ieee80211_node.c:1407`) and the V1 path in
    `AirportSTAIOCTL.cpp` already handles it — the V2 `eventHandler` simply does not, so the event
    exists and is being ignored. Drive completion off it. **Done when** no `setTimeoutMS(100)`
    remains on the scan path. It blocks 14: channel subsets and dwell times have nothing to act on
    while completion is unrelated to the scan. → `AirportItlwm/AGENTS.md`

14. **The scan request's parameters are ignored.** `setWCL_SCAN_REQ` discards the
    `apple80211ScanRequest` whole: no channel subset, no SSID filter — so no hidden networks — and
    no dwell times. A full scan is a superset of what was asked, so this is slow rather than wrong,
    which is why it is deferred rather than urgent. Layout already recovered: `+0x54` channel count,
    `+0x5c` an array of 12-byte entries. **Done when** the request is honoured.
    (The IE-truncation half of this entry is resolved: `postScanBeacon` now caps at `0x800`, which
    is what Apple's own `getBeaconMsgFromWLBSSInfo+0x2d8` does, so it is no longer a divergence.)
    → `AirportItlwm/AGENTS.md`

15. **RESOLVED — the WCL association path connects and the connection holds.** Measured on 26.6:
    one association request, a first-try join, `NET_MANAGER` in `LINK_UP`, DHCP complete, and the
    link held past 2.5 minutes with 0% packet loss. **Done when** was "an association completes";
    that is met, and the missed-beacons ceiling that followed it is closed too (message 39).
    What remains from this work is tracked as 17–20 below, not here.
    *The body below is the recovered contract plus the rules the bring-up bought. It has grown into
    a narrative and is due a trim pass: keep every `Rule:` and every counter-reading instruction,
    drop the boot-by-boot sequence.*
    `setWCL_ASSOCIATE` (slot 602) now
    programs the WCL's chosen candidate into net80211, and the two completion messages the
    `JOIN_MANAGER` FSM waits on are posted: **211** (`apple80211_assoc_event`, length **0x1c**) →
    `JOIN_ASSOC_COMPLETE` on `IEEE80211_EVT_STA_ASSOC_DONE`, and **213**
    (`apple80211_connection_complete_event`, length **0xa4**) → `JOIN_CONNECT_COMPLETE` on link-up;
    `JOIN_COMPLETE` the FSM raises itself. Failure is reported as a non-zero *connect* status, which
    walks the candidate list, because a non-zero *assoc* status raises no FSM event at all. Payload
    structs are in `include/Airport/JoinCompleteEvents.h`, packed and `_Static_assert`ed; the request
    is `include/Airport/AssocCandidates.h`. **The key travels inside that request**, at `+0x40`, as
    a whole `apple80211_key` — the field the header called `_unk40[0x94]`, identified by
    `sizeof(apple80211_key) == 0x94` matching the producer's `memmove` length and by
    `getKeyCipherType` reading offset 8 of it.
    **The WCL side is confirmed working on 26.6** — `ItlwmAssocCalls = Started = 5`, `Refused = 0`,
    and the failure reports walked the candidate list — but nothing has associated yet. Two causes
    found and fixed across two boots, both unretested:
    - `associateSSID` opens with `ieee80211_disable_rsn`, which `memset`s `ic_psk` and clears
      `IEEE80211_F_PSK`, so any key left in `ic_psk` was wiped on entry and `ieee80211_match_bss`
      then rejected the target BSS on every scan. **Rule: reusing a helper means inheriting what it
      *resets*, not just what it sets.**
    - there was no key to preserve in the first place. `ItlwmAssocNoPmk = 5` on 5 calls proved the
      WCL never calls `setCIPHER_KEY` during a join, so the documented "the PMK arrives through
      `setCIPHER_KEY` and that is the only way" was false. The key was in the request all along,
      behind an `_unkNN`. **Rule: when a producer copies a fixed-size blob into an unknown field,
      match that length against every struct the same code path already handles before naming it
      `_unkNN`.** That is the flag-bit hazard at field scale.
    - **the association request never left the host.** `ItlwmJoinMgtqMax = 1` — the frame sat in
      `ic_mgtq`. `ieee80211_send_mgmt` enqueues and calls `if_start`, which is
      `iwx_start` → `getMainCommandGate()->attemptAction(...)`: **non-blocking**, so it runs
      nothing when another thread holds `_fWorkloop`'s gate — the work loop the whole WCL
      interaction shares. Nothing retries, because `ifp->if_timer` is armed only after a
      *successful* transmit. `drainStrandedMgmtFrames()` re-drives `if_start` from the watchdog
      work loop and counts `ItlwmMgtqKicks`. **That repairs the missing retry, not the
      contention** — see the two open questions in `AirportItlwm/AGENTS.md`.
    - with the key in place net80211 reached `ASSOC` (`ItlwmJoinMaxState = 3`) and the exchange
      still never completed, because the setter called `ieee80211_end_scan` itself. That function
      belongs to `ItlIwx::iwx_endscan`, which clears `IWX_FLAG_SCANNING` before calling it; going
      round it left the firmware sweeping channels while net80211 walked to `AUTH`, so the frames
      went out off-channel. Now nudges `ieee80211_new_state(SCAN)` instead, which reaches the same
      place through the HAL. **Rule: a net80211 entry point the HAL also calls is not a free
      function — grep the HAL before calling one from the AirportItlwm layer, because whatever it
      does around the call is part of the contract.**
    - **the association now completes.** With the stranded frame re-driven, the join reached
      `ieee80211_recv_4way_msg3` — authentication, association and the start of the WPA2 four-way
      handshake all work. It then panicked `"trying to send on thread panic"` in
      `IO80211Glue::sendIOUCToWcl`, reached from `setLinkState` →
      `setLinkStateInternal` → `updateLinkSpeed` on `_fWorkloop`'s own thread. **The gate/thread
      contract is the *family's*, not `postMessage`'s** — only the `postMessage` calls had been
      deferred, while `setLinkState`, `setRunningState` and `reportLinkStatus` beside them were
      not. All of them now share the deferral ring, which also keeps them ordered.
      **Rule: anything the driver calls on an `IO80211*Interface` from a net80211 or HAL thread
      must be assumed to reach `sendIOUCToWcl` until the disassembly says otherwise.**
    **The join FSM completes on 26.6.** One request, `ItlwmAssocCalls = Started = 1`,
    `ItlwmJoinAssocDone = ConnectDone = 1`, `Timeouts = 0`, nothing stranded, and no
    `ItlwmJoinFail*` property published at all — those appear only on a non-zero connect status, so
    the join never failed. `JOIN_MANAGER` walks `IDLE → IN_PROGRESS → ASSOC_DONE →
    CONNECT_COMPLETE → IDLE` cleanly in the WCL log.
    Read `ItlwmAssocCalls`/`Started`/`Refused`, `ItlwmJoinAssocDone`/`ConnectDone`/`Timeouts` and
    `ItlwmJoinMaxState` to localise any regression to the setter, BSS selection, the association
    or the handshake. `ItlwmJoinMaxState 3` is correct on success — sampling stops at link-up,
    one transition before RUN.
    **And the interface still reported `status: inactive`, because a completed join is not a
    connection.** `ifconfig en3` showed `RUNNING` but inactive, `networksetup` reported no
    association, `IOLinkStatus = 1` (valid, not active), and auto-join aborted every attempt with
    `driver not available`. The WCL log named the cause in one line: `NET_MANAGER: in state
    NET_MANAGER_STATE_LINK_DOWN got event NET_MANAGER_EVENT_CONNECT_COMPLETE moved into
    NET_MANAGER_STATE_LINK_DOWN` — a no-op, while `ROAM_MANAGER` accepted the same event and went
    `LINK_UP`. `WCLNetManager` subscribes to exactly one link message, **216**, and nothing else
    moves it out of `LINK_DOWN`. Now posted as `apple80211_link_status_ind` (0x10 bytes, byte 6 the
    link flag), **before** 213, counted by `ItlwmLinkIndUp`/`ItlwmLinkIndDown`. **Booted and
    working:** `NET_MANAGER: LINK_DOWN --LINK_UP--> WAITING_FOR_CONNECT_COMPLETE`.
    It then refused the link one transition later, and the log named why in three lines:
    `firstLinkUp` → `updateBss` → `getWCL_BSS_INFO` (ioctl **433**) → our `kIOReturnUnsupported` →
    `"Fail to get bss info"` → `leaveNetworkCommand` → `DEAUTH` → `LINK_DOWN`. **A stubbed getter
    was the gate for the third time**, after `getWCL_LOW_LATENCY_INFO` and slot 452. 433 is now
    implemented: its reply type `apple80211_beacon_msg` is a `BeaconMetaData` plus IE list — the
    exact structure `postScanBeacon` already builds, going to the same consumer
    (`WCLScanCacheStore::updateOrAddBeacon`) — so it is answered from a cache of the target BSS's
    last beacon, filled during the join scan. The WCL's buffer is `IOMallocZeroData(0x844)` =
    `sizeof(BeaconMetaData) + 0x800`, matching the IE cap already in use. Counters
    `ItlwmBssInfoCalls`/`ItlwmBssInfoEmpty`. **That cache was a race and failed on the next boot**
    (`Empty = 1`, `Calls = 0`, 546 beacons seen): it held one beacon, stored only on a match against
    the join target or `ic_bss`, and `ieee80211_notify_scan_beacon` fires only while `ic_state ==
    SCAN`, where `ic_bss` is the scan's scratch node rather than the target. Now keyed by BSSID over
    every BSS seen, 16 entries, oldest-out, with no reference to join state. **Rule: a cache whose
    fill condition depends on when something else happens is a race wearing a cache's clothes.**
    **Booted before that fix: 433 answers** (`Calls = 1`, `Empty = 0`) and
    the refusal moved one call later, to `getWCL_EXTENDED_BSS_INFO` (**460**) from `setCurrentBSS` —
    a fourth stubbed getter with the same disposal. Now implemented from
    `include/Airport/ExtendedBssInfo.h` (`0x214`, pinned from the caller's allocation and the four
    offsets `AppleBCMWLANNetAdapter::getExtendedBssInfo` writes); the WCL zeroes the buffer, so the
    unfilled MCS/MLO fields correctly read as "not supported", and the rate set and RSN element are
    filled from the node and the cached beacon. Counter `ItlwmExtBssInfoCalls`. Unbooted.
    **Rule: a WCL getter whose reply type the driver already produces elsewhere wants the same
    bytes.** Look for an existing producer before building a new one.
    **Rule: on this path, read the callee for its *next* `cmdIouc` before rebuilding.** Every gate
    here disposes of failure the same way — abandon the network — so fixing them one boot at a time
    buys exactly one call per reboot. Done statically for the whole link-up path, which found two
    more fatal gates the log had not reached: **454** (`WCL_LINK_STATE_UPDATE`, whose one-line
    caller `updateLinkState` returns its result verbatim into `setCurrentBSS`'s failure branch) and
    **502** (`WCL_UPDATE_FAST_LANE`). Both are *setters* and are now accepted rather than filled —
    the WCL is telling the driver what the driver already knows, so success invents nothing. 372
    and 489 are on the path too but their results are discarded, so they stay stubbed. The full
    table is in `AirportItlwm/AGENTS.md`.
    **Rule: accepting a setter and answering a getter are different acts.** Success on a setter
    means "understood"; success on a getter means "here is the data", so every remaining
    `kIOReturnUnsupported` getter must be judged on what its caller does with the answer.
    **The whole ioctl chain now passes and `NET_MANAGER` reaches `WAITING_FOR_IP`** — 433, 460, 454
    and 502 all satisfied, `ItlwmBssInfoEmpty = 0`, no `Fail to Get` in the log. It then died 57 ms
    later on **our own** message 216 with the flag clear. The cause is in the vendored stack:
    `ieee80211_newstate` drops `LINK_STATE_DOWN` at the top of every transition and relies on the
    `RUN` case to raise it again, but under RSN that is deferred to `ni_port_valid`, which the
    handshakes set exactly once — so a RUN → RUN transition (a background scan ending on the same
    BSS) drops the link permanently. Guarded in `ieee80211_proto.c`. **That guard alone was not
    enough** — the next boot showed the same teardown, because the `RUN -> RUN` transition should
    never have reached net80211: `iwx_newstate`'s same-state guard has been missing its `return 0`
    since it was added, so the `if` silently took the following block as its body and passed
    same-state transitions straight through (and inverted the A-MPDU/task teardown beside it).
    Restored. `ItlwmLinkDownState` sampled `ic_state` in `setLinkStatus` and read RUN, which is
    ambiguous — at least three different causes read RUN there. Replacing the deduction with
    `ItlwmLinkDownPair` (`(ostate << 16) | (nstate << 8) | mgt`) **named the edge in one boot:
    `0x030410`, ASSOC → RUN with `mgt` `ASSOC_RESP`, and `ItlwmDisableCalls = 0`.** The link-down
    the WCL acted on was emitted by the transition that *completes* the association, not by any
    loss of one. Both earlier guards are superseded by one predicate in `ieee80211_set_link_state`,
    the function that owns `if_link_state`: **never report the link down while `ic_state ==
    IEEE80211_S_RUN`** — in RUN an association exists by definition, and anything that ends one
    leaves RUN first. Unbooted.
    **Rule: a state sampled at a report is not the event that caused the report.** `ic_state` is a
    position; the question was about a transition, and no single position distinguishes transitions
    that share an endpoint. Record the edge, not the node — it cost three boots to stop deducing.
    **CONNECTED on 26.6 with that predicate in place**: link held, DHCP completed, `en3` reported
    `status: active` with an address — the first working association on Tahoe. The session then
    panicked in CoreCapture, and that was ours too: `CCPipe::withOwnerNameCapacity` only *inits* a
    pipe, and `CCDataPipe::start` is what allocates the notify timer that
    `CCDataPipe::enqueueBlob` dereferences unchecked when no client is draining. Our three pipes
    read `!registered, !matched` in the registry beside the family's own `registered, matched`
    ones. `startCCPipes()` now starts them, after `super::start()` so the documented pre-start
    CCPipe boot hang is not re-armed. Counters `ItlwmCCPipesStarted`/`ItlwmCCPipeStartFail`.
    **Rule: a factory named `with…` may only construct — check the IOKit `registered`/`matched`
    states of objects you create against Apple's equivalents on the same node.**
    **With the pipes started, `NET_MANAGER` reaches `LINK_UP`** (`WAITING_FOR_IP --IPV4_STATUS-->
    LINK_UP` after DHCP) and holds for **55 s**, then the WCL leaves on `<missed beacons timeout>`.
    `WCLNetManager::handleMissedBeacons` escalates in two stages — first a `reportFault` (which is
    what raised the CoreCapture fault report above), then `leaveNetworkCommand(…, 9, …)`, matching
    the logged `enhancedDisassocReason=<9>`. `assocTimerAction` calls it when **two** elapsed-time
    values both exceed 60001 ms. **Driver message 39 (`APPLE80211_M_LQM_UPDATE`) is the only thing
    that refreshes either**, and it is now posted every 5 s from the watchdog with per-interval
    beacon and rx deltas; payload `include/Airport/LqmEventData.h`, counters
    `ItlwmLqmPosts`/`ItlwmLqmBeaconStall`. net80211 had no beacon count at all, so `ic_rx_beacons`
    was added and is incremented only for beacons whose BSSID matches `ic_bss`.
    **CONFIRMED WORKING on 26.6.** `NET_MANAGER` reached `LINK_UP` and stayed there past 2.5
    minutes with `ItlwmLqmPosts = 33`, `ItlwmLqmBeaconStall = 0`, `ItlwmLeaveNetCalls = 0`,
    `ItlwmLinkIndDown = 0`, no `<missed beacons timeout>` in the log, and 75/75 pings at 0% loss.
    The 55-second ceiling that ended every previous session is gone. Read `ItlwmLqmPosts` first if
    it comes back: it must keep advancing for the whole life of a connection, and stops exactly
    60 s before the teardown.
    **That failure is a second open problem, not this one.** `ItlwmJoinMaxState = 1` — net80211
    stayed in SCAN and `ieee80211_node_choose_bss` selected nothing, which has now happened twice
    and was unattributable both times, because `ieee80211_match_bss` computes the reason and
    upstream discards it into a `DPRINTF` with no sink here. Now published as
    `ItlwmScanFailDes`/`ScanFailOr`/`ScanCand`/`ScanSkipped`; read `ItlwmScanFailDes` first whenever
    `ItlwmJoinMaxState` is 1. **Rule: when a failure is reported by four layers and explained by
    none, find the layer that computed the reason and threw it away.**
    **That instrumentation found the cause on its first boot, and it was ours.** `ScanFailDes = 0`
    with `ScanCand = 42`, twenty joins in two minutes each dying at exactly one watchdog tick with
    `ItlwmJoinTimeouts = 0`. `checkJoinProgress`'s "net80211 gave up" branch armed `fJoinWatchEss`
    from the *request's* `ssid_len` in `joinStarted()`, but tests net80211's `ic_des_esslen` — and
    between them sits `beginAssociateGated`'s early return for an exchange already in flight, which
    never calls `associateSSID`. The watch then guards an ESS nobody set, the next tick reports a
    failure that did not happen, the WCL tries the next candidate, and that candidate arrives in the
    same state. **The loop recreates its own precondition and has no exit.** Now armed after
    `associateSSID`, from `ic_des_esslen` itself. Unbooted.
    **Rule: a watch armed from a request and tested against driver state is only sound if the code
    between them cannot decline to act.** Arm it from the state the test reads, where that state is
    written — the beacon-cache race again, one layer up.
    **With that fixed the join succeeds on the first request.** `ItlwmAssocCalls = 1`,
    `ItlwmJoinAssocDone = ConnectDone = 1`, `ItlwmJoinTimeouts = 0`, `ItlwmJoinMaxState = 3`,
    `ItlwmAssocEsslen = 9`, `ItlwmEssClears = 0`, and `ItlwmScanFailDes = 0xC000` — target seen
    (`0x8000`), exact requested BSSID (`0x4000`), fail mask **zero**.
    **The `ASSOCFAIL_WPA_PROTO` rejection seen the boot before was corruption, not an RSN problem.**
    That boot read `ItlwmScanFailDes = 0x8060` off an `ieee80211com` a stale HAL object was
    overwriting; `ItlwmScanRsnDes` now reads 0 on a clean build and the same AP associates
    immediately. **Rule: a reading taken from a corrupted struct is not evidence of anything.**
    Confirm `ItlwmIcSizeHal == ItlwmIcSizeNet` before believing any `ic_*`-derived counter — an
    afternoon was nearly spent chasing an RSN mismatch that did not exist.
    The RSN breakdown (`ItlwmScanRsnDes`, with both sides of the overlap in
    `ItlwmScanNiRsn`/`NiCipher` and `ItlwmScanIcRsn`/`IcCipher`) is kept: `ASSOCFAIL_WPA_PROTO` is
    set by **eight** independent tests and is unattributable without it.
    Note `ItlwmJoinIcRsn`/`ItlwmJoinNiRsnCipher` do not cover this: they are captured only once
    `ic_state >= AUTH`, which a join rejected in `match_bss` never reaches, so they read 0 on
    exactly the boots where RSN is the question.
    **That boot also panicked, and it was a stale object file, not net80211.** A GP fault in
    `ieee80211_match_ess` on an `ess` pointer made of x86 instruction bytes, with `ic_ess` — the
    *last* member of `ieee80211com`, itself the *first* member of `iwx_softc` — overwritten by a
    HAL object built against the older, smaller struct. This session grew `ieee80211com` three
    times under incremental builds. Fixed by building clean; `ItlwmIcSizeHal`/`ItlwmIcSizeNet` now
    publish each translation unit's `sizeof` so the next occurrence names itself.
    **Rule: a fault inside vendored code is not evidence of a bug in vendored code.** Ask what
    wrote the memory it faulted on before reading the function it faulted in.
    (Two real `ieee80211_add_ess` bugs were found and fixed en route — an unzeroed `malloc` and a
    `free` of a still-linked list member — but neither caused this panic; that function has no
    caller on Tahoe.)
    **Message 175 and ioctl 434 were both false leads and are recorded here so they are not
    re-followed.** 175 (`updateBeaconCounter`) writes a counter on a different object and touches
    neither timestamp; 434 (`WCL_TRAFFIC_COUNTERS`) feeds `checkTrafficCounters`, which sets a power
    -state flag and posts a bulletin but also writes neither. Both *look* like the mechanism from
    their names, and neither is. **Rule: name the writer of the field the deadline actually reads —
    `findfield.py` over the struct offset settles in one command what a function name only
    suggests.** The offsets are `state[0x150]` and `state[0x158]` on `WCLNetManager::this[0x20]`.
    **`setWCL_LEAVE_NETWORK` (425) is implemented and confirmed working on 26.6**, which closes the
    state divergence the teardown exposed: `ItlwmLinkIndDown = 0` with `ifconfig` still `active` on
    an interface that could not pass a packet, because the WCL had left the network while net80211
    stayed in RUN. Tahoe removed `setDISASSOCIATE` from the protocol and routes every disconnect
    through 425, so that stub was a divergence rather than a missing feature. Measured on the
    missed-beacons teardown: `ItlwmLeaveNetCalls = 1`, `ItlwmLinkIndDown = 1`, and `ifconfig`
    correctly reports `status: inactive`. → `AirportItlwm/AGENTS.md`
    **Rule: a signal that was merely cosmetic becomes load-bearing the moment a new consumer acts
    on it.** That link-down had been emitted for years and cost nothing but a blink of `ifconfig`'s
    media status. When wiring a consumer to an existing signal, audit what *raises* it, not just
    what reads it.
    **Rule: one driver message can complete one FSM and leave a second one, watching the same
    event, unmoved.** Satisfying the FSM that owns the operation says nothing about the FSM the
    rest of the system reads. Before calling a WCL interaction finished, run `wclfsm.py` over
    *every* manager that subscribes to the same area and check each one reached the state its name
    implies — the live transition log makes that a single command.
    **Still open on a working connection:** `networksetup -getairportnetwork` reports "not
    associated" although `ifconfig` reports `active`, `NET_MANAGER` is in `LINK_UP` and `airportd`
    resolves the network, BSSID, channel and band — so this is a specific getter rather than a
    state problem. `getBEACON_INFO` (346) is the loudest remaining refusal on that path. The LQM
    rate and frame counters are also left unflagged, so the Wi-Fi UI shows `0.0Mbps`.
    **Also not closed:** `setWCL_REASSOC` (426) and `setWCL_JOIN_ABORT` are still stubbed, so
    roaming is unimplemented — the log already shows `setROAM@645: Unable to start roaming` when
    the WCL tries. The transmit contention that stranded the association request is *worked
    around*, not fixed — `ItlwmMgtqKicks` was 2 on the last boot, so the 1 Hz retry is carrying the
    driver, not standing by. Ioctl **446** (`WCL_SET_SCAN_HOME_AWAY_TIME`) is refused hundreds of
    times per connection; `WCLScanManager::handleScanComplete` only logs the failure, so it is
    noise rather than a gate, but it is the loudest remaining stub in the log and drowns the lines
    that matter — filter it out when reading, or accept it.
    Four findings from this worth keeping:
    - **A stubbed *getter* with no apparent connection to the task can be a fatal gate, and this
      has now happened twice on the join path alone.** The first blocker was not the setter:
      `WCLJoinManager::handleJoinRequest` called `getWCL_LOW_LATENCY_INFO` (ioctl 427), took our
      `kIOReturnUnsupported` as fatal, and fired `JOIN_REQ_FAILED` without ever calling
      `setWCL_ASSOCIATE`. The last one was `getWCL_BSS_INFO` (433), which made `WCLNetManager`
      abandon the network one transition after accepting the link. Neither reads like a
      precondition for joining. **Before blaming a setter, read the log for the first
      `kIOReturnUnsupported` the FSM sees** — one `log show` finds it, and every remaining
      `return kIOReturnUnsupported` in `AirportItlwmSkywalkInterface.hpp` is a candidate for the
      next one.
    - **A failure return from the setter is only logged.** `WCLFsmManager::cmdIouc(442, false,
      candidates, 0x6f8, NULL, 0)` sends a non-zero result to nothing but
      `logEmergency("Failed to send candidate to driver")`; no FSM event is raised, so an error
      *stalls* the join for 35 s rather than failing it.
    - **The subscription table says which message; the transition matrix says whether the state
      will act on it.** `scripts/abi/wclfsm.py` prints both now. It is what establishes that 211
      must precede 213 (`CONNECT_COMPLETE` does not accept `JOIN_ASSOC_COMPLETE`), that 213 alone
      suffices from `IN_PROGRESS`, and that 216 must precede 213 for `NET_MANAGER`.
    - **The instrumentation was aimed at the wrong layer for weeks.** Every question answered above
      by one `log show` had been answered instead by adding an ioreg counter, rebooting, and
      reading it back — a whole boot cycle per bit. The WCL logs its own FSM transitions and every
      ioctl result to the unified log; see the root **Runtime Debugging** section. Reach for that
      before adding a counter, and add a counter only for what the family does not already say.
    → `AirportItlwm/AGENTS.md`

16. **`apple80211_assoc_candidates` is only partly decoded, and three of its undecoded fields are
    flag bytes.** `include/Airport/AssocCandidates.h` pins every offset, but the meaning of
    `flags1e0`, `flags1e4` and `flags1e8` is recorded only as *which bits Apple sets*, not what any
    bit requires. Ten smaller `_unkNN` fields remain besides.
    The largest one is gone and it was load-bearing: `_unk40[0x94]` is an `apple80211_key`, and not
    reading it meant every association was attempted with no PSK. **That is the proof this entry is
    not cosmetic** — an unnamed field is an unread instruction, not merely a gap in the notes.
    Tracked separately from 15 on purpose: 15 closes when an association completes, and that would
    bury this entirely. The risk is not a panic — this struct is written by Apple and read by us, so
    ignoring a field cannot corrupt anything. It is that we **fail to honour a requirement** encoded
    in a bit we never read, which is the `BeaconMetaData` bit-14 shape again: nothing fails, the
    association merely comes up wrong or not at all, and no build or `mapdrv` check can see it. If a
    join is refused, misconfigured, or silently insecure, suspect these three bytes before
    suspecting the join code. The rule the RSSI bug bought — *treat every bit as gating something
    until the disassembly says otherwise* — is not yet paid for here.
    *How:* the producer names them. `WCLJoinRequest::fillAssocCandidatesList` sets each bit from a
    specific join-request field, and `AppleBCMWLANJoinAdapter::performJoin` / `::adjustMfp` read
    them back; `adjustMfp`'s name alone suggests at least one governs management-frame protection.
    **The struct was also 796 bytes short, now corrected to `0x6f8`, and the reason it went unnoticed
    generalises.** It had been sized at `0x3dc` from the last field `fillAssocCandidatesList` writes —
    but there is a *second* producer, `WCLJoinManager::getVendorSpeificIes`, which runs afterwards and
    writes a vendor IE run at `0x3e0` and a product-info IE at `0x4e8`/`0x4ec`/`0x4ee`. The buffer is
    `IOMallocZeroData`'d, so everything neither producer touches reads as zero and a short
    reconstruction stays self-consistent indefinitely. **Size a struct from its allocation, not from
    the last field one producer writes** — here `IOMallocZeroData(0x6f8)`, the `cmdIouc` length
    argument and `IOFreeData(p, 0x6f8)` all agree. Corollary: having found one producer, check for
    another before believing a field is unused.
    **Done when** no `_unkNN` remains in the header and each flag bit is named or explicitly
    recorded as inert. → `AirportItlwm/AGENTS.md`

22. **Setting "Private Wi-Fi Address" to Off makes every join fail.** Isolated by a controlled
    experiment on 26.6, no reboot involved: forget both networks, join each — both work with the
    default **Fixed**; then set Private Wi-Fi Address to **Off** on either and the join fails
    immediately with an error box, after prompting for a password that is already stored. The
    setting, not the kext, is the variable — it reproduced across `-itlskywalk`, `-itlskywalkbsd`
    and no flag at all, and survived every code change made while it was being (wrongly) chased as
    a driver regression.
    *Why Off is worse than Fixed:* with Fixed, macOS asks the driver to adopt a private address,
    `setSET_MAC_ADDRESS` returns `kIOReturnUnsupported`, and macOS falls back to the factory MAC —
    a soft failure. With Off it must assert the factory address explicitly, and something on that
    path is refused fatally.
    **REFUTED as the mechanism, and the userspace log names the real one.** `RANDOMISATION_STATUS`
    never appears in a failing capture. What does, from `airportd`:

    ```text
    -[CWXPCSubsystem associateToWiFiNetworkUsingSystemKeychain:...]: Association failed for network
      <redacted> on interface en3, returned error Code=-536870206 "tmpErr"
    [corewifi] END REQ [ASSOC] took 0.479s ... err=-536870206
    [corewifi] AUTO-JOIN: Auto-join aborted ... error=(37 'driver not available')
    ```

    `-536870206` = `0xE00002C2` = **`kIOReturnBadArgument`**, and the `driver not available`
    auto-join line after it is downstream noise, not the cause. That value appears exactly **once**
    in this driver's source and is compiled out for Tahoe, so it comes from `IO80211Family`
    rejecting the request before it reaches us — which is also why `ItlwmAssocCalls = 0`: the
    failure is on the `APPLE80211_IOC_ASSOCIATE` (ioctl 20) path, not the WCL path that counter
    tracks. On Tahoe this driver deliberately does **not** implement `setASSOCIATE`
    (`#if __IO80211_TARGET < __MAC_26_0` in both the header and the definition), because slot 602
    `setWCL_ASSOCIATE` is documented as Tahoe's only association entry point.
    **CONFIRMED by A/B traces captured with `log stream` across the join** (the only way to catch
    it — a `log show --last N` after the fact lands in the post-join poll and misses it):

    ```text
    Fixed / succeeds:            Off / fails:
      Set DISASSOCIATE   GOOD      Set DISASSOCIATE  GOOD
      Set WCL_ASSOCIATE  GOOD      Set ASSOCIATE     FAIL 0xe00002c2   <- and repeats
      Set ASSOCIATE      GOOD      (WCL_ASSOCIATE NEVER APPEARS)
      0 FAIL lines in the whole capture
    ```

    So **both settings use ioctl 20**, and with Off the family rejects it with
    `kIOReturnBadArgument` *before* dispatching to `setWCL_ASSOCIATE`. The earlier hypothesis —
    that Off selects a legacy path this driver does not implement — is **refuted**: ioctl 20 also
    carries the successful join, and returns GOOD there without any `setASSOCIATE` override.
    The difference must therefore be in the `apple80211_assoc_data` `airportd` builds when privacy
    is Off, and the validation that refuses it is family-side.
    **"Our `setASSOCIATE` is missing" is REFUTED, and the proof is structural rather than
    circumstantial: on 26.6 there is no driver-side `setASSOCIATE` slot at all.**
    `nm` over the whole collection finds **no class method named `setASSOCIATE`** — not in
    IO80211Family, not in Apple's driver — and `include/Airport/IO80211InfraProtocol.h` declares it
    only in the `#else` (pre-Tahoe) branch. Adding the override to the Tahoe target fails to
    compile with "does not override any member function", which is the check doing its job. Tahoe's
    only association hook for a driver is `setWCL_ASSOCIATE`, and that one demonstrably works.
    *Where to look next:* the single ioctl-20 handler, `apple80211setASSOCIATE`, casts to
    `IO80211NoneProtocol` and returns `0xe082280e` on failure — not the observed `0xe00002c2`. So
    for an infra interface the request is refused *before* that handler, in the ioctl dispatcher,
    which is where `kIOReturnBadArgument` usually means a length/version mismatch on the incoming
    `apple80211_assoc_data`. Find that validation.
    **Priority is low:** leaving the setting at the default "Fixed" works completely, on every
    network and every boot-arg.
    **Rule: to test "we are missing handler X", try to declare it.** A slot that does not exist in
    the release fails at compile time, which is faster and more certain than reasoning about traces.
    **Rule: `[IOC DEBUG]` shows what the kernel was asked; `airportd`'s own log shows why it gave
    up.** Six theories were spent reading only the first of those two.

    **`isDriverAvailable=<0>` is a DEAD END — measured 0 on a fully working, associated
    connection.** It appears on every `[IOC DEBUG]` line in both the failing and the succeeding
    case, so it discriminates nothing. Earlier notes attribute it to `enable()` never running; that
    attribution is wrong and the field should not be read as a health indicator again.

    *Earlier suspect, refuted:* `getRANDOMISATION_STATUS` (ioctl **378** = `0x17a`) is stubbed
    `kIOReturnUnsupported` in `AirportItlwmSkywalkInterface.hpp`. `apple80211getRANDOMISATION_STATUS`
    checks `isCommandProhibited(0x17a)` (slot 409) and then dispatches to the driver; Apple's
    *setter* is a hard `0xe082280e` stub, so only the getter matters. This is the same shape as the
    three stubbed getters that already gated the join path.
    **Done when** a network with Private Wi-Fi Address Off joins. **Confirm before implementing:**
    the failure reproduces by toggling the setting, so one `[IOC DEBUG]` capture of a failing
    attempt names the refused ioctl without costing a boot.
    **Rule: when a symptom survives every code change, stop varying the code.** Five theories and
    as many boots were spent on driver mechanisms for a failure that a two-minute settings A/B
    isolated exactly. Ask what the user changed, and what is *configuration* rather than code,
    before the third theory — not after the fifth.
    → `AirportItlwm/AGENTS.md`

17. **`networksetup` reports the interface as not associated on a working connection.**
    `ifconfig en3` reports `active`, `NET_MANAGER` is in `LINK_UP`, and `airportd` resolves the
    network, BSSID, channel and band — so the association state is right everywhere except in what
    `networksetup -getairportnetwork` reads. That makes it a **getter gap, not a state problem**,
    and the same class as the four stubbed getters that gated the join: read the log for the first
    `kIOReturnUnsupported` on that path rather than guessing. `getBEACON_INFO` (346) is the loudest
    remaining refusal there, polled a few times per connection and currently stubbed.
    **Done when** `networksetup -getairportnetwork en3` names the network while connected.
    → `AirportItlwm/AGENTS.md`

18. **Roaming is unimplemented.** `setWCL_REASSOC` (426) and `setWCL_JOIN_ABORT` (438) are still
    stubbed, and the log already shows `setROAM@645: Unable to start roaming` when the WCL tries.
    Harmless on a single-AP network and a hard failure the moment the client should move between
    APs, which makes it the largest functional gap left on a connection that otherwise works.
    **Done when** both are implemented and a roam between two BSSIDs of one ESS completes.
    → `AirportItlwm/AGENTS.md`

19. **The management-frame transmit contention is worked around, not fixed.** `ieee80211_send_mgmt`
    enqueues to `ic_mgtq` and calls `if_start`, which is `iwx_start` →
    `getMainCommandGate()->attemptAction(...)`: **non-blocking**, so it runs nothing when another
    thread holds `_fWorkloop`'s gate, and nothing retries because `ifp->if_timer` is armed only
    after a *successful* transmit. `drainStrandedMgmtFrames()` re-drives `if_start` from the
    watchdog at 1 Hz and counts `ItlwmMgtqKicks`, which has been non-zero on every successful join
    — so the retry is carrying the driver rather than standing by, and every association pays up to
    a second of latency it should not. **Done when** a join completes with `ItlwmMgtqKicks = 0`,
    i.e. the frame leaves on the first `if_start`. → `itl80211/AGENTS.md`, `AirportItlwm/AGENTS.md`

20. **The LQM payload is filled only as far as the keepalive needs.** `postLqmUpdate` sets the
    counter group and the two master flags; the RSSI, SNR, CCA, channel and rate/frame groups are
    left with their validity bytes clear, which is *correct* rather than broken — the consumer skips
    an unflagged group instead of reading zeros. But it is visible: `airportd` reports
    `txRate=0.0Mbps rxRate=0.0Mbps txFrames=0` and the Wi-Fi UI shows no link rate. The driver has
    honest values for most of these. **Done when** the rate and RSSI groups are filled and flagged,
    and `log show --predicate 'process == "airportd"' | grep LQM:` shows a real rate.
    → `include/Airport/LqmEventData.h`, `AirportItlwm/AGENTS.md`

21. **A changed link address never reaches net80211: `ic_myaddr` and `ac_enaddr` are separate and
    the sync is ONE-WAY.** *(Scope, corrected: this is LIVE on `-itlskywalkbsd`, and it is the reason
    a working Skywalk data path still could not complete DHCP. This entry previously said it was
    unexercised because `setSET_MAC_ADDRESS` is stubbed — that reasoning was sound and the
    conclusion was wrong, because the address does not arrive through Apple80211 at all. It is
    assigned by `IO80211MacAddressAgent` inside the family and delivered to the driver through
    **slot 335 `setLinkLayerAddress`**, which nothing here overrode. See mechanism 1 for the trace.
    A stubbed setter blocks one route, not every route.)*
    **RESOLVED AND BOOTED on 26.6:** `ItlwmLlAddrCalls = 2`, `Synced = 2`, `Late = 0`, and `en3` came
    up with `inet 192.168.87.149` and the default route — the first DHCP lease on the Skywalk path.
    Both address changes the family made were followed into net80211, and both landed before the
    join, so the unimplemented re-association case below was not exercised.
    **The fix was in two parts, and each is useless without the other:**
    - `AirportItlwmV2::start` no longer passes NULL as `IO80211SkywalkInterface::init`'s
      `ether_addr *`. That argument seeds `state[0xe4]`, which is the agent's initial address, so a
      NULL made the family invent a random one before any privacy setting was consulted.
    - `AirportItlwmSkywalkInterface::setLinkLayerAddress` (slot 335) overridden: it copies the
      family's chosen address into **both** `ic_myaddr` and `ac_enaddr` and then calls super, which
      is what publishes `IOMACAddress` and calls `ifnet_set_lladdr`. Our half runs first so the two
      layers are never briefly inconsistent.
      Counters `ItlwmLlAddrCalls` / `Synced` / `Late` / `Last`. **`Calls = 0` means net80211 was
      never told and this fix did not run**; `Late` counts changes that arrived while `ic_state ==
      RUN`, which cannot take effect on the air — the AP knows us by the address we authenticated
      with — and is the signal that a re-association is needed and not yet implemented.
    `ic_myaddr` is its own array in `struct ieee80211com`
    (`ieee80211_var.h:444`) — it is *not* `ic_ac.ac_enaddr`. `ieee80211_ifattach` copies
    `ic_myaddr` → `ac_enaddr` once (`ieee80211.c:225`) and nothing ever copies back. The driver's
    `setHardwareAddress` calls `if_setlladdr`, which in this port is
    `memcpy(((struct arpcom *)ifp)->ac_enaddr, lladdr, 6)` (`_if_ether.h:267`) — so a MAC change
    lands on the BSD side only.

    That split is not cosmetic, because `ic_myaddr` is what 802.11 actually runs on:

    | user | file |
    | ---- | ---- |
    | source address of every transmitted frame (`wh->i_addr2`) | `ieee80211_output.c:222,2168,2244` |
    | the PTK derivation input for the WPA2 four-way handshake | `ieee80211_pae_input.c:321,420` |
    | **the RX "is this frame for me" filter** (`ETHER_IS_EQ(ic->ic_myaddr, eh->ether_dhost)`) | `ieee80211_input.c:1245` |

    So when a link address is assigned from outside, the BSD stack advertises it in ARP/DHCP while
    net80211 associates, encrypts and filters on the factory one. Replies the AP sends to the
    assigned address are then dropped **by our own RX filter**. `169.254` with the association up is
    the expected outcome.

    **This does NOT break authentication, and assuming it does has already produced one wrong
    diagnosis.** The PTK is derived from `ic_myaddr` and every transmitted frame carries
    `ic_myaddr` as `i_addr2`, so the AP derives from the same address the driver does — both sides
    agree and the four-way handshake completes. The known-good legacy path demonstrates this: it
    has the identical split and associates fine. The blast radius is L3 only: ARP, DHCP and any
    unicast reply addressed to the assigned MAC.
    **Rule: a two-address split only breaks the layers that disagree.** Enumerate which addresses
    each layer actually uses before attributing a failure to the split — here 802.11 is
    self-consistent and only the BSD/802.11 boundary is not.

    Visible without instrumentation: two different MACs on one device in `ioreg` —
    `AirportItlwmSkywalkInterface` showing the random one (the family publishes it from the ifnet)
    and `IOSkywalkLegacyEthernet` showing the real one (ours, taken from `ic_myaddr` at start).
    **Two MAC addresses on one device is the whole diagnosis; nothing else needs to be measured.**

    **The two slots that carry a MAC change, so the next one is not hunted from scratch:**

    | slot | who calls it | what it does |
    | ---- | ------------ | ------------ |
    | 334 `setHardwareAddress` | `ioctl_lladdr`, i.e. `SIOCSIFLLADDR` | asks the driver to ACCEPT a change; a non-zero return aborts the ioctl and it is called again with the old address to roll back. Apple's forwards to the agent. Not overridden here. |
    | 335 `setLinkLayerAddress` | `IO80211MacAddressAgent::updateMacAddress` when `informSkywalk` | TELLS the driver the change is happening. Apple's publishes `IOMACAddress` and calls `ifnet_set_lladdr`. **This is the one that fires on this machine.** |

    Both were declared `void` in our headers and both return `IOReturn`; 334's caller acts on the
    value. Corrected — return types are not mangled, so no slot moved.

    **Done when** was "a boot with a Private Wi-Fi Address enabled completes DHCP, and `ifconfig`
    and `ioreg` agree on one address". The first clause is met. **The second is NOT verified**: this
    boot was not re-checked with `ioreg`, and the two nodes may still legitimately differ — the
    `IOSkywalkLegacyEthernet` bridge reports the *hardware* address while the ifnet carries the
    assigned one, which may be correct rather than a bug. Establish what a genuine Mac shows before
    treating a difference there as a defect.
    The mid-session case is now mechanism 23; do not delete this entry without reading it.
    **Rule: two names for the same thing in a vendored struct are a one-way copy until proven
    otherwise.** `ac_enaddr` reads like an alias of `ic_myaddr` and is a snapshot of it.
    → `itl80211/AGENTS.md`, `AirportItlwm/AGENTS.md`

23. **A link address assigned while associated is accepted and never takes effect.**
    `setLinkLayerAddress` copies the new address into `ic_myaddr`/`ac_enaddr` whenever the family
    hands one over, but if `ic_state == IEEE80211_S_RUN` the change cannot reach the air: the AP
    knows the station by the address it authenticated with, and the PTK is derived from that
    address. The driver then transmits `i_addr2 = <new>` on a session established as `<old>`, and
    filters RX on `<new>` while the AP still sends to `<old>` — the same L3-only blast radius as 21,
    reached from the other direction. Nothing re-associates.
    Split out of 21 rather than left inside it: 21's fix is booted and working, and an open item
    inside an entry headed RESOLVED is one deletion away from being lost.
    *Not hypothetical:* macOS's Private Wi-Fi Address offers **Rotating**, which changes the address
    on a schedule by design, and any such rotation on a live connection lands here. The default
    **Fixed** does not — both changes on the measured boot arrived before the join.
    *How you know it is happening:* `ItlwmLlAddrLate` > 0. It was **0** on the only boot measured so
    far, so this has never actually been observed; the counter exists precisely to say whether it
    can occur in practice before any work is spent on it.
    *Cheap test before implementing anything:* set a network to Rotating, hold a connection, and
    watch `ItlwmLlAddrLate` and `ifconfig`. If it stays 0 this entry is theoretical and should be
    demoted, not built.
    *Real fix, if it does fire:* re-associate from the change — which is more than a `memcpy`, since
    it means tearing down and rebuilding the 802.11 session, and the WCL owns join on Tahoe
    (`setWCL_ASSOCIATE`), so the driver cannot simply re-run `associateSSID` behind its back.
    **Done when** a rotation on a live connection either completes a re-association, or is measured
    never to occur and this entry is deleted with that measurement recorded.
    → `AirportItlwm/AGENTS.md`, `itl80211/AGENTS.md`

## DOX framework

- DOX is highly performant AGENTS.md hierarchy installed here
- Agent must follow DOX instructions across any edits

### Core Contract

- AGENTS.md files are binding work contracts for their subtrees
- Work products, source materials, instructions, records, assets, and durable docs must stay understandable from the nearest applicable AGENTS.md plus every parent AGENTS.md above it

### Read Before Editing

1. Read the root AGENTS.md
2. Identify every file or folder you expect to touch
3. Walk from the repository root to each target path
4. Read every AGENTS.md found along each route
5. If a parent AGENTS.md lists a child AGENTS.md whose scope contains the path, read that child and continue from there
6. Use the nearest AGENTS.md as the local contract and parent docs for repo-wide rules
7. If docs conflict, the closer doc controls local work details, but no child doc may weaken DOX

Do not rely on memory. Re-read the applicable DOX chain in the current session before editing.

### Update After Editing

Every meaningful change requires a DOX pass before the task is done.

Update the closest owning AGENTS.md when a change affects:

- purpose, scope, ownership, or responsibilities
- durable structure, contracts, workflows, or operating rules
- required inputs, outputs, permissions, constraints, side effects, or artifacts
- user preferences about behavior, communication, process, organization, or quality
- AGENTS.md creation, deletion, move, rename, or index contents

Update parent docs when parent-level structure, ownership, workflow, or child index changes. Update child docs when parent changes alter local rules. Remove stale or contradictory text immediately. Small edits that do not change behavior or contracts may leave docs unchanged, but the DOX pass still must happen.

### Hierarchy

- Root AGENTS.md is the DOX rail: project-wide instructions, global preferences, durable workflow rules, and the top-level Child DOX Index
- Child AGENTS.md files own domain-specific instructions and their own Child DOX Index
- Each parent explains what its direct children cover and what stays owned by the parent
- The closer a doc is to the work, the more specific and practical it must be

### Child Doc Shape

- Create a child AGENTS.md when a folder becomes a durable boundary with its own purpose, rules, responsibilities, workflow, materials, or quality standards
- Work Guidance must reflect the current standards of the project or user instructions; if there are no specific standards or instructions yet, leave it empty
- Verification must reflect an existing check; if no verification framework exists yet, leave it empty and update it when one exists

Default section order:

- Purpose
- Ownership
- Local Contracts
- Work Guidance
- Verification
- Child DOX Index

### Style

- Keep docs concise, current, and operational
- Document stable contracts, not diary entries
- Put broad rules in parent docs and concrete details in child docs
- Prefer direct bullets with explicit names
- Do not duplicate rules across many files unless each scope needs a local version
- Delete stale notes instead of explaining history
- Trim obvious statements, repeated rules, misplaced detail, and warnings for risks that no longer exist

### Closeout

1. Re-check changed paths against the DOX chain
2. Update nearest owning docs and any affected parents or children
3. Refresh every affected Child DOX Index
4. Remove stale or contradictory text
5. Run existing verification when relevant
6. Report any docs intentionally left unchanged and why

### User Preferences

When the user requests a durable behavior change, record it here or in the relevant child AGENTS.md

- Run builds and tests through Claude Code's terminal, not by hand.
- Do not run `/init`; this AGENTS.md hierarchy is the documentation.
- **Checkpoint into DOX, not just into the reply.** At every pause, break or hand-off — not only
  at the end of a task — write the state worth keeping into the owning AGENTS.md: what was
  established, what is still open, and the **remaining to-do list in order**. A session's own
  task list is short-term by nature and disappears with the session; the next agent starts from
  the docs and nothing else. Long-lived findings go in the owning doc as contracts; short-term
  state goes in the relevant mechanism entry as an explicit *next steps* list, and is deleted
  when it is done rather than left to rot.

### Child DOX Index

- `include/AGENTS.md` — shared interfaces: HAL and ClientKit. Indexes
  `include/Airport/AGENTS.md`, which owns the `__IO80211_TARGET` ABI reconstruction
  workflow, records known ABI gaps, and holds the rule that a verified-on-disk vtable can
  still be mis-bound in memory by the kext loader.
- `AirportItlwm/AGENTS.md` — the `AirportItlwm.kext` sources and per-release Info.plist
  files. Owns how a macOS release gets a build target, and the panic-string trace/trap
  apparatus used for Tahoe bring-up.
- `itlwm/AGENTS.md` — the `itlwm.kext` driver, Intel HALs, and bundled firmware.
- `itl80211/AGENTS.md` — the vendored OpenBSD 802.11 stack and its kernel shims.
- `scripts/AGENTS.md` — firmware packing and local load/unload helpers.
- Root-owned files: `README.md`, `LICENSE`, `iwlwifi-firmware-license`,
  `itlwm.xcodeproj`, `.github/`, and root-level project documentation.
