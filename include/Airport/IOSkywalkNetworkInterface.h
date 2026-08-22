//
//  IOSkywalkNetworkInterface.h
//  itlwm
//
//  Created by qcwap on 2023/6/7.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef IOSkywalkNetworkInterface_h
#define IOSkywalkNetworkInterface_h

#include <net/if.h>

#include "IOSkywalkInterface.h"

// XNU-private (net/if_var.h, KERNEL_PRIVATE), so it is not in any SDK. Only ever used
// through a pointer here, so an opaque declaration is enough — and it has to be a
// class type, because it is part of the mangled name of the vtable slot that binds to
// IOSkywalkNetworkInterface::reportDetailedLinkStatus(if_link_status const*).
// The old `typedef UInt` mangled that slot as `PKj`, which matched nothing and so
// linked as a null vtable entry.
struct if_link_status;
class IOSkywalkPacketQueue;
class IOSkywalkLogicalLink;
class IOSkywalkPacketBufferPool;
struct ifnet_traffic_descriptor_common;

class IOSkywalkNetworkInterface : public IOSkywalkInterface {
    OSDeclareAbstractStructors( IOSkywalkNetworkInterface )
    
public:
    // NOT the same size as IOSkywalkEthernetInterface::RegistrationInfo (304). Tahoe's is
    // 264, recovered two ways: the kalloc_type_view that registerNetworkInterface passes to
    // IOMallocTypeImpl carries kt_size 0x108, and the memmove that fills the allocation is
    // `mov edx, 0x108`. Sizes for earlier releases were never verified against a binary, so
    // they keep the original value.
    struct RegistrationInfo {
#if __IO80211_TARGET >= __MAC_26_0
        uint8_t pad[264];
#else
        uint8_t pad[304];
#endif
    } __attribute__((packed));
    struct IOSkywalkTSOOptions;
    
public:
    virtual void free() APPLE_KEXT_OVERRIDE;
    virtual bool init(OSDictionary *) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *) APPLE_KEXT_OVERRIDE;
    virtual void joinPMtree( IOService * driver ) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setAggressiveness(
                                       unsigned long type,
                                       unsigned long newLevel ) APPLE_KEXT_OVERRIDE;
    virtual IOReturn enable(UInt) APPLE_KEXT_OVERRIDE;
    virtual IOReturn disable(UInt) APPLE_KEXT_OVERRIDE;
#if __IO80211_TARGET >= __MAC_26_0
    // Tahoe moved these two to the head of the class's own slots.
    virtual IOReturn registerNetworkInterfaceWithLogicalLink(IOSkywalkNetworkInterface::RegistrationInfo const*,IOSkywalkLogicalLink *,IOSkywalkPacketBufferPool *,IOSkywalkPacketBufferPool *,UInt);
    virtual IOReturn deregisterLogicalLink(void);
#endif
    virtual SInt32 initBSDInterfaceParameters(ifnet_init_eparams *,sockaddr_dl **) = 0;
    virtual bool prepareBSDInterface(ifnet_t,UInt);
    virtual void finalizeBSDInterface(ifnet_t,UInt);
#if __IO80211_TARGET >= __MAC_26_0
    virtual ifnet_t getBSDInterface(void) const;
#else
    virtual ifnet_t getBSDInterface(void);
#endif
    virtual void setBSDName(char const*);
#if __IO80211_TARGET >= __MAC_26_0
    virtual const char *getBSDName(void) const;
#else
    virtual const char *getBSDName(void);
#endif
    virtual IOReturn processBSDCommand(ifnet_t,UInt,void *);
    virtual IOReturn processInterfaceCommand(ifdrv *);
    virtual IOReturn interfaceAdvisoryEnable(bool);
#if __IO80211_TARGET >= __MAC_26_0
    virtual IOReturn setRxFlowSteering(UInt,ifnet_traffic_descriptor_common *,UInt);
#endif
    virtual SInt32 setInterfaceEnable(bool);
    virtual SInt32 setRunningState(bool);
    virtual IOReturn handleChosenMedia(UInt);
    virtual void *getSupportedMediaArray(UInt *,UInt *);
    virtual void *getPacketTapInfo(UInt *,UInt *);
#if __IO80211_TARGET >= __MAC_26_0
    virtual UInt getUnsentDataByteCount(UInt *,UInt *,UInt) const;
#else
    virtual UInt getUnsentDataByteCount(UInt *,UInt *,UInt);
#endif
    virtual UInt32 getSupportedWakeFlags(UInt *);
    virtual void enableNetworkWake(UInt);
#if __IO80211_TARGET >= __MAC_26_0
    virtual void calculateRingSizeForQueue(IOSkywalkPacketQueue const*,UInt *) const;
#else
    virtual void calculateRingSizeForQueue(IOSkywalkPacketQueue const*,UInt *);
#endif
    virtual UInt getMaxTransferUnit(void);
    virtual void setMaxTransferUnit(UInt);
    virtual UInt getMinPacketSize(void);
    virtual UInt getHardwareAssists(void);
    virtual void setHardwareAssists(UInt,UInt);
    virtual void *getInterfaceFamily(void);
    virtual void *getInterfaceSubFamily(void);
    virtual UInt getInitialMedia(void);
    virtual UInt getFeatureFlags(void);
    virtual UInt getTxDataOffset(void);
    virtual UInt captureInterfaceState(UInt);
    virtual void restoreInterfaceState(UInt);
    virtual void setMTU(UInt);
    virtual bool bpfTap(UInt,UInt);
    virtual const char *getBSDNamePrefix(void);
    virtual UInt getBSDUnitNumber(void);
    virtual const char *classNameOverride(void);
    virtual void deferBSDAttach(bool);
    virtual void reportDetailedLinkStatus(if_link_status const*);
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn registerNetworkInterfaceWithLogicalLink(IOSkywalkNetworkInterface::RegistrationInfo const*,IOSkywalkLogicalLink *,IOSkywalkPacketBufferPool *,IOSkywalkPacketBufferPool *,UInt);
    virtual IOReturn deregisterLogicalLink(void);
#endif
    virtual UInt getTSOOptions(IOSkywalkNetworkInterface::IOSkywalkTSOOptions *);
    OSMetaClassDeclareReservedUnused( IOSkywalkNetworkInterface,  0);
    OSMetaClassDeclareReservedUnused( IOSkywalkNetworkInterface,  1);
    OSMetaClassDeclareReservedUnused( IOSkywalkNetworkInterface,  2);
    OSMetaClassDeclareReservedUnused( IOSkywalkNetworkInterface,  3);
    OSMetaClassDeclareReservedUnused( IOSkywalkNetworkInterface,  4);
    OSMetaClassDeclareReservedUnused( IOSkywalkNetworkInterface,  5);
    OSMetaClassDeclareReservedUnused( IOSkywalkNetworkInterface,  6);
    OSMetaClassDeclareReservedUnused( IOSkywalkNetworkInterface,  7);
    OSMetaClassDeclareReservedUnused( IOSkywalkNetworkInterface,  8);
    OSMetaClassDeclareReservedUnused( IOSkywalkNetworkInterface,  9);
    
public:
    // Non-virtual and exported, so this is a direct call and the linker checks it.
    //
    // The ONLY link-state notification into the Skywalk layer. It stores the state under a lock
    // and, when it actually changes, tail-calls reportEventType with 0xE0060100 (down) or
    // 0xE0060102 (up). It returns early with kIOReturnNotAttached-ish 0xe00002d8 when
    // getBSDInterface() is NULL, so it is inert unless this interface owns the ifnet.
    //
    // `status` is compared masked to 3 and means "up" when 1. `speed` of 0 keeps the stored value.
    // Returns an IOReturn — the header used to declare this void, which threw away the one signal
    // that says the precondition failed.
    IOReturn reportLinkStatus(unsigned int status, unsigned int speed);
    
public:
    void *vptr;
#if __IO80211_TARGET >= __MAC_26_0
    // Tahoe's 16 bytes of growth are NOT all trailing: 8 land here, ahead of the expansion
    // pointer, which moves it 0xb8 -> 0xc0. Recovered from the shipping kernel — both
    // IOSkywalkNetworkInterface::init and ::free address it as [this + 0xc0]. Modelling the
    // growth as trailing padding alone kept sizeof() correct while leaving mExpansionData
    // one slot early, on a neighbour that reads NULL; AirportItlwm::start then faulted
    // writing through it. sizeof() agreeing is not evidence a member offset is right.
    uint8_t pad0[8];
#endif
    struct ExpansionData
    {
        RegistrationInfo *fRegistrationInfo;
        ifnet_t fBSDInterface;
    };
    ExpansionData *mExpansionData;
#if __IO80211_TARGET >= __MAC_26_0
    uint8_t pad[3 * 8];
#else
    uint8_t pad[2 * 8];
#endif
};

#if __IO80211_TARGET >= __MAC_26_0
static_assert(__offsetof(IOSkywalkNetworkInterface, mExpansionData) == 0xC0,
              "Invalid member offset");
static_assert(sizeof(IOSkywalkNetworkInterface) == 0xE0, "Invalid class size");
#else
static_assert(__offsetof(IOSkywalkNetworkInterface, mExpansionData) == 0xB8,
              "Invalid member offset");
static_assert(sizeof(IOSkywalkNetworkInterface) == 0xD0, "Invalid class size");
#endif

#endif /* IOSkywalkNetworkInterface_h */
