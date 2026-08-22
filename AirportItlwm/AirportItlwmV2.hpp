//
//  AirportItlwmV2.hpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef AirportItlwmV2_hpp
#define AirportItlwmV2_hpp

#include "Apple80211.h"

#include "IOKit/network/IOGatedOutputQueue.h"
#include <libkern/c++/OSString.h>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOLib.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <IOKit/IOFilterInterruptEventSource.h>

#include "ItlIwm.hpp"
#include "ItlIwx.hpp"
#include "ItlIwn.hpp"

#include "AirportItlwmEthernetInterface.hpp"

enum
{
    kPowerStateOff = 0,
    kPowerStateOn,
    kPowerStateCount
};

#define kWatchDogTimerPeriod 1000

// Delivery route for IO80211SkywalkInterface::postMessage — see the declaration in
// IO80211SkywalkInterface.h for what the flag selects.
//
// true is what Apple's own drivers pass, and it is the only legal value from an interrupt
// handler or a work-loop thread: it routes through IO80211Glue::routeEventToWcl, which just
// enqueues and signals. false takes the synchronous route, which asserts
// inGate() && !onThread() and panics `"trying to send on thread panic"` otherwise — and every
// caller in this driver is on the wrong side of one of those two conditions.
//
// Tahoe only. The pre-Tahoe targets keep passing false because they work: async delivery
// changes the ordering and timing of link/BSSID/SSID notifications, which is a behaviour change
// on shipping kexts for no benefit there, and the panic has only been observed on 26.6.
#if __IO80211_TARGET >= __MAC_26_0
#define ITLWM_POSTMSG_ASYNC  true
#else
#define ITLWM_POSTMSG_ASYNC  false
#endif

extern "C" {
const char *convertApple80211IOCTLToString(signed int cmd);
}

class AirportItlwm : public IO80211Controller {
    OSDeclareDefaultStructors(AirportItlwm)
#define IOCTL(REQ_TYPE, REQ, DATA_TYPE) \
if (REQ_TYPE == SIOCGA80211) { \
ret = get##REQ(interface, (struct DATA_TYPE* )data); \
} else { \
ret = set##REQ(interface, (struct DATA_TYPE* )data); \
}
    
#define IOCTL_GET(REQ_TYPE, REQ, DATA_TYPE) \
if (REQ_TYPE == SIOCGA80211) { \
ret = get##REQ(interface, (struct DATA_TYPE* )data); \
}
#define IOCTL_SET(REQ_TYPE, REQ, DATA_TYPE) \
if (REQ_TYPE == SIOCSA80211) { \
ret = set##REQ(interface, (struct DATA_TYPE* )data); \
}
#define FUNC_IOCTL(REQ, DATA_TYPE) \
FUNC_IOCTL_GET(REQ, DATA_TYPE) \
FUNC_IOCTL_SET(REQ, DATA_TYPE)
#define FUNC_IOCTL_GET(REQ, DATA_TYPE) \
IOReturn get##REQ(OSObject *object, struct DATA_TYPE *data);
#define FUNC_IOCTL_SET(REQ, DATA_TYPE) \
IOReturn set##REQ(OSObject *object, struct DATA_TYPE *data);
    
public:
    virtual bool init(OSDictionary *properties) override;
    virtual void free() override;
    virtual IOService* probe(IOService* provider, SInt32* score) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
#if __IO80211_TARGET < __MAC_26_0
    // Tahoe removed these overloads from IO80211Controller.
    virtual IOReturn enable(IO80211SkywalkInterface *netif) override;
    virtual IOReturn disable(IO80211SkywalkInterface *netif) override;
#endif
    virtual IOReturn setHardwareAddress(const void *addr, UInt32 addrBytes) override;
    virtual IOReturn getHardwareAddress(IOEthernetAddress* addrP) override;
    virtual IOReturn getPacketFilters(const OSSymbol *group, UInt32 *filters) const override;
    virtual IOReturn setPromiscuousMode(IOEnetPromiscuousMode mode) override;
    virtual IOReturn setMulticastMode(IOEnetMulticastMode mode) override;
    virtual IOReturn setMulticastList(IOEthernetAddress* addr, UInt32 len) override;
    virtual UInt32 getFeatures() const override;
    virtual const OSString * newVendorString() const override;
    virtual const OSString * newModelString() const override;
    virtual IOReturn selectMedium(const IONetworkMedium *medium) override;
    virtual bool createWorkQueue() override;
    virtual IONetworkInterface * createInterface() override;
    virtual bool configureInterface(IONetworkInterface *netif) override;
    virtual UInt32 outputPacket(mbuf_t, void * param) override;
#ifdef __PRIVATE_SPI__
    virtual IOReturn outputStart(IONetworkInterface *interface, IOOptionBits options) override;
    virtual IOReturn networkInterfaceNotification(
                        IONetworkInterface * interface,
                        uint32_t              type,
                        void *                  argument ) override;
#endif
    virtual bool setLinkStatus(
                               UInt32                  status,
                               const IONetworkMedium * activeMedium = 0,
                               UInt64                  speed        = 0,
                               OSData *                data         = 0) override;
    static IOReturn setLinkStateGated(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3);
    
    static IOReturn tsleepHandler(OSObject* owner, void* arg0 = 0, void* arg1 = 0, void* arg2 = 0, void* arg3 = 0);
    static void eventHandler(struct ieee80211com *, int, void *);
    IOReturn enableAdapter(IONetworkInterface *netif);
    void disableAdapter(IONetworkInterface *netif);
    bool initCCLogs();
#if __IO80211_TARGET >= __MAC_26_0
    // No instrumentation in this slot, deliberately. getLogger() is called by Apple from
    // inside IO80211Controller::start() and IO80211ControllerMonitor, in lock contexts we
    // do not control. It used to trace here; every form of that was a mistake:
    // setProperty took the global gPropertiesLock, and IOLog takes locks and can block
    // too, while IODelay spun for milliseconds inside those same regions. The result was
    // an intermittent early hang — roughly half of boots died right after this slot was
    // called, which was then wrongly blamed on two unrelated changes.
    //
    // Instrument our own start() thread (STAGE) or record plain integers (see
    // ITLWM_PREINIT_MARK in ItlIwx.cpp). Never log from a slot Apple calls.
    virtual void *getLogger(void) const override {
        return driverLogger;
    }
#endif
    
#if __IO80211_TARGET >= __MAC_26_0
    virtual IO80211WorkQueue *getWorkQueue() const override;
#else
    virtual IO80211WorkQueue *getWorkQueue() override;
#endif
    virtual bool requiresExplicitMBufRelease() override {
        return false;
    }
    virtual bool flowIdSupported() override {
        return false;
    }
    virtual SInt32 monitorModeSetEnabled(bool, UInt) override {
        return kIOReturnSuccess;
    }
    virtual IOReturn requestQueueSizeAndTimeout(unsigned short *queue, unsigned short *timeout) override {
        XYLog("%s\n", __FUNCTION__);
        return kIOReturnSuccess;
    }
    
    virtual bool getLogPipes(CCPipe**, CCPipe**, CCPipe**) override;
    
    virtual void *getFaultReporterFromDriver() override;
    
#if __IO80211_TARGET < __MAC_26_0
    virtual SInt32 apple80211_ioctl(IO80211SkywalkInterface *,unsigned long,void *, bool, bool) override;
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual SInt32 apple80211SkywalkRequest(UInt,int,IO80211SkywalkInterface *,void *) override;
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual SInt32 apple80211SkywalkRequest(UInt,int,IO80211SkywalkInterface *,void *,void *) override;
#endif

    bool createMediumTables(const IONetworkMedium **primary);
    void releaseAll();
    void watchdogAction(IOTimerEventSource *timer);
    
    virtual SInt32 enableFeature(IO80211FeatureCode, void*) override;
    virtual bool isCommandProhibited(int command) override {
//        if (!ml_at_interrupt_context())
//            XYLog("%s %s\n", __FUNCTION__, convertApple80211IOCTLToString(command));
        return false;
    };
    virtual SInt32 handleCardSpecific(IO80211SkywalkInterface *,unsigned long,void *,bool) override {
        XYLog("%s\n", __FUNCTION__);
        return 0;
    };
    virtual IOReturn getDRIVER_VERSION(IO80211SkywalkInterface *interface,apple80211_version_data *data) override {
        XYLog("%s\n", __FUNCTION__);
        return getDRIVER_VERSION((OSObject *)interface, data);
    };
    virtual IOReturn getHARDWARE_VERSION(IO80211SkywalkInterface *interface,apple80211_version_data *data) override {
        XYLog("%s\n", __FUNCTION__);
        return getHARDWARE_VERSION((OSObject *)interface, data);
    };
    virtual IOReturn getCARD_CAPABILITIES(IO80211SkywalkInterface *interface,apple80211_capability_data *data) override {
//        XYLog("%s\n", __FUNCTION__);
        return getCARD_CAPABILITIES((OSObject *)interface, data);
    }
    virtual IOReturn getPOWER(IO80211SkywalkInterface *interface,apple80211_power_data *data) override {
//        XYLog("%s\n", __FUNCTION__);
        return getPOWER((OSObject *)interface, data);
    }
    virtual IOReturn setPOWER(IO80211SkywalkInterface *interface,apple80211_power_data *data) override {
//        XYLog("%s\n", __FUNCTION__);
        return setPOWER((OSObject *)interface, data);
    }
    virtual IOReturn getCOUNTRY_CODE(IO80211SkywalkInterface *interface,apple80211_country_code_data *data) override {
//        XYLog("%s\n", __FUNCTION__);
        return getCOUNTRY_CODE((OSObject *)interface, data);
    }
    virtual IOReturn setCOUNTRY_CODE(IO80211SkywalkInterface *interface,apple80211_country_code_data *data) override {
//        XYLog("%s\n", __FUNCTION__);
        return setCOUNTRY_CODE((OSObject *)interface, data);
    }
    virtual IOReturn setGET_DEBUG_INFO(IO80211SkywalkInterface *interface,apple80211_debug_command *data) override {
        XYLog("%s\n", __FUNCTION__);
        return kIOReturnSuccess;
    }
    
    //scan
    static void fakeScanDone(OSObject *owner, IOTimerEventSource *sender);
    
    //-----------------------------------------------------------------------
    // Power management support.
    //-----------------------------------------------------------------------
    virtual IOReturn registerWithPolicyMaker( IOService * policyMaker ) override;
    virtual IOReturn setPowerState( unsigned long powerStateOrdinal,
                                    IOService *   policyMaker) override;
    virtual IOReturn setWakeOnMagicPacket( bool active ) override;
    void setPowerStateOff(void);
    void setPowerStateOn(void);
#if __IO80211_TARGET >= __MAC_26_0
    // Non-virtual: republishes the bring-up counters from the watchdog, because
    // publishPreinitMark only runs on an attach failure. Not an override, no vtable impact.
    void publishRuntimeCounters(void);
#endif
    void unregistPM();
    bool initPCIPowerManagment(IOPCIDevice *provider);

#if __IO80211_TARGET >= __MAC_26_0
    // Deferred postMessage. Every call must reach IO80211Glue with the interface work queue's
    // gate closed and off its thread; no call site in this driver can do that itself. See
    // postMessageSafe() in the .cpp for the full contract. All non-virtual: no vtable slot.
    // Sized for the largest payload this driver posts: a BeaconMetaData plus its IE list. Most
    // messages are 0 or 4 bytes, so the ring is mostly empty space — but a scan result cannot be
    // posted inline (it arrives on the work-loop thread) and cannot point at the receive mbuf,
    // which is gone by the time the thread_call drains. It has to be copied.
    // 0x44 + BEACON_META_MAX_IE_LEN, spelled out because BeaconMetaData.h is not on this
    // header's include path. Keep the two in step; the .cpp _Static_asserts that they agree.
    static const uint32_t kPendingMsgMaxData = 0x44 + 0x800;
    static const uint32_t kPendingMsgCount = 16;
    // The ring carries two kinds of deferred work, because both end in IO80211Glue::sendIOUCToWcl
    // and both are raised from threads that cannot satisfy its gate/thread contract. Sharing one
    // ring also keeps them ordered: a link-state change and the LINK_CHANGED/BSSID/SSID messages
    // that follow it must reach the family in the order the driver produced them.
    enum {
        kPendingKindMessage = 0,    // postMessage(type, data, len)
        kPendingKindLinkState = 1,  // setLinkState(type = IO80211LinkState, arg = reason)
    };
    struct PendingMsg {
        uint32_t kind;
        uint32_t type;
        uint32_t arg;
        uint32_t len;
        uint8_t  data[kPendingMsgMaxData];
    };

    void postMessageSafe(uint32_t type, const void *data, size_t len);
    void deferLinkState(uint32_t linkState, uint32_t reason);
    bool enqueuePending(const PendingMsg *msg);
    void postScanBeacon(const struct ieee80211_beacon_event *ev);

    // WCL join completion. AirportItlwmSkywalkInterface::setWCL_ASSOCIATE starts the join and
    // calls joinStarted(); the JOIN_MANAGER FSM then sits in IN_PROGRESS until one of these two
    // messages arrives. All non-virtual: no vtable slot. See the definitions.
    void joinStarted(const uint8_t *bssid, uint32_t ssid_len);
    // Must be called after net80211 has actually been programmed, never from joinStarted. See
    // the definition for the failure loop that arming it too early produced.
    void armJoinEssWatch();
    void snapshotJoinFailure();
    void drainStrandedMgmtFrames();
    void postJoinAssocComplete(uint16_t status, uint16_t reason);
    void postJoinConnectComplete(uint16_t status, uint16_t reason);
    // Message 216, to a *different* FSM (WCLNetManager) than the two above, and not gated on a
    // join. Must be posted before postJoinConnectComplete on the way up. See the definition.
    void postLinkStatusInd(bool up, uint32_t reason);
    // Runs each CCPipe's IOService::start, which the CoreCapture factory does not. Must run
    // after super::start(); see the definition for why, and for the panic it fixes.
    void startCCPipes();
    void checkJoinProgress();
    // Message 39, the connection keepalive: without it WCLNetManager tears the network down 60 s
    // after link-up. Called from the watchdog; posts every kLqmPostTicks. See the definition.
    void postLqmUpdate();
    // Watchdog ticks between two message-39 posts. The WCL's deadline is 60 s, so this must stay
    // well under kWatchDogTimerPeriod * this = 60000 ms.
    static const uint32_t kLqmPostTicks = 5;
    // Previous absolute values of the two counters message 39 reports as per-interval deltas, and
    // the tick divider. Watchdog-thread only, so unlocked.
    uint32_t fLqmTick;
    uint32_t fLqmLastBeacons;
    uint32_t fLqmLastRxPackets;

    // The last beacon seen from the BSS this driver is joining or joined, kept in exactly the
    // layout `getWCL_BSS_INFO` (ioctl 433) must return: a BeaconMetaData followed by the IE list.
    // The WCL asks for it at link-up and tears the network down if the answer fails, and it is not
    // reconstructible from an ieee80211_node — the IE list is only ever seen on the wire. Written
    // from postScanBeacon on the net80211/HAL thread, read from the WCL's thread, so both sides go
    // through fPendingMsgLock; kPendingMsgMaxData is already the 0x844 the WCL allocates.
    void cacheScanBeacon(const uint8_t *bssid, const void *msg, size_t len);
    bool copyCurrentBssBeacon(void *out, size_t outLen);
    size_t copyCurrentBssIe(uint8_t id, void *out, size_t outLen);
    // Sized to hold every BSS a join could plausibly pick from. Entries are refreshed in place and
    // the oldest is evicted, so what survives is the set still on the air.
    static const uint32_t kBssBeaconCacheCount = 16;
    struct BssBeaconEntry {
        uint8_t  bssid[6];
        uint32_t len;                       // 0 == slot free
        uint64_t seq;                       // eviction order; monotonic, not a timestamp
        uint8_t  data[kPendingMsgMaxData];  // BeaconMetaData + IEs, the ioctl-433 reply verbatim
    };
    BssBeaconEntry fBssBeacon[kBssBeaconCacheCount];
    uint64_t       fBssBeaconSeq;

    // fJoinPending is written from the work queue (setWCL_ASSOCIATE), the command gate
    // (setLinkStateGated) and the watchdog work loop, with no lock. The race is benign in both
    // directions: a duplicate completion lands in a state that does not accept it and is dropped,
    // and a lost one is caught by the next watchdog tick.
    bool     fJoinPending;
    bool     fJoinWatchEss;
    uint8_t  fJoinBssid[6];
    uint64_t fJoinDeadlineNs;
    // Highest ieee80211_state seen while a join was pending, across all joins this boot.
    uint32_t fJoinMaxState;
    // Same, but for the join currently in flight — it is what extends the stall deadline.
    uint32_t fJoinLastState;
    // First association failure of the boot, latched by snapshotJoinFailure(). Sticky because
    // anything sampled later describes the teardown rather than the failure.
    bool     fJoinFailCaptured;
    uint32_t fJoinFailState;
    uint32_t fJoinFailAssocStatus;
    uint32_t fJoinFailDeauthReason;
    uint32_t fJoinFailRxAuthFail;
    uint32_t fJoinFailNiRsn;        // the AP's, same packing
    uint32_t fJoinFailNiCipher;
    uint32_t fJoinFailNiCaps;       // ni_rsncaps: bit 6 MFPC, bit 7 MFPR
    uint32_t fJoinFailNiFails;
    uint32_t fJoinFailNiAssocFail;  // ieee80211_match_bss's IEEE80211_NODE_ASSOCFAIL_* bitmask
    // First non-zero ic_assoc_status seen during any join: the AP's own status code.
    uint32_t fJoinAssocStatusSeen;
    // What *we* advertise, latched the first time a join gets past SCAN — i.e. while the attempt
    // is still live. The failure snapshot cannot carry these: deselect_ess zeroes them on the way
    // out, and the failure is reported from the branch that observes exactly that.
    bool     fJoinRsnCaptured;
    uint32_t fJoinIcFlags;
    uint32_t fJoinIcRsn;
    uint32_t fJoinIcCipher;
    uint32_t fJoinNiRsnCipher;      // (ni_rsncipher << 16) | ni_rsngroupcipher, as negotiated
    uint32_t fJoinTicksAuth;
    uint32_t fJoinTicksAssoc;
    uint32_t fJoinFailMgtDiscard;
    uint32_t fJoinFailBadRsnIe;
    uint32_t fJoinFailElemBad;
    uint32_t fJoinFailTxNombuf;
    uint32_t fJoinFailIfFlags;
    uint32_t fJoinMgtqMax;
    uint32_t fJoinMgtqStuck;    // watchdog samples during a join with ic_mgtq non-empty
    uint32_t fJoinOactive;      // ... with ifq_is_oactive(if_snd) set
    void drainPendingMessages();
    static void postMsgThreadCall(thread_call_param_t, thread_call_param_t);
    static IOReturn postMsgGated(OSObject *, void *, void *, void *, void *);

    PendingMsg    fPendingMsg[kPendingMsgCount];
    uint32_t      fPendingMsgHead;
    uint32_t      fPendingMsgTail;
    IOSimpleLock *fPendingMsgLock;
    thread_call_t fPostMsgCall;
    bool          fPostMsgArmed;
#endif

    FUNC_IOCTL_GET(CARD_CAPABILITIES, apple80211_capability_data)
    FUNC_IOCTL(POWER, apple80211_power_data)
    FUNC_IOCTL_GET(DRIVER_VERSION, apple80211_version_data)
    FUNC_IOCTL_GET(HARDWARE_VERSION, apple80211_version_data)
    FUNC_IOCTL(COUNTRY_CODE, apple80211_country_code_data)
    
public:
    IOInterruptEventSource* fInterrupt;
    IOTimerEventSource *watchdogTimer;
    IOPCIDevice *pciNub;
    IONetworkStats *fpNetStats;
    AirportItlwmEthernetInterface *bsdInterface;
#if __IO80211_TARGET >= __MAC_26_0
    // Tahoe moved setLinkState down to IO80211InfraInterface; the object is always an
    // AirportItlwmSkywalkInterface, so hold the more derived base here.
    IO80211InfraInterface *fNetIf;
#else
    IO80211SkywalkInterface *fNetIf;
#endif
    IOWorkLoop *fWatchdogWorkLoop;
    ItlHalService *fHalService;
    
    //IO80211
    uint8_t power_state;
    struct ieee80211_node *fNextNodeToSend;
    bool fScanResultWrapping;
    IOTimerEventSource *scanSource;
    
    u_int32_t current_authtype_lower;
    u_int32_t current_authtype_upper;
    UInt64 currentSpeed;
    UInt32 currentStatus;
    bool disassocIsVoluntary;
    char geo_location_cc[3];
    
    //pm
    thread_call_t powerOnThreadCall;
    thread_call_t powerOffThreadCall;
    UInt32 pmPowerState;
    IOService *pmPolicyMaker;
    UInt8 pmPCICapPtr;
    bool magicPacketEnabled;
    bool magicPacketSupported;
    
    //AWDL
    uint8_t *syncFrameTemplate;
    uint32_t syncFrameTemplateLength;
    uint8_t awdlBSSID[6];
    uint32_t awdlSyncState;
    uint32_t awdlElectionId;
    uint32_t awdlPresenceMode;
    uint16_t awdlMasterChannel;
    uint16_t awdlSecondaryMasterChannel;
    uint8_t *roamProfile;
    struct apple80211_btc_profiles_data *btcProfile;
    struct apple80211_btc_config_data btcConfig;
    uint32_t btcMode;
    uint32_t btcOptions;
    bool awdlSyncEnable;
    
    CCPipe *driverLogPipe;
    CCPipe *driverDataPathPipe;
    CCPipe *driverSnapshotsPipe;
    
#if __IO80211_TARGET >= __MAC_26_0
    // The fault-reporter chain, built bottom-up in initCCLogs() and torn down in
    // releaseAll(). Only driverFaultReporter is handed to IO80211Family (slot 432); the two
    // objects below it and the workloop are held solely to own the references. See
    // IO80211FaultReporter.h for why nothing shorter than the full chain will do.
    //
    // Tahoe-only. The pre-Tahoe targets keep returning a bare CCStream from slot 432, which
    // is the same type confusion — but latent there, and those targets work. Changing them
    // would also make the kext depend on three more corecapture/IO80211Family exports whose
    // presence has only been checked on 26.6, which is a load failure if any is missing.
    CCDataStream *driverFaultStream;
    IOWorkLoop *driverFaultWorkLoop;
    CCFaultReporter *driverCCFaultReporter;
    IO80211FaultReporter *driverFaultReporter;

    CCStream *driverLogger;
#else
    CCStream *driverFaultReporter;
#endif
};

#endif /* AirportItlwmV2_hpp */
