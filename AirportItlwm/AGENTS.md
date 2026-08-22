# AGENTS.md — AirportItlwm

## Purpose

Sources for `AirportItlwm.kext`, the native Wi-Fi driver that subclasses Apple's private
`IO80211Family` classes so macOS treats the adapter as a real AirPort device.

## Ownership

- `AirportItlwm.cpp/.hpp` — the V1 controller (High Sierra … Ventura), subclasses
  `IO80211Controller` from `include/Airport/IO80211Controller.h`.
- `AirportItlwmV2.cpp/.hpp` — the V2 controller (Sonoma and later), built when
  `IO80211FAMILY_V2` is defined.
- `AirportItlwmSkywalkInterface.cpp/.hpp` — the V2 Skywalk data-path interface,
  subclasses `IO80211InfraProtocol`. Carries the bulk of the `apple80211_*` accessors. Its
  `prepareBSDInterface` gets the ethernet `RegistrationInfo` allocated by Apple's own
  `copyRegistrationInfo`; the network one is allocated by `registerNetworkInterface`, which this
  driver now genuinely reaches. It used to lend kext statics for both when the fields came up NULL
  — that stopgap was measured never to fire on 26.6.2 (`ItlwmRegInfoLentNet`/`LentEth` both 0 with
  `ItlwmSkywalkStage = 11`) and is deleted, along with `reclaimLentRegistrationInfo` and its hooks.
  **Never hand-allocate either field:** Apple frees them with `IOFreeType` from `early.kalloc.288`,
  a zone no kext can allocate into, and an earlier attempt panicked on the free.
  Its `start` override is where the real Skywalk registration hangs off a successful
  `super::start()`, and **its return is load-bearing**: `AirportItlwmV2::start` treats false as
  fatal on Tahoe. That check replaced hand-written NULL guards on
  `createEventPipe`/`destroyEventPipe` — both dereference Apple's `state[0xa8]` unchecked, and the
  only way to reach a NULL there was to ignore `start()`'s return. Do not weaken it back to a bare
  call without restoring the guards.
  It also seeds the family's MAC agent: `init`'s `ether_addr *` is forwarded to
  `IO80211SkywalkInterface::setInitMacAddress`, which it used to discard — see root AGENTS.md
  mechanism 21.
  See `include/Airport/AGENTS.md`.
  It also carries the **real Skywalk registration** (root AGENTS.md mechanism 1), which on Tahoe is
  **unconditional and is the machine's only Wi-Fi data path**. `start` builds two
  `IOSkywalkPacketBufferPool`s and the four queues from Apple's static factories, then calls
  `registerSkywalkInterface` → `IO80211InfraInterface::registerInfraEthernetInterface`. It
  suppresses `deferBSDAttach`, writes `-1` to `RegistrationInfo + 0x38` to select the
  legacy-ethernet bridge, sets the Wi-Fi subfamily at `+0x0c`, seeds the MAC at `+0x108`, attaches
  the ethernet interface *unregistered* (`attachInterface(..., false)`), and turns the RX tee's
  fallback into a drop.

  The ethernet interface object is kept on purpose — `_if_input` delivers RX through
  `iface->inputPacket()` directly, so the tee and the HAL paths still need it; only its BSD
  registration is gone. Retiring the class entirely is a later step.

  **There is no boot-arg and no fallback path.** `-itlskywalk`, `-itlskywalkreg` and
  `-itlskywalkbsd` were the bring-up staging and are deleted, along with `itlwmSkywalkOwnsBSD()`.
  Recovery for a bad boot is the **previous kext**, kept on the EFI beside the new one and
  identified with `scripts/kextuuid.py --expect`. Do not reintroduce a permanent opt-in: the
  configuration nobody boots is the one that breaks. A *new* change may take its own boot-arg,
  which is then deleted when that change lands.
  **TX and RX are both implemented; the two completion callbacks are still inert stubs.**
  The contracts they implement are in root AGENTS.md mechanism 1 and must be read before touching
  either, because on both submission queues `packets` is a **list head, not an array** and a 0
  return is a **refusal**, not "nothing to do".
  - `txSubmissionDequeue` → `handleTxDequeue` copies each frame into an mbuf, enqueues it on
    `ifp->if_snd`, returns the packets with `IOSkywalkTxCompletionQueue::enqueuePackets` and kicks
    `if_start`. Copy-to-mbuf rather than zero-copy is deliberate: `PoolOptions.memorySpec` is NULL
    so IOVAs are not established, and it reuses encapsulation, crypto and rate control whole.
  - `rxSubmissionDequeue` → `handleRxDequeue` parks the empty buffers the stack lends on
    `fRxFreeHead`/`fRxFreeTail`, guarded by `fRxFreeLock` (an `IOSimpleLock`: the list is touched
    from the submission queue's gate and from the HAL's receive thread, and the receive side must
    not block). `skywalkRxInput`, reached from `AirportItlwmEthernetInterface::inputPacket` through
    the `itlwmSkywalkRxInput` C shim, fills one with `mbuf_copydata` and delivers it with
    `IOSkywalkRxCompletionQueue::enqueuePackets`. **It returns false rather than dropping** on every
    failure, so the frame falls back to the BSD path; that is only correct while both interfaces
    exist — see the note in mechanism 1 about what must change when the BSD attach is retired.
    Every error path puts the buffer back on the free list: the pool is sized to the ring, so
    leaking one per fault drains RX within seconds.
  Counters: `ItlwmSkywalkStage` (1–11; **11 is complete**, 10 means registration succeeded and
  `enableSkywalkQueues` did not return), `RegRet`, `QueuesAdded` and `QueuesEnabled` (**both must
  read 4** — a queue that is added but not enabled refuses everything in silence),
  `Enabled` (now always 3), `BsdUnit` (advisory: the free unit that *would* have been used; the
  seeded value is -1, which is what selects the legacy-ethernet bridge),
  `RxFallbackDrops` (**must stay 0** — with the BSD fallback gone these are frames genuinely lost,
  not merely delivered slowly), the
  four raw callback hits `TxDequeue`/`TxComplete`/`RxDequeue`/`RxComplete`, and the per-direction
  detail — TX: `TxFrames`, `TxDrops`, `TxNoMbuf`, `TxComplFail`, `TxListShort`; RX: `RxFrames`,
  `RxDrops`, `RxNoBuf`, `RxOversize`, `RxComplFail`, `RxFree`, `RxListShort`.
  **Read the pairs, not the singles.** `TxDequeue` climbing with `TxFrames` flat is frames consumed
  and thrown away — healthy from the family's side, dead from the user's, and the exact shape of the
  stub it replaced. `RxNoBuf` climbing with `RxFrames` flat is a starved RX ring, i.e. the
  submission path is not running even though the receive path is. `RxFree` at 0 means the stack
  never lent anything. `TxComplFail`/`RxComplFail` non-zero means the pool is leaking, and
  `TxListShort`/`RxListShort` non-zero means the family's count disagreed with its own list, which
  should be impossible and invalidates the whole run.
  **Stage 11 with every failure counter at 0, a DHCP lease and a default route through this
  interface has been measured on 26.6** — registration, the queues, both data-path directions and
  the link-address sync all work, so none of it needs re-proving. Treat these counters as the
  acceptance test for any change to this path.
  Read them without a reboot:

  ```bash
  /usr/bin/log show --last 10m --predicate 'process == "kernel"' | grep itlskywalk
  ioreg -r -n IOPCIEDeviceWrapper -l -w0 | grep ItlwmSkywalk
  ```

  **Use the ioreg counters. The `IOLog` line does not arrive.** The `itlskywalk:` lines this code
  emits are absent from the unified log under any predicate, measured on a registering boot,
  while the counters for the same code path read correctly. Grepping the log for `itlskywalk`
  matches the **boot-args** line, which contains the string — easy to mistake for driver output and
  the reason this was believed to work. Deferring the publish did not make `IOLog` reach
  `log show` either, and that deferral is gone now regardless.
  Still add both: a counter must be wired into `publishRuntimeCounters` in the same edit that sets
  it, or it publishes nothing and the boot answers nothing — which is how a whole bring-up boot was
  wasted.
- `AirportItlwmInterface.*` — V1 interface class.
- `AirportItlwmEthernetInterface.*` — the BSD-facing `IOEthernetInterface` subclass. Used by
  V2 as well as V1: `AirportItlwmV2` attaches one as `bsdInterface` alongside the Skywalk
  interface. Its `getProvider()` override returns the Skywalk interface once `isAttach` is
  set, to keep IOSkywalkFamily's instance casts happy. That hack was suspected of causing the
  post-attach ioctl panic and **was ruled out** — with the trap armed it never fired, because
  nothing on that path calls `getProvider`. Its `errnoFromReturn` / `stringFromReturn`
  overrides exist only to pin vtable slots 241/240; see the loader contract below.
  Its `inputPacket` also carries the **Skywalk RX tee** (mechanism 1) — it offers each received
  frame to `itlwmSkywalkRxInput` before handing it to `super`, and frees the mbuf only when Skywalk
  took it. The tee reaches the Skywalk interface through a C-linkage shim rather than by including
  `AirportItlwmSkywalkInterface.hpp`, which needs `AirportItlwm`, `ItlHalService` and net80211 and
  cannot be included here. **This tee dies with this class**: retiring the BSD attach means moving
  it and turning its fallback-to-BSD into a drop.
- `AirportSTAIOCTL.cpp`, `AirportVirtualIOCTL.cpp`, `AirportAWDL.cpp` — `apple80211`
  request handlers.
- `IOPCIEDeviceWrapper.*` — the PCI nub that the kext actually matches on.
- `Info.plist`, `AirportItlwm-Monterey-Info.plist`, `AirportItlwm-Sonoma-Info.plist`,
  `AirportItlwm-Tahoe-Info.plist` — per-release bundle definitions.

## Local Contracts

- One Xcode target per macOS release, named `AirportItlwm-<Release>`. Each sets
  `__IO80211_TARGET`, its own `CONFIGURATION_BUILD_DIR` under
  `$(SYMROOT)/$(CONFIGURATION)/<Release>`, and its `INFOPLIST_FILE`.
- The PCI device list lives in the `IOPCIMatch` string of each Info.plist. A new device ID
  must be added to **every** release's plist, or it silently works on some OS versions only.
- `IOProviderClass` is `IOPCIEDeviceWrapper`, not `IOPCIDevice` — the wrapper personality
  matches the hardware and republishes it. Keep both personalities in step.
- Code that differs by release is gated on `__IO80211_TARGET`, never on `#ifdef` of a
  target name.
- **`IOResourceMatch` in a personality may be an OSString or an OSDictionary, and nothing else.**
  `IOService::checkResource` walks the metaclass chain against exactly those two and otherwise logs
  `<class>: Can't match using: <type>` and fails open — so a wrong type silently disables the gate
  rather than refusing to match, taking any previously-working entry with it. All 61
  `IOResourceMatch` entries in the 26.6.2 boot collection are `<string>`; an XML plist cannot
  express an OSSet, so **one resource per personality is the hard limit**. The kernel's own
  `('IOBSD', 'boot-uuid-media')` array is not a counter-example: it is a *matching dictionary* fed
  to `waitForMatchingService`, parsed by `IOResources::matchPropertyTable`, a different consumer
  with different type rules. Root AGENTS.md mechanism 7 has the full trace.
- **`IOResourceMatch` in a personality may be an OSString or an OSDictionary, and nothing else.**
  `IOService::checkResource` walks the metaclass chain against exactly those two and otherwise logs
  `<class>: Can't match using: <type>` and **fails open** — so a wrong type silently disables the
  gate rather than refusing to match, taking any previously-working entry with it. All 61
  `IOResourceMatch` entries in the 26.6.2 boot collection are `<string>`; an XML plist cannot
  express an OSSet, so **one resource per personality is the hard limit**. The kernel's own
  `('IOBSD','boot-uuid-media')` array is not a counter-example: it is a *matching dictionary* fed
  to `waitForMatchingService`, parsed by `IOResources::matchPropertyTable`, a different consumer
  with different type rules. Tried, measured and reverted here — root AGENTS.md mechanism 7.
- **Identity properties belong on the Skywalk interface, not the ethernet one.**
  `AirportItlwmEthernetInterface::attachToDataLinkLayer` owns the BSD ifnet, but the object
  userspace treats as the Wi-Fi device is `AirportItlwmSkywalkInterface`. Every property that
  describes the device — `built-in`, `IOInterfaceName`, `kIOInterfaceUnit`,
  `kIOInterfaceNamePrefix`, `kIOMACAddress` — must be set on `interface`, and `kIOMACAddress`
  on `this` as well. Neither interface node gets a MAC for free:
  `IOEthernetController::publishProperties()` publishes `kIOMACAddress` on the *controller*
  node only, so a plain `IOEthernetInterface` node carries none — verified against IntelMausi's
  `en0`, which has no `IOMACAddress` while its controller does. A write aimed at `this` when it
  was meant for `interface` therefore fails silently in the direction that matters: the
  ethernet node looks correct and the Skywalk node keeps a zero value. Diff the two nodes in
  `ioreg` rather than trusting either alone.
- **The link address has two homes and the driver owns the sync.** `ic_myaddr` (net80211) and
  `ac_enaddr` (BSD) are separate arrays copied one way at attach, and 802.11 runs on `ic_myaddr` —
  transmitted source address, PTK input, and the RX "is this for me" filter. On Tahoe the address is
  chosen by `IO80211MacAddressAgent` *inside the family*, not by this driver, and it is delivered
  through **slot 335 `setLinkLayerAddress`**, which Apple implements as "publish `IOMACAddress`,
  call `ifnet_set_lladdr`" — the BSD half only. `AirportItlwmSkywalkInterface::setLinkLayerAddress`
  therefore writes both arrays and **calls `super` last**, so the two layers are never briefly
  inconsistent. Two things follow and neither is optional:
  - **Seed the agent.** `AirportItlwmV2::start` must pass a real `ether_addr *` to
    `IO80211SkywalkInterface::init` — that argument is the family's *only* source for the card's
    address (it lands in `state[0xe4]`, which `start()` hands to `IO80211MacAddressAgent::
    withOptions`). NULL there is not a default: the agent mints a random locally-administered
    address instead, silently, and the interface then fails DHCP with every other counter perfect.
  - **`setHardwareAddress` (slot 334) is a different question from 335.** 334 is `SIOCSIFLLADDR`
    asking whether the driver *accepts* a change — a non-zero return aborts the ioctl and it is
    called again with the old address to roll back. 335 is the family *announcing* one. Both return
    `IOReturn` despite reading like `void` setters.
  `ItlwmLlAddrCalls`/`Synced`/`Late` say whether this ran; `Calls = 0` means net80211 was never
  told. See root `AGENTS.md` mechanisms 21 and 23.
- **An inherited vtable slot is a hole the kext loader fills, and it can fill it wrong.** In
  the built kext only our own overrides carry an address; every slot inherited from an Apple
  class is zero on disk and patched at load time. When our kext also contains another class
  that overrides the *same method name*, the loader can bind that one instead. This is a
  loader defect, not an ABI reconstruction error, and no header check can see it — the table
  is correct in the file and wrong in memory. If a method matters on a live path, define it
  and forward to `super`, which leaves no hole. `AirportItlwmEthernetInterface::
  errnoFromReturn` / `stringFromReturn` and `IONetworkController::_RESERVEDIONetworkController6/7`
  in `AirportItlwmV2.cpp` are the existing instances. `include/Airport/AGENTS.md` carries the
  scan that lists every exposed slot.

## Work Guidance

### Adding a macOS release

1. Copy the nearest existing `*-Info.plist` to `AirportItlwm-<Release>-Info.plist`.
2. Clone the nearest Xcode target in `itlwm.xcodeproj/project.pbxproj`: duplicate every
   object owned by that target under fresh IDs, then set `name`, `CONFIGURATION_BUILD_DIR`,
   `INFOPLIST_FILE`, and `__IO80211_TARGET`. Register the target in the `PBXProject`
   `targets` list, add its product to the Products group, and add a `BuildActionEntry` to
   `xcshareddata/xcschemes/AirportItlwm (all).xcscheme`.
3. Add the release's `__MAC_*` fallback to `itlwm/PrivateSPI.pch`.
4. Port `include/Airport/*.h` to the release's real class layout. **This is the actual
   work**; steps 1–3 only produce something that compiles. See `include/Airport/AGENTS.md`.

### Interface adoption by airportd

A boot that attaches cleanly is not a working Wi-Fi driver. Firmware bring-up and *adoption by
macOS* are separate failure domains, and the second is only reachable once the first stops
panicking. Two independent preconditions, both observed missing on Tahoe:

1. **A Wi-Fi network service must exist.** `NetworkInterfaces.plist` learns the interface on its
   own (`SCNetworkInterfaceType = IEEE80211`), but no service is created in `preferences.plist`,
   so `networksetup -setairportpower` answers "all AirPort network services are disabled" and
   nothing ever sets power. Create it once per machine:

   ```bash
   sudo networksetup -createnetworkservice "Wi-Fi" <bsd-name>
   sudo networksetup -setnetworkserviceenabled "Wi-Fi" on
   ```

2. **Apple80211's own enumeration must match the interface.** Independently of
   SystemConfiguration, `airportd` calls `Apple80211GetIfListCopy` → `_getIfListCopy`, which
   walks `getifaddrs()` and applies its own predicate. It logs both counts:

   ```text
   _getIfListCopy/9280: getifaddrs nInterfaces[24], count[0]
   [iflist] ... scnet: ((en3,)), ifnames: ((null))
   ```

   `scnet` non-empty with `count[0]` is the signature: SystemConfiguration accepts the interface
   and Apple80211 rejects it, so `airportd` believes no Wi-Fi hardware exists and never sends
   `APPLE80211_IOC_POWER`.

   The predicate's **first gate** is recovered from `_getIfListCopy` (read out of the shared
   cache with `scripts/abi/dsc.py` + `xref.py` + `dscdis.py`; the function has no symbol, so
   anchor on its `getifaddrs nInterfaces[%3u], count[%3ld]` format string). It issues plain
   interface ioctls — **no `apple80211` request is involved** — and accepts when

   ```text
   (SIOCGIFMEDIA current & IFM_NMASK 0xe0) == IFM_IEEE80211 0x80
   ```

   or, only if that ioctl fails, when `SIOCGIFTYPE == IFT_ETHER (6)` **and**
   `SIOCGIFFUNCTIONALTYPE == IFRTYPE_FUNCTIONAL_WIFI_INFRA (3)`. Wi-Fi is `IFT_ETHER`, not
   `IFT_IEEE80211`; do not "fix" `if_type`. Check any interface with `scripts/ifpred.c`.

   **That gate is not the whole test.** en3 passes it — `createMediumTables` already publishes
   `kIOMediumIEEE80211`, so the media word reads `0x80` — while `airportd` still logs
   `count[0]`. A further test follows at `_getIfListCopy` + the accept branch, whose call
   returns NULL for en3; resolving it needs the cache's chained-fixup format decoded to name
   the callee. Treat a passing `ifpred` as necessary, never sufficient.

   Ruled out as the cause, and worth not re-chasing:
   - The Skywalk interface's zero `IOMACAddress`. Real defect, fixed, but the code that caused
     it is in the shared V2 path, so Sonoma — where this driver works — had the identical zero.
   - `IODeferBSDAttach = Yes`. Also set unconditionally on the shared V2 path, so likewise not
     Tahoe-specific.
   - `if_type`. It is `IFT_ETHER` on en3, which is what Wi-Fi is supposed to be.

   Leading remaining candidate: en3's functional type is `WIRED (2)` where Wi-Fi must be
   `WIFI_INFRA (3)`. In XNU that value derives from `if_subfamily == IFNET_SUBFAMILY_WIFI`, and
   a plain `IOEthernetInterface` never sets it. It is also the most likely input to CoreWiFi's
   separate check, which logs `"is a valid network interface, but was not a valid 80211
   interface"`.

   **Functional type is not reachable through IONetworkingFamily.** Recovered from the 26.6
   (25G72) kernel collection, and the reason mechanism 10 in the root AGENTS.md exists:

   | fact | evidence |
   | --- | --- |
   | `if_family` is `ifnet+0x228`, `if_subfamily` is `ifnet+0x22c` | `_ifnet_subfamily` returns `[ifp+0x22c]` |
   | `WIFI_INFRA` needs `family == 2` **and** `subfamily == 3`; `ifnet+0xc2 & 0x10` then picks AWDL | `_if_functional_type` |
   | `ifnet+0x22c` comes from `ifnet_init_eparams+0x140` | `_ifnet_allocate_extended+0x47f` |
   | `IFNET_SUBFAMILY_WIFI == 3` | `AppleBCMWLANIO80211APSTAInterface::getInterfaceSubFamily` returns literal 3 |
   | `IONetworkInterface` builds a real eparams but **never writes `+0x140`** | stores are `0x10 0x18 0x20 0x24 0x78 0x7a 0xd0-0xf8 0x100-0x128 0x138`; `ver`/`len` go in as one `movabs 0x15800000002` |
   | `setInterfaceSubType` writes eparams `+0x138`, a *different* field | its only two kernelcache callers are `AppleUSBNCMData` and `BCM5701Enet`, both wired |

   `sizeof(ifnet_init_eparams)` is `0x158` and `ver` must be `2`; `family` is `+0x2c` and `type`
   is `+0x30`. `subfamily` is `KERNEL_PRIVATE` and in no SDK header here, so any change to these
   offsets must be re-derived with `scripts/abi/findfield.py` and `scripts/abi/disrange.py`.

   So every interface IONetworkingFamily creates gets `subfamily 0` — `WIRED` for the ethernet
   family. Apple's Wi-Fi drivers get theirs from the Skywalk path, whose
   `getInterfaceSubFamily()` is virtual; our ifnet comes from the legacy `IOEthernetInterface`,
   so that route stays closed until the registration is real.

   **That route is now open, and the raw poke is GONE.** `forceWiFiSubfamily` used to write
   `ifnet+0x22c` directly, guarded by a check that the family word read `IFNET_FAMILY_ETHERNET (2)`.
   It is deleted along with `ITLWM_IFNET_FAMILY_OFFSET` / `ITLWM_IFNET_SUBFAMILY_OFFSET` and the
   four `ItlwmIfnet*` properties (root AGENTS.md mechanism 10, closed). The replacement is
   `RegistrationInfo + 0x0c`, seeded in `registerSkywalkInterface` before the ifnet exists — a
   struct field rather than a release-pinned ifnet offset, and the route Apple's own drivers take.
   The poke was additionally dead on Tahoe: its only caller,
   `AirportItlwmEthernetInterface::attachToDataLinkLayer`, never runs there.

   The table above stays because it is what *establishes* that no IONetworkingFamily API reaches
   the field — that is the reason the Skywalk route is mandatory, and it has to be re-derived if
   the interface ever reads `WIRED` again. Check it from userspace, no rebuild needed:

   ```bash
   clang -O0 -o /tmp/ifpred scripts/ifpred.c && /tmp/ifpred en0 en3
   ifconfig -v en3 | grep type:
   ```

   **Measured on 26.6 with the RegistrationInfo route:** `ifpred` reports functional type 3
   (`WIFI_INFRA`), `ifconfig -v en3` reads `type: Wi-Fi`, two `Skywalk` agents plus an
   `agent domain:WiFiManager type:CallInProgress desc:"WiFi"` attach to the ifnet, and `airportd`
   drives `IO80211APIUserClient` — which it never did while the interface read `WIRED`.
   So functional type was a real gate. Whether `_getIfListCopy` now returns `ifCount[1]` is
   now **confirmed**: airportd fully adopts the interface. `networksetup
   -listpreferredwirelessnetworks en3` returns the stored network list, `-getairportpower en3`
   reports On, and `system_profiler SPAirPortDataType` reports `Card Type: Wi-Fi (0x8086, 0x84)`,
   the firmware version, `Country Code: DE`, `Locale: ETSI` and the full 2.4/5 GHz channel list.
   Adoption is no longer a suspect for anything.

3. **The event pipe must survive adoption — no longer guarded here, guarded upstream of it.**
   Once `airportd` adopts the interface it calls `IO80211APIUserClient::externalMethod` →
   `destroyEventPipe` → `destroyEventPipeGated` → `IO80211SkywalkInterface::destroyEventPipe`,
   which faults at `+0x22` on the same `state[0xa8]` NULL that `createEventPipe` does — so
   guarding only `create` would convert a boot-time panic into a panic ~90 s in, under `airportd`,
   as soon as adoption started working. That shape is still worth knowing.
   Neither is overridden any more. `state[0xa8]` is NULL only when `IO80211SkywalkInterface::start`
   half-failed, and `AirportItlwmV2::start` now treats that as fatal, so the state the guards
   defended against cannot be reached. Two release-pinned offsets went with them.
   **If that check is ever weakened back to a bare `fNetIf->start(this);`, restore both guards
   together** — root AGENTS.md mechanism 2 carries the disassembly.

4. **`prepareBSDInterface` must run with the interface work queue's gate closed.**
   `IO80211InfraInterface::prepareBSDInterface+0x112` → `updateStaticProperties` →
   `IO80211Glue::sendIOUCToWcl`, which tests two things on the work queue at glue ivars +0x38
   and panics `"trying to send on thread panic" @IO80211Glue.cpp:419` unless both hold:

   ```text
   wq->inGate()   == true      slot 39; false -> refuse
   wq->onThread() == false     slot 38; true  -> refuse
   ```

   That work queue is the interface's own — `IO80211SkywalkInterface::start+0xd5` fills the
   field from the controller's `getWorkQueue()` (slot 397), i.e. this driver's `_fWorkloop`, and
   `IO80211SkywalkInterface::getWorkQueue()` is the exported non-virtual accessor for it.

   Apple satisfies `inGate()` because it only reaches this hook from inside its own Skywalk
   registration. This driver has no registration to reach it from (mechanism 1), so it calls the
   hook from `AirportItlwmEthernetInterface::attachToDataLinkLayer` — on configd's thread via
   `IONetworkStack::attachNetworkInterfaceToBSD`, with nothing closed. `AirportItlwmSkywalk‐
   Interface::gatedSuperPrepareBSDInterface` wraps the super call in `wq->runAction()`, which is
   `IOWorkLoop::runAction` (slot 52): `closeGate` (48) / call / `openGate` (47), synchronously on
   the calling thread — so `inGate()` becomes true and `onThread()` stays false.
   `closeGate`/`openGate` cannot be called directly; they are protected in `IOWorkLoop`.

   Keep the gate scoped to the super call. `attachToDataLinkLayer`'s own work, IONetworkStack
   machinery included, must not run under it. `ItlwmPrepareBSDUngated` counts calls that found
   no work queue and therefore ran ungated; it should stay 0.

   **This wrapper is temporary and must be deleted when Skywalk registration becomes real** —
   mechanism 12 in the root AGENTS.md. It compensates for *this driver* calling the hook from an
   ungated thread; Apple's registration path already holds the gate, so once that path is in use
   the wrapper is a recursive no-op that still compiles, still runs, and no longer documents
   anything true. Remove `gatedSuperPrepareBSDInterface`, `superPrepareBSDInterface`,
   `gatedPrepareBSDAction` and `ItlwmPrepareBSDUngated` at the same time as the
   `RegistrationInfo` loan, since both exist for the same missing caller.

   Note what happens next, so a stall is not misread: `sendIOUCToWcl` hands the work to
   `runActionBlockOnSerial`, which runs the block inline only if the caller is in the gate of a
   **second**, separate queue (`IO80211WorkQueue::workQueue()`, glue ivars +0x58). We do not hold
   that one, so it takes the queueing path: signal an event source, then sleep on a **50 s**
   deadline waiting for the serial thread. Apple's `inGate(+0x38)` requirement means its own
   driver waits under the same held gate, so this is the intended shape rather than a
   self-inflicted deadlock — but a 50 s stall in BSD attach is a plausible symptom, and it ends
   in an error return rather than a hang.

### How this section reads

It is a worklog, kept in the order things were found, because each entry records a trap a future
release can re-set. **For where the port actually stands, read the last three sections** — the join
FSM completes, and message 216 to `WCLNetManager` is the current unbooted change. What follows below
is history from the point where scanning did not work.

**Property names below that no longer exist.** The measurements are still true of the boots they
record, but these counters have since been deleted with the mechanisms that needed them, so do not
try to read them on a current build: `ItlwmSkyIfStarted` / `HasState` / `HasEvtSrc` / `Ladder`,
`ItlwmEventPipe*`, `ItlwmIfnet*`, `ItlwmTraceCount`, `ItlwmGetProv*`, `ItlwmSkywalkEnabled`,
`ItlwmSkipCC` / `MinCC` / `CCOwner` / `CCPipeKB` / `NoStart` / `NoHal`. Likewise the boot-args
`-itlnocc`, `-itlmincc`, `-itlccowner`, `itlccsize`, `-itlnostart`, `-itlnohal`, `itlprovtrap`,
`itlmarktrap`, `itlifnettrap`, `itlcmdtrap`.

### Where it started on Tahoe: the interface comes up and scanning fails

Boots clean with no `-itlnocc`, no panic. Everything below is read from a running 26.6/25G72
system, not inferred:

```text
ItlwmSkyIfStarted 1     ItlwmSkyIfLadder 1023 (0x3ff)   ItlwmSkyIfHasEvtSrc 1
ItlwmEventPipeCalls 17  Destroys 13   Refused 0   DestroysRefused 0
ItlwmPostMsgQueued 1    Sent 1        Dropped 0   ItlwmPrepareBSDUngated 0
ItlwmIfnetFamily 2      SubfamilyNow 3            SubfamilySet 1
en3: Wi-Fi hardware port, radio On, DE/ETSI, 2.4+5 GHz channels, fw 68.01d30b0c.0
```

So: the interface starts, is adopted by `airportd`, has a live event pipe, a peer manager, a
scan manager and a powered radio. **`en3` has no `RUNNING` flag and `Status: Not Associated`,
because scanning fails.**

*That last sentence is where this section starts, not where it ends.* Scanning and results were
both fixed below, and the association path is now implemented too — see "Implemented —
`setWCL_ASSOCIATE` and the two completions". The sections in between are kept in the order they
were worked, because each one records a trap a future release can re-set.

```text
Apple80211IOCTLSetWrapper:6555 ifname['en3'] IOUC type 10/'APPLE80211_IOC_SCAN_REQ',
    len[5528] return -536870201/0xe00002c7        <- kIOReturnUnsupported
Apple80211Scan:1489 ifname['en3'] err[-536870201], Apple80211Scan Failed
```

`0xe00002c7` is `kIOReturnUnsupported`, so this is no longer a crash or a missing object — the
request reaches a handler that declines it. Note the failure has *changed*: it used to be `ENXIO`
from a NULL scan manager, which is gone. `copyChanInfoList` returns the same code; the
BSSID/SSID failures alongside it are `0xe0822403`, the ordinary not-associated error, and are not
a fault.

**Found and implemented: `setWCL_SCAN_REQ` (slot 601) was one of the "unsupported" stubs.** The
full route, all from disassembly:

```text
airportd APPLE80211_IOC_SCAN_REQ (type 10)
 -> setSCAN_REQ                     gSetHandlerTable[10], file-static
      getPrimaryInterfaceScanManager()      NULL here was the old ENXIO
      IO80211ScanManager::isScanAllowedByP2P
      IO80211Controller::scanStarted
      IO80211Glue::sendIOUCToWcl(..., type 10, ...)     the WCL owns the scan
 -> apple80211setWCL_SCAN_REQ
      isCommandProhibited(441)
      safeMetaCast to the infra protocol
      slot 601 -> AirportItlwmSkywalkInterface::setWCL_SCAN_REQ
```

There is **no second route**. The same handler falls back to `apple80211setSCAN_REQ` only when the
result is exactly `0xe082280f`, and that fallback safeMetaCasts to **`IO80211NoneProtocol`**,
returning `0xe082280e` for anything else — so it can never serve an infra interface. Returning
`kIOReturnUnsupported` from slot 601 both failed the scan *and* suppressed the fallback.

Tahoe's whole driver-facing scan surface is four slots, and this port stubbed all of them:
`setWCL_SCAN_REQ` (601), `setWCL_SCAN_ABORT` (596), `getWCL_BSS_INFO` (524),
`getWCL_BGSCAN_CACHE_RESULT` (533), plus `getBEACON_INFO` (506). There is **no `getSCAN_RESULT`**
on Tahoe — the pre-Tahoe result path is gone.

`setWCL_SCAN_REQ` now starts the same net80211 cached background scan the pre-Tahoe
`setSCAN_REQ` starts, under the work queue's gate, and lets `scanSource` post
`APPLE80211_M_SCAN_DONE` 100 ms later. Completion was already wired: the interface takes the
controller's `scanSource` in init. `ItlwmScanReqCalls` / `Started` / `Refused` report it.

**Then EBUSY(16): the WCL scan FSM was never told the scan finished.** Apple's own log says it
outright, and `wlan.debug.enable=1` does surface it — the `[IOC DEBUG]` and `[wcl]` lines come
through `log show --predicate 'process == "kernel"'`, which is where to look first for anything
scan-related:

```text
FSM SCAN_MANAGER: IDLE --SCAN_MANAGER_EVENT_SCAN_REQ--> IN_PROGRESS
INTERNAL: Set <APPLE80211_IOC_WCL_SCAN_REQ>  res=<GOOD:0:0x0>     slot 601 succeeds
EXTERNAL: Set <APPLE80211_IOC_SCAN_REQ>      res=<GOOD:0:0x0>     the scan IS accepted
...
FSM SCAN_MANAGER: IN_PROGRESS --SCAN_MANAGER_EVENT_SCAN_REQ--> IN_PROGRESS
[wcl] handleScanRequest@1208: WCLScanManager scan in progress rejecting
EXTERNAL: Set <APPLE80211_IOC_SCAN_REQ>      res=<FAIL:16:0x10>
```

Counters reconciled exactly against that log: `ItlwmScanReqCalls = 4` for **4**
`Set WCL_SCAN_REQ` out of **162** `IO80211ScanManager::startScan` attempts. The driver is called
precisely when the WCL forwards a request and starts the scan every time — the first scan of each
burst works and everything after is refused.

**Tahoe wants two completion messages, and this port sent one.** The FSM event
`SCAN_MANAGER_EVENT_SCAN_COMPLETE` is driven by message type **237**, not by
`APPLE80211_M_SCAN_DONE` (10). Apple splits them across two objects:
`AppleBCMWLANScanAdapter::scanComplete+0x55` posts 237 (`mov edx, 0xed`), and
`AppleBCMWLANCore::scanComplete+0x1d2` posts 10 for userspace. Both carry a 4-byte status with the
async flag, and the adapter's WCL event goes first. 237 is beyond `APPLE80211_M_MAX` (170) and has
no SDK name; it is declared as `APPLE80211_M_WCL_SCAN_COMPLETE` in `apple80211_var.h`, kept out of
the `M_BUFF_SIZE` bitmap. `fakeScanDone` now posts both, in that order.

**A route change that was NOT the fix, kept because the reasoning still holds.** Completion also
moved from `fNetIf->postMessage` to `IO80211Controller::postMessage`, which hands the event to the
`IO80211PostOffice` (controller ivars `+0xb18`, from `CreatePostOffice()`, slot 440) the way
`postMessageInfra` and both `scanComplete`s do. On its own it changed nothing — the FSM stayed
`IN_PROGRESS`, `ScanReqCalls` stayed pinned — because the missing piece was the message *type*.
It is the route Apple uses, so it stays, but do not credit it with fixing the EBUSY loop.

**Superseded — kept so it is not re-derived.** The EBUSY was at one point pinned on
`IO80211ScanManager::isScanAllowedByP2P`, whose refusal condition is
`(NAN disallows || AWDL disallows) && scan_type != 3`. That gate cannot fire here: both predicates
short-circuit to "allowed" when their manager is absent, and the IOKit class census shows
**`IO80211AWDLPeerManager = 0`, `IO80211NANPeerManager = 0`**.

```bash
ioreg -l -w0 | grep -o '"IO80211[A-Za-z]*"=[0-9]*'
```

`awdlSyncEnable` was also briefly suspected; it is only read back by an ioctl getter and causes
the family to build nothing, so changing it would have been a no-op costing a boot.

**Earlier stage, for reference: the error before all of this was `kIOReturnUnsupported`.** Measured
after implementing slot 601: `ItlwmScanReqCalls = 2`, `Started = 2`, `Refused = 0` — the driver
starts scans successfully — while `airportd` retried dozens of times, every one returning
`16/0x00000010`. Only 2 of those retries ever reached the driver, so the refusal is upstream.

Ruled out by measurement, not argument: `IO80211ScanManager::isScanAllowedByP2P` returns EBUSY
when `(NAN disallows || AWDL disallows) && scan_type != 3`, and it *looked* like the culprit. It
is not. Both predicates short-circuit to "allowed" when their manager is absent, and
`ioreg`'s IOKit class census shows **`IO80211AWDLPeerManager = 0` and
`IO80211NANPeerManager = 0`** — neither is ever created. So that whole gate returns 0.

```bash
# the census that settled it — cheaper and more certain than reading the family
ioreg -l -w0 | grep -o '"IO80211[A-Za-z]*"=[0-9]*'
```

That leaves `IO80211Glue::sendIOUCToWcl`, whose result the handler returns verbatim: the **WCL**
says EBUSY. `WCLScanManager = 1`, so it is live, and it believes a scan is still in flight — which
is exactly what you get if it never sees a completion.

**The route was wrong.** Completion was being posted as `fNetIf->postMessage(...)`, straight at the
interface. Apple's drivers post through the **IO80211PostOffice** instead:
`IO80211Controller::postMessage(itf, type, data, len, flag)` tail-calls slot 36 on controller ivars
`+0xb18`, which `IO80211Controller::start+0x471` fills from `CreatePostOffice()` (slot 440).
`AppleBCMWLANCore::postMessageInfra` and `::scanComplete` both go that way, and
`scanComplete` posts exactly what this driver already posted — type `0xa`
(`APPLE80211_M_SCAN_DONE`), 4-byte status, async flag — so only the route differed.
`postMsgGated` now uses the controller route for every message. The method is non-virtual and
exported, so declaring it costs no vtable slot.

**RESOLVED — the FSM now cycles.** Measured after adding message 237: `ItlwmScanReqCalls = 13`,
`Started = 13`, `Refused = 0`, `PostMsgSent = 27` (13 scans x 2 messages), and the log shows the
full round trip repeating:

```text
FSM SCAN_MANAGER: IDLE --SCAN_MANAGER_EVENT_SCAN_REQ--> IN_PROGRESS
INTERNAL: Set <APPLE80211_IOC_WCL_SCAN_REQ>  res=<GOOD:0:0x0>
FSM SCAN_MANAGER: IN_PROGRESS --SCAN_MANAGER_EVENT_SCAN_COMPLETE--> IDLE
```

No more `scan in progress rejecting`, no more EBUSY. Scanning is started and completed correctly.

#### Scan results: delivered by pushing beacons, not by any getter

`Status: Not Associated` and no networks, because the scan cache is empty:
`Get <APPLE80211_IOC_SCAN_RESULT>` returns `5`. The whole set of internal ioctls the WCL issues
during a scan is only three, and **`getWCL_BSS_INFO` (slot 524) is never among them** — so results
are not pulled from the driver:

```text
Set <APPLE80211_IOC_WCL_SET_SCAN_HOME_AWAY_TIME>  FAIL:-536870201   our stub, slot 605
Set <APPLE80211_IOC_WCL_SCAN_REQ>                 GOOD              ours, slot 601
Get <APPLE80211_IOC_OP_MODE>                      GOOD
```

They are **pushed**, as another `postMessage`. From
`AppleBCMWLANScanAdapter::processScanResults+0x224`:

```text
AppleBCMWLANBSSBeacon::getBeaconMsgFromWLBSSInfo(BeaconMetaData&, uint8 *frame, wl_bss_info*, ...)
IO80211Controller::postMessage(itf, 0xc9 /* 201 */, buf, *(uint32_t *)buf + 0x44, true)
```

So one message of type **201** per BSS, whose payload is a `BeaconMetaData` — 0x44-byte header with
the IE list length at offset 0 — followed by the IE list.
`WCLScanCacheStore::updateOrAddBeacon(BeaconMetaData&, uint8 *)` is the consumer.

Implemented: `AirportItlwm::postScanBeacon`, driven by `IEEE80211_EVT_SCAN_BEACON`. Measured
working on 26.6 — `ItlwmScanBeacons` climbs and networks appear in the menu and in
`system_profiler SPAirPortDataType` with correct SSID, channel, band and security, the last of
which proves the forwarded IE list is intact and correctly offset.

**`BeaconMetaData`, recovered.** Producer:
`AppleBCMWLANBSSBeacon::getBeaconMsgFromWLBSSInfo`. Authoritative consumer, which is where the
*meanings* come from: **`IO80211BSSBeacon::setBeaconDataFromMsg(BeaconMetaData&, uint8 *)`**.

The payload is this 0x44-byte header followed by the **IE list** — the tagged parameters, *not* a
whole 802.11 frame. `setBeaconDataFromMsg+0x27` passes `[md+0x00]` and the trailing pointer
straight to `allocateIEListBufferAndCopy(size_t, uint8 *)`, and the caller sends
`len = *(uint32_t *)buf + 0x44`.

| off | size | field | evidence (offsets in `setBeaconDataFromMsg`) |
| --- | --- | --- | --- |
| 0x00 | u32 | IE list length | `+0x1a` → `allocateIEListBufferAndCopy` |
| 0x04 | u16 | AppleChannelSpec | `+0x66` → beacon`+0x29f` |
| 0x06 | u8[32] | SSID | `+0x8b`/`+0xb7` → `setSSID(uint8 const*, uint8)` |
| 0x26 | u8 | SSID length | `+0x95` |
| 0x27 | u8 | primary channel number | `+0x6b` → beacon`+0x29e`; producer `+0x1d0` `ChanSpecGetPrimaryChannel` |
| 0x29 | u8[6] | BSSID | `+0x30` → `setAddress(ether_addr *)`, vtable `+0x178` |
| 0x30 | **s32** | **RSSI**, dropped unless flag bit 14 | `+0xc6` → `isNewBssBetter(int, bool)`; stored at `+0xfe` only under `bt edx,0xe` |
| 0x34 | s16 | noise, gated by flag bit 12 | `+0x130` `bt edx,0xc` → beacon`+0x288`; producer feeds it `wl_bss_info.phy_noise` |
| 0x36 | u16 | SNR, gated by flag bit 13 | `+0x113` `bt edx,0xd` → beacon`+0x28a` |
| 0x38 | u16 | beacon interval | `+0x5a` → beacon`+0x29c` |
| 0x3a | u16 | capability info | `+0x4e` → beacon`+0x2a2` |
| 0x3c | u8 | optional, gated | `+0x1f0` |
| 0x40 | u32 | flag bitmap | read ~10 times; see below |
| 0x44 | — | IE list bytes | |

The flag word at `0x40` is a **validity and provenance bitmap, not a set of optional booleans**.
Several fields above are discarded outright unless their bit is set, so a value can be at the right
offset, in the right units, and still be thrown away. Full bit list in
`include/Airport/BeaconMetaData.h`, derived from both sides — the consumer reads the bits, and the
producer `getBeaconMsgFromWLBSSInfo+0x1ef..0x27b` writes them straight out of Broadcom's
`wl_bss_info` flag byte, whose names identify them. The ones that bite:

- **bit 14 gates the RSSI.** `+0xe4` does `bt edx, 0xe` and branches clean past the store when it
  is clear, so a perfectly correct RSSI at `0x30` is dropped and every BSS reports 0 dBm. The
  inverse is republished as an explicit "RSSI invalid" byte at beacon`+0x2d5`. Apple derives it
  from `!(flags & WL_BSS_FLAGS_RSSI_INVALID)`. **This cost a full boot cycle to notice**, because
  nothing fails — scan results appear, correctly named, with 0 dBm.
- bit 6 is "RSSI measured on the BSS's own channel", and is also the `bool` argument to
  `isNewBssBetter(rssi, onchannel)`. Set it or the family distrusts a good reading.
- bits 1 and 2 together reach `setSSID` from `setBeaconDataFromMsg` (`+0xa6` computes `~flags & 6`).
  We send both; **Apple sends bit 1 only** and never takes that call. See the divergence note in
  `BeaconMetaData.h` — the short version is that Apple's other `setSSID`, at `+0x271`, is gated on
  `ie_len == 0`, so it exists for the no-IE case and implies a BSS with IEs is named from its SSID
  IE; but no such parse exists anywhere in the kernel, `setSSID` has zero direct callers, and ours
  do have names. Keep bit 2 until something proves the IE route works for us.
- bit 0 skips the `isNewBssBetter` comparison (`+0xc1`). Apple never sets it — every flag update in
  the producer ANDs with a mask ending `0xfa`, clearing bits 0 and 2 at each step. Leave clear: it
  defeats the family's own best-BSS selection.
- bits 12 and 13 claim `noise`/`snr` at `0x34`/`0x36`. Left clear: net80211's `rxi` carries
  neither, and a zero presented as valid is worse than an absent field.

**There is no timestamp field.** `setBeaconDataFromMsg+0xf8` calls `mach_continuous_time()` itself,
so ageing is the family's job, not ours. That removes the field I had assumed was missing and
blocking.

**`AppleChannelSpec_t` recovered.** `WCLDeviceConfiguration::fill20MHzChanSpec` is literally
`(band << 14) | 0x1000 | channel`:

- bits 0–7: channel number
- bits 11–13: width index into `{ 5, 10, 20, 40, 80, 160, 80, 160 }` MHz — **2 means 20 MHz**
- bits 14–15: band — **0 = 2.4 GHz, 1 = 6 GHz, 3 = 5 GHz**

Verified numerically, not by eye: Apple's own `ChanSpecGetPrimaryChannel` and
`ChanSpecConvToApple80211Channel` were transcribed from the disassembly and the encoder
round-tripped through them for channels 1/6/11/13/36/40/100/144/165 — every channel recovered
exactly, 2.4 GHz yielding flags `0x8`, 5 GHz `0x10`, and DFS `0x100` appearing on 100 and 144 but
not on 36/40/165. Only 20 MHz is emitted; anything wider needs the centre-channel table those two
functions use. `0x27` remains unidentified — a single byte next to the spec, sent as 0.

#### Scan results are implemented: the beacon push

Wiring, all Tahoe-only:

- `include/Airport/BeaconMetaData.h` — the struct with `_Static_assert`s pinning `sizeof == 0x44`
  and the offsets of `primary_chan`, `bssid`, `rssi`, `noise`, `snr`, `bintval`, `capinfo` and
  `flags`, so a mis-edit fails the build instead of the machine. Also `APPLE80211_M_BSS_BEACON`
  (201), `BEACON_META_MAX_IE_LEN` (`0x800`, Apple's own cap), the `BEACON_META_FLAG_*` bits and
  `AppleChanSpec20MHz()`. The flag-bit commentary there is the authoritative version; the table
  above summarises it.
- `IEEE80211_EVT_SCAN_BEACON` plus `struct ieee80211_beacon_event` in
  `itl80211/openbsd/net80211/ieee80211_var.h`, raised by `ieee80211_notify_scan_beacon` from
  `ieee80211_recv_mgmt`'s BEACON and PROBE_RESP cases. One site, so every HAL is covered, and it
  reuses the existing `ic_event_handler` rather than adding a second driver hook. **Gated on
  scanning** (`ic_state == IEEE80211_S_SCAN || IEEE80211_F_BGSCAN`): once associated, beacons
  arrive continuously and the only consumer is scan reporting.
- `AirportItlwm::postScanBeacon` builds the payload and posts it. The IE list starts at
  `sizeof(ieee80211_frame) + 12` — the fixed body is timestamp[8], interval[2], capability[2] — and
  the SSID is read from the first IE (id 0) with **both** SSID flag bits set. RSSI uses the same
  `rssi + IWM_MIN_DBM` conversion as `getRSSI`.

The deferral ring entry grew from 8 bytes to `0x44 + 512`, because a scan result cannot be posted
inline — it arrives on the work-loop thread — and cannot point at the receive mbuf, which is gone
by the time the `thread_call` drains. `ItlwmScanBeacons` counts pushes; zero while
`ItlwmScanReqStarted` climbs means the net80211 hook is not firing.

Known-incomplete, deliberately, and tracked as **mechanisms 13, 14 and 15** in the root
`AGENTS.md` so none of it reads as finished: scan completion is a 100 ms timer rather than the
`IEEE80211_EVT_SCAN_DONE` net80211 already raises (13); the request's channel subset, SSID filter
and dwell times are ignored (14); and the whole WCL association path — `setWCL_ASSOCIATE` and
friends — is still stubbed, so nothing can connect even once scanning works end to end (15).

#### The join FSM's actual first gate: a low-latency *getter*

Measured on 26.6 by picking a network in the menu. `setWCL_ASSOCIATE` was never reached:

```text
FSM JOIN_MANAGER: IDLE -> JOIN_REQ -> IN_PROGRESS
initWCLJoinRequest@717: lowerAuth = AUTHTYPE_OPEN, upperAuth = AUTHTYPE_WPA2_PSK, key = CIPHER_PMK
cmdIouc@145: Fail to Get cmd=<APPLE80211_IOC_WCL_LOW_LATENCY_INFO, 427> res=<0xe00002c7>
handleJoinRequest@1199: WCLJoinManager unable to get low latency traffic stats retVal -536870201
FSM JOIN_MANAGER: IN_PROGRESS -> JOIN_REQ_FAILED -> IDLE
Exit-setASSOCIATE:153 ret:-536870201
```

`getWCL_LOW_LATENCY_INFO` is now implemented, not stubbed — all-zero plus success, which is what
`AppleBCMWLANCore`'s own getter returns whenever `core[0x128]->[0x2c48]` is NULL, permanently our
case. The contract is at the struct definition in `include/Airport/IO80211InfraProtocol.h`.

Two things worth carrying forward from this:

- **A stubbed getter with no obvious relevance can be a fatal gate.** "Low latency traffic stats"
  has nothing to do with joining, and it stopped every association. Read the log for the first
  `kIOReturnUnsupported` the FSM sees before suspecting the setter you were about to write.
- Failures that are logged but **not** fatal look identical in the log. `WCL_SET_SCAN_HOME_AWAY_TIME`
  (446) and the `sendDrbgEntropy` RNG_EVENT_JOIN call both fail loudly every time and neither blocks
  anything. Severity is not visible in the message; only the following FSM transition tells you.

#### Gate after that: `SET_MAC_ADDRESS`, which the driver cannot intercept

With 427 answered, the FSM advances and dies further along. Measured on 26.6:

```text
[IOC DEBUG] INTERNAL: Get <APPLE80211_IOC_WCL_LOW_LATENCY_INFO>  res=<GOOD:0:0x0>   <- fixed
[wcl] getJoinCandidatesList@92: upper_auth=0x8, allow_auth=0x4
[wcl] ..block_invoke@149: BSSID AC:15:A2:F4:E8:8E Channel 44 RSSI -55 bandWidth 2, band 3
[wcl] ..block_invoke@149: BSSID AC:15:A2:F4:E8:8F Channel  7 RSSI -51 bandWidth 2, band 0
[ik]  setMacAddress@273: role<Infrastructure> client=<2> fail to set mac addr 402883984
[IOC DEBUG] INTERNAL: Set <APPLE80211_IOC_SET_MAC_ADDRESS> res=<FAIL:402883984:0x18038590>
[wcl] handleJoinRequest@1241: unable to set mac addre -> JOIN_REQ_FAILED
```

**Confirmation worth keeping:** those two candidate lines are the scan path being consumed by the
join path. Correct BSSIDs, `Channel 44 band 3` / `Channel 7 band 0`, `bandWidth 2` (the 20 MHz width
index this driver emits) and **real RSSI**, which means the `BEACON_META_FLAG_RSSI_VALID` fix reaches
BSS selection and not merely the menu bar. `upper_auth=0x8` also independently confirms
candidates`+0x14` is the `APPLE80211_AUTHTYPE_*` word.

**`setSET_MAC_ADDRESS` is not ours to implement.** `apple80211setSET_MAC_ADDRESS` checks slot 409
`isCommandProhibited(0x170)`, then `safeMetaCast`s and `jmp`s **non-virtually** straight to
`IO80211SkywalkInterface::setSET_MAC_ADDRESS`, which reaches
`IO80211MacAddressAgent::setMacAddress(addr, 2, false)` via `interface[0x120]->[0x50]`. Our override
is unreachable dead code, so implementing it would change nothing. The tell was in the return value:
`0x18038590` is not a valid `IOReturn` and is none of the constants on that path (`0xe00002c2`,
`0xe00002bc`, `0xe00002e2`), and our stubs return `kIOReturnUnsupported` — so the driver was never
in the path at all. See the rule now recorded in `include/Airport/AGENTS.md`.

The agent is not missing: the class census reports `IO80211MacAddressAgent = 1`, so it exists, is
called, and rejects. **The rejected value comes back out of our own vtable.** Traced inside
`IO80211MacAddressAgent::setMacAddress`:

```text
+0x0d/+0x87  two safeMetaCasts of the interface:
               [rbp-0x88] <- IO80211InfraInterface     (gMetaClass @0xffffff800248d890)
               rbx        <- IO80211VirtualInterface   (gMetaClass @0xffffff800248d478)
+0x238  rdi = [rbp-0x88]; rax = [rdi]      <- dereferenced with NO null check
+0x246  call [rax + 0xea0]                 <- slot 452 on IO80211InfraInterface
+0x24c  r12d = eax
   ...alternative branch...
+0x255  test rbx, rbx ; je +0x466          <- +0x466 sets up and falls into _panic
+0x268  call [rax + 0xf40]                 <- slot 488 on IO80211VirtualInterface
+0x26e  r12d = eax
+0x271  test r12d, r12d ; jne -> logs "fail to set mac addr %d" and returns it
```

**Root cause: slot 452 was mis-bound. This is mechanism 3, caught by evidence for the first time.**

The chain, established entirely from the binary:

- The logger is `IO80211MacAddressAgent::setMacAddress+0x3bf` (`ecx = 0x111` = 273, matching
  `setMacAddress@273`), and the logged `%d` is `r12d` (`push r12` at `+0x3df`).
- `r12d` reaches the log only through `+0x274`, so it is the return of one of two virtual calls.
- The branch is chosen at `+0x101` by `cmp qword [rbp-0x88], 0` — whether the interface casts to
  `IO80211InfraInterface`. Ours does, so we take `+0x246`, **slot 452**. (The other branch is
  impossible for us anyway: it panics at `+0x466` on a NULL `IO80211VirtualInterface` cast, the
  census reports `IO80211VirtualInterface = 0`, and that class's slot 488 is `___cxa_pure_virtual`.)
- Slot 452's correct occupant, `IO80211SkywalkInterface::getLastRxUnicastLinkActivityTime`, is
  `xor eax,eax; ret` — it *always* returns 0, i.e. success. We saw a constant `0x18038590`.
  **A slot whose correct occupant cannot fail, failing, is a mis-binding.**
- `IO80211PeerManager::getLastRxUnicastLinkActivityTime` is the candidate: same method name, unrelated
  class, and it reads `[this+0x18]` then `[+0x558]`/`[+0x560]` and tail-jumps through slot 452 of
  whatever that yields — against our object, an arbitrary but reproducible value, which is exactly
  the observed "constant across two attempts and two boots".

**The slot was pinned and it did NOT fix this.** `AirportItlwmSkywalkInterface` now overrides
`getLastRxUnicastLinkActivityTime` returning 0, and `mapdrv.py` went 209 → **210** correct overrides
with 0 wrong, so the override provably occupies Tahoe's slot 452. The join still fails at the same
place. Keep the pin — it is correct, cheap, and closes a real mechanism-3 hole — but it is not the
cause here. (It must stay inside `#if __IO80211_TARGET >= __MAC_26_0`; the slot is Tahoe-only and an
unguarded override fails the Sonoma targets outright.)

**What the pin bought instead was the decisive clue: the value is a truncated pointer.**

| when | value |
| --- | --- |
| two boots before the pin | `0x18038590` |
| after the pin | `0x0f038590` |

Identical low 24 bits, different high byte — the signature of a kernel pointer whose slide changed
across boots, *not* of an error code. Since our pinned override returns literal `0`, the failing call
cannot be reaching it, so slot 452 is **exonerated** and the earlier branch reasoning was wrong
somewhere. The value being pointer-shaped also explains why it looked like a stable constant across
two boots that happened to share a slide.

**The contradiction is now sharp enough to be worth stating, because it means a premise is false.**
The hierarchy is `AirportItlwmSkywalkInterface : IO80211InfraProtocol : IO80211InfraInterface :
IO80211SkywalkInterface`, so the `IO80211InfraInterface` cast at `+0x0d` *must* succeed, the
`+0x114` branch is *not* taken, slot 452 *is* the call, and our override returns 0 — yet the join
fails. Every step of that is independently checked, so at least one of them is wrong, and further
static reasoning on the same premises is not going to find out which.

*Resolved by measurement instead:* `ItlwmLastRxActivityCalls` counts entries to the pinned override
(`gItlwmLastRxActivityCalls`, published by `publishRuntimeCounters`). Read it after a join attempt:

- **climbing** — the slot is genuinely ours, so the error-checked value is not coming from slot 452
  and the branch analysis is wrong about *which call* fails. Re-read `setMacAddress` for a third
  route to the log at `+0x393`.
- **zero while a join is attempted** — the call never reaches this interface, so the object the agent
  holds is not ours, and the whole cast/branch analysis is about the wrong object.

Either answer eliminates half the search space for the cost of a boot, which no amount of
disassembly was going to do.

**RESOLVED, and confirmed on 26.6.** `SET_MAC_ADDRESS` now answers `GOOD:0:0x0`, `JOIN_MANAGER`
advances through `JOIN_MANAGER_EVENT_TRY_NEXT_CANDIDATE` into `handleSendCandidateToDriver@1306`, and
the join now fails at `APPLE80211_IOC_WCL_ASSOCIATE` (442) — our own stub — which is mechanism 15
proper. Final proof of the diagnosis came from three boots' worth of the bad value:
`0x18038590`, `0x0f038590`, `0x0a038590` — identical low 24 bits with the high byte changing every
boot, i.e. a leftover pointer tracking the KASLR slide.

**ROOT CAUSE: slot 468, a `void` placeholder whose undefined return value was error-checked.**

`IO80211MacAddressAgent::setMacAddress` calls `[rax + 0xea0]`, and **0xea0 / 8 == 468, not 452** — an
arithmetic slip that sent this investigation through slot 452, two useless pins, three boots and two
retracted conclusions. 452 × 8 is 0xE20, which is the offset `IO80211PeerManager` tail-jumps through
and where the same arithmetic was done correctly. **Convert vtable byte offsets with a calculator, and
sanity-check by converting back.**

Slot 468 was `virtual void _RESERVEDIO80211InfraInterface0(void) {}` in
`include/Airport/IO80211InfraInterface.h` — a placeholder occupying an unnamed pure-virtual slot that
neither Apple's shipping class nor `AppleBCMWLANInfraProtocol` overrides. The family calls it anyway
and treats a non-zero result as fatal. Because the placeholder returned `void`, `%eax` held whatever
the previous code left there, so the family read a **leftover pointer as a status**: hence a large
value, identical across attempts within a boot and changing its high byte between boots with the
KASLR slide. Now returns `kIOReturnSuccess`.

**The general rule this earns: a placeholder for an unnamed slot must still return a plausible
status, not `void`.** An empty `void` body is not neutral — it leaves the return register undefined,
and any caller that error-checks it sees garbage. The root `AGENTS.md` already warned that a struct's
flag word can gate data; this is the same class of bug in a *register*.

*Superseded diagnostics, retained only so they are not repeated:* the earlier measurement
`ItlwmLastRxActivityCalls = 0` across 8 join attempts was correct and correctly interpreted — slot
452 genuinely is never called — but it was answering a question that only existed because of the
arithmetic error. The neighbour counters `ItlwmSlot450Calls`/`ItlwmSlot451Calls` reading 0 confirmed
the same thing. All three, and the slot 452 pin, can be deleted with the rest of the bring-up
instrumentation (mechanism 9); the pin is harmless and `mapdrv`-correct, so it may as well stay until
then.

That leaves one explanation consistent with every other check, and it is the ugly one: the override
sits at compiled slot 452 — `mapdrv.py` says 210 correct / 0 wrong — but the **loaded** vtable does
not dispatch to it. This is mechanism 3's documented shape, *correct on disk and wrong in memory*,
which `mapdrv.py` structurally cannot detect because it reads the built kext, not the loaded image.
It also explains the pointer-shaped return: whatever the loader bound into that slot returns
something pointer-like, and the high byte tracked the KASLR slide across boots.

**The consequence for mechanism 3 is significant: pinning a declaration does not defeat a loader
mis-binding.** The entry's stated "real fix" of "pin every duplicate-named slot" is therefore not
sufficient on its own — slot 452 is now pinned, verified by name, and still not reached. Understanding
the loader's binding rule moves from optional to required.

*Two dead hypotheses, recorded so they are not re-run:*

- *The agent holds a family-created interface rather than ours.* Withdrawn. The census reading
  `AirportItlwmSkywalkInterface = IO80211InfraProtocol = IO80211InfraInterface =
  IO80211SkywalkInterface = 1` does **not** mean four objects: in xnu each class constructor calls
  its own metaclass's `instanceConstructed`, so a single derived object increments every ancestor's
  counter. Do not read an ancestor count as a separate instance — this misreading also sent the
  earlier `IO80211VirtualInterface = 0` argument astray.
- *Slot 452 is the wrong slot.* Excluded: the hierarchy makes the `IO80211InfraInterface` cast
  succeed, so the `+0x114` branch cannot be taken.

*Constraint that narrows it: the mis-dispatch is targeted, not wholesale.* Other overrides on the
same object demonstrably run — `getWCL_LOW_LATENCY_INFO` answers `GOOD` and `setWCL_SCAN_REQ` is
entered (29 times in one boot). So the vtable is being dispatched correctly in the WCL slot range
(~500-640) while slot 452, in the `IO80211SkywalkInterface` data-path block, is not reached. Whatever
the cause, it affects a region rather than the table as a whole.

*External patching: Lilu is ruled out.* This machine boots OpenCore with **Lilu 1.7.2 and 15 plugins**
(`kextstat | grep -v com.apple`), which made in-memory patching a natural suspect: Lilu routing can
rewrite a vtable while the on-disk kext stays correct, exactly the observed shape. Tested with a
debug Lilu build, which writes `/var/log/Lilu_<ver>_<build>.txt`:

- the log is a genuine record, not a stub — 2760 lines, `debug 1`, 279 patch/route lines, and it
  enumerates the kexts it processed (graphics, audio, SMC, the plugins' actual targets);
- **`com.apple.iokit.IO80211Family` appears zero times**, as do `80211`, `itlwm` and `airport`.

Lilu only processes kexts a plugin has registered interest in, so no mention means no plugin asked
for `IO80211Family` and Lilu never touched it. Do not re-test this without a reason.

Note the earlier reasoning error worth avoiding: Lilu logs **nothing** in the unified log by default,
so its silence there was not evidence of inaction — the on-disk debug log is the only valid check.

*OpenCore is ruled out too:* `Kernel -> Patch` and `Kernel -> Block` contain no enabled entries.
**External patching is therefore excluded entirely** — the cause is internal to this driver's
reconstruction or to OSKext's own vtable binding. Do not re-open the Lilu/OpenCore line without new
evidence.

*Alignment is exact, which deepens rather than solves it.* Real slot = clang dump index − 2 (the dump
counts `offset_to_top` and RTTI). On that basis our table and Tahoe's agree name-for-name across the
whole neighbourhood — 445 `getRingMD`, 449 `getDataPathInterfaceStats`, 450 `getDataPathPeerStats`,
451 `getLastQueuePacketTime`, 452 `getLastRxUnicastLinkActivityTime`. So slot 452 is at the right
index and still is not reached.

*Experiment now in the build:* counters on the two neighbours, published as `ItlwmSlot450Calls`
(`getDataPathPeerStats`) and `ItlwmSlot451Calls` (`getLastQueuePacketTime`), both returning exactly
what Apple's own implementations return so behaviour is unchanged. After a join attempt:

- **a neighbour fires** — the loaded table is displaced by a knowable amount, and which counter moved
  names the offset;
- **none fires and 452 stays 0** — the call never reaches this object, so the agent holds a different
  interface and the cast/branch reading must be redone against whatever `agent[0x10]` really points
  at. `getRingMD` at 445 is worth noting here: it is the only pointer-returning method in the block,
  and the failing value is pointer-shaped.

Method notes worth keeping:

- **Self-test a string cross-reference scan before believing a zero.** A first pass found no
  reference from any `MacAddressAgent` method and briefly "proved" the wrong conclusion; the fault
  was a hand-rolled file-offset→VA mapping. The real mapping is `VA = fileoff + 0xffffff8000100000`,
  and the reference is at `MacAddressAgent::setMacAddress+0x3bf`. Anchor the scan on `48 8d`
  (REX.W `lea`) with `(modrm & 0xC7) == 0x05`, and **reference a format string by its start**, not by
  a `grep` hit inside it — here the start was 32 bytes earlier than the matched text.
- **A return value can convict a slot.** Every stub in this driver returns `kIOReturnUnsupported`
  (`0xe00002c7`); the correct occupant of 452 returns `0`; the agent's own error constants are
  `0xe00002c2`/`0xe00002bc`/`0xe00002e2`. `0x18038590` being none of these is what localised the bug
  without a boot.

**Two things this cost, recorded so they are not repeated:**

- The clang `-fdump-vtable-layouts` indices are **not** vtable slot numbers. The dump counts
  `offset_to_top`, RTTI and `vcall_offset` pseudo-entries, so `AirportItlwmSkywalkInterface` shows
  671 entries where the real table has 668. `mapdrv.py` strips them before aligning, which is
  exactly why it must be trusted over an ad-hoc comparison: reading raw dump indices produced a
  convincing but entirely false "+2 slot misalignment" here. Align with `mapdrv.py`/`ownslots.py`,
  never by eyeballing the dump.
- A return value can exonerate a slot. Apple's implementations of the two candidates return `0` and
  `kIOReturnUnsupported` (`0xe00002c7`); the observed `0x18038590` is neither, which is what
  narrowed this down without a single boot.

*Next:* boot the pin and see how much further `JOIN_MANAGER` gets. `setWCL_ASSOCIATE` is still
stubbed, so the expected outcome is that the MAC set now succeeds and the FSM fails at the associate
call instead — which is also the first chance to learn which completion message it waits on.

Also recovered from that one log line: `upperAuth = AUTHTYPE_WPA2_PSK` is `1 << 3` == 8, and
`AppleBCMWLANJoinAdapter::adjustMfp` switches on candidates`+0x14` with exactly the
`APPLE80211_AUTHTYPE_*` bit values — 8/0x10/0x400 (WPA2-PSK, FT-PSK, SHA256-PSK) → MFP optional,
0x1000/0x2000 (SAE, FT-SAE) → MFP required unless a flag bit says otherwise, 0x4000/0x8000
(WPA3 enterprise) → MFP required unconditionally. That is textbook WPA3 policy, so `+0x14` is the
**upper auth type** and candidates`+0x213` bit 6 downgrades MFP from required to merely capable
(`cmp al,1 / adc ecx,1` yields 2 when clear, 1 when set). Both were `_unkNN`; mechanism 16 shrinks
by two.

#### The association request, recovered

The layout `setWCL_ASSOCIATE` reads, in `include/Airport/AssocCandidates.h` with
`_Static_assert`s on every offset below. The setter is implemented; see "Implemented" further
down for what it does with these fields.

Entry point chain, ending at the implementation that is actually *bound*:

```text
slot 602  AppleBCMWLANInfraProtocol::setWCL_ASSOCIATE(apple80211AssocCandidates *)
            loads [this+0x130], derefs, tail-jumps that object's slot 540
AppleBCMWLANCore::setWCL_ASSOCIATE          <- the real work
            -> AppleBCMWLANJoinAdapter::performJoin(apple80211AssocCandidates *)
```

Layout authority is the producer pair `WCLJoinRequest::fillAssocCandidatesList` (request-wide
fields) and `WCLJoinRequest::addAssocCandidates` (one entry per BSS). Fields we will need:

| off | field | evidence |
| --- | --- | --- |
| 0x04 | version, always `0x78` | `fill+0x84` stores the literal |
| 0x08 | `apple80211_battery_save_modes` | the function's own third argument |
| 0x1c | SSID length | precedes `ssid`, mirroring `apple80211_ssid` |
| 0x20 | SSID, 32 bytes | `fill+0x1c1` four qwords out of the join request |
| 0xd4 | RSN IE length | `fill+0x196` ← `WCLJoinRequest::trimRsnIeLen()` |
| 0xd6 | RSN IE, 0x101 bytes | `fill+0x1f3` `memmove` |
| 0x1ec/0x1f0 | OWE transition SSID + length | same length-then-SSID shape |
| 0x214 | candidate count | zeroed at `fill+0x1a`, incremented per `addAssocCandidates` |
| 0x218 | candidate array, 10 × 18 bytes | see below |
| 0x2cc/0x2ce | vendor IEs + length | `WCLJoinManager::getVendorSpeificIes` |

**The array bound is derived, not assumed.** `addAssocCandidates` computes its slot as
`base + 18 * count` with `base = 0x218`, and the next field the producer touches is `0x2cc`.
`0x218 + 10 * 18 == 0x2cc` exactly, so the array is `[10]` and both the stride and the base are
confirmed by the same arithmetic.

Each entry is filled from a virtual on the `IO80211BSSBeacon` the WCL chose, which is what names
the fields — slots 112, 87, 46, 50, 62 in call order, resolved against a `vtdump.py` of
`IO80211BSSBeacon` (the slot reference only covers the two `AppleBCMWLAN*` classes):

| off | field | source |
| --- | --- | --- |
| 0x00 | u8 SAE-PK capable | `isBSSSAEPKCapable()` |
| 0x02 | u16 encryption mode | `getEncryptionMode()` |
| 0x04 | u8[6] **BSSID** | `getAddress(uint8 *)` |
| 0x0a | u8[6] OWE transition BSSID | `getOWETransAddress(uint8 *)` |
| 0x10 | u16 **chanspec** | `getChanPrimarySWSpec()` — same `AppleChannelSpec_t` we emit for scans |

So a join is: pick a candidate, and its BSSID and channel are already in hand in the encoding this
driver produces, alongside the SSID, the encryption mode and the RSN IE the AP requires.

### Every WCL manager's message subscriptions are in one static table — read it, don't hunt

This is the general tool, and it retroactively answers how message 237 should have been found. Each
`WCL*Manager::initWCL*Manager` fills a `WCLFsmManagerOptions` and passes it to
`WCLFsmManager::initWithOptions`. The `lea rax, [rip + …]` immediately before that call points into
one contiguous static descriptor — **at the subscription table**, with the name arrays laid out just
*before* it, which is the part that misleads if you scan forward from the lea target:

```text
  state-handler function pointers  (pairs, 16 bytes each)
  state-name  char*[]              ("<MGR>_STATE_IDLE" first — state 0)
  event-name  char*[]              ("<MGR>_EVENT_…"    first — event 0)
  char*  manager name              ("JOIN_MANAGER")
  0
  subscription table               24-byte entries, terminated by an all-zero entry
```

A subscription entry is `{ u16 msgType; u16 msgnum; u32 pad; handler_fn; 0 }`. The handler pointer is
a chained fixup, so its stored value is a **file offset**: recover the VA with the repo's standard
`VA = fileoff + 0xffffff8000100000`. `msgType` comes straight out of
`WCLGlue::receiveMessageInternal` (`0xffffff800218a03c`), which packs `(msgnum << 16) | msgType` and
copies the payload through unchanged:

| msgType | meaning |
| --- | --- |
| 0 | `apple80211` **GET** ioctl |
| 1 | `apple80211` **SET** ioctl |
| 2 | **message posted by the driver** (`postMessage`) |
| 3 | WCL-internal notification (sleep/wake/reset/…) |
| 4 | msgnum `0x17f` only |

**The message number is not translated.** The earlier note here claimed `WCLBulletinBoardMsgIndex`
was an internal index requiring a conversion — that was wrong, and it is why the scan hunt went the
long way round. `receiveMessageInternal` shifts the driver's number into the entry verbatim, so the
subscription table is directly greppable for the number to post.

Self-test that proves the decoding rather than assuming it: `WCLScanManager`'s table
(`0xffffff8002486b10`) begins with `type=2 num=237 → WCLScanManager::scanDoneEventHandler`. 237 is the
message that was already established by other means, so the encoding is confirmed against a known
answer before being trusted for the join.

**`scripts/abi/wclfsm.py <WCLManagerClass>` does all of the above in one command** — states, events
and subscriptions — so none of this has to be re-derived per manager. Two things it has to handle,
which are also the traps when reading the bytes by hand: the tables of adjacent managers sit **back to
back in `__const` with no terminator between them** (the entry count lives in the options struct, not
in the data), so `WCLScanManager`'s runs straight on into `WCLConfigManager`'s unless the boundary is
drawn where the handler's owning class changes; and the name arrays **precede** the table rather than
following it, with states before events.

`CommonFsmManager::processEvent` is the authority on how the transition matrix is indexed:
`entry = table[curState * options[0x29] + event]`, each entry `{ u8 nextState, u8 handlerIndex }`,
with `nextState == 0xff` meaning "stay". It also settles that the payload pointer is passed to the
handler **unchanged**, so a handler reading `[arg + 0xc]` is reading the driver's own message at
offset 0xc — which is how 216's layout was pinned.

**The matrix is 16-byte aligned and the handler array after it is too, so managers whose
`nst * nev * 2` is not a multiple of 16 have padding between them.** `wclfsm.py` locates the table
by walking backwards from the handler array and now rounds down to the boundary; before that fix
`WCLNetManager` (154 bytes) printed a table rotated by three entries, with every handler attached to
the wrong event and no internal inconsistency to give it away. `WCLScanManager` (80) and
`WCLJoinManager` (192) are multiples of 16 and were never affected, which is exactly why the
self-test above kept passing. **Check a decoded matrix against a live transition from the WCL log
before acting on it.**

### RESOLVED — the JOIN_MANAGER completion messages are 211 and 213

`WCLJoinManager`'s subscription table is at **`0xffffff8002487dd0`** (21 entries). The driver-posted
ones (`msgType 2`) are the whole answer to what a join must raise:

| msgnum | handler | FSM event raised |
| --- | --- | --- |
| **211** (0xd3) | `authAssocCompleteEventHandler` | 4 = `JOIN_MANAGER_EVENT_JOIN_ASSOC_COMPLETE` |
| **212** (0xd4) | `firstBeaconEventHandler` | — |
| **213** (0xd5) | `connectCompleteEventHandler` | 5 = `JOIN_MANAGER_EVENT_JOIN_CONNECT_COMPLETE` |
| **214** (0xd6) | `joinAbortHandler` | — |
| 5, 6 | `micErrorHandler` | — |

Both completion handlers **validate the payload length and silently fail if it is wrong**, which is
the trap to avoid: each sets `msg[0x28] = 1`, then requires a non-NULL payload pointer *and* an exact
length, and only then tail-calls `CommonFsmManager::processEvent`. A wrong length returns
`kIOReturnError` (`0xe00002bc`) and raises no event at all — the FSM would sit exactly as it does
today, so a length mistake is indistinguishable from not posting the message.

| msgnum | required length | payload struct (named by the callee) |
| --- | --- | --- |
| 211 | **0x1c** (28) | `apple80211_assoc_event` — `WCLJoinRequest::updateAuthAssocStatus` |
| 213 | **0xa4** (164) | `apple80211_connection_complete_event` — `WCLJoinRequest::updateConnectCompleteEvent` |

The event numbers are verified, not inferred from string order: the state-name and event-name arrays
sit adjacent in the descriptor at `0xffffff8002487d10`, states from `JOIN_MANAGER_STATE_IDLE` (0), and
the event array starts at `0xffffff8002487d40`, putting `JOIN_ASSOC_COMPLETE` at 4 and
`JOIN_CONNECT_COMPLETE` at 5 — exactly the literals the two handlers pass.

Join FSM states: `IDLE`(0), `IN_PROGRESS`(1), `ASSOC_DONE`(2), `CONNECT_COMPLETE`(3), `HALTED`(4),
`ABORTED`(5). Events: `JOIN_REQ`(0), `JOIN_REQ_FAILED`(1), `JOIN_ABORT_REQ`(2),
`JOIN_ABORT_COMPLETE`(3), `JOIN_ASSOC_COMPLETE`(4), `JOIN_CONNECT_COMPLETE`(5), `JOIN_COMPLETE`(6),
`DRIVER_RESET`(7), `HALT`(8), `RESUME`(9), `TIMEOUT`(10), `SYSTEM_POWER_OFF`(11),
`SYSTEM_POWER_ON`(12), `TRY_NEXT_CANDIDATE`(13), `TRY_NEXT_CANDIDATE_DELAYED`(14),
`JOIN_SCAN_UPDATE`(15).

**The transition matrix decides more than the message numbers do**, and `wclfsm.py` now prints it
(`scripts/abi/wclfsm.py WCLJoinManager`). The rows that constrain this driver:

```text
IDLE              JOIN_REQ              -> IN_PROGRESS       handleJoinRequest
IN_PROGRESS       JOIN_ASSOC_COMPLETE   -> ASSOC_DONE        handleJoinAssocComplete
IN_PROGRESS       JOIN_CONNECT_COMPLETE -> CONNECT_COMPLETE  handleJoinConnectComplete
IN_PROGRESS       TRY_NEXT_CANDIDATE    -> IN_PROGRESS       handleSendCandidateToDriver
ASSOC_DONE        JOIN_CONNECT_COMPLETE -> CONNECT_COMPLETE  handleJoinConnectComplete
CONNECT_COMPLETE  TRY_NEXT_CANDIDATE    -> IN_PROGRESS       handleSendCandidateToDriver
CONNECT_COMPLETE  JOIN_COMPLETE         -> IDLE              handleJoinComplete
```

Three consequences, all load-bearing in the implementation:

- `IN_PROGRESS` accepts `JOIN_CONNECT_COMPLETE` **directly**, so a join that fails before it ever
  associates can still be reported with 213 alone. There is no need to fabricate a 211 first.
- `CONNECT_COMPLETE` has **no** `JOIN_ASSOC_COMPLETE` entry, so 211 must precede 213 and never
  follow it. `postMessageSafe`'s ring preserves the order they are queued in.
- `IDLE` accepts neither, so either message posted outside a join is inert.

**Failure must be reported as a non-zero connect status, never a non-zero assoc status.**
`handleJoinAssocComplete` raises no FSM event for a non-zero status other than the literal 1000
(which becomes `JOIN_ABORT_REQ`): it calls `updateAuthAssocStatus`, optionally logs through
`debugCCOnAuthFailures`, and returns. So message 211 is a dead end for reporting failure —
posting it with a bad status is exactly as effective as posting nothing.

**How far the candidate walk goes: four attempts.**
`WCLJoinRequest::updateAndCheckForNextCandidate` pulls the next `WCLJoinCandidate` off an
`IO80211Queue` into `joinRequest[0x10]+0x20`, increments the attempt counter at `+4`, and gives up
once it reaches 4. `handleSendCandidateToDriver` then rebuilds the whole `apple80211_assoc_candidates`
from that new current candidate — which is why `candidates[0]` is *the* BSS to try rather than
merely the first of a set, and why pinning it in net80211 does not prevent the walk.

`JOIN_COMPLETE` (6) is **not** driver-posted: `handleJoinConnectComplete` raises it itself after
`isJoinProcessDone`, then calls `sendWCLJoinDone`. So the driver owes 211 and 213 only.

One dead end kept, because it looks like a dispatch table and is not: the 16-byte-entry table at
`~0xffffff80036c9e88` holding handler addresses is a symbolication name map — its entries are sorted
by mangled-name length and its second word is a string offset.

**Both payload structs are decoded, in `include/Airport/JoinCompleteEvents.h`.** They are the
mirror image of `AssocCandidates.h`: Apple writes the candidates and we read them, but *we* write
these, so a wrong field is one the family acts on. Both are **packed** — the 64-bit fields land at
`0x0c` and `0x14`, and neither total (`0x1c`, `0xa4`) is a multiple of 8 — so without the attribute
the compiler pads, offsets shift, and the exact-length check then rejects the message.

```text
apple80211_assoc_event                 (0x1c)   msg 211
  0x00 u16 status      0 == success; 1000 raises JOIN_ABORT_REQ    -> joinRequest+0x06
  0x02 u16 reason      status detail; -> debugCCOnAuthFailures
  0x04 u8  auth_phase  != 0 selects auth_time, == 0 selects assoc_time
  0x05 u8[6] bssid     unaligned by design; memcmp'd against the target BSS
  0x0c u64 auth_time   taken when auth_phase != 0
  0x14 u64 assoc_time  taken when auth_phase == 0

apple80211_connection_complete_event   (0xa4)   msg 213
  0x00 u16 status      0 == success, and that is the whole success condition
  0x02 u16 reason      -> joinRequest+0x08
  0x0c u64 timestamp   stored with a companion validity byte
  rest                 never read — but must be present, so zero it
```

The control flow that makes `status` the important field: `WCLJoinManager::isJoinProcessDone`
short-circuits to *done* when the connect event's status is zero, and `handleJoinConnectComplete`
then raises `JOIN_COMPLETE` (6) itself. A **non-zero** status is not a dead end — it calls
`WCLJoinRequest::updateAndCheckForNextCandidate` and only gives up if that returns negative or
35000 ms have elapsed since the join started; otherwise it raises `TRY_NEXT_CANDIDATE` (13), or
`TRY_NEXT_CANDIDATE_DELAYED` (14) carrying a 1000 ms delay. So reporting a real failure walks the
candidate list, which is the correct thing to do and better than silence.

That the trailing bytes are unread is verified, not assumed: `updateConnectCompleteEvent` is the only
consumer of the payload pointer, and `handleJoinConnectComplete` passes stack locals — not the
payload — to every subsequent `processEvent`.

### RESOLVED — the PMK already reaches the driver, through `setCIPHER_KEY`, and is thrown away

Not a missing route: an implemented one that declines. The chain, all verified on 26.6:

```text
userspace APPLE80211_IOC_CIPHER_KEY (3)
  -> WCLNetManager::setKey(bulletinBoardMessage&)        subscription: type=1 SET, num=3
  -> apple80211setCIPHER_KEY(IO80211SkywalkInterface *, apple80211_key *)
       isCommandProhibited(3), then safeMetaCast and `jmp [vtable + 0x1110]`  -> slot 546
  -> AirportItlwmSkywalkInterface::setCIPHER_KEY        AirportItlwmSkywalkInterface.cpp:694
       case APPLE80211_CIPHER_PMK: XYLog("Setting WPA PMK is not supported"); break;
```

**The dispatch is virtual here**, unlike `apple80211setSET_MAC_ADDRESS` — the cast is followed by an
indirect `jmp` through the vtable, not a direct call to the family's own implementation — so our
override genuinely runs and the log line above is reachable. That is the difference that made
`setSET_MAC_ADDRESS` unfixable at the driver layer and makes this one a two-line fix.

For WPA2/WPA3-PSK the PMK is exactly what net80211 wants in `ic_psk`: `ieee80211_pae_input.c:269`
copies `ic_psk` into `ni_pmk`, and the V1 path already does
`memcpy(ic->ic_psk, key, sizeof(ic->ic_psk)); ic->ic_flags |= IEEE80211_F_PSK;`
(`AirportItlwmSkywalkInterface.cpp:505`). `APPLE80211_CIPHER_MSK` and `APPLE80211_CIPHER_PMKSA` in the
same switch already call `ieee80211_pmksa_add`.

**Implemented**, with a length guard and `#if __IO80211_TARGET >= __MAC_26_0`. The guard is not
timidity: this file compiles into every V2 target, the WCL is Tahoe's stack, and older releases
associate through `setASSOCIATE` which sets `ic_psk` from the assoc data itself — so it is *not*
established that `CIPHER_PMK` ever arrives pre-Tahoe, and accepting it there would change releases
that work today on the strength of an assumption. Widen it only after observing an older release
send it.

### RESOLVED — what a failure return does: nothing useful

`handleSendCandidateToDriver` issues the associate as
`WCLFsmManager::cmdIouc(442, false, candidates, 0x6f8, NULL, 0)` — note it goes through `cmdIouc`,
not a direct virtual call on the interface. A non-zero return reaches **only**
`CCLogStream::logEmergency("Failed to send candidate to driver")`; no `processEvent` runs on that
path, and the function returns 0 either way. So an error from `setWCL_ASSOCIATE` does not fail the
join, it *stalls* it until the 35 s timeout — which is precisely the symptom this bring-up has been
looking at.

**So prefer reporting failure through message 213 with a non-zero status** over returning an error
from the setter: that path calls `updateAndCheckForNextCandidate` and walks the candidate list, which
is both faster and diagnosable.

That call site is also where the candidates struct's true size came from: `IOMallocZeroData(0x6f8)`,
the `0x6f8` length argument, and `IOFreeData(p, 0x6f8)`. `AssocCandidates.h` had it at `0x3dc` — the
last offset `fillAssocCandidatesList` writes — and was 796 bytes short, because
`WCLJoinManager::getVendorSpeificIes` is a **second producer** that writes a vendor IE run at `0x3e0`
and a product-info IE at `0x4e8`/`0x4ec`/`0x4ee` (via `IO80211_GetProductInfoIe`). Zero-allocated
buffers make that class of error self-consistent forever; the header now asserts `0x6f8`.

Fields whose meaning is not yet established are named `_unkNN` in the header and sized rather than
guessed at. **That incompleteness is tracked as mechanism 16, deliberately separate from 15**,
because 15 closes when an association completes and would otherwise bury it. `flags1e0`, `flags1e4`
and `flags1e8` are the sharp end: we know which bits Apple sets and not what any bit requires, and
`AppleBCMWLANJoinAdapter::adjustMfp` reads them, so at least one plausibly governs management-frame
protection. Since Apple writes this struct and we only read it, ignoring a field cannot corrupt
anything — the failure mode is that a requirement carried in an unread bit goes unhonoured and the
join comes up wrong instead of failing, which is the `BeaconMetaData` bit-14 shape over again.

### Implemented — `setWCL_ASSOCIATE` and the two completions

Slot 602 is no longer a stub, and neither is the completion side. **The setter is confirmed
reached and accepted on 26.6**; the association itself has not completed yet, and the first boot's
failure is written up under "First boot" below.

**The setter** — `AirportItlwmSkywalkInterface::setWCL_ASSOCIATE` → `beginAssociateGated`, under
`getWorkQueue()->runAction()` for the same reason `setWCL_SCAN_REQ` is gated: the WCL calls in on
its own thread and net80211 is otherwise touched only from the work loop. What it takes from
`apple80211_assoc_candidates`, and nothing else:

| field | where it goes |
| --- | --- |
| `ssid` / `ssid_len` | `ic_des_essid` — what `ieee80211_match_bss` selects on |
| `upper_auth_type` | the `APPLE80211_AUTHTYPE_*` bitmask `associateSSID` already understands |
| `candidates[0].bssid` | `ic_des_bssid` + `IEEE80211_F_DESBSSID` |

No key is read, because there is none in the struct: the PMK arrived earlier through
`setCIPHER_KEY(APPLE80211_CIPHER_PMK)` and is already in `ic_psk`. `associateSSID` now tolerates a
NULL `key` for exactly that caller — the pre-Tahoe one passes an array member and is unaffected.
The RSN IE is not read either: `setRSN_IE` is a `USE_APPLE_SUPPLICANT` feature and **Sonoma and
Tahoe do not define it** (checked in `project.pbxproj`; every target up to Ventura does), so
net80211 builds its own RSN IE from the parameters `associateSSID` sets.

Then, by state:

- `AUTH`/`ASSOC` — an exchange is already in flight; left to finish, as the pre-Tahoe
  `setASSOCIATE` does. The join is registered either way, so its outcome is still reported.
- `RUN` — associated elsewhere. Deauth the current BSS, then `ieee80211_new_state(SCAN)`, which
  frees the node cache and rescans; `ieee80211_end_scan` then selects on the new desired ESS.
- `SCAN` — the normal case. Nudges the state machine with `ieee80211_new_state(SCAN)`. **Not
  `ieee80211_end_scan`, which belongs to the HAL** — see "Third boot" below.
  `ieee80211_switch_ess`, which `end_scan` eventually calls, cannot clobber the programming:
  `ic_ess` is empty on this driver (which is *why* `IEEE80211_F_AUTO_JOIN` is set at start), so
  its loop finds no matching entry.

**The return value is deliberately not the failure channel.** `cmdIouc`'s non-zero result reaches
only `logEmergency("Failed to send candidate to driver")`; no FSM event is raised, so an error
here stalls the join for 35 s instead of failing it.

**The completions** — `AirportItlwm::postJoinAssocComplete` / `postJoinConnectComplete`, both
gated on `fJoinPending` so the driver's own roaming cannot move the FSM:

| trigger | message |
| --- | --- |
| `IEEE80211_EVT_STA_ASSOC_DONE` (raised only on a success status) | 211, status 0, `auth_phase` 0 |
| link up in `setLinkStateGated` | 213, status 0 |
| `IEEE80211_EVT_STA_DEAUTH` during a join | 213, status `IEEE80211_STATUS_UNSPECIFIED`, reason = `ic_deauth_reason` |
| `checkJoinProgress` from the watchdog | 213, non-zero |

Link-up is the right point for both kinds of network: net80211 raises it straight out of RUN for
an open BSS and only once the port is valid — after the four-way handshake — for an RSN one.
Link-*down* deliberately posts nothing: leaving one network for another drives RUN → SCAN with the
join already registered, so treating that as failure would abandon the join it belongs to.

**`checkJoinProgress`, on the existing 1 s watchdog, is what ends most failures**, and only its
second half is a timer:

- `ic_des_esslen == 0` under a pending join means net80211 gave up on its own —
  `ieee80211_watchdog` calls `ieee80211_deselect_ess` after three failed auth/assoc attempts,
  which is the ordinary outcome of a wrong PSK or an AP that has gone. That is a real signal, not
  a deadline, and it is the common path.
- a 10 s deadline covers "nothing happened at all". It is measured from the last observed
  *progress*, not from the request: `ic_state` advancing pushes it out again, so the budget is
  "10 s without moving". Selecting a BSS costs a whole scan pass and the handshake takes its own
  time, so a single fixed budget would either cut a healthy join short or wait out a dead one.

**Reading the first boot.** New ioreg counters, alongside the existing `ItlwmScanReq*`:

```bash
ioreg -r -c AirportItlwm -l -w0 | grep -E 'ItlwmAssoc|ItlwmJoin'
```

| counter | meaning when it does not move |
| --- | --- |
| `ItlwmAssocCalls` | the FSM never reached the setter — look for an earlier `kIOReturnUnsupported` in the log, as with `getWCL_LOW_LATENCY_INFO` |
| `ItlwmAssocStarted` / `Refused` | Refused means net80211 was not ready (`ic_state <= INIT`) |
| `ItlwmJoinAssocDone` | started but never associated: net80211 never selected the BSS, so suspect the ESSID/BSSID programming or `ieee80211_match_bss` |
| `ItlwmJoinConnectDone` | associated but never linked up: the four-way handshake, i.e. the PMK |
| `ItlwmJoinTimeouts` | non-zero means joins are dying with no net80211 signal at all |
| `ItlwmAssocKeyInReq` | the request carried no usable key at `+0x40` — should equal `AssocCalls` |
| `ItlwmAssocNoPmk` | should stay **0**: a PSK network with no key from the request *or* the `setCIPHER_KEY` stash cannot associate |
| `ItlwmJoinMaxState` | highest `ieee80211_state` any join reached: 1 SCAN, 2 AUTH, 3 ASSOC, 4 RUN |

`ItlwmJoinMaxState` is the one that splits the two failures the other counters cannot tell apart.
Stuck at **1** means `ieee80211_match_bss` rejected the target on every pass, so the *programming*
is wrong; **2 or 3** means the BSS was selected and the exchange itself failed. It is sampled at
1 Hz from the watchdog, so a 1 is weaker evidence than a 2 — a short-lived AUTH can be missed.

Confirm against the WCL's own log, which names the FSM transition each message causes:

```bash
log show --last 10m --predicate 'process == "kernel"' | grep -E 'JOIN_MANAGER|WCL_ASSOCIATE'
```

**Still open, and tracked as mechanism 16:** the three flag bytes in the request are unread. If a
join is refused, comes up misconfigured, or is silently insecure, suspect those before suspecting
this code — that failure mode is the `BeaconMetaData` bit-14 shape, where nothing fails and the
result is merely wrong.

#### First boot: the setter works, and `associateSSID` destroys the PMK it is given

Measured on 26.6, picking a WPA2 network from the menu:

```text
ItlwmAssocCalls 4   ItlwmAssocStarted 4   ItlwmAssocRefused 0
ItlwmJoinAssocDone 0   ItlwmJoinConnectDone 3   ItlwmJoinTimeouts 3
```

So the whole WCL side is working: the FSM reaches slot 602, the setter accepts, our failure
reports move it on, and it walks candidates. Nothing associated.

**`ieee80211_ioctl.c:223` is the bug**, and it is one line into the function this path calls:

```c
void ieee80211_disable_rsn(struct ieee80211com *ic) {
    ic->ic_flags &= ~(IEEE80211_F_PSK | IEEE80211_F_RSNON);
    memset(ic->ic_psk, 0, sizeof(ic->ic_psk));      /* <-- */
    ...
}
```

`associateSSID` calls it first thing. The pre-Tahoe caller copies its key back in twenty lines
later and never notices; the WCL caller passes no key, because the PMK had already been stashed
in `ic_psk` by `setCIPHER_KEY` — which this call had just wiped. `ieee80211_match_bss` then hits

```c
if ((ni->ni_rsnakms & ic->ic_rsnakms & ~(PSK|SHA256_PSK)) == 0)   /* AP is PSK-only */
    if (!(ic->ic_flags & IEEE80211_F_PSK))
        fail |= IEEE80211_NODE_ASSOCFAIL_WPA_PROTO;
```

and rejects the target on every scan, for ever. That is the exact counter signature above:
net80211 keeps scanning, so it never deselects the ESS (the prompt failure signal), never
associates, and only the 10 s deadline ends each attempt.

**Fixed by handing the key in rather than leaving it in state the callee resets.**
`beginAssociateGated` passes a key as `associateSSID`'s argument, putting the WCL caller on exactly
the pre-Tahoe code path instead of a special case.

#### Second boot: there was no PMK to preserve — the key is in the request

The first fix was necessary and not sufficient. Measured immediately after it:

```text
ItlwmAssocCalls 5   ItlwmAssocStarted 5   ItlwmAssocNoPmk 5
ItlwmJoinMaxState 1   ItlwmJoinAssocDone 0
```

`AssocNoPmk == AssocCalls` means `fPmkValid` was false on every call: **`setCIPHER_KEY` is never
invoked on this path at all.** The premise this driver had been carrying — "the PMK arrives through
`setCIPHER_KEY(APPLE80211_CIPHER_PMK)`, so `setWCL_ASSOCIATE` has nothing to work with unless it
was stashed first" — was simply false, and `JoinMaxState 1` confirmed the consequence: net80211
never left SCAN, so `ieee80211_match_bss` was still rejecting the BSS.

**The key is at `candidates + 0x40`, the field the header called `_unk40[0x94]`.** Three
independent facts identify it, none of them a guess:

| fact | evidence |
| --- | --- |
| 148 bytes are copied there from the join request | `fillAssocCandidatesList+0x1dd`: `memmove(cand + 0x40, joinRequest[0x10] + 0x64, 0x94)` |
| 0x94 == `sizeof(apple80211_key)` | `apple80211_assoc_data.ad_key` at 0x38, `ad_rsn_ie_len` at 204 → 148 bytes |
| offset 8 of that blob is `key_cipher_type` | `WCLJoinRequest::getKeyCipherType` returns `[joinRequest[0x10] + 0x6c]`, and `0x6c - 0x64 == 8` |

And the join request is built by
`WCLJoinRequest::initWCLJoinRequest(apple80211_assoc_data *, CCLogStream *)` — so this is the
*pre-Tahoe* key, arriving by a new road rather than through a new mechanism. The WCL log line that
was read as evidence for the `setCIPHER_KEY` route,
`initWCLJoinRequest@717: ... key = CIPHER_PMK`, was really saying the opposite: the key is in the
assoc data the WCL is holding, which is what it forwards.

`AssocCandidates.h` now declares `struct apple80211_key key;` at 0x40 with the offset and the
struct size both `_Static_assert`ed. `beginAssociateGated` prefers it and keeps the `setCIPHER_KEY`
stash as a fallback for a release that does use that route. `ItlwmAssocKeyInReq` counts requests
that carried a usable key and should equal `ItlwmAssocCalls`.

**Two rules this pair of boots earns**, and the second is the more expensive one:

- *Reusing a helper means inheriting what it resets, not just what it sets.* `associateSSID` looks
  like a pure setter and is really reset-then-set; the reset is invisible at the call site and
  deleted state established by a different ioctl seconds earlier.
- *An `_unkNN` in a struct you are about to act on is not neutral — it is an unread instruction.*
  The root AGENTS.md already said a flag **bit** can gate data; this is the same failure at field
  scale, and it was hiding in plain sight behind a `memmove` whose length matched a struct already
  defined in this repo. **When a producer copies a fixed-size blob into an unknown field, match
  that length against every struct the same code path already handles before naming it `_unkNN`.**

#### Third boot: `ieee80211_end_scan` is the HAL's to call, and this layer was calling it

The key fix worked. Measured immediately after it:

```text
ItlwmAssocCalls 5   ItlwmAssocKeyInReq 3   ItlwmAssocNoPmk 0   ItlwmJoinTimeouts 0
ItlwmJoinMaxState 3   ItlwmJoinAssocDone 0   ItlwmJoinConnectDone 4
```

`JoinMaxState 3` is the result that matters: net80211 now selects the BSS and walks SCAN → AUTH →
ASSOC, so `ieee80211_match_bss` is satisfied and the PSK is reaching it. `Timeouts 0` says every
failure was detected promptly through the deselect-ESS or deauth signal rather than the deadline.
But `JoinAssocDone 0` — the association exchange never completed.

**Cause: `beginAssociateGated` called `ieee80211_end_scan` directly.** That function has exactly
one legitimate caller in this driver, `ItlIwx::iwx_endscan`, and the two lines before the call are
the point:

```c
if ((sc->sc_flags & (IWX_FLAG_SCANNING | IWX_FLAG_BGSCAN)) == 0)
    return;
sc->sc_flags &= ~(IWX_FLAG_SCANNING | IWX_FLAG_BGSCAN);
ieee80211_end_scan(&ic->ic_if);
```

`iwx_endscan` runs off the firmware's `IWX_SCAN_COMPLETE_UMAC` notification and clears the softc's
scanning flags *first*. Calling `ieee80211_end_scan` from the interface skipped both: the firmware
scan was still sweeping channels, `IWX_FLAG_SCANNING` stayed set, and net80211 went to AUTH anyway.
Nothing on that path stops a running scan — `iwx_newstate_task`'s teardown block only runs when
`nstate <= ostate`, and `iwx_auth` goes straight to `iwx_phy_ctxt_update` / `iwx_mac_ctxt_cmd` — so
the authentication and association frames went out with the radio off-channel. Hence a BSS selected
and an exchange that never completed.

**Fixed by nudging the state machine instead:** `ieee80211_new_state(ic, IEEE80211_S_SCAN, -1)`.
`iwx_newstate_task` special-cases SCAN → SCAN — it returns early if `IWX_FLAG_SCANNING` is set, and
otherwise falls into `next_scan` and starts one. Both branches converge on the firmware notification
and therefore on `iwx_endscan`, with the flags cleared in the right order.

#### Fourth boot: the association exchange itself is refused

The `end_scan` fix changed nothing measurable — same shape, one more attempt:

```text
ItlwmAssocCalls 6   ItlwmAssocKeyInReq 3   ItlwmAssocNoPmk 0   ItlwmJoinTimeouts 0
ItlwmJoinMaxState 3   ItlwmJoinAssocDone 0   ItlwmJoinConnectDone 5
```

**Reaching ASSOC is stronger evidence than it looks.** net80211 leaves AUTH only from
`ieee80211_recv_auth`, i.e. on an authentication *response* from the AP — so the radio is
on-channel, the BSS is the right one, and the AP is willing to talk to us. What fails is the
association exchange: `ieee80211_recv_assoc_resp` raises `IEEE80211_EVT_STA_ASSOC_DONE` only for
`IEEE80211_STATUS_SUCCESS`, and it never fired.

**There is no log route for the reason.** net80211 explains this failure in an `XYLog`, and
`XYLog` is `kprintf` (`itl80211/linux/types.h`), which has no sink on this machine — and
`log show` returns nothing for `kernel` here either. So the ioreg properties are the whole
diagnostic channel, which is why this round adds instrumentation rather than another guess.

Latched on the **first** failure of the boot by `snapshotJoinFailure()`, for the reason
`ITLWM_PREINIT_SNAP` records — sampling after a teardown path measures the teardown:

| property | meaning |
| --- | --- |
| `ItlwmJoinAssocStatus` | first non-zero `ic_assoc_status`: **the AP's own status code**. 0 means no association response ever arrived |
| `ItlwmJoinFailStatus` / `FailState` / `FailDeauth` | the same fields at the moment failure was reported |
| `ItlwmJoinFailRxAuthFail` | `is_rx_auth_fail`; non-zero with a status means the AP refused, zero means nothing came back |
| `ItlwmJoinFailIcRsn` / `IcCipher` | what **we** advertise: `(ic_rsnprotos << 16) \| ic_rsnakms`, `(ic_rsnciphers << 16) \| ic_rsngroupcipher` |
| `ItlwmJoinFailNiRsn` / `NiCipher` / `NiCaps` | what the **AP** advertises, same packing; `NiCaps` bit 6 is MFPC, bit 7 MFPR |
| `ItlwmJoinFailNiAssocFail` | `ieee80211_match_bss`'s `IEEE80211_NODE_ASSOCFAIL_*` bitmask |

Both RSN halves are captured together deliberately: an association that reaches ASSOC and is
refused is most often a parameter the AP will not accept, and reading only our side would cost
another boot to get theirs.

Deliberately *not* changed this round, so the reading is clean: the BSSID pin, and the fact that a
retry arriving while `ic_state` is AUTH/ASSOC returns early without programming the new candidate.
That second one is a real defect — fix it once the current failure is understood.

#### Fifth boot: authentication succeeds and the association response never arrives

```text
ItlwmJoinAssocStatus 65535   ItlwmJoinFailDeauth 1   ItlwmJoinFailRxAuthFail 0
ItlwmJoinFailNiRsn 65538     ItlwmJoinFailNiCipher 524296   ItlwmJoinFailNiCaps 12
ItlwmJoinFailNiAssocFail 32  ItlwmJoinFailIcRsn 0    ItlwmJoinFailIcCipher 0
```

**`0xffff` / `1` is a fingerprint, not a status.** `ieee80211_recv_auth` writes
`ic_deauth_reason = IEEE80211_REASON_UNSPECIFIED` and `ic_assoc_status = 0xffff` at
`ieee80211_input.c:2277` immediately before `ieee80211_auth_open`. Reading that pair back means an
authentication frame **was received and processed**, and that `ieee80211_recv_assoc_resp` never
overwrote it (`:2666`). With `is_rx_auth_fail = 0` — the counter its failure branch bumps — the
conclusion is unambiguous: *the association response never arrived at all.* Not a refusal.

The AP decodes as ordinary WPA2-PSK/CCMP with no PMF: `ni_rsnprotos 1` = `PROTO_RSN`,
`ni_rsnakms 2` = `AKM_PSK`, `ni_rsnciphers 8` = `CCMP`, group `CCMP`, `ni_rsncaps 0xc` = replay
counters only, MFPC and MFPR clear. Nothing exotic to negotiate.

**Two of those readings are worthless, and one of them was my own instrumentation measuring the
teardown.**

- `ItlwmJoinFailIcRsn` / `IcCipher` read 0 **by construction**. `ieee80211_deselect_ess` calls
  `ieee80211_disable_rsn`, which zeroes `ic_rsnprotos`/`akms`/`ciphers`/`groupcipher`, and the
  failure is *reported* from the `ic_des_esslen == 0` branch — which only becomes true once
  `deselect_ess` has run. The snapshot could never have said anything else. This is exactly the
  trap `ITLWM_PREINIT_SNAP` records, walked into a second time while quoting it. Both properties
  are removed; the replacement latches `ic_flags` and the RSN words the first time a join gets
  past `SCAN`, i.e. while the attempt is live.
- `ItlwmJoinFailNiAssocFail 32` = `ASSOCFAIL_BSSID` is post-failure scan noise:
  `ieee80211_match_bss` writes `ni_assoc_fail` for **every** node it evaluates and copies it to
  `ic_bss` (`ieee80211_node.c:1174-1178`), so with the BSSID pinned every other BSS sets that bit.

**One deduction does survive, and it is load-bearing: RSN *was* configured during the attempt.**
`ieee80211_match_bss` fails a `PRIVACY`-capable AP with `ASSOCFAIL_PRIVACY` unless
`IEEE80211_F_WEPON | IEEE80211_F_RSNON` is set. The BSS was selected and net80211 reached ASSOC,
so `F_RSNON` was set and `ieee80211_ioctl_setwpaparms` had run. "We advertised no RSN IE" is
therefore excluded without another boot.

That leaves: RSN configured, BSS selected, authentication answered, association request sent
(`ieee80211_newstate`'s `AUTH -> ASSOC` case is the only path to state 3 and it is what calls
`IEEE80211_SEND_MGMT(ASSOC_REQ)`), and **no association response**. Next round's instrumentation
is aimed at exactly that gap:

| property | question it answers |
| --- | --- |
| `ItlwmJoinIcFlags` / `IcRsn` / `IcCipher` | what we advertised, captured at AUTH rather than at teardown |
| `ItlwmJoinNiRsnCipher` | the *negotiated* pairwise cipher, which is what goes into the assoc request's RSN IE |
| `ItlwmJoinFailMgtDiscard` | `is_rx_mgtdiscard`. `recv_assoc_resp` bumps it and returns when `ic_state != ASSOC`, so non-zero means a response arrived and was dropped; zero means none came |
| `ItlwmJoinFailBadRsnIe` / `ElemBad` | parse failures on the same path |
| `ItlwmJoinTicksAuth` / `TicksAssoc` | 1 Hz dwell per state: whether it waited in ASSOC or bounced straight out |
| `ItlwmAssocUpperAuth0` / `AuthN`, `KeyInfo0` / `KeyInfoN` | the auth-type bitmask and `(cipher << 16) \| len` of the key, first and last call — `AssocKeyInReq 3` of 5 with `AssocNoPmk 0` says these differ between calls |

#### Sixth boot: everything the driver controls is correct, and the frame gets no answer

Every input to the association is now measured and every one of them is right:

```text
ItlwmAssocUpperAuth0/N 8        WPA2_PSK, on the first call and the last
ItlwmAssocKeyInfo0/N   0x60020  cipher 6 = APPLE80211_CIPHER_PMK, len 0x20 = 32
ItlwmJoinIcFlags       0x326e0800  DESBSSID SHSLOT SHPREAMBLE QOS RSNON PSK HTON AUTO_JOIN VHTON
ItlwmJoinIcRsn         0x3000a  ic_rsnprotos RSN|WPA, ic_rsnakms PSK|SHA256_PSK
ItlwmJoinIcCipher      0xc0004  ic_rsnciphers CCMP|TKIP, ic_rsngroupcipher TKIP
ItlwmJoinNiRsnCipher   0x80008  ni_rsncipher CCMP, ni_rsngroupcipher CCMP
ItlwmJoinTicksAuth 1   ItlwmJoinTicksAssoc 16
ItlwmJoinFailMgtDiscard 0   BadRsnIe 0   ElemBad 0   RxAuthFail 0
```

- `F_RSNON | F_PSK` are set, so the earlier deduction is now measured rather than inferred.
- **The RSN IE the driver transmits is correct.** `ieee80211_add_rsn_body` builds it from the
  *node's* `ni_rsn*`, not from `ic_rsn*` — group CCMP, pairwise CCMP, AKM PSK, which is exactly
  what this AP advertises. `ic_rsngroupcipher` being TKIP is a harmless artefact of
  `associateSSID` passing `i_protos = WPA1|WPA2`, which makes `setwpaparms` default the group
  cipher to TKIP; it never reaches the air.
- The PMK is present and 32 bytes, and the auth type is `WPA2_PSK` on every call that matters.
- `TicksAuth 1` against `TicksAssoc 16`: authentication is answered immediately, then net80211
  sits in ASSOC for ~4 s per attempt across four attempts and gives up each time.
- `MgtDiscard 0` closes the last "arrived but dropped" route: `ieee80211_recv_assoc_resp` bumps
  that counter and returns when `ic_state != ASSOC`, so **no association response reached
  net80211 by any path**.

So every parameter the driver chooses is right, the frame is built correctly, and the AP — which
answers our authentication a moment earlier — never answers the association. That leaves the
transmit path, and one question splits it cleanly:

| property | reading |
| --- | --- |
| `ItlwmJoinMgtqMax` | high-water mark of `mq_len(&ic->ic_mgtq)` during a join. Non-zero while net80211 sits in ASSOC means `ieee80211_send_mgmt` built the frame and `if_start` never took it — a defect in this layer. Zero means the frame reached the hardware |
| `ItlwmJoinFailTxNombuf` | `is_tx_nombuf`. The association request is far larger than the authentication one (SSID, rates, RSN IE, HT and VHT capabilities), so it can fail to allocate where the auth frame did not — and `ieee80211_newstate` discards `IEEE80211_SEND_MGMT`'s return, so nothing else records it |
| `ItlwmJoinFailIfFlags` | `if_flags` at failure: `IFF_UP`/`IFF_RUNNING`/`IFF_OACTIVE` |

Worth stating because it bounds the search: **`itlwm.kext` associates with this AP on this machine
every day**, with the same net80211 and the same `iwx` HAL. So the defect is in the AirportItlwm
layer or in something it configures differently.

#### FOUND — the association request never leaves the host

```text
ItlwmJoinMgtqMax 1   ItlwmJoinFailTxNombuf 0   ItlwmJoinFailIfFlags 0x8847
```

`if_flags` decodes as `UP | BROADCAST | DEBUG | RUNNING | SIMPLEX | MULTICAST` — `IFF_OACTIVE`
clear, so the interface is fine. The allocation succeeded. And **a management frame was sitting in
`ic_mgtq`**: `ieee80211_send_mgmt` built the association request, enqueued it, and it was never
dequeued. The AP had nothing to answer, which is why every "no response" reading was consistent.

**Two silent exits on the transmit path, and no retry behind either.**

```c
ItlIwx::iwx_start(ifp)
    getMainCommandGate()->attemptAction(_iwx_start_task, ifp);   // non-blocking!

ItlIwx::_iwx_start_task(...)
    if (!(ifp->if_flags & IFF_RUNNING) || ifq_is_oactive(&ifp->if_snd))
        return kIOReturnError;
    for (;;) {
        if (sc->qfullmsk != 0) { ifq_set_oactive(&ifp->if_snd); break; }   // before the mgmt dequeue
        ...
        m = mq_dequeue(&ic->ic_mgtq);
```

`attemptAction` returns `kIOReturnCannotLock` and does nothing if another thread holds the gate —
and that gate is `_fCommandGate`, which `AirportItlwm::start` adds to **`_fWorkloop`**, the
`IO80211WorkQueue` shared by the WCL's inbound calls, the deferred `postMessage` drain, the scan
timer and the firmware interrupt. `_iwx_start_task` has a second exit of the same kind, and its
`qfullmsk` check sets `oactive` and breaks *before* the management dequeue, so a full data queue
strands management traffic too.

**Nothing retries.** `ifp->if_timer` is armed only *after* a successful transmit, so a frame that
never went out never arms the retry; `iwx_clear_oactive`'s `(*ifp->if_start)(ifp)` only fires from
the Tx-completion path, which by construction is not running when nothing was sent. On
`itlwm.kext` this is survivable because almost nothing else holds its work loop's gate. On Tahoe
the same work loop carries the entire WCL interaction.

`AirportItlwm::drainStrandedMgmtFrames()` re-drives `if_start` from the **watchdog** work loop —
a different one, so it is exactly the caller that can take the gate once the owner lets go — and
counts it in `ItlwmMgtqKicks`. `ItlwmJoinMgtqStuck` and `ItlwmJoinOactive` say how persistent the
condition is and whether `oactive` is involved.

**This is a repair for the missing retry, not a cure for the contention, and it must not be
mistaken for one.** A management frame that waits up to a second still misses its exchange. Two
questions stay open and matter more than the workaround:

- who holds `_fWorkloop`'s gate during a join, and for how long. `postMsgGated` wraps
  `IO80211Controller::postMessage` in `_fWorkloop->runAction()`, and `IO80211Glue::sendIOUCToWcl`
  can sleep on a **50 s** deadline waiting for the serial queue — a gate hold of that order would
  strand every frame in the window.
- whether `iwx_start` should retry rather than drop. Upstream chose `attemptAction` deliberately
  to avoid blocking, but upstream also runs where the gate is rarely contended.

`ItlwmMgtqKicks` climbing on an otherwise healthy system is the signal that this is still live.

**The rule: net80211 entry points that a HAL drives are not free functions.** `ieee80211_end_scan`
reads like part of the public net80211 API, and in OpenBSD it is one — but in this port the HAL owns
a parallel piece of state (`IWX_FLAG_SCANNING`) that only its own wrapper maintains. Before calling
any `ieee80211_*` function from the AirportItlwm layer, grep the HAL for it: if the HAL calls it too,
whatever the HAL does around the call is part of the contract. The generic version of the
`associateSSID` lesson two boots earlier — there the extra work was a *reset*, here it is the caller's
own bookkeeping.

**The general shape, which is the part worth keeping:** *reusing a helper means inheriting what it
resets, not just what it sets.* `associateSSID` looked like a pure setter and is really
reset-then-set; the reset is invisible at the call site and silently deleted state established by
a completely different ioctl several seconds earlier. Nothing failed loudly — the build was clean,
`mapdrv` was clean, the setter returned success, and the WCL was satisfied. Before calling an
existing net80211 helper from a new path, read what it clears, not only what it writes.

**The frames are available.** `ItlIwx::iwx_rx_frame` hands `(m, ni, rxi)` to `ieee80211_inputm` at
`itlwm/hal_iwx/ItlIwx.cpp:5439` — full management frame in the mbuf, plus `rxi` for RSSI and
channel. Hooking there keeps the change inside this port's own HAL and out of the vendored
net80211 in `itl80211/`. Note net80211 itself keeps only parsed `ieee80211_node`s, so the existing
`convertNodeToScanResult` is no help: it builds an `apple80211_scan_result`, the pre-Tahoe shape,
not what type 201 wants.

Note `setWCL_SET_SCAN_HOME_AWAY_TIME` (slot 605) is left returning `kIOReturnUnsupported`
deliberately. The WCL logs `unable to set scan home time & away time` and carries on, and since the
scan request's dwell parameters are ignored anyway, answering success would be a lie of the same
kind that made `getFaultReporterFromDriver` panic.

**Also still open:** the request is ignored — no
channel subset, no SSID filter, no dwell times — and results are whatever net80211 has cached.
Known `apple80211ScanRequest` layout so far, from `AppleBCMWLANCore::setWCL_SCAN_REQ`: `+0x54`
channel count, `+0x5c` an array of 12-byte entries whose first dword is the channel number.
Whether airportd is satisfied by cached results at all, or whether the WCL expects beacons pushed
into `IO80211ScanCacheStore::updateOrAddBeacon` / pulled via `getWCL_BSS_INFO`, is unresolved —
the next boot's log says which, and that is cheaper than reading more of the family.

Also idle-looping, not blocking: `airportd` retries an AWDL virtual interface every 2 s
(`Failed to match cached interface (role=awdl parent=(null))`). That is `createVirtualInterface`,
a separate feature, and it costs only AWDL/P2P.

### RESOLVED — the JOIN_MANAGER FSM completes on 26.6 (but see the section after it)

Measured after deferring `setLinkState`, on one attempt from the menu:

```text
ItlwmAssocCalls 1   ItlwmAssocStarted 1   ItlwmAssocRefused 0   ItlwmAssocNoPmk 0
ItlwmJoinAssocDone 1   ItlwmJoinConnectDone 1   ItlwmJoinTimeouts 0
ItlwmJoinMgtqMax 0     ItlwmJoinMgtqStuck 0     ItlwmJoinOactive 0
```

One request, no candidate walk, no retries; both completion messages posted; nothing stranded on
the transmit path. **The absence of every `ItlwmJoinFail*` property is itself the result** — they
are published only when `snapshotJoinFailure()` has run, and it runs only for a non-zero connect
status. The join never failed.

Two readings that look wrong and are not:

- `ItlwmJoinMaxState 3` (ASSOC, not RUN). `checkJoinProgress` stops sampling the moment
  `fJoinPending` clears, and that is at link-up — one transition before RUN. Expected on success.
- `ItlwmJoinMgtqMax 0` does **not** prove the transmit contention is gone. The watchdog samples at
  1 Hz and `drainStrandedMgmtFrames()` re-drives `if_start` in the same pass, so a frame that is
  stranded and immediately rescued can leave no trace in the high-water mark. **`ItlwmMgtqKicks`
  is the counter that answers it** — it is outside the `ItlwmJoin*`/`ItlwmAssoc*` prefixes, so
  grep for it explicitly:

  ```bash
  ioreg -r -c AirportItlwm -l -w0 | grep -E 'ItlwmMgtq|ItlwmAssoc|ItlwmJoin'
  ```

  Non-zero means `iwx_start`'s `attemptAction` is still losing the gate and the 1 Hz retry is
  carrying the driver. That is a stopgap, not a fix — see the two open questions at the end of
  "FOUND — the association request never leaves the host".

  **Measured: `ItlwmMgtqKicks = 1` on this join.** The contention is live and the retry is what
  carried the association request out. Do not read the zeros above as "the transmit path is fine".

### The join completed and the interface was still down — message 216 and `WCLNetManager`

Same boot as the counters above. `JOIN_MANAGER` reached `IDLE` with no failures, and:

```text
$ ifconfig en3                    ... flags=8863<UP,...,RUNNING,...>  status: inactive
$ networksetup -getairportnetwork en3   You are not associated with an AirPort network.
ioreg: IOLinkStatus = 1           (kIONetworkLinkValid, without kIONetworkLinkActive)
airportd: AUTO-JOIN: Auto-join aborted (error=(37 'driver not available'))
```

The WCL transition log named it in one line — and this was the first use of that log on this
bring-up, after months of answering the same class of question one reboot at a time:

```text
JOIN_MANAGER: IDLE --JOIN_REQ--> IN_PROGRESS --JOIN_ASSOC_COMPLETE--> ASSOC_DONE
              --JOIN_CONNECT_COMPLETE--> CONNECT_COMPLETE --JOIN_COMPLETE--> IDLE
ROAM_MANAGER: LINK_DOWN --CONNECT_COMPLETE--> ROAM_MANAGER_STATE_LINK_UP
NET_MANAGER:  LINK_DOWN --CONNECT_COMPLETE--> NET_MANAGER_STATE_LINK_DOWN     <-- no-op
```

`ROAM_MANAGER` accepted `CONNECT_COMPLETE` and moved; `NET_MANAGER` treats it as `ignore` while in
`LINK_DOWN`. **`NET_MANAGER` is the FSM the rest of the system reads** — BSD link state, `airportd`,
auto-join — so its staying down is the whole of "the join worked and nothing is connected".

`wclfsm.py WCLNetManager` gives both halves of the fix:

- subscriptions: exactly one link message, `type=2 num=216 -> WCLNetManager::linkStatusInd`.
- transitions: `LINK_DOWN + LINK_UP -> WAITING_FOR_CONNECT_COMPLETE (firstLinkUp)`, then
  `WAITING_FOR_CONNECT_COMPLETE + CONNECT_COMPLETE -> WAITING_FOR_IP (connectComplete)`, then
  `IPV4_STATUS -> LINK_UP`. **So 216 must precede 213** — the same ordering rule as 211 before 213,
  for a different FSM. `IPV4_STATUS` arrives from configd via ioctl 489, so DHCP drives the last
  step; the driver owes only 216.

`linkStatusInd` is four instructions: it raises `LINK_UP` when payload byte 6 is non-zero and
`LINK_DOWN_IND` when it is zero. Layout in `include/Airport/JoinCompleteEvents.h`, recovered from
both of Apple's producers — `AppleBCMWLANNetAdapter::handleLink` (firmware event) and
`::sendInternalLinkDownInd` (driver-originated), which post `0x10` bytes with the trailing `bool`
true. Bytes 0..5 are the BSSID: `handleLink` copies them out of `wl_event_msg_t.addr` as a dword
plus a word, which is what identifies them.

Now posted from `setLinkStateGated` via `AirportItlwm::postLinkStatusInd`, before
`postJoinConnectComplete`, and **not gated on `fJoinPending`** — a link coming up or going down is a
fact about the interface, not about a join, and net80211 raises both on its own during roaming and
beacon loss. Counters `ItlwmLinkIndUp` / `ItlwmLinkIndDown`. Unbooted.

**Booted, and 216 works.** `NET_MANAGER: LINK_DOWN --LINK_UP--> WAITING_FOR_CONNECT_COMPLETE`,
`ItlwmLinkIndUp = 1`. The FSM moved exactly as the corrected transition table predicted.

### CONFIRMED — `firstLinkUp` refuses the link, and the reason is a stubbed getter (ioctl 433)

The link-up was accepted and then withdrawn inside the same millisecond. The WCL log gives the
whole chain with no inference required:

```text
NET_MANAGER: LINK_DOWN --LINK_UP--> WAITING_FOR_CONNECT_COMPLETE
INTERNAL: Get APPLE80211_IOC_WCL_BSS_INFO (433)  res=<FAIL:-536870201:0xe00002c7>
[wcl] updateBss@2534:Fail to get bss info
[wcl] leaveNetworkCommand@2381:ask to leave network due to <Update Bss fail>
       isAbortJoin=<0> isDisconnectInVoluntary=<1> enhancedDisassocReason=<10>
NET_MANAGER: WAITING_FOR_CONNECT_COMPLETE --LEAVE_NETWORK--> DEAUTH --LINK_DOWN_COMPLETE--> LINK_DOWN
```

`firstLinkUp` calls `updateBss`, and `updateBss` opens by fetching the BSS description **from the
driver**. Our `kIOReturnUnsupported` stub made it abandon the network one transition after
accepting the link. This is `getWCL_LOW_LATENCY_INFO` again — **the third time a stubbed getter,
unrelated on its face, has been the actual gate.** Treat "unsupported" as a decision, not a default.

The contract is fully pinned and needs no guesswork:

- `updateBss` does `IOMallocZeroData(0x844)` → `cmdIouc(433, get, buf, 0x844, …)` → `IOFreeData(buf,
  0x844)`. **0x844 is `sizeof(BeaconMetaData) + BEACON_META_MAX_IE_LEN`** — the same 0x800 IE cap
  already established for the scan path.
- the reply type is `apple80211_beacon_msg`, which is a `BeaconMetaData` followed by the IE list:
  the identical structure `postScanBeacon` already builds.
- on success it goes to `WCLScanCacheStore::updateOrAddBeacon(BeaconMetaData&, uint8_t *)` — the
  same consumer as the scan beacons — and then `setCurrentBSS`.

So the getter is answered from a **cache of scanned beacons**, filled in `postScanBeacon` and copied
out under `fPendingMsgLock`. That is not a shortcut: an `ieee80211_node` stores the *parsed* results
— SSID, rates, RSN parameters — and never the bytes, so a beacon cannot be rebuilt from one after
the fact. The IE list exists only on the wire, and only while scanning. Counters
`ItlwmBssInfoCalls` / `ItlwmBssInfoEmpty`.

**The first version of that cache was a race, and it failed on the very next boot.** It held one
beacon and stored it only when the BSSID matched the join target or `ic_bss`. Two things make that
unreliable, and both are visible in `ieee80211_notify_scan_beacon`:

- the event fires **only while `ic_state == IEEE80211_S_SCAN`** (or `F_BGSCAN`), so there is no
  steady refresh once associated; and
- *during* a scan `ic_bss` is the scan's scratch node, not the BSS about to be joined, so that arm
  of the condition essentially never matches.

Which left the whole cache depending on the target's beacon arriving inside one join-scan window.
It did on one boot (`BssInfoCalls = 1`) and did not on the next (`Empty = 1`, `Calls = 0`) — with
546 beacons seen either way, and the association torn down for want of one of them.

Now keyed by BSSID over **every** BSS seen, 16 entries, refreshed in place with oldest-out
eviction, and looked up by `ic_bss->ni_bssid` first and `fJoinBssid` second — they differ during a
join precisely because net80211 has not adopted the target yet. No reference to join state
anywhere. **The general shape: a cache whose fill condition depends on when something else happens
is a race wearing a cache's clothes.** Key it by identity and let the eviction policy bound it.

**Rule: when a WCL getter's reply type is one the driver already produces elsewhere, the answer is
almost certainly the same bytes.** 433 and the scan-beacon push differ only in direction.

Still stubbed on the teardown path, and all seen failing in the same log — none is a blocker yet
because they run while the link is going down, but `WCL_LINK_STATE_UPDATE` and `WCL_ARP_MODE` will
matter once it stays up: `WCL_LEAVE_NETWORK` (425), `WCL_LINK_STATE_UPDATE` (454), `PM_MODE` (392),
`WCL_ARP_MODE` (437), `WCL_SET_SCAN_HOME_AWAY_TIME` (446).

Not the cause, but still unverified and worth keeping in view: `firstLinkUp` has a *second* refusal
branch after `updateBss` succeeds — it sends internal message `0x90003` and, if the reply byte is
clear and a config bit at `+0x439` is also clear, calls `leaveNetworkCommand` rather than `linkUp`.
If a `leaveNetwork` still follows `LINK_UP` once 433 answers, that branch is next.

### CONFIRMED — 433 answers, and the refusal moves one call later to 460

Booted. `ItlwmBssInfoCalls = 1`, `ItlwmBssInfoEmpty = 0`: the beacon cache was warm and the getter
answered. `updateBss` got past it into `setCurrentBSS`, which asks for the *next* thing:

```text
NET_MANAGER: LINK_DOWN --LINK_UP--> WAITING_FOR_CONNECT_COMPLETE
cmdIouc@145:Fail to Get cmd=<APPLE80211_IOC_WCL_EXTENDED_BSS_INFO, 460>
setCurrentBSS@2446:Fail to get EXTENDED_BSS_INFO
leaveNetworkCommand@2381:ask to leave network due to <Update Bss fail>
NET_MANAGER: WAITING_FOR_CONNECT_COMPLETE --LEAVE_NETWORK--> DEAUTH
```

**A fourth stubbed getter, and the same disposal: any non-zero result abandons the network.**
Implementing one gate at a time on this path buys exactly one call per boot, so it is worth reading
the *callee* for its next `cmdIouc` before rebuilding rather than after.

`apple80211_extended_bss_info` is reconstructed in `include/Airport/ExtendedBssInfo.h`, pinned from
both ends with nothing left to guess: the caller's `IOMallocZeroData(0x214)` /
`cmdIouc(460, get, buf, 0x214)`, and `AppleBCMWLANNetAdapter::getExtendedBssInfo`, which writes at
exactly four offsets — `updateRateSetSync(p)`, `updateMCSSetSyc(p+0xbc, p+0xcc, p+0xd4)`,
`getMloContext(p+0xdc)`, `getAssociatedWPARSNIESync(p+0x113, 0x101)`. Those four and the natural
sizes of the member structs agree, and `0x113 + 0x101 == 0x214` closes it.

**The caller zeroes the buffer, which is what makes a partial answer legitimate here.** This struct
travels driver→family like the join events, but unlike them an unfilled field is not a false claim:
zero MCS maps and a zero MLO context say "not supported", which is true. Filled: the rate set from
`ic_bss->ni_rates`, and the RSN element lifted out of the same cached beacon that answers 433.
Tahoe dropped `getRATE_SET`/`getMCS_INDEX_SET` from `IO80211InfraProtocol`, so this cannot delegate
to them the way a pre-Tahoe composition would. Counter `ItlwmExtBssInfoCalls`. Unbooted.

**This header is the worked example of "sizeof right, offsets wrong".** `apple80211_vht_mcs_index_
set_data` is `__attribute__((packed))` and 6 bytes, and every member after it is a byte array, so
the compiler inserts no padding and the last three offsets landed 2 bytes low — while `sizeof`
still rounded to exactly 0x214 and the size assert passed. Only the per-offset asserts caught it.
Assert every offset, never just the total; the total is the check most likely to pass by accident.

### The link completes, held 55 s, and the WCL left on "missed beacons"

The CoreCapture fix booted clean: `ItlwmCCPipesStarted = 3`, `StartFail = 0`, no hang, and the
registry now shows our three pipes `registered, matched` — each with a `CCPipeUserClient` attached,
so userspace is actually draining them and the NULL-timer path cannot be reached at all.

**`NET_MANAGER` reaches `NET_MANAGER_STATE_LINK_UP`** — the whole FSM, end to end:

```text
LINK_DOWN --LINK_UP--> WAITING_FOR_CONNECT_COMPLETE --CONNECT_COMPLETE--> WAITING_FOR_IP
WAITING_FOR_IP --IPV4_STATUS--> LINK_UP          (IPV4_STATUS arrives from configd after DHCP)
```

`en3` gets an address and reports `status: active`. Then, **55 s after LINK_UP**:

```text
leaveNetworkCommand@2381:ask to leave network due to <missed beacons timeout>
    isAbortJoin=<1> isDisconnectInVoluntary=<1> enhancedDisassocReason=<9>
LINK_UP --LEAVE_NETWORK--> DEAUTH --LINK_DOWN_COMPLETE--> LINK_DOWN
```

`WCLNetManager::handleMissedBeacons()` is the source, and it is a two-stage escalation — the first
firing calls `CommonFaultReporter::reportFault` and clears its flag, the second calls
`leaveNetworkCommand`. **That first stage is what raised the fault report that panicked
CoreCapture**, so the panic and this teardown are the same event one step apart.

`WCLNetManager::assocTimerAction` runs every 5 s and calls `handleMissedBeacons` when **two**
elapsed-time values both exceed `0xea61` = 60001 ms:

```text
cmp qword ptr [rbp - 0x78], 0xea61   ; jb -> skip
cmp qword ptr [rbp - 0x70], 0xea61   ; jb -> skip
call WCLNetManager::handleMissedBeacons
```

Those two values are `now - state[0x150]` and `now - state[0x158]`, where `state` is
`WCLNetManager::this[0x20]`. **`findfield.py` over those two offsets is what settles the whole
question**, and it names one writer that matters:

```text
W  WCLNetManager::linkUp           +0x20a   [0x150]     once, at link-up
W  WCLNetManager::wake             +0x181   [0x150]     once, on system wake
W  WCLNetManager::assocTimerAction +0x22d   [0x150]     only if a flag byte is set — never, for us
W  WCLNetManager::handleLqmUpdate  +0x181   [0x150]  <- driver message 39
W  WCLNetManager::handleLqmUpdate  +0x1e2   [0x158]  <- driver message 39, and nothing else at all
```

So `[0x158]` is stale from boot for any driver that never posts 39, and `[0x150]` stops being
refreshed one link-up later. The connection dies 60 s after it is established, every time, and
message 39 is the only thing that prevents it.

**Two plausible-looking leads were both wrong. Recorded so they are not re-followed:**

- **message 175 (`updateBeaconCounter`)** accumulates `payload[0x54]` into
  `this[0x20]->[0x20]->[0x10] + 0x70` — a *different object*, and it touches neither timestamp. Its
  name and its `beacon counter` role make it look like the mechanism; it is not.
- **ioctl 434 (`WCL_TRAFFIC_COUNTERS`)** feeds `checkTrafficCounters`, which sets a power-state flag
  and posts a bulletin. It writes neither timestamp either. Implementing it does not prevent the
  teardown.

**Rule: name the writer of the field the deadline actually reads.** Both of the above were reached
by following function names from `handleMissedBeacons` outward; one `findfield.py` on the offset the
comparison uses would have skipped straight to `handleLqmUpdate`. Do that first.

*A separate earlier note in this file named `IO80211ControllerMonitor::setFrameStats` as 175's
producer — that was wrong too.* The `mov edx, 0xaf` there is a `memcmp` length, not a message
number; a byte-scan matched an instruction of the right shape in the wrong role. **The reliable way
to find a message's producer is to enumerate `call` sites of `IO80211Controller::postMessage` by
their `e8 rel32` encoding and read each one's `edx`** — a linear capstone sweep desyncs and finds
nothing, and a bare immediate scan finds false positives. That scan lists all 185 call sites with
their message numbers in one pass, and it is what found `AppleBCMWLANCore::postLQMEvent`.

#### Message 39, `APPLE80211_M_LQM_UPDATE` — implemented, unbooted

```text
AppleBCMWLANCore::postLQMEvent:
    mov  r8d, 0x1dc          ; length, hard-checked by the consumer
    mov  edx, 0x27           ; message 39
    mov  r9d, 1              ; async
    jmp  IO80211Controller::postMessage
```

Sole subscriber is `WCLNetManager::handleLqmUpdate` — verified across every manager with
`wclfsm.py`, so there is no second consumer to satisfy. Payload
`include/Airport/LqmEventData.h` (`apple80211_lqm_event_data`, `0x1dc`), Apple's own type name from
the two mangled symbols.

`AirportItlwm::postLqmUpdate()` posts it every 5 watchdog ticks while `ic_state == RUN`. What it
fills, and why only this much:

| offset | field | filled from |
| --- | --- | --- |
| `0x028` | `liveness_beacon` | `ic_rx_beacons` delta — `!= 0` refreshes `state[0x150]` |
| `0x024` | `liveness_traffic` | `netStat->inputPackets` delta — `!= 0` refreshes `state[0x158]` |
| `0x030` | `counters_valid` | 1 |
| `0x1d9` | `counters_fresh` | 1 only if a delta advanced, mirroring Apple |
| `0x1d8` | `event_valid` | 1 |

Everything else stays zero. **That is safe here specifically because every group in this payload is
gated by its own validity byte** — the consumer skips a group whose flag is clear rather than
reading zeros out of it. This is the `BeaconMetaData` bit-14 shape working in our favour for once,
and it is the reason a partially-filled reconstruction of this struct is legitimate rather than a
latent bug. Do not extend that reasoning to a struct without the flags.

**Deltas, not running totals.** Apple's producer keeps the previous absolute value per slice and
sends `current - previous` (`AppleBCMWLANLQM::updateLinkQualityMetrics`, the pairs at `+0x1090`/
`+0x10b0` and `+0x1094`/`+0x10b4`). A total would also read non-zero and would *keep* reading
non-zero after the AP vanished — the one thing this message must never do.

net80211 had no beacon count, so `ic_rx_beacons` was added (see `itl80211/AGENTS.md`). It counts
only beacons whose BSSID matches `ic_bss`, because counting a neighbour's beacons would keep a dead
link looking alive.

**`ifp->if_ipackets` is a trap on this port** — the field exists on the ifnet shim and *nothing ever
increments it*. Reading it would have made `liveness_traffic` a permanent zero that looks like a
working field and silently disables half the keepalive. The live counter is
`ifp->netStat->inputPackets`, incremented in `ieee80211_input.c`. Grep for a writer before using any
counter on that struct.

Counters `ItlwmLqmPosts` and `ItlwmLqmBeaconStall`. Posts must keep advancing for the whole life of
a connection — it stops exactly 60 s before a `<missed beacons timeout>`. A rising `BeaconStall`
means the message is going out but carrying nothing the WCL accepts, which localises the fault to
`ic_rx_beacons` rather than to the message. `ItlwmLqmPosts` is change-guarded on `>> 6`, not on its
exact value, so publishing it does not defeat `publishRuntimeCounters`' guard.

**Confirmed working on 26.6**: `ItlwmLqmPosts = 33` and `ItlwmLqmBeaconStall = 0` across a
connection held well past the 60 s deadline, with no `<missed beacons timeout>` and
`ItlwmLeaveNetCalls = 0`. Message 39 is what keeps a Tahoe connection alive; nothing else refreshes
either timestamp.

It also reaches userspace, which is a second and cheaper way to check it — `airportd` prints the
payload every interval:

```bash
/usr/bin/log show --last 5m --predicate 'process == "airportd"' | grep 'LQM:'
# LQM: txRate=0.0Mbps txFrames=0 ... rxFrames=6 ... beaconRecv=48 beaconSched=48 channel=44 ...
```

`beaconRecv` is our `liveness_beacon`, and ~48 per 5 s interval is the ~10 beacons/s a real AP
sends — so that field is confirmed correct end to end, not merely accepted. The zeroed rate and
frame fields are the groups this driver deliberately leaves unflagged; they show as `0.0Mbps` in
the Wi-Fi UI and are the obvious next thing to fill if that matters.

### A join that never leaves SCAN, and why it used to be unattributable

Seen twice now: `ItlwmJoinMaxState = 1`, three join attempts each aborted at a precise ~10.25 s
(our own `checkJoinProgress` deadline, `ItlwmJoinTimeouts = 3`), and the WCL reporting
`assoc status 0x22 retVal: 0x3f1` with every per-BSSID status zero — because nothing ever happened.
net80211 stayed in SCAN and `ieee80211_node_choose_bss` selected nothing.

**Every symptom above net80211 says only "it did not work".** The WCL's join timeout, our deadline,
`sendWCLJoinDone lastStatusCode=1009`, and the whole `JOIN_MANAGER` abort sequence are all
downstream of one decision made inside `ieee80211_match_bss`, whose reason upstream discards into a
`DPRINTF` with no sink here. `ItlwmJoinFailNiAssocFail` looked like it covered this and does not —
it read `32` on the *successful* boot too, because it samples `ic_bss`, which during SCAN is the
scan's scratch node rather than any candidate.

Now captured directly (see `itl80211/AGENTS.md`): **`ItlwmScanFailDes`** is the one to read when
`ItlwmJoinMaxState` is 1.

| reading | meaning |
| --- | --- |
| `0x8000 \| mask` | target ESSID was in the cache and `match_bss` rejected it; low byte is one `IEEE80211_NODE_ASSOCFAIL_*` bit (`0x01` chan, `0x04` privacy, `0x08` basic rate, `0x10` essid, `0x20` bssid, `0x40` WPA proto, `0x80` WPA key) |
| `0` | the target was never a candidate at all — a scan-coverage problem, not a selection one |

with `ItlwmScanCand` / `ItlwmScanSkipped` saying whether there was anything to reject
(`Skipped` counts nodes dropped on `ni_fails` before `match_bss` ran) and `ItlwmScanFailOr` the OR
across the pass as a fallback when the target is absent.

**Rule: when a failure is reported by four layers and explained by none, find the layer that
computed the reason and threw it away.** Every one of those symptoms was a faithful report of a
decision made somewhere else.

#### What it found on the first boot: a self-sustaining join-failure loop

`ItlwmScanFailDes = 0` with `ItlwmScanCand = 42` — 42 candidates examined and the target among none
of them — plus twenty join attempts in two minutes, each dying at **exactly one watchdog tick**
with `ItlwmJoinTimeouts = 0`. One tick, and not the deadline, points at one branch:

```c
if (fJoinWatchEss && ic->ic_des_esslen == 0) { postJoinConnectComplete(...); return; }
```

`fJoinWatchEss` was armed in `joinStarted()` from the **request's** `ssid_len`, but the branch tests
**net80211's** `ic_des_esslen`. Between the two sits `beginAssociateGated`'s early return for an
exchange already in flight — the path that deliberately does *not* call `associateSSID`. Take that
return and the watch is armed against an ESS nobody programmed; the next tick reports a failure that
never happened; the WCL answers a failed connect by trying the next candidate; the next candidate
arrives in the same state and arms the watch again. **The loop recreates its own precondition, so it
has no exit.**

Fixed by arming the watch only after `associateSSID` has run, from `ic_des_esslen` itself
(`AirportItlwm::armJoinEssWatch`). Unbooted.

**Rule: a watch armed from a request and tested against driver state is only sound if the code
between them cannot decline to act.** Arm it from the state the test reads, at the point that state
is written. This is the same shape as the beacon-cache race — a condition sampled where it is
convenient rather than where it is decided.

Three counters now settle this class in one boot rather than by elimination:

| counter | reading |
| --- | --- |
| `ItlwmAssocEsslen` | `ic_des_esslen` after the last `associateSSID`. `0` with `ItlwmAssocCalls` non-zero means net80211 was never told what to look for |
| `ItlwmEssClears` | total `ieee80211_deselect_ess` calls — the single choke point for clearing the ESS |
| `ItlwmEssClearState` | `ic_state` at the last clear. `2`/`3` is net80211's watchdog giving up from AUTH/ASSOC, a real failure; anything else is a caller taking the ESS away mid-join |

Ruling out the clearers by reading alone cost most of a session: `setWCL_LEAVE_NETWORK` was excluded
by `ItlwmLeaveNetCalls = 0`, `ieee80211_watchdog` by its AUTH/ASSOC precondition, and
`setDISASSOCIATE` only by noticing it is `#if __IO80211_TARGET < __MAC_26_0` and not compiled on
Tahoe at all. **A counter at the choke point would have said "never cleared" immediately**, which is
the answer that actually mattered.

Note also that the first version of `ItlwmScanFailDes` was last-write-wins across *every*
`ieee80211_node_choose_bss` pass, including idle auto-join scans that have no target and therefore
reject everything for a trivial reason. It now samples only passes where `ic_des_esslen != 0`.
**Instrumentation that runs on a mixed workload records the least interesting case by
construction** unless it is told which case it is for.

#### RESOLVED — the connection holds

On a clean build the whole path works on 26.6: one association request, a first-try join
(`ItlwmAssocCalls = 1`, `AssocDone = ConnectDone = 1`, `Timeouts = 0`, `JoinMaxState = 3`),
`NET_MANAGER` walking `LINK_DOWN → WAITING_FOR_CONNECT_COMPLETE → WAITING_FOR_IP → LINK_UP`, and the
link **holding past 2.5 minutes** — with `ItlwmLqmPosts = 33`, `ItlwmLqmBeaconStall = 0`,
`ItlwmLeaveNetCalls = 0`, `ItlwmLinkIndDown = 0`, no `<missed beacons timeout>`, and 75/75 pings at
0% loss. The 55-second ceiling is gone.

`ItlwmScanFailDes = 0xC000` — target seen, exact requested BSSID, fail mask zero — and
`ItlwmScanRsnDes = 0`. **The RSN rejection recorded in the section below was corruption, not an RSN
problem**: those numbers were read out of an `ieee80211com` that a stale HAL object was writing
over. The same AP now associates on the first attempt with no change to any RSN code.

**Rule: a reading taken from a corrupted struct is not evidence of anything.** `ItlwmIcSizeHal` and
`ItlwmIcSizeNet` (both `6664` here) are the precondition for trusting any `ic_*`-derived counter;
check them before acting on one. The section below is kept because the instrumentation it describes
is still the right tool — only its one measurement was false.

#### The join fails honestly, and the reason looked like RSN

With the watch fixed the loop is gone — `ItlwmJoinConnectDone` fell from 20 to 3, all three now
from the real 10 s deadline (`ItlwmJoinTimeouts = 3`) rather than from a fabricated failure. The
supporting readings confirm the earlier exclusion-by-reading was correct: `ItlwmAssocEsslen = 9`
(the ESS *is* programmed) and `ItlwmEssClears = 0` (nothing ever took it away).

`ItlwmScanFailDes = 0x8060` — target seen, rejected for `ASSOCFAIL_BSSID (0x20)` and
**`ASSOCFAIL_WPA_PROTO (0x40)`**. So the BSS is in the cache and `ieee80211_match_bss` declines it
on RSN grounds.

`ASSOCFAIL_WPA_PROTO` is set by **eight independent tests** — protocol, AKM, PSK-required,
group cipher, pairwise cipher, IGTK cipher, and MFP in either direction — so on its own it says only
"the RSN parameters do not overlap". Split out, one bit per test, into `ItlwmScanRsnDes`
(`IEEE80211_RSNFAIL_*` in `ieee80211_var.h`).

**A zero intersection says nothing about which side is wrong**, so both sides are recorded too:

| counter | contents |
| --- | --- |
| `ItlwmScanRsnDes` | which sub-check(s) rejected: `0x01` proto, `0x02` AKM, `0x04` AP is PSK-only and `IEEE80211_F_PSK` is clear, `0x08` group cipher, `0x10` pairwise cipher, `0x20` IGTK not BIP, `0x40` AP requires MFP and we lack it, `0x80` we require MFP and the AP lacks it |
| `ItlwmScanNiRsn` / `ItlwmScanNiCipher` | the AP's `(protos << 16) \| akms` and `(ciphers << 16) \| groupcipher` |
| `ItlwmScanIcRsn` / `ItlwmScanIcCipher` | ours, same packing |

`ItlwmScanFailDes` also now prefers the candidate whose BSSID the join actually asked for, marked
with `0x4000`. An ESS with more than one AP was otherwise leaving whichever was scanned last, and
that one carries `ASSOCFAIL_BSSID` for the trivial reason that it is not the one we wanted — a bit
that says nothing while hiding the ones that do. That is the same "records the least interesting
case" trap as above, so expect it whenever a sample is taken inside a loop over candidates.

Note the existing `ItlwmJoinIcRsn`/`ItlwmJoinNiRsnCipher` do **not** cover this: `checkJoinProgress`
captures them only once `ic_state >= IEEE80211_S_AUTH`, and a join rejected in `match_bss` never
gets there. Both read 0 on exactly the boots where the RSN parameters are the question.

#### The panic that boot, and why it was not net80211's fault

That kext panicked shortly after login with a general-protection fault in
`ieee80211_match_ess + 0xa`, reached from `ieee80211_switch_ess` ← `ieee80211_end_scan`. `RDI` — the
`ess` argument — was `0x4c40248d4c03e0c1`, which is not a pointer at all but **x86 instruction
bytes** (`c1 e0 03 4c 8d 24 40 4c`). A pointer read out of the middle of the text segment means the
memory holding it was not what the code thought it was.

`ieee80211_add_ess` is the only thing that populates `ic_ess`, and on Tahoe **nothing calls it** —
its two callers are the HeliPort ioctl path and `itlwm.kext`. The list is initialised empty in
`ieee80211_node_attach` and should have stayed that way, so the list head itself had been
overwritten.

`ic_ess` is the **last member of `struct ieee80211com`**, which is in turn the **first member of
`iwx_softc`**. Every `sc_*` offset is therefore set by that struct's size — and this session added
fields to it three times under incremental builds. A single HAL object file compiled against the
older, smaller layout writes its softc fields directly over the ESS list head. That is exactly the
observed corruption, it links and loads cleanly, and it surfaces arbitrarily far from the cause.

Fixed by building clean, and made self-diagnosing: `ItlwmIcSizeHal` and `ItlwmIcSizeNet` publish
`sizeof(struct ieee80211com)` as the HAL and as net80211 each see it. **They must be equal** — check
them first whenever the driver faults somewhere it has no business faulting.

Two genuine bugs in `ieee80211_add_ess` were found while chasing this and fixed regardless, since
both are live for `itlwm.kext` and HeliPort even though neither caused this panic: its `malloc` is
never zeroed (this port's shim ignores `M_ZERO`, so `flags`, the RSN parameters and the PSK were
inherited from stale heap), and its error paths freed `ess` unconditionally — including when the
lookup had *found* an existing entry that was still linked into `ic_ess`. See `itl80211/AGENTS.md`.

**Rule: a fault inside vendored code is not evidence of a bug in vendored code.** Ask what wrote the
memory it faulted on before reading the function it faulted in.

**RESOLVED — `setWCL_LEAVE_NETWORK` (425).** The teardown used to leave the two halves of the
driver disagreeing: `ItlwmLinkIndDown = 0` and `ifconfig` still reporting `status: active` on an
interface that could not pass a packet, because the WCL had abandoned the network while net80211
stayed in RUN. Tahoe removed `setDISASSOCIATE` from `IO80211InfraProtocol` and routes every
disconnect through 425 instead, so the stub was not "a feature not implemented" but a state
divergence. Now implemented with the pre-Tahoe `setDISASSOCIATE` body — deauthenticate the current
BSS, `ieee80211_del_ess`/`deselect_ess`, then `ieee80211_new_state(SCAN)` — gated on the interface
work queue exactly like `beginAssociateGated`, because it mutates net80211 and sends a management
frame. `apple80211_leave_network` is in `include/Airport/ExtendedBssInfo.h`, sized `0x1c` from
`WCLNetManager::leaveNetwork`'s `cmdIouc(425, set, buf, 0x1c)` and offset-pinned from
`AppleBCMWLANCore::setWCL_LEAVE_NETWORK`. Counter `ItlwmLeaveNetCalls`. Unbooted.

### CONNECTED — and then a CoreCapture panic from a pipe that was never started

With the `ic_state == RUN` predicate in place the link held, DHCP completed, and `en3` reported
`status: active` with an address. **The first working association on Tahoe.**

~150 s into that session the machine panicked, and it was ours:

```text
page fault, CR2 = 0, RDI = 0
CCDataPipe::enqueueBlob + 0x128
 <- CCDataStream::saveData
 <- CCFaultReport::triggerCoreCapture
 <- CCFaultReporter::completeReport      (on an IOTimerEventSource)
```

The faulting sequence is three instructions:

```text
rdi = pipeState[0x38]        ; the pipe's notify timer
rax = [rdi]                  ; <- fault, rdi == NULL
esi = 0x3e8                  ; 1000 ms
call [rax + 0x1d0]           ; arm it
```

`enqueueBlob` takes that path whenever **no client is draining the pipe**: it sets the
notify-pending bit, drops the lock, and arms a 1 s retry timer — unchecked. Nothing ever attaches
to our snapshots pipe, so every blob written to it takes that path. It had simply never been
written to before, because it takes a *fault report* to get there, and this was the first session
that ran long enough to raise one.

**Cause: `CCPipe::withOwnerNameCapacity` does not start the pipe.** It calls only
`initWithOwnerNameCapacity` (vtable slot 281). `CCDataPipe::start` — an ordinary `IOService::start`
— is what allocates the notify timer into `state[0x38]`, and nothing was calling it. The registry
says so in one line, and this is the cheapest way to check it:

```text
+-o AirportItlwm
    +-o CCLogPipe   !registered, !matched     <- ours
    +-o CCLogPipe   !registered, !matched     <- ours
    +-o CCDataPipe  !registered, !matched     <- ours, the one that panicked
    +-o CCPipe      registered, matched       <- the IO80211 family's own
    +-o CCPipe      registered, matched
    +-o CCPipe      registered, matched
```

Same class, same parent, different lifecycle. `AirportItlwm::startCCPipes()` now runs
`start()` + `registerService()` on all three, counted by `ItlwmCCPipesStarted` /
`ItlwmCCPipeStartFail`.

**Called after `super::start()`, not beside the creation in `initCCLogs()`.** Starting a pipe adds
an event source to a work loop, and this is the driver where a single CCPipe created before
`super::start()` hangs the boot reliably. Creation stays where it must be; only the start moves
late, where a work loop exists. Nothing needs the timer before then — it is touched only on
enqueue.

**Rule: a factory named `with…` may only construct.** `withOwnerNameCapacity` returns a usable
object with a half-built lifecycle, and every subsequent operation works until one path reaches a
field that only `start()` fills. **The IOKit registry states `registered` / `matched` are the cheap
check** — comparing our objects against Apple's own on the same node found this in one command,
after the disassembly had already pinned the faulting field.

### CONFIRMED — the whole ioctl chain passes, and the link is then dropped by net80211

Booted with the BSSID-keyed cache. Every gate cleared: `ItlwmBssInfoCalls = 1`, `Empty = 0`,
`ItlwmExtBssInfoCalls = 1`, and no `Fail to Get` for 433 or 460 anywhere in the log.
**`NET_MANAGER` reached `WAITING_FOR_IP`** — the furthest this port has ever got:

```text
LINK_DOWN --LINK_UP--> WAITING_FOR_CONNECT_COMPLETE --CONNECT_COMPLETE--> WAITING_FOR_IP
```

57 ms later it received `NET_MANAGER_EVENT_LINK_DOWN_IND` — **our own message 216 with the flag
clear** (`ItlwmLinkIndDown = 1`) — and left the network with `enhancedDisassocReason=11`.

The cause is in the vendored stack, not in the WCL plumbing. `ieee80211_newstate` drops
`LINK_STATE_DOWN` at the top of *every* transition and relies on the `RUN` case to raise it again;
under RSN that case defers link-up to `ni_port_valid`, which the handshakes set exactly once. So a
transition that re-enters RUN from RUN — a background scan ending on the same BSS — drops the link
state for good. Fixed at the call site in `ieee80211_proto.c`; see `itl80211/AGENTS.md`.

**The general point is worth more than the fix.** That link-down had always been emitted; it only
ever cost a blink of `ifconfig`'s media status, so nothing about it looked wrong for years. Adding
216 gave a noisy cosmetic signal a consumer that acts on it destructively. When wiring a new
consumer to an existing signal, audit what *raises* the signal, not just what reads it.

`ItlwmLinkDownState` records `ic_state` at the transition that dropped the link. **It is sampled in
`setLinkStatus`, which `ieee80211_set_link_state` calls synchronously — not in `postLinkStatusInd`.**
The first version sampled it at the post, which runs later on the deferral thread, by which time
net80211 has moved on; it read `RUN` and answered a different question than the one asked. That is
the `ITLWM_PREINIT_SNAP` trap for the third time: **sample where the event is, not where the
reporting is** — and on this driver those are never the same thread, because every WCL-facing call
is deferred by construction.

#### FOUND — the teardown edge is ASSOC -> RUN, the transition that *completes* the association

`ItlwmLinkDownPair = 0x030410` — `ostate` ASSOC(3), `nstate` RUN(4), `mgt` 0x10 (`ASSOC_RESP`) —
with `ItlwmDisableCalls = 0`. So the link-down the WCL acted on was emitted by
`ieee80211_newstate`'s unconditional `LINK_STATE_DOWN` **on the transition that establishes the
association**, not on any loss of it.

That makes the rule simple and the two previous attempts obviously too clever:

- suppressing `RUN -> RUN` in `ieee80211_newstate` — too narrow; the real edge was `ASSOC -> RUN`,
  and `nstate == RUN` is what all such edges share.
- filtering in `AirportItlwm::setLinkStatus` — wrong layer. It leaves `if_link_state` stuck at DOWN,
  so `ieee80211_set_link_state` treats the *next* genuine disconnect as no change and swallows it.

The predicate now lives in `ieee80211_set_link_state`, the one function that owns `if_link_state`:
**never report the link down while `ic_state == IEEE80211_S_RUN`.** In RUN there is an association
by definition; anything that really ends one leaves RUN first, and that transition still reports.
See `itl80211/AGENTS.md`.

#### How three boots were spent getting there

Booted with both the `ieee80211_newstate` suppression and the restored `iwx_newstate` `return 0`.
Same teardown, `ItlwmLinkDownState = 4` (RUN) — now sampled synchronously, so net80211 really was
in RUN when the link dropped.

That reading is *ambiguous*, which is why two rounds of reasoning from it were wrong. `ic_state`
reads RUN at a link-down for at least three different reasons:

- a transition **into** RUN (`ic_state` is assigned before the link is reported);
- the `nstate == AUTH, ostate == RUN, mgt == AUTH` case, which reports the link down and then
  executes `ic->ic_state = ostate; /* stay RUN */`, leaving RUN behind;
- a link-down that **did not come from net80211 at all** — `AirportItlwm::disable()` also calls
  `setLinkStatus(kIONetworkLinkValid)`, and reads whatever state net80211 happens to be in.

Two instruments replace the deduction, both temporary (mechanism 9):

- `ItlwmLinkDownPair` — `(ostate << 16) | (nstate << 8) | mgt`, latched by `ieee80211_newstate`
  immediately before it reports the link and sampled in `setLinkStatus`. This names the transition
  outright.
- `ItlwmDisableCalls` — non-zero means the family disabled the interface and none of the net80211
  analysis applies.

**The rule this cost three boots to learn: a state variable sampled at a report is not the same
thing as the event that caused the report.** `ic_state` is a *position*; the question was about a
*transition*, and no single position can answer it when several transitions share an endpoint.
Record the edge, not the node.

#### The RUN -> RUN transition existed because a `return 0` was missing

The suppression added to `ieee80211_newstate` did not stop it, and the reason is in the HAL.
`iwx_newstate`'s "prevent attemps to transition towards the same state" guard has **no statement of
its own** — the `return 0` was absent from the commit that added it (3e8da1c), so the `if` adopted
the `if (ic->ic_state == IEEE80211_S_RUN) { … }` block below as its body. Same-state transitions
were therefore passed through to `ieee80211_newstate` rather than dropped at the HAL, which is what
produced the `RUN -> RUN` churn in the first place. Restored; see `itlwm/AGENTS.md` for the second
inversion it caused.

#### The rest of the link-up path, cleared statically instead of one gate per boot

Rather than rebuild and discover the next refusal, every `cmdIouc` on the path was enumerated and
each one's result traced to see whether the caller acts on it. That is a few minutes of
disassembly against a reboot per gate, and it found two more fatal ones the log had not reached:

| ioctl | name | dir | caller | fatal | disposition |
| --- | --- | --- | --- | --- | --- |
| 433 | `WCL_BSS_INFO` | get | `updateBss` | yes | answered from the beacon cache |
| 460 | `WCL_EXTENDED_BSS_INFO` | get | `setCurrentBSS` | yes | `ExtendedBssInfo.h` |
| 454 | `WCL_LINK_STATE_UPDATE` | set | `updateLinkState` | **yes** | accepted |
| 502 | `WCL_UPDATE_FAST_LANE` | set | `setCurrentBSS` | **yes** | accepted |
| 372 | `BSS_BLACKLIST` | set | `connectComplete` | no | result discarded; left stubbed |
| 489 | `IPV4_PARAMS` | set | `setIPV4_PARAMS` | no | `receivedIPv4Address` runs regardless |
| 434 | `WCL_TRAFFIC_COUNTERS` | get | `checkTrafficCounters` | no | left stubbed — see below |
| 446 | `WCL_SET_SCAN_HOME_AWAY_TIME` | set | `handleScanComplete` | no | logged only; left stubbed |
| 426 | `WCL_REASSOC` | set | `setROAM` | no | roaming unimplemented; left stubbed |

434 is the one worth a note, because its name makes it look load-bearing and it is polled every 5 s
for the life of a connection. `checkTrafficCounters` uses it to drive a power-state hint and a
bulletin; it writes **neither** missed-beacon timestamp, so implementing it does not keep a link
alive. Message 39 does that — see the frontier section above.

`updateLinkState` is the trap in that table: it consists of one `cmdIouc(454)` and returns its
result verbatim, so a refusal that looks like a declined notification propagates into
`setCurrentBSS`'s failure branch and abandons the network.

**Accepting a setter and refusing a getter are different acts.** 454 and 502 now return success
because the WCL is *telling* the driver something it already knows — the link state the driver
itself produced, and a fast-lane hint. Nothing is fabricated. Answering a *getter* with success
would invent data, which is why 433 and 460 had to be filled rather than waved through, and why the
remaining `kIOReturnUnsupported` getters must each be judged on what a caller does with the answer.

**Resolving the ioctl number → name table.** The `[IOC DEBUG]` lines print names, so the kernel has
the mapping: find the `APPLE80211_IOC_*` string in `BootKernelExtensions.kc`, find the 8-byte slot
whose chained-fixup target is that string, and index it by `(n - known) * 8`. Verified against six
known numbers (425, 433, 437, 446, 454, 460) before being trusted for 502. This is how an
unrecognised ioctl number gets a name without guessing.

**The general lesson, and it is the expensive one.** Satisfying the FSM that owns an operation says
nothing about the FSM that reports its result. `JOIN_MANAGER` was complete, correct, and measured —
and the machine was not on the network. Before treating any WCL interaction as finished, run
`wclfsm.py` over every manager subscribing to the same area and check each reached the state its
name implies. The live transition log makes that one command, and it is the same shape as the
`BeaconMetaData` flag bit: nothing fails, and the result is simply absent.

### CONFIRMED — the association completes, and the next gate violation is `setLinkState`

The stranded-frame repair worked. The evidence is the panic backtrace, which is better news than
any counter:

```text
ItlIwx::iwx_intr -> iwx_rx_pkt -> iwx_rx_mpdu_mq -> iwx_rx_frame -> ieee80211_decap
  -> ieee80211_enqueue_data -> ieee80211_eapol_key_input -> ieee80211_recv_4way_msg3
  -> AirportItlwm::setLinkStatus -> IOCommandGate::runAction -> setLinkStateGated
  -> IO80211InfraInterface::setLinkStateInternal -> updateLinkSpeed
  -> IO80211Glue::sendIOUCToWcl -> panic "trying to send on thread panic" @IO80211Glue.cpp:419
```

`ieee80211_recv_4way_msg3` means the association **succeeded** and the WPA2 four-way handshake
reached message 3 — everything from `setWCL_ASSOCIATE` through authentication, association and the
start of the handshake now works.

**The gate/thread contract is not a `postMessage` property, which is what the section below said.**
It belongs to `IO80211Glue::sendIOUCToWcl`, and `setLinkState` reaches it by a completely separate
route: `setLinkStateInternal` → `updateLinkSpeed` → `sendIOUCToWcl`. `setRunningState` and
`reportLinkStatus`, called beside it, are on the same footing. Only the `postMessage` calls had
been deferred; the `setLinkState` in the same function was still running inline.

`getCommandGate()->runAction()` was never enough for this and could not be: it closes the gate but
leaves `onThread()` **true**, and `onThread()` is the half that panics here. The call arrives from
net80211 on whatever thread the packet did — for a WPA2 join, from `ieee80211_recv_4way_msg3`
inside `iwx_intr`, i.e. `_fWorkloop`'s own thread.

Fixed by routing link state through the same deferral ring as the messages. The ring now carries a
`kind`, and sharing it is deliberate: a link-state change and the `LINK_CHANGED`/`BSSID_CHANGED`/
`SSID_CHANGED` messages that follow must reach the family in the order the driver produced them.

**The rule, stated in the form that would have caught this:** *anything the driver calls on an
`IO80211*Interface` from a net80211 or HAL thread must be assumed to reach `sendIOUCToWcl` until
the disassembly says otherwise.* The contract is the family's, not any one method's — enumerate
the calls, not the known offenders.

### postMessage must be deferred off the work-queue thread

Every `postMessage` in this driver reaches `IO80211Glue::sendIOUCToWcl`, which panics
`"trying to send on thread panic" @IO80211Glue.cpp:419` unless **both** hold on the interface
work queue (`_fWorkloop`, the object at glue ivars `+0x38`):

```text
inGate()   == true      slot 39; false -> refuse
onThread() == false     slot 38; true  -> refuse
```

**This is not a `postMessage` property.** The contract belongs to `sendIOUCToWcl`, and
`setLinkState` reaches it independently through `setLinkStateInternal` -> `updateLinkSpeed`;
`setRunningState` and `reportLinkStatus` sit beside it. Deferring only the `postMessage` calls left
`setLinkState` panicking from `ieee80211_recv_4way_msg3` — see the section above. Every one of them
now goes through the ring.

Not one call site in this driver could satisfy that pair:

| call site | context | why it could not work |
| --- | --- | --- |
| `eventHandler` (country/assoc/deauth) | HAL `ic_event_handler`, inside `iwx_intr` | on the work-queue thread → `onThread()` true |
| `fakeScanDone` | `IOTimerEventSource` on `_fWorkloop` | same |
| `setLinkStateGated` (link/BSSID/SSID) | command gate on `_fWorkloop`, driven from the HAL | same |
| `setCOUNTRY_CODE` | airportd's ioctl thread, gate open | `inGate()` false |

`runEventSources+0x1b` calls `closeGate` (slot 48), so on the work-loop thread `inGate()` is
already true and it is `onThread()` that fires. **The only fix is to move the post to another
thread**, then close the gate from there.

`AirportItlwm::postMessageSafe` does that: a 16-entry ring plus `IOSimpleLock`, armed with
`thread_call_enter` (safe from any context), drained on the thread_call's own thread with each
post issued inside `_fWorkloop->runAction()` — `inGate()` true, `onThread()` false. That is the
same shape Apple uses for its own deferred posts: `IO80211Glue::processPendingEventQueueSource`
takes the interface work queue from glue ivars `+0x38`, wraps the drain in
`IOWorkLoop::runActionBlock` on it, and runs on a different queue's event-source thread.

Deferral is unconditional, including for callers that could close the gate themselves:
`sendIOUCToWcl` can sleep on a **50 s** deadline waiting for the serial queue, and no caller
thread of ours can afford that — least of all the work loop that services the firmware
interrupt. `ItlwmPostMsgQueued` / `Sent` / `Dropped` track the ring.

**The trailing `bool` is not the fix, and this cost a boot.** It is real —
`IO80211SkywalkInterface::postMessageInternal+0x00` routes a true flag to
`IO80211Glue::routeEventToWcl`, which only enqueues and signals — and Apple's own drivers do pass
true (`AppleBCMWLANCore::postMessageInfra` and `postLQMEvent` both `mov r9d, 1`). But the bound
override is **`IO80211InfraInterface::postMessage`**, not the base, and at `+0xf9a` it calls
`updateCountryCodeProperty(true)` inline with a hardcoded argument, never consulting the flag.
Passing true changed nothing and panicked at the identical offsets. `ITLWM_POSTMSG_ASYNC` is kept
because it matches Apple and does shorten the message types that honour it, but it is not what
makes this safe.

Method note worth more than the finding: **disassemble the override that is actually bound, not
the base class that declares the method.** `tahoe-26.6-slots.txt` names the binding — slot 355 is
`IO80211InfraInterface::postMessage` — and reading `IO80211SkywalkInterface::postMessageInternal`
instead produced a confident, verified-looking, wrong conclusion.

Two things to carry forward:

- **This is not faked-registration fallout.** The interrupt handler runs on the work loop
  whatever registration is in use, so a real `registerNetworkInterface` would not have changed
  it. Do not file it under mechanism 1 or 12.
- **Tahoe only**, by the same reasoning as the fault reporter. Pre-Tahoe now differs from Apple in
  two distinct ways, both deliberate: it posts **inline** rather than deferred, and it posts to
  **`IO80211SkywalkInterface::postMessage`** rather than through the post office. Both are guarded
  by `#if __IO80211_TARGET >= __MAC_26_0` — on the declaration in `IO80211ControllerV2.h` and on
  the call site — so no pre-Tahoe kext even links
  `IO80211Controller::postMessage`; `nm -m` on each built kext is the check, and a missing export
  on an older release would be a load failure rather than a bug. Those targets work, have no WCL
  to inform, and rerouting them would change notification ordering on shipping kexts for no
  gain. A latent divergence, not a fix withheld.

### The apple80211 request surface on Tahoe

Tahoe **deleted the per-request virtual dispatch**. There is no `setSCAN_REQ`, `getSCAN_RESULT`,
`setASSOCIATE`, `setDISASSOCIATE`, `getSSID`, `getBSSID`, `getSTATE`, `setAUTH_TYPE`,
`setRSN_IE`, `getASSOCIATION_STATUS` or `setDEAUTH` virtual anywhere in Apple's 668-slot
`AppleBCMWLANInfraProtocol` vtable — the survivors in that area are `setScanningState` (slot 370)
and `getScanManager` (372). It is the same removal that turned `apple80211_ioctl`,
`apple80211SkywalkRequest` and friends into `_RESERVEDIO80211Controller0..15` padding.

So the 25 `#if __IO80211_TARGET < __MAC_26_0` regions in `AirportItlwmSkywalkInterface.cpp`,
covering 27 handlers, are **load-bearing, not vestigial**. They are matched by the same gating in
`AirportItlwmSkywalkInterface.hpp` and by the pure virtuals living in the `#else` branch of
`#if __IO80211_TARGET >= __MAC_26_0` in `include/Airport/IO80211InfraProtocol.h`.

**Do not un-gate them.** Declaring `override` on a method with no base virtual does not compile;
un-gating the header too would *insert 27 slots* and shift every later slot, which is the
failure mode the whole `__IO80211_TARGET` contract exists to prevent. The proof the current
gating is right is that our Tahoe vtable is 668 slots against Apple's 668, with `mapdrv`
reporting every override on a correctly named slot and 0 wrong (212 at the time of writing; the
count moves whenever an override is added, the zero must not).

Consequence, and the current bring-up blocker: `airportd` reaches the driver, and
`APPLE80211_IOC_SCAN_REQ` (type 10, len 5528) lands on Apple's inherited stub and returns
`6` (`ENXIO`), retried ~20 times per scan:

```text
Apple80211IOCTLSetWrapper: ifname['en3'] IOUC type 10/'APPLE80211_IOC_SCAN_REQ', len[5528] return 6
Apple80211Scan:1489 ifname['en3'], err[6], Apple80211Scan Failed
```

`AirportItlwmSkywalkInterface::setSCAN_REQ` is not in the binary on Tahoe, so it is not the
source of that 6.

**Tahoe's actual scan dispatch**, recovered from the kernel collection. The handler tables named
in `include/Airport/IO80211ControllerV2.h` are real (`__ZL16gSetHandlerTable`), and entry 10 is a
file-static function, not a virtual:

```text
airportd -> IO80211APIUserClient -> gSetHandlerTable[10]
 -> setSCAN_REQ(IO80211Controller*, IO80211SkywalkInterface*, IO80211APIUserClient*, apple80211req*)
     |- req->len == 0 || req->data == NULL          -> 0x16 (EINVAL)
     |- IO80211Controller::getPrimaryInterfaceScanManager()
     |    |- controller slot 400  getPrimarySkywalkInterface()
     |    '- interface  slot 372  getScanManager()
     |  NULL -> error branch at +0x106              <-- where we land
     |- IO80211ScanManager::isScanAllowedByP2P(apple80211_scan_data *)
     |- IO80211Controller::scanStarted(scanSource, apple80211_scan_data *)
     '- IO80211ScanRequest::createIO80211ScanRequest(apple80211_scan_data *, scanType)
```

We override **neither** `getPrimarySkywalkInterface` (controller slot 400) nor `getScanManager`
(interface slot 372); both bind to Apple's implementations, which have nothing to return because
no scan manager is ever created — so `getPrimaryInterfaceScanManager()` yields NULL and the
request fails before any driver code runs.

Do not confuse this with `apple80211setSCAN_REQ(IO80211SkywalkInterface *, apple80211_scan_data *)`,
which looks like the entry point and is not: it `safeMetaCast`s to `IO80211NoneProtocol` and
returns `0xe082280e` for anything else, so it never applies to an infra interface.

**Root cause: `IO80211InfraInterface::start` returns false.** `ItlwmSkyIfStarted = 0` on a
booted machine, with `ItlwmSkyIfHasState = 1` and `ItlwmSkyIfHasEvtSrc = 0`. The scan manager is
created in exactly one place — `IO80211InfraInterface::start + 0x1f2`, via `operator new` at
`+0x1ab` then `initWithControllerAndSkywalkInterface` — so a start that fails leaves
`getScanManager()` with nothing to return, hence `getPrimaryInterfaceScanManager() == NULL` and
`SCAN_REQ -> ENXIO`. **The same failed start is why `state[0xa8]` is never published**, which is
mechanism 2's event-pipe NULL. One cause, both symptoms. Do **not** hand-construct a scan
manager to route around it — a start that completes also yields the AVC advisory, the datapath
inform agent and the Bonjour offload agent.

(For the record, it *is* constructible: `operator new` and
`initWithControllerAndSkywalkInterface(IO80211Controller *, IO80211SkywalkInterface *)` are both
exported, and Apple does exactly that pair. Constructibility was never the blocker.)

Bail-out points in `IO80211InfraInterface::start`, in order — the failure is one of these:

| offset | condition |
| --- | --- |
| `+0x28` | `IO80211SkywalkInterface::start(provider)` returned false |
| `+0x5f` | `safeMetaCast` of the provider failed |
| `+0x13f` | `IO80211AVCAdvisory::withOptions` returned NULL |
| `+0x15e` | `IO80211DataPathInformAgent::withOptions` returned NULL |
| `+0x17d` | `IO80211BonjourOffloadAgent::withOptions` returned NULL |
| `+0x1f9` | `IO80211ScanManager::initWithControllerAndSkywalkInterface` returned false |

Note `+0x1a0`: the scan manager is only created when `[state+0x58] == 1`, so interface role
matters — `AirportItlwmV2::start` does `setInterfaceRole(1)` before `fNetIf->start()`.

**`wlan.debug.enable=1` turns on Apple's own IO80211 logging** — read by
`IO80211Controller::io80211isDebuggable`, and confirmed working on 26.6. It is the best
diagnostic on this whole path, and it costs a boot-arg:

```bash
log show --start "<boot time>" --predicate \
  'processImagePath CONTAINS "kernel" AND senderImagePath CONTAINS "IO80211Family"' --style compact
```

What it establishes, measured:

- The interface start path logs `IO80211SkywalkInterface::init start`, `initIvars complete`,
  `getFeatureFlags: Enabling 'kFeatureOnDemandInterfaceEnable', 'en3' ifId[1] role[1]`, then
  `IO80211SkywalkInterface::createIOReporters getPeerManager() doesn't exist`. **No `logDebug`
  output from `IO80211InfraInterface::start` appears at all** — and its two `logDebug` calls sit
  at `+0x3a`/`+0xe6`, *after* the `+0x28` bail, so the failure is most likely the base
  `IO80211SkywalkInterface::start(provider)` returning false. That function is ~0x704 long with
  its own bail points at `+0x2b`, `+0x90`, `+0xb7`, `+0xea`, `+0xfc`, `+0x16d`.
- Per-request results name themselves: `[IOC DEBUG] EXTERNAL: Get type=<...> res=<...>`.
  `0xe082280e` is returned for exactly the compiled-out handlers (`SSID`, `PHY_MODE`,
  `AP_IE_LIST`), while `CARD_CAPABILITIES` and `POWER` return `GOOD:0` — independent
  confirmation that the gating above matches real behaviour.
- Every request logs **`isDriverAvailable=<0>`**. Not yet traced to its setter; a likely
  common gate and worth following.

**`createEventPipe: ERROR 0xe00002be` is ours** (`kIOReturnNoResources` from the guard), and it
is reached from `IO80211APIUserClient::extEventMonitorInit` — a *userspace* external method, not
the start path. So the guard does **not** cause the start failure it works around; that
circularity is ruled out, and the two are independent.

#### Locating the failure inside `IO80211SkywalkInterface::start`

That function has ~20 conditional branches and **twelve separate jumps to one failure label**
(`+0x6a3`), so the disassembly alone cannot say which gate we hit. Gates already cleared by
inspection: `+0x2b` is a `panic()` assertion, not a return; `+0x88`'s call resolves through
`IOSkywalkEthernetInterface`'s vtable to plain `IOService::start`; `+0xb7` casts the provider to
`IO80211Controller`, which `AirportItlwm` is; `+0xea` needs `controller->getWorkQueue()`
non-NULL, and that returns the file-scope global `_fWorkloop` which `IOPCIEDeviceWrapper::start`
has already set. (`_fWorkloop` and `_fCommandGate` are globals in `AirportItlwmV2.cpp`, not
members — worth knowing before reasoning about either.)

`ItlwmSkyIfLadder` resolves the rest empirically. Apple stores each object it builds into
`state[]` in a fixed order, so the highest bit set localises the failure between two rungs:

| bit | `state[]` | written at | what it is |
| --- | --- | --- | --- |
| 0 | `0x30` | `+0xad` | provider cast to `IO80211Controller` |
| 1 | `0x38` | `+0xe3` | `controller->getWorkQueue()` |
| 2 | `0x80` | `+0x26f` | |
| 3 | `0x78` | `+0x31c` | |
| 4 | `0x28` | `+0x395` | the "started" marker the entry assertion tests |
| 5 | `0xc8` | `+0x460` | |
| 6 | `0x18` | `+0x482` | |
| 7 | `0xa8` | `+0x54d` | the event source — mechanism 2's field |
| 8 | `0xd8` | `+0x573` | |
| 9 | `0x50` | `+0x59e` | last object before the tail |

`ItlwmSkyIfLadder = 0x003` means it died between `+0xe3` and `+0x26f`, and so on. Bit 7 clear is
the same fact as `ItlwmSkyIfHasEvtSrc = 0`, so the two must agree — if they disagree, the offsets
no longer describe the running kernel and must be re-derived with `scripts/abi/disrange.py`.

#### Settled and now fixed: the peer manager was the single root cause

**Current measurement (26.6, after the fixes below): `ItlwmSkyIfLadder = 1023` (`0x3ff`) — every
rung — with `ItlwmSkyIfStarted = 1` and `ItlwmSkyIfHasEvtSrc = 1`.** The ladder has served its
purpose; it comes out with the rest of the instrumentation.

The diagnosis that got here, kept because the method is reusable. Measured
`ItlwmSkyIfLadder = 15` (`0b1111`) — bits 0-3 set, bit 4 clear, and bit 7 clear
agreeing with `ItlwmSkyIfHasEvtSrc = 0`. Interface role is 1, so `+0x36d` takes the `jne` to
`+0x3f8`, and there:

```text
+0x438  call [vtable + 0xb08]     ; slot 353 = IO80211SkywalkInterface::createPeerManager()
+0x446  mov  [state + 0x28], rax  ; the ladder's bit 4
+0x44d  je   +0x653               ; NULL -> logDebug, then the failure exit at +0x6a3
```

Apple's own string on that path: **`ERROR: Family Skywalk Interface start peer manager init
fail`**. The `createIOReporters getPeerManager() doesn't exist` line in the boot log was this
same fact all along.

The chain, and every symptom it accounts for:

```text
IO80211PeerManager::initWithInterface(NULL, this)  returns false
 -> IO80211PeerManager::withInterface        releases, returns NULL
 -> IO80211SkywalkInterface::createPeerManager (slot 353) NULL
 -> IO80211SkywalkInterface::start           false  (state[0x28] unset = ladder bit 4)
 -> IO80211InfraInterface::start             bails at +0x28
      never reaches +0x1f2  -> no IO80211ScanManager -> SCAN_REQ = ENXIO
      never reaches +0x54d  -> state[0xa8] NULL      -> mechanism 2's event-pipe guards
 -> no association path at all
```

**One failing factory explains the whole remaining bring-up.** Mechanism 2, the scan failure and
the missing association were three faces of it.

`createPeerManager` is *not* something to override: the base is
`IO80211PeerManager::withInterface(NULL, this)` — allocate, then
`initWithInterface(NULL, this)`, returning NULL if init fails — and slot 353 in
`AppleBCMWLANInfraProtocol` holds that same base implementation, so Apple's own driver relies on
it working. Find why init fails for us instead.

**And the reason init fails: `allocIO80211RecursiveLock` was stubbed to return NULL.**

```text
IO80211PeerManager::initWithInterface
  +0x236  interface slot 373  getController()
  +0x24d  controller slot 397 getWorkQueue()              -> privateData[0x528]
  +0x27c  controller slot 433 allocIO80211RecursiveLock()  -> privateData[0x78]
  +0x28d  NULL -> return false
```

`include/Airport/IO80211ControllerV2.h` carried
`virtual void *allocIO80211RecursiveLock(void) { return NULL; }`, listed in a comment as
"deliberately stubbed — not required yet". It is required, and it is the whole reason Tahoe
bring-up stalled. Apple's implementation is
`IO80211IORecursiveLock::allocWithParams(getWorkQueue())`; `IO80211IORecursiveLock` is not
reconstructed here, so a local substitute is not possible. The fix is to declare it **with no
body** so the slot binds to IO80211Family's exported implementation — which then calls our own
`getWorkQueue()`, already working. Verified: the symbol is `n_type 0x0f` (defined + external) in
IO80211Family, it appears in the built kext as `(undefined) external ... (dynamically looked up)`,
and it is the only reference to that name in the kext, so there is no same-name class for the
loader to mis-bind.

**Do not stub a slot in this header without first checking whether the family requires a
non-NULL result** — `scripts/abi/findfield.py` and `scripts/abi/callers.py` answer that. A stub
that satisfies the compiler and the vtable diff can still stop the driver dead, and it will not
show up in any build or `mapdrv` check.

**`initWithInterface` has more than one gate.** With the lock bound, the same function ran ~3.6 KB
further and page-faulted at `+0x10bc` on the *next* required object: the CoreCapture fault
reporter, which this port was returning as the wrong class. That one is written up in
`include/Airport/AGENTS.md`. The general lesson is that `+0x28d` was the first of a series — this
function acquires a long list of collaborators and fails or panics on each one, so expect the
panic offset to move forward rather than the problem to end. Read the new offset each time;
`disrange.py <symbol> 0 <offset>` from 0 always decodes cleanly, and the `IOLog` format string
just before the faulting call usually names the step outright (`Finding ivars->_faultReporter`).

Diagnostics note: the `logDebug` family (`IO80211PeerManager::logDebug`,
`IO80211SkywalkInterface::logDebug`) writes to **CoreCapture**, not os_log — which is why no
`logDebug` line appears even with `wlan.debug.enable=1`, while `__os_log_internal` lines do.
CoreCapture is not readable on this machine as configured: the `corecapture` kext is loaded and
the CC pipes exist in the `CoreCapture` ioreg plane, but no `CCCapture` service is ever published,
so `corecaptured` — which launchd starts on `IOProviderClass = CCCapture` — never runs and nothing
is written to disk. Don't spend time there without first arranging for a capture to be triggered.

### Diagnostics on a boot that panics

**The panic string is the only channel.** `start()` runs early in boot, before anything is
reading the console reliably, so XYLog has no reader — not the console, not dmesg, not the
unified log. And a boot that panics never reaches `publishPreinitMark`, so no
ioreg property is ever written. The panic report is collected on the *next* boot, with the
kext disabled, from macOS's "previous session crashed" dialog.

So anything that must survive a panicking boot has to be **inside the panic string**. ioreg
markers only answer questions about boots that survive.

Two things to do with a report before reading anything into it:

- **Bucket it by uptime against the whole corpus.** `System uptime in nanoseconds` plus the
  first kext frame, swept across `/Library/Logs/DiagnosticReports/*.panic`, separates boot
  phases that look alike in a single report. This driver's own failures cluster in the
  driver's own start window, with frames in `IO80211Family`, `IOSkywalkFamily` or
  `IONetworkingFamily`. **That window moved when the deferral was deleted**: it used to be
  35–58 s (at `itldefer=30`) and is now early boot, so the old "single-digit seconds means someone
  else's bug" heuristic no longer holds. Re-derive the bucket from the current corpus before
  attributing a report.
- **Disassemble the faulting Apple frame.** `scripts/abi/disrange.py <symbol> <start> <end>`
  takes the `symbol + 0xNNN` straight from the backtrace, and `scripts/abi/findfield.py` names
  who writes the field it faulted on. That turns "NULL deref in Apple code" into a named
  member: which one was loaded, which was guarded, and which was not.

**Known signature — a wrong-typed object handed to Apple.** A page fault whose innermost frames
are `OSSymbol::withString + 0x18` → `IORegistryEntry::getProperty` → `copyProperty`, with
**CR2 = 0x38 and RAX = 0**, called from an Apple frame that is not doing property lookup at all,
means a virtual was dispatched through the wrong class: slot 36 of every `IORegistryEntry`
subclass is `copyProperty(OSString const *, …)`, so handing Apple any `IOService` where it
expects a non-`IOService` lands there and reinterprets the real argument as an `OSString`. The
zeroed first word of that argument is the NULL vptr, and `getMetaClass` at vtable `+0x38` is the
faulting call. Read it as a **type** error, not a NULL-pointer error: the object exists, its
class is wrong. First instance here was `getFaultReporterFromDriver` returning a `CCStream` —
see "The fault reporter is three objects" in `include/Airport/AGENTS.md`.

**Known-not-ours:** a page fault at `IOPCIFamily : IOPCIDevice::detach + 0x64` roughly 6 s into
boot is an Apple race, not a driver bug — do not spend a bring-up cycle on it. `attach`
publishes `reserved->[0x198]` (event source) before `reserved->[0x1a0]` (work loop) with an
unlocked `IOWorkLoop::workLoop()` thread-spawn between them, and `detach` null-checks the first
then dereferences the second unguarded — which Apple's own guard on the same field at
`detach+0xc6` shows is reachable. On this machine a child of the empty bridge `BR2A` detaches at
~5.9 s on every boot, including boots that survive, so the trigger is routine and only the race
is rare. Nothing in itlwm calls either function; the driver's own panics land at 35–58 s.

**`itldefer` is gone** — measured, then deleted after `-itlnodefer` booted 5/5 (root AGENTS.md
mechanism 7). `IOPCIEDeviceWrapper::start` publishes immediately again, so `AirportItlwm::start()`
now runs early in boot rather than ~30 s in. That changes what the panic-uptime bucketing below is
worth: this driver's own failures no longer cluster at 35–58 s, and a report at single-digit
seconds is no longer automatically someone else's bug.

### BSD-attach tracing — REMOVED

The 16-entry `ItlwmTrace` ring, `ItlwmMarkRef`, the KASLR-delta decoding recipe and all four
boot-arg panic traps (`itlprovtrap`, `itlmarktrap`, `itlifnettrap`, `itlcmdtrap`) are deleted, with
the raw `ifnet + 0x280` read and the hardcoded vtable indices 241/291 that went with them. The NX
fault they existed to catch is root-caused: the kext loader bound slot 241 to
`IO80211SkywalkInterface::errnoFromReturn` instead of `IOService::errnoFromReturn`, and it is now
pinned by overriding 240/241 in `AirportItlwmEthernetInterface`. See `include/Airport/AGENTS.md`.

**Two of the four defaulted to ARMED** — `itlprovtrap` to 1 and `itlmarktrap` to 5, not 0 — so a
shipping kext carried two live `panic()` triggers. They never fired only because both need
`attachToDataLinkLayer` to have run, and on Tahoe it never does. Nothing in the code or the docs
said the default was armed.
**Rule: a diagnostic whose default is "armed" is a landmine, not a diagnostic.** Disarmed must be
what you get by typing nothing, and the removal date belongs in the edit that adds it.

Findings worth keeping from that apparatus, now that it is gone:

- **A trap that never fires is a result.** `itlmarktrap` armed on every id and staying silent is
  what proved Apple never enters our code during the post-attach ioctl.
- **Read a suspect vtable slot off the live object** — `((void *const *)*(void *const *)this)[N]`
  from one of our own overrides, delivered in the `panic()` string. That is what identified the
  loader mis-bind, and it is reusable without any of the deleted machinery.
- **"Gated" ioctls are exactly seven.** `IOEthernetInterface::performCommand` splits on
  `add rax, 0xffffffff7fdf96f4 ; cmp rax, 0x39 ; bt 0x201016000000011, rax`, and only that side
  reaches `executeCommand` → `performGatedCommand` → a `syncSIOC*` handler: `SIOCSIFADDR`,
  `SIOCSIFFLAGS`, `SIOCADDMULTI`, `SIOCDELMULTI`, `SIOCSIFMTU`, `SIOCSIFLLADDR`, `SIOCSIFCAP`.
  configd's `SIOCGIFMEDIA` polling takes the other branch and cannot reach them.

## Verification

```bash
xcodebuild -jobs 8 -target "AirportItlwm-<Release>" -configuration Release build
xcodebuild -jobs 8 -target "AirportItlwm-<Release>" -configuration Debug build
lipo -info build/Release/<Release>/AirportItlwm.kext/Contents/MacOS/AirportItlwm
```

Adding a target must not disturb the existing ones — build `AirportItlwm (all)` before
calling the change done. A green build is not evidence of ABI correctness; that check
lives in `include/Airport/AGENTS.md`.

## Child DOX Index

- No child AGENTS.md files. All files in this folder are owned here.
