//
//  IO80211InfraInterface.h
//  itlwm
//
//  Created by qcwap on 2023/6/12.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef IO80211InfraInterface_h
#define IO80211InfraInterface_h

struct apple80211_wcl_advisory_info;
struct apple80211_wcl_tx_rx_latency;
#if __IO80211_TARGET >= __MAC_26_0
struct apple80211_wcl_update_link_state;
class IOSkywalkPacketQueue;
class IOSkywalkPacketBufferPool;
#endif

class IO80211InfraInterface : public IO80211SkywalkInterface {
    OSDeclareAbstractStructors(IO80211InfraInterface)
    
public:
    virtual bool init() APPLE_KEXT_OVERRIDE;
    virtual void free() APPLE_KEXT_OVERRIDE;
    virtual IOReturn configureReport(IOReportChannelList *,UInt,void *,void *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn updateReport(IOReportChannelList *,UInt,void *,void *) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *) APPLE_KEXT_OVERRIDE;
    virtual SInt32 initBSDInterfaceParameters(ifnet_init_eparams *,sockaddr_dl **) APPLE_KEXT_OVERRIDE;
    virtual bool prepareBSDInterface(ifnet_t, UInt) APPLE_KEXT_OVERRIDE;
    virtual IOReturn processBSDCommand(ifnet_t, UInt, void *) APPLE_KEXT_OVERRIDE;
    virtual SInt32 setInterfaceEnable(bool) APPLE_KEXT_OVERRIDE;
    virtual UInt getHardwareAssists(void) APPLE_KEXT_OVERRIDE;
    virtual bool bpfTap(UInt,UInt) APPLE_KEXT_OVERRIDE;
    // getHardwareAddress/setHardwareAddress (slots 333/334) are deliberately NOT
    // redeclared here: Apple's implementation of both lives on IO80211SkywalkInterface,
    // so declaring them on this class emitted references to
    // IO80211InfraInterface::{get,set}HardwareAddress, which IO80211Family does not
    // export. See IO80211SkywalkInterface.h, which now carries the overrides.
    virtual void postMessage(UInt,void *,unsigned long,bool) APPLE_KEXT_OVERRIDE;
    virtual IOReturn recordOutputPackets(TxSubmissionDequeueStats *,TxSubmissionDequeueStats *) APPLE_KEXT_OVERRIDE;
    virtual void logTxPacket(IO80211NetworkPacket *,PacketSkywalkScratch *,apple80211_wme_ac,bool) APPLE_KEXT_OVERRIDE;
#if __IO80211_TARGET >= __MAC_26_0
    virtual void logTxCompletionPacket(IO80211NetworkPacket *,PacketSkywalkScratch *,unsigned char *,apple80211_wme_ac,int,UInt,bool,bool) APPLE_KEXT_OVERRIDE;
#else
    virtual void logTxCompletionPacket(IO80211NetworkPacket *,PacketSkywalkScratch *,unsigned char *,apple80211_wme_ac,int,UInt,bool) APPLE_KEXT_OVERRIDE;
#endif
    virtual IOReturn recordCompletionPackets(TxCompletionEnqueueStats *,TxCompletionEnqueueStats *) APPLE_KEXT_OVERRIDE;
#if __IO80211_TARGET >= __MAC_26_0
    virtual IOReturn inputPacket(IO80211NetworkPacket *,packet_info_tag *,ether_header *,bool *,bool) APPLE_KEXT_OVERRIDE;
#else
    virtual IOReturn inputPacket(IO80211NetworkPacket *,packet_info_tag *,ether_header *,bool *) APPLE_KEXT_OVERRIDE;
#endif
    virtual SInt64 pendingPackets(unsigned char) APPLE_KEXT_OVERRIDE;
    virtual SInt64 packetSpace(unsigned char) APPLE_KEXT_OVERRIDE;
    virtual bool isDebounceOnGoing(void) APPLE_KEXT_OVERRIDE;
#if __IO80211_TARGET < __MAC_26_0
    virtual bool setLinkState(IO80211LinkState,UInt,bool debounceTimeout = 30,UInt code = 0) APPLE_KEXT_OVERRIDE;
#endif
    virtual IO80211LinkState linkState(void) APPLE_KEXT_OVERRIDE;
    virtual void setScanningState(UInt,bool,apple80211_scan_data *,int) APPLE_KEXT_OVERRIDE;
    virtual void setDataPathState(bool) APPLE_KEXT_OVERRIDE;
    virtual void *getScanManager(void) APPLE_KEXT_OVERRIDE;
    virtual void updateLinkParameters(apple80211_interface_availability *) APPLE_KEXT_OVERRIDE;
    virtual void updateInterfaceCoexRiskPct(unsigned long long) APPLE_KEXT_OVERRIDE;
    virtual void setLQM(unsigned long long) APPLE_KEXT_OVERRIDE;
    virtual void updateLinkStatus(void) APPLE_KEXT_OVERRIDE;
    virtual void updateLinkStatusGated(void) APPLE_KEXT_OVERRIDE;
    virtual void setInterfaceExtendedCCA(apple80211_channel,apple80211_cca_report *) APPLE_KEXT_OVERRIDE;
    virtual void setInterfaceCCA(apple80211_channel,int) APPLE_KEXT_OVERRIDE;
    virtual void setInterfaceNF(apple80211_channel,long long) APPLE_KEXT_OVERRIDE;
    virtual void setInterfaceOFDMDesense(apple80211_channel,long long) APPLE_KEXT_OVERRIDE;
    virtual void setDebugFlags(unsigned long long,UInt) APPLE_KEXT_OVERRIDE;
    virtual SInt64 debugFlags(void) APPLE_KEXT_OVERRIDE;
    virtual void setInterfaceChipCounters(apple80211_stat_report *,apple80211_chip_counters_tx *,apple80211_chip_error_counters_tx *,apple80211_chip_counters_rx *) APPLE_KEXT_OVERRIDE;
    virtual void setInterfaceMIBdot11(apple80211_stat_report *,apple80211_ManagementInformationBasedot11_counters *) APPLE_KEXT_OVERRIDE;
    virtual void setFrameStats(apple80211_stat_report *,apple80211_frame_counters *) APPLE_KEXT_OVERRIDE;
#if __IO80211_TARGET >= __MAC_14_4
    virtual void setInfraSpecificFrameStats(apple80211_stat_report *,apple80211_infra_specific_stats *) APPLE_KEXT_OVERRIDE;
#endif
#if __IO80211_TARGET >= __MAC_26_0
    virtual void setRxDataStallStats(apple80211_stat_report *,apple80211_rx_data_stall_report *) APPLE_KEXT_OVERRIDE;
#endif
    virtual SInt64 getWmeTxCounters(unsigned long long *) APPLE_KEXT_OVERRIDE;
#if __IO80211_TARGET < __MAC_26_0
    virtual void setEnabledBySystem(bool) APPLE_KEXT_OVERRIDE;
    virtual bool enabledBySystem(void) APPLE_KEXT_OVERRIDE;
    virtual bool willRoam(ether_addr *,UInt) APPLE_KEXT_OVERRIDE;
#endif
    virtual void setPeerManagerLogFlag(UInt,UInt,UInt) APPLE_KEXT_OVERRIDE;
    virtual void setWoWEnabled(bool) APPLE_KEXT_OVERRIDE;
    virtual bool wowEnabled(void) APPLE_KEXT_OVERRIDE;
#if __IO80211_TARGET >= __MAC_26_0
    virtual UInt64 createLinkQualityMonitor(IO80211Peer *,bool) APPLE_KEXT_OVERRIDE;
#else
    virtual UInt64 createLinkQualityMonitor(IO80211Peer *,IOService *) APPLE_KEXT_OVERRIDE;
#endif
    virtual void releaseLinkQualityMonitor(IO80211Peer *) APPLE_KEXT_OVERRIDE;
    virtual int getAssocState(void) APPLE_KEXT_OVERRIDE;
    virtual void *getLQMSummary(apple80211_lqm_summary *) APPLE_KEXT_OVERRIDE;
#if __IO80211_TARGET >= __MAC_26_0
    // Own slots 461..468. setLinkState moved down here from IO80211SkywalkInterface and
    // gained a trailing argument; setPoweredOnByUser is gone and setCurrentBssid was
    // renamed setCurrentApAddress.
    // Defaults keep the existing two-argument call sites working; they do not affect
    // the vtable slot.
    virtual bool setLinkState(IO80211LinkState,UInt,bool debounceTimeout = 30,UInt code = 0,UInt = 0);
    virtual IOReturn setLinkStateInternal(IO80211LinkState,UInt,bool,UInt,UInt);
    virtual void setCurrentApAddress(ether_addr *);
    virtual void setWCL_ADVISORTY_INFO(apple80211_wcl_advisory_info *);
    virtual void *getWCL_TX_RX_LATENCY(apple80211_wcl_tx_rx_latency *);
    virtual IOReturn setWCL_LINK_STATE_UPDATE(apple80211_wcl_update_link_state *);
    virtual void *createLQMData(void);
    // Slot 468 is pure in the shipping class and unnamed — AppleBCMWLANInfraProtocol does not
    // override it either, so the vtable carries ___cxa_pure_virtual and the symbol is lost.
    //
    // **It is nevertheless called, and its return value is error-checked.**
    // `IO80211MacAddressAgent::setMacAddress` calls this slot (`call [rax + 0xea0]`, and
    // 0xea0 / 8 == 468) on the interface once the `IO80211InfraInterface` cast succeeds — which for
    // this driver it always does — then treats a non-zero result as fatal:
    // "fail to set mac addr", JOIN_MANAGER_EVENT_JOIN_REQ_FAILED, and no association is possible.
    //
    // This was declared `void` for a long time, which is exactly the trap: a void placeholder
    // leaves %eax holding whatever the previous code left there, so the family read a *leftover
    // pointer* as a status. The symptom was a large constant that stayed identical across attempts
    // within a boot and changed its high byte between boots, tracking the KASLR slide.
    //
    // Returning IOReturn 0 is the minimum honest answer: the contract is still unknown, but the one
    // thing established about it is that the family requires success here. Do not restore `void`,
    // and do not return anything non-zero.
    virtual IOReturn _RESERVEDIO80211InfraInterface0(void) { return kIOReturnSuccess; }
#else
    virtual IOReturn setLinkStateInternal(IO80211LinkState,uint,bool,uint,apple80211_link_changed_event_data &);
    virtual void setPoweredOnByUser(bool);
    virtual void setCurrentBssid(ether_addr *);
    virtual void setWCL_ADVISORTY_INFO(apple80211_wcl_advisory_info *);
    virtual void *getWCL_TX_RX_LATENCY(apple80211_wcl_tx_rx_latency *);
#endif

#if __IO80211_TARGET >= __MAC_26_0
public:
    // NON-VIRTUAL and exported, so this declaration is vtable-neutral — confirmed by its absence
    // from scripts/abi/abi-26.6-25G72.txt, which is the check before declaring anything this way.
    //
    // The family's purpose-built entry point for registering an infra interface, and the reason
    // full Skywalk registration is reachable at all. It stamps the MAC from getSelfMacAddr()
    // (slot 415) into info+0x108, then tails into
    // IOSkywalkEthernetInterface::registerEthernetInterface(info, queues, n, pool, pool, 0),
    // whose queue-array form builds the IOSkywalkQueueSet and IOSkywalkLogicalLink internally —
    // so the driver never touches either. Sole Apple caller: AppleBCMWLANSkywalkInterface::start.
    //
    // CAUTION: the MAC stamp is skipped, silently and without failing the call, unless
    //     ((char *)this)[0x128] -> byte[0x3c50] & 1     and
    //     ((char *)this)[0x120] -> dword[0x58] == 1
    // both hold. A successful return therefore does NOT prove the interface has a MAC.
    IOReturn registerInfraEthernetInterface(IOSkywalkEthernetInterface::RegistrationInfo *,
                                            IOSkywalkPacketQueue **, unsigned int,
                                            IOSkywalkPacketBufferPool *,
                                            IOSkywalkPacketBufferPool *);
#endif

public:
#if __IO80211_TARGET >= __MAC_26_0
    // sizeof(IO80211InfraInterface) == 0x130 over a 0x128 base.
    char _data[0x8];
#else
    char _data[0x120];
#endif
};

#if __IO80211_TARGET >= __MAC_26_0
static_assert(sizeof(IO80211InfraInterface) == 0x130, "Invalid class size");
#endif

#endif /* IO80211InfraInterface_h */
