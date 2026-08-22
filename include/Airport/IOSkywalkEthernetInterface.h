#ifndef IOSkywalkEthernetInterface_h
#define IOSkywalkEthernetInterface_h

#include "IOSkywalkNetworkInterface.h"

struct nicproxy_limits_info_s;
struct nicproxy_info_s;

class IOSkywalkEthernetInterface : public IOSkywalkNetworkInterface {
    OSDeclareAbstractStructors( IOSkywalkEthernetInterface )
    
public:
    // 304 is confirmed for Tahoe: initRegistrationInfo rejects any other size outright
    // (`cmp rcx, 0x130` / `sete`), then bzeros the struct from +8 for 0x128 bytes.
    //
    // Left opaque deliberately — the driver never writes a field directly, it calls
    // initRegistrationInfo and lets Apple fill it. The map below is what that initialiser writes,
    // recovered on 26.6, and exists so the two load-bearing fields can be found without
    // re-disassembling it. Name a field here only when the driver actually needs to set it.
    //
    //   +0x00  u32   version = 1                    +0x04  u32   size = 0x130 (validated)
    //   +0x08  u32   interface family    ?: 2       (IFNET_FAMILY_ETHERNET; read back by
    //                                                getInterfaceFamily, slot 308)
    //   +0x0c  u32   interface SUBfamily            (read back by getInterfaceSubFamily, slot 309;
    //                                                0 = WIRED, 3 = IFNET_SUBFAMILY_WIFI)
    //   +0x14  u32   0x100                          +0x24  u32   0x22
    //   +0x28  u32   slot 312            ?: 0x20
    //   +0x30  const char *  BSD name prefix  <- getBSDNamePrefix() ?: the literal "en"
    //   +0x38  s32           BSD unit number  <- getBSDUnitNumber()
    //   +0x44  u32   slot 305            ?: 0x2e    +0x48  u32   slot 303 ?: 0x5dc (1500, the MTU)
    //
    // +0x30 and +0x38 are the interface's identity, and they are read back out again:
    // IOSkywalkNetworkInterface::getBSDNamePrefix returns [reginfo+0x30] and getBSDUnitNumber
    // returns [reginfo+0x38] (via mExpansionData->fRegistrationInfo, -1 when either is NULL).
    //
    // NOTE THE CIRCULARITY. It applies to +0x38 AND +0x0c, and both were measured going wrong:
    // initRegistrationInfo seeds each field by calling the getter that reads that very field out
    // of the CURRENTLY INSTALLED struct. It propagates values; it never allocates or derives one.
    // So a driver that never seeds them ships unit 0 (an "en0" collision) and subfamily 0 (the
    // interface reports `type: Ethernet` and macOS does not treat it as Wi-Fi). Both were observed
    // exactly that way on the first -itlskywalkbsd boot.
    // A negative unit is the "unassigned" marker: registerNetworkInterface skips publishing the
    // `IOInterfaceUnit` property entirely when the unit is < 0 — which is not an escape, because
    // IOSkywalkNetworkBSDClient::start requires that property to exist.
    // **Rule: a field an initialiser seeds from its own getter is one the caller must supply.**
    // See root AGENTS.md mechanism 1.
    struct RegistrationInfo {
        uint8_t pad[304];
    } __attribute__((packed));
    
public:
    virtual void free() APPLE_KEXT_OVERRIDE;
    virtual bool init(OSDictionary *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn newUserClient( task_t owningTask, void * securityID,
                                   UInt32 type, OSDictionary * properties,
                                   LIBKERN_RETURNS_RETAINED IOUserClient ** handler ) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setPowerState(
                                   unsigned long powerStateOrdinal,
                                   IOService *   whatDevice ) APPLE_KEXT_OVERRIDE;
    virtual IOReturn enable(UInt) APPLE_KEXT_OVERRIDE;
    virtual SInt32 initBSDInterfaceParameters(ifnet_init_eparams *,sockaddr_dl **) APPLE_KEXT_OVERRIDE;
    virtual bool prepareBSDInterface(ifnet_t,UInt) APPLE_KEXT_OVERRIDE;
    virtual IOReturn processBSDCommand(ifnet_t,UInt,void *) APPLE_KEXT_OVERRIDE;
    virtual void *getPacketTapInfo(UInt *,UInt *) APPLE_KEXT_OVERRIDE;
    virtual void enableNetworkWake(UInt) APPLE_KEXT_OVERRIDE;
    virtual UInt getMaxTransferUnit(void) APPLE_KEXT_OVERRIDE;
    virtual UInt getMinPacketSize(void) APPLE_KEXT_OVERRIDE;
    virtual void *getInterfaceFamily(void) APPLE_KEXT_OVERRIDE;
    virtual void *getInterfaceSubFamily(void) APPLE_KEXT_OVERRIDE;
    virtual UInt getInitialMedia(void) APPLE_KEXT_OVERRIDE;
    virtual const char *getBSDNamePrefix(void) APPLE_KEXT_OVERRIDE;
#if __IO80211_TARGET < __MAC_26_0
    // Tahoe dropped this overload; the base class one at the head of
    // IOSkywalkNetworkInterface's slots is the only remaining entry point.
    virtual IOReturn registerNetworkInterfaceWithLogicalLink(IOSkywalkEthernetInterface::RegistrationInfo const*, IOSkywalkLogicalLink*, IOSkywalkPacketBufferPool*, IOSkywalkPacketBufferPool*, UInt);
#endif
    // Returns an IOReturn, NOT void. initBSDInterfaceParameters calls this when the installed
    // RegistrationInfo's MAC (+0x108) is all zeros and then does `test eax,eax`, taking the value
    // as success/failure — so a void declaration leaves the caller reading an undefined register
    // and deciding the interface's link address on it.
    virtual IOReturn getHardwareAddress(ether_addr *);
    // Slot 334, and an IOReturn for the same reason: IOSkywalkEthernetInterface::ioctl_lladdr —
    // the SIOCSIFLLADDR handler — calls this with the requested address, `test eax,eax`, and
    // ABORTS the ioctl on non-zero, then calls it again with the OLD address to roll back. Apple's
    // implementation forwards to IO80211MacAddressAgent::setMacAddress.
    virtual IOReturn setHardwareAddress(ether_addr *);
    // Slot 335, and THE notification a driver gets when the family changes the link address.
    // IO80211MacAddressAgent::updateMacAddress calls it whenever its `informSkywalk` argument is
    // set; Apple's implementation publishes the IOMACAddress property and calls ifnet_set_lladdr.
    // It returns an IOReturn (0xe00002bc when there is no BSD interface); updateMacAddress ignores
    // the value, but nothing guarantees the next caller will.
    virtual IOReturn setLinkLayerAddress(ether_addr *);
    virtual bool configureMulticastFilter(UInt,ether_addr const*,UInt);
    virtual bool setMulticastAddresses(ether_addr const*,UInt);
    virtual void setAllMulticastModeEnable(bool);
    virtual IOReturn setPromiscuousModeEnable(bool, UInt);
    virtual void reportNicProxyLimits(nicproxy_limits_info_s);
    virtual void hwConfigNicProxyData(nicproxy_info_s *);
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface,  0 );
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface,  1 );
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface,  2 );
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface,  3 );
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface,  4 );
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface,  5 );
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface,  6 );
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface,  7 );
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface,  8 );
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface,  9 );
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface, 10 );
    
public:
    bool initRegistrationInfo(IOSkywalkEthernetInterface::RegistrationInfo*, unsigned int, unsigned long);
    // Not virtual (absent from the vtable), so this binds to the exported symbol directly.
    // Allocates mExpansionData2->fRegistrationInfo with Apple's own allocator, memmoves 0x130
    // bytes from the argument, and fills the MAC at +0x108. Validates info->[0] == 1 and
    // info->[4] >= 0x130, which is exactly what initRegistrationInfo writes. This is the only
    // way to get that buffer allocated without a full registerEthernetInterface.
    IOReturn copyRegistrationInfo(IOSkywalkEthernetInterface::RegistrationInfo const*);
    bool registerEthernetInterface(IOSkywalkEthernetInterface::RegistrationInfo const*, IOSkywalkPacketQueue**, unsigned int, IOSkywalkPacketBufferPool*, IOSkywalkPacketBufferPool*, unsigned int);
    
public:
    void *vptr;
    uint8_t pad1[0x30];
    struct ExpansionData
    {
        RegistrationInfo *fRegistrationInfo;
        ifnet_t fBSDInterface;
    };
    ExpansionData *mExpansionData2;
};

#if __IO80211_TARGET >= __MAC_26_0
static_assert(__offsetof(IOSkywalkEthernetInterface, mExpansionData2) == 0x118, "Invalid class size");
static_assert(sizeof(IOSkywalkEthernetInterface) == 0x120, "Invalid class size");
#else
static_assert(__offsetof(IOSkywalkEthernetInterface, mExpansionData2) == 0x108, "Invalid class size");
static_assert(sizeof(IOSkywalkEthernetInterface) == 0x110, "Invalid class size");
#endif

#endif /* IOSkywalkEthernetInterface_h */
