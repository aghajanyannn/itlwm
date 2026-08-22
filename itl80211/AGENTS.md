# AGENTS.md — itl80211

## Purpose

The vendored OpenBSD 802.11 stack and the shims that let it compile and run inside the
macOS kernel. Shared unchanged by `itlwm.kext` and `AirportItlwm.kext`.

## Ownership

- `openbsd/net80211/` — the OpenBSD `net80211` stack (`ieee80211*.c/.h`).
- `openbsd/crypto/` — the crypto primitives `net80211` depends on.
- `openbsd/sys/` — BSD kernel headers the stack expects.
- `linux/` — small Linux-flavoured helpers (`bitfield.h`, `kernel.h`, `random.h`, `types.h`)
  used by the newer HALs.
- `compat.cpp/.h` — the macOS kernel compatibility layer (mbuf, timeout, task shims).
- `zutil.c/.h` — zlib support for compressed firmware.

## Local Contracts

- This is vendored upstream code. Prefer importing OpenBSD's fix over writing a local one,
  and keep any local divergence minimal and obvious so the next sync stays reviewable.
- Do not introduce macOS-specific API into `openbsd/`. Anything platform-specific belongs in
  `compat.h`/`compat.cpp`.
- Both kexts compile these files with `IEEE80211_STA_ONLY`. Code paths outside STA mode are
  not built and must not become load-bearing.
- **`ic_myaddr` is not an alias of `ic_ac.ac_enaddr`, and the copy between them is ONE-WAY.**
  `ieee80211_ifattach` copies `ic_myaddr` → `ac_enaddr` once and nothing ever copies back, while
  `if_setlladdr` writes `ac_enaddr` only. So a link address assigned from outside lands on the BSD
  side alone unless a driver writes `ic_myaddr` itself. That matters because `ic_myaddr` is what
  802.11 actually runs on: the source address of every transmitted frame (`ieee80211_output.c`), the
  PTK derivation input (`ieee80211_pae_input.c`), and the RX "is this frame for me" filter
  (`ieee80211_input.c`). The failure is L3-only and looks nothing like an 802.11 problem — the
  association and four-way handshake both succeed, because 802.11 is self-consistent on the *old*
  address, and only ARP/DHCP and unicast replies to the *new* one are lost.
  Nothing here needs changing; the sync lives in `AirportItlwm`, which owns the address the family
  assigns. Recorded so the next reader does not diagnose it as a net80211 bug.
- **Known local divergence: the `ic_event_handler` hook.** Not upstream. `ieee80211com` carries a
  `void (*ic_event_handler)(struct ieee80211com *, int, void *)` that the stack calls at a handful
  of points so a driver can raise an `IEEE80211_EVT_*`; `AirportItlwm` turns those into
  apple80211 messages. Call sites: `ieee80211_input.c` (assoc done, deauth, and
  `ieee80211_notify_scan_beacon` for scan-time beacons/probe responses),
  `ieee80211_node.c` (scan done). Add to this mechanism rather than inventing a second hook, and
  keep each call site a one-liner next to the upstream code it observes so a sync still diffs
  cleanly. `IEEE80211_EVT_SCAN_BEACON` hands out a pointer into the receive mbuf and is gated on
  scanning — see `AirportItlwm/AGENTS.md` for why the payload must be copied by the handler.
- **Known local divergence: `ic_rx_beacons`.** Not upstream. net80211 tracks a beacon miss
  *threshold* (`ic_bmissthres`) but never counts the beacons it receives, and macOS needs that
  count: the WCL tears a connection down 60 s after link-up unless `AirportItlwm` keeps posting
  driver message 39, whose only mandatory field is "beacons since the last update". Incremented in
  `ieee80211_count_bss_beacon` (`ieee80211_input.c`), called from the `BEACON` case of
  `ieee80211_recv_mgmt`. Free-running; consumers take deltas. **The BSSID match is the point of the
  function** — it counts only beacons whose `i_addr3` equals `ic_bss->ni_bssid`, and only in RUN,
  because counting a neighbour's beacons would keep a dead link looking alive to macOS. Deliberately
  the complement of `ieee80211_notify_scan_beacon` beside it, which wants every AP while scanning.
- **Known local divergence: the `ic_scan_*` BSS-selection counters.** Not upstream.
  `ieee80211_node_choose_bss` fills `ic_scan_cand`, `ic_scan_skipped`, `ic_scan_fail_or` and
  `ic_scan_fail_des` each pass; `AirportItlwm` publishes them as `ItlwmScan*`. Upstream already
  computes the reason a candidate is rejected — `ieee80211_match_bss` returns an
  `IEEE80211_NODE_ASSOCFAIL_*` mask — and then throws it away into a `DPRINTF` that has **no sink
  on this platform**. A join that stalls in SCAN therefore reports nothing but "it did not work",
  at every layer above. `ic_scan_fail_des` is the useful one: `0x8000 | mask` means the target
  ESSID was in the node cache and `match_bss` rejected it for the named reason; plain `0` means the
  target was never seen, which is a scan problem rather than a selection one. Sampled **only on
  passes where `ic_des_esslen != 0`** — auto-join and background passes run constantly with no
  target and reject everything trivially, and letting those overwrite the sample buries the pass
  that was trying to join.
- **Known local divergence: `ic_ess_clears` / `ic_ess_clear_state`.** Not upstream. Counted in
  `ieee80211_deselect_ess`, the single choke point for `ic_des_esslen = 0`. An ESS cleared moments
  after a join programmed it kills that join silently, and the callers are spread across net80211,
  the ioctl path and `AirportItlwm`, so excluding them by reading is slow and error-prone.
  `ic_ess_clear_state` is `ic_state` at the last clear: `IEEE80211_S_AUTH`/`S_ASSOC` means
  `ieee80211_watchdog` gave up, anything else means a caller took the ESS away mid-join.
- **Known local divergence: the `ic_scan_rsn_*` breakdown.** Not upstream.
  `ieee80211_match_bss` folds **eight** independent RSN tests into a single
  `IEEE80211_NODE_ASSOCFAIL_WPA_PROTO` bit, so a caller learns only "the parameters do not
  overlap". Each test now also sets its own `IEEE80211_RSNFAIL_*` bit in `ic_scan_rsn_last`, which
  `ieee80211_node_choose_bss` latches into `ic_scan_rsn_des` for the target, alongside both sides
  of the comparison in `ic_scan_ni_*` / `ic_scan_ic_*` — an empty intersection says nothing about
  which side is wrong. The flag assignments sit beside the existing lines rather than restructuring
  the function, so a diff against OpenBSD still lines up.

- **`struct ieee80211com` is the first member of every HAL's softc, and `ic_ess` is its last
  member.** Adding or removing a field shifts every `sc_*` offset in `iwx_softc`/`iwm_softc`/
  `iwn_softc`. An object file built against a stale `ieee80211_var.h` therefore writes its softc
  fields straight over the tail of `ieee80211com` — the ESS list head — and the next
  `ieee80211_switch_ess` general-protection-faults inside `ieee80211_match_ess` on a pointer made
  of instruction bytes. **It reads as a net80211 bug and is not one.** `gItlwmIcSizeNet`
  (`ieee80211_node_attach`) and `gItlwmIcSizeHal` (`iwx_attach`) publish each translation unit's
  view as `ItlwmIcSizeNet` / `ItlwmIcSizeHal`; they must be equal. **Build clean after any change
  to this struct**, and check those two first if the driver faults somewhere it has no business
  faulting.

## Work Guidance

- When porting an OpenBSD change, keep the upstream function name, structure, and comments
  so a later diff against OpenBSD still lines up.
- **`malloc()` in this port's shim zeroes unconditionally, so `M_ZERO` is redundant, not absent.**
  `itl80211/openbsd/sys/_malloc.h` ends every allocation with `bzero(buf, len)` whatever flags the
  caller passed — strictly stronger than upstream's `M_ZERO`, not weaker. Do **not** add a `memset`
  after a ported `malloc`; one was added to `ieee80211_add_ess` on the opposite belief and was dead
  code. This entry previously asserted the opposite and blamed that function's corruption on
  uninitialised heap; the real bug was the dangling-list `free` below, which is fixed independently.
- **Freeing a structure that a list still points at is the same bug however upstream wrote it.**
  `ieee80211_add_ess`'s error paths freed `ess` unconditionally, but `ess` is the *existing* entry
  when the lookup above found one — still linked into `ic_ess`. The dangling pointer then killed
  the machine one `ieee80211_switch_ess` later, arbitrarily far from the free. Free only what the
  call allocated.
- **`if_start` can silently do nothing, and net80211 assumes it cannot.** `ieee80211_send_mgmt`
  enqueues to `ic_mgtq` and calls `ifp->if_start`, then treats the frame as sent. In this port
  `if_start` is `ItlIwx::iwx_start`, which is `getMainCommandGate()->attemptAction(...)` — a
  **non-blocking** gate acquisition that returns `kIOReturnCannotLock` and runs nothing when
  another thread holds the gate. `_iwx_start_task` exits the same way on `ifq_is_oactive`, and its
  `qfullmsk` check sets `oactive` *before* the management dequeue, so a full data queue strands
  management frames. Nothing re-drives them: `ifp->if_timer` is armed only after a *successful*
  transmit. This stranded every Tahoe association request in `ic_mgtq` while net80211 waited in
  ASSOC for a response the AP was never asked for. Anything queued to `ic_mgtq` needs a retry that
  does not depend on the frame having gone out.
- **Known local divergence: `ieee80211_set_link_state` never reports the link down while
  `ic_state == IEEE80211_S_RUN`.** `ieee80211_newstate` drops the link at the top of *every*
  transition, before it knows the new state, and relies on the `RUN` case to raise it again — but
  that case only raises it when RSN is off or the AKM is 802.1X. Under RSN the link-up is deferred
  to `ni_port_valid`, which the four-way and group handshakes set **exactly once**. So the
  unconditional drop is bookkeeping, not information, and on the `ASSOC -> RUN` transition that
  *completes* an association it is actively false: `ic_state` is already `RUN` when it fires.
  Guarded in `ieee80211_set_link_state`, the one function that owns `if_link_state`, so a genuine
  disconnect — which always leaves RUN first — still reports normally.
  Measured cause: `(ostate ASSOC, nstate RUN, mgt 0x10 ASSOC_RESP)`. This was nearly invisible while
  `ifconfig`'s media status was the only consumer. It is not now: `AirportItlwm` forwards the link
  state to Tahoe's WCL as message 216, and `WCLNetManager` answers a link-down indication by
  leaving the network — so a completed WPA2 association died ~57 ms after the handshake.
  **A signal that was merely cosmetic can become load-bearing when a new consumer is added; re-read
  what raises it, not just what reads it.**
  Two earlier attempts at this were wrong and are worth not repeating: suppressing only `RUN -> RUN`
  in `ieee80211_newstate` (too narrow — the real edge was `ASSOC -> RUN`), and filtering in
  `AirportItlwm::setLinkStatus` instead (leaves `if_link_state` stuck DOWN, so the *next* genuine
  disconnect is silently swallowed). The predicate belongs where `if_link_state` is owned.
  A comment block describing the first of those attempts survived in `ieee80211_newstate`, directly
  above the unconditional drop it claimed to have replaced, and has been deleted. **A comment that
  describes a divergence must sit at the code that implements it**; left at the site of a reverted
  attempt it reads as documentation of behaviour that is not there.
- **A net80211 entry point the HAL also calls is not a free function.** Before calling any
  `ieee80211_*` from `AirportItlwm/` or `itlwm/`, grep the HALs for it: if a HAL calls it too,
  whatever the HAL does *around* the call is part of the contract. `ieee80211_end_scan` is the
  worked example — `ItlIwx::iwx_endscan` clears `IWX_FLAG_SCANNING` before calling it, and a
  direct call from the driver layer left the firmware scanning while net80211 walked to `AUTH`,
  so every association went out with the radio off-channel. The same shape applies to any
  net80211 helper that *resets* state on entry: `associateSSID`'s `ieee80211_disable_rsn` zeroes
  `ic_psk`, so a caller that left a key there rather than passing it in loses it silently.

## Verification

Changes here affect both kexts:

```bash
xcodebuild -jobs 8 -target itlwm -configuration Release build
xcodebuild -jobs 8 -target "AirportItlwm-Sonoma14.4" -configuration Release build
```

## Child DOX Index

- No child AGENTS.md files. `openbsd/` and `linux/` are owned here.
