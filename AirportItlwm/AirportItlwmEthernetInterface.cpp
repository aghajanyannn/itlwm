//
//  AirportItlwmEthernetInterface.cpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#include "AirportItlwmEthernetInterface.hpp"

#include <sys/_if_ether.h>
#include <net80211/ieee80211_var.h>

#define super IOEthernetInterface
OSDefineMetaClassAndStructors(AirportItlwmEthernetInterface, IOEthernetInterface);

bool AirportItlwmEthernetInterface::
initWithSkywalkInterfaceAndProvider(IONetworkController *controller, IO80211SkywalkInterface *interface)
{
    bool ret = super::init(controller);
    if (ret)
        this->interface = interface;
    this->isAttach = false;
    return ret;
}

IOReturn AirportItlwmEthernetInterface::
attachToDataLinkLayer( IOOptionBits options, void *parameter )
{
    XYLog("%s\n", __FUNCTION__);
    char infName[IFNAMSIZ];
    IOReturn ret = super::attachToDataLinkLayer(options, parameter);
    if (ret == kIOReturnSuccess && interface) {
        UInt8 builtIn = 0;
        IOEthernetAddress addr;
        interface->setProperty("built-in", OSData::withBytes(&builtIn, sizeof(builtIn)));
        snprintf(infName, sizeof(infName), "%s%u", ifnet_name(getIfnet()), ifnet_unit(getIfnet()));
        interface->setProperty("IOInterfaceName", OSString::withCString(infName));
        interface->setProperty(kIOInterfaceUnit, OSNumber::withNumber(ifnet_unit(getIfnet()), 8));
        interface->setProperty(kIOInterfaceNamePrefix, OSString::withCString(ifnet_name(getIfnet())));
        if (OSDynamicCast(IOEthernetController, getController())->getHardwareAddress(&addr) == kIOReturnSuccess) {
            setProperty(kIOMACAddress,  (void *) &addr,
                        kIOEthernetAddressSize);
            // Both writes are needed. IOEthernetController::publishProperties() publishes
            // kIOMACAddress on the *controller* node only — a plain IOEthernetInterface node
            // carries none (checked against IntelMausi's en0), so neither interface gets it
            // for free. Without this second write the Skywalk interface, which is the object
            // Apple80211's userspace enumeration inspects, publishes IOMACAddress as all
            // zeros while the ethernet node looks correct.
            interface->setProperty(kIOMACAddress, (void *) &addr,
                                   kIOEthernetAddressSize);
        }
        interface->registerService();
        interface->prepareBSDInterface(getIfnet(), 0);
//        ret = bpf_attach(getIfnet(), DLT_RAW, 0x48, &AirportItlwmEthernetInterface::bpfOutputPacket, &AirportItlwmEthernetInterface::bpfTap);
    }
    isAttach = true;
    return ret;
}

void AirportItlwmEthernetInterface::
detachFromDataLinkLayer(IOOptionBits options, void *parameter)
{
    super::detachFromDataLinkLayer(options, parameter);
    isAttach = false;
}

/**
 Add another hack to fake that the provider is IOSkywalkNetworkInterface, to avoid skywalkfamily instance cast panic.
 */
IOService *AirportItlwmEthernetInterface::
getProvider() const
{
    return isAttach ? this->interface : super::getProvider();
}

// Forwarding to super is the whole point: the behaviour must stay IOService's. What matters
// is that the slot carries a real address at link time rather than a hole for the loader.
int AirportItlwmEthernetInterface::
errnoFromReturn(IOReturn rtn)
{
    return super::errnoFromReturn(rtn);
}

const char *AirportItlwmEthernetInterface::
stringFromReturn(IOReturn rtn)
{
    return super::stringFromReturn(rtn);
}

SInt32 AirportItlwmEthernetInterface::
performCommand(IONetworkController *controller, unsigned long cmd, void *arg0, void *arg1)
{
    return super::performCommand(controller, cmd, arg0, arg1);
}

errno_t AirportItlwmEthernetInterface::
bpfOutputPacket(ifnet_t interface, u_int32_t data_link_type, mbuf_t packet)
{
    XYLog("%s data_link_type: %d\n", __FUNCTION__, data_link_type);
    AirportItlwmEthernetInterface *networkInterface = (AirportItlwmEthernetInterface *)ifnet_softc(interface);
    return networkInterface->enqueueOutputPacket(packet);
}

errno_t AirportItlwmEthernetInterface::
bpfTap(ifnet_t interface, u_int32_t data_link_type, bpf_tap_mode direction)
{
    XYLog("%s data_link_type: %d direction: %d\n", __FUNCTION__, data_link_type, direction);
    return 0;
}

bool AirportItlwmEthernetInterface::
setLinkState(IO80211LinkState state)
{
    if (state == kIO80211NetworkLinkUp) {
        ifnet_set_flags(getIfnet(), ifnet_flags(getIfnet()) | (IFF_UP | IFF_RUNNING), (IFF_UP | IFF_RUNNING));
    } else {
        ifnet_set_flags(getIfnet(), ifnet_flags(getIfnet()) & ~(IFF_UP | IFF_RUNNING), 0);
    }
    return true;
}

extern const char* hexdump(uint8_t *buf, size_t len);

#if __IO80211_TARGET >= __MAC_26_0
// Defined in AirportItlwmSkywalkInterface.cpp. Takes IO80211SkywalkInterface* and dynamic-casts
// internally, so this TU needs no knowledge of AirportItlwmSkywalkInterface's definition.
extern "C" bool itlwmSkywalkRxInput(IO80211SkywalkInterface *iface, mbuf_t m);
extern "C" uint32_t gItlwmSkywalkRxFallbackDrops;
#endif

UInt32 AirportItlwmEthernetInterface::
inputPacket(mbuf_t packet, UInt32 length, IOOptionBits options, void *param)
{
    ether_header_t *eh;
    size_t len = mbuf_len(packet);
    
    eh = (ether_header_t *)mbuf_data(packet);
    if (len >= sizeof(ether_header_t) && eh->ether_type == htons(ETHERTYPE_PAE)) { // EAPOL packet
        const char* dump = hexdump((uint8_t*)mbuf_data(packet), len);
        XYLog("input EAPOL packet, len: %zu, data: %s\n", len, dump ? dump : "Failed to allocate memory");
        if (dump)
            IOFree((void*)dump, 3 * len + 1);
    }
#if __IO80211_TARGET >= __MAC_26_0
    // Skywalk RX tee (root AGENTS.md mechanism 1). Inert until registration has published the
    // interface for real; the shim checks that itself and returns false otherwise, so no boot-arg
    // test belongs here.
    //
    // Reached through a C-linkage shim rather than by including AirportItlwmSkywalkInterface.hpp:
    // that header needs AirportItlwm, ItlHalService and the net80211 headers, none of which this
    // translation unit has, and pulling them in for one call is how include cycles start.
    //
    // Deliberately BEFORE the BSD hand-off and not instead of it: the shim returns false for every
    // reason it cannot deliver, and the frame then takes the legacy path exactly as before. While
    // both interfaces exist that is the safe ordering — the alternative drops frames whenever the
    // RX ring is momentarily starved.
    if (itlwmSkywalkRxInput(this->interface, packet)) {
        // Consumed: the frame was copied into a Skywalk buffer, so this mbuf is ours to free.
        // mbuf_freem, not freePacket — the latter is IONetworkController's, and this is an
        // IONetworkInterface.
        mbuf_freem(packet);
        return 0;
    }
    // Skywalk owns the ifnet on Tahoe, so this interface is attached WITHOUT registration and has
    // no BSD interface behind it. There is nothing to fall back to: handing the frame to super
    // would queue it into an interface that will never flush anywhere. Drop it.
    //
    // This is the deliberate consequence of retiring the BSD attach, and it makes RX starvation
    // total rather than merely slow — ItlwmSkywalkRxNoBuf climbing while RxFrames stays flat now
    // means lost traffic, not a fallback. It is the counter to read first.
    gItlwmSkywalkRxFallbackDrops++;
    mbuf_freem(packet);
    return 0;
#endif
    return IOEthernetInterface::inputPacket(packet, length, options, param);
}
