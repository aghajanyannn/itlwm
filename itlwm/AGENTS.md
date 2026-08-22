# AGENTS.md — itlwm

## Purpose

Sources for `itlwm.kext` — the Ethernet-shaped driver that needs no private Apple API — and
the Intel hardware abstraction layers that both kexts share.

## Ownership

- `itlwm.cpp/.hpp` — the `IOEthernetController` driver.
- `ItlNetworkUserClient.*` — the HeliPort user client; implements the `include/ClientKit`
  ioctl contract.
- `pm.cpp` — power management.
- `PrivateSPI.pch` — prefix header for every target, including the `AirportItlwm-*` ones.
  Owns the `__MAC_*` fallback definitions.
- `hal_iwm/` — `iwm` devices (7260 … 9560). The largest HAL.
- `hal_iwn/` — legacy `iwn` devices.
- `hal_iwx/` — `iwx` devices (AX200 and later).
- `firmware/` — Intel firmware blobs, packed into `FwBinary.cpp` by the `fw_gen` target.

## Local Contracts

- Each `hal_*` implements `ItlHalService` from `include/HAL`. A new pure virtual there must
  land in all three HALs.
- `PrivateSPI.pch` is shared by every target. Anything added here is visible to both kexts
  and to all `AirportItlwm-*` releases — keep it to type shims and version constants.
- `firmware/` blobs are covered by `iwlwifi-firmware-license` at the repo root, not the
  project's own licence. Adding a blob means adding its `PBXFileReference` and its entry in
  the `fw_gen` inputs.
- Register a new device ID in the HAL's device table **and** in every
  `AirportItlwm/*-Info.plist` `IOPCIMatch` string, plus `itlwm/Info.plist`.

## Work Guidance

- **`iwx_newstate`'s same-state guard was missing its `return 0` from the day it was added**
  (3e8da1c, which cites an OpenBSD change that has it), and the omission was invisible because the
  `if` silently adopted the following statement as its body. Two inversions at once: same-state
  transitions reached `ieee80211_newstate` instead of being dropped, and the A-MPDU stop plus
  `ba_task`/`mac_ctxt_task`/`chan_ctxt_task` teardown that 6dc344d added — to stop the newstate
  thread racing systq — ran only when `ns_nstate == nstate`, i.e. almost never, and never on the
  `RUN -> SCAN` case its own inner comment describes. Restored, with the history at the site.
  **The shape to watch for: a conditional whose body is on the next line and whose intended
  statement is absent.** No compiler warning, no behavioural clue at the call site, and a diff
  against upstream shows a line that "looks right". Grep this port for `if (...)` whose next
  non-blank line is another statement at the same indent before trusting any similar guard.

- The HAL code tracks OpenBSD upstream. Keep ported functions recognisably close to their
  OpenBSD counterparts so future syncs stay reviewable.
- Where this port has *diverged* from upstream, say so at the site. `iwx_run_init_mvm_ucode`
  replaced upstream's `while ((sc->sc_init_complete & wait_flags) != wait_flags)` loop with a
  single unconditional `tsleep_nsec`, which cannot observe a notification that arrived before
  the sleep. Silent divergences like this are where the bugs are. Why the loop was dropped is
  unknown and scheduled for investigation — see "Tahoe bring-up: temporary mechanisms" in the
  root AGENTS.md. The leftover unused `wait_flags` is the compiler's own hint that something
  was removed rather than rewritten.
- `iwx_post_alive` deliberately does **not** enable the ICT, unlike the `iwm` code this HAL
  inherited the path from. The divergence is commented at the site; see the ICT section below.

### Bring-up markers (`__IO80211_TARGET >= __MAC_26_0` only)

`ItlIwx.cpp` records plain integers into globals that `AirportItlwm` publishes on its
provider — XYLog reaches neither dmesg nor the unified log, and a failing attach leaves no
other trace. Read them with:

```bash
ioreg -r -n IOPCIEDeviceWrapper -l -w0 | grep -i itlwm
```

`ItlwmPreinitMark` names the `iwx_preinit` exit, `ItlwmInitMark` the
`iwx_run_init_mvm_ucode` exit, and `ItlwmPreSleepInitComplete` is `sc_init_complete` sampled
immediately before the INIT_COMPLETE wait — non-zero there with `ItlwmInitMark == 5` is a
lost wakeup rather than an absent notification.

**These are published from two places now, and for years they were published from only one that
could never fire on a working machine.** `publishPreinitMark` is called from exactly one site,
inside the `if (!fHalService->attach(pciNub))` **failure** branch, so on every boot where the
driver works none of these properties existed at all — while root AGENTS.md mechanisms 4 and 8
both say "read this on one surviving boot", which was impossible by construction.
`AirportItlwm::publishRuntimeCounters` now calls it too, on the `AirportItlwm` node:

```bash
ioreg -r -c AirportItlwm -l -w0 | grep -i itlwm      # the surviving-boot copy
ioreg -r -n IOPCIEDeviceWrapper -l -w0 | grep -i itlwm   # the attach-failure copy
```

**Rule: a counter published only from an error path cannot answer a question about success.**

Command-path counters localise a host command that is written to the ring and never
acknowledged (`ItlwmInitMark == 3`). `ItlwmCmdDoneCount` is incremented in `iwx_cmd_done`
**before** its `qid != IWX_DQA_CMD_QUEUE` early return, so a response delivered on the wrong
queue is still counted; `ItlwmCmdDoneLast` carries `(qid << 16) | code`.

| reading | meaning |
| --- | --- |
| `ItlwmIsrCount == 0` after ALIVE | no interrupts reaching the driver at all |
| `IsrCount > 0`, `NotifIntrCount == 0` | interrupts fire, nothing dequeued from the RX ring |
| `NotifIntrCount > 0`, `CmdDoneCount == 0` | RX works; no response routed to the command queue |
| `CmdDoneCount > 0`, `CmdDoneLast` qid wrong | response arrived on an unexpected queue |

`ItlwmCmdDoorbell` is the exact value written to `IWX_HBUS_TARG_WRPTR` (`qid << 16 | cur`),
so a wrong queue id or stale write pointer is visible without a second boot.

### ICT is not used on `iwx` (settled)

`ItlwmIsrCount` climbing while `ItlwmNotifIntrCount` stays flat was traced to the Interrupt
Cause Table. Measured on AX200 (`pci8086,2723`) under Tahoe: `IsrCount` 205,
`IctResetAtIsr` 2, `IctZeroCount` **203** — every single interrupt after ICT was switched on
read `ict[ict_cur] == 0`, and `IctZeroCur` stayed at 0, so `ict_cur` never advanced off the
first slot. The table received nothing from the first interrupt onward. There was never an
index drift to resynchronise; `iwx_ict_reset` ran exactly once and in the right place
(`IctResetCount` 1, `IctResetAtKick` 1).

The 203 are a feedback loop, not 203 separate events: `iwx_intr` leaves via `out_ena`, which
calls `iwx_restore_interrupts` **without** writing `IWX_CSR_INT`, so an unacknowledged cause
re-raises immediately.

`iwx_post_alive` therefore no longer calls `iwx_ict_reset`; it acknowledges pending causes,
leaves `IWX_FLAG_USE_ICT` clear, and enables interrupts. That keeps `iwx_intr` on the
`IWX_CSR_INT` branch — the branch that delivered the `IWX_ALIVE` which reaches this code.
`iwx_ict_reset` itself is retained but unreferenced.

Do not read `ItlwmIctZeroInt` / `ItlwmIctZeroFh` as a desync test. With ICT enabled the
hardware routes causes into the table *instead of* `IWX_CSR_INT`, so zero there is expected
whether or not the table works. The count and `IctZeroCur` are what carried the finding.

Remaining ICT markers, sampled once in `iwx_post_alive`:

| reading | meaning |
| --- | --- |
| `IctResetCount == 0`, `IctZeroCount == 0` | ICT is off; `iwx_intr` is on the `IWX_CSR_INT` path |
| `IctPaddrLo & 0xfff != 0` | root cause of the dead table — `CSR_DRAM_INT_TBL_REG` takes `paddr >> IWX_ICT_PADDR_SHIFT`, so a misaligned IOVA truncates to a different page. No other DMA structure shifts its address, which is why only ICT broke |

**"Then just ask for 4096-byte alignment" is REFUTED — it already does.** `iwx_alloc_ict` passes
`1 << IWX_ICT_PADDR_SHIFT` to `iwx_dma_contig_alloc`, byte-identical to `hal_iwm` and to OpenBSD's
`iwm`. That value reaches only the 6-argument `IODMACommand::withSpecification`, whose alignment
parameter lands in `fAlignMask`. XNU carries **three** alignment fields — `fAlignMask`,
`fAlignMaskLength`, `fAlignMaskInternalSegments` — and only the `SegmentOptions` overload sets all
three; the 6-argument factory leaves the other two at their defaults, so the request constrains the
wrong one. Nothing then checks what comes back: the `>> IWX_ICT_PADDR_SHIFT` is unconditional.
Read `ItlwmIctPaddrLo & 0xfff` on a surviving boot (now possible, see above) before touching the
allocator.

Note `iwx_ict_reset` has **no callers**, so `IWX_FLAG_USE_ICT` is never set: the nine
`ItlwmIct{Zero,Reset}*` counters are provably 0 forever and only `IctPaddrLo` / `IctTblReg` carry
information. They come out with mechanism 4, not before — that boot has not happened yet.
| `IctTblReg & 0x80000000` | `CSR_DRAM_INT_TBL_ENABLE` still set in hardware; causes would go to DRAM and `IWX_CSR_INT` would read 0 |

Everything else comes from `ITLWM_PREINIT_SNAP`, which **must stay ahead of
`iwx_stop_device`**: that call resets every tx ring, zeroing `cur`/`queued`/`tail`. An
earlier version read the rings from the softc afterwards and reported zeros regardless of
what happened, which was misread as "no command was ever sent".

Integer stores only — no formatting, allocation, or locking. Mirroring XYLog call sites
through a `vsnprintf` collector once made the machine unbootable. Remove the whole
apparatus when Tahoe bring-up is finished.

## Verification

```bash
xcodebuild -jobs 8 -target itlwm -configuration Release build
xcodebuild -jobs 8 -target itlwm -configuration Debug build
lipo -info build/Release/itlwm.kext/Contents/MacOS/itlwm
```

HAL changes affect `AirportItlwm` too — build at least one `AirportItlwm-*` target as well.

## Child DOX Index

- No child AGENTS.md files. `hal_iwm/`, `hal_iwn/`, `hal_iwx/`, and `firmware/` are owned
  here.
