//
//  IOSkywalkInterface.h
//  itlwm
//
//  Created by qcwap on 2023/6/7.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef _IO80211CONTROLLER_H
#define _IO80211CONTROLLER_H

#if defined(KERNEL) && defined(__cplusplus)

#include <Availability.h>
#include <libkern/version.h>

// This is necessary, because even the latest Xcode does not support properly targeting 11.0.
#ifndef __IO80211_TARGET
#error "Please define __IO80211_TARGET to the requested version"
#endif

#if VERSION_MAJOR > 8
#define _MODERN_BPF
#endif

#include <sys/kpi_mbuf.h>

#include <IOKit/network/IOEthernetController.h>

#include <sys/param.h>
#include <net/bpf.h>

#include "apple80211_ioctl.h"
#include "IO80211SkywalkInterface.h"
#include "IO80211WorkLoop.h"
#include "IO80211WorkQueue.h"
#include "CCStream.h"
#include "CCDataPipe.h"
#include "CCLogPipe.h"
#include "CCLogStream.h"
#include "CCDataStream.h"
#include "CCFaultReporter.h"
#include "IO80211FaultReporter.h"
#include "BeaconMetaData.h"
#include "AssocCandidates.h"
#include "JoinCompleteEvents.h"
#include "ExtendedBssInfo.h"
#include "LqmEventData.h"

#define AUTH_TIMEOUT            15    // seconds

/*! @enum LinkSpeed.
 @abstract ???.
 @discussion ???.
 @constant LINK_SPEED_80211A 54 Mbps
 @constant LINK_SPEED_80211B 11 Mbps.
 @constant LINK_SPEED_80211G 54 Mbps.
 */
enum {
    LINK_SPEED_80211A    = 54000000ul,        // 54 Mbps
    LINK_SPEED_80211B    = 11000000ul,        // 11 Mbps
    LINK_SPEED_80211G    = 54000000ul,        // 54 Mbps
    LINK_SPEED_80211N    = 300000000ul,        // 300 Mbps (MCS index 15, 400ns GI, 40 MHz channel)
};

enum IO80211CountryCodeOp
{
    kIO80211CountryCodeReset,                // Reset country code to world wide default, and start
    // searching for 802.11d beacon
};
typedef enum IO80211CountryCodeOp IO80211CountryCodeOp;

enum IO80211SystemPowerState
{
    kIO80211SystemPowerStateUnknown,
    kIO80211SystemPowerStateAwake,
    kIO80211SystemPowerStateSleeping,
};
typedef enum IO80211SystemPowerState IO80211SystemPowerState;

enum IO80211FeatureCode
{
    kIO80211Feature80211n = 1,
};
typedef enum IO80211FeatureCode IO80211FeatureCode;


class IOSkywalkInterface;
class IO80211ScanManager;

enum scanSource
{
    SOURCE_1,
};

enum joinStatus
{
    STATUS_1,
};

class IO80211Controller;
class IO80211Interface;
class IO82110WorkLoop;
class IO80211VirtualInterface;
class IO80211ControllerMonitor;
class CCLogPipe;
class CCIOReporterLogStream;
class CCLogStream;
class IO80211VirtualInterface;
class IO80211RangingManager;
class IO80211FlowQueue;
class IO80211FlowQueueLegacy;
class FlowIdMetadata;
class IOReporter;
class IO80211InfraInterface;
extern void IO80211VirtualInterfaceNamerRetain();


struct apple80211_hostap_state;

struct apple80211_awdl_sync_channel_sequence;
struct ieee80211_ht_capability_ie;
struct apple80211_channel_switch_announcement;
struct apple80211_beacon_period_data;
struct apple80211_power_debug_sub_info;
struct apple80211_stat_report;
struct apple80211_frame_counters;
struct apple80211_leaky_ap_event;
struct apple80211_chip_stats;
struct apple80211_extended_stats;
struct apple80211_ampdu_stat_report;
struct apple80211_btCoex_report;
struct apple80211_cca_report;
class CCPipe;
struct apple80211_lteCoex_report;

//typedef int scanSource;
//typedef int joinStatus;
//typedef int CCStreamLogLevel;
typedef IOReturn (*IOCTL_FUNC)(IO80211Controller*, IO80211Interface*, IO80211VirtualInterface*, apple80211req*, bool);
extern IOCTL_FUNC gGetHandlerTable[];
extern IOCTL_FUNC gSetHandlerTable[];

class IO80211InterfaceAVCAdvisory;

#if __IO80211_TARGET >= __MAC_26_0
struct apple80211_platform_config;
#endif

class IO80211Controller : public IOEthernetController {
    OSDeclareAbstractStructors(IO80211Controller)

public:

    virtual void free() APPLE_KEXT_OVERRIDE;
    virtual bool init(OSDictionary *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn configureReport(IOReportChannelList *,UInt,void *,void *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn updateReport(IOReportChannelList *,UInt,void *,void *) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *) APPLE_KEXT_OVERRIDE;
    virtual IOWorkLoop* getWorkLoop(void) const APPLE_KEXT_OVERRIDE;
    virtual const char* stringFromReturn(int) APPLE_KEXT_OVERRIDE;
    virtual int errnoFromReturn(int) APPLE_KEXT_OVERRIDE;
    virtual UInt32 getFeatures() const APPLE_KEXT_OVERRIDE;
    virtual const OSString * newVendorString() const APPLE_KEXT_OVERRIDE;
    virtual const OSString * newModelString() const APPLE_KEXT_OVERRIDE;
    virtual bool createWorkLoop() APPLE_KEXT_OVERRIDE;
    virtual IOReturn getHardwareAddress(IOEthernetAddress *) APPLE_KEXT_OVERRIDE;
#if __IO80211_TARGET < __MAC_26_0
    // Tahoe's IO80211Controller does not override this — slot 357 still holds
    // IOEthernetController::setHardwareAddress(IOEthernetAddress const*). Declaring it
    // here emitted a reference to IO80211Controller::setHardwareAddress, which
    // IO80211Family does not export, leaving the slot null.
    virtual IOReturn setHardwareAddress(const IOEthernetAddress * addrP) APPLE_KEXT_OVERRIDE;
#endif
    virtual IOReturn setMulticastMode(bool active) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setPromiscuousMode(bool active) APPLE_KEXT_OVERRIDE;
#if __IO80211_TARGET >= __MAC_26_0
    // Tahoe own slots 394..462; see scripts/abi/tahoe-26.6-slots.txt.
    // Only 12 of these are pure in the shipping class. The old apple80211_ioctl*,
    // apple80211VirtualRequest, apple80211SkywalkRequest, setVirtualHardwareAddress,
    // getHardwareAddressForInterface, useAppleRSNSupplicant, isAssociatedToMovingNetwork,
    // enable/disable(IO80211SkywalkInterface *) and postMessage entry points are gone —
    // their slots are now _RESERVEDIO80211Controller0..15 padding.
    virtual bool isCommandProhibited(int) = 0;
    virtual bool createWorkQueue();
    // No body == the slot binds to IO80211Family's exported implementation.
    // getPostOffice/CreatePostOffice must be bound: IO80211Controller::start() logs
    // "fPostOffice NULL !!!" and fails if CreatePostOffice() returns NULL.
    // getActionFramePoolCapacity/getPLATFORM_CONFIG are left stubbed deliberately — not
    // required yet, and keeping them stubbed limits how much untested kernel code a single
    // bring-up step turns on. allocIO80211RecursiveLock was in that list and must NOT be:
    // stubbing it returned NULL to IO80211PeerManager::initWithInterface and stalled the whole
    // bring-up. Before stubbing anything else here, check with scripts/abi/findfield.py and
    // callers.py whether the family requires a non-NULL result.
    virtual void debugStateInit(void);
    virtual IO80211WorkQueue *getWorkQueue() const;
    virtual void requestPacketTx(void*, UInt);
    virtual IOCommandGate *getIO80211CommandGate() const;
    virtual IO80211SkywalkInterface* getPrimarySkywalkInterface(void);
    virtual int bpfOutputPacket(OSObject *,UInt,mbuf_t m);
    virtual SInt32 monitorModeSetEnabled(bool, UInt);
    virtual SInt32 handleCardSpecific(IO80211SkywalkInterface *,unsigned long,void *,bool) = 0;
    virtual UInt32 hardwareOutputQueueDepth();
    virtual SInt32 performCountryCodeOperation(IO80211CountryCodeOp);
    virtual void dataLinkLayerAttachComplete();
    virtual SInt32 enableFeature(IO80211FeatureCode, void*) = 0;
    virtual IOReturn getDRIVER_VERSION(IO80211SkywalkInterface *,apple80211_version_data *) = 0;
    virtual IOReturn getHARDWARE_VERSION(IO80211SkywalkInterface *,apple80211_version_data *) = 0;
    virtual IOReturn getCARD_CAPABILITIES(IO80211SkywalkInterface *,apple80211_capability_data *) = 0;
    virtual IOReturn getPOWER(IO80211SkywalkInterface *,apple80211_power_data *) = 0;
    virtual IOReturn setPOWER(IO80211SkywalkInterface *,apple80211_power_data *) = 0;
    virtual IOReturn getCOUNTRY_CODE(IO80211SkywalkInterface *,apple80211_country_code_data *) = 0;
    virtual IOReturn setCOUNTRY_CODE(IO80211SkywalkInterface *,apple80211_country_code_data *) = 0;
    virtual IOReturn setGET_DEBUG_INFO(IO80211SkywalkInterface *,apple80211_debug_command *) = 0;
    virtual IOReturn getPLATFORM_CONFIG(IO80211SkywalkInterface *,apple80211_platform_config *) { return kIOReturnUnsupported; }
    virtual SInt32 enableVirtualInterface(IO80211VirtualInterface *);
    virtual SInt32 disableVirtualInterface(IO80211VirtualInterface *);
    virtual bool requiresExplicitMBufRelease();
    virtual bool flowIdSupported() {
        return false;
    }
    virtual IO80211FlowQueueLegacy* requestFlowQueue(FlowIdMetadata const*);
    virtual void releaseFlowQueue(IO80211FlowQueue *);
    virtual bool getLogPipes(CCPipe**, CCPipe**, CCPipe**);
    virtual void *getLogger(void) const { return NULL; }
    virtual void enableFeatureForLoggingFlags(unsigned long long) {};
    virtual IOReturn requestQueueSizeAndTimeout(unsigned short *, unsigned short *) { return kIOReturnIOError; };
    virtual IOReturn enablePacketTimestamping(void) {
        return kIOReturnUnsupported;
    }
    virtual IOReturn disablePacketTimestamping(void) {
        return kIOReturnUnsupported;
    }
    virtual UInt getPacketTSCounter();
    virtual void *getDriverTextLog();
    virtual UInt32 selfDiagnosticsReport(int,char const*,UInt);
    // Slot 432. Declared void * because the pre-Tahoe targets still return a CCStream here,
    // but on Tahoe the only correct value is an IO80211FaultReporter *: Apple stores this
    // unchecked as the controller's CommonFaultReporter and later calls vtable slot 36 on
    // it, and NULL is a panic rather than a soft failure. See IO80211FaultReporter.h.
    virtual void *getFaultReporterFromDriver();
    // MUST have no body: the slot has to bind to IO80211Family's implementation, which is
    // `IO80211IORecursiveLock::allocWithParams(getWorkQueue())` — a class this repo does not
    // reconstruct, so returning NULL here cannot be substituted for. Stubbing it was the single
    // root cause of the Tahoe bring-up stalling: IO80211PeerManager::initWithInterface+0x27c
    // calls this slot and fails if it gets NULL, which fails createPeerManager, which fails
    // IO80211SkywalkInterface::start ("peer manager init fail"), which stops
    // IO80211InfraInterface::start before it creates the scan manager or the event source —
    // i.e. no scan, no association, and the event-pipe NULL that mechanism 2 guards against.
    // See "Settled: the peer manager is the single root cause" in AirportItlwm/AGENTS.md.
    virtual void *allocIO80211RecursiveLock(void);
    // Non-virtual (absent from tahoe-26.6-slots.txt) and exported by IO80211Family, so declaring
    // it costs no slot. This is how Apple's own drivers raise events: it routes through the
    // IO80211PostOffice at controller ivars +0xb18 — built by CreatePostOffice(), slot 440 — not
    // straight at the interface. AppleBCMWLANCore::postMessageInfra and ::scanComplete both go
    // this way. Posting directly to IO80211SkywalkInterface::postMessage instead reaches the
    // interface but apparently not the WCL. Prefer this route.
    void postMessage(IO80211SkywalkInterface *,UInt,void *,unsigned long,bool);
    virtual UInt32 getDataQueueDepth(OSObject *);
    virtual bool wasDynSARInFailSafeMode(void) { return false; }
    virtual void updateAdvisoryScoresIfNeed(void);
    virtual UInt64 getAVCAdvisoryInfo(IO80211InterfaceAVCAdvisory *);
    virtual UInt32 getActionFramePoolCapacity(void) { return 0; }
    virtual void *getPostOffice(void);
    virtual void *CreatePostOffice(void);
    virtual bool attachInterface(OSObject *,IOService *);
    virtual void detachInterface(OSObject *,bool);
    virtual IO80211VirtualInterface* createVirtualInterface(ether_addr *,UInt);
    virtual bool attachVirtualInterface(IO80211VirtualInterface **,ether_addr *,UInt,bool);
    virtual bool detachVirtualInterface(IO80211VirtualInterface *,bool);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  0);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  1);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  2);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  3);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  4);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  5);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  6);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  7);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  8);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  9);
    OSMetaClassDeclareReservedUnused( IO80211Controller, 10);
    OSMetaClassDeclareReservedUnused( IO80211Controller, 11);
    OSMetaClassDeclareReservedUnused( IO80211Controller, 12);
    OSMetaClassDeclareReservedUnused( IO80211Controller, 13);
    OSMetaClassDeclareReservedUnused( IO80211Controller, 14);
    OSMetaClassDeclareReservedUnused( IO80211Controller, 15);
    virtual IOReturn setMulticastList(ether_addr const*, UInt);
#else
    virtual bool isCommandProhibited(int) = 0;
    virtual bool createWorkQueue();
    virtual IO80211WorkQueue *getWorkQueue();
    virtual void requestPacketTx(void*, UInt);
    virtual IOCommandGate *getIO80211CommandGate();
    virtual IOReturn getHardwareAddressForInterface(IOEthernetAddress *);
    virtual bool useAppleRSNSupplicant(IO80211VirtualInterface *);
    virtual IO80211SkywalkInterface* getPrimarySkywalkInterface(void);
    virtual int bpfOutputPacket(OSObject *,UInt,mbuf_t m);
    virtual SInt32 monitorModeSetEnabled(bool, UInt);
    virtual SInt32 apple80211_ioctl(IO80211SkywalkInterface *,unsigned long,void *, bool, bool);
    virtual SInt32 apple80211VirtualRequest(UInt,int,IO80211VirtualInterface *,void *);
    virtual SInt32 apple80211SkywalkRequest(UInt,int,IO80211SkywalkInterface *,void *);
    virtual SInt32 apple80211SkywalkRequest(UInt,int,IO80211SkywalkInterface *,void *,void *);
    
    virtual SInt32 handleCardSpecific(IO80211SkywalkInterface *,unsigned long,void *,bool) = 0;
    
    virtual UInt32 hardwareOutputQueueDepth();
    virtual SInt32 performCountryCodeOperation(IO80211CountryCodeOp);
    
    virtual void dataLinkLayerAttachComplete();
    virtual SInt32 enableFeature(IO80211FeatureCode, void*) = 0;
    
    virtual IOReturn getDRIVER_VERSION(IO80211SkywalkInterface *,apple80211_version_data *) = 0;
    virtual IOReturn getHARDWARE_VERSION(IO80211SkywalkInterface *,apple80211_version_data *) = 0;
    virtual IOReturn getCARD_CAPABILITIES(IO80211SkywalkInterface *,apple80211_capability_data *) = 0;
    virtual IOReturn getPOWER(IO80211SkywalkInterface *,apple80211_power_data *) = 0;
    virtual IOReturn setPOWER(IO80211SkywalkInterface *,apple80211_power_data *) = 0;
    virtual IOReturn getCOUNTRY_CODE(IO80211SkywalkInterface *,apple80211_country_code_data *) = 0;
    virtual IOReturn setCOUNTRY_CODE(IO80211SkywalkInterface *,apple80211_country_code_data *) = 0;
    virtual IOReturn setGET_DEBUG_INFO(IO80211SkywalkInterface *,apple80211_debug_command *) = 0;
    
    virtual SInt32 setVirtualHardwareAddress(IO80211VirtualInterface *,ether_addr *);
    virtual SInt32 enableVirtualInterface(IO80211VirtualInterface *);
    virtual SInt32 disableVirtualInterface(IO80211VirtualInterface *);
    virtual bool requiresExplicitMBufRelease();
    virtual bool flowIdSupported() {
        return false;
    }
    virtual IO80211FlowQueueLegacy* requestFlowQueue(FlowIdMetadata const*);
    virtual void releaseFlowQueue(IO80211FlowQueue *);
    virtual bool getLogPipes(CCPipe**, CCPipe**, CCPipe**);
    virtual void enableFeatureForLoggingFlags(unsigned long long) {};
    virtual IOReturn requestQueueSizeAndTimeout(unsigned short *, unsigned short *) { return kIOReturnIOError; };
    virtual IOReturn enablePacketTimestamping(void) {
        return kIOReturnUnsupported;
    }
    virtual IOReturn disablePacketTimestamping(void) {
        return kIOReturnUnsupported;
    }
    
    virtual UInt getPacketTSCounter();
    virtual void *getDriverTextLog();
    
    virtual UInt32 selfDiagnosticsReport(int,char const*,UInt);
    
    virtual void *getFaultReporterFromDriver();

    virtual UInt32 getDataQueueDepth(OSObject *);
    virtual bool isAssociatedToMovingNetwork(void) { return false; }
    virtual bool wasDynSARInFailSafeMode(void) { return false; }
    virtual void updateAdvisoryScoresIfNeed(void);
    virtual UInt64 getAVCAdvisoryInfo(IO80211InterfaceAVCAdvisory *);
    virtual SInt32 apple80211_ioctl_get(IO80211SkywalkInterface *,void *, bool, bool);
    virtual SInt32 apple80211_ioctl_set(IO80211SkywalkInterface *,void *, bool, bool);
    virtual bool attachInterface(OSObject *,IOService *);
    virtual SInt32 apple80211_ioctl_get(IO80211VirtualInterface *,void *,bool,bool);
    virtual SInt32 apple80211_ioctl_set(IO80211VirtualInterface *,void *,bool,bool);
    virtual void detachInterface(OSObject *,bool);
    virtual IO80211VirtualInterface* createVirtualInterface(ether_addr *,UInt);
    virtual bool attachVirtualInterface(IO80211VirtualInterface **,ether_addr *,UInt,bool);
    virtual bool detachVirtualInterface(IO80211VirtualInterface *,bool);
    virtual IOReturn enable(IO80211SkywalkInterface *);
    virtual IOReturn disable(IO80211SkywalkInterface *);
    
    OSMetaClassDeclareReservedUnused( IO80211Controller,  0);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  1);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  2);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  3);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  4);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  5);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  6);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  7);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  8);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  9);
    OSMetaClassDeclareReservedUnused( IO80211Controller, 10);
    OSMetaClassDeclareReservedUnused( IO80211Controller, 11);
    OSMetaClassDeclareReservedUnused( IO80211Controller, 12);
    OSMetaClassDeclareReservedUnused( IO80211Controller, 13);
    OSMetaClassDeclareReservedUnused( IO80211Controller, 14);
    OSMetaClassDeclareReservedUnused( IO80211Controller, 15);
    
    virtual void postMessage(UInt,void *,unsigned long,UInt,void *);
    virtual IOReturn setMulticastList(ether_addr const*, UInt);
#endif

protected:
#if __IO80211_TARGET >= __MAC_26_0
    uint8_t  filler[0x10];
#else
    uint8_t  filler[0x128];
#endif
};

#if __IO80211_TARGET >= __MAC_26_0
static_assert(sizeof(IO80211Controller) == 0x128, "Invalid class size");
#endif

#endif /* defined(KERNEL) && defined(__cplusplus) */

#endif /* !_IO80211CONTROLLER_H */
