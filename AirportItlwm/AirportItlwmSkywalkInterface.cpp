//
//  AirportItlwmSkywalkInterface.cpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//
#include "AirportItlwmV2.hpp"
#include "AirportItlwmSkywalkInterface.hpp"
#include <sys/CTimeout.hpp>
#include <libkern/c++/OSMetaClass.h>
#include <crypto/sha1.h>
#include <net80211/ieee80211_node.h>
#include <net80211/ieee80211_ioctl.h>
#include <net80211/ieee80211_priv.h>

#define super IO80211InfraProtocol
OSDefineMetaClassAndStructors(AirportItlwmSkywalkInterface, IO80211InfraProtocol);

#if __IO80211_TARGET >= __MAC_26_0

// Two RegistrationInfo buffers are mandatory here, and they are different structs.
// IOSkywalkNetworkInterface::prepareBSDInterface, with no null check anywhere:
//
//     mov rax, [rdi + 0xc0]     ; mExpansionData
//     mov [rax + 8], rsi        ; ->fBSDInterface = ifp
//     mov rax, [rax]            ; ->fRegistrationInfo
//     mov esi, [rax + 0x4c]     ; MTU   <-- faults on CR2 = 0x4c when NULL
//
// and IOSkywalkEthernetInterface::prepareBSDInterface, after its super call returns:
//
//     mov rcx, [r15 + 0x118]    ; mExpansionData2
//     mov rcx, [rcx]            ; ->fRegistrationInfo
//     mov [rcx + 0x20], eax     ; WRITE  <-- faults on CR2 = 0x20 when NULL
//
// Neither is optional, and they are not the only readers: scanning both families for the
// `[this+0xc0]`/`[this+0x118]` load followed by a dereference finds ~15, among them
// ioctl_sifcap, ioctl_gifmedia and ioctl_sifmedia, which run in the ifnet_ioctl that
// IONetworkStack::attachNetworkInterfaceToBSD issues moments after this. Rerun that scan
// before assuming any such field is safe to leave NULL.
//
// ETHERNET SIDE — Apple allocates and owns it. copyRegistrationInfo is the standalone
// allocator registerEthernetInterface itself calls first: it IOMallocTypes the buffer, copies
// 0x130 bytes from the argument, and fills the MAC at +0x108. It validates info->[0] == 1 and
// info->[4] >= 0x130, which is exactly what initRegistrationInfo writes. No pool, queue or
// logical link needed.
//
// NETWORK SIDE — allocated by registerNetworkInterface, which this driver now genuinely reaches:
// it needs a logical link, which needs a queue set, which needs real IOSkywalkPacketQueue objects,
// and buildSkywalkDataPath() builds exactly those (root AGENTS.md mechanism 1).
//
// Both fields are therefore non-NULL by the time prepareBSDInterface runs, and this override only
// has to cover the ethernet side's allocator call. It used to lend kext statics for both when the
// fields came up NULL; that stopgap was measured never to fire on 26.6.2 —
// `ItlwmRegInfoLentNet = 0` and `ItlwmRegInfoLentEth = 0` with `ItlwmSkywalkStage = 11` — and was
// deleted with its two counters, its MTU-offset poke, and the reclaim hooks that free(), stop()
// and deregisterLogicalLink() used to carry.
//
// DO NOT hand-allocate either field if one ever comes up NULL again. Apple frees them with
// IOFreeType from early.kalloc.288, a zone no kext can allocate into — IOMalloc lands in
// kalloc.type.var4.*, IOMallocData in data.kalloc.*, and "early" is a boot-ordering property, not
// a type property. An earlier attempt to own the buffer panicked on the free.

bool AirportItlwmSkywalkInterface::
prepareBSDInterface(ifnet_t ifp, UInt flags)
{
    if (mExpansionData == NULL || mExpansionData2 == NULL)
        return gatedSuperPrepareBSDInterface(ifp, flags);

    // Ethernet side: have Apple allocate and own it, the way registerEthernetInterface would.
    // Success is checked by outcome — did the field become non-NULL — rather than by these
    // reconstructed return types, which are guesses.
    if (mExpansionData2->fRegistrationInfo == NULL) {
        IOSkywalkEthernetInterface::RegistrationInfo info;
        memset(&info, 0, sizeof(info));
        initRegistrationInfo(&info, 1, sizeof(info));
        copyRegistrationInfo(&info);
    }
    return gatedSuperPrepareBSDInterface(ifp, flags);
}

void AirportItlwmSkywalkInterface::
free()
{
    teardownSkywalkDataPath();
    super::free();
}

void AirportItlwmSkywalkInterface::
stop(IOService *provider)
{
    teardownSkywalkDataPath();
    super::stop(provider);
}

IOReturn AirportItlwmSkywalkInterface::
deregisterLogicalLink(void)
{
    // Nothing to do here any more — this existed to take the RegistrationInfo loan back, and the
    // loan is gone. Kept as a deliberate slot pin rather than deleted: an inherited slot is a hole
    // the kext loader fills at load time and can fill wrong, and define-and-forward leaves no hole
    // (see the vtable-hole contract in AirportItlwm/AGENTS.md). Costs one forwarding call on a
    // teardown path.
    return super::deregisterLogicalLink();
}

// =================================================================================================
// Real Skywalk registration — root AGENTS.md mechanism 1. Unconditional on Tahoe.
//
// STAGED AND UNPROVEN. This builds the pool, the four queues and calls
// registerInfraEthernetInterface. It does NOT yet move traffic: the callbacks below are stubs and
// the driver's data path still runs over the legacy AirportItlwmEthernetInterface. Registration
// SUCCEEDING is therefore not the same as this working — IOSkywalkNetworkBSDClient will publish a
// second, nexus-backed ifnet that nothing feeds. Do not enable the boot-arg on a machine you need.
//
// Recipe transcribed from AppleEthernetRL::startInterface, which does the whole thing in one
// function; AppleBCMWLANSkywalkInterface::start does the same work spread across an ivar bundle.
// Pool sizes are derived the way the producer derives them — from each queue class's own
// getEffectiveCapacity — rather than chosen, because a pool too small for the rings fails in the
// data path rather than at construction.
// =================================================================================================
extern "C" {
uint32_t gItlwmSkywalkStage;        // how far buildSkywalkDataPath/registerSkywalkInterface got
uint32_t gItlwmSkywalkRegRet;       // registerInfraEthernetInterface's IOReturn
uint32_t gItlwmSkywalkTxDequeue;    // callback hit counters. TX is implemented, so TxDequeue is
uint32_t gItlwmSkywalkTxComplete;   // expected to climb once traffic flows; the RX pair must stay
uint32_t gItlwmSkywalkRxDequeue;    // 0 until the RX path exists, and a non-zero one there means
uint32_t gItlwmSkywalkRxComplete;   // the family is driving queues this driver is not filling.
uint32_t gItlwmSkywalkTxFrames;     // frames actually handed to net80211
uint32_t gItlwmSkywalkTxDrops;      // consumed but discarded (link down, or an unusable descriptor)
uint32_t gItlwmSkywalkTxNoMbuf;     // mbuf_allocpacket failed under the gate
uint32_t gItlwmSkywalkTxComplFail;  // enqueuePackets refused; the pool leaks if this is non-zero
uint32_t gItlwmSkywalkTxListShort;  // list ran out before `count` — must stay 0; see the call site
uint32_t gItlwmSkywalkRxFrames;     // frames delivered up the RX completion queue
uint32_t gItlwmSkywalkRxDrops;      // buffer taken but unusable; returned to the free list
uint32_t gItlwmSkywalkRxNoBuf;      // no lent buffer available — climbing with RxFrames flat is a
                                    // starved RX ring, which is what a broken submission path
                                    // looks like from here
uint32_t gItlwmSkywalkRxOversize;   // frame longer than the RX buffer
uint32_t gItlwmSkywalkRxComplFail;  // enqueuePackets refused
uint32_t gItlwmSkywalkRxFree;       // buffers currently lent to us and unfilled
uint32_t gItlwmSkywalkRxListShort;  // as TxListShort, on the RX submission list
uint32_t gItlwmSkywalkBsdUnit = ~0u; // BSD unit seeded into RegistrationInfo+0x38; ~0 = not seeded
uint32_t gItlwmSkywalkQueuesAdded;  // queues attached to the work loop; MUST be 4, or nothing polls them
uint32_t gItlwmSkywalkQueuesEnabled; // queues enable()d; MUST be 4. A queue is born DISABLED and a
                                     // disabled queue silently refuses every submission, dequeue and
                                     // completion — see the header comment on IOSkywalkPacketQueue.
uint32_t gItlwmSkywalkRxPrimeCalls; // requestDequeue calls on the RX submission queue
uint32_t gItlwmSkywalkRxPrimeRet;   // its last IOReturn; 0xe00002d7 = the queue was still disabled
uint32_t gItlwmSkyLinkReportCalls;  // reportLinkStatus calls into the Skywalk layer
uint32_t gItlwmSkyLinkReportRet;    // its last IOReturn; 0xe00002d8 = getBSDInterface() was NULL
uint32_t gItlwmSkywalkRxFallbackDrops; // frames dropped because Skywalk owns the ifnet and could
                                       // not take them; MUST stay 0 on a healthy boot
uint32_t gItlwmLlAddrCalls;         // setLinkLayerAddress (slot 335) calls — the family telling us
                                    // it changed the link address. 0 means net80211 never learned.
uint32_t gItlwmLlAddrSynced;        // of those, how many actually moved ic_myaddr
uint32_t gItlwmLlAddrLate;          // ...while ic_state was RUN, i.e. under a live association, so
                                    // the change cannot take effect without re-associating
uint64_t gItlwmLlAddrLast;          // the whole last address we were handed, as a 48-bit big-endian
                                    // integer. A uint32 of the low four bytes printed as
                                    // 18446744072000957813 in ioreg — sign-extended and unreadable,
                                    // and it dropped the OUI, which is the half that says whether
                                    // the address is locally administered.
}

// The two fields of IOSkywalkEthernetInterface::RegistrationInfo this driver writes by hand. The
// struct stays opaque on purpose (see the field map in its header); naming the offsets here keeps
// the writes explicit and greppable rather than hiding literals in casts.
//
// Both exist for the same reason: initRegistrationInfo seeds each of them by calling the getter
// that reads that very field back out of the currently installed struct, so both propagate
// whatever was there and neither is ever allocated. Left alone they stay 0, which means unit 0
// ("en0") and subfamily 0 (WIRED).
#define ITLWM_REGINFO_BSD_UNIT_OFFSET  0x38
#define ITLWM_REGINFO_SUBFAMILY_OFFSET 0x0c
// IFNET_SUBFAMILY_WIFI. The family field (+0x08) stays 2 = IFNET_FAMILY_ETHERNET, which is
// correct: _if_functional_type reports WIFI_INFRA only for family ETHERNET *and* subfamily WIFI.
#define ITLWM_REGINFO_SUBFAMILY_WIFI   3
// The link address, and the THIRD field in this struct with the propagate-never-allocate shape.
// registerInfraEthernetInterface stamps it from getSelfMacAddr() -- but only when BOTH
// [this+0x128]->byte[0x3c50] & 1 and [this+0x120]->dword[0x58] == 1. Registration proceeds either
// way and returns success, so a failed stamp is invisible: the ifnet attaches with no address and
// something downstream synthesises a random locally-administered one, fresh on every boot.
// Measured on 26.6 as en3 coming up with ee:a6:14:c7:09:55 and then 6a:88:fb:80:b5:71 while the
// card's real address sat correctly on the IOSkywalkLegacyEthernet node -- two addresses on one
// device, which looks exactly like a macOS Private Wi-Fi Address and is not one.
#define ITLWM_REGINFO_MAC_OFFSET       0x108

// THE SKYWALK INTERFACE OWNS THE BSD IFNET ON TAHOE, UNCONDITIONALLY. There is no boot-arg.
//
// This was staged behind `-itlskywalk` / `-itlskywalkreg` / `-itlskywalkbsd` while it was being
// brought up, because the failure mode is "no network" on a machine whose kext is injected by
// OpenCore — recovery is an EFI edit. All three are gone now that the path is measured working end
// to end: registration, the legacy-ethernet bridge, SystemConfiguration visibility, association,
// a live TX/RX data path and a DHCP lease.
//
// Deliberately NOT kept as a failsafe. An alternate path that nothing exercises is not a failsafe,
// it is untested code that will have rotted by the time anyone reaches for it — and keeping it
// means every change here has to be reasoned about twice. If a future change needs staging, give
// *that* change its own boot-arg and delete it when it lands.
//
// The recovery path is now the previous kext, not a boot-arg: keep the last known-good
// AirportItlwm.kext on the EFI beside the new one, and check `scripts/kextuuid.py --expect` if
// there is any doubt about which one booted.
//
// Everything from the top of this file to the matching #endif is `__IO80211_TARGET >= __MAC_26_0`,
// so no pre-Tahoe target is affected by any of this.

// The BSD unit the Skywalk interface should attach as.
//
// Nothing in the Skywalk path allocates one: IOSkywalkNetworkBSDClient::start formats the name
// as snprintf("%s%u", getBSDNamePrefix(), getBSDUnitNumber()) and calls setBSDName with the
// result, so the number is used verbatim. IOSkywalkNetworkInterface::getBSDUnitNumber just
// returns RegistrationInfo+0x38, and initRegistrationInfo seeds that field *from itself* — it
// propagates a unit, it never picks one. So the driver has to.
//
// Picking the first free enN mirrors what IONetworkStack does for the legacy path (which is how
// this driver's own ethernet interface became en3), and avoids hardcoding a number that is only
// correct on one machine. A negative unit is NOT a way to ask for allocation: the name is built
// before the sign is ever tested, so -1 yields the name "en4294967295".
static int32_t
itlwmPickFreeBSDUnit(const char *prefix)
{
    char name[IFNAMSIZ];

    for (int32_t unit = 0; unit < 16; unit++) {
        ifnet_t ifp = NULL;
        snprintf(name, sizeof(name), "%s%d", prefix, unit);
        if (ifnet_find_by_name(name, &ifp) != 0)
            return unit;                    // nothing owns this name
        // Found one: it holds a reference, so give it back before trying the next.
        if (ifp != NULL)
            ifnet_release(ifp);
    }
    return -1;
}

#define ITLWM_SKYWALK_RING     0x100   // AppleEthernetRL's requested ring size
#define ITLWM_SKYWALK_TXBUFSZ  0x10000 // its tx pool buffer size
#define ITLWM_SKYWALK_RXBUFSZ  0x4000  // and its rx pool buffer size
// Refill the RX free list once it falls this low. A quarter of the ring: high enough that an
// asynchronous pull has time to land before the list empties, low enough not to ask on every frame.
#define ITLWM_SKYWALK_RXLOWAT  (ITLWM_SKYWALK_RING / 4)

bool AirportItlwmSkywalkInterface::
buildSkywalkDataPath()
{
    IOSkywalkPacketBufferPool::PoolOptions opts;

    gItlwmSkywalkStage = 1;

    // Before anything can be handed to us. rxSubmissionDequeue refuses while this is NULL, which
    // would otherwise be a silent no-receive rather than a visible failure.
    if (fRxFreeLock == NULL) {
        fRxFreeLock = IOSimpleLockAlloc();
        if (fRxFreeLock == NULL)
            return false;
    }

    // ---- tx pool -------------------------------------------------------------------------------
    memset(&opts, 0, sizeof(opts));
    opts.bufferSize = ITLWM_SKYWALK_TXBUFSZ;
    opts.packetCount = IOSkywalkTxSubmissionQueue::getEffectiveCapacity(ITLWM_SKYWALK_RING) +
                       IOSkywalkTxCompletionQueue::getEffectiveCapacity(ITLWM_SKYWALK_RING);
    opts.bufferCount = opts.packetCount;
    opts.maxBuffersPerPacket = 1;
    opts._unk14 = 1;
    fTxPool = IOSkywalkPacketBufferPool::withName("ItlwmTx", this, 1, &opts);
    if (fTxPool == NULL)
        return false;
    gItlwmSkywalkStage = 2;

    // ---- rx pool -------------------------------------------------------------------------------
    memset(&opts, 0, sizeof(opts));
    opts.bufferSize = ITLWM_SKYWALK_RXBUFSZ;
    opts.packetCount = IOSkywalkRxSubmissionQueue::getEffectiveCapacity(ITLWM_SKYWALK_RING) +
                       IOSkywalkRxCompletionQueue::getEffectiveCapacity(ITLWM_SKYWALK_RING);
    // AppleEthernetRL doubles the rx count; it feeds both rx rings out of the one pool.
    opts.packetCount *= 2;
    opts.bufferCount = opts.packetCount;
    opts.maxBuffersPerPacket = 1;
    opts._unk14 = 1;
    fRxPool = IOSkywalkPacketBufferPool::withName("ItlwmRx", this, 1, &opts);
    if (fRxPool == NULL)
        return false;
    gItlwmSkywalkStage = 3;

    // ---- queues --------------------------------------------------------------------------------
    // Note IOSkywalkRxSubmissionQueue::withPool takes an EXTRA uint the other three do not. Copying
    // the Tx shape here would shift every later argument one register left, which the compiler
    // cannot catch because the callback is a plain function pointer.
    // BOTH SUBMISSION QUEUES RUN IN LIST MODE, AND THE HANDLER'S POINTER CONSTNESS IS WHAT SELECTS
    // IT — not the trailing `options` word, which is 0 here on purpose. See the note above
    // IOSkywalkTxSubmissionQueue in IOSkywalkDataPath.h: Apple ships two overloads per factory that
    // differ only in that qualifier, and the non-const one is a shim that ORs the mode bit in for
    // us. The bit is a DIFFERENT VALUE per queue (TX 8, RX 2), so setting it by hand is how this
    // driver came to run legacyDequeue against a list handler and trap on the first frame it sent.
    //
    // Do not switch either queue to the const overload without rewriting the matching handler: the
    // two modes disagree about what `packets` is, and no build or ABI check in this repo can see the
    // difference. The compiler does check the one thing it can — a handler whose `packets` constness
    // does not match the overload will not bind — which is the whole reason to select the mode this
    // way rather than with a constant.
    //
    // NULL is the optional budget callback and -1 the trailing reserved word: both are exactly what
    // Apple's short const overload hardcodes. There is no short non-const TX overload, which is the
    // only reason this one call reads longer than the other three.
    fTxSubQ = IOSkywalkTxSubmissionQueue::withPool(fTxPool, ITLWM_SKYWALK_RING, 0, this,
                                                   NULL, txSubmissionDequeue, this,
                                                   0, (uint32_t)-1);
    if (fTxSubQ == NULL)
        return false;
    gItlwmSkywalkStage = 4;

    fTxComplQ = IOSkywalkTxCompletionQueue::withPool(fTxPool, ITLWM_SKYWALK_RING, 0, this,
                                                     txCompletionEnqueue, this, 0);
    if (fTxComplQ == NULL)
        return false;
    gItlwmSkywalkStage = 5;

    // List mode again, selected the same way — rxSubmissionDequeue takes `IOSkywalkPacket **`, so
    // this binds Apple's non-const overload, whose shim ORs RX's own mode bit (2). See above.
    fRxSubQ = IOSkywalkRxSubmissionQueue::withPool(fRxPool, ITLWM_SKYWALK_RING, 1, 0, this,
                                                   rxSubmissionDequeue, this, 0);
    if (fRxSubQ == NULL)
        return false;
    gItlwmSkywalkStage = 6;

    fRxComplQ = IOSkywalkRxCompletionQueue::withPool(fRxPool, ITLWM_SKYWALK_RING, 0, this,
                                                     rxCompletionEnqueue, this, 0);
    if (fRxComplQ == NULL)
        return false;
    gItlwmSkywalkStage = 7;

    // ---- the step the recipe records and this driver skipped -------------------------------
    // Every queue is an IOEventSource (slot 36 is checkForWork, 37 setWorkLoop), and an event
    // source that belongs to no work loop is never polled. Constructing them, handing them to
    // registration and stopping there produces a data path that looks completely assembled from
    // the outside — nexus, flowswitch, queue set, logical link all present and holding these very
    // queues — and moves not one packet. That was measured on 26.6 across several boots.
    //
    // AppleEthernetRL::startInterface does this between building the queues and registering them,
    // which is why the recovered recipe in the root AGENTS.md says "each queue added to the work
    // queue, then the array passed straight to registration".
    IOWorkLoop *wq = getWorkQueue();
    if (wq == NULL)
        return false;
    IOSkywalkPacketQueue *const queues[4] = { fTxSubQ, fTxComplQ, fRxSubQ, fRxComplQ };
    for (int i = 0; i < 4; i++) {
        if (wq->addEventSource(queues[i]) != kIOReturnSuccess)
            return false;
        gItlwmSkywalkQueuesAdded++;
    }
    gItlwmSkywalkStage = 9;
    return true;
}

bool AirportItlwmSkywalkInterface::
registerSkywalkInterface()
{
    IOSkywalkEthernetInterface::RegistrationInfo info;
    IOSkywalkPacketQueue *queues[4];

    if (!buildSkywalkDataPath()) {
        teardownSkywalkDataPath();
        return false;
    }

    // Apple passes tx queues first, then the two rx ones, and nQueues = nTx + 2.
    queues[0] = fTxSubQ;
    queues[1] = fTxComplQ;
    queues[2] = fRxSubQ;
    queues[3] = fRxComplQ;

    memset(&info, 0, sizeof(info));
    initRegistrationInfo(&info, 1, sizeof(info));

    // Seed the BSD unit. initRegistrationInfo has just filled +0x38 by calling getBSDUnitNumber(),
    // which reads the *currently installed* registration info's +0x38 — so it copies whatever was
    // there (0) rather than allocating. Overwrite it with a unit that is actually free, or the
    // interface attaches as "en0" and collides with a real one. Field map and the disassembly this
    // comes from are in include/Airport/IOSkywalkEthernetInterface.h.
    //
    // Only when we are the ones attaching. With the legacy BSD attach still in charge this value is
    // never consumed, and writing a plausible-looking unit into a struct nobody reads is how a
    // wrong value survives long enough to be trusted.
    // Seed the link address unconditionally: unlike the unit and the subfamily below, this is not a
    // mode choice, and a correct MAC in the struct is never wrong. Do it whether or not the stamp
    // branch would also run -- it writes the same bytes, and we cannot observe its two guards.
    if (fHalService != NULL) {
        struct ieee80211com *ic = fHalService->get80211Controller();
        if (ic != NULL)
            memcpy((uint8_t *)&info + ITLWM_REGINFO_MAC_OFFSET, ic->ic_myaddr, ETHER_ADDR_LEN);
    }

    // WHICH BSD PATH THE FAMILY TAKES IS DECIDED BY THIS ONE FIELD, and the choice is a
    // designed, mutually exclusive fork rather than a tuning knob:
    //
    //   unit >= 0  -> registerNetworkInterface publishes `IOInterfaceUnit`.
    //                 IOSkywalkNetworkBSDClient::start REQUIRES that property, so it attaches
    //                 and publishes a nexus-backed ifnet. But IOSkywalkLegacyEthernet::probe
    //                 REFUSES a provider that has it, so no IONetworkInterface is ever created
    //                 -- and SystemConfiguration enumerates IONetworkInterface, so the device
    //                 is invisible to `networksetup`, has no hardware port and no Wi-Fi
    //                 service. Measured on 26.6: en2 worked at every layer below SC and could
    //                 not be configured.
    //   unit <  0  -> the property is not published at all (registerNetworkInterface skips it
    //                 when negative). IOSkywalkNetworkBSDClient::start then fails its own
    //                 precondition and IOSkywalkLegacyEthernet::probe SUCCEEDS, giving an
    //                 IOSkywalkLegacyEthernetInterface -- an IOEthernetInterface, hence an
    //                 IONetworkInterface -- which IONetworkStack names and SC can see.
    //
    // We take the negative branch. A Wi-Fi device that macOS cannot configure is not useful,
    // and IONetworkStack allocating the unit is also what lets the interface keep the name the
    // legacy path used to get, instead of orphaning the user's Wi-Fi service.
    //
    // itlwmPickFreeBSDUnit is deliberately still called, for its log line only: if this ever
    // needs to move back to the positive branch, the unit it would have taken is the value to
    // use, and having it recorded costs one ifnet walk at start.
    const char *prefix = getBSDNamePrefix();
    int32_t wouldBe = itlwmPickFreeBSDUnit(prefix ? prefix : "en");
    int32_t unit = -1;
    *(int32_t *)((uint8_t *)&info + ITLWM_REGINFO_BSD_UNIT_OFFSET) = unit;
    gItlwmSkywalkBsdUnit = (uint32_t)wouldBe;

    // Same story as the unit, and this is mechanism 10's real fix rather than its stopgap:
    // the interface subfamily is RegistrationInfo+0x0c, initRegistrationInfo seeds it from
    // getInterfaceSubFamily() which reads that same field, so it stays 0 = WIRED. Measured:
    // the first boot on this path published en2 with `type: Ethernet`.
    //
    // This replaced a raw poke at ifnet+0x22c after the fact (root AGENTS.md mechanism 10, now
    // closed): the documented Skywalk route Apple's own drivers take, a struct field rather than
    // a release-specific ifnet offset, and applied before the ifnet is created instead of
    // patched afterwards.
    *(uint32_t *)((uint8_t *)&info + ITLWM_REGINFO_SUBFAMILY_OFFSET) =
        ITLWM_REGINFO_SUBFAMILY_WIFI;

    IOLog("itlskywalk: unit=-1 (legacy-ethernet bridge path), free %s unit would be %d, subfamily=wifi\n", prefix ? prefix : "en", wouldBe);

    IOReturn ret = registerInfraEthernetInterface(&info, queues, 4, fTxPool, fRxPool);
    gItlwmSkywalkRegRet = (uint32_t)ret;

    // IOLog, not XYLog: XYLog has no sink on the target machine, but IOLog reaches the unified
    // log. itldefer means start() runs long after the GUI is up, so this is readable with
    //   /usr/bin/log show --last 10m --predicate 'process == "kernel"' | grep itlskywalk
    // without a reboot-and-read-ioreg cycle. Apple's own registerInfraEthernetInterface logs
    // "Override mac address for infra interface" only when it takes the MAC-stamp branch, so
    // that line's ABSENCE next to ours means the two state guards refused, not that we never ran.
    IOLog("itlskywalk: register ret=0x%08x stage=%u pools=%p/%p q=%p/%p/%p/%p\n",
          (unsigned)ret, gItlwmSkywalkStage, fTxPool, fRxPool,
          fTxSubQ, fTxComplQ, fRxSubQ, fRxComplQ);

    if (ret != kIOReturnSuccess) {
        teardownSkywalkDataPath();
        return false;
    }
    fSkywalkRegistered = true;
    // Registration is the last thing that can fail with a return code; everything past here is
    // driver-side setup, so give it its own stage rather than folding it into the next one — a
    // stage that stops at 10 says "registered, then enable faulted", which no counter else would.
    gItlwmSkywalkStage = 10;

    // ---- the SECOND step nothing in the family does for you ---------------------------------
    // A queue is constructed disabled (initWithPool clears IOEventSource::enabled right after init
    // sets it), and every entry point checks that byte before doing anything: the netif's TX notify,
    // requestDequeue, and both completion queues' enqueuePackets. Skipping this produces a fully
    // assembled, completely inert data path with no error anywhere — which is exactly what was
    // measured on 26.6 for several boots after the work-loop fix landed.
    //
    // Order matters and is Apple's: enable, then pull the first RX buffers. Doing it here rather
    // than at FWSetupDone (where AppleBCMWLANPCIeSkywalk does it) is safe — enable() on a queue
    // whose netif queue is not bound yet just sends a notification — but it does mean the initial
    // prime can find nothing to take, so skywalkRxInput re-arms it whenever the free list runs dry.
    enableSkywalkQueues();

    gItlwmSkywalkStage = 11;
    return true;
}

// enable() and disable() are IOEventSource slots 42/43, overridden by each queue class. Called
// virtually; the concrete overrides close the queue's own gate, flip the byte and kick the queue.
void AirportItlwmSkywalkInterface::
enableSkywalkQueues()
{
    IOSkywalkPacketQueue *const queues[4] = { fTxSubQ, fTxComplQ, fRxSubQ, fRxComplQ };
    for (int i = 0; i < 4; i++) {
        if (queues[i] == NULL)
            continue;
        queues[i]->enable();
        gItlwmSkywalkQueuesEnabled++;
    }
    primeSkywalkRx(false);
}

// Pull empty buffers from the stack. Nothing else starts RX — the driver pulls, the stack does not
// push. `async` is for the receive path, which must not close a gate.
void AirportItlwmSkywalkInterface::
primeSkywalkRx(bool async)
{
    if (fRxSubQ == NULL)
        return;
    gItlwmSkywalkRxPrimeCalls++;
    gItlwmSkywalkRxPrimeRet = (uint32_t)fRxSubQ->requestDequeue(NULL, async ? 1 : 0);
}

void AirportItlwmSkywalkInterface::
teardownSkywalkDataPath()
{
    // Drop the borrowed RX buffers before the queues that own them. They belong to the pool, which
    // the queues retain, so forgetting the list is enough — deallocating them here would double
    // free against the pool's own teardown.
    if (fRxFreeLock != NULL) {
        IOSimpleLockLock(fRxFreeLock);
        fRxFreeHead = NULL;
        fRxFreeTail = NULL;
        fRxFreeCount = 0;
        fRxRefillPending = false;
        IOSimpleLockUnlock(fRxFreeLock);
    }
    gItlwmSkywalkRxFree = 0;

    // Quiesce before unhooking: disable() takes each queue's own gate and stops it accepting new
    // work, so nothing is in flight when the event source leaves the loop. Apple pairs
    // enableAllSubmissionQueue with disableAllSubmissionQueue the same way.
    if (gItlwmSkywalkQueuesEnabled > 0) {
        IOSkywalkPacketQueue *const queues[4] = { fTxSubQ, fTxComplQ, fRxSubQ, fRxComplQ };
        for (int i = 0; i < 4; i++)
            if (queues[i] != NULL)
                queues[i]->disable();
        gItlwmSkywalkQueuesEnabled = 0;
    }

    // Detach the queues from the work loop before releasing them: an event source left on a work
    // loop after its last reference goes is a use-after-free the next time the loop polls.
    if (gItlwmSkywalkQueuesAdded > 0) {
        IOWorkLoop *wq = getWorkQueue();
        if (wq != NULL) {
            IOSkywalkPacketQueue *const queues[4] = { fTxSubQ, fTxComplQ, fRxSubQ, fRxComplQ };
            for (int i = 0; i < 4; i++)
                if (queues[i] != NULL)
                    wq->removeEventSource(queues[i]);
        }
        gItlwmSkywalkQueuesAdded = 0;
    }

    // Release in reverse construction order. Each queue retains its pool, so the pools go last.
    OSSafeReleaseNULL(fRxComplQ);
    OSSafeReleaseNULL(fRxSubQ);
    OSSafeReleaseNULL(fTxComplQ);
    OSSafeReleaseNULL(fTxSubQ);
    OSSafeReleaseNULL(fRxPool);
    OSSafeReleaseNULL(fTxPool);
    fSkywalkRegistered = false;

    // Last: the free list above is empty and both producers are gone, so nothing can take it now.
    if (fRxFreeLock != NULL) {
        IOSimpleLockFree(fRxFreeLock);
        fRxFreeLock = NULL;
    }
}

// ---- TX -----------------------------------------------------------------------------------
// The contract, recovered from IOSkywalkTxSubmissionQueue::listDequeue on 26.6. None of it is
// guessable from the signature, and three parts of it are the opposite of what the signature
// suggests:
//
//   * `packets` is NOT an array of `count` pointers. It is `&queue[0x108]`, the address of the
//     head of a singly-linked list built by getPacketListForKPipe with setNextPacket(). Only
//     packets[0] is valid; the rest are reached with getNextPacket(). Indexing packets[1] reads
//     queue[0x110], the list *tail*, and would transmit the last frame repeatedly.
//   * the return value is the number of packets CONSUMED. listDequeue subtracts it from the
//     pending count and calls back again until either the count reaches zero or we return 0.
//   * returning 0 is not "no packets were available", it is "I refuse them". listDequeue then
//     sets a stall flag, timestamps it, and KEEPS the list for a later retry. That is exactly
//     what the inert stub used to do, and it is why a successful registration with stub callbacks
//     killed networking: every frame the system sent queued here and nothing ever drained it.
//
// Called with the submission queue's gate closed (packetSubmissionForKPipe wraps listDequeue in
// IOEventSource::closeGate), so this must not block and must not reach anything that takes the
// main command gate synchronously.
//
// DESIGN: copy each frame into an mbuf and hand it to the existing, working net80211 TX path,
// rather than mapping the packet for DMA and handing it to iwx directly. One memcpy per frame is
// the price. It buys: no dependence on the pool's DMA mapping (our PoolOptions.memorySpec is
// NULL, so IOVAs are not established), no packet lifetime threaded through the HAL, and complete
// reuse of encapsulation, crypto and rate control. Zero-copy is a later optimisation and needs
// iwx_tx restructured to take a non-mbuf buffer; it is not a prerequisite for working traffic.
uint32_t AirportItlwmSkywalkInterface::
txSubmissionDequeue(OSObject *target, IOSkywalkTxSubmissionQueue *queue,
                    IOSkywalkPacket **packets, uint32_t count, void *)
{
    gItlwmSkywalkTxDequeue++;

    AirportItlwmSkywalkInterface *me =
        OSDynamicCast(AirportItlwmSkywalkInterface, target);
    if (me == NULL || packets == NULL || count == 0)
        return 0;
    // The non-const `packets` is not a style choice — it is what binds the list-mode factory
    // overload, and it is why the slot may be written. `packets` is the ADDRESS OF THE HEAD POINTER,
    // and listDequeue re-passes that same address on every iteration without advancing it: it only
    // clears the head once the pending count reaches zero. So a partial consume must leave the
    // remaining head in *packets, or the next iteration hands back packets we have already taken and
    // completed. Harmless while we always drain the whole list, which is why it went unnoticed;
    // TxListShort is the counter that says we did not.
    return me->handleTxDequeue(queue, packets[0], count, packets);
}

uint32_t AirportItlwmSkywalkInterface::
handleTxDequeue(IOSkywalkTxSubmissionQueue *queue, IOSkywalkPacket *head, uint32_t count,
                IOSkywalkPacket **headSlot)
{
    struct ieee80211com *ic = fHalService ? fHalService->get80211Controller() : NULL;
    struct _ifnet *ifp = ic ? &ic->ic_ac.ac_if : NULL;

    if (head == NULL)
        return 0;

    // Refusing while down would strand the list until the next submission, so drop instead:
    // consume the packets, free the frames, and report them consumed. A dropped frame is a
    // retransmit; a stranded list is a dead interface.
    bool deliver = (ic != NULL && ifp != NULL &&
                    ic->ic_state == IEEE80211_S_RUN && ifp->if_snd.queue != NULL);

    IOSkywalkPacket *pkt = head;
    IOSkywalkPacket *tail = NULL;
    uint32_t taken = 0;
    uint32_t queued = 0;

    while (pkt != NULL && taken < count) {
        IOSkywalkPacket *next = pkt->getNextPacket();

        if (deliver) {
            // The frame starts at the buffer base PLUS the data offset — getDataVirtualAddress
            // does not include it. See the note in IOSkywalkDataPath.h.
            uint64_t base = pkt->getDataVirtualAddress();
            uint32_t len  = pkt->getDataLength();
            uint16_t off  = pkt->getDataOffset();

            if (base != 0 && len > 0 && len <= ITLWM_SKYWALK_TXBUFSZ) {
                mbuf_t m = NULL;
                unsigned int chunks = 1;
                // DONTWAIT: we hold the submission queue's gate.
                mbuf_allocpacket(MBUF_DONTWAIT, len, &chunks, &m);
                if (m != NULL) {
                    mbuf_setlen(m, len);
                    mbuf_pkthdr_setlen(m, len);
                    memcpy(mtod(m, void *), (const void *)(base + off), len);
                    if (ifp->if_snd.queue->lockEnqueue(m)) {
                        queued++;
                    } else {
                        mbuf_freem(m);
                        gItlwmSkywalkTxDrops++;
                    }
                } else {
                    gItlwmSkywalkTxNoMbuf++;
                }
            } else {
                gItlwmSkywalkTxDrops++;
            }
        } else {
            gItlwmSkywalkTxDrops++;
        }

        tail = pkt;
        pkt = next;
        taken++;
    }

    if (taken == 0)
        return 0;

    // NEVER return more than `count`: listDequeue does an unsigned `pending -= returned` and loops
    // while the result is non-zero, so overshooting by one wraps the pending count to ~4e9 and
    // spins forever inside the gate. The walk is bounded by `count` above for exactly this reason.
    //
    // The converse — walking off the end of the list before reaching `count` — must not happen:
    // getPacketListForKPipe increments the pending count by the number of packets it chained, so
    // length and count agree by construction. It matters because the family does NOT advance
    // queue[0x108] between iterations; it only clears head/tail once pending reaches zero. So a
    // short walk would leave the family holding a head we have just recycled to the pool, and the
    // next iteration would walk freed packets. Counted rather than assumed away.
    if (pkt == NULL && taken < count)
        gItlwmSkywalkTxListShort++;

    // Hand the remainder back to the family before cutting, so a partial consume leaves its head
    // pointing at what we did NOT take. listDequeue re-passes this same slot on the next iteration
    // and never advances it itself.
    if (headSlot != NULL)
        *headSlot = pkt;

    // Detach the consumed run from anything left behind, so the completion queue walks exactly
    // the packets we took. listDequeue only adjusts its pending count; the chain is ours to cut.
    if (tail != NULL)
        tail->setNextPacket(NULL);

    // Give the packets straight back. We copied, so nothing downstream still refers to them, and
    // holding them would drain a pool sized to the ring.
    //
    // ORDERING, and the one open risk in this path: enqueuePackets closes the COMPLETION queue's
    // gate while we already hold the SUBMISSION queue's. Those are two different IOEventSources,
    // so this is submission -> completion. Nothing observed takes them the other way round, but
    // that has not been proven exhaustively; if this ever deadlocks, defer completion to the
    // watchdog rather than reordering anything here.
    if (fTxComplQ != NULL) {
        IOReturn cret = fTxComplQ->enqueuePackets(head, taken, 0);
        if (cret != kIOReturnSuccess)
            gItlwmSkywalkTxComplFail++;
    }

    // Kick the HAL. iwx_start goes through attemptAction, which is non-blocking, so this cannot
    // deadlock against the gate we hold — but it also means it may do nothing right now, which is
    // mechanism 19. drainStrandedMgmtFrames re-drives if_start from the watchdog.
    if (queued > 0 && ifp != NULL && ifp->if_start != NULL)
        (*ifp->if_start)(ifp);

    gItlwmSkywalkTxFrames += queued;
    return taken;
}

uint32_t AirportItlwmSkywalkInterface::
txCompletionEnqueue(OSObject *, IOSkywalkTxCompletionQueue *, IOSkywalkPacket **, uint32_t, void *)
{
    gItlwmSkywalkTxComplete++;
    return 0;
}

// ---- RX -----------------------------------------------------------------------------------
// Same list-not-array, consumed-count, 0-means-refusal contract as TX, re-derived from
// IOSkywalkRxSubmissionQueue::listDequeue rather than assumed symmetrical with it: `packets` is
// &queue[0xe8], `count` is queue[0x120], and a 0 return bumps a stall stat and stops the loop.
//
// The SEMANTICS are the mirror image, though, and that is the part worth stating: these packets
// are EMPTY. The stack is lending the driver buffers to fill, not handing it data. Consuming one
// means taking ownership of an empty buffer; the frame goes back up later through the RX
// COMPLETION queue. So "consumed" here is an acquisition, and refusing them all — which the stub
// did — means the driver never obtains a buffer to receive into, and the interface cannot receive
// at all.
uint32_t AirportItlwmSkywalkInterface::
rxSubmissionDequeue(OSObject *target, IOSkywalkRxSubmissionQueue *queue,
                    IOSkywalkPacket **packets, uint32_t count, void *)
{
    gItlwmSkywalkRxDequeue++;

    AirportItlwmSkywalkInterface *me =
        OSDynamicCast(AirportItlwmSkywalkInterface, target);
    if (me == NULL || packets == NULL || count == 0)
        return 0;
    // See the note in txSubmissionDequeue: `packets` is the address of the queue's head pointer,
    // re-passed unchanged on every listDequeue iteration, so a partial consume must update it.
    return me->handleRxDequeue(queue, packets[0], count, packets);
}

uint32_t AirportItlwmSkywalkInterface::
handleRxDequeue(IOSkywalkRxSubmissionQueue *queue, IOSkywalkPacket *head, uint32_t count,
                IOSkywalkPacket **headSlot)
{
    if (head == NULL || fRxFreeLock == NULL)
        return 0;

    // Walk the chain we are being offered, bounded by `count` for the same unsigned-underflow
    // reason as TX: returning more than `count` wraps the family's pending counter and spins.
    IOSkywalkPacket *pkt = head;
    IOSkywalkPacket *tail = NULL;
    uint32_t taken = 0;

    while (pkt != NULL && taken < count) {
        tail = pkt;
        pkt = pkt->getNextPacket();
        taken++;
    }
    if (taken == 0)
        return 0;
    if (pkt == NULL && taken < count)
        gItlwmSkywalkRxListShort++;

    // Give the family back the head of what we did NOT take, before cutting; listDequeue re-passes
    // this slot rather than advancing it itself.
    if (headSlot != NULL)
        *headSlot = pkt;

    // Cut the run we are taking away from anything beyond it before publishing it, so the receive
    // thread can never walk past the end of what we own.
    if (tail != NULL)
        tail->setNextPacket(NULL);

    IOSimpleLockLock(fRxFreeLock);
    if (fRxFreeTail != NULL)
        fRxFreeTail->setNextPacket(head);
    else
        fRxFreeHead = head;
    fRxFreeTail = tail;
    fRxFreeCount += taken;
    // Cleared here rather than where the request is issued: this is the point at which asking again
    // would actually be a new question.
    fRxRefillPending = false;
    IOSimpleLockUnlock(fRxFreeLock);

    gItlwmSkywalkRxFree = fRxFreeCount;
    return taken;
}

// C-linkage shim for AirportItlwmEthernetInterface::inputPacket, which cannot include this class's
// header without dragging in AirportItlwm, ItlHalService and net80211. The cast lives here so the
// caller needs no knowledge of this type.
extern "C" bool
itlwmSkywalkRxInput(IO80211SkywalkInterface *iface, mbuf_t m)
{
    AirportItlwmSkywalkInterface *sky = OSDynamicCast(AirportItlwmSkywalkInterface, iface);
    return sky != NULL && sky->skywalkRxInput(m);
}

// Called on the HAL's receive thread with a complete ethernet frame. Copies it into one of the
// buffers the stack lent us and hands it straight up the RX completion queue.
//
// Returns false rather than dropping whenever it cannot do that, so the caller falls back to the
// legacy BSD path. That matters during bring-up: the two paths coexist until
// AirportItlwmEthernetInterface's BSD attach is retired, and a frame is far better delivered twice
// slowly than not at all. Once registration is the only path, the false return becomes a drop and
// this comment must change with it.
bool AirportItlwmSkywalkInterface::
skywalkRxInput(mbuf_t m)
{
    if (!fSkywalkRegistered || fRxComplQ == NULL || fRxFreeLock == NULL || m == NULL)
        return false;

    size_t len = mbuf_pkthdr_len(m);
    if (len == 0 || len > ITLWM_SKYWALK_RXBUFSZ) {
        gItlwmSkywalkRxOversize++;
        return false;
    }

    IOSimpleLockLock(fRxFreeLock);
    IOSkywalkPacket *pkt = fRxFreeHead;
    if (pkt != NULL) {
        fRxFreeHead = pkt->getNextPacket();
        if (fRxFreeHead == NULL)
            fRxFreeTail = NULL;
        fRxFreeCount--;
    }
    // Refill BEFORE the list empties, not after. RX is driver-pulled — nothing tops this list up
    // unless we ask — so waiting for it to hit zero guarantees that the frame which discovers the
    // shortage is dropped, once per drain cycle, forever. Measured on 26.6: one prime lent 255
    // buffers and 245 were consumed by a single short session, so the first drop was ten frames
    // away and the connection had never yet run long enough to reach it.
    //
    // fRxRefillPending stops this asking once per frame for the whole time the list sits below the
    // mark: the pull is asynchronous, so the buffers do not arrive before the next frame does. It
    // is cleared in handleRxDequeue, i.e. when buffers actually land, not when the request is made.
    bool needRefill = (fRxFreeCount <= ITLWM_SKYWALK_RXLOWAT) && !fRxRefillPending;
    if (needRefill)
        fRxRefillPending = true;
    IOSimpleLockUnlock(fRxFreeLock);

    if (needRefill)
        primeSkywalkRx(true);

    if (pkt == NULL) {
        // The stack has not lent us a buffer. Not an error on its own — it throttles this way —
        // but a counter that climbs while RxFrames does not means the RX ring is starved.
        //
        // Re-arm the pull rather than waiting to be handed one: RX is driver-pulled, so nothing
        // else will refill the free list, and the initial prime at registration can legitimately
        // have come up empty if the netif had not bound the queues yet. Asynchronous on purpose —
        // this runs on the HAL's receive thread and must not close the submission queue's gate.
        // Reaching here now means the low-water refill above did not arrive in time, so this is a
        // real starvation rather than the normal end of a drain cycle. Ask again regardless of
        // fRxRefillPending — a pending request that has not landed by the time the list is empty is
        // not evidence that another will not help, and dropping frames silently is worse.
        gItlwmSkywalkRxNoBuf++;
        primeSkywalkRx(true);
        return false;
    }
    pkt->setNextPacket(NULL);
    gItlwmSkywalkRxFree = fRxFreeCount;

    uint64_t base = pkt->getDataVirtualAddress();
    uint16_t off  = pkt->getDataOffset();
    if (base == 0) {
        gItlwmSkywalkRxDrops++;
        goto give_back;
    }

    // mbuf_copydata flattens a chain; mbuf_data() would only give the first segment, and an
    // 802.11 receive path hands up chained mbufs routinely.
    if (mbuf_copydata(m, 0, len, (void *)(base + off)) != 0) {
        gItlwmSkywalkRxDrops++;
        goto give_back;
    }
    if (pkt->setDataLength((uint32_t)len) != kIOReturnSuccess) {
        gItlwmSkywalkRxDrops++;
        goto give_back;
    }

    if (fRxComplQ->enqueuePackets(pkt, 1, 0) != kIOReturnSuccess) {
        gItlwmSkywalkRxComplFail++;
        goto give_back;
    }
    gItlwmSkywalkRxFrames++;
    return true;

give_back:
    // Put the buffer back on the free list rather than leaking it; the pool is sized to the ring,
    // so leaking on an error path drains RX within seconds of the first fault.
    IOSimpleLockLock(fRxFreeLock);
    pkt->setNextPacket(fRxFreeHead);
    fRxFreeHead = pkt;
    if (fRxFreeTail == NULL)
        fRxFreeTail = pkt;
    fRxFreeCount++;
    IOSimpleLockUnlock(fRxFreeLock);
    gItlwmSkywalkRxFree = fRxFreeCount;
    return false;
}

uint32_t AirportItlwmSkywalkInterface::
rxCompletionEnqueue(OSObject *, IOSkywalkRxCompletionQueue *, IOSkywalkPacket **, uint32_t, void *)
{
    gItlwmSkywalkRxComplete++;
    return 0;
}

extern "C" {
// Non-zero means prepareBSDInterface ran with no work queue to close the gate on, which leaves
// Apple's sendIOUCToWcl free to panic. Expected to stay 0.
uint32_t gItlwmPrepareBSDUngated;
// Deferred postMessage accounting. Queued should equal Sent once things settle; Dropped
// non-zero means the ring overflowed or the interface went away mid-flight.
uint32_t gItlwmPostMsgQueued;
uint32_t gItlwmPostMsgSent;
uint32_t gItlwmPostMsgDropped;
// Tahoe scan entry point. Calls should be non-zero once airportd asks for a scan; Refused
// non-zero means net80211 was not ready (ic_state <= INIT, or a result walk in progress).
uint32_t gItlwmScanReqCalls;
uint32_t gItlwmScanReqStarted;
uint32_t gItlwmScanReqRefused;
// Scan results pushed to IO80211Family as APPLE80211_M_BSS_BEACON. Should climb during a scan;
// zero while ScanReqStarted climbs means the beacon hook is not firing.
uint32_t gItlwmScanBeacons;
// Mechanism 13 — scan completion. Read these together; individually they mislead.
//   DoneEvents  every IEEE80211_EVT_SCAN_DONE net80211 raised. While disconnected it sweeps
//               continuously on its own, so this climbs with no request outstanding. Expect it
//               to be much LARGER than Completes; that is correct, not a leak.
//   DoneIgnored the subset with no request outstanding — i.e. the 10 Hz storm, suppressed.
//   Completes   237+10 pairs actually posted. Should track ScanReqCalls closely.
//   Backstops   completions the TIMER had to author because no SCAN_DONE arrived inside
//               kScanBackstopMs. MUST BE 0 (or near it) on a healthy boot; if it equals
//               Completes the event path is dead and this is the old timer behaviour, slower.
//   Stray       the timer fired with the latch already clear. Small is fine (an abort raced a
//               pending firing); climbing with Completes means the latch is being lost.
//   Coalesced   a request arrived while one was outstanding. The WCL queues these itself and
//               drains one per completion, so a nonzero value is expected under load.
//   NoStart     accepted in RUN but the HAL reports no sweep in flight — begin_cache_bgscan
//               declined (already scanning, mgt timer, RSN port not valid). The backstop
//               answers these; it is the reason Backstops may be nonzero while healthy.
//   InFlight    the sweep that will complete us had already started when the request arrived.
//   ReqState    ic_state at the last request (enum ieee80211_state, ieee80211_proto.h:53):
//               0 = INIT, 1 = SCAN, 2 = AUTH, 3 = ASSOC, 4 = RUN. A boot pinned at 0 with
//               Refused == Calls is the enable/iwx_init stall, NOT a scan bug — see mechanism 26.
//   LastMs/MaxMs elapsed request->completion. THIS IS THE NUMBER THAT RETUNES kScanBackstopMs.
//               Expect order 4000 while disconnected. ~10000 means the backstop authored it.
uint32_t gItlwmScanDoneEvents;
uint32_t gItlwmScanDoneIgnored;
uint32_t gItlwmScanCompletes;
uint32_t gItlwmScanBackstops;
uint32_t gItlwmScanStray;
uint32_t gItlwmScanReqCoalesced;
uint32_t gItlwmScanReqNoStart;
uint32_t gItlwmScanReqInFlight;
uint32_t gItlwmScanAborts;
uint32_t gItlwmScanReqState;
uint32_t gItlwmScanLastMs;
uint32_t gItlwmScanMaxMs;
// Tahoe association entry point, slot 602 (mechanism 15). Calls climbs as soon as a network is
// picked in the menu; Refused means net80211 was in no state to start a join. The two Join*
// counters are the completion messages the WCL's JOIN_MANAGER FSM waits on — a join that reaches
// AssocDone but never ConnectDone died between association and link-up, i.e. in the handshake.
// Timeouts counts joins abandoned because net80211 never got there; each one is reported to the
// WCL as a failed connect so it moves to the next candidate.
uint32_t gItlwmAssocCalls;
uint32_t gItlwmAssocStarted;
uint32_t gItlwmAssocRefused;
// Associate calls whose apple80211_assoc_candidates carried a usable key at +0x40. This should
// equal AssocCalls: the WCL puts the key in the request and never calls setCIPHER_KEY, so a zero
// here with AssocCalls climbing means the key field is not populated the way the producer's
// memmove says it is.
uint32_t gItlwmAssocKeyInReq;
// A PSK network was asked for and neither the request nor the setCIPHER_KEY stash had a key. The
// join cannot succeed: ieee80211_match_bss rejects a PSK-only AP when IEEE80211_F_PSK is clear.
uint32_t gItlwmAssocNoPmk;
// The apple80211_assoc_candidates fields that decide what associateSSID does: the auth-type
// bitmask at +0x14, and (cipher << 16) | len of the key at +0x40. First and last call, because
// they have been observed to differ within one boot.
uint32_t gItlwmAssocUpperAuthFirst;
uint32_t gItlwmAssocUpperAuthLast;
uint32_t gItlwmAssocKeyInfoFirst;
uint32_t gItlwmAssocKeyInfoLast;
// Times the watchdog found a management frame stranded in ic_mgtq and re-drove if_start. Should
// be 0 on a healthy system: a non-zero value means iwx_start's attemptAction lost the gate (or
// ifq_oactive was set) and nothing else would ever have retried the frame.
uint32_t gItlwmMgtqKicks;
uint32_t gItlwmJoinAssocDone;
uint32_t gItlwmJoinConnectDone;
uint32_t gItlwmJoinTimeouts;
// Message 216, the link-status indication that drives WCLNetManager's FSM. Independent of the
// ItlwmJoin* counters on purpose: a join can complete the JOIN_MANAGER FSM (AssocDone = ConnectDone
// = 1, no failures) and still leave the interface link-down to BSD, which is exactly the state that
// ItlwmLinkIndUp = 0 names. Non-zero here with `ifconfig` still `inactive` moves the fault past the
// driver, to whatever NET_MANAGER did with the indication.
uint32_t gItlwmLinkIndUp;
uint32_t gItlwmLinkIndDown;
// net80211's ic_state at the last link-down indication. Names the transition that tore the
// association down; see postLinkStatusInd().
uint32_t gItlwmLinkDownState;
// The net80211 transition that dropped the link: (ostate << 16) | (nstate << 8) | mgt.
uint32_t gItlwmLinkDownPair;
// AirportItlwm::disable() — the non-net80211 route to a link-down. See the call site.
uint32_t gItlwmDisableCalls;
// CoreCapture pipes whose IOService::start ran, and those it failed for. A pipe that never starts
// has no notify timer, and CCDataPipe::enqueueBlob dereferences that timer unchecked — so
// Started must equal the number of pipes created, and StartFail must be 0.
uint32_t gItlwmCCPipesStarted;
uint32_t gItlwmCCPipeStartFail;
// Ioctl 425, setWCL_LEAVE_NETWORK. Non-zero means the WCL asked the driver to disconnect and the
// driver acted on it; a teardown in the WCL log with this at 0 means the two halves have diverged.
uint32_t gItlwmLeaveNetCalls;
// Message 39, the LQM keepalive. Posts counts what left the driver; BeaconStall counts the ticks
// where the beacon delta was zero while associated, which is the condition that lets the WCL's
// missed-beacon timer run out. Posts must advance ~1/s for the whole life of a connection: it stops
// advancing exactly 60 s before a `<missed beacons timeout>` teardown, and a rising BeaconStall says
// the message is being posted but carrying nothing the WCL will accept.
uint32_t gItlwmLqmPosts;
uint32_t gItlwmLqmBeaconStall;
// ic_des_esslen as it stood after the last associateSSID(). Zero with ItlwmAssocCalls non-zero
// means a join request reached the driver and net80211 was never told what to look for, which is
// indistinguishable from a rejected BSS in every counter above it.
uint32_t gItlwmAssocEsslen;
// Ioctl 433, getWCL_BSS_INFO. Calls counts the answers given; Empty counts the refusals, each of
// which costs the association — WCLNetManager::updateBss turns a failed get into leaveNetworkCommand.
// A non-zero Empty means a beacon for the target BSS was never cached before link-up, which is a
// scan-coverage problem rather than a bug in the getter.
uint32_t gItlwmBssInfoCalls;
uint32_t gItlwmBssInfoEmpty;
// Ioctl 460, getWCL_EXTENDED_BSS_INFO — the call immediately after 433 on the link-up path.
uint32_t gItlwmExtBssInfoCalls;
// Slot 452, getLastRxUnicastLinkActivityTime. Answers one question that static analysis could not
// settle: is our pinned override actually called? IO80211MacAddressAgent::setMacAddress
// error-checks that slot's return, our override returns 0, and the join still fails with a
// pointer-shaped value — so either the slot is not called on our object, or one of the premises
// in the branch analysis is wrong. Climbing during a join attempt means the slot is ours and the
// failure is elsewhere; staying zero means the call never reaches us and the object the agent
// holds is not this interface.
uint32_t gItlwmLastRxActivityCalls;
// Neighbours of slot 452, counted to identify which slot the loaded table really dispatches.
uint32_t gItlwmDataPathPeerStatsCalls;   // real slot 450
uint32_t gItlwmLastQueueTimeCalls;       // real slot 451
}

// TEMPORARY — delete when Skywalk registration becomes real (mechanism 12 in the root
// AGENTS.md, subsumed by mechanism 1). This exists only because *this driver* calls
// prepareBSDInterface from an ungated thread. Apple's registration path already holds the gate,
// so once that path is in use this wrapper degrades to a recursive no-op that still compiles and
// still runs while documenting something untrue. Retire it together with the RegistrationInfo
// loan above — same missing caller, same removal.
//
// Apple's prepareBSDInterface must run with the interface work queue's gate CLOSED.
//
// IO80211InfraInterface::prepareBSDInterface+0x112 calls updateStaticProperties, which calls
// IO80211Glue::sendIOUCToWcl, which refuses and panics unless both of these hold:
//
//     wq->inGate()   == true      slot 39 on interface ivars +0x38; false -> refuse
//     wq->onThread() == false     slot 38; true -> refuse
//     -> panic("trying to send on thread panic") @IO80211Glue.cpp:419
//
// Apple reaches this hook from inside its own Skywalk registration, which already holds the
// gate. This driver has no registration to reach it from (see the RegistrationInfo loan note
// above), so it calls the hook from AirportItlwmEthernetInterface::attachToDataLinkLayer —
// i.e. on configd's thread via IONetworkStack::attachNetworkInterfaceToBSD, with nothing
// closed. runAction reproduces the context Apple's own callers provide: IOWorkLoop::runAction
// is closeGate (slot 48) / call action / openGate, synchronously on the calling thread.
//
// Both conditions then hold and keep holding: the gate is an IORecursiveLock, so a nested
// close inside Apple's code is fine, and runAction does not migrate the caller, so onThread()
// stays false. closeGate/openGate cannot be called directly — they are protected in
// IOWorkLoop and this class does not derive from it.
//
// Scoped to the super call alone. attachToDataLinkLayer's own work, IONetworkStack machinery
// included, must not run under this gate.
struct ItlwmPrepareBSDCall {
    AirportItlwmSkywalkInterface *self;
    ifnet_t ifp;
    UInt flags;
    bool ok;
};

IOReturn AirportItlwmSkywalkInterface::
gatedPrepareBSDAction(OSObject *, void *arg0, void *, void *, void *)
{
    ItlwmPrepareBSDCall *call = (ItlwmPrepareBSDCall *)arg0;
    call->ok = call->self->superPrepareBSDInterface(call->ifp, call->flags);
    return kIOReturnSuccess;
}

bool AirportItlwmSkywalkInterface::
superPrepareBSDInterface(ifnet_t ifp, UInt flags)
{
    return super::prepareBSDInterface(ifp, flags);
}

bool AirportItlwmSkywalkInterface::
gatedSuperPrepareBSDInterface(ifnet_t ifp, UInt flags)
{
    IO80211WorkQueue *wq = getWorkQueue();
    if (wq == NULL) {
        // Nothing to close the gate on. Apple would panic on the send either way, so let it
        // fail at its own check rather than inventing a different failure here — and record
        // that this happened, because it means the diagnosis above does not apply.
        gItlwmPrepareBSDUngated++;
        return superPrepareBSDInterface(ifp, flags);
    }
    ItlwmPrepareBSDCall call = { this, ifp, flags, false };
    if (wq->runAction(&AirportItlwmSkywalkInterface::gatedPrepareBSDAction, this,
                      &call) != kIOReturnSuccess)
        return false;
    return call.ok;
}

// Slot 335 — the family telling the driver it has settled on a new link address. This is the only
// such notification, and root AGENTS.md mechanism 21 is what happens without it.
//
// `ic_myaddr` is NOT an alias of `ic_ac.ac_enaddr`. ieee80211_ifattach copies ic_myaddr into
// ac_enaddr once and nothing ever copies back, and it is ic_myaddr that 802.11 actually runs on:
// the source address of every transmitted frame, the PTK derivation input, and — the one that
// costs a DHCP lease — the RX "is this frame for me" filter in ieee80211_input.c. Apple's
// implementation of this slot publishes IOMACAddress and calls ifnet_set_lladdr, i.e. it updates
// the BSD side only, so a driver that does not override it associates and filters on the factory
// address while the BSD stack advertises the assigned one.
//
// Order matters and super() must go LAST: it is what calls ifnet_set_lladdr, so doing our half
// first means there is no window in which the two layers disagree.
IOReturn AirportItlwmSkywalkInterface::
setLinkLayerAddress(ether_addr *addr)
{
    gItlwmLlAddrCalls++;

    struct ieee80211com *ic = fHalService ? fHalService->get80211Controller() : NULL;
    if (addr != NULL && ic != NULL) {
        const uint8_t *mac = (const uint8_t *)addr;
        gItlwmLlAddrLast = 0;
        for (int i = 0; i < ETHER_ADDR_LEN; i++)
            gItlwmLlAddrLast = (gItlwmLlAddrLast << 8) | mac[i];
        if (!IEEE80211_ADDR_EQ(ic->ic_myaddr, mac)) {
            // The address the family picked becomes the one 802.11 uses. Both sides of the split
            // are written here; if_setlladdr only reaches ac_enaddr, and super() below only
            // reaches the ifnet, so neither alone is sufficient.
            IEEE80211_ADDR_COPY(ic->ic_myaddr, mac);
            IEEE80211_ADDR_COPY(ic->ic_ac.ac_enaddr, mac);
            gItlwmLlAddrSynced++;
            // A change under a live association cannot take effect on the air: the AP knows us by
            // the address we authenticated with and the PTK is derived from it. Nothing is done
            // about that here — re-associating from inside this callback would be a much larger
            // change — but it is recorded, because it is the difference between "the sync works"
            // and "the sync works and arrived too late".
            if (ic->ic_state == IEEE80211_S_RUN)
                gItlwmLlAddrLate++;
        }
    }
    return super::setLinkLayerAddress(addr);
}

// super::start() is IO80211SkywalkInterface::start, which builds the per-interface state block
// the whole family dereferences unchecked. Its return is propagated verbatim, and
// AirportItlwmV2::start treats false as fatal — see the comment there. If it ever starts failing
// again, it has twelve separate jumps to one failure label, so localise it by sampling the
// objects it stores into state[] as it goes:
//   scripts/abi/disrange.py __ZN23IO80211SkywalkInterface5startEP9IOService 0 760
// and look for `mov qword ptr [<state> + 0xNN]`.
bool AirportItlwmSkywalkInterface::
start(IOService *provider)
{
    bool ret = super::start(provider);

    // Real Skywalk registration (mechanism 1). Unconditional on Tahoe — see the note at
    // ITLWM_REGINFO_MAC_OFFSET for why the three staging boot-args are gone. After super::start(),
    // because registration needs the state block super::start() populates.
    //
    // A failure here is recorded and never fatal to start(): the interface still exists, still
    // answers Apple80211 and still drives the HAL, so a half-built data path is better diagnosed
    // from a running machine than turned into a failed start with nothing to read.
    // ItlwmSkywalkStage says how far it got; 11 is complete.
    if (ret)
        registerSkywalkInterface();
    return ret;
}

#endif

const char* hexdump(uint8_t *buf, size_t len) {
    ssize_t str_len = len * 3 + 1;
    char *str = (char*)IOMalloc(str_len);
    if (!str)
        return nullptr;
    for (size_t i = 0; i < len; i++)
    snprintf(str + 3 * i, (len - i) * 3, "%02x ", buf[i]);
    str[MAX(str_len - 2, 0)] = 0;
    return str;
}

static int ieeeChanFlag2appleScanFlagVentura(int flags)
{
    int ret = 0;
    if (flags & IEEE80211_CHAN_2GHZ)
        ret |= APPLE80211_C_FLAG_2GHZ;
    if (flags & IEEE80211_CHAN_5GHZ)
        ret |= APPLE80211_C_FLAG_5GHZ;
    ret |= (APPLE80211_C_FLAG_ACTIVE | APPLE80211_C_FLAG_20MHZ);
    return ret;
}

static int ieeeChanFlag2apple(int flags, int bw)
{
    int ret = 0;
    if (flags & IEEE80211_CHAN_2GHZ)
        ret |= APPLE80211_C_FLAG_2GHZ;
    if (flags & IEEE80211_CHAN_5GHZ)
        ret |= APPLE80211_C_FLAG_5GHZ;
    if (!(flags & IEEE80211_CHAN_PASSIVE))
        ret |= APPLE80211_C_FLAG_ACTIVE;
    if (flags & IEEE80211_CHAN_DFS)
        ret |= APPLE80211_C_FLAG_DFS;
    if (bw == -1) {
        if (flags & IEEE80211_CHAN_VHT) {
            if ((flags & IEEE80211_CHAN_VHT160) || (flags & IEEE80211_CHAN_VHT80_80))
                ret |= APPLE80211_C_FLAG_160MHZ;
            if (flags & IEEE80211_CHAN_VHT80)
                ret |= APPLE80211_C_FLAG_80MHZ;
        } else if ((flags & IEEE80211_CHAN_HT40) && (flags & IEEE80211_CHAN_HT)) {
            ret |= APPLE80211_C_FLAG_40MHZ;
            if (flags & IEEE80211_CHAN_HT40U)
                ret |= APPLE80211_C_FLAG_EXT_ABV;
        } else if (flags & IEEE80211_CHAN_HT20)
            ret |= APPLE80211_C_FLAG_20MHZ;
        else if ((flags & IEEE80211_CHAN_CCK) || (flags & IEEE80211_CHAN_OFDM))
            ret |= APPLE80211_C_FLAG_10MHZ;
    } else {
        switch (bw) {
            case IEEE80211_CHAN_WIDTH_80P80:
            case IEEE80211_CHAN_WIDTH_160:
                ret |= APPLE80211_C_FLAG_160MHZ;
                break;
            case IEEE80211_CHAN_WIDTH_80:
                ret |= APPLE80211_C_FLAG_80MHZ;
                break;
            case IEEE80211_CHAN_WIDTH_40:
                ret |= APPLE80211_C_FLAG_40MHZ;
                if (flags & IEEE80211_CHAN_HT40U)
                    ret |= APPLE80211_C_FLAG_EXT_ABV;
                break;
            case IEEE80211_CHAN_WIDTH_20:
                ret |= APPLE80211_C_FLAG_20MHZ;
                break;
            default:
                if (flags & IEEE80211_CHAN_HT20)
                    ret |= APPLE80211_C_FLAG_20MHZ;
                else if ((flags & IEEE80211_CHAN_CCK) || (flags & IEEE80211_CHAN_OFDM))
                    ret |= APPLE80211_C_FLAG_10MHZ;
                break;
        }
    }
    return ret;
}

void AirportItlwmSkywalkInterface::associateSSID(uint8_t *ssid, uint32_t ssid_len, const struct ether_addr &bssid, uint32_t authtype_lower, uint32_t authtype_upper, uint8_t *key, uint32_t key_len, int key_index)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    
    ieee80211_disable_rsn(ic);
    ieee80211_disable_wep(ic);
    
    struct ieee80211_wpaparams     wpa;
    struct ieee80211_nwkey         nwkey;
    bzero(&wpa, sizeof(wpa));
    bzero(&nwkey, sizeof(nwkey));
    
    memset(ic->ic_des_essid, 0, IEEE80211_NWID_LEN);
    memcpy(ic->ic_des_essid, ssid, ssid_len);
    ic->ic_des_esslen = ssid_len;
    
    bool is_zero = true;
    for (int i = 0; i < IEEE80211_ADDR_LEN; i++)
    is_zero &= bssid.octet[i] == 0;
    
    if (!is_zero) {
        IEEE80211_ADDR_COPY(ic->ic_des_bssid, bssid.octet);
        ic->ic_flags |= IEEE80211_F_DESBSSID;
    }
    else {
        memset(ic->ic_des_bssid, 0, IEEE80211_ADDR_LEN);
        ic->ic_flags &= ~IEEE80211_F_DESBSSID;
    }

    // AUTHTYPE_WPA3_SAE AUTHTYPE_WPA3_FT_SAE
    // we don't really support WPA3, but we have announced we support WPA3 in card capability function. so we fake it as WPA2 to support some WPA2/WPA3 mix wifi connection.
    if (authtype_upper == APPLE80211_AUTHTYPE_WPA3_SAE || authtype_upper == APPLE80211_AUTHTYPE_WPA3_FT_SAE) {
        wpa.i_protos |= IEEE80211_WPA_PROTO_WPA2;
        authtype_upper |= APPLE80211_AUTHTYPE_WPA2_PSK;// hack
    }
    // AUTHTYPE_WPA3_ENTERPRISE AUTHTYPE_WPA3_FT_ENTERPRISE
    if (authtype_upper == APPLE80211_AUTHTYPE_WPA3_ENTERPRISE || authtype_upper == APPLE80211_AUTHTYPE_WPA3_FT_ENTERPRISE) {
        wpa.i_protos |= IEEE80211_WPA_PROTO_WPA2;
        authtype_upper |= APPLE80211_AUTHTYPE_WPA2;// hack
    }
    
    if (authtype_upper & (APPLE80211_AUTHTYPE_WPA | APPLE80211_AUTHTYPE_WPA_PSK | APPLE80211_AUTHTYPE_WPA2 | APPLE80211_AUTHTYPE_WPA2_PSK | APPLE80211_AUTHTYPE_SHA256_PSK | APPLE80211_AUTHTYPE_SHA256_8021X)) {
        XYLog("%s %d\n", __FUNCTION__, __LINE__);
        wpa.i_protos = IEEE80211_WPA_PROTO_WPA1 | IEEE80211_WPA_PROTO_WPA2;
    }
    
    if (authtype_upper & (APPLE80211_AUTHTYPE_WPA_PSK | APPLE80211_AUTHTYPE_WPA2_PSK | APPLE80211_AUTHTYPE_SHA256_PSK)) {
        XYLog("%s %d\n", __FUNCTION__, __LINE__);
        wpa.i_akms |= IEEE80211_WPA_AKM_PSK | IEEE80211_WPA_AKM_SHA256_PSK;
        wpa.i_enabled = 1;
        // A NULL key means the caller has none — an open network reached through a PSK auth type,
        // or a WCL join whose PMK has not arrived yet. There is nothing to copy and dereferencing
        // it would fault, so leave the RSN parameters set and the PSK absent; ieee80211_match_bss
        // will then decline the BSS, which is the honest outcome.
        //
        // Note what has *already* happened by this point: ieee80211_disable_rsn at the top of this
        // function zeroed ic_psk and cleared IEEE80211_F_PSK. So this branch is the only thing
        // that puts a key back, and a caller that stashed one elsewhere must hand it in here
        // rather than assume ic_psk survived. The pre-Tahoe caller passes ad->ad_key.key, an array
        // member, so it is never NULL and this changes nothing for the targets that work today.
        if (key != NULL) {
            memcpy(ic->ic_psk, key, sizeof(ic->ic_psk));
            ic->ic_flags |= IEEE80211_F_PSK;
        }
        ieee80211_ioctl_setwpaparms(ic, &wpa);
    }
    if (authtype_upper & (APPLE80211_AUTHTYPE_WPA | APPLE80211_AUTHTYPE_WPA2 | APPLE80211_AUTHTYPE_SHA256_8021X)) {
        XYLog("%s %d\n", __FUNCTION__, __LINE__);
        wpa.i_akms |= IEEE80211_WPA_AKM_8021X | IEEE80211_WPA_AKM_SHA256_8021X;
        wpa.i_enabled = 1;
        ieee80211_ioctl_setwpaparms(ic, &wpa);
    }
    
    if (authtype_lower == APPLE80211_AUTHTYPE_SHARED) {
        XYLog("shared key authentication is not supported!\n");
        return;
    }
    
    if (authtype_upper == APPLE80211_AUTHTYPE_NONE && authtype_lower == APPLE80211_AUTHTYPE_OPEN) { // Open or WEP Open System
        if (key_len > 0) {
            XYLog("%s %d\n", __FUNCTION__, __LINE__);
            nwkey.i_wepon = IEEE80211_NWKEY_WEP;
            nwkey.i_defkid = key_index + 1;
            nwkey.i_key[key_index].i_keylen = (int)key_len;
            nwkey.i_key[key_index].i_keydat = key;
            ieee80211_ioctl_setnwkeys(ic, &nwkey);
        }
    }
}

void AirportItlwmSkywalkInterface::setPTK(const u_int8_t *key, size_t key_len) {
    struct ieee80211com *ic = fHalService->get80211Controller();
    struct ieee80211_node    * ni = ic->ic_bss;
    struct ieee80211_key *k;
    int keylen;
    
    ni->ni_rsn_supp_state = RNSA_SUPP_PTKDONE;
    
    if (ni->ni_rsncipher != IEEE80211_CIPHER_USEGROUP) {
        u_int64_t prsc;
        
        /* check that key length matches that of pairwise cipher */
        keylen = ieee80211_cipher_keylen(ni->ni_rsncipher);
        if (key_len != keylen) {
            XYLog("PTK length mismatch. expected %d, got %zu\n", keylen, key_len);
            return;
        }
        prsc = /*(gtk == NULL) ? LE_READ_6(key->rsc) :*/ 0;
        
        /* map PTK to 802.11 key */
        k = &ni->ni_pairwise_key;
        memset(k, 0, sizeof(*k));
        k->k_cipher = ni->ni_rsncipher;
        k->k_rsc[0] = prsc;
        k->k_len = keylen;
        memcpy(k->k_key, key, k->k_len);
        /* install the PTK */
        if ((*ic->ic_set_key)(ic, ni, k) != 0) {
            XYLog("setting PTK failed\n");
            return;
        }
        else
            XYLog("setting PTK successfully\n");
        ni->ni_flags &= ~IEEE80211_NODE_RSN_NEW_PTK;
        ni->ni_flags &= ~IEEE80211_NODE_TXRXPROT;
        ni->ni_flags |= IEEE80211_NODE_RXPROT;
    } else if (ni->ni_rsncipher != IEEE80211_CIPHER_USEGROUP)
        XYLog("%s: unexpected pairwise key update received from %s\n",
              ic->ic_if.if_xname, ether_sprintf(ni->ni_macaddr));
}

void AirportItlwmSkywalkInterface::setGTK(const u_int8_t *gtk, size_t key_len, u_int8_t kid, u_int8_t *rsc) {
    struct ieee80211com *ic = fHalService->get80211Controller();
    struct ieee80211_node    * ni = ic->ic_bss;
    struct ieee80211_key *k;
    int keylen;
    
    if (gtk != NULL) {
        /* check that key length matches that of group cipher */
        keylen = ieee80211_cipher_keylen(ni->ni_rsngroupcipher);
        if (key_len != keylen) {
            XYLog("GTK length mismatch. expected %d, got %zu\n", keylen, key_len);
            return;
        }
        /* map GTK to 802.11 key */
        k = &ic->ic_nw_keys[kid];
        if (k->k_cipher == IEEE80211_CIPHER_NONE || k->k_len != keylen || memcmp(k->k_key, gtk, keylen) != 0) {
            memset(k, 0, sizeof(*k));
            k->k_id = kid;    /* 0-3 */
            k->k_cipher = ni->ni_rsngroupcipher;
            k->k_flags = IEEE80211_KEY_GROUP;
            //if (gtk[6] & (1 << 2))
            //  k->k_flags |= IEEE80211_KEY_TX;
            k->k_rsc[0] = LE_READ_6(rsc);
            k->k_len = keylen;
            memcpy(k->k_key, gtk, k->k_len);
            /* install the GTK */
            if ((*ic->ic_set_key)(ic, ni, k) != 0) {
                XYLog("setting GTK failed\n");
                return;
            }
            else
                XYLog("setting GTK successfully\n");
        }
    }
    
    if (true) {
        ni->ni_flags |= IEEE80211_NODE_TXRXPROT;
#ifndef IEEE80211_STA_ONLY
        if (ic->ic_opmode != IEEE80211_M_IBSS ||
            ++ni->ni_key_count == 2)
#endif
        {
            XYLog("marking port %s valid\n",
                  ether_sprintf(ni->ni_macaddr));
            ni->ni_port_valid = 1;
            ieee80211_set_link_state(ic, LINK_STATE_UP);
            ni->ni_assoc_fail = 0;
            if (ic->ic_opmode == IEEE80211_M_STA)
                ic->ic_rsngroupcipher = ni->ni_rsngroupcipher;
        }
    }
}

#if __IO80211_TARGET >= __MAC_26_0
bool AirportItlwmSkywalkInterface::
init(IOService *provider, ether_addr *initMac)
#else
bool AirportItlwmSkywalkInterface::
init(IOService *provider)
#endif
{
    // IO80211InfraInterface::init() is the right base call and NOT interchangeable with
    // IO80211SkywalkInterface::init(IOService *, ether_addr *): the Infra one chains through the
    // zero-argument Skywalk init (which is what runs initIvars) and then allocates the Infra
    // expansion block at this+0x128 — the block registerInfraEthernetInterface's MAC-stamp guard
    // reads. The two-argument Skywalk init allocates no such block. So the base call stays, and
    // the one thing it does not do is seeded explicitly below.
    bool ret = IO80211InfraInterface::init();
    if (!ret) {
        XYLog("%s IO80211InfraInterface init failed\n", __PRETTY_FUNCTION__);
        return false;
    }
#if __IO80211_TARGET >= __MAC_26_0
    // The ether_addr argument used to be discarded here, which made the whole caller-side seed in
    // AirportItlwmV2::start a no-op: state[0xe4] stayed zero, IO80211SkywalkInterface::start
    // handed that zero to IO80211MacAddressAgent::withOptions, and the agent minted a random
    // locally-administered address. Nothing fails — the interface comes up and can even complete
    // DHCP, because setLinkLayerAddress syncs whatever address the agent chose into net80211 —
    // so the only visible tell is en3 carrying a different MAC every boot instead of the card's.
    // Root AGENTS.md mechanism 21.
    if (initMac != NULL)
        setInitMacAddress(*initMac);
#endif
    instance = OSDynamicCast(AirportItlwm, provider);
    if (!instance)
        return false;
    this->fHalService = instance->fHalService;
    this->scanSource = instance->scanSource;
#if __IO80211_TARGET >= __MAC_26_0
    fPmkValid = false;
    memset(fPmk, 0, sizeof(fPmk));
    fTxPool = NULL;
    fRxPool = NULL;
    fTxSubQ = NULL;
    fTxComplQ = NULL;
    fRxSubQ = NULL;
    fRxComplQ = NULL;
    fSkywalkRegistered = false;
#endif
    return ret;
}

//ifnet_t AirportItlwmSkywalkInterface::
//getBSDInterface()
//{
//    if (instance->bsdInterface)
//        return instance->bsdInterface->getIfnet();
//    return NULL;
//}

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
getSSID(struct apple80211_ssid_data *sd)
{
    struct ieee80211com * ic = fHalService->get80211Controller();
    if (ic->ic_state == IEEE80211_S_RUN) {
        memset(sd, 0, sizeof(*sd));
        sd->version = APPLE80211_VERSION;
        memcpy(sd->ssid_bytes, ic->ic_des_essid, strlen((const char*)ic->ic_des_essid));
        sd->ssid_len = (uint32_t)strlen((const char*)ic->ic_des_essid);
        return kIOReturnSuccess;
    }
    return 6;
}
#endif

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
getAUTH_TYPE(struct apple80211_authtype_data *ad)
{
    ad->version = APPLE80211_VERSION;
    ad->authtype_lower = current_authtype_lower;
    ad->authtype_upper = current_authtype_upper;
    return kIOReturnSuccess;
}
#endif

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
setAUTH_TYPE(struct apple80211_authtype_data *ad)
{
    current_authtype_lower = ad->authtype_lower;
    current_authtype_upper = ad->authtype_upper;
    return kIOReturnSuccess;
}
#endif

IOReturn AirportItlwmSkywalkInterface::
setCIPHER_KEY(struct apple80211_key *key)
{
    XYLog("%s\n", __FUNCTION__);
    const char* keydump = hexdump(key->key, key->key_len);
    const char* rscdump = hexdump(key->key_rsc, key->key_rsc_len);
    const char* eadump = hexdump(key->key_ea.octet, APPLE80211_ADDR_LEN);
    static_assert(__offsetof(struct apple80211_key, key_ea) == 92, "struct corrupted");
    static_assert(__offsetof(struct apple80211_key, key_rsc_len) == 80, "struct corrupted");
    static_assert(__offsetof(struct apple80211_key, wowl_kck_len) == 100, "struct corrupted");
    static_assert(__offsetof(struct apple80211_key, wowl_kek_len) == 120, "struct corrupted");
    static_assert(__offsetof(struct apple80211_key, wowl_kck_key) == 104, "struct corrupted");
    if (keydump && rscdump && eadump)
        XYLog("Set key request: len=%d cipher_type=%d flags=%d index=%d key=%s rsc_len=%d rsc=%s ea=%s\n",
              key->key_len, key->key_cipher_type, key->key_flags, key->key_index, keydump, key->key_rsc_len, rscdump, eadump);
    else
        XYLog("Set key request, but failed to allocate memory for hexdump\n");
    
    if (keydump)
        IOFree((void*)keydump, 3 * key->key_len + 1);
    if (rscdump)
        IOFree((void*)rscdump, 3 * key->key_rsc_len + 1);
    if (eadump)
        IOFree((void*)eadump, 3 * APPLE80211_ADDR_LEN + 1);
    
    switch (key->key_cipher_type) {
        case APPLE80211_CIPHER_NONE:
            // clear existing key
//            XYLog("Setting NONE key is not supported\n");
            break;
        case APPLE80211_CIPHER_WEP_40:
        case APPLE80211_CIPHER_WEP_104:
            XYLog("Setting WEP key %d is not supported\n", key->key_index);
            break;
        case APPLE80211_CIPHER_TKIP:
        case APPLE80211_CIPHER_AES_OCB:
        case APPLE80211_CIPHER_AES_CCM:
            switch (key->key_flags) {
                case 4: // PTK
                    setPTK(key->key, key->key_len);
                    break;
                case 0: // GTK
                    setGTK(key->key, key->key_len, key->key_index, key->key_rsc);
                    break;
            }
            break;
        case APPLE80211_CIPHER_PMK:
            // A secondary route for the PMK, not the primary one. This was believed to be the
            // only way a key could reach the driver on Tahoe; measured on 26.6, the WCL never
            // calls it during a join — five associate requests, zero setCIPHER_KEY calls. The key
            // the WCL actually uses travels inside apple80211_assoc_candidates at +0x40, as a
            // whole apple80211_key; see AssocCandidates.h. Kept because the route is real (the
            // dispatch to slot 546 is genuinely virtual, unlike setSET_MAC_ADDRESS) and costs
            // nothing, and because a release or configuration that does use it then works.
            //
            // For WPA2/WPA3-PSK the PMK is exactly what net80211 wants in ic_psk:
            // ieee80211_pae_input.c copies ic_psk straight into ni_pmk for the 4-way handshake, so
            // no derivation is needed here. Same two lines the V1 associate path uses.
            //
            // Deliberately confined to __MAC_26_0. The WCL is Tahoe's stack; older releases
            // associate through setASSOCIATE, which sets ic_psk itself from the assoc data, so it
            // is not established that CIPHER_PMK ever arrives there. Accepting it unconditionally
            // would change behaviour on targets that work today on the strength of an assumption.
            // Widen the guard only after observing a pre-Tahoe release actually send this.
#if __IO80211_TARGET >= __MAC_26_0
            if (key->key_len < IEEE80211_PMK_LEN) {
                XYLog("PMK too short (%d < %d), ignoring\n", key->key_len, IEEE80211_PMK_LEN);
                break;
            }
            {
                struct ieee80211com *ic = fHalService->get80211Controller();
                // Kept in the driver as well as in ic_psk, because ic_psk does not survive the
                // association path: associateSSID opens with ieee80211_disable_rsn, which
                // memsets ic_psk and clears IEEE80211_F_PSK. The pre-Tahoe caller copies its key
                // back in immediately afterwards; the WCL caller has no key to copy, so without
                // this stash the PMK is gone by the time ieee80211_match_bss looks for it — and
                // that check ("AP only supports PSK AKMPs" with F_PSK clear) rejects the target
                // BSS silently and forever.
                memcpy(fPmk, key->key, IEEE80211_PMK_LEN);
                fPmkValid = true;
                memcpy(ic->ic_psk, key->key, IEEE80211_PMK_LEN);
                ic->ic_flags |= IEEE80211_F_PSK;
                XYLog("Set WPA PMK (%d bytes)\n", IEEE80211_PMK_LEN);
            }
#else
            XYLog("Setting WPA PMK is not supported\n");
#endif
            break;
        case APPLE80211_CIPHER_MSK:
            XYLog("Setting MSK\n");
            ieee80211_pmksa_add(fHalService->get80211Controller(), IEEE80211_AKM_8021X,
                                fHalService->get80211Controller()->ic_bss->ni_macaddr, key->key, 0);
            break;
        case APPLE80211_CIPHER_PMKSA:
            XYLog("Setting WPA PMKSA\n");
            ieee80211_pmksa_add(fHalService->get80211Controller(), IEEE80211_AKM_8021X,
                                fHalService->get80211Controller()->ic_bss->ni_macaddr, key->key, 0);
            break;
    }
    //fInterface->postMessage(APPLE80211_M_CIPHER_KEY_CHANGED);
    return kIOReturnSuccess;
}

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
getPHY_MODE(struct apple80211_phymode_data *pd)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    
    pd->version = APPLE80211_VERSION;
    pd->phy_mode = APPLE80211_MODE_11A
    | APPLE80211_MODE_11B
    | APPLE80211_MODE_11G
    | APPLE80211_MODE_11N;
    
    if (ic->ic_flags & IEEE80211_F_VHTON)
        pd->phy_mode |= APPLE80211_MODE_11AC;
    
    if (ic->ic_flags & IEEE80211_F_HEON)
        pd->phy_mode |= APPLE80211_MODE_11AX;
    
    switch (fHalService->get80211Controller()->ic_curmode) {
        case IEEE80211_MODE_AUTO:
            pd->active_phy_mode = APPLE80211_MODE_AUTO;
            break;
        case IEEE80211_MODE_11A:
            pd->active_phy_mode = APPLE80211_MODE_11A;
            break;
        case IEEE80211_MODE_11B:
            pd->active_phy_mode = APPLE80211_MODE_11B;
            break;
        case IEEE80211_MODE_11G:
            pd->active_phy_mode = APPLE80211_MODE_11G;
            break;
        case IEEE80211_MODE_11N:
            pd->active_phy_mode = APPLE80211_MODE_11N;
            break;
        case IEEE80211_MODE_11AC:
            pd->active_phy_mode = APPLE80211_MODE_11AC;
            break;
        case IEEE80211_MODE_11AX:
            pd->active_phy_mode = APPLE80211_MODE_11AX;
            break;
            
        default:
            pd->active_phy_mode = APPLE80211_MODE_AUTO;
            break;
    }
    
    return kIOReturnSuccess;
}
#endif

IOReturn AirportItlwmSkywalkInterface::
getCHANNEL(struct apple80211_channel_data *cd)
{
    struct ieee80211com * ic = fHalService->get80211Controller();
    if (ic->ic_state == IEEE80211_S_RUN) {
        memset(cd, 0, sizeof(apple80211_channel_data));
        cd->version = APPLE80211_VERSION;
        cd->channel.version = APPLE80211_VERSION;
        cd->channel.channel = ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan);
        cd->channel.flags = ieeeChanFlag2apple(ic->ic_bss->ni_chan->ic_flags, ic->ic_bss->ni_chw);
        return kIOReturnSuccess;
    }
    return 6;
}

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
getSTATE(struct apple80211_state_data *sd)
{
    memset(sd, 0, sizeof(*sd));
    sd->version = APPLE80211_VERSION;
    sd->state = fHalService->get80211Controller()->ic_state;
    return kIOReturnSuccess;
}
#endif

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
getMCS_INDEX_SET(struct apple80211_mcs_index_set_data *ad)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic->ic_state == IEEE80211_S_RUN) {
        memset(ad, 0, sizeof(*ad));
        ad->version = APPLE80211_VERSION;
        size_t size = min(ARRAY_SIZE(ic->ic_bss->ni_rxmcs), ARRAY_SIZE(ad->mcs_set_map));
        for (int i = 0; i < size; i++)
            ad->mcs_set_map[i] = ic->ic_bss->ni_rxmcs[i];
        return kIOReturnSuccess;
    }
    return 6;
}
#endif

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
getVHT_MCS_INDEX_SET(struct apple80211_vht_mcs_index_set_data *data)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic->ic_bss == NULL || ic->ic_curmode < IEEE80211_MODE_11AC) {
        return kIOReturnError;
    }
    memset(data, 0, sizeof(struct apple80211_vht_mcs_index_set_data));
    data->version = APPLE80211_VERSION;
    data->mcs_map = ic->ic_bss->ni_vht_mcsinfo.tx_mcs_map;
    return kIOReturnSuccess;
}
#endif

IOReturn AirportItlwmSkywalkInterface::
getMCS_VHT(struct apple80211_mcs_vht_data *data)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic->ic_bss == NULL || ic->ic_curmode < IEEE80211_MODE_11AC) {
        return kIOReturnError;
    }
    memset(data, 0, sizeof(struct apple80211_mcs_vht_data));
    data->version = APPLE80211_VERSION;
    data->guard_interval = (ieee80211_node_supports_vht_sgi80(ic->ic_bss) || ieee80211_node_supports_vht_sgi160(ic->ic_bss)) ? APPLE80211_GI_SHORT : APPLE80211_GI_LONG;
    data->index = ic->ic_bss->ni_txmcs;
    data->nss = fHalService->getDriverInfo()->getTxNSS();
    switch (ic->ic_bss->ni_chw) {
        case IEEE80211_CHAN_WIDTH_40:
            data->bw = 40;
            break;
        case IEEE80211_CHAN_WIDTH_80:
            data->bw = 80;
            break;
        case IEEE80211_CHAN_WIDTH_80P80:
        case IEEE80211_CHAN_WIDTH_160:
            data->bw = 160;
            break;
            
        default:
            data->bw = 20;
            break;
    }
    return kIOReturnSuccess;
}

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
getRATE_SET(struct apple80211_rate_set_data *ad)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic->ic_state == IEEE80211_S_RUN) {
        memset(ad, 0, sizeof(*ad));
        ad->version = APPLE80211_VERSION;
        ad->num_rates = ic->ic_bss->ni_rates.rs_nrates;
        size_t size = min(ic->ic_bss->ni_rates.rs_nrates, ARRAY_SIZE(ad->rates));
        // By reference. This loop used to bind `apple80211_rate apple_rate = ad->rates[i]` — a
        // *copy* — so every store below was discarded and the reply carried a non-zero num_rates
        // with an all-zero rate table.
        for (int i=0; i < size; i++) {
            struct apple80211_rate &apple_rate = ad->rates[i];
            apple_rate.version = APPLE80211_VERSION;
            apple_rate.rate = ic->ic_bss->ni_rates.rs_rates[i];
            apple_rate.flags = 0;
        }
        return kIOReturnSuccess;
    }
    return 6;
}
#endif

#if __IO80211_TARGET >= __MAC_26_0
// Ioctl 460, the second gate on a link coming up. Tahoe dropped getRATE_SET/getMCS_INDEX_SET from
// IO80211InfraProtocol, so this cannot delegate to them the way a pre-Tahoe composition would; the
// rate set is filled here directly from the associated node.
//
// Everything left untouched stays as the WCL's IOMallocZeroData left it, and that is the correct
// answer rather than a gap: zero HT/VHT/HE MCS maps and a zero MLO context say "not supported",
// which is true of this port. The one field worth real effort is the RSN element, because it is
// what tells the family how the link is protected, and the driver already has it — the join scan
// cached the target's beacon for getWCL_BSS_INFO.
IOReturn AirportItlwmSkywalkInterface::
getWCL_EXTENDED_BSS_INFO(struct apple80211_extended_bss_info *info)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    struct ieee80211_node *ni = ic->ic_bss;

    if (info == NULL)
        return kIOReturnError;
    gItlwmExtBssInfoCalls++;
    info->rate_set.version = APPLE80211_VERSION;
    if (ni != NULL) {
        size_t n = min(ni->ni_rates.rs_nrates, ARRAY_SIZE(info->rate_set.rates));
        info->rate_set.num_rates = (uint16_t)n;
        for (size_t i = 0; i < n; i++) {
            info->rate_set.rates[i].version = APPLE80211_VERSION;
            info->rate_set.rates[i].rate = ni->ni_rates.rs_rates[i];
            info->rate_set.rates[i].flags = 0;
        }
    }
    info->mcs.version = APPLE80211_VERSION;
    info->vht_mcs.version = APPLE80211_VERSION;
    instance->copyCurrentBssIe(IEEE80211_ELEMID_RSN, info->wpa_rsn_ie, sizeof(info->wpa_rsn_ie));
    return kIOReturnSuccess;
}
#endif

IOReturn AirportItlwmSkywalkInterface::
getOP_MODE(struct apple80211_opmode_data *od)
{
    od->version = APPLE80211_VERSION;
    od->op_mode = APPLE80211_M_STA;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
getTXPOWER(struct apple80211_txpower_data *txd)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic->ic_state == IEEE80211_S_RUN) {
        memset(txd, 0, sizeof(*txd));
        txd->version = APPLE80211_VERSION;
        txd->txpower = ic->ic_txpower;
        txd->txpower_unit = APPLE80211_UNIT_PERCENT;
        return kIOReturnSuccess;
    }
    return 6;
}

IOReturn AirportItlwmSkywalkInterface::
getRATE(struct apple80211_rate_data *rd)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic->ic_bss == NULL)
        return 6;
    int nss;
    int sgi;
    int index = 0;
    if (ic->ic_state == IEEE80211_S_RUN) {
        memset(rd, 0, sizeof(*rd));
        rd->version = APPLE80211_VERSION;
        rd->num_radios = 1;
        sgi = ieee80211_node_supports_sgi(ic->ic_bss);
        if (ic->ic_curmode == IEEE80211_MODE_11AC) {
            if (sgi)
                index += 1;
            nss = fHalService->getDriverInfo()->getTxNSS();
            switch (ic->ic_bss->ni_chw) {
                case IEEE80211_CHAN_WIDTH_40:
                    index += 4;
                    break;
                case IEEE80211_CHAN_WIDTH_80:
                    index += 8;
                    break;
                case IEEE80211_CHAN_WIDTH_80P80:
                case IEEE80211_CHAN_WIDTH_160:
                    index += 12;
                    break;

                default:
                    break;
            }
            index += 2 * (nss - 1);
            const struct ieee80211_vht_rateset *rs = &ieee80211_std_ratesets_11ac[index];
            rd->rate[0] = rs->rates[ic->ic_bss->ni_txmcs % rs->nrates] / 2;
        } else if (ic->ic_curmode == IEEE80211_MODE_11N) {
            int is_40mhz = ic->ic_bss->ni_chw == IEEE80211_CHAN_WIDTH_40;
            if (sgi)
                index += 1;
            if (is_40mhz)
                index += (IEEE80211_HT_RATESET_MIMO4_SGI + 1);
            index += (ic->ic_bss->ni_txmcs / 16);
            nss = ic->ic_bss->ni_txmcs / 8 + 1;
            index += 2 * (nss - 1);
            rd->rate[0] = ieee80211_std_ratesets_11n[index].rates[ic->ic_bss->ni_txmcs % 8] / 2;
        } else
            rd->rate[0] = ic->ic_bss->ni_rates.rs_rates[ic->ic_bss->ni_txrate];
        return kIOReturnSuccess;
    }
    return 6;
}

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
getBSSID(struct apple80211_bssid_data *bd)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic->ic_state == IEEE80211_S_RUN) {
        memset(bd, 0, sizeof(*bd));
        bd->version = APPLE80211_VERSION;
        memcpy(bd->bssid.octet, ic->ic_bss->ni_bssid, APPLE80211_ADDR_LEN);
        return kIOReturnSuccess;
    }
    return 6;
}
#endif

IOReturn AirportItlwmSkywalkInterface::
getRSSI(struct apple80211_rssi_data *rd)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic->ic_state == IEEE80211_S_RUN) {
        memset(rd, 0, sizeof(*rd));
        rd->num_radios = 1;
        rd->rssi_unit = APPLE80211_UNIT_DBM;
        rd->rssi[0] = rd->aggregate_rssi
        = rd->rssi_ext[0]
        = rd->aggregate_rssi_ext
        = -(0 - IWM_MIN_DBM - ic->ic_bss->ni_rssi);
        return kIOReturnSuccess;
    }
    return 6;
}

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
getRSN_IE(struct apple80211_rsn_ie_data *data)
{
#ifdef USE_APPLE_SUPPLICANT
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic->ic_bss == NULL || ic->ic_bss->ni_rsnie == NULL) {
        return kIOReturnError;
    }
    data->version = APPLE80211_VERSION;
    if (ic->ic_rsn_ie_override[1] > 0) {
        data->len = 2 + ic->ic_rsn_ie_override[1];
        memcpy(data->ie, ic->ic_rsn_ie_override, data->len);
    }
    else {
        data->len = 2 + ic->ic_bss->ni_rsnie[1];
        memcpy(data->ie, ic->ic_bss->ni_rsnie, data->len);
    }
    return kIOReturnSuccess;
#else
    return kIOReturnUnsupported;
#endif
}
#endif

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
setRSN_IE(struct apple80211_rsn_ie_data *data)
{
#ifdef USE_APPLE_SUPPLICANT
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!data)
        return kIOReturnError;
    static_assert(sizeof(ic->ic_rsn_ie_override) == APPLE80211_MAX_RSN_IE_LEN, "Max RSN IE length mismatch");
    memcpy(ic->ic_rsn_ie_override, data->ie, APPLE80211_MAX_RSN_IE_LEN);
    if (ic->ic_state == IEEE80211_S_RUN && ic->ic_bss != nullptr)
        ieee80211_save_ie(data->ie, &ic->ic_bss->ni_rsnie);
    return kIOReturnSuccess;
#else
    return kIOReturnUnsupported;
#endif
}
#endif

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
getAP_IE_LIST(struct apple80211_ap_ie_data *data)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!data)
        return kIOReturnError;
    if (ic->ic_bss == NULL || ic->ic_bss->ni_rsnie_tlv == NULL || ic->ic_bss->ni_rsnie_tlv_len == 0 || ic->ic_bss->ni_rsnie_tlv_len > data->len || ic->ic_bss->ni_rsnie_tlv_len > 1024)
        return kIOReturnError;
    data->version = APPLE80211_VERSION;
    data->len = ic->ic_bss->ni_rsnie_tlv_len;
    memcpy(data->ie_data, ic->ic_bss->ni_rsnie_tlv, data->len);
    return kIOReturnSuccess;
}
#endif

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
getNOISE(struct apple80211_noise_data *nd)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic->ic_state == IEEE80211_S_RUN) {
        memset(nd, 0, sizeof(*nd));
        nd->version = APPLE80211_VERSION;
        nd->num_radios = 1;
        nd->noise[0]
        = nd->aggregate_noise = -fHalService->getDriverInfo()->getBSSNoise();
        nd->noise_unit = APPLE80211_UNIT_DBM;
        return kIOReturnSuccess;
    }
    return 6;
}
#endif

IOReturn AirportItlwmSkywalkInterface::
getPOWERSAVE(struct apple80211_powersave_data *pd)
{
    pd->version = APPLE80211_VERSION;
    pd->powersave_level = APPLE80211_POWERSAVE_MODE_DISABLED;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
getNSS(struct apple80211_nss_data *data)
{
    memset(data, 0, sizeof(*data));
    data->version = APPLE80211_VERSION;
    data->nss = fHalService->getDriverInfo()->getTxNSS();
    return kIOReturnSuccess;
}

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
setASSOCIATE(struct apple80211_assoc_data *ad)
{
    XYLog("%s [%s] mode=%d ad_auth_lower=%d ad_auth_upper=%d rsn_ie_len=%d%s%s%s%s%s%s%s\n", __FUNCTION__, ad->ad_ssid, ad->ad_mode, ad->ad_auth_lower, ad->ad_auth_upper, ad->ad_rsn_ie_len,
          (ad->ad_flags & 2) ? ", Instant Hotspot" : "",
          (ad->ad_flags & 4) ? ", Auto Instant Hotspot" : "",
          (ad->ad_rsn_ie[APPLE80211_MAX_RSN_IE_LEN] & 1) ? ", don't disassociate" : "",
          (ad->ad_rsn_ie[APPLE80211_MAX_RSN_IE_LEN] & 2) ? ", don't blacklist" : "",
          (ad->ad_rsn_ie[APPLE80211_MAX_RSN_IE_LEN] & 4) ? ", closed Network" : "",
          (ad->ad_rsn_ie[APPLE80211_MAX_RSN_IE_LEN] & 8) ? ", 802.1X" : "",
          (ad->ad_rsn_ie[APPLE80211_MAX_RSN_IE_LEN] & 0x20) ? ", force BSSID" : "");
    
    struct apple80211_rsn_ie_data rsn_ie_data;
    struct apple80211_authtype_data auth_type_data;
    struct ieee80211com *ic = fHalService->get80211Controller();

    if (!ad)
        return kIOReturnError;
    
    if (ic->ic_state < IEEE80211_S_SCAN)
        return kIOReturnSuccess;
    
    if (ic->ic_state == IEEE80211_S_ASSOC || ic->ic_state == IEEE80211_S_AUTH)
        return kIOReturnSuccess;

    if (ad->ad_mode != APPLE80211_AP_MODE_IBSS) {
        disassocIsVoluntary = false;
        auth_type_data.version = APPLE80211_VERSION;
        auth_type_data.authtype_upper = ad->ad_auth_upper;
        auth_type_data.authtype_lower = ad->ad_auth_lower;
        setAUTH_TYPE(&auth_type_data);
        rsn_ie_data.version = APPLE80211_VERSION;
        rsn_ie_data.len = ad->ad_rsn_ie[1] + 2;
        memcpy(rsn_ie_data.ie, ad->ad_rsn_ie, rsn_ie_data.len);
        setRSN_IE(&rsn_ie_data);

        associateSSID(ad->ad_ssid, ad->ad_ssid_len, ad->ad_bssid, ad->ad_auth_lower, ad->ad_auth_upper, ad->ad_key.key, ad->ad_key.key_len, ad->ad_key.key_index);
    }
    return kIOReturnSuccess;
}
#endif

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
setDISASSOCIATE(struct apple80211_disassoc_data *ad)
{
    XYLog("%s\n", __FUNCTION__);
    struct ieee80211com *ic = fHalService->get80211Controller();

    if (ic->ic_state < IEEE80211_S_SCAN)
        return kIOReturnSuccess;
    
    if (ic->ic_state > IEEE80211_S_AUTH && ic->ic_bss != NULL)
        IEEE80211_SEND_MGMT(ic, ic->ic_bss, IEEE80211_FC0_SUBTYPE_DEAUTH, IEEE80211_REASON_AUTH_LEAVE);
    
    if (ic->ic_state == IEEE80211_S_ASSOC || ic->ic_state == IEEE80211_S_AUTH)
        return kIOReturnSuccess;
    
    disassocIsVoluntary = true;

    ieee80211_del_ess(ic, nullptr, 0, 1);
    ieee80211_deselect_ess(ic);
#ifdef USE_APPLE_SUPPLICANT
    ic->ic_rsn_ie_override[1] = 0;
#endif
    ic->ic_assoc_status = APPLE80211_STATUS_UNAVAILABLE;
    ic->ic_deauth_reason = APPLE80211_REASON_ASSOC_LEAVING;
    ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
    return kIOReturnSuccess;
}
#endif

IOReturn AirportItlwmSkywalkInterface::
getSUPPORTED_CHANNELS(struct apple80211_sup_channel_data *ad)
{
    if (!ad)
        return kIOReturnError;
    ad->version = APPLE80211_VERSION;
    ad->num_channels = 0;
    struct ieee80211com *ic = fHalService->get80211Controller();
    for (int i = 0; i < IEEE80211_CHAN_MAX; i++) {
        if (ic->ic_channels[i].ic_freq != 0) {
            ad->supported_channels[ad->num_channels].channel = ieee80211_chan2ieee(ic, &ic->ic_channels[i]);
            ad->supported_channels[ad->num_channels].flags = ieeeChanFlag2appleScanFlagVentura(ic->ic_channels[i].ic_flags);
            ad->num_channels++;
        }
    }
    return kIOReturnSuccess;
}

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
getLOCALE(struct apple80211_locale_data *ld)
{
    if (!ld)
        return kIOReturnError;
    ld->version = APPLE80211_VERSION;
    ld->locale  = APPLE80211_LOCALE_FCC;
    
    return kIOReturnSuccess;
}
#endif

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
getDEAUTH(struct apple80211_deauth_data *da)
{
    if (!da)
        return kIOReturnError;
    da->version = APPLE80211_VERSION;
    struct ieee80211com *ic = fHalService->get80211Controller();
    da->deauth_reason = ic->ic_deauth_reason;
//    XYLog("%s, %d\n", __FUNCTION__, da->deauth_reason);
    return kIOReturnSuccess;
}
#endif

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
getASSOCIATION_STATUS(struct apple80211_assoc_status_data *hv)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    
    if (!hv)
        return kIOReturnError;
    memset(hv, 0, sizeof(*hv));
    hv->version = APPLE80211_VERSION;
    if (ic->ic_state == IEEE80211_S_RUN)
        hv->status = APPLE80211_STATUS_SUCCESS;
    else
        hv->status = APPLE80211_STATUS_UNAVAILABLE;
//    XYLog("%s, %d\n", __FUNCTION__, hv->status);
    return kIOReturnSuccess;
}
#endif

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
setSCANCACHE_CLEAR(void *req)
{
    XYLog("%s\n", __FUNCTION__);
    struct ieee80211com *ic = fHalService->get80211Controller();
    //if doing background or active scan, don't free nodes.
    if ((ic->ic_flags & IEEE80211_F_BGSCAN) || (ic->ic_flags & IEEE80211_F_ASCAN))
        return kIOReturnSuccess;
    ieee80211_free_allnodes(ic, 0);
    return kIOReturnSuccess;
}
#endif

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
setDEAUTH(struct apple80211_deauth_data *da)
{
    XYLog("%s\n", __FUNCTION__);
    return kIOReturnSuccess;
}
#endif

IOReturn AirportItlwmSkywalkInterface::
getMCS(struct apple80211_mcs_data* md)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic->ic_state != IEEE80211_S_RUN ||  ic->ic_bss == NULL || !md)
        return 6;
    md->version = APPLE80211_VERSION;
    md->index = ic->ic_bss->ni_txmcs;
    return kIOReturnSuccess;
}

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
getLINK_CHANGED_EVENT_DATA(struct apple80211_link_changed_event_data *ed)
{
    if (ed == nullptr)
        return 16;
    
    struct ieee80211com *ic = fHalService->get80211Controller();
    
    bzero(ed, sizeof(apple80211_link_changed_event_data));
    ed->isLinkDown = !(instance->currentStatus & kIONetworkLinkActive);
    if (ed->isLinkDown) {
        ed->voluntary = disassocIsVoluntary;
        ed->reason = APPLE80211_LINK_DOWN_REASON_DEAUTH;
    } else
        ed->rssi = -(0 - IWM_MIN_DBM - ic->ic_bss->ni_rssi);
    XYLog("Link %s, reason: %d, voluntary: %d\n", ed->isLinkDown ? "down" : "up", ed->reason, ed->voluntary);
    return kIOReturnSuccess;
}
#endif

#if __IO80211_TARGET >= __MAC_26_0
// Tahoe's only scan entry point for an infra driver.
//
// How a scan reaches here, all of it established by disassembly:
//
//   airportd  APPLE80211_IOC_SCAN_REQ (type 10)
//     -> setSCAN_REQ, the file-static gSetHandlerTable[10] entry
//          getPrimaryInterfaceScanManager()   NULL here used to be the ENXIO era
//          IO80211ScanManager::isScanAllowedByP2P
//          IO80211Controller::scanStarted
//          IO80211Glue::sendIOUCToWcl(..., type 10, ...)      -> the WCL owns the scan
//     -> apple80211setWCL_SCAN_REQ
//          isCommandProhibited(441)
//          safeMetaCast to the infra protocol
//          slot 601 -> this
//
// The same handler has a legacy fallback, `apple80211setSCAN_REQ`, but it is unreachable for
// this driver: it safeMetaCasts to IO80211NoneProtocol and returns 0xe082280e for anything
// else. So there is no second route — if this returns an error, scanning is simply dead, and
// `kIOReturnUnsupported` (what this was until now) came straight back out to airportd as
// 0xe00002c7 with `Apple80211Scan Failed`.
//
// The work this does is deliberately the same as the pre-Tahoe setSCAN_REQ below, because that
// path is exercised on every shipping target: kick net80211's cached background scan and arm
// scanSource, whose 100 ms timeout posts APPLE80211_M_SCAN_DONE. scanSource is the controller's
// timer, shared with this object in init, so completion is already wired.
//
// Gated because the WCL calls us on its own thread while net80211 state is otherwise only
// touched from the work loop. runAction returns the action's own IOReturn.
//
// Not yet done: this ignores the apple80211ScanRequest entirely — no channel subset, no SSID
// filter, no dwell times — and results are whatever net80211 already has cached. Known layout
// so far, from AppleBCMWLANCore::setWCL_SCAN_REQ: +0x54 is the channel count and +0x5c an array
// of 12-byte entries whose first dword is the channel number. Honour those before calling scan
// support complete.
IOReturn AirportItlwmSkywalkInterface::
beginScanGated(OSObject *target, void *, void *, void *, void *)
{
    AirportItlwmSkywalkInterface *that = OSDynamicCast(AirportItlwmSkywalkInterface, target);
    if (that == NULL || that->fHalService == NULL || that->instance == NULL)
        return kIOReturnNotReady;
    struct ieee80211com *ic = that->fHalService->get80211Controller();
    ItlDriverController *dc = that->fHalService->getDriverController();

    gItlwmScanReqState = ic->ic_state;
    if (that->fScanResultWrapping)
        return kIOReturnBusy;

    switch (ic->ic_state) {
    case IEEE80211_S_RUN:
        // Associated. begin_cache_bgscan is the only entry point that does not disturb the link.
        // It returns void and declines SILENTLY when IEEE80211_F_BGSCAN is already set, when
        // ic_mgt_timer != 0, and when the RSN port is not yet valid (ieee80211.c:135-141).
        //
        // ACCEPT EITHER WAY — do not turn a decline into a refusal. Today an associated scan
        // always returns success and completes in 100 ms, so the WCL harvests its beacon cache
        // even when nothing started; refusing instead would convert a soft failure into a hard
        // one on the path the machine depends on, including during every 4-way handshake
        // (ic_mgt_timer != 0). The backstop answers the declined case, and ItlwmScanReqNoStart
        // names it. The command begin_cache_bgscan issues is ASYNC, so this is safe under the
        // gate; a synchronous host command here would self-deadlock, because tsleep_nsec sleeps
        // without releasing the work-loop gate the completion interrupt needs.
        ieee80211_begin_cache_bgscan(&ic->ic_ac.ac_if);
        if (dc == NULL || !dc->isScanning())
            gItlwmScanReqNoStart++;
        break;

    case IEEE80211_S_SCAN:
        // Disconnected — the auto-join case. NOTHING IS STARTED HERE, ON PURPOSE.
        //
        // begin_cache_bgscan is a hard no-op outside RUN, which is why the old code certified a
        // scan that had never begun. The fix is NOT to start one: net80211 is already sweeping
        // continuously without being asked. ieee80211_begin_scan ran once from iwx_init and
        // every completion re-enters end_scan's `notfound` path, which calls next_scan ->
        // new_state(SCAN) forever, because IEEE80211_F_AUTO_JOIN with an empty ic_des_essid
        // makes ieee80211_match_bss reject every candidate. That loop is what produced
        // ItlwmScanBeacons = 2108 with no request outstanding, and riding it is exactly what
        // makes the completion honest.
        //
        // **DO NOT nudge ieee80211_new_state(ic, IEEE80211_S_SCAN, -1) here.** It looks free and
        // is not. iwx_newstate exempts SCAN from its same-state guard and writes sc->ns_nstate
        // and sc->ns_arg UNCONDITIONALLY, then calls iwx_add_task, whose task_add is a no-op
        // when newstate_task is already queued. So a nudge issued after the join path queued an
        // AUTH transition — which end_scan does, via ieee80211_node_join_bss, on this same
        // thread — rewrites ns_nstate from AUTH to SCAN and the authentication is silently
        // dropped. No ic_state test can see it, because ic_state has not moved yet.
        break;

    default:
        // INIT (net80211 never started — a separate bug; one boot measured 502/502 refusals
        // with 0 beacons) or AUTH/ASSOC (a join is mid-exchange and a sweep would take the
        // radio off-channel). Refuse.
        //
        // A refusal is CLEAN but not free, and the cost is worth stating: it reaches
        // WCLScanManager::handleScanRequest, which turns a non-zero return into SCAN_REQ_FAILED,
        // whose handler disassembles to `xor eax,eax; ret` — so the FSM returns to IDLE and NO
        // scan-complete reaches userspace at all, and airportd's request waits out its own
        // timeout. That is still better than the false success this used to return, which is
        // what produced the 10 Hz storm.
        return kIOReturnBusy;
    }

    if (dc != NULL && dc->isScanning())
        gItlwmScanReqInFlight++;   // the sweep that will complete us started BEFORE the request
    // Everything touching scanSource now goes through the CONTROLLER. The interface's own
    // `scanSource` (set in init) is an unretained alias that releaseAll never NULLs.
    that->instance->scanRequested();
    return kIOReturnSuccess;
}

// Ioctl 435, slot 596 — mechanism 13's companion, and NOT optional once completions take
// seconds. WCLScanManager::abortScan cancels its own scan timer, sends 435, and on a NON-ZERO
// return immediately synthesises SCAN_COMPLETE(kIOReturnAborted) and drops to IDLE — the fast
// recovery. On SUCCESS it instead arms a 1000 ms timer and waits for a 237 we would then owe it.
//
// THE RETURN VALUE THEREFORE STAYS kIOReturnUnsupported ON PURPOSE. What changes is purely
// local: drop our own outstanding completion, so that neither the real sweep's SCAN_DONE nor the
// backstop can post a stale 237 into SCAN_MANAGER_STATE_IDLE — where the transition table runs
// handleScanComplete rather than ignoring it, costing a full re-harvest and a spurious
// userspace SCAN_DONE against a request that is no longer current.
//
// No runAction wrapper: scanAborted only CASes the latch and never touches the timer, precisely
// because this can be called without _fWorkloop's gate.
IOReturn AirportItlwmSkywalkInterface::
setWCL_SCAN_ABORT(void *)
{
    if (instance != NULL)
        instance->scanAborted();
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmSkywalkInterface::
setWCL_SCAN_REQ(apple80211ScanRequest *req)
{
    gItlwmScanReqCalls++;
    IO80211WorkQueue *wq = getWorkQueue();
    IOReturn ret = (wq != NULL) ? wq->runAction(beginScanGated, this)
                                : beginScanGated(this, NULL, NULL, NULL, NULL);
    if (ret == kIOReturnSuccess)
        gItlwmScanReqStarted++;
    else
        gItlwmScanReqRefused++;
    return ret;
}

// Tahoe's only association entry point for an infra driver, slot 602 — mechanism 15.
//
// Route, all from disassembly:
//
//   WCLJoinManager FSM, state IN_PROGRESS, event TRY_NEXT_CANDIDATE
//     -> handleSendCandidateToDriver
//          IOMallocZeroData(0x6f8)
//          WCLJoinRequest::fillAssocCandidatesList   + ::getVendorSpeificIes
//          WCLFsmManager::cmdIouc(442, false, candidates, 0x6f8, NULL, 0)
//     -> apple80211setWCL_ASSOCIATE -> slot 602 -> this
//
// **The return value barely matters and must not be used to report failure.** A non-zero result
// from cmdIouc reaches nothing but CCLogStream::logEmergency("Failed to send candidate to
// driver") — no FSM event is raised — so an error here does not fail the join, it *stalls* it
// until the 35 s timeout. Failure is reported through message 213 instead; see
// AirportItlwm::postJoinConnectComplete.
//
// What the WCL hands us and what we do with it (see include/Airport/AssocCandidates.h):
//
//   ssid / ssid_len       -> ic_des_essid, which is what ieee80211_match_bss selects on
//   upper_auth_type       -> the APPLE80211_AUTHTYPE_* bitmask associateSSID already understands
//   candidates[0].bssid   -> ic_des_bssid + IEEE80211_F_DESBSSID
//
// candidates[0] is *the* candidate to try, not merely the first of a set: on a failure the FSM
// raises TRY_NEXT_CANDIDATE, WCLJoinRequest::updateAndCheckForNextCandidate pulls the next
// WCLJoinCandidate off its queue into joinRequest[0x10]+0x20 and bumps the attempt counter at +4
// (giving up at 4), and handleSendCandidateToDriver then rebuilds the whole struct from that new
// current candidate. So pinning the BSSID honours the WCL's choice and still walks the list.
//
// No key is passed, and none is needed: the PMK arrived earlier through
// setCIPHER_KEY(APPLE80211_CIPHER_PMK) and is already in ic_psk. Nor is the RSN IE used — this
// file's setRSN_IE is a USE_APPLE_SUPPLICANT feature and Sonoma/Tahoe do not define it, so
// net80211 builds its own RSN IE from the parameters associateSSID sets.
//
// Gated, because the WCL calls in on its own thread while net80211 state is otherwise touched
// only from the work loop — the same reason setWCL_SCAN_REQ is gated.

// The upper auth types for which associateSSID consumes a PSK. WPA3-SAE is included because
// associateSSID fakes SAE as WPA2-PSK — this port does not implement SAE — so those joins need a
// key too.
#define ITLWM_AUTHTYPE_NEEDS_PSK (APPLE80211_AUTHTYPE_WPA_PSK  |    \
                                  APPLE80211_AUTHTYPE_WPA2_PSK |    \
                                  APPLE80211_AUTHTYPE_SHA256_PSK |  \
                                  APPLE80211_AUTHTYPE_WPA3_SAE |    \
                                  APPLE80211_AUTHTYPE_WPA3_FT_SAE)

IOReturn AirportItlwmSkywalkInterface::
beginAssociateGated(OSObject *target, void *arg0, void *, void *, void *)
{
    AirportItlwmSkywalkInterface *that = OSDynamicCast(AirportItlwmSkywalkInterface, target);
    struct apple80211_assoc_candidates *cand = (struct apple80211_assoc_candidates *)arg0;
    struct ether_addr bssid;
    uint32_t ssid_len;

    if (that == NULL || that->fHalService == NULL || that->instance == NULL || cand == NULL)
        return kIOReturnNotReady;

    struct ieee80211com *ic = that->fHalService->get80211Controller();
    if (ic->ic_state <= IEEE80211_S_INIT)
        return kIOReturnNotReady;

    ssid_len = cand->ssid_len;
    if (ssid_len > sizeof(cand->ssid))
        ssid_len = sizeof(cand->ssid);

    bzero(&bssid, sizeof(bssid));
    if (cand->candidate_count > 0)
        IEEE80211_ADDR_COPY(bssid.octet, cand->candidates[0].bssid);

    XYLog("%s ssid=%.*s bssid=%s upper_auth=0x%x candidates=%u state=%d\n", __FUNCTION__,
          (int)ssid_len, cand->ssid, ether_sprintf(bssid.octet),
          cand->upper_auth_type, cand->candidate_count, ic->ic_state);

    // Register the join before touching net80211: everything below can reach the state machine,
    // and a completion the driver is not expecting is a completion it drops.
    that->instance->joinStarted(bssid.octet, ssid_len);

    // An authentication or association exchange already in flight is left to finish rather than
    // restarted, exactly as the pre-Tahoe setASSOCIATE does. The join is registered either way,
    // so whichever way that exchange ends, the WCL is told.
    if (ic->ic_state == IEEE80211_S_AUTH || ic->ic_state == IEEE80211_S_ASSOC)
        return kIOReturnSuccess;

    that->disassocIsVoluntary = false;
    that->current_authtype_lower = APPLE80211_AUTHTYPE_OPEN;
    that->current_authtype_upper = cand->upper_auth_type;

    // The key comes with the request, and it has to be handed to associateSSID rather than left
    // anywhere for it to find: its first act is ieee80211_disable_rsn, which zeroes ic_psk and
    // clears IEEE80211_F_PSK. Anything already in ic_psk is gone before the function does its
    // work, so the only key that survives is the one passed as an argument.
    //
    // `cand->key` is preferred over the setCIPHER_KEY stash because it is the route that actually
    // fires: measured on 26.6, five associate calls arrived with `fPmkValid` false every time —
    // the WCL never calls setCIPHER_KEY on this path. The stash stays as a fallback for a release
    // or configuration that does.
    const struct apple80211_key *ck = &cand->key;
    uint8_t *key = NULL;
    uint32_t key_len = 0;
    int key_index = 0;
    if (ck->key_len > 0 && ck->key_len <= sizeof(ck->key)) {
        key = (uint8_t *)ck->key;
        key_len = ck->key_len;
        key_index = ck->key_index;
        gItlwmAssocKeyInReq++;
    } else if (that->fPmkValid) {
        key = that->fPmk;
        key_len = IEEE80211_PMK_LEN;
    }
    if (key == NULL && (cand->upper_auth_type & ITLWM_AUTHTYPE_NEEDS_PSK))
        gItlwmAssocNoPmk++;         // a PSK network with no key from either route

    XYLog("%s key cipher=%d len=%d source=%s\n", __FUNCTION__,
          ck->key_cipher_type, ck->key_len,
          key == (uint8_t *)ck->key ? "request" : (key ? "stash" : "none"));

    // Recorded because AssocKeyInReq came back 3 of 5 with AssocNoPmk at 0, which means two
    // requests carried no key *and* an auth type this driver does not treat as needing one. Both
    // the first and the most recent are kept: they differ, and only the first belongs to the same
    // join as the sticky failure snapshot.
    gItlwmAssocUpperAuthLast = cand->upper_auth_type;
    gItlwmAssocKeyInfoLast = ((uint32_t)ck->key_cipher_type << 16) | (uint16_t)ck->key_len;
    if (gItlwmAssocCalls == 1) {
        gItlwmAssocUpperAuthFirst = cand->upper_auth_type;
        gItlwmAssocKeyInfoFirst = gItlwmAssocKeyInfoLast;
    }

    // candidates carries no lower auth type; the WCL logs `lowerAuth = AUTHTYPE_OPEN` for every
    // join, and associateSSID only uses the lower type to reject shared-key WEP and to spot the
    // open/WEP case, neither of which the WCL reaches this way.
    that->associateSSID(cand->ssid, ssid_len, bssid,
                        APPLE80211_AUTHTYPE_OPEN, cand->upper_auth_type,
                        key, key_len, key_index);

    // Only now is there a desired ESS for checkJoinProgress to watch. Every return above this line
    // leaves net80211 unprogrammed, and arming the watch before them reports a failure that never
    // happened. See armJoinEssWatch().
    that->instance->armJoinEssWatch();

    if (ic->ic_state == IEEE80211_S_RUN) {
        // Associated elsewhere — leave first, then rescan. newstate(SCAN) from RUN frees the node
        // cache and begins a fresh scan, which ends in ieee80211_end_scan selecting the BSS the
        // desired ESSID and BSSID just pinned.
        if (ic->ic_bss != NULL)
            IEEE80211_SEND_MGMT(ic, ic->ic_bss, IEEE80211_FC0_SUBTYPE_DEAUTH,
                                IEEE80211_REASON_AUTH_LEAVE);
        ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
    } else if (ic->ic_state == IEEE80211_S_SCAN) {
        // Already scanning, which is where an unassociated net80211 sits permanently. Nudge the
        // state machine rather than calling ieee80211_end_scan() directly.
        //
        // **ieee80211_end_scan is the HAL's to call, not this layer's.** Its only legitimate
        // caller here is ItlIwx::iwx_endscan, driven by the firmware's IWX_SCAN_COMPLETE_UMAC
        // notification, and it clears IWX_FLAG_SCANNING *before* calling it. Calling end_scan
        // from here left that flag set, so net80211 walked SCAN -> AUTH -> ASSOC while the
        // firmware was still sweeping channels: nothing on the SCAN -> AUTH path aborts a running
        // scan (iwx_newstate_task's teardown block only runs for nstate <= ostate, and iwx_auth
        // goes straight to the PHY/MAC context), so the auth and assoc exchanges went out with
        // the radio off-channel. Measured: ItlwmJoinMaxState 3 with ItlwmJoinAssocDone 0 — the
        // BSS was selected and the exchange never completed.
        //
        // A SCAN -> SCAN transition is the HAL-aware equivalent and converges either way:
        // iwx_newstate_task returns early if a firmware scan is already in flight (that scan then
        // ends in iwx_endscan and selects the BSS the desired ESSID now pins), and otherwise
        // starts a fresh one with the same ending. Either path reaches ieee80211_end_scan through
        // the HAL, with IWX_FLAG_SCANNING correctly cleared first.
        ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
    }
    return kIOReturnSuccess;
}

// Ioctl 425, and the counterpart to setWCL_ASSOCIATE: Tahoe dropped setDISASSOCIATE from
// IO80211InfraProtocol and routes every disconnect through the WCL instead. Leaving it stubbed did
// not fail loudly — it desynchronised the two halves of the driver. `WCLNetManager` would run
// LINK_UP -> DEAUTH -> LINK_DOWN and consider the network abandoned while net80211 stayed in RUN,
// so `ifconfig` still reported `status: active` on an interface that could not pass a packet.
//
// The body is what the pre-Tahoe setDISASSOCIATE does, which is the same job: deauthenticate the
// current BSS, drop the desired ESS so net80211 does not immediately re-select it, and walk the
// state machine back to SCAN through ieee80211_new_state — never ieee80211_end_scan, which belongs
// to the HAL.
//
// Gated on the interface work queue for the same reason as beginAssociateGated: this mutates
// net80211 state and sends a management frame, and the HAL's work loop must not be doing either at
// the same time.
IOReturn AirportItlwmSkywalkInterface::
beginLeaveNetworkGated(OSObject *target, void *arg0, void *, void *, void *)
{
    AirportItlwmSkywalkInterface *that = OSDynamicCast(AirportItlwmSkywalkInterface, target);
    struct apple80211_leave_network *ln = (struct apple80211_leave_network *)arg0;
    struct ieee80211com *ic = that->fHalService->get80211Controller();

    if (ic->ic_state < IEEE80211_S_SCAN)
        return kIOReturnSuccess;
    if (ic->ic_state > IEEE80211_S_AUTH && ic->ic_bss != NULL)
        IEEE80211_SEND_MGMT(ic, ic->ic_bss, IEEE80211_FC0_SUBTYPE_DEAUTH,
                            IEEE80211_REASON_AUTH_LEAVE);
    // Mid-exchange there is no association to tear down and no ESS worth clearing; the exchange
    // will fail on its own. Same early-out as the pre-Tahoe path.
    if (ic->ic_state == IEEE80211_S_ASSOC || ic->ic_state == IEEE80211_S_AUTH)
        return kIOReturnSuccess;

    that->disassocIsVoluntary = true;
    ieee80211_del_ess(ic, nullptr, 0, 1);
    ieee80211_deselect_ess(ic);
    ic->ic_assoc_status = APPLE80211_STATUS_UNAVAILABLE;
    // The WCL's own reason, so a later getDEAUTH reports why *it* left rather than a constant.
    ic->ic_deauth_reason = (ln != NULL && ln->reason != 0) ? ln->reason
                                                          : APPLE80211_REASON_ASSOC_LEAVING;
    ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
setWCL_LEAVE_NETWORK(struct apple80211_leave_network *ln)
{
    gItlwmLeaveNetCalls++;
    IO80211WorkQueue *wq = getWorkQueue();
    return (wq != NULL) ? wq->runAction(beginLeaveNetworkGated, this, ln)
                        : beginLeaveNetworkGated(this, ln, NULL, NULL, NULL);
}

IOReturn AirportItlwmSkywalkInterface::
setWCL_ASSOCIATE(struct apple80211_assoc_candidates *cand)
{
    gItlwmAssocCalls++;
    if (cand == NULL)
        return kIOReturnError;
    IO80211WorkQueue *wq = getWorkQueue();
    IOReturn ret = (wq != NULL) ? wq->runAction(beginAssociateGated, this, cand)
                                : beginAssociateGated(this, cand, NULL, NULL, NULL);
    if (ret == kIOReturnSuccess)
        gItlwmAssocStarted++;
    else
        gItlwmAssocRefused++;
    return ret;
}
#endif

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
setSCAN_REQ(struct apple80211_scan_data *sd)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
#if 0
    XYLog("%s Type: %u BSS Type: %u PHY Mode: %u Dwell time: %u Rest time: %u Num channels: %u SSID: %s BSSID: %s\n",
          __FUNCTION__,
          sd->scan_type,
          sd->bss_type,
          sd->phy_mode,
          sd->dwell_time,
          sd->rest_time,
          sd->num_channels,
          sd->ssid,
          ether_sprintf(sd->bssid.octet));
#endif
    if (fScanResultWrapping)
        return 22;
    if (ic->ic_state <= IEEE80211_S_INIT)
        return 22;
    if (sd->scan_type == APPLE80211_SCAN_TYPE_FAST || sd->scan_type == APPLE80211_SCAN_TYPE_PASSIVE) {
        if (scanSource) {
            scanSource->setTimeoutMS(100);
            scanSource->enable();
        }
        return kIOReturnSuccess;
    }
    ieee80211_begin_cache_bgscan(&ic->ic_ac.ac_if);
    if (scanSource) {
        scanSource->setTimeoutMS(100);
        scanSource->enable();
    }
    return kIOReturnSuccess;
}
#endif

extern OSDictionary *convertScanToDictionary(apple80211_scan_result *a1);

static int convertNodeToScanResult(ItlHalService *fHalService, struct ieee80211_node *fNextNodeToSend, apple80211_scan_result *result)
{
    bzero(result, sizeof(*result));
    result->version = APPLE80211_VERSION;
    if (fNextNodeToSend->ni_rsnie_tlv && fNextNodeToSend->ni_rsnie_tlv_len > 0) {
        result->asr_ie_len = fNextNodeToSend->ni_rsnie_tlv_len;
        memcpy(result->asr_ie_data, fNextNodeToSend->ni_rsnie_tlv, MIN(result->asr_ie_len, sizeof(result->asr_ie_data)));
    } else {
        result->asr_ie_len = 0;
    }
    result->asr_beacon_int = fNextNodeToSend->ni_intval;
    for (int i = 0; i < result->asr_nrates; i++ )
        result->asr_rates[i] = fNextNodeToSend->ni_rates.rs_rates[i];
    result->asr_nrates = fNextNodeToSend->ni_rates.rs_nrates;
    result->asr_age = (uint32_t)(airport_up_time() - fNextNodeToSend->ni_age_ts);
    result->asr_cap = fNextNodeToSend->ni_capinfo;
    result->asr_channel.version = APPLE80211_VERSION;
    result->asr_channel.channel = ieee80211_chan2ieee(fHalService->get80211Controller(), fNextNodeToSend->ni_chan);
    result->asr_channel.flags = ieeeChanFlag2appleScanFlagVentura(fNextNodeToSend->ni_chan->ic_flags);
    result->asr_noise = -fHalService->getDriverInfo()->getBSSNoise();
    result->asr_rssi = -(0 - IWM_MIN_DBM - fNextNodeToSend->ni_rssi);
    memcpy(result->asr_bssid, fNextNodeToSend->ni_bssid, IEEE80211_ADDR_LEN);
    result->asr_ssid_len = fNextNodeToSend->ni_esslen;
    if (result->asr_ssid_len != 0)
        memcpy(&result->asr_ssid, fNextNodeToSend->ni_essid, result->asr_ssid_len);
    return 0;
}

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
getCURRENT_NETWORK(apple80211_scan_result *sr)
{
    if (fHalService->get80211Controller()->ic_state != IEEE80211_S_RUN || fHalService->get80211Controller()->ic_bss == NULL)
        return kIOReturnError;
    convertNodeToScanResult(fHalService, fHalService->get80211Controller()->ic_bss, sr);
    return kIOReturnSuccess;
}
#endif

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
getCOLOCATED_NETWORK_SCOPE_ID(apple80211_colocated_network_scope_id *as)
{
    if (!as)
        return kIOReturnBadArgument;
    as->version = APPLE80211_VERSION;
    return kIOReturnSuccess;
}
#endif

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwmSkywalkInterface::
getSCAN_RESULT(struct apple80211_scan_result *sr)
{
    if (fNextNodeToSend == NULL) {
        if (fScanResultWrapping) {
            fScanResultWrapping = false;
            return 5;
        } else {
            fNextNodeToSend = RB_MIN(ieee80211_tree, &fHalService->get80211Controller()->ic_tree);
            if (fNextNodeToSend == NULL) {
                return 5;
            }
        }
    }
//    XYLog("%s ni_bssid=%s ni_essid=%s channel=%d flags=%d asr_cap=%d asr_nrates=%d asr_ssid_len=%d asr_ie_len=%d asr_rssi=%d\n", __FUNCTION__, ether_sprintf(fNextNodeToSend->ni_bssid), fNextNodeToSend->ni_essid, ieee80211_chan2ieee(ic, fNextNodeToSend->ni_chan), ieeeChanFlag2apple(fNextNodeToSend->ni_chan->ic_flags, -1), fNextNodeToSend->ni_capinfo, fNextNodeToSend->ni_rates.rs_nrates, fNextNodeToSend->ni_esslen, fNextNodeToSend->ni_rsnie_tlv == NULL ? 0 : fNextNodeToSend->ni_rsnie_tlv_len, fNextNodeToSend->ni_rssi);
    convertNodeToScanResult(fHalService, fNextNodeToSend, sr);
    
    fNextNodeToSend = RB_NEXT(ieee80211_tree, &HalService->get80211Controller()->ic_tree, fNextNodeToSend);
    if (fNextNodeToSend == NULL)
        fScanResultWrapping = true;

    return kIOReturnSuccess;
}
#endif
