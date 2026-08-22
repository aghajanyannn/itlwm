//
//  IO80211SkywalkInterface.h
//  IO80211Family
//
//  Created by 钟先耀 on 2019/10/18.
//  Copyright © 2019 钟先耀. All rights reserved.
//

#ifndef _IO80211SKYWALK_H
#define _IO80211SKYWALK_H

#include <Availability.h>
#include "IOSkywalkEthernetInterface.h"

// This is necessary, because even the latest Xcode does not support properly targeting 11.0.
#ifndef __IO80211_TARGET
#error "Please define __IO80211_TARGET to the requested version"
#endif

class TxSubmissionDequeueStats;
class TxCompletionEnqueueStats;
class IO80211NetworkPacket;
class PacketSkywalkScratch;
#include "IO80211FlowQueueHash.h"
class IO80211Peer;
class CCPipe;
class IO80211APIUserClient;
class IO80211WorkQueue;
struct apple80211_wme_ac;
struct apple80211_interface_availability;
struct apple80211_cca_report;
struct apple80211_stat_report;
struct apple80211_chip_counters_tx;
struct apple80211_chip_counters_rx;
struct apple80211_chip_error_counters_tx;
struct apple80211_ManagementInformationBasedot11_counters;
struct apple80211_lteCoex_report;
struct apple80211_frame_counters;
struct userPrintCtx;
struct apple80211_lqm_summary;
struct apple80211_infra_specific_stats;
#if __IO80211_TARGET >= __MAC_26_0
class IO80211PeerManager;
struct apple80211_rx_data_stall_report;
struct apple80211_data_path_interface_stats;
struct apple80211_data_path_peer_stats;
struct apple80211_latency_all_ac;
struct apple82011_postMessage_dps;
#endif

struct TxPacketRequest {
    uint16_t    unk1;       // 0
    uint16_t    t;       // 2
    uint16_t    mU;       // 4
    uint16_t    mM;       // 6
    uint16_t    pkt_cnt;
    uint16_t    unk2;
    uint16_t    unk3;
    uint16_t    unk4;
    uint32_t    pad;
    mbuf_t      bufs[8];    // 18
    uint32_t    reqTx;
};

static_assert(sizeof(struct TxPacketRequest) == 0x60, "TxPacketRequest size error");

class IO80211SkywalkInterface : public IOSkywalkEthernetInterface {
    OSDeclareAbstractStructors(IO80211SkywalkInterface)

public:
    
    virtual bool init() APPLE_KEXT_OVERRIDE;
    virtual void free() APPLE_KEXT_OVERRIDE;
    virtual IOReturn configureReport(IOReportChannelList *,UInt,void *,void *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn updateReport(IOReportChannelList *,UInt,void *,void *) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setPowerState(
        unsigned long powerStateOrdinal,
        IOService *   whatDevice ) APPLE_KEXT_OVERRIDE;
    virtual unsigned long maxCapabilityForDomainState( IOPMPowerFlags domainState ) APPLE_KEXT_OVERRIDE;
    virtual unsigned long initialPowerStateForDomainState( IOPMPowerFlags domainState ) APPLE_KEXT_OVERRIDE;
    virtual IOReturn enable(UInt) APPLE_KEXT_OVERRIDE;
    virtual IOReturn disable(UInt) APPLE_KEXT_OVERRIDE;
    virtual SInt32 initBSDInterfaceParameters(ifnet_init_eparams *,sockaddr_dl **) APPLE_KEXT_OVERRIDE;
    virtual bool prepareBSDInterface(ifnet_t, UInt) APPLE_KEXT_OVERRIDE;
    virtual IOReturn processBSDCommand(ifnet_t, UInt, void *) APPLE_KEXT_OVERRIDE;
    virtual SInt32 setInterfaceEnable(bool) APPLE_KEXT_OVERRIDE;
    virtual SInt32 setRunningState(bool) APPLE_KEXT_OVERRIDE;
    virtual IOReturn handleChosenMedia(UInt) APPLE_KEXT_OVERRIDE;
    virtual void *getSupportedMediaArray(UInt *,UInt *) APPLE_KEXT_OVERRIDE;
    virtual UInt32 getFeatureFlags(void) APPLE_KEXT_OVERRIDE;
    virtual const char *classNameOverride(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setPromiscuousModeEnable(bool, UInt) APPLE_KEXT_OVERRIDE;
    // Slots 333/334, declared by IOSkywalkEthernetInterface and overridden here — this
    // is the class Apple's implementation is attributed to, so the override has to sit
    // on this class for the vtable entry to bind.
    // IOReturn, not void — see the note on the base declaration. Apple's implementation reads
    // getSelfMacAddr() (slot 415) and returns 0; that chain is empty on this driver, which is how
    // the ifnet ended up with a synthesised locally-administered address.
    virtual IOReturn getHardwareAddress(ether_addr *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setHardwareAddress(ether_addr *) APPLE_KEXT_OVERRIDE;
    virtual void *createPeerManager(void);
#if __IO80211_TARGET >= __MAC_26_0
    virtual void *createPeer(unsigned char const*,IO80211PeerManager *);
#endif
    // The trailing bool selects the delivery route, and it is not optional book-keeping.
    // postMessageInternal+0x00 does `flag ^ 1` and, when the flag is TRUE and the glue exists,
    // tail-calls IO80211Glue::routeEventToWcl — addEventToPendingQueue + signalWorkAvailable,
    // with no thread and no gate requirement, safe from an interrupt handler.
    // FALSE takes the synchronous route, which ends in IO80211Glue::sendIOUCToWcl and panics
    // unless inGate() is true and onThread() is false.
    // Apple's own drivers always pass true: AppleBCMWLANCore::postMessageInfra and
    // postLQMEvent both set the flag to 1 before tail-calling IO80211Controller::postMessage.
    // Pass ITLWM_POSTMSG_ASYNC, not a literal.
    virtual void postMessage(UInt,void *,unsigned long,bool);
    virtual IOReturn reportDataPathEvents(UInt,void *,unsigned long,bool);
    virtual IOReturn recordOutputPackets(TxSubmissionDequeueStats *,TxSubmissionDequeueStats *);
    virtual IOReturn recordOutputPacket(apple80211_wme_ac,int,int);
    virtual void logTxPacket(IO80211NetworkPacket *,PacketSkywalkScratch *,apple80211_wme_ac,bool);
#if __IO80211_TARGET >= __MAC_26_0
    virtual void logTxCompletionPacket(IO80211NetworkPacket *,PacketSkywalkScratch *,unsigned char *,apple80211_wme_ac,int,UInt,bool,bool);
#else
    virtual void logTxCompletionPacket(IO80211NetworkPacket *,PacketSkywalkScratch *,unsigned char *,apple80211_wme_ac,int,UInt,bool);
#endif
    virtual IOReturn recordCompletionPackets(TxCompletionEnqueueStats *,TxCompletionEnqueueStats *);
#if __IO80211_TARGET >= __MAC_26_0
    virtual IOReturn inputPacket(IO80211NetworkPacket *,packet_info_tag *,ether_header *,bool *,bool);
#else
    virtual IOReturn inputPacket(IO80211NetworkPacket *,packet_info_tag *,ether_header *,bool *);
#endif
    virtual IOReturn forwardInfraRelayPackets(IO80211NetworkPacket*, ether_header*);
    virtual void logSkywalkTxReqPacket(IO80211NetworkPacket *,PacketSkywalkScratch *,unsigned char *,apple80211_wme_ac,bool);
    virtual SInt64 pendingPackets(unsigned char);
    virtual SInt64 packetSpace(unsigned char);
    virtual bool isChipInterfaceReady(void);
    virtual bool isDebounceOnGoing(void);
#if __IO80211_TARGET < __MAC_26_0
    // Tahoe moved setLinkState down into IO80211InfraInterface, with an extra argument.
    virtual bool setLinkState(IO80211LinkState,UInt,bool debounceTimeout = 30,UInt code = 0);
#endif
    virtual IO80211LinkState linkState(void);
    virtual void setScanningState(UInt,bool,apple80211_scan_data *,int);
    virtual void setDataPathState(bool);
    virtual void *getScanManager(void);
#if __IO80211_TARGET >= __MAC_26_0
    virtual void *getController(void);
#endif
    virtual void updateLinkParameters(apple80211_interface_availability *);
    virtual void updateInterfaceCoexRiskPct(unsigned long long);
    virtual void setLQM(unsigned long long);
    virtual void updateLinkStatus(void);
    virtual void updateLinkStatusGated(void);
    virtual void setInterfaceExtendedCCA(apple80211_channel,apple80211_cca_report *);
    virtual void setInterfaceCCA(apple80211_channel,int);
    virtual void setInterfaceNF(apple80211_channel,long long);
    virtual void setInterfaceOFDMDesense(apple80211_channel,long long);
    virtual void removePacketQueue(IO80211FlowQueueHash *);
    virtual void setDebugFlags(unsigned long long,UInt);
    virtual SInt64 debugFlags(void);
    virtual void setInterfaceChipCounters(apple80211_stat_report *,apple80211_chip_counters_tx *,apple80211_chip_error_counters_tx *,apple80211_chip_counters_rx *);
    virtual void setInterfaceMIBdot11(apple80211_stat_report *,apple80211_ManagementInformationBasedot11_counters *);
    virtual void setFrameStats(apple80211_stat_report *,apple80211_frame_counters *);
#if __IO80211_TARGET >= __MAC_14_4
    virtual void setInfraSpecificFrameStats(apple80211_stat_report *,apple80211_infra_specific_stats *);
#endif
#if __IO80211_TARGET >= __MAC_26_0
    virtual void setRxDataStallStats(apple80211_stat_report *,apple80211_rx_data_stall_report *);
#endif
    virtual SInt64 getWmeTxCounters(unsigned long long *);
#if __IO80211_TARGET < __MAC_26_0
    virtual void setEnabledBySystem(bool);
    virtual bool enabledBySystem(void);
    virtual bool willRoam(ether_addr *,UInt);
#endif
    virtual void setPeerManagerLogFlag(UInt,UInt,UInt);
    virtual void setWoWEnabled(bool);
    virtual bool wowEnabled(void);
    virtual void printDataPath(userPrintCtx *);
#if __IO80211_TARGET >= __MAC_26_0
    virtual UInt64 getDataQueueDepth(void);
#endif
    virtual bool findOrCreateFlowQueue(IO80211FlowQueueHash);
    virtual UInt64 findOrCreateFlowQueueWithCache(IO80211FlowQueueHash,bool *);
    virtual UInt64 findExistingFlowQueue(IO80211FlowQueueHash);
    virtual void removePacketQueue(IO80211FlowQueueHash const*);
    virtual void flushPacketQueues(void);
    virtual void cachePeer(ether_addr *,UInt *);
    virtual bool shouldLog(unsigned long long);
    virtual void vlogDebug(unsigned long long,char const*,va_list);
    virtual void vlogDebugBPF(unsigned long long,char const*,va_list);
#if __IO80211_TARGET >= __MAC_26_0
    virtual UInt64 createLinkQualityMonitor(IO80211Peer *,bool);
#else
    virtual UInt64 createLinkQualityMonitor(IO80211Peer *,IOService *);
#endif
    virtual void releaseLinkQualityMonitor(IO80211Peer *);
    virtual void *getP2PSkywalkPeerMgr(void);
    virtual bool isCommandProhibited(int);
#if __IO80211_TARGET >= __MAC_26_0
    virtual void *findPeer(ether_addr &);
#endif
    virtual void setNotificationProperty(OSSymbol const*,OSObject const*);
    virtual void *getWorkerMatchingDict(OSString *);
#if __IO80211_TARGET >= __MAC_26_0
    virtual bool init(IOService *,ether_addr *);
#else
    virtual bool init(IOService *);
#endif
    virtual bool isInterfaceEnabled(void);

#if __IO80211_TARGET >= __MAC_26_0
    // NOT virtual and NOT in any vtable — an exported symbol only, so declaring it costs no
    // slot (the declaration-only pattern in include/Airport/AGENTS.md; verified absent from
    // every class table in scripts/abi/abi-26.6-25G72.txt).
    //
    // Three instructions on 26.6: state[0xe4] = *(u32 *)mac; state[0xe8] = *(u16 *)(mac+4).
    // It is the SAME field that init(IOService *, ether_addr *) seeds, and that field is
    // IO80211MacAddressAgent's initial address — IO80211SkywalkInterface::start hands
    // state[0xe4] straight to IO80211MacAddressAgent::withOptions. Left at zero the agent mints
    // a random locally-administered address instead, silently. Root AGENTS.md mechanism 21.
    //
    // Needs this+0x120 non-NULL, i.e. it must be called after a successful init().
    void setInitMacAddress(ether_addr &);
#endif
    virtual ether_addr *getSelfMacAddr(void);
#if __IO80211_TARGET < __MAC_26_0
    virtual void setSelfMacAddr(ether_addr *);
#endif
    virtual void *getPacketPool(OSString *);
#if __IO80211_TARGET >= __MAC_26_0
    virtual void *getLogger(void) const;
#else
    virtual void *getLogger(void);
#endif
    virtual IOReturn handleSIOCSIFADDR(void);
    virtual IOReturn debugHandler(apple80211_debug_command *);
    virtual void statsDump(void);
    virtual void powerOnNotification(void);
    virtual void powerOffNotification(void);
    virtual UInt64 getTxQueueDepth(void);
    virtual UInt64 getRxQueueCapacity(void);
    virtual void updateRxCounter(unsigned long long);
    virtual void *getMultiCastQueue(void);
#if __IO80211_TARGET < __MAC_26_0
    virtual void *getCurrentBssid(void);
#endif
    virtual int getAssocState(void);
    virtual void notifyQueueState(apple80211_wme_ac,unsigned short);
    virtual int getTxHeadroom(void);
    virtual void *getRxCompQueue(void);
    virtual void *getTxCompQueue(void);
    virtual void *getTxSubQueue(apple80211_wme_ac);
    virtual void *getTxPacketPool(void);
    virtual void *getRxPacketPool(void);
    virtual void enableDatapath(void);
    virtual void disableDatapath(void);
    virtual int getNumTxQueues(void);
    virtual void *getLQMSummary(apple80211_lqm_summary *);
    virtual int getEventPipeSize(void);
    virtual UInt64 createEventPipe(IO80211APIUserClient *);
    virtual void destroyEventPipe(IO80211APIUserClient *);
#if __IO80211_TARGET >= __MAC_26_0
    virtual IOReturn setUserBufferInfo(IOMemoryDescriptor *,unsigned long long);
#endif
    virtual void postMessageIOUC(char const*,UInt,void *,unsigned long);
    virtual bool isIOUCPipeOpened(void);
    virtual void *getRingMD(IO80211APIUserClient *,unsigned long long);
#if __IO80211_TARGET >= __MAC_26_0
    // Tahoe tail: peer attach/detach, data-path stats and latency accounting.
    virtual IOReturn attachPeer(ether_addr *);
    virtual IOReturn detachPeer(ether_addr *);
    virtual void setDebugTrafficReport(bool);
    virtual IOReturn getDataPathInterfaceStats(apple80211_data_path_interface_stats *);
    virtual IOReturn getDataPathPeerStats(apple80211_data_path_peer_stats *);
    virtual UInt64 getLastQueuePacketTime(ether_addr *);
    virtual UInt64 getLastRxUnicastLinkActivityTime(ether_addr *);
    virtual void updateInterfaceDataStats(apple80211_data_path_interface_stats *);
    virtual void updatePeerDataStats(apple80211_data_path_peer_stats *);
    virtual void logTxLatency(unsigned char *,UInt,unsigned long long);
    virtual void logRxLatency(UInt,unsigned long long);
    virtual void getNClearTxRxLatency(apple80211_latency_all_ac *,apple80211_latency_all_ac *);
    virtual void getLastTxTimeStamp(unsigned long long &);
    virtual void getLastRxTimeStamp(unsigned long long &);
    virtual void syncDPSStats(apple82011_postMessage_dps *);
#endif

public:
    OSString *setInterfaceRole(UInt role);
    void *setInterfaceId(UInt id);
    int getInterfaceRole();
    // Non-virtual, so vtable-neutral, and exported by IO80211Family. This is the accessor for
    // the very field IO80211Glue tests before it will send anything (interface ivars +0x38,
    // filled in IO80211SkywalkInterface::start+0xd5 from the controller's getWorkQueue(),
    // slot 397 — i.e. this driver's _fWorkloop). Use it rather than _fWorkloop directly so the
    // gate closed here cannot diverge from the gate Apple checks.
    IO80211WorkQueue *getWorkQueue();

public:
#if __IO80211_TARGET >= __MAC_26_0
    // Tahoe: sizeof(IO80211SkywalkInterface) == 0x128 over a 0x120 base.
    char _data[0x8];
#else
    char _data[0x118];
#endif
};

#if __IO80211_TARGET >= __MAC_26_0
static_assert(sizeof(IO80211SkywalkInterface) == 0x128, "Invalid class size");
#endif

#endif /* _IO80211SKYWALK_H */
