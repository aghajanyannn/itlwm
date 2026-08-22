//
//  AirportItlwmV2.cpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#include "AirportItlwmV2.hpp"
#include <sys/_netstat.h>
#include <crypto/sha1.h>
#include <net80211/ieee80211_priv.h>
#include <net80211/ieee80211_var.h>

#include "AirportItlwmSkywalkInterface.hpp"
#include "IOPCIEDeviceWrapper.hpp"
#if __IO80211_TARGET >= __MAC_26_0
#include "ItlIwx.hpp"      // Tahoe bring-up: read iwx_preinit outcome out of the softc
#endif

#define super IO80211Controller
OSDefineMetaClassAndStructors(AirportItlwm, IO80211Controller);
OSDefineMetaClassAndStructors(CTimeout, OSObject)

IO80211WorkQueue *_fWorkloop;
IOCommandGate *_fCommandGate;


#if __IO80211_TARGET >= __MAC_26_0
// Tahoe consumed IONetworkController's reserved vtable slots 6 and 7 for real methods
// (allocatePacketNoWait(unsigned int) and setHardwareAssists(unsigned int, unsigned int)),
// but MacKernelSDK still declares both as OSMetaClassDeclareReservedUnused. The kernel
// therefore no longer exports _RESERVEDIONetworkController6/7, and our vtable copy linked
// those two slots as null pointers — an immediate fault if anything ever dispatched
// through them.
//
// Defining them here makes the slots point at inert code instead of at address zero. It
// does not restore Apple's behaviour: a caller reaching slot 6 gets a failed allocation
// rather than a packet, and slot 7 silently drops the hardware-assist flags. Neither is
// on a path this driver uses. The real fix belongs in MacKernelSDK's header, which would
// let the slots carry the correct signatures and bind to the kernel's implementations.
void IONetworkController::_RESERVEDIONetworkController6()
{
    // Slot 6 is allocatePacketNoWait(), which returns a pointer. The declared return type
    // here is void, so zero the return register explicitly — otherwise a caller would read
    // whatever happened to be in rax and treat it as an mbuf. x86_64-only, as is the build.
    asm volatile ("xorl %%eax, %%eax" ::: "eax");
}

void IONetworkController::_RESERVEDIONetworkController7()
{
}

// Set by the iwx HAL; see the marker note at the top of ItlIwx.cpp for the encodings.
extern "C" {
extern int gItlwmPreinitMark;
extern int gItlwmPreinitErr;
extern int gItlwmInitMark;
extern int gItlwmInitErr;
extern uint32_t gItlwmPreSleepInitComplete;
extern uint32_t gItlwmSnapInitComplete;
extern uint32_t gItlwmSnapUcOk;
extern uint32_t gItlwmSnapUcIntr;
extern uint32_t gItlwmSnapCmdCur;
extern uint32_t gItlwmSnapCmdQueued;
extern uint32_t gItlwmSnapGeneration;
extern uint32_t gItlwmSnapSkuId0;
extern uint32_t gItlwmSnapSkuId1;
extern uint32_t gItlwmSnapSkuId2;
extern uint32_t gItlwmCmdQid;
extern uint32_t gItlwmCmdDoorbell;
extern uint32_t gItlwmCmdDoneCount;
extern uint32_t gItlwmCmdDoneLast;
extern uint32_t gItlwmNotifIntrCount;
extern uint32_t gItlwmIsrCount;
extern uint32_t gItlwmIsrAtKick;
extern uint32_t gItlwmNotifAtKick;
extern uint32_t gItlwmIctZeroCount;
extern uint32_t gItlwmIctZeroCur;
extern uint32_t gItlwmIctZeroInt;
extern uint32_t gItlwmIctZeroFh;
extern uint32_t gItlwmIctZeroIntAcc;
extern uint32_t gItlwmIctZeroFhAcc;
extern uint32_t gItlwmIctResetCount;
extern uint32_t gItlwmIctResetAtIsr;
extern uint32_t gItlwmIctResetAtKick;
extern uint32_t gItlwmIctPaddrLo;
extern uint32_t gItlwmIctTblReg;
}
extern "C" {
extern uint32_t gItlwmPrepareBSDUngated;
extern uint32_t gItlwmPostMsgQueued;
extern uint32_t gItlwmPostMsgSent;
extern uint32_t gItlwmPostMsgDropped;
extern uint32_t gItlwmScanReqCalls;
extern uint32_t gItlwmScanReqStarted;
extern uint32_t gItlwmScanReqRefused;
extern uint32_t gItlwmScanBeacons;
extern uint32_t gItlwmAssocCalls;
extern uint32_t gItlwmAssocStarted;
extern uint32_t gItlwmAssocRefused;
extern uint32_t gItlwmAssocKeyInReq;
extern uint32_t gItlwmAssocNoPmk;
extern uint32_t gItlwmAssocUpperAuthFirst;
extern uint32_t gItlwmAssocUpperAuthLast;
extern uint32_t gItlwmAssocKeyInfoFirst;
extern uint32_t gItlwmAssocKeyInfoLast;
extern uint32_t gItlwmMgtqKicks;
extern uint32_t gItlwmJoinAssocDone;
extern uint32_t gItlwmJoinConnectDone;
extern uint32_t gItlwmJoinTimeouts;
extern uint32_t gItlwmLinkIndUp;
extern uint32_t gItlwmLinkIndDown;
extern uint32_t gItlwmLinkDownState;
extern uint32_t gItlwmLinkDownPair;
extern uint32_t gItlwmDisableCalls;
extern uint32_t gItlwmCCPipesStarted;
extern uint32_t gItlwmCCPipeStartFail;
extern uint32_t gItlwmLeaveNetCalls;
extern uint32_t gItlwmLqmPosts;
extern uint32_t gItlwmLqmBeaconStall;
extern uint32_t gItlwmAssocEsslen;
extern uint32_t gItlwmIcSizeHal;
extern uint32_t gItlwmIcSizeNet;
extern uint32_t gItlwmLastStatePair;
extern uint32_t gItlwmBssInfoCalls;
extern uint32_t gItlwmBssInfoEmpty;
extern uint32_t gItlwmExtBssInfoCalls;
// Mechanism 1 (real Skywalk registration). Stage is set once during start(), so it
// is deliberately part of the change guard below: without that, a boot where nothing else moved
// would publish nothing and the stage would read as absent rather than as its real value.
extern uint32_t gItlwmSkywalkStage;
extern uint32_t gItlwmSkywalkRegRet;
extern uint32_t gItlwmSkywalkTxDequeue;
extern uint32_t gItlwmSkywalkTxComplete;
extern uint32_t gItlwmSkywalkRxDequeue;
extern uint32_t gItlwmSkywalkRxComplete;
extern uint32_t gItlwmSkywalkTxFrames;
extern uint32_t gItlwmSkywalkTxDrops;
extern uint32_t gItlwmSkywalkTxNoMbuf;
extern uint32_t gItlwmSkywalkTxComplFail;
extern uint32_t gItlwmSkywalkTxListShort;
extern uint32_t gItlwmSkywalkRxFrames;
extern uint32_t gItlwmSkywalkRxDrops;
extern uint32_t gItlwmSkywalkRxNoBuf;
extern uint32_t gItlwmSkywalkRxOversize;
extern uint32_t gItlwmSkywalkRxComplFail;
extern uint32_t gItlwmSkywalkRxFree;
extern uint32_t gItlwmSkywalkRxListShort;
extern uint32_t gItlwmSkywalkBsdUnit;
extern uint32_t gItlwmSkywalkRxFallbackDrops;
extern uint32_t gItlwmSkywalkQueuesAdded;
extern uint32_t gItlwmSkywalkQueuesEnabled;
extern uint32_t gItlwmSkywalkRxPrimeCalls;
extern uint32_t gItlwmSkywalkRxPrimeRet;
extern uint32_t gItlwmSkyLinkReportCalls;
extern uint32_t gItlwmSkyLinkReportRet;
extern uint32_t gItlwmLlAddrCalls;
extern uint32_t gItlwmLlAddrSynced;
extern uint32_t gItlwmLlAddrLate;
extern uint64_t gItlwmLlAddrLast;
extern uint32_t gItlwmLastRxActivityCalls;
extern uint32_t gItlwmDataPathPeerStatsCalls;
extern uint32_t gItlwmLastQueueTimeCalls;
}


// Publish the outcome of iwx_preinit, read straight out of the softc AFTER attach() has
// returned. This deliberately does NOT instrument the HAL: an earlier attempt to record
// which tsleep expired, by storing to globals at each wait site, correlated with a hung
// boot every time it was enabled — with the same binary booting fine when the writes were
// gated off, so it was execution, not layout. No mechanism was ever found. Reading the
// state afterwards, once, on our own start() thread, answers the same question with no
// writes on any HAL path.
//
// mark 6 with errno 35 already proves the firmware came alive (a dead firmware returns
// EINVAL 22), so these two bits split the remaining cases:
//
//   ItlwmUcOk           sc_uc.uc_ok        1 = the IWX_ALIVE notification was processed
//   ItlwmInitComplete   sc_init_complete   1 = IWX_INIT_COMPLETE_NOTIF was processed
//
//   UcOk=1, InitComplete=1 -> the notification DID arrive and the sleeper still timed out:
//                             a lost wakeup (wakeupOn() does not hold the lock tsleep_nsec
//                             sleeps on, and the OpenBSD predicate loops are commented out)
//   UcOk=1, InitComplete=0 -> the notification genuinely never arrived; the failing wait is
//                             iwx_load_pnvm, iwx_send_cmd, or iwx_run_init_mvm_ucode
//   UcOk=0                 -> firmware never came alive (inconsistent with errno 35)
static void publishPreinitMark(IOService *provider, ItlHalService *hal)
{
    if (!provider)
        return;
    provider->setProperty("ItlwmPreinitMark", (UInt64)gItlwmPreinitMark, 32);
    provider->setProperty("ItlwmPreinitErr", (UInt64)gItlwmPreinitErr, 32);

    // Which exit of iwx_run_init_mvm_ucode was taken, and — the decisive one —
    // sc_init_complete as it stood immediately before the INIT_COMPLETE wait. See the
    // marker block in ItlIwx.cpp for the numbering.
    //   ItlwmInitMark 5 + ItlwmPreSleepInitComplete != 0  -> lost wakeup; restore the loop
    //   ItlwmInitMark 5 + ItlwmPreSleepInitComplete == 0  -> the notification never came
    provider->setProperty("ItlwmInitMark", (UInt64)gItlwmInitMark, 32);
    provider->setProperty("ItlwmInitErr", (UInt64)gItlwmInitErr, 32);
    provider->setProperty("ItlwmPreSleepInitComplete",
                          (UInt64)gItlwmPreSleepInitComplete, 32);

    // Command path. With ItlwmInitMark 3 (first host command timed out) these say where it
    // broke — see the marker block in ItlIwx.cpp for how to read them together.
    provider->setProperty("ItlwmIsrCount", (UInt64)gItlwmIsrCount, 32);
    provider->setProperty("ItlwmNotifIntrCount", (UInt64)gItlwmNotifIntrCount, 32);
    provider->setProperty("ItlwmCmdDoneCount", (UInt64)gItlwmCmdDoneCount, 32);
    provider->setProperty("ItlwmCmdDoneLast", (UInt64)gItlwmCmdDoneLast, 32);
    provider->setProperty("ItlwmCmdQid", (UInt64)gItlwmCmdQid, 32);
    provider->setProperty("ItlwmCmdDoorbell", (UInt64)gItlwmCmdDoorbell, 32);
    // Latched at the doorbell. The DIFFERENCE from the totals above is the answer; the
    // totals themselves are dominated by firmware-load DMA interrupts.
    provider->setProperty("ItlwmIsrAtKick", (UInt64)gItlwmIsrAtKick, 32);
    provider->setProperty("ItlwmNotifAtKick", (UInt64)gItlwmNotifAtKick, 32);

    // ICT. Measured 203 zero reads out of 203 ICT-mode interrupts with ict_cur pinned at 0,
    // so iwx_post_alive no longer enables ICT at all. These now mainly confirm that:
    //   IctResetCount == 0, IctZeroCount == 0 -> ICT is off; iwx_intr is on the CSR_INT path
    //   IctPaddrLo & 0xfff != 0               -> why the table was dead: CSR_DRAM_INT_TBL_REG
    //                                            takes paddr >> 12 and truncated a misaligned
    //                                            IOVA to a different page
    //   IctTblReg & 0x80000000                -> ICT still enabled in hardware; causes would
    //                                            go to DRAM and IWX_CSR_INT would read 0
    // IctZeroInt / IctZeroFh do NOT distinguish a desynced ICT from a synced one: with ICT
    // enabled the hardware routes causes to the table instead of IWX_CSR_INT, so zero there
    // is expected either way. The count and IctZeroCur are what carried the finding.
    provider->setProperty("ItlwmIctZeroCount", (UInt64)gItlwmIctZeroCount, 32);
    provider->setProperty("ItlwmIctZeroCur", (UInt64)gItlwmIctZeroCur, 32);
    provider->setProperty("ItlwmIctZeroInt", (UInt64)gItlwmIctZeroInt, 32);
    provider->setProperty("ItlwmIctZeroFh", (UInt64)gItlwmIctZeroFh, 32);
    provider->setProperty("ItlwmIctZeroIntAcc", (UInt64)gItlwmIctZeroIntAcc, 32);
    provider->setProperty("ItlwmIctZeroFhAcc", (UInt64)gItlwmIctZeroFhAcc, 32);
    provider->setProperty("ItlwmIctResetCount", (UInt64)gItlwmIctResetCount, 32);
    provider->setProperty("ItlwmIctResetAtIsr", (UInt64)gItlwmIctResetAtIsr, 32);
    provider->setProperty("ItlwmIctResetAtKick", (UInt64)gItlwmIctResetAtKick, 32);
    provider->setProperty("ItlwmIctPaddrLo", (UInt64)gItlwmIctPaddrLo, 32);
    provider->setProperty("ItlwmIctTblReg", (UInt64)gItlwmIctTblReg, 32);


    ItlIwx *iwx = OSDynamicCast(ItlIwx, hal);
    if (iwx) {
        // All from ITLWM_PREINIT_SNAP, sampled before iwx_stop_device. Reading the softc
        // here instead would report zeros: stop_device resets every tx ring.
        provider->setProperty("ItlwmUcOk", (UInt64)gItlwmSnapUcOk, 32);
        provider->setProperty("ItlwmUcIntr", (UInt64)gItlwmSnapUcIntr, 32);
        provider->setProperty("ItlwmInitComplete", (UInt64)gItlwmSnapInitComplete, 32);
        provider->setProperty("ItlwmGeneration", (UInt64)gItlwmSnapGeneration, 32);
        // sku_id decides whether iwx_load_pnvm() runs at all (ItlIwx.cpp: "if the SKU_ID
        // is empty, there's nothing to do"). It is populated ONLY in the ALIVE-v5 branch,
        // which is the else of a bare payload-size test against sizeof(alive_resp_v4). If
        // an AX200 (device family 22000) reports a size that matches neither, sku_id is
        // filled from whatever lies at that offset, PNVM is kicked on a device that never
        // sends PNVM_INIT_COMPLETE_NTFY, and the 2 s wait expires — which is exactly the
        // observed mark 6 / errno 35 / InitComplete 0. Non-zero sku_id here confirms that.
        provider->setProperty("ItlwmSkuId0", (UInt64)gItlwmSnapSkuId0, 32);
        provider->setProperty("ItlwmSkuId1", (UInt64)gItlwmSnapSkuId1, 32);
        provider->setProperty("ItlwmSkuId2", (UInt64)gItlwmSnapSkuId2, 32);
        provider->setProperty("ItlwmDeviceFamily", (UInt64)iwx->com.sc_device_family, 32);
        // ItlwmInitMark now says directly which exit was taken, so these are corroboration
        // rather than the discriminator they were written to be. iwx_send_cmd does
        // ring->queued++ before sleeping and iwx_cmd_done does ring->queued-- on
        // completion, so a command that timed out leaves the counter standing — but only
        // if sampled before stop_device, which is what ITLWM_PREINIT_SNAP now guarantees.
        provider->setProperty("ItlwmCmdQueued", (UInt64)gItlwmSnapCmdQueued, 32);
        provider->setProperty("ItlwmCmdCur", (UInt64)gItlwmSnapCmdCur, 32);
    }
}
#endif


void AirportItlwm::releaseAll()
{
#if __IO80211_TARGET >= __MAC_26_0
    // Same contract as IOPCIEDeviceWrapper::fPublishCall, and the same trade for the same
    // reason. thread_call_cancel returns TRUE only when the call was pending and has now been
    // dequeued, so it can never run; it cannot distinguish "idle" from "executing right now".
    //
    // Never armed => it cannot be executing, so freeing is safe and the retain is ours to drop.
    // Ever armed => leak the allocation and keep the retain rather than free memory a live
    // callback is standing on. thread_call_cancel_wait is not an option; it requires a
    // THREAD_CALL_OPTIONS_ONCE call.
    if (fPostMsgCall != NULL) {
        thread_call_cancel(fPostMsgCall);
        if (!fPostMsgArmed) {
            thread_call_free(fPostMsgCall);
            fPostMsgCall = NULL;
            release();
        }
    }
    // Left allocated when the thread_call is leaked: the callback dereferences it.
    if (fPendingMsgLock != NULL && fPostMsgCall == NULL) {
        IOSimpleLockFree(fPendingMsgLock);
        fPendingMsgLock = NULL;
    }
#endif
    OSSafeReleaseNULL(driverLogPipe);
    OSSafeReleaseNULL(driverDataPathPipe);
    OSSafeReleaseNULL(driverSnapshotsPipe);
    // Top of the fault-reporter chain down, so nothing is freed while a live object above it
    // still holds a use of it.
    OSSafeReleaseNULL(driverFaultReporter);
#if __IO80211_TARGET >= __MAC_26_0
    OSSafeReleaseNULL(driverCCFaultReporter);
    OSSafeReleaseNULL(driverFaultStream);
    OSSafeReleaseNULL(driverFaultWorkLoop);
    OSSafeReleaseNULL(driverLogger);
#endif
    if (fHalService) {
        fHalService->release();
        fHalService = NULL;
    }
    if (_fWorkloop) {
        if (_fCommandGate) {
//            _fCommandGate->disable();
            _fWorkloop->removeEventSource(_fCommandGate);
            _fCommandGate->release();
            _fCommandGate = NULL;
        }
        if (scanSource) {
            scanSource->cancelTimeout();
            scanSource->disable();
            _fWorkloop->removeEventSource(scanSource);
            scanSource->release();
            scanSource = NULL;
        }
        if (fWatchdogWorkLoop && watchdogTimer) {
            watchdogTimer->cancelTimeout();
            fWatchdogWorkLoop->removeEventSource(watchdogTimer);
            watchdogTimer->release();
            watchdogTimer = NULL;
            fWatchdogWorkLoop->release();
            fWatchdogWorkLoop = NULL;
        }
        _fWorkloop->release();
        _fWorkloop = NULL;
    }
    unregistPM();
}

void AirportItlwm::
eventHandler(struct ieee80211com *ic, int msgCode, void *data)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, ic->ic_ac.ac_if.controller);
    IO80211SkywalkInterface *interface = that->fNetIf;
    if (!interface)
        return;
    // Reached from the HAL inside iwx_intr, on _fWorkloop's own thread — see postMessageSafe().
#if __IO80211_TARGET >= __MAC_26_0
#define ITLWM_POST(msg)  that->postMessageSafe((msg), NULL, 0)
#else
#define ITLWM_POST(msg)  interface->postMessage((msg), NULL, 0, ITLWM_POSTMSG_ASYNC)
#endif
    switch (msgCode) {
        case IEEE80211_EVT_COUNTRY_CODE_UPDATE:
            ITLWM_POST(APPLE80211_M_COUNTRY_CODE_CHANGED);
            break;
        case IEEE80211_EVT_STA_ASSOC_DONE:
            ITLWM_POST(APPLE80211_M_ASSOC_DONE);
#if __IO80211_TARGET >= __MAC_26_0
            // APPLE80211_M_ASSOC_DONE is the userspace audience only. The WCL's JOIN_MANAGER
            // waits on message 211, and posting one without the other is the mistake the scan
            // path made with 10 vs 237 — the notification looked delivered and the FSM never
            // moved. net80211 raises this event only for a success status.
            that->postJoinAssocComplete(0, 0);
#endif
            break;
        case IEEE80211_EVT_STA_DEAUTH:
            ITLWM_POST(APPLE80211_M_DEAUTH_RECEIVED);
#if __IO80211_TARGET >= __MAC_26_0
            // A deauth during a join is that join failing. Reporting it as a failed *connect*
            // rather than a failed association is deliberate: a non-zero status in message 211
            // raises no FSM event at all (handleJoinAssocComplete only special-cases 1000, and
            // otherwise just logs), whereas a non-zero status in 213 reaches
            // updateAndCheckForNextCandidate and moves the FSM on to the next candidate. The
            // transition table accepts JOIN_CONNECT_COMPLETE in IN_PROGRESS as well as in
            // ASSOC_DONE, so this works whether or not the association itself got that far.
            that->postJoinConnectComplete(IEEE80211_STATUS_UNSPECIFIED, ic->ic_deauth_reason);
#endif
            break;
#if __IO80211_TARGET >= __MAC_26_0
        case IEEE80211_EVT_SCAN_BEACON:
            that->postScanBeacon((const struct ieee80211_beacon_event *)data);
            break;
#endif
        default:
            break;
    }
#undef ITLWM_POST
}

#if __IO80211_TARGET >= __MAC_26_0
// publishPreinitMark runs *only* when fHalService->attach() fails, so anything published only
// from there is invisible on exactly the boots where the driver works. Republish from the
// watchdog, which already ticks every kWatchDogTimerPeriod once the adapter is enabled.
//
// On `this`, not on the provider: getProvider() is one of the inherited slots the kext loader
// can mis-bind, and this kext also carries AirportItlwmEthernetInterface::getProvider. Read with
//     ioreg -r -c AirportItlwm -l -w0 | grep -i itlwm
// Change-guarded so a healthy system is not rewriting IORegistry properties once a second
// forever. File-scope statics, POD and zero-initialised, so no __cxa_guard is emitted.
static uint32_t sLastPostQueued, sLastPostSent, sLastPostDropped;
static uint32_t sLastScanCalls, sLastScanStarted, sLastScanRefused, sLastScanBeacons;
static uint32_t sLastAssocCalls, sLastAssocStarted, sLastAssocRefused, sLastAssocNoPmk, sLastAssocKeyInReq;
static uint32_t sLastJoinAssocDone, sLastJoinConnectDone, sLastJoinTimeouts, sLastJoinMaxState;
static uint32_t sLastLinkIndUp, sLastLinkIndDown, sLastBssInfoCalls, sLastBssInfoEmpty;
static uint32_t sLastExtBssInfoCalls, sLastLinkDownState, sLastLinkDownPair, sLastDisableCalls;
static uint32_t sLastAssocStatusSeen, sLastJoinTicksAssoc, sLastUpperAuth, sLastJoinMgtqMax, sLastMgtqKicks;
static bool sLastJoinFailCaptured;
static uint32_t sLastRxActivityCalls, sLastPeerStatsCalls, sLastQueueTimeCalls;
static uint32_t sLastSkywalkStage, sLastSkywalkTxDequeue, sLastSkywalkRxDequeue;
// ItlwmLqmPosts advances for the whole life of every connection, so guarding on its exact value
// would republish every interval forever and defeat the guard. Guard on a coarse bucket instead:
// ioreg stays current to within kLqmPostTicks * 64 seconds, which is enough to answer the only
// question asked of it — is it *still* posting — without a write per interval.
static uint32_t sLastLqmPostBucket, sLastLqmBeaconStall;
static uint32_t sLastScanFailDes, sLastScanFailOr, sLastEssClears, sLastAssocEsslen;
static bool sPublishedOnce;

void AirportItlwm::publishRuntimeCounters()
{
    if (!sPublishedOnce) {
        sPublishedOnce = true;
        // The HAL bring-up markers. These used to be published ONLY from publishPreinitMark,
        // which runs only inside the `if (!fHalService->attach(pciNub))` failure branch — so on
        // every boot where the driver works they were never published at all, and two mechanisms
        // whose "Done when" is "read this on one surviving boot" could not be closed even in
        // principle. Root AGENTS.md mechanisms 4 (ItlwmIctPaddrLo / ItlwmIctTblReg) and 8
        // (ItlwmInitMark / ItlwmPreSleepInitComplete). Same defect the SkyIf counters had, and
        // the same fix: republish here, where the boot survives.
        //
        // On `this`, not the provider, so both nodes are readable and comparable:
        //     ioreg -r -c AirportItlwm -l -w0 | grep -i itlwm
        publishPreinitMark(this, fHalService);
        // struct ieee80211com's size as the HAL and as net80211 each see it. These must be equal.
        // They differ only if one object file was built against a stale ieee80211_var.h, and then
        // every ic_* offset in that object is wrong — the HAL writes its softc over the tail of
        // ieee80211com, whose last member is the ic_ess list head, and the next
        // ieee80211_switch_ess general-protection-faults on a pointer made of instruction bytes.
        // Reads as a net80211 bug and is not one. Check these two first after any change to
        // ieee80211com; a clean build is the fix.
        setProperty("ItlwmIcSizeHal", (UInt64)gItlwmIcSizeHal, 32);
        setProperty("ItlwmIcSizeNet", (UInt64)gItlwmIcSizeNet, 32);
        setProperty("ItlwmPrepareBSDUngated", (UInt64)gItlwmPrepareBSDUngated, 32);
    }
    if (gItlwmPostMsgQueued == sLastPostQueued &&
        gItlwmPostMsgSent == sLastPostSent &&
        gItlwmPostMsgDropped == sLastPostDropped &&
        gItlwmScanReqCalls == sLastScanCalls &&
        gItlwmScanReqStarted == sLastScanStarted &&
        gItlwmScanReqRefused == sLastScanRefused &&
        gItlwmScanBeacons == sLastScanBeacons &&
        gItlwmAssocCalls == sLastAssocCalls &&
        gItlwmAssocStarted == sLastAssocStarted &&
        gItlwmAssocRefused == sLastAssocRefused &&
        gItlwmAssocNoPmk == sLastAssocNoPmk &&
        gItlwmAssocKeyInReq == sLastAssocKeyInReq &&
        fJoinMaxState == sLastJoinMaxState &&
        fJoinFailCaptured == sLastJoinFailCaptured &&
        fJoinAssocStatusSeen == sLastAssocStatusSeen &&
        fJoinTicksAssoc == sLastJoinTicksAssoc &&
        fJoinMgtqMax == sLastJoinMgtqMax &&
        gItlwmMgtqKicks == sLastMgtqKicks &&
        gItlwmAssocUpperAuthLast == sLastUpperAuth &&
        gItlwmJoinAssocDone == sLastJoinAssocDone &&
        gItlwmJoinConnectDone == sLastJoinConnectDone &&
        gItlwmJoinTimeouts == sLastJoinTimeouts &&
        gItlwmLinkIndUp == sLastLinkIndUp &&
        gItlwmLinkIndDown == sLastLinkIndDown &&
        gItlwmLinkDownState == sLastLinkDownState &&
        gItlwmLinkDownPair == sLastLinkDownPair &&
        gItlwmDisableCalls == sLastDisableCalls &&
        gItlwmBssInfoCalls == sLastBssInfoCalls &&
        gItlwmBssInfoEmpty == sLastBssInfoEmpty &&
        gItlwmExtBssInfoCalls == sLastExtBssInfoCalls &&
        gItlwmLastRxActivityCalls == sLastRxActivityCalls &&
        gItlwmDataPathPeerStatsCalls == sLastPeerStatsCalls &&
        (gItlwmLqmPosts >> 6) == sLastLqmPostBucket &&
        gItlwmLqmBeaconStall == sLastLqmBeaconStall &&
        fHalService->get80211Controller()->ic_scan_fail_des == sLastScanFailDes &&
        fHalService->get80211Controller()->ic_scan_fail_or == sLastScanFailOr &&
        fHalService->get80211Controller()->ic_ess_clears == sLastEssClears &&
        gItlwmAssocEsslen == sLastAssocEsslen &&
        gItlwmSkywalkStage == sLastSkywalkStage &&
        gItlwmSkywalkTxDequeue == sLastSkywalkTxDequeue &&
        gItlwmSkywalkRxDequeue == sLastSkywalkRxDequeue &&
        gItlwmLastQueueTimeCalls == sLastQueueTimeCalls)
        return;
    sLastSkywalkStage = gItlwmSkywalkStage;
    sLastSkywalkTxDequeue = gItlwmSkywalkTxDequeue;
    sLastSkywalkRxDequeue = gItlwmSkywalkRxDequeue;
    sLastEssClears = fHalService->get80211Controller()->ic_ess_clears;
    sLastAssocEsslen = gItlwmAssocEsslen;
    sLastLqmPostBucket = gItlwmLqmPosts >> 6;
    sLastLqmBeaconStall = gItlwmLqmBeaconStall;
    sLastScanFailDes = fHalService->get80211Controller()->ic_scan_fail_des;
    sLastScanFailOr = fHalService->get80211Controller()->ic_scan_fail_or;
    sLastScanBeacons = gItlwmScanBeacons;
    sLastAssocCalls = gItlwmAssocCalls;
    sLastAssocStarted = gItlwmAssocStarted;
    sLastAssocRefused = gItlwmAssocRefused;
    sLastAssocNoPmk = gItlwmAssocNoPmk;
    sLastAssocKeyInReq = gItlwmAssocKeyInReq;
    sLastJoinMaxState = fJoinMaxState;
    sLastJoinFailCaptured = fJoinFailCaptured;
    sLastAssocStatusSeen = fJoinAssocStatusSeen;
    sLastJoinTicksAssoc = fJoinTicksAssoc;
    sLastJoinMgtqMax = fJoinMgtqMax;
    sLastMgtqKicks = gItlwmMgtqKicks;
    sLastUpperAuth = gItlwmAssocUpperAuthLast;
    sLastJoinAssocDone = gItlwmJoinAssocDone;
    sLastJoinConnectDone = gItlwmJoinConnectDone;
    sLastJoinTimeouts = gItlwmJoinTimeouts;
    sLastLinkIndUp = gItlwmLinkIndUp;
    sLastLinkIndDown = gItlwmLinkIndDown;
    sLastLinkDownState = gItlwmLinkDownState;
    sLastLinkDownPair = gItlwmLinkDownPair;
    sLastDisableCalls = gItlwmDisableCalls;
    sLastBssInfoCalls = gItlwmBssInfoCalls;
    sLastBssInfoEmpty = gItlwmBssInfoEmpty;
    sLastExtBssInfoCalls = gItlwmExtBssInfoCalls;
    sLastRxActivityCalls = gItlwmLastRxActivityCalls;
    sLastPeerStatsCalls = gItlwmDataPathPeerStatsCalls;
    sLastQueueTimeCalls = gItlwmLastQueueTimeCalls;
    sLastScanCalls = gItlwmScanReqCalls;
    sLastScanStarted = gItlwmScanReqStarted;
    sLastScanRefused = gItlwmScanReqRefused;
    sLastPostQueued = gItlwmPostMsgQueued;
    sLastPostSent = gItlwmPostMsgSent;
    sLastPostDropped = gItlwmPostMsgDropped;
    setProperty("ItlwmPostMsgQueued", (UInt64)gItlwmPostMsgQueued, 32);
    setProperty("ItlwmPostMsgSent", (UInt64)gItlwmPostMsgSent, 32);
    setProperty("ItlwmPostMsgDropped", (UInt64)gItlwmPostMsgDropped, 32);
    setProperty("ItlwmScanReqCalls", (UInt64)gItlwmScanReqCalls, 32);
    setProperty("ItlwmScanReqStarted", (UInt64)gItlwmScanReqStarted, 32);
    setProperty("ItlwmScanReqRefused", (UInt64)gItlwmScanReqRefused, 32);
    setProperty("ItlwmScanBeacons", (UInt64)gItlwmScanBeacons, 32);
    setProperty("ItlwmAssocCalls", (UInt64)gItlwmAssocCalls, 32);
    setProperty("ItlwmAssocStarted", (UInt64)gItlwmAssocStarted, 32);
    setProperty("ItlwmAssocRefused", (UInt64)gItlwmAssocRefused, 32);
    setProperty("ItlwmAssocNoPmk", (UInt64)gItlwmAssocNoPmk, 32);
    setProperty("ItlwmAssocKeyInReq", (UInt64)gItlwmAssocKeyInReq, 32);
    setProperty("ItlwmJoinMaxState", (UInt64)fJoinMaxState, 32);
    setProperty("ItlwmJoinAssocStatus", (UInt64)fJoinAssocStatusSeen, 32);
    // Why the last BSS-selection pass chose nothing. Read these whenever ItlwmJoinMaxState is 1:
    // a join that never leaves SCAN has been reached twice now with no way to attribute it.
    //   ScanFailDes  0x8000 | mask  -> the target SSID was in the cache and match_bss rejected it;
    //                                 the low byte is one IEEE80211_NODE_ASSOCFAIL_* bit.
    //                0            -> the target was never seen at all, which is a scan problem.
    //   ScanCand / ScanSkipped    -> whether there was anything to reject; Skipped counts nodes
    //                                 dropped on ni_fails before match_bss ran.
    {
        struct ieee80211com *ic = fHalService->get80211Controller();
        setProperty("ItlwmScanCand", (UInt64)ic->ic_scan_cand, 32);
        setProperty("ItlwmScanSkipped", (UInt64)ic->ic_scan_skipped, 32);
        setProperty("ItlwmScanFailOr", (UInt64)ic->ic_scan_fail_or, 32);
        setProperty("ItlwmScanFailDes", (UInt64)ic->ic_scan_fail_des, 32);
        // Who takes the ESS away, and from where. Clears at 0 with ItlwmAssocEsslen also 0 means
        // it was never programmed; clears non-zero with ClearState 2 or 3 is net80211's watchdog
        // giving up from AUTH/ASSOC, which is a real failure rather than a bookkeeping one.
        setProperty("ItlwmEssClears", (UInt64)ic->ic_ess_clears, 32);
        setProperty("ItlwmEssClearState", (UInt64)ic->ic_ess_clear_state, 32);
        setProperty("ItlwmAssocEsslen", (UInt64)gItlwmAssocEsslen, 32);
        // Which RSN sub-check rejected the target, and both sides of the comparison. Read these
        // when ItlwmScanFailDes carries 0x40 (ASSOCFAIL_WPA_PROTO), which on its own says only
        // "the RSN parameters do not overlap" — not which field, and not whose side is wrong.
        setProperty("ItlwmScanRsnDes", (UInt64)ic->ic_scan_rsn_des, 32);
        setProperty("ItlwmScanNiRsn", (UInt64)ic->ic_scan_ni_rsn, 32);
        setProperty("ItlwmScanNiCipher", (UInt64)ic->ic_scan_ni_cipher, 32);
        setProperty("ItlwmScanIcRsn", (UInt64)ic->ic_scan_ic_rsn, 32);
        setProperty("ItlwmScanIcCipher", (UInt64)ic->ic_scan_ic_cipher, 32);
    }
    setProperty("ItlwmAssocUpperAuth0", (UInt64)gItlwmAssocUpperAuthFirst, 32);
    setProperty("ItlwmAssocUpperAuthN", (UInt64)gItlwmAssocUpperAuthLast, 32);
    setProperty("ItlwmAssocKeyInfo0", (UInt64)gItlwmAssocKeyInfoFirst, 32);
    setProperty("ItlwmAssocKeyInfoN", (UInt64)gItlwmAssocKeyInfoLast, 32);
    setProperty("ItlwmJoinTicksAuth", (UInt64)fJoinTicksAuth, 32);
    setProperty("ItlwmJoinTicksAssoc", (UInt64)fJoinTicksAssoc, 32);
    setProperty("ItlwmJoinMgtqMax", (UInt64)fJoinMgtqMax, 32);
    setProperty("ItlwmJoinMgtqStuck", (UInt64)fJoinMgtqStuck, 32);
    setProperty("ItlwmJoinOactive", (UInt64)fJoinOactive, 32);
    setProperty("ItlwmMgtqKicks", (UInt64)gItlwmMgtqKicks, 32);
    if (fJoinRsnCaptured) {
        setProperty("ItlwmJoinIcFlags", (UInt64)fJoinIcFlags, 32);
        setProperty("ItlwmJoinIcRsn", (UInt64)fJoinIcRsn, 32);
        setProperty("ItlwmJoinIcCipher", (UInt64)fJoinIcCipher, 32);
        setProperty("ItlwmJoinNiRsnCipher", (UInt64)fJoinNiRsnCipher, 32);
    }
    if (fJoinFailCaptured) {
        setProperty("ItlwmJoinFailState", (UInt64)fJoinFailState, 32);
        setProperty("ItlwmJoinFailStatus", (UInt64)fJoinFailAssocStatus, 32);
        setProperty("ItlwmJoinFailDeauth", (UInt64)fJoinFailDeauthReason, 32);
        setProperty("ItlwmJoinFailRxAuthFail", (UInt64)fJoinFailRxAuthFail, 32);
        setProperty("ItlwmJoinFailNiRsn", (UInt64)fJoinFailNiRsn, 32);
        setProperty("ItlwmJoinFailNiCipher", (UInt64)fJoinFailNiCipher, 32);
        setProperty("ItlwmJoinFailNiCaps", (UInt64)fJoinFailNiCaps, 32);
        setProperty("ItlwmJoinFailNiFails", (UInt64)fJoinFailNiFails, 32);
        setProperty("ItlwmJoinFailNiAssocFail", (UInt64)fJoinFailNiAssocFail, 32);
        setProperty("ItlwmJoinFailMgtDiscard", (UInt64)fJoinFailMgtDiscard, 32);
        setProperty("ItlwmJoinFailBadRsnIe", (UInt64)fJoinFailBadRsnIe, 32);
        setProperty("ItlwmJoinFailElemBad", (UInt64)fJoinFailElemBad, 32);
        setProperty("ItlwmJoinFailTxNombuf", (UInt64)fJoinFailTxNombuf, 32);
        setProperty("ItlwmJoinFailIfFlags", (UInt64)fJoinFailIfFlags, 32);
    }
    setProperty("ItlwmJoinAssocDone", (UInt64)gItlwmJoinAssocDone, 32);
    setProperty("ItlwmJoinConnectDone", (UInt64)gItlwmJoinConnectDone, 32);
    setProperty("ItlwmLinkIndUp", (UInt64)gItlwmLinkIndUp, 32);
    setProperty("ItlwmLinkIndDown", (UInt64)gItlwmLinkIndDown, 32);
    setProperty("ItlwmLinkDownState", (UInt64)gItlwmLinkDownState, 32);
    setProperty("ItlwmLinkDownPair", (UInt64)gItlwmLinkDownPair, 32);
    setProperty("ItlwmDisableCalls", (UInt64)gItlwmDisableCalls, 32);
    setProperty("ItlwmCCPipesStarted", (UInt64)gItlwmCCPipesStarted, 32);
    setProperty("ItlwmCCPipeStartFail", (UInt64)gItlwmCCPipeStartFail, 32);
    setProperty("ItlwmLeaveNetCalls", (UInt64)gItlwmLeaveNetCalls, 32);
    setProperty("ItlwmLqmPosts", (UInt64)gItlwmLqmPosts, 32);
    setProperty("ItlwmLqmBeaconStall", (UInt64)gItlwmLqmBeaconStall, 32);
    setProperty("ItlwmBssInfoCalls", (UInt64)gItlwmBssInfoCalls, 32);
    setProperty("ItlwmBssInfoEmpty", (UInt64)gItlwmBssInfoEmpty, 32);
    setProperty("ItlwmExtBssInfoCalls", (UInt64)gItlwmExtBssInfoCalls, 32);
    // Mechanism 1. Stage: 0 = registration never ran (boot-arg off, or start() returned false),
    // 1-7 = which factory call failed, 8 = registerInfraEthernetInterface returned success.
    // RegRet is that call's IOReturn. TX is implemented, so TxDequeue/TxFrames are expected to
    // climb once traffic flows; the RX pair MUST still read 0, and a non-zero one there means the
    // family is driving queues this driver is not filling.
    // Read TxFrames against TxDequeue to tell "the family is asking" from "we are sending":
    // TxDequeue climbing with TxFrames flat is frames consumed and thrown away, which looks
    // exactly like working TX from the family's side and like a dead network from the user's.
    setProperty("ItlwmSkywalkStage", (UInt64)gItlwmSkywalkStage, 32);
    setProperty("ItlwmSkywalkRegRet", (UInt64)gItlwmSkywalkRegRet, 32);
    setProperty("ItlwmSkywalkTxDequeue", (UInt64)gItlwmSkywalkTxDequeue, 32);
    setProperty("ItlwmSkywalkTxComplete", (UInt64)gItlwmSkywalkTxComplete, 32);
    setProperty("ItlwmSkywalkRxDequeue", (UInt64)gItlwmSkywalkRxDequeue, 32);
    setProperty("ItlwmSkywalkRxComplete", (UInt64)gItlwmSkywalkRxComplete, 32);
    setProperty("ItlwmSkywalkTxFrames", (UInt64)gItlwmSkywalkTxFrames, 32);
    setProperty("ItlwmSkywalkTxDrops", (UInt64)gItlwmSkywalkTxDrops, 32);
    setProperty("ItlwmSkywalkTxNoMbuf", (UInt64)gItlwmSkywalkTxNoMbuf, 32);
    setProperty("ItlwmSkywalkTxComplFail", (UInt64)gItlwmSkywalkTxComplFail, 32);
    setProperty("ItlwmSkywalkTxListShort", (UInt64)gItlwmSkywalkTxListShort, 32);
    setProperty("ItlwmSkywalkRxFrames", (UInt64)gItlwmSkywalkRxFrames, 32);
    setProperty("ItlwmSkywalkRxDrops", (UInt64)gItlwmSkywalkRxDrops, 32);
    setProperty("ItlwmSkywalkRxNoBuf", (UInt64)gItlwmSkywalkRxNoBuf, 32);
    setProperty("ItlwmSkywalkRxOversize", (UInt64)gItlwmSkywalkRxOversize, 32);
    setProperty("ItlwmSkywalkRxComplFail", (UInt64)gItlwmSkywalkRxComplFail, 32);
    setProperty("ItlwmSkywalkRxFree", (UInt64)gItlwmSkywalkRxFree, 32);
    setProperty("ItlwmSkywalkRxListShort", (UInt64)gItlwmSkywalkRxListShort, 32);
    setProperty("ItlwmSkywalkBsdUnit", (UInt64)gItlwmSkywalkBsdUnit, 32);
    setProperty("ItlwmSkywalkRxFallbackDrops", (UInt64)gItlwmSkywalkRxFallbackDrops, 32);
    setProperty("ItlwmSkywalkQueuesAdded", (UInt64)gItlwmSkywalkQueuesAdded, 32);
    setProperty("ItlwmSkywalkQueuesEnabled", (UInt64)gItlwmSkywalkQueuesEnabled, 32);
    setProperty("ItlwmSkywalkRxPrimeCalls", (UInt64)gItlwmSkywalkRxPrimeCalls, 32);
    setProperty("ItlwmSkywalkRxPrimeRet", (UInt64)gItlwmSkywalkRxPrimeRet, 32);
    setProperty("ItlwmSkyLinkReportCalls", (UInt64)gItlwmSkyLinkReportCalls, 32);
    setProperty("ItlwmSkyLinkReportRet", (UInt64)gItlwmSkyLinkReportRet, 32);
    setProperty("ItlwmLlAddrCalls", (UInt64)gItlwmLlAddrCalls, 32);
    setProperty("ItlwmLlAddrSynced", (UInt64)gItlwmLlAddrSynced, 32);
    setProperty("ItlwmLlAddrLate", (UInt64)gItlwmLlAddrLate, 32);
    setProperty("ItlwmLlAddrLast", (UInt64)gItlwmLlAddrLast, 48);
    setProperty("ItlwmJoinTimeouts", (UInt64)gItlwmJoinTimeouts, 32);
    setProperty("ItlwmLastRxActivityCalls", (UInt64)gItlwmLastRxActivityCalls, 32);
    setProperty("ItlwmSlot450Calls", (UInt64)gItlwmDataPathPeerStatsCalls, 32);
    setProperty("ItlwmSlot451Calls", (UInt64)gItlwmLastQueueTimeCalls, 32);
}
#endif

void AirportItlwm::watchdogAction(IOTimerEventSource *timer)
{
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    (*ifp->if_watchdog)(ifp);
#if __IO80211_TARGET >= __MAC_26_0
    drainStrandedMgmtFrames();
    checkJoinProgress();
    postLqmUpdate();
    publishRuntimeCounters();
#endif
    watchdogTimer->setTimeoutMS(kWatchDogTimerPeriod);
}

#if __IO80211_TARGET >= __MAC_26_0
// Every postMessage in this driver ends in IO80211Glue::sendIOUCToWcl, which panics
// "trying to send on thread panic" @IO80211Glue.cpp:419 unless BOTH hold on the interface work
// queue (_fWorkloop, the object at glue ivars +0x38):
//
//     inGate()   == true      slot 39; false -> refuse
//     onThread() == false     slot 38; true  -> refuse
//
// Not one call site in this driver could satisfy that pair:
//
//   eventHandler        HAL ic_event_handler, i.e. inside iwx_intr   -> onThread() true
//   fakeScanDone        IOTimerEventSource on _fWorkloop             -> onThread() true
//   setLinkStateGated   command gate on _fWorkloop, driven from HAL  -> onThread() true
//   setCOUNTRY_CODE     airportd's ioctl thread, gate open           -> inGate()  false
//
// The trailing "async" flag does not rescue any of them, which is worth stating because it
// looks like it should: IO80211SkywalkInterface::postMessageInternal does route a true flag to
// IO80211Glue::routeEventToWcl (enqueue + signal, no thread rules), but the bound override is
// IO80211InfraInterface::postMessage, and at +0xf9a it calls updateCountryCodeProperty(true)
// inline with a hardcoded argument, never consulting the flag. Verified the hard way: passing
// true changed nothing and panicked at the identical offsets.
//
// So the post has to happen on a thread that is neither the work queue's nor gate-less. That is
// exactly the context Apple builds for its own deferred posts —
// IO80211Glue::processPendingEventQueueSource takes the interface work queue out of glue ivars
// +0x38, wraps the drain in IOWorkLoop::runActionBlock on it (inGate() true), and runs on a
// different queue's event-source thread (onThread() false). This reproduces that shape with a
// thread_call, which is safe to arm from any context.
//
// Always deferred, never inline, even from callers that could close the gate themselves:
// sendIOUCToWcl can sleep on a 50 s deadline waiting for the serial queue, and no caller thread
// of ours — least of all the work loop that services the firmware interrupt — can afford that.
// The flag stays ITLWM_POSTMSG_ASYNC because Apple's own drivers pass it, and it still shortens
// the message types that do honour it.
bool AirportItlwm::enqueuePending(const PendingMsg *msg)
{
    bool queued = false;

    if (fPostMsgCall == NULL || fPendingMsgLock == NULL) {
        gItlwmPostMsgDropped++;
        return false;
    }
    IOSimpleLockLock(fPendingMsgLock);
    uint32_t next = (fPendingMsgTail + 1) % kPendingMsgCount;
    if (next != fPendingMsgHead) {          // full ring keeps the oldest, drops the newest
        fPendingMsg[fPendingMsgTail] = *msg;
        fPendingMsgTail = next;
        queued = true;
    }
    IOSimpleLockUnlock(fPendingMsgLock);

    if (!queued) {
        gItlwmPostMsgDropped++;
        return false;
    }
    gItlwmPostMsgQueued++;
    fPostMsgArmed = true;
    thread_call_enter(fPostMsgCall);
    return true;
}

void AirportItlwm::postMessageSafe(uint32_t type, const void *data, size_t len)
{
    PendingMsg msg;

    if (len > sizeof(msg.data))
        len = sizeof(msg.data);
    msg.kind = kPendingKindMessage;
    msg.type = type;
    msg.arg = 0;
    msg.len = (uint32_t)len;
    if (data != NULL && len > 0)
        memcpy(msg.data, data, len);
    enqueuePending(&msg);
}

// setLinkState has the same contract as postMessage and for the same reason, which cost a panic
// to learn: IO80211InfraInterface::setLinkState -> setLinkStateInternal -> updateLinkSpeed ->
// IO80211Glue::sendIOUCToWcl, which panics "trying to send on thread panic" @IO80211Glue.cpp:419
// unless inGate() && !onThread() on the interface work queue.
//
// The old call site could not satisfy it. ieee80211_set_link_state runs from wherever net80211
// happens to be — for a WPA2 join that is ieee80211_recv_4way_msg3, inside iwx_intr, on
// _fWorkloop's own thread — and wrapping it in getCommandGate()->runAction() only closed the gate
// while leaving onThread() true, which is the half that panics. The deferral ring is the one
// context in this driver that gets both right, so link state goes through it too.
//
// Ordered against the messages that accompany it precisely because they share the ring.
void AirportItlwm::deferLinkState(uint32_t linkState, uint32_t reason)
{
    PendingMsg msg;

    msg.kind = kPendingKindLinkState;
    msg.type = linkState;
    msg.arg = reason;
    msg.len = 0;
    enqueuePending(&msg);
}

// Push one scan result. Tahoe has no getSCAN_RESULT for a driver to answer and the WCL never
// calls getWCL_BSS_INFO, so results arrive at the family only this way — see
// include/Airport/BeaconMetaData.h for the payload contract and where each field came from.
//
// Called from ieee80211_notify_scan_beacon on the work-loop thread, so it must not post inline;
// it copies into the deferral ring, which is also why the ring entry is large. The frame belongs
// to the receive mbuf and is gone by the time the thread_call runs.
// postMessageSafe() silently clamps an oversized payload, which for a beacon would ship a header
// whose ie_len disagrees with the bytes that followed it. The ring must therefore hold a maximal
// beacon exactly, not merely happen to.
_Static_assert(AirportItlwm::kPendingMsgMaxData == sizeof(struct BeaconMetaData) + BEACON_META_MAX_IE_LEN,
               "pending-message ring must hold a maximal BeaconMetaData payload");

void AirportItlwm::postScanBeacon(const struct ieee80211_beacon_event *ev)
{
    struct BeaconMetaData md;
    const uint8_t *ies;
    uint32_t ie_len;
    uint8_t buf[sizeof(md) + BEACON_META_MAX_IE_LEN];

    // 802.11 management header, then the beacon's fixed body: timestamp[8], beacon interval[2],
    // capability[2]. The IE list starts after that, and it is the IE list alone that Apple wants.
    const uint32_t kFixedBody = 8 + 2 + 2;
    const uint32_t kHdr = sizeof(struct ieee80211_frame);
    if (ev == NULL || ev->frame == NULL || ev->len <= kHdr + kFixedBody)
        return;

    const struct ieee80211_frame *wh = (const struct ieee80211_frame *)ev->frame;
    const uint8_t *body = ev->frame + kHdr;

    ies = body + kFixedBody;
    ie_len = ev->len - kHdr - kFixedBody;
    if (ie_len > sizeof(buf) - sizeof(md))
        ie_len = (uint32_t)(sizeof(buf) - sizeof(md));   // truncate rather than drop the BSS

    bzero(&md, sizeof(md));
    md.ie_len = ie_len;
    md.chanspec = AppleChanSpec20MHz(ev->chan);
    md.primary_chan = ev->chan;      // only 20 MHz specs are emitted, so primary == the channel
    memcpy(md.bssid, wh->i_addr2, sizeof(md.bssid));
    md.bintval = (uint16_t)(body[8] | (body[9] << 8));
    md.capinfo = (uint16_t)(body[10] | (body[11] << 8));

    // Same conversion the rest of this driver uses for net80211's rssi (see getRSSI).
    //
    // FLAG_RSSI_VALID is not optional: without it setBeaconDataFromMsg branches straight past the
    // store and every BSS reports 0 dBm, which is what "rssi= 0 snr= 0" in CoreCapture's
    // "Scan Updated" lines means. ONCHANNEL is true because net80211 only raises this event for
    // frames received while tuned to the channel being scanned, and it is also the bool handed to
    // isNewBssBetter(), so leaving it clear would make the family distrust a correct reading.
    // `noise` and `snr` stay unclaimed — net80211's rxi carries neither, and a zero presented as
    // valid is worse than an absent field.
    md.rssi = ev->rssi + IWM_MIN_DBM;
    md.flags |= BEACON_META_FLAG_RSSI_VALID | BEACON_META_FLAG_RSSI_ONCHANNEL;
    if (!ev->probe_resp)
        md.flags |= BEACON_META_FLAG_FROM_BEACON;

    // SSID comes from the first IE, id 0. BEACON_META_FLAG_SSID is both bits: bit 1 alone marks
    // the inline copy present but does not reach setSSID. Apple sends bit 1 only and is named by
    // some other route; see the divergence note in BeaconMetaData.h before narrowing this.
    if (ie_len >= 2 && ies[0] == 0 && ies[1] <= sizeof(md.ssid) && (uint32_t)ies[1] + 2 <= ie_len) {
        md.ssid_len = ies[1];
        if (md.ssid_len > 0)
            memcpy(md.ssid, ies + 2, md.ssid_len);
        md.flags |= BEACON_META_FLAG_SSID;
    }

    memcpy(buf, &md, sizeof(md));
    memcpy(buf + sizeof(md), ies, ie_len);
    postMessageSafe(APPLE80211_M_BSS_BEACON, buf, sizeof(md) + ie_len);
    gItlwmScanBeacons++;

    // Keep a copy for getWCL_BSS_INFO. **This is the only place the IE list is ever available** —
    // an ieee80211_node keeps the parsed results (SSID, rates, RSN parameters), not the bytes, so
    // a beacon cannot be rebuilt from one after the fact. The WCL demands the bytes at link-up and
    // abandons the network if it cannot have them.
    //
    // Cached by BSSID, for every BSS seen, with no reference to the join state. The first version
    // stored one beacon and only when it matched the join target or ic_bss, and that is a race, not
    // an optimisation: this event fires only while `ic_state == IEEE80211_S_SCAN` (see
    // ieee80211_notify_scan_beacon), and *during* a scan `ic_bss` is the scan's scratch node rather
    // than the BSS about to be joined — so the ic_bss arm almost never matches, and the whole cache
    // then depended on the target's beacon happening to arrive inside one join-scan window. It did
    // on one boot and did not on the next, with 546 beacons seen either way and the association
    // torn down for want of one of them.
    cacheScanBeacon(md.bssid, buf, sizeof(md) + ie_len);
}

// Most-recent-wins, keyed by BSSID: refresh the entry for this BSS if it has one, else take a free
// slot, else evict the oldest. Bounded and self-refreshing, so the BSSs still on the air are the
// ones retained — which is exactly the set a join can pick from.
void AirportItlwm::cacheScanBeacon(const uint8_t *bssid, const void *msg, size_t len)
{
    uint32_t victim = 0;
    uint64_t oldest;

    if (len > sizeof(fBssBeacon[0].data))
        return;
    IOSimpleLockLock(fPendingMsgLock);
    oldest = fBssBeacon[0].seq;
    for (uint32_t i = 0; i < kBssBeaconCacheCount; i++) {
        if (fBssBeacon[i].len == 0) {           // free slot: take it
            victim = i;
            break;
        }
        if (IEEE80211_ADDR_EQ(fBssBeacon[i].bssid, bssid)) {
            victim = i;                         // same BSS: refresh in place
            break;
        }
        if (fBssBeacon[i].seq < oldest) {
            oldest = fBssBeacon[i].seq;
            victim = i;
        }
    }
    IEEE80211_ADDR_COPY(fBssBeacon[victim].bssid, bssid);
    memcpy(fBssBeacon[victim].data, msg, len);
    fBssBeacon[victim].len = (uint32_t)len;
    fBssBeacon[victim].seq = ++fBssBeaconSeq;
    IOSimpleLockUnlock(fPendingMsgLock);
}

// Answer ioctl 433 out of that cache. `outLen` is the WCL's own `IOMallocZeroData(0x844)`, which
// is `sizeof(BeaconMetaData) + BEACON_META_MAX_IE_LEN` exactly, so a cached beacon always fits;
// the check is here so a future release changing the size fails visibly rather than overruns.
//
// Returning false is not neutral: WCLNetManager::updateBss treats a failed get as
// "Update Bss fail" and issues leaveNetworkCommand, which drops the association. So the lookup
// tries the associated BSS first and the join target second — during a join those differ, because
// net80211 has not adopted the target as ic_bss until the exchange completes.
bool AirportItlwm::copyCurrentBssBeacon(void *out, size_t outLen)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    const uint8_t *keys[2];
    uint32_t nkeys = 0;
    bool have = false;

    if (ic->ic_bss != NULL)
        keys[nkeys++] = ic->ic_bss->ni_bssid;
    if (fJoinPending)
        keys[nkeys++] = fJoinBssid;

    IOSimpleLockLock(fPendingMsgLock);
    for (uint32_t k = 0; k < nkeys && !have; k++) {
        for (uint32_t i = 0; i < kBssBeaconCacheCount; i++) {
            if (fBssBeacon[i].len == 0 || !IEEE80211_ADDR_EQ(fBssBeacon[i].bssid, keys[k]))
                continue;
            if (fBssBeacon[i].len <= outLen) {
                memcpy(out, fBssBeacon[i].data, fBssBeacon[i].len);
                have = true;
            }
            break;
        }
    }
    IOSimpleLockUnlock(fPendingMsgLock);
    return have;
}

// Pull one information element out of the same cache, header included, for callers that want a
// single IE rather than the whole beacon — getWCL_EXTENDED_BSS_INFO wants the RSN element. Returns
// the bytes copied, 0 if the BSS does not advertise that element or the cache is cold.
size_t AirportItlwm::copyCurrentBssIe(uint8_t id, void *out, size_t outLen)
{
    uint8_t msg[kPendingMsgMaxData];
    const uint8_t *ies;
    size_t left, copied = 0;

    // Through the same lookup as 433 rather than reaching into the table, so the two answers always
    // describe the same BSS.
    if (!copyCurrentBssBeacon(msg, sizeof(msg)))
        return 0;
    ies = msg + sizeof(struct BeaconMetaData);
    left = ((const struct BeaconMetaData *)msg)->ie_len;
    if (left > sizeof(msg) - sizeof(struct BeaconMetaData))
        return 0;                       // ie_len is ours, but never trust it against the buffer
    // Walk the list rather than indexing: element order is not guaranteed, and a malformed length
    // must stop the walk rather than run off the end.
    while (left >= 2) {
        size_t elen = 2 + (size_t)ies[1];
        if (elen > left)
            break;
        if (ies[0] == id) {
            if (elen <= outLen) {
                memcpy(out, ies, elen);
                copied = elen;
            }
            break;
        }
        ies += elen;
        left -= elen;
    }
    return copied;
}

// The WCL join-completion messages — the other half of setWCL_ASSOCIATE, and the half that is
// easy to leave out. The setter only starts the join; WCLJoinManager's FSM then sits in
// JOIN_MANAGER_STATE_IN_PROGRESS until the driver posts one of two messages, and until then
// nothing can connect and nothing says why. This is the same shape as the scan path, where the
// FSM waited on message 237 while the driver posted only the userspace notification 10.
//
// Decoded from WCLJoinManager's own FSM tables (gfsmJOIN_MANAGERConfiguration, whose transition
// matrix, handler list and name arrays are all reachable from __GLOBAL__sub_I_WCLJoinManager.cpp):
//
//   IN_PROGRESS  --JOIN_ASSOC_COMPLETE(4)---> ASSOC_DONE        handleJoinAssocComplete
//   IN_PROGRESS  --JOIN_CONNECT_COMPLETE(5)-> CONNECT_COMPLETE  handleJoinConnectComplete
//   ASSOC_DONE   --JOIN_CONNECT_COMPLETE(5)-> CONNECT_COMPLETE  handleJoinConnectComplete
//   IN_PROGRESS  --JOIN_COMPLETE(6)---------> IDLE              raised by the FSM itself
//
// Three consequences the code below depends on:
//
//  - CONNECT_COMPLETE does *not* accept JOIN_ASSOC_COMPLETE, so 211 must precede 213, never
//    follow it. postMessageSafe's ring preserves order.
//  - IN_PROGRESS accepts JOIN_CONNECT_COMPLETE directly, so a join that fails before it ever
//    associates can still be reported with 213 alone.
//  - IDLE accepts neither, so posting either one outside a join is inert — but they are gated on
//    fJoinPending anyway, so the driver's own roaming never confuses the FSM.
//
// Failure is always reported as a non-zero *connect* status, never a non-zero assoc status:
// handleJoinAssocComplete raises no event for a non-zero status other than the special value
// 1000, so message 211 is a dead end for reporting failure, while handleJoinConnectComplete
// hands a non-zero status to WCLJoinRequest::updateAndCheckForNextCandidate, which walks to the
// next candidate (up to four) and re-enters setWCL_ASSOCIATE.

// How long a join may sit without net80211 advancing a state before the driver gives up on it and
// tells the WCL to try the next candidate. Measured from the last observed progress, not from the
// start: selecting a BSS costs a scan pass, and the auth/assoc exchange and the four-way handshake
// each take their own time, so a single fixed budget would either cut a healthy join short or wait
// out a dead one. Well inside the WCL's own 35 s for the whole join.
#define ITLWM_JOIN_STALL_NS  (10ULL * 1000000000ULL)

void AirportItlwm::joinStarted(const uint8_t *bssid, uint32_t ssid_len)
{
    uint64_t now;

    clock_get_uptime(&now);
    absolutetime_to_nanoseconds(now, &now);
    memcpy(fJoinBssid, bssid, sizeof(fJoinBssid));
    fJoinLastState = 0;
    // Left disarmed here on purpose. See armJoinEssWatch(): this runs before the caller decides
    // whether to program net80211 at all, and arming on the request's ssid_len rather than on what
    // was actually programmed is what produced a self-sustaining 1 Hz failure loop.
    fJoinWatchEss = false;
    fJoinDeadlineNs = now + ITLWM_JOIN_STALL_NS;
    fJoinPending = true;
}

// Arm the "net80211 gave up" watch, and only once net80211 has actually been told what to join.
//
// checkJoinProgress treats an empty ic_des_esslen under a pending join as net80211 having
// abandoned the attempt. That inference is only valid if a non-empty one was programmed, and the
// watch used to be armed from the *request's* ssid_len inside joinStarted() — which runs before
// beginAssociateGated's early return for an exchange already in flight, the path that deliberately
// does not call associateSSID at all.
//
// On that path the watch was armed against an ESS nobody had set, so the next watchdog tick
// reported a failure that had not happened. The WCL answers a failed connect by trying the next
// candidate, which arrives in the same state, which arms the watch again: twenty joins in two
// minutes, each dying at exactly one tick, with ItlwmJoinTimeouts at 0 because the deadline was
// never the thing that fired. The loop has no exit, because every iteration recreates its own
// precondition.
//
// **Rule: a watch armed from a request and tested against driver state is only sound if the code
// between them cannot decline to act.** Arm it from the state the test reads.
void AirportItlwm::armJoinEssWatch()
{
    struct ieee80211com *ic = fHalService->get80211Controller();

    fJoinWatchEss = (ic->ic_des_esslen != 0);
    gItlwmAssocEsslen = (uint32_t)ic->ic_des_esslen;
}

void AirportItlwm::postJoinAssocComplete(uint16_t status, uint16_t reason)
{
    struct apple80211_assoc_event ev;
    uint64_t now;

    if (!fJoinPending)
        return;
    bzero(&ev, sizeof(ev));
    ev.status = status;
    ev.reason = reason;
    // auth_phase == 0 selects assoc_time and leaves auth_time untouched, which is what this
    // event reports: net80211 raises IEEE80211_EVT_STA_ASSOC_DONE from the association response,
    // not from authentication. A non-zero value would instead route `reason` into
    // debugCCOnAuthFailures and have the WCL read auth_time.
    ev.auth_phase = 0;
    memcpy(ev.bssid, fJoinBssid, sizeof(ev.bssid));
    clock_get_uptime(&now);
    absolutetime_to_nanoseconds(now, &now);
    ev.assoc_time = now;
    // sizeof and nothing else: authAssocCompleteEventHandler requires exactly 0x1c and raises no
    // event at all on any other length, which is indistinguishable from never posting.
    postMessageSafe(APPLE80211_M_WCL_AUTH_ASSOC_COMPLETE, &ev, sizeof(ev));
    gItlwmJoinAssocDone++;
}

// Snapshot everything that names an association failure, at the moment the driver decides the
// join has failed. Sticky on the first failure of the boot, for the reason ITLWM_PREINIT_SNAP
// records: sampling after a teardown path measures the teardown. net80211's own explanation of
// this failure only exists in an XYLog, which is kprintf and has no sink on this machine, so
// these properties are the whole diagnostic channel.
//
// The pair that matters is what each side advertises. An association that reaches ASSOC and is
// refused is almost always an RSN parameter the AP will not accept, and having both halves in one
// reading avoids spending a boot to get the second.
// Re-drive the transmit path when a management frame is sitting in ic_mgtq.
//
// **Nothing else in this driver retries one.** ieee80211_send_mgmt enqueues the frame and calls
// ifp->if_start, which for this HAL is
//
//     ItlIwx::iwx_start -> getMainCommandGate()->attemptAction(_iwx_start_task, ifp)
//
// and `attemptAction` is *non-blocking*: if any other thread holds the gate — and that gate lives
// on _fWorkloop, the IO80211WorkQueue that the WCL, the deferred postMessage drain, the scan timer
// and the firmware interrupt all run on — it returns kIOReturnCannotLock, the action never runs,
// and the frame stays queued with nobody to notice. `_iwx_start_task` has a second silent exit of
// the same kind: it returns immediately when ifq_is_oactive, and its qfullmsk check sets oactive
// and breaks *before* the mgmt dequeue, so a full data queue also strands management traffic.
//
// The only thing that would normally re-drive it is `ifp->if_timer`, and iwx sets that **after** a
// successful transmit — so a frame that never got transmitted never arms the retry. The HAL's own
// recovery, iwx_clear_oactive, only fires from the Tx-completion path, which by construction is
// not running when nothing was sent.
//
// Measured on 26.6: ItlwmJoinMgtqMax = 1 for the whole join while net80211 sat in ASSOC for 15 s
// across four attempts and no association response ever arrived. The association request was
// built correctly and never left the host.
//
// This runs on fWatchdogWorkLoop, a *different* work loop, so it is exactly the caller that can
// take the gate when the owner has let go. Same shape as iwx_clear_oactive's own
// `(*ifp->if_start)(ifp)`.
//
// **This is a repair for a missing retry, not a cure for the contention.** A frame that waits up
// to a second to go out is still a frame that missed its exchange; the underlying question — who
// holds _fWorkloop's gate, and for how long — is unanswered, and 1 Hz is a poor substitute for
// transmitting on time. See the write-up in AGENTS.md.
void AirportItlwm::drainStrandedMgmtFrames()
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    struct _ifnet *ifp = &ic->ic_ac.ac_if;

    if (mq_len(&ic->ic_mgtq) == 0)
        return;
    gItlwmMgtqKicks++;
    if (ifp->if_start != NULL)
        (*ifp->if_start)(ifp);
}

void AirportItlwm::snapshotJoinFailure()
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    struct ieee80211_node *ni = ic->ic_bss;

    if (fJoinFailCaptured)
        return;
    fJoinFailCaptured = true;
    fJoinFailState = ic->ic_state;
    fJoinFailAssocStatus = ic->ic_assoc_status;
    fJoinFailDeauthReason = ic->ic_deauth_reason;
    fJoinFailRxAuthFail = ic->ic_stats.is_rx_auth_fail;
    // Why net80211 threw a management frame away. is_rx_mgtdiscard is the one that matters here:
    // ieee80211_recv_assoc_resp's first act is to bump it and return when ic_state is not ASSOC,
    // so a non-zero value means an association response *did* arrive and was dropped, while zero
    // means none ever came. The rest cover the parse and content failures on the same path.
    fJoinFailMgtDiscard = ic->ic_stats.is_rx_mgtdiscard;
    fJoinFailBadRsnIe = ic->ic_stats.is_rx_assoc_badrsnie;
    fJoinFailElemBad = ic->ic_stats.is_rx_elem_missing +
                       ic->ic_stats.is_rx_elem_toobig +
                       ic->ic_stats.is_rx_elem_toosmall;
    // The association request is a much larger frame than the authentication one — SSID, rates,
    // RSN IE, HT and VHT capabilities — so it can fail to allocate where the auth frame did not.
    // ieee80211_newstate discards IEEE80211_SEND_MGMT's return value, so this counter is the only
    // record that a frame was never built.
    fJoinFailTxNombuf = ic->ic_stats.is_tx_nombuf;
    fJoinFailIfFlags = ic->ic_ac.ac_if.if_flags;
    // No ic_rsn* here on purpose — see the capture in checkJoinProgress. deselect_ess has
    // already zeroed them by the time this runs, so anything read here describes the teardown.
    // What the AP advertises, plus its RSN capabilities (bit 6 MFPC, bit 7 MFPR).
    if (ni != NULL) {
        fJoinFailNiRsn = ((uint32_t)ni->ni_rsnprotos << 16) | (uint16_t)ni->ni_rsnakms;
        fJoinFailNiCipher = ((uint32_t)ni->ni_rsnciphers << 16) | (uint16_t)ni->ni_rsngroupcipher;
        fJoinFailNiCaps = ni->ni_rsncaps;
        fJoinFailNiFails = ni->ni_fails;
        fJoinFailNiAssocFail = ni->ni_assoc_fail;
    }
}

void AirportItlwm::postJoinConnectComplete(uint16_t status, uint16_t reason)
{
    struct apple80211_connection_complete_event ev;
    uint64_t now;

    if (!fJoinPending)
        return;
    if (status != 0)
        snapshotJoinFailure();
    fJoinPending = false;
    bzero(&ev, sizeof(ev));
    ev.status = status;
    ev.reason = reason;
    clock_get_uptime(&now);
    absolutetime_to_nanoseconds(now, &now);
    ev.timestamp = now;
    // Exactly 0xa4, same contract as above; the trailing bytes are unread but must be present.
    postMessageSafe(APPLE80211_M_WCL_CONNECT_COMPLETE, &ev, sizeof(ev));
    gItlwmJoinConnectDone++;
}

// The link half of a join, owed to a different FSM than 211/213 and easy to miss because the join
// looks finished without it. WCLJoinManager reaches JOIN_MANAGER_STATE_IDLE on 211 + 213 alone —
// measured, with no failures — while WCLNetManager stays in NET_MANAGER_STATE_LINK_DOWN, and it is
// NET_MANAGER that the rest of the system reads: with it down, `ifconfig` reports `status:
// inactive`, `networksetup -getairportnetwork` reports no association, and auto-join never runs.
//
// Unlike 211/213 this one is *not* gated on fJoinPending. A link coming up or going down is a fact
// about the interface, not about a join: net80211 drops and re-establishes a link on its own
// (roaming, beacon loss, a CSA), and the WCL has to track every one of those or its idea of the
// link diverges from the driver's for as long as the association lasts.
//
// Ordering: NET_MANAGER only accepts CONNECT_COMPLETE once it is out of LINK_DOWN, so this must be
// posted **before** 213. Both go through postMessageSafe's ring, which preserves order.
void AirportItlwm::postLinkStatusInd(bool up, uint32_t reason)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    struct apple80211_link_status_ind ind;

    bzero(&ind, sizeof(ind));
    if (ic->ic_bss != NULL)
        IEEE80211_ADDR_COPY(ind.bssid, ic->ic_bss->ni_bssid);
    ind.link_up = up ? 1 : 0;
    ind.reason = reason;
    // Exactly 0x10, matching both of Apple's producers.
    postMessageSafe(APPLE80211_M_WCL_LINK_STATUS_IND, &ind, sizeof(ind));
    if (up)
        gItlwmLinkIndUp++;
    else
        gItlwmLinkIndDown++;      // ItlwmLinkDownState is sampled in setLinkStatus, not here

#if __IO80211_TARGET >= __MAC_26_0
    // Tell the *Skywalk* layer, which the message above does not reach. This is the only link-state
    // notification into it: reportLinkStatus stores the state and, on a change, raises a nexus
    // event (0xE0060102 up / 0xE0060100 down). `IO80211SkywalkInterface::enableDatapath()` looks
    // like the obvious candidate for starting the data path and is a **stub** — `xor eax,eax; ret`
    // — so it cannot be, and this is what is left.
    //
    // Measured on the boot that prompted this: with the legacy-ethernet bridge attached, the netif
    // nexus created ("netif:0:AirportItlwmSkywalkInterface.en3") and the queue set holding exactly
    // our four queues, every queue callback still read 0. The link had come up as far as net80211
    // and the WCL were concerned, and the Skywalk layer had never been told.
    //
    if (fNetIf != NULL) {
        gItlwmSkyLinkReportRet = (uint32_t)fNetIf->reportLinkStatus(up ? 1 : 0, 0);
        gItlwmSkyLinkReportCalls++;
    }
#endif
}

// Message 39, and the only thing keeping a completed connection alive. WCLNetManager::
// assocTimerAction tears the network down 60 s after link-up unless this message keeps arriving —
// see include/Airport/LqmEventData.h for the two timestamps it refreshes and why nothing else
// writes them.
//
// Posted every kLqmPostTicks watchdog ticks rather than every tick: the WCL's deadline is 60 s and
// its own poll is 5 s, so 5 s here leaves an order of magnitude of margin while costing one
// deferred-message slot per interval instead of one per second. A longer interval also makes the
// beacon delta meaningful — over one second a single missed beacon period can read zero on a link
// that is perfectly healthy.
//
// Only the counter group is filled. Every other group in the payload is gated by its own validity
// byte and this driver has nothing honest to put in any of them, so they stay clear and the WCL
// skips them; see the header for why that is safe here and would not be for a struct without the
// flags. Deltas, not totals — a running total keeps reading non-zero after the AP disappears, which
// is the one thing this message must never do.
void AirportItlwm::postLqmUpdate()
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    struct _ifnet *ifp = &ic->ic_ac.ac_if;
    struct apple80211_lqm_event_data lqm;
    uint32_t beacons, rxPackets, beaconDelta, rxDelta;

    if (ic->ic_state != IEEE80211_S_RUN || ic->ic_bss == NULL)
        return;
    if (++fLqmTick < kLqmPostTicks)
        return;
    fLqmTick = 0;

    beacons = ic->ic_rx_beacons;
    // netStat->inputPackets, not ifp->if_ipackets: the latter exists on this port's ifnet shim but
    // nothing ever increments it, so it would make liveness_traffic a permanent zero that looks
    // like a working field.
    rxPackets = (ifp->netStat != NULL) ? ifp->netStat->inputPackets : 0;
    // Unsigned subtraction, so a wrap of either free-running counter still yields the true delta.
    beaconDelta = beacons - fLqmLastBeacons;
    rxDelta     = rxPackets - fLqmLastRxPackets;
    fLqmLastBeacons   = beacons;
    fLqmLastRxPackets = rxPackets;

    if (beaconDelta == 0)
        gItlwmLqmBeaconStall++;

    bzero(&lqm, sizeof(lqm));
    lqm.liveness_beacon  = beaconDelta;
    lqm.liveness_beacon2 = beaconDelta;
    lqm.liveness_traffic = rxDelta;
    lqm.counters_valid   = 1;
    // Apple clears this, and zeroes the counter group with it, when the firmware statistics have
    // not advanced. Mirror that rather than always claiming freshness: the consumer refuses to
    // stamp either timestamp without it, which is the correct outcome when nothing arrived.
    lqm.counters_fresh   = (beaconDelta != 0 || rxDelta != 0) ? 1 : 0;
    lqm.event_valid      = 1;

    // Exactly 0x1dc. handleLqmUpdate checks the length and silently drops anything else.
    postMessageSafe(APPLE80211_M_LQM_UPDATE, &lqm, sizeof(lqm));
    gItlwmLqmPosts++;
}

// Called once per watchdog tick. Two ways a join dies without any event reaching the driver, and
// both would otherwise leave the FSM in IN_PROGRESS until the WCL's 35 s timeout:
//
//  - net80211 gives up on its own. ieee80211_watchdog calls ieee80211_deselect_ess after three
//    failed auth/assoc attempts, which clears ic_des_essid — so an empty desired ESSID under a
//    pending join means the state machine has stopped trying. This is the ordinary outcome of a
//    wrong PSK or an AP that has gone away, and it is a real signal rather than a deadline.
//  - nothing happens at all. The deadline covers that.
void AirportItlwm::checkJoinProgress()
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    uint64_t now;

    if (!fJoinPending)
        return;
    clock_get_uptime(&now);
    absolutetime_to_nanoseconds(now, &now);
    // Highest net80211 state any join has reached, sampled at 1 Hz. It splits the two failures
    // that look identical from the counters alone: staying at SCAN(1) means ieee80211_match_bss
    // rejected the target on every pass, so the programming is wrong; reaching AUTH(2) or
    // ASSOC(3) means the BSS was selected and the exchange itself failed. The sampling can miss a
    // short-lived AUTH, so a 1 here is weaker evidence than a 2.
    if ((uint32_t)ic->ic_state > fJoinMaxState)
        fJoinMaxState = ic->ic_state;
    // Progress within *this* join extends the deadline, so the budget is "10 s without moving"
    // rather than "10 s in total". Selecting a BSS costs a whole scan pass, and a fixed budget
    // measured from the request would spend most of it there and then abandon a join that was
    // about to succeed.
    if ((uint32_t)ic->ic_state > fJoinLastState) {
        fJoinLastState = ic->ic_state;
        fJoinDeadlineNs = now + ITLWM_JOIN_STALL_NS;
    }
    // What we advertise, captured the first time net80211 gets past SCAN — i.e. while the attempt
    // is live. **Not at failure.** ieee80211_deselect_ess calls ieee80211_disable_rsn, which zeroes
    // ic_rsnprotos/akms/ciphers/groupcipher, and the failure is *reported* from the
    // `ic_des_esslen == 0` branch below, which is only reachable once deselect_ess has run. A
    // snapshot taken there therefore reads zero by construction and describes the teardown, not
    // the attempt — the trap ITLWM_PREINIT_SNAP already records, walked into a second time.
    if (!fJoinRsnCaptured && ic->ic_state >= IEEE80211_S_AUTH) {
        struct ieee80211_node *ni = ic->ic_bss;
        fJoinRsnCaptured = true;
        fJoinIcFlags = ic->ic_flags;    // IEEE80211_F_RSNON / F_PSK live here
        fJoinIcRsn = ((uint32_t)ic->ic_rsnprotos << 16) | (uint16_t)ic->ic_rsnakms;
        fJoinIcCipher = ((uint32_t)ic->ic_rsnciphers << 16) | (uint16_t)ic->ic_rsngroupcipher;
        // The negotiated pairwise cipher for this node is what goes into the assoc request's
        // RSN IE, so it is the field that decides whether the AP will accept the frame.
        if (ni != NULL)
            fJoinNiRsnCipher = ((uint32_t)ni->ni_rsncipher << 16) |
                               (uint16_t)ni->ni_rsngroupcipher;
    }
    // Watchdog ticks spent in each state during a join, at 1 Hz. Distinguishes "sat in ASSOC
    // waiting for a response that never came" from "bounced straight back out".
    if (ic->ic_state == IEEE80211_S_AUTH)
        fJoinTicksAuth++;
    else if (ic->ic_state == IEEE80211_S_ASSOC)
        fJoinTicksAssoc++;
    // Management frames waiting to be transmitted. ieee80211_send_mgmt builds the association
    // request, mq_enqueue()s it here and calls if_start; a queue that stays non-empty while
    // net80211 sits in ASSOC means the frame was built and never handed to the hardware.
    // Measured: max 1 during a join, i.e. the association request never left. See
    // drainStrandedMgmtFrames() for why nothing retried it.
    if (mq_len(&ic->ic_mgtq) > fJoinMgtqMax)
        fJoinMgtqMax = mq_len(&ic->ic_mgtq);
    if (mq_len(&ic->ic_mgtq) > 0)
        fJoinMgtqStuck++;
    if (ifq_is_oactive(&ic->ic_ac.ac_if.if_snd))
        fJoinOactive++;
    // ieee80211_recv_assoc_resp stores the AP's status code here on every association response,
    // success or not, and nothing on the failure path clears it. Latching the first non-zero one
    // separates "the AP refused us, and this is why" from "no response ever arrived", which the
    // failure snapshot alone cannot tell apart.
    if (fJoinAssocStatusSeen == 0 && ic->ic_assoc_status != 0)
        fJoinAssocStatusSeen = ic->ic_assoc_status;
    if (fJoinWatchEss && ic->ic_des_esslen == 0) {
        XYLog("WCL join: net80211 deselected the ESS, reporting a failed connect\n");
        postJoinConnectComplete(IEEE80211_STATUS_UNSPECIFIED, ic->ic_deauth_reason);
        return;
    }
    if (now < fJoinDeadlineNs)
        return;
    XYLog("WCL join: no completion within the deadline, reporting a failed connect\n");
    gItlwmJoinTimeouts++;
    postJoinConnectComplete(IEEE80211_STATUS_UNSPECIFIED, 0);
}

void AirportItlwm::postMsgThreadCall(thread_call_param_t p0, thread_call_param_t)
{
    ((AirportItlwm *)p0)->drainPendingMessages();
}

IOReturn AirportItlwm::postMsgGated(OSObject *target, void *arg0, void *, void *, void *)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, target);
    PendingMsg *msg = (PendingMsg *)arg0;
    // Through the controller, not the interface: IO80211Controller::postMessage hands the event
    // to the IO80211PostOffice (controller ivars +0xb18, from CreatePostOffice, slot 440), which
    // is what AppleBCMWLANCore::postMessageInfra and ::scanComplete do. Calling
    // fNetIf->postMessage reached the interface but left the WCL believing a scan was still in
    // flight, so every later APPLE80211_IOC_SCAN_REQ came back EBUSY(16) without ever reaching
    // the driver. Same type and payload Broadcom sends: scanComplete posts type 0xa
    // (APPLE80211_M_SCAN_DONE) with a 4-byte status and the async flag set.
    if (that != NULL && that->fNetIf != NULL)
        that->postMessage(that->fNetIf, msg->type, msg->len ? msg->data : NULL, msg->len,
                          ITLWM_POSTMSG_ASYNC);
    return kIOReturnSuccess;
}

// Runs on the thread_call's own thread, so onThread(_fWorkloop) is false; runAction closes the
// gate, so inGate() is true. Drains to empty rather than one per wake, because thread_call_enter
// coalesces: several posts can arrive for a single firing.
void AirportItlwm::drainPendingMessages()
{
    for (;;) {
        PendingMsg msg;
        bool have = false;

        IOSimpleLockLock(fPendingMsgLock);
        if (fPendingMsgHead != fPendingMsgTail) {
            msg = fPendingMsg[fPendingMsgHead];
            fPendingMsgHead = (fPendingMsgHead + 1) % kPendingMsgCount;
            have = true;
        }
        IOSimpleLockUnlock(fPendingMsgLock);

        if (!have)
            return;
        if (_fWorkloop == NULL || fNetIf == NULL) {
            gItlwmPostMsgDropped++;         // keep draining; the ring must not stall
            continue;
        }
        if (msg.kind == kPendingKindLinkState)
            _fWorkloop->runAction(setLinkStateGated, this,
                                  (void *)(uintptr_t)msg.type, (void *)(uintptr_t)msg.arg);
        else
            _fWorkloop->runAction(postMsgGated, this, &msg);
        gItlwmPostMsgSent++;
    }
}
#endif

void AirportItlwm::fakeScanDone(OSObject *owner, IOTimerEventSource *sender)
{
    UInt32 msg = 0;
    AirportItlwm *that = (AirportItlwm *)owner;
#if __IO80211_TARGET >= __MAC_26_0
    // Two messages, because Tahoe has two audiences and only one of them was being told.
    //
    // 237 drives the WCL scan-manager FSM. Without it the FSM sits in
    // SCAN_MANAGER_STATE_IN_PROGRESS forever and every later APPLE80211_IOC_SCAN_REQ is refused
    // by `handleScanRequest: WCLScanManager scan in progress rejecting` with EBUSY(16). Measured
    // before this: 162 `IO80211ScanManager::startScan` attempts, of which 4 reached the driver.
    //
    // 10 (APPLE80211_M_SCAN_DONE) is the userspace notification airportd waits on, and is what
    // this driver already sent — which is why the first scan of every burst looked fine and every
    // one after it failed.
    //
    // Apple splits these across two objects: AppleBCMWLANScanAdapter::scanComplete posts 237,
    // AppleBCMWLANCore::scanComplete posts 10. Both use a 4-byte status and the async flag, and
    // the adapter sends the WCL event first. postMessageSafe preserves order.
    that->postMessageSafe(APPLE80211_M_WCL_SCAN_COMPLETE, &msg, sizeof(msg));
    that->postMessageSafe(APPLE80211_M_SCAN_DONE, &msg, sizeof(msg));
#else
    that->fNetIf->postMessage(APPLE80211_M_SCAN_DONE, &msg, 4, ITLWM_POSTMSG_ASYNC);
#endif
}

bool AirportItlwm::init(OSDictionary *properties)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    bool ret = super::init(properties);
    awdlSyncEnable = true;
    power_state = 0;
#if __IO80211_TARGET >= __MAC_26_0
    fJoinPending = false;
    fJoinWatchEss = false;
    fJoinMaxState = 0;
    fJoinLastState = 0;
    fJoinFailCaptured = false;
    fJoinAssocStatusSeen = 0;
    fJoinRsnCaptured = false;
    fJoinTicksAuth = 0;
    fJoinTicksAssoc = 0;
    fJoinMgtqMax = 0;
    fJoinMgtqStuck = 0;
    fJoinOactive = 0;
    fJoinDeadlineNs = 0;
    memset(fJoinBssid, 0, sizeof(fJoinBssid));
#endif
    memset(geo_location_cc, 0, sizeof(geo_location_cc));
    return ret;
}

IOService* AirportItlwm::probe(IOService *provider, SInt32 *score)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    IOPCIEDeviceWrapper *wrapper = OSDynamicCast(IOPCIEDeviceWrapper, provider);
    if (!wrapper) {
        XYLog("%s Not a IOPCIEDeviceWrapper instance\n", __FUNCTION__);
        return NULL;
    }
    pciNub = wrapper->pciNub;
    fHalService = wrapper->fHalService;
    if (!pciNub || !fHalService) {
        XYLog("%s Not a valid IOPCIEDeviceWrapper instance\n", __FUNCTION__);
        return NULL;
    }
    return super::probe(provider, score);
}

#define LOWER32(x)  ((uint64_t)(x) & 0xffffffff)
#define HIGHER32(x) ((uint64_t)(x) >> 32)

// CoreCapture ring size, and the notify threshold that has to track it.
// CCLogPipe::initWithOwnerNameCapacity validates the options before allocating anything and
// returns false when min_log_size_notify > pipe_size:
//
//     mov rax, [r14 + 0x18]   ; min_log_size_notify
//     cmp rax, [r14 + 0x10]   ; pipe_size
//     jbe proceed             ; else os_log the mismatch and return false
//
// Upstream pins the threshold at the literal 0xccccc, which is 40% of the stock 2 MB ring, so
// expressing it as a fraction reproduces upstream exactly and keeps the two in step if the ring
// is ever resized. Apple's own `ccpipe:DriverLogs` boot-arg overrides the real allocation from
// outside without touching this struct, which is the right way to test a different size.
#define ITLWM_CC_PIPE_SIZE          0x200000ULL
#define ITLWM_CC_NOTIFY(pipeSize)   ((pipeSize) * 2 / 5)

bool AirportItlwm::
initCCLogs()
{
    CCPipeOptions driverLogOptions = { 0 };
    driverLogOptions.pipe_type = 0;
    driverLogOptions.log_data_type = 1;
    driverLogOptions.pipe_size = ITLWM_CC_PIPE_SIZE;
    driverLogOptions.min_log_size_notify = ITLWM_CC_NOTIFY(ITLWM_CC_PIPE_SIZE);
    driverLogOptions.notify_threshold = 1000;
    strlcpy(driverLogOptions.file_name, "Itlwm_Logs", sizeof(driverLogOptions.file_name));
    snprintf(driverLogOptions.name, sizeof(driverLogOptions.name), "wlan%d", 0);
    strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
    driverLogOptions.pad9 = 0x1000000;
    driverLogOptions.pad10 = 2;
    driverLogOptions.file_options = 0;
    driverLogOptions.log_policy = 0;
    driverLogPipe = CCPipe::withOwnerNameCapacity(this, "com.zxystd.AirportItlwm", "DriverLogs", &driverLogOptions);
    XYLog("%s driverLogPipeRet %d\n", __FUNCTION__, driverLogPipe != NULL);
    
    memset(&driverLogOptions, 0, sizeof(driverLogOptions));
    driverLogOptions.pipe_type = 0;
    driverLogOptions.log_data_type = 0;
    driverLogOptions.pipe_size = ITLWM_CC_PIPE_SIZE;
    driverLogOptions.min_log_size_notify = ITLWM_CC_NOTIFY(ITLWM_CC_PIPE_SIZE);
    driverLogOptions.notify_threshold = 1000;
    strlcpy(driverLogOptions.file_name, "AppleBCMWLAN_Datapath", sizeof(driverLogOptions.file_name));
    strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
    driverLogOptions.pad9 = HIGHER32(0x202800000);
    driverLogOptions.pad10 = LOWER32(0x202800000);
    driverLogOptions.file_options = 0;
    driverLogOptions.log_policy = 0;
    driverDataPathPipe = CCPipe::withOwnerNameCapacity(this, "com.zxystd.AirportItlwm", "DatapathEvents", &driverLogOptions);
    XYLog("%s driverDataPathPipeRet %d\n", __FUNCTION__, driverDataPathPipe != NULL);

    // The snapshots pipe and the fault-reporter chain are NOT optional on Tahoe.
    // getFaultReporterFromDriver() returning NULL is not a clean start()-returns-false: Apple
    // panics on it at findAndAttachToFaultReporter+0x10f. getLogger() is checked earlier, at
    // IO80211Controller::start+0x17e, which is why a driver with no CoreCapture at all fails
    // cleanly while one with a partial chain does not.
    {
        memset(&driverLogOptions, 0, sizeof(driverLogOptions));
        driverLogOptions.pipe_type = 0x200000001;
        driverLogOptions.log_data_type = 2;
        strlcpy(driverLogOptions.file_name, "StateSnapshots", sizeof(driverLogOptions.file_name));
        strlcpy(driverLogOptions.name, "0", sizeof(driverLogOptions.name));
        strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
        driverLogOptions.pipe_size = 128;
        driverSnapshotsPipe = CCPipe::withOwnerNameCapacity(this, "com.zxystd.AirportItlwm", "StateSnapshots", &driverLogOptions);
        XYLog("%s driverSnapshotsPipeRet %d\n", __FUNCTION__, driverSnapshotsPipe != NULL);

        CCStreamOptions faultReportOptions = { 0 };
        faultReportOptions.stream_type = 1;
        faultReportOptions.console_level = 0xFFFFFFFFFFFFFFFF;
#if __IO80211_TARGET >= __MAC_26_0
        // The fault reporter is three objects, not one, and slot 432 must hand Apple the
        // top of the chain. This used to return the CCStream directly, which Apple stored
        // as its CommonFaultReporter and then called vtable slot 36 on — landing in
        // IORegistryEntry::copyProperty and page-faulting in
        // IO80211PeerManager::initWithInterface+0x10b5. See IO80211FaultReporter.h.
        //
        // Build bottom-up and check every step: each factory returns NULL if its input is
        // NULL, and a NULL at the top is a panic inside Apple's start(), not a soft failure.
        driverFaultStream = CCDataStream::withPipeAndName(driverSnapshotsPipe, "FaultReporter", &faultReportOptions);
        XYLog("%s driverFaultStreamRet %d\n", __FUNCTION__, driverFaultStream != NULL);

        // Own workloop rather than getWorkLoop(): initCCLogs() runs before super::start(),
        // so IONetworkController::_workLoop does not exist yet and getWorkLoop() is NULL —
        // which CCFaultReporter rejects. It must also be a real IOWorkLoop, so _fWorkloop
        // (an IO80211WorkQueue, and itself only created inside Apple's start) is not a
        // candidate either. CoreCapture retains this and arms a timer on it.
        if (driverFaultStream) {
            driverFaultWorkLoop = IOWorkLoop::workLoop();
            if (driverFaultWorkLoop)
                driverCCFaultReporter = CCFaultReporter::withStreamWorkloop(driverFaultStream,
                                                                            driverFaultWorkLoop);
        }
        XYLog("%s driverCCFaultReporterRet %d\n", __FUNCTION__, driverCCFaultReporter != NULL);

        if (driverCCFaultReporter)
            driverFaultReporter = IO80211FaultReporter::allocWithParams(driverCCFaultReporter);
#else
        driverFaultReporter = CCStream::withPipeAndName(driverSnapshotsPipe, "FaultReporter", &faultReportOptions);
#endif
        XYLog("%s driverFaultReporterRet %d\n", __FUNCTION__, driverFaultReporter != NULL);
    }
#if __IO80211_TARGET >= __MAC_26_0
    CCStreamOptions loggerOptions = { 0 };
    loggerOptions.stream_type = 0;
    loggerOptions.console_level = 0;
    driverLogger = CCStream::withPipeAndName(driverLogPipe, "DriverLogs", &loggerOptions);
    XYLog("%s driverLoggerRet %d\n", __FUNCTION__, driverLogger != NULL);
    if (!driverLogger)
        return false;
#endif
    // driverFaultReporter is required: returning false here is the only way to refuse the
    // start *before* Apple reaches the slot that panics on NULL.
    return driverLogPipe && driverDataPathPipe && driverSnapshotsPipe && driverFaultReporter;
}


#if __IO80211_TARGET >= __MAC_26_0
// Temporary bring-up diagnostic, removed once Tahoe starts cleanly.
//
// This is the ONLY place logging is safe: it runs on our own start() thread, between our
// own statements, so no Apple lock is held. Two channels, because neither alone is enough:
//   - a property on the *provider*, which stays registered even when we fail to attach:
//       ioreg -l -w0 | grep AirportItlwmStage
//   - IOLog, which is the only thing that reaches the verbose boot screen on this
//     machine (XYLog uses kprintf, which reaches neither dmesg nor the unified log).
//     This interleaves with IO80211Family's own IOLog output, so a photographed verbose
//     boot shows exactly where in Apple's start sequence we are.
//
// IOLog goes first, then a spin so the console drains before anything downstream can hang,
// then setProperty. Do NOT copy this pattern into a slot Apple calls — see the note on
// getLogger() in AirportItlwmV2.hpp for what that cost.
#define STAGE(n)  do { \
    IOLog("AirportItlwm: STAGE %d\n", (int)(n)); \
    IODelay(20000); \
    if (provider) provider->setProperty("AirportItlwmStage", (UInt64)(n), 32); \
} while (0)
#else
#define STAGE(n)  do { } while (0)
#endif

bool AirportItlwm::start(IOService *provider)
{
    STAGE(1);
    XYLog("%s\n", __PRETTY_FUNCTION__);
    struct IOSkywalkEthernetInterface::RegistrationInfo registInfo;
    int boot_value = 0;
    
    UInt8 builtIn = 0;
    setProperty("built-in", OSData::withBytes(&builtIn, sizeof(builtIn)));
    setProperty("DriverKitDriver", kOSBooleanFalse);
#if __IO80211_TARGET >= __MAC_26_0
    // Tahoe is the only target that builds the CoreCapture pipes BEFORE super::start(); every
    // earlier release builds them after. It has to, because IO80211ControllerMonitor::
    // initWithControllerAndProvider fails with kIOReturnNoResources when getLogger()
    // (slot 424) returns NULL, and IO80211Controller::start() calls it at +0x17e.
    //
    // Creating a CCPipe this early in boot is what IOPCIEDeviceWrapper's deferred publish
    // exists to move out of the way; see the note there and root AGENTS.md mechanism 7.
    bool ccOk = initCCLogs();
    // Publish whether a ring was actually allocated. AirportItlwmStage cannot answer this:
    // STAGE(2) sits after super::start(), so Stage 1 is ambiguous between "the pipe was
    // rejected" and "the pipe was fine but super::start() failed".
    if (provider)
        provider->setProperty("ItlwmCCPipeOK", (UInt64)(driverLogPipe != NULL), 32);
    if (!ccOk) {
        XYLog("CCLog init fail\n");
        releaseAll();
        return false;
    }
#endif
    if (!super::start(provider)) {
        return false;
    }
    STAGE(2);
#if __IO80211_TARGET >= __MAC_26_0
    startCCPipes();
#endif
    STAGE(3);
    pciNub->setBusMasterEnable(true);
    pciNub->setIOEnable(true);
    pciNub->setMemoryEnable(true);
    pciNub->configWrite8(0x41, 0);
    if (pciNub->requestPowerDomainState(kIOPMPowerOn,
                                        (IOPowerConnection *) getParentEntry(gIOPowerPlane), IOPMLowestState) != IOPMNoErr) {
        super::stop(provider);
        return false;
    }
    STAGE(4);
    if (initPCIPowerManagment(pciNub) == false) {
        super::stop(pciNub);
        return false;
    }
    STAGE(5);
    if (_fWorkloop == NULL) {
        XYLog("No _fWorkloop!!\n");
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    STAGE(6);
    _fCommandGate = IOCommandGate::commandGate(this, (IOCommandGate::Action)AirportItlwm::tsleepHandler);
    if (_fCommandGate == 0) {
        XYLog("No command gate!!\n");
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    _fWorkloop->addEventSource(_fCommandGate);
#if __IO80211_TARGET >= __MAC_26_0
    // Deferred postMessage — must exist before the HAL can raise an interrupt, i.e. before
    // initWithController below. See postMessageSafe() for why nothing may post inline.
    // The retain pairs with the thread_call, not with any single firing: it guarantees `this`
    // outlives a callback that stop() could not cancel.
    fPendingMsgLock = IOSimpleLockAlloc();
    fPostMsgCall = thread_call_allocate(&AirportItlwm::postMsgThreadCall, this);
    if (fPendingMsgLock == NULL || fPostMsgCall == NULL) {
        XYLog("postMessage deferral alloc fail\n");
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    retain();
#endif
    STAGE(7);
    const IONetworkMedium *primaryMedium;
    if (!createMediumTables(&primaryMedium) ||
        !setCurrentMedium(primaryMedium) || !setSelectedMedium(primaryMedium)) {
        XYLog("setup medium fail\n");
        releaseAll();
        return false;
    }
    STAGE(8);
    fHalService->initWithController(this, _fWorkloop, _fCommandGate);
    fHalService->get80211Controller()->ic_event_handler = eventHandler;
    
    if (PE_parse_boot_argn("-novht", &boot_value, sizeof(boot_value)))
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_NOVHT;
    if (PE_parse_boot_argn("-noht40", &boot_value, sizeof(boot_value)))
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_NOHT40;
    
    STAGE(9);
    if (!fHalService->attach(pciNub)) {
        XYLog("attach fail\n");
#if __IO80211_TARGET >= __MAC_26_0
        // Which iwx_preinit exit was taken, and its errno — see ItlIwx.cpp. Published on
        // the provider, which stays registered after we fail and detach. Must happen
        // before releaseAll() tears the HAL down.
        publishPreinitMark(provider, fHalService);
#endif
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    STAGE(10);
    fWatchdogWorkLoop = IOWorkLoop::workLoop();
    if (fWatchdogWorkLoop == NULL) {
        XYLog("init watchdog workloop fail\n");
        fHalService->detach(pciNub);
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    watchdogTimer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &AirportItlwm::watchdogAction));
    if (!watchdogTimer) {
        XYLog("init watchdog fail\n");
        fHalService->detach(pciNub);
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    fWatchdogWorkLoop->addEventSource(watchdogTimer);
    STAGE(11);
    scanSource = IOTimerEventSource::timerEventSource(this, &fakeScanDone);
    _fWorkloop->addEventSource(scanSource);
    scanSource->enable();

    STAGE(12);
    fNetIf = new AirportItlwmSkywalkInterface;
#if __IO80211_TARGET >= __MAC_26_0
    // THE ether_addr IS THE FAMILY'S ONLY SOURCE FOR THE CARD'S REAL MAC, AND PASSING NULL HERE
    // COST A DHCP LEASE. IO80211SkywalkInterface::init copies it into state[0xe4] (skipping the
    // copy when NULL), and IO80211SkywalkInterface::start passes that same field straight to
    // IO80211MacAddressAgent::withOptions as the agent's initial address. Seeded with zeros the
    // agent has nothing to work from and mints a random locally-administered address instead —
    // observed on 26.6 as `updateMacAddress ... mac address changed = <00:00:00:00:00:00> ->
    // <E6:D3:46:D3:CF:FE>` in the CoreCapture interface log, before the per-network private
    // address then replaced that one in turn.
    //
    // fHalService->attach() has already run by here, so ic_myaddr is populated; getHardwareAddress
    // returns failure if it is still all zeros, and NULL is then passed as before.
    //
    // This is NOT the same field as RegistrationInfo+0x108. That one feeds the ifnet's link
    // address; this one feeds the family's MAC agent, and they are seeded from different places.
    ether_addr initMac;
    bool haveInitMac = (getHardwareAddress((IOEthernetAddress *)&initMac) == kIOReturnSuccess);
    // IO80211InfraInterface::init() hides the base overload, so go through the concrete type.
    if (!((AirportItlwmSkywalkInterface *)fNetIf)->init(this, haveInitMac ? &initMac : NULL)) {
#else
    if (!fNetIf->init(this)) {
#endif
        XYLog("Skywalk interface init fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    fNetIf->setInterfaceRole(1);
    fNetIf->setInterfaceId(1);
    
#if __IO80211_TARGET < __MAC_26_0
    if (!initCCLogs()) {
        XYLog("CCLog init fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
#endif
    if (!fNetIf->attach(this)) {
        XYLog("attach to service fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    if (!attachInterface(fNetIf, this)) {
        XYLog("attach to interface fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    // Tahoe puts the BSD ifnet on the Skywalk interface; every earlier release keeps it here.
    // A compile-time constant, not a boot-arg — the staging flags are gone, see the note at
    // ITLWM_REGINFO_MAC_OFFSET in AirportItlwmSkywalkInterface.cpp.
#if __IO80211_TARGET >= __MAC_26_0
    const bool skywalkOwnsBSD = true;
#else
    const bool skywalkOwnsBSD = false;
#endif
    // The `true` is "register this interface", which is what makes IONetworkStack give it a unit
    // and a BSD ifnet. On Tahoe the Skywalk interface owns the ifnet instead, so the
    // ethernet interface is attached but NOT registered: the object stays alive — the HAL, the
    // ioctl paths and the RX tee in its inputPacket all still need it — while publishing no second
    // network interface. Retiring it entirely is a later step; this is the smallest change that
    // stops the two attaches colliding.
    if (!IONetworkController::attachInterface((IONetworkInterface **)&bsdInterface,
                                              !skywalkOwnsBSD)) {
        XYLog("attach to IONetworkController interface fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    memset(&registInfo, 0, sizeof(registInfo));
    if (!fNetIf->initRegistrationInfo(&registInfo, 1, sizeof(registInfo))) {
        XYLog("initRegistrationInfo fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    // Upstream calls this twice with identical arguments on the same stack object. Left as-is
    // deliberately: it predates the Tahoe work, it is idempotent, and every V2 release ships it.
    // Root AGENTS.md mechanism 11.
    if (!fNetIf->initRegistrationInfo(&registInfo, 1, sizeof(registInfo))) {
        XYLog("initRegistrationInfo fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
#if __IO80211_TARGET >= __MAC_26_0
    // fRegistrationInfo is Apple's to allocate. registerNetworkInterface creates it with
    // IOMallocType and deregisterNetworkInterface releases it with IOFreeType; every path
    // that touches it — both free() overrides and deregisterNetworkInterface — tests it for
    // NULL first, so leaving it unset is a state the family handles.
    //
    // Hand-allocating it panicked Tahoe two ways at once. IOMalloc lands the block in
    // kalloc.type.var4.*, while IOFreeType demands the data-heap zone belonging to Apple's
    // type ("not in the expected zone early.kalloc.288, but found in kalloc.type.var4.384").
    // And the sizes were wrong: IOSkywalkNetworkInterface::RegistrationInfo is 264 bytes,
    // but registInfo is the *Ethernet* type at 304, so both memcpys copied 304 bytes —
    // 40 past the end of the network one. Matching Apple's allocator would mean declaring a
    // kalloc_type_view by hand, which is XNU-internal and in no SDK here.
#else
    fNetIf->mExpansionData->fRegistrationInfo = (struct IOSkywalkNetworkInterface::RegistrationInfo *)IOMalloc(sizeof(struct IOSkywalkNetworkInterface::RegistrationInfo));
    fNetIf->mExpansionData2->fRegistrationInfo = (struct IOSkywalkEthernetInterface::RegistrationInfo *)IOMalloc(sizeof(struct IOSkywalkEthernetInterface::RegistrationInfo));
    memcpy(fNetIf->mExpansionData->fRegistrationInfo, &registInfo, sizeof(registInfo));
    memcpy(fNetIf->mExpansionData2->fRegistrationInfo, &registInfo, sizeof(registInfo));
#endif
    // deferBSDAttach(true) sets the IOKit property "IODeferBSDAttach", whose only reader in the
    // whole collection is IOSkywalkNetworkBSDClient::start — which returns false when it is
    // present, so no BSD client, no netif nexus, and the Skywalk queues are never driven. That is
    // exactly what we want while AirportItlwmEthernetInterface owns the ifnet, and exactly what
    // must not be set when the Skywalk interface owns it.
    //
    // It is read ONCE, at the client's start(); there is no notification and no retry, so this
    // cannot be cleared later — it has to be absent before matching.
    if (fNetIf->getInterfaceRole() == 1 && !skywalkOwnsBSD)
        fNetIf->deferBSDAttach(true);
#if __IO80211_TARGET >= __MAC_26_0
    // Tahoe only, and deliberately not extended to the releases that already ship without it.
    //
    // A false here is fatal. IO80211SkywalkInterface::start is what allocates the per-interface
    // state block that the family then dereferences unchecked — createEventPipe and
    // destroyEventPipe reach state[0xa8] with no NULL test, so the first IO80211APIUserClient a
    // half-started interface sees takes the machine down. This driver used to carry hand-written
    // raw-offset NULL guards on both of those (root AGENTS.md mechanism 2); checking the return
    // here is what replaced them, so do not weaken it back to a bare call without restoring them.
    //
    // Measured 1 on every booting Tahoe build, so the check is inert on the working path.
    if (!fNetIf->start(this)) {
        XYLog("Skywalk interface start fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
#else
    fNetIf->start(this);
#endif

#if __IO80211_TARGET >= __MAC_26_0
    // Replace the two jobs AirportItlwmEthernetInterface::attachToDataLinkLayer used to do for the
    // Skywalk interface and no longer can, because on Tahoe that interface is attached
    // unregistered and attachToDataLinkLayer never runs.
    //
    // Measured on the first boot that retired the legacy attach: without these the Skywalk node sat at
    // `!registered, !matched` with `IOMACAddress = <000000000000>`, so IOKit never even ran
    // matching for IOSkywalkNetworkBSDClient — every precondition inside its start() was
    // irrelevant because start() was never called. Apple's registerNetworkInterface does NOT
    // registerService for you; it only sets properties (slot 80 is setProperty, not
    // registerService), so the driver has to.
    if (skywalkOwnsBSD) {
        IOEthernetAddress addr;
        if (getHardwareAddress(&addr) == kIOReturnSuccess)
            fNetIf->setProperty(kIOMACAddress, (void *)&addr, kIOEthernetAddressSize);
        fNetIf->registerService();
    }
#endif

    setLinkStatus(kIONetworkLinkValid);
    if (TAILQ_EMPTY(&fHalService->get80211Controller()->ic_ess))
        fHalService->get80211Controller()->ic_flags |= IEEE80211_F_AUTO_JOIN;
    registerService();
    return true;
}

void AirportItlwm::stop(IOService *provider)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);XYLog("%s\n", __PRETTY_FUNCTION__);
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    super::stop(provider);
    disableAdapter(bsdInterface);
    setLinkStatus(kIONetworkLinkValid);
    fHalService->detach(pciNub);
    ether_ifdetach(ifp);
    detachInterface(fNetIf, true);
    OSSafeReleaseNULL(fNetIf);
    releaseAll();
}

void AirportItlwm::free()
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    if (fHalService != NULL) {
        fHalService->release();
        fHalService = NULL;
    }
    if (syncFrameTemplate != NULL && syncFrameTemplateLength > 0) {
        IOFree(syncFrameTemplate, syncFrameTemplateLength);
        syncFrameTemplateLength = 0;
        syncFrameTemplate = NULL;
    }
    if (roamProfile != NULL) {
        IOFree(roamProfile, sizeof(struct apple80211_roam_profile_band_data));
        roamProfile = NULL;
    }
    if (btcProfile != NULL) {
        IOFree(btcProfile, sizeof(struct apple80211_btc_profiles_data));
        btcProfile = NULL;
    }
    super::free();
}

bool AirportItlwm::createWorkQueue()
{
    XYLog("%s %d\n", __FUNCTION__, _fWorkloop != 0);
    return _fWorkloop != 0;
}

#if __IO80211_TARGET >= __MAC_26_0
IO80211WorkQueue *AirportItlwm::getWorkQueue() const
{
    // No instrumentation here — see the note on getLogger() in AirportItlwmV2.hpp.
    // Apple calls this slot from many sites during start(), in lock contexts we do not
    // control, so logging or spinning here risks wedging the boot.
    return _fWorkloop;
}
#else
IO80211WorkQueue *AirportItlwm::getWorkQueue()
{
    return _fWorkloop;
}
#endif

void *AirportItlwm::getFaultReporterFromDriver()
{
    return driverFaultReporter;
}

#if __IO80211_TARGET < __MAC_26_0
IOReturn AirportItlwm::enable(IO80211SkywalkInterface *netif)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    super::enable(netif);
    _fCommandGate->enable();
    if (power_state)
        enableAdapter(bsdInterface);
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::disable(IO80211SkywalkInterface *netif)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    // TEMPORARY instrumentation (mechanism 9). This is the *other* way a link-down reaches the
    // WCL, and it is indistinguishable from net80211's at the point it is posted: it drops the
    // link while ic_state is whatever net80211 last set, so it reads as RUN during an association
    // and looks exactly like a spurious transition. Non-zero here means the family disabled the
    // interface and the teardown is not net80211's doing at all.
#if __IO80211_TARGET >= __MAC_26_0
    gItlwmDisableCalls++;
#endif
    super::disable(netif);
    setLinkStatus(kIONetworkLinkValid);
    return kIOReturnSuccess;
}
#endif

bool AirportItlwm::configureInterface(IONetworkInterface *netif)
{
    IONetworkData *nd;
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    
    if (super::configureInterface(netif) == false) {
        XYLog("super failed\n");
        return false;
    }
    
    nd = netif->getParameter(kIONetworkStatsKey);
    if (!nd || !(fpNetStats = (IONetworkStats *)nd->getBuffer())) {
        XYLog("network statistics buffer unavailable?\n");
        return false;
    }
    ifp->netStat = fpNetStats;
    ether_ifattach(ifp, OSDynamicCast(IOEthernetInterface, netif));
    fpNetStats->collisions = 0;
#ifdef __PRIVATE_SPI__
    netif->configureOutputPullModel(fHalService->getDriverInfo()->getTxQueueSize(), 0, 0, IOEthernetInterface::kOutputPacketSchedulingModelNormal, 0);
#endif
    
    return true;
}

IONetworkInterface *AirportItlwm::createInterface()
{
    AirportItlwmEthernetInterface *netif = new AirportItlwmEthernetInterface;
    if (!netif)
        return NULL;
    if (!netif->initWithSkywalkInterfaceAndProvider(this, fNetIf)) {
        netif->release();
        return NULL;
    }
    return netif;
}

bool AirportItlwm::createMediumTables(const IONetworkMedium **primary)
{
    IONetworkMedium    *medium;

    OSDictionary *mediumDict = OSDictionary::withCapacity(2);
    if (mediumDict == NULL) {
        XYLog("Cannot allocate OSDictionary\n");
        return false;
    }
    
    medium = IONetworkMedium::medium(kIOMediumIEEE80211, 54000000);
    IONetworkMedium::addMedium(mediumDict, medium);
    medium->release();
    if (primary) {
        *primary = medium;
    }
    medium = IONetworkMedium::medium(kIOMediumIEEE80211None, 0);
    IONetworkMedium::addMedium(mediumDict, medium);
    medium->release();
    
    bool result = publishMediumDictionary(mediumDict);
    if (!result) {
        XYLog("Cannot publish medium dictionary!\n");
    }

    mediumDict->release();
    return result;
}

IOReturn AirportItlwm::selectMedium(const IONetworkMedium *medium) {
    setSelectedMedium(medium);
    return kIOReturnSuccess;
}

bool AirportItlwm::
setLinkStatus(UInt32 status, const IONetworkMedium * activeMedium, UInt64 speed, OSData * data)
{
    struct _ifnet *ifq = &fHalService->get80211Controller()->ic_ac.ac_if;
    if (status == currentStatus) {
        return true;
    }
    bool ret = super::setLinkStatus(status, activeMedium, speed, data);
    currentStatus = status;
    if (fNetIf) {
        if (status & kIONetworkLinkActive) {
#ifdef __PRIVATE_SPI__
            bsdInterface->startOutputThread();
#endif
            // Deferred on Tahoe: setLinkState reaches IO80211Glue::sendIOUCToWcl, which panics
            // unless the caller is in the interface work queue's gate and off its thread. This
            // runs from net80211 — for a WPA2 join, from ieee80211_recv_4way_msg3 inside
            // iwx_intr — which is that thread. See deferLinkState().
#if __IO80211_TARGET >= __MAC_26_0
            deferLinkState(kIO80211NetworkLinkUp, 0);
#else
            getCommandGate()->runAction(setLinkStateGated, (void *)kIO80211NetworkLinkUp, (void *)0);
#endif
//            fNetIf->setLinkQualityMetric(100);
        } else if (!(status & kIONetworkLinkNoNetworkChange)) {
#ifdef __PRIVATE_SPI__
            bsdInterface->stopOutputThread();
            bsdInterface->flushOutputQueue();
#endif
            ifq_flush(&ifq->if_snd);
            mq_purge(&fHalService->get80211Controller()->ic_mgtq);
#if __IO80211_TARGET >= __MAC_26_0
            // **Sampled here, not in postLinkStatusInd.** This runs synchronously from
            // ieee80211_set_link_state, which ieee80211_newstate calls right after assigning
            // ic_state — so this is the state of the transition that dropped the link. The post
            // itself happens later, on the deferral thread, by which time net80211 has moved on;
            // reading ic_state there recorded the state at *drain* time and answered a different
            // question. Same trap as ITLWM_PREINIT_SNAP: sample where the event is, not where the
            // reporting is.
            gItlwmLinkDownState = fHalService->get80211Controller()->ic_state;
            // The transition itself, (ostate << 16) | (nstate << 8) | mgt, latched by
            // ieee80211_newstate immediately before it reported the link. ic_state alone was
            // ambiguous — it reads RUN both for "entered RUN" and for the AUTH case that restores
            // ostate to stay RUN — and two different readings of it were wrong.
            gItlwmLinkDownPair = gItlwmLastStatePair;
            deferLinkState(kIO80211NetworkLinkDown,
                           fHalService->get80211Controller()->ic_deauth_reason);
#else
            getCommandGate()->runAction(setLinkStateGated, (void *)kIO80211NetworkLinkDown, (void *)fHalService->get80211Controller()->ic_deauth_reason);
#endif
        }
    }
    return ret;
}

// On Tahoe this runs from the deferral ring's thread_call, inside _fWorkloop->runAction() — so
// inGate() is true and onThread() is false, which is what every IO80211 call below requires.
// **Three of them reach IO80211Glue::sendIOUCToWcl**, not just the postMessages: setLinkState goes
// setLinkStateInternal -> updateLinkSpeed -> sendIOUCToWcl, and setRunningState and
// reportLinkStatus are on the same footing. Running any of them on _fWorkloop's own thread panics
// "trying to send on thread panic". Pre-Tahoe keeps the command-gate call: it has no WCL to
// inform, and those targets work.
IOReturn AirportItlwm::
setLinkStateGated(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, target);
    IOReturn ret = that->fNetIf->setLinkState((IO80211LinkState)(uint64_t)arg0, (unsigned int)(uint64_t)arg1);
    that->fNetIf->setRunningState((IO80211LinkState)(uint64_t)arg0 == kIO80211NetworkLinkUp);
#if __IO80211_TARGET >= __MAC_26_0
    // Link up is what "connected" means to the WCL, and it is the right point for either kind of
    // network: net80211 raises it straight out of RUN for an open BSS, and only once the port is
    // valid — i.e. after the four-way handshake — for an RSN one. Posted before the three
    // userspace notifications below, matching Apple's own habit of sending the WCL event first
    // (AppleBCMWLANScanAdapter::scanComplete posts 237 before AppleBCMWLANCore::scanComplete
    // posts 10). No-op unless a WCL join is actually in flight.
    //
    // Only on the way up. A link-down here is routinely the *start* of a join — leaving one
    // network to reach another drives RUN -> SCAN and a link-down with the join already
    // registered — so reporting failure from it would abandon the join it belongs to. Real
    // failures come from the deauth event and from checkJoinProgress.
    // 216 first, then 213. NET_MANAGER treats CONNECT_COMPLETE as `ignore` while it is still in
    // LINK_DOWN, so the reverse order loses the link-up entirely — the shape that left a completed
    // join reporting `status: inactive`. On the way down there is no 213 to order against, but the
    // indication is still owed so the WCL learns the link went away.
    if ((IO80211LinkState)(uint64_t)arg0 == kIO80211NetworkLinkUp) {
        that->postLinkStatusInd(true, 0);
        that->postJoinConnectComplete(0, 0);
    } else {
        that->postLinkStatusInd(false, APPLE80211_LINK_REASON_INTERNAL_DOWN);
    }
    that->postMessageSafe(APPLE80211_M_LINK_CHANGED, NULL, 0);
    that->postMessageSafe(APPLE80211_M_BSSID_CHANGED, NULL, 0);
    that->postMessageSafe(APPLE80211_M_SSID_CHANGED, NULL, 0);
#else
    that->fNetIf->postMessage(APPLE80211_M_LINK_CHANGED, NULL, 0, ITLWM_POSTMSG_ASYNC);
    that->fNetIf->postMessage(APPLE80211_M_BSSID_CHANGED, NULL, 0, ITLWM_POSTMSG_ASYNC);
    that->fNetIf->postMessage(APPLE80211_M_SSID_CHANGED, NULL, 0, ITLWM_POSTMSG_ASYNC);
#endif
    if ((IO80211LinkState)(uint64_t)arg0 == kIO80211NetworkLinkUp) {
        that->fNetIf->reportLinkStatus(3, 0x80);
    } else {
        that->fNetIf->reportLinkStatus(1, 0);
    }
    that->bsdInterface->setLinkState((IO80211LinkState)(uint64_t)arg0);
    return ret;
}

#ifdef __PRIVATE_SPI__
IOReturn AirportItlwm::outputStart(IONetworkInterface *interface, IOOptionBits options)
{
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    mbuf_t m = NULL;
    if (ifq_is_oactive(&ifp->if_snd))
        return kIOReturnNoResources;
    while (kIOReturnSuccess == interface->dequeueOutputPackets(1, &m)) {
        if (outputPacket(m, NULL)!= kIOReturnOutputSuccess ||
            ifq_is_oactive(&ifp->if_snd))
            return kIOReturnNoResources;
    }
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::networkInterfaceNotification(
                    IONetworkInterface * interface,
                    uint32_t              type,
                    void *                  argument )
{
    XYLog("%s\n", __FUNCTION__);
    return kIOReturnSuccess;
}
#endif

extern const char* hexdump(uint8_t *buf, size_t len);

UInt32 AirportItlwm::outputPacket(mbuf_t m, void *param)
{
//    XYLog("%s\n", __FUNCTION__);
    IOReturn ret = kIOReturnOutputSuccess;
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    
    if (fHalService->get80211Controller()->ic_state != IEEE80211_S_RUN || ifp->if_snd.queue == NULL) {
        if (m && mbuf_type(m) != MBUF_TYPE_FREE)
            freePacket(m);
        return kIOReturnOutputDropped;
    }
    if (m == NULL) {
        XYLog("%s m==NULL!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        ret = kIOReturnOutputDropped;
    }
    if (!(mbuf_flags(m) & MBUF_PKTHDR) ){
        XYLog("%s pkthdr is NULL!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        freePacket(m);
        ret = kIOReturnOutputDropped;
    }
    if (mbuf_type(m) == MBUF_TYPE_FREE) {
        XYLog("%s mbuf is FREE!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        ret = kIOReturnOutputDropped;
    }
    size_t len = mbuf_len(m);
    ether_header_t *eh = (ether_header_t *)mbuf_data(m);
    if (len >= sizeof(ether_header_t) && eh->ether_type == htons(ETHERTYPE_PAE)) { // EAPOL packet
        const char* dump = hexdump((uint8_t*)mbuf_data(m), len);
        XYLog("output EAPOL packet, len: %zu, data: %s\n", len, dump ? dump : "Failed to allocate memory");
        if (dump)
            IOFree((void*)dump, 3 * len + 1);
    }
    if (!ifp->if_snd.queue->lockEnqueue(m)) {
        freePacket(m);
        ret = kIOReturnOutputDropped;
    }
    (*ifp->if_start)(ifp);
    return ret;
}

const OSString * AirportItlwm::newVendorString() const
{
    return OSString::withCString("Apple");
}

const OSString * AirportItlwm::newModelString() const
{
    return OSString::withCString(fHalService->getDriverInfo()->getFirmwareName());
}

IOReturn AirportItlwm::getHardwareAddress(IOEthernetAddress *addrP)
{
    if (IEEE80211_ADDR_EQ(etheranyaddr, fHalService->get80211Controller()->ic_myaddr))
        return kIOReturnError;
    else {
        IEEE80211_ADDR_COPY(addrP, fHalService->get80211Controller()->ic_myaddr);
        return kIOReturnSuccess;
    }
}

IOReturn AirportItlwm::setHardwareAddress(const void *addrP, UInt32 addrBytes)
{
    if (!fNetIf || !addrP)
        return kIOReturnError;
    if_setlladdr(&fHalService->get80211Controller()->ic_ac.ac_if, (const UInt8 *)addrP);
    if (fHalService->get80211Controller()->ic_state > IEEE80211_S_INIT) {
        fHalService->disable(bsdInterface);
        fHalService->enable(bsdInterface);
    }
    return kIOReturnSuccess;
}

UInt32 AirportItlwm::getFeatures() const
{
    return fHalService->getDriverInfo()->supportedFeatures();
}

IOReturn AirportItlwm::setPromiscuousMode(IOEnetPromiscuousMode mode)
{
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::setMulticastMode(IOEnetMulticastMode mode)
{
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::setMulticastList(IOEthernetAddress* addr, UInt32 len)
{
    return fHalService->getDriverController()->setMulticastList(addr, len);
}

IOReturn AirportItlwm::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
    IOReturn    rtn = kIOReturnSuccess;
    if (group == gIOEthernetWakeOnLANFilterGroup && magicPacketSupported)
        *filters = kIOEthernetWakeOnMagicPacket;
    else if (group == gIONetworkFilterGroup)
        *filters = kIOPacketFilterMulticast | kIOPacketFilterPromiscuous;
    else
        rtn = IOEthernetController::getPacketFilters(group, filters);
    return rtn;
}

SInt32 AirportItlwm::
enableFeature(IO80211FeatureCode code, void *data)
{
    if (code == kIO80211Feature80211n) {
        return 0;
    }
    return 102;
}

#if __IO80211_TARGET >= __MAC_26_0
// Run each pipe's IOService::start, which is the step `CCPipe::withOwnerNameCapacity` does not do.
//
// **This is a panic fix, not tidiness.** The factory only calls `initWithOwnerNameCapacity`
// (vtable slot 281); `CCDataPipe::start` is what allocates the pipe's notify timer into
// `state[0x38]`. `CCDataPipe::enqueueBlob` dereferences that timer **unchecked** on the path taken
// when no client is draining the pipe — it sets the "notify pending" bit, drops the lock and arms
// the timer for 1000 ms — so the first blob written with nothing listening is a NULL deref. That
// is what panicked CoreCapture from `CCFaultReporter::completeReport`, ~150 s into a session, the
// first time a fault report was actually raised.
//
// The registry names the omission directly: our three pipes read `!registered, !matched` while the
// three the IO80211 family creates for itself, on the same AirportItlwm node, read
// `registered, matched`. Same class, same parent, different lifecycle.
//
// **Deliberately after `super::start()`**, not beside the creation in initCCLogs(): starting a
// pipe adds an event source to a work loop, and this is the driver where a single CCPipe created
// before `super::start()` hangs the boot reliably (see IOPCIEDeviceWrapper). Creation stays where
// it has to be; only the start moves late, where a work loop exists and Apple's own start has
// already run. Nothing needs the timer before then — it is only touched on enqueue.
void AirportItlwm::startCCPipes()
{
    CCPipe *pipes[3] = { driverLogPipe, driverDataPathPipe, driverSnapshotsPipe };

    for (uint32_t i = 0; i < 3; i++) {
        if (pipes[i] == NULL)
            continue;
        if (pipes[i]->start(this)) {
            pipes[i]->registerService();
            gItlwmCCPipesStarted++;
        } else {
            // Not fatal by itself: a pipe that never starts behaves exactly as before this fix,
            // which is to say it panics the next time something writes to it with no client
            // attached. Counted so that stays visible rather than becoming a surprise.
            gItlwmCCPipeStartFail++;
        }
    }
}
#endif

bool AirportItlwm::getLogPipes(CCPipe**logPipe, CCPipe**eventPipe, CCPipe**snapshotsPipe)
{
#if __IO80211_TARGET >= __MAC_26_0
#endif
    bool ret = false;
    if (logPipe) {
        *logPipe = driverLogPipe;
        ret = true;
    }
    if (eventPipe) {
        *eventPipe = driverDataPathPipe;
        ret = true;
    }
    if (snapshotsPipe) {
        *snapshotsPipe = driverSnapshotsPipe;
        ret = true;
    }
    return ret;
}

#define APPLE80211_CAPA_AWDL_FEATURE_AUTO_UNLOCK    0x00000004
#define APPLE80211_CAPA_AWDL_FEATURE_WOW            0x00000080

IOReturn AirportItlwm::
getCARD_CAPABILITIES(OSObject *object,
                                     struct apple80211_capability_data *cd)
{
    uint32_t caps = fHalService->get80211Controller()->ic_caps;
    memset(cd, 0, sizeof(struct apple80211_capability_data));
    
    if (caps & IEEE80211_C_WEP)
        cd->capabilities[0] |= 1 << APPLE80211_CAP_WEP;
    if (caps & IEEE80211_C_RSN)
        cd->capabilities[0] |= 1 << APPLE80211_CAP_TKIP | 1 << APPLE80211_CAP_AES_CCM;
    // Disable not implemented capabilities
    // if (caps & IEEE80211_C_PMGT)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_PMGT;
    // if (caps & IEEE80211_C_IBSS)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_IBSS;
    // if (caps & IEEE80211_C_HOSTAP)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_HOSTAP;
    // AES not enabled, like on Apple cards
    
    if (caps & IEEE80211_C_SHSLOT)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_SHSLOT - 8);
    if (caps & IEEE80211_C_SHPREAMBLE)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_SHPREAMBLE - 8);
    if (caps & IEEE80211_C_RSN)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_WPA1 - 8) | 1 << (APPLE80211_CAP_WPA2 - 8) | 1 << (APPLE80211_CAP_TKIPMIC - 8);
    // Disable not implemented capabilities
    // if (caps & IEEE80211_C_TXPMGT)
    //     cd->capabilities[1] |= 1 << (APPLE80211_CAP_TXPMGT - 8);
    // if (caps & IEEE80211_C_MONITOR)
    //     cd->capabilities[1] |= 1 << (APPLE80211_CAP_MONITOR - 8);
    // WPA not enabled, like on Apple cards

    cd->version = APPLE80211_VERSION;
    cd->capabilities[2] = 0xFF; // BURST, WME, SHORT_GI_40MHZ, SHORT_GI_20MHZ, WOW, TSN, ?, ?
    cd->capabilities[3] = 0x2B;
    cd->capabilities[5] = 0x40;
    cd->capabilities[6] = (
//                           1 |    //MFP capable
                           0x8 |
                           0x4 |
                           0x80
                           );
    *(uint16_t *)&cd->capabilities[8] = 0x201;
//
//    cd->capabilities[2] |= 0x10;
//    cd->capabilities[5] |= 0x1;
//
//    cd->capabilities[2] |= 0x2;
//
//    cd->capabilities[3] |= 0x20;
//
//    cd->capabilities[0] |= 0x80;
//
//    cd->capabilities[3] |= 0x80;
//    cd->capabilities[4] |= 0x4;
//
//    cd->capabilities[4] |= 0x1;
//    cd->capabilities[3] |= 0x1;
//    cd->capabilities[6] |= 0x8;
//
//    cd->capabilities[3] |= 3;
//    cd->capabilities[4] |= 2;
//    cd->capabilities[6] |= 0x10;
//    cd->capabilities[5] |= 0x20;
//    cd->capabilities[5] |= 0x80;
//
//    if (cd->capabilities[6] & 0x20) {
//        cd->capabilities[2] |= 8;
//    }
//    cd->capabilities[5] |= 8;
//    cd->capabilities[8] |= 2;
//
//    cd->capabilities[11] |= (2 | 4 | 8 | 0x10 | 0x20 | 0x40 | 0x80);
    
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
getDRIVER_VERSION(OSObject *object,
                                  struct apple80211_version_data *hv)
{
    if (!hv)
        return kIOReturnError;
    hv->version = APPLE80211_VERSION;
    snprintf(hv->string, sizeof(hv->string), "itlwm: %s%s fw: %s", ITLWM_VERSION, GIT_COMMIT, fHalService->getDriverInfo()->getFirmwareVersion());
    hv->string_len = strlen(hv->string);
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
getHARDWARE_VERSION(OSObject *object,
                                    struct apple80211_version_data *hv)
{
    if (!hv)
        return kIOReturnError;
    hv->version = APPLE80211_VERSION;
    strncpy(hv->string, fHalService->getDriverInfo()->getFirmwareVersion(), sizeof(hv->string));
    hv->string_len = strlen(fHalService->getDriverInfo()->getFirmwareVersion());
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
getCOUNTRY_CODE(OSObject *object,
                                struct apple80211_country_code_data *cd)
{
    char user_override_cc[3];
    const char *cc_fw = fHalService->getDriverInfo()->getFirmwareCountryCode();
    
    if (!cd)
        return kIOReturnError;
    cd->version = APPLE80211_VERSION;
    memset(user_override_cc, 0, sizeof(user_override_cc));
    PE_parse_boot_argn("itlwm_cc", user_override_cc, 3);
    /* user_override_cc > firmware_cc > geo_location_cc */
    strncpy((char*)cd->cc, user_override_cc[0] ? user_override_cc : ((cc_fw[0] == 'Z' && cc_fw[1] == 'Z' && geo_location_cc[0]) ? geo_location_cc : cc_fw), sizeof(cd->cc));
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
setCOUNTRY_CODE(OSObject *object, struct apple80211_country_code_data *data)
{
    XYLog("%s cc=%s\n", __FUNCTION__, data->cc);
    if (data && data->cc[0] != 120 && data->cc[0] != 88) {
        memcpy(geo_location_cc, data->cc, sizeof(geo_location_cc));
        // airportd's ioctl thread with the gate open — inGate() would be false. Deferred.
#if __IO80211_TARGET >= __MAC_26_0
        postMessageSafe(APPLE80211_M_COUNTRY_CODE_CHANGED, NULL, 0);
#else
        fNetIf->postMessage(APPLE80211_M_COUNTRY_CODE_CHANGED, NULL, 0, ITLWM_POSTMSG_ASYNC);
#endif
    }
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
getPOWER(OSObject *object,
                         struct apple80211_power_data *pd)
{
    if (!pd)
        return kIOReturnError;
    pd->version = APPLE80211_VERSION;
    pd->num_radios = 4;
    pd->power_state[0] = power_state;
    pd->power_state[1] = power_state;
    pd->power_state[2] = power_state;
    pd->power_state[3] = power_state;
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
setPOWER(OSObject *object,
                         struct apple80211_power_data *pd)
{
    if (!pd)
        return kIOReturnError;
    IOLog("itlwm: setPOWER: num_radios[%d]  power_state(0:%u  1:%u  2:%u  3:%u)\n", pd->num_radios, pd->power_state[0], pd->power_state[1], pd->power_state[2], pd->power_state[3]);
    if (pd->num_radios > 0) {
        bool isRunning = (fHalService->get80211Controller()->ic_ac.ac_if.if_flags & (IFF_UP | IFF_RUNNING)) != 0;
        if (pd->power_state[0] == 0) {
            changePowerStateToPriv(1);
            if (isRunning) {
                net80211_ifstats(fHalService->get80211Controller());
                disableAdapter(bsdInterface);
            }
        } else {
            changePowerStateToPriv(2);
            if (!isRunning)
                enableAdapter(bsdInterface);
        }
        power_state = (pd->power_state[0]);
    }
    
    return kIOReturnSuccess;
}

#if __IO80211_TARGET < __MAC_26_0
#if __IO80211_TARGET < __MAC_26_0
SInt32 AirportItlwm::apple80211_ioctl(IO80211SkywalkInterface *interface,unsigned long cmd,void *data, bool b1, bool b2)
{
    if (!ml_at_interrupt_context())
        XYLog("%s cmd: %s b1: %d b2: %d\n", __FUNCTION__, convertApple80211IOCTLToString((unsigned int)cmd), b1, b2);
    return super::apple80211_ioctl(interface, cmd, data, b1, b2);
}
#endif
#endif

#if __IO80211_TARGET < __MAC_26_0
#if __IO80211_TARGET < __MAC_26_0
SInt32 AirportItlwm::apple80211SkywalkRequest(UInt request,int cmd,IO80211SkywalkInterface *interface,void *data)
{
    if (!ml_at_interrupt_context())
        XYLog("%s 1 cmd: %s request: %d\n", __FUNCTION__, convertApple80211IOCTLToString(cmd), request);
    return kIOReturnUnsupported;
}
#endif
#endif

#if __IO80211_TARGET < __MAC_26_0
#if __IO80211_TARGET < __MAC_26_0
SInt32 AirportItlwm::apple80211SkywalkRequest(UInt request,int cmd,IO80211SkywalkInterface *interface,void *data,void *)
{
    if (!ml_at_interrupt_context())
        XYLog("%s 2 cmd: %s request: %d\n", __FUNCTION__, convertApple80211IOCTLToString(cmd), request);
    return kIOReturnUnsupported;
}
#endif
#endif

IOReturn AirportItlwm::enableAdapter(IONetworkInterface *netif)
{
    fHalService->enable(netif);
    watchdogTimer->setTimeoutMS(kWatchDogTimerPeriod);
    watchdogTimer->enable();
    return kIOReturnSuccess;
}

void AirportItlwm::disableAdapter(IONetworkInterface *netif)
{
    watchdogTimer->cancelTimeout();
    watchdogTimer->disable();
    fHalService->disable(netif);
}

IOReturn AirportItlwm::
tsleepHandler(OSObject* owner, void* arg0, void* arg1, void* arg2, void* arg3)
{
    AirportItlwm* dev = OSDynamicCast(AirportItlwm, owner);
    if (dev == 0)
        return kIOReturnError;
    
    if (arg1 == 0) {
        if (_fCommandGate->commandSleep(arg0, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED)
            return kIOReturnSuccess;
        else
            return kIOReturnTimeout;
    } else {
        AbsoluteTime deadline;
        clock_interval_to_deadline((*(int*)arg1), kNanosecondScale, reinterpret_cast<uint64_t*> (&deadline));
        if (_fCommandGate->commandSleep(arg0, deadline, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED)
            return kIOReturnSuccess;
        else
            return kIOReturnTimeout;
    }
}

bool AirportItlwm::initPCIPowerManagment(IOPCIDevice *provider)
{
    UInt16 reg16;

    reg16 = provider->configRead16(kIOPCIConfigCommand);

    reg16 |= ( kIOPCICommandBusMaster       |
               kIOPCICommandMemorySpace     |
               kIOPCICommandMemWrInvalidate );

    reg16 &= ~kIOPCICommandIOSpace;  // disable I/O space

    provider->configWrite16( kIOPCIConfigCommand, reg16 );
    provider->findPCICapability(kIOPCIPowerManagementCapability,
                                &pmPCICapPtr);
    if (pmPCICapPtr) {
        UInt16 pciPMCReg = provider->configRead32( pmPCICapPtr ) >> 16;
        if (pciPMCReg & kPCIPMCPMESupportFromD3Cold)
            magicPacketSupported = true;
        provider->configWrite16((pmPCICapPtr + 4), 0x8000 );
        IOSleep(10);
    }
    return true;
}

static IOPMPowerState powerStateArray[kPowerStateCount] =
{
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

void AirportItlwm::unregistPM()
{
    if (powerOffThreadCall) {
        thread_call_free(powerOffThreadCall);
        powerOffThreadCall = NULL;
    }
    if (powerOnThreadCall) {
        thread_call_free(powerOnThreadCall);
        powerOnThreadCall = NULL;
    }
}

IOReturn AirportItlwm::setPowerState(unsigned long powerStateOrdinal, IOService *policyMaker)
{
    IOReturn result = IOPMAckImplied;
    
    if (pmPowerState == powerStateOrdinal)
        return result;
    switch (powerStateOrdinal) {
        case kPowerStateOff:
            if (powerOffThreadCall) {
                retain();
                if (thread_call_enter(powerOffThreadCall))
                    release();
                result = 5000000;
            }
            break;
        case kPowerStateOn:
            if (powerOnThreadCall) {
                retain();
                if (thread_call_enter(powerOnThreadCall))
                    release();
                result = 5000000;
            }
            break;
            
        default:
            break;
    }
    return result;
}

IOReturn AirportItlwm::setWakeOnMagicPacket(bool active)
{
    magicPacketEnabled = active;
    return kIOReturnSuccess;
}

static void handleSetPowerStateOff(thread_call_param_t param0,
                             thread_call_param_t param1)
{
    AirportItlwm *self = (AirportItlwm *)param0;

    if (param1 == 0)
    {
        self->getCommandGate()->runAction((IOCommandGate::Action)
                                           handleSetPowerStateOff,
                                           (void *) 1);
    }
    else
    {
        self->setPowerStateOff();
        self->release();
    }
}

static void handleSetPowerStateOn(thread_call_param_t param0,
                            thread_call_param_t param1)
{
    AirportItlwm *self = (AirportItlwm *) param0;

    if (param1 == 0)
    {
        self->getCommandGate()->runAction((IOCommandGate::Action)
                                           handleSetPowerStateOn,
                                           (void *) 1);
    }
    else
    {
        self->setPowerStateOn();
        self->release();
    }
}

IOReturn AirportItlwm::registerWithPolicyMaker(IOService *policyMaker)
{
    IOReturn ret;
    
    pmPowerState = kPowerStateOn;
    pmPolicyMaker = policyMaker;
    
    powerOffThreadCall = thread_call_allocate(
                                            (thread_call_func_t)handleSetPowerStateOff,
                                            (thread_call_param_t)this);
    powerOnThreadCall  = thread_call_allocate(
                                            (thread_call_func_t)handleSetPowerStateOn,
                                              (thread_call_param_t)this);
    ret = pmPolicyMaker->registerPowerDriver(this,
                                             powerStateArray,
                                             kPowerStateCount);
    return ret;
}

void AirportItlwm::setPowerStateOff()
{
    XYLog("%s\n", __FUNCTION__);
    pmPowerState = kPowerStateOff;
    disableAdapter(bsdInterface);
    pmPolicyMaker->acknowledgeSetPowerState();
}

void AirportItlwm::setPowerStateOn()
{
    XYLog("%s\n", __FUNCTION__);
    pmPowerState = kPowerStateOn;
    pmPolicyMaker->acknowledgeSetPowerState();
}
