//
//  AirportItlwmEthernetInterface.hpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef AirportItlwmEthernetInterface_hpp
#define AirportItlwmEthernetInterface_hpp

extern "C" {
#include <net/bpf.h>
}
#include "Airport/Apple80211.h"
#include <IOKit/IOLib.h>
#include <libkern/OSKextLib.h>
#include <sys/kernel_types.h>
#include <IOKit/network/IOEthernetInterface.h>

class AirportItlwmEthernetInterface : public IOEthernetInterface {
    OSDeclareDefaultStructors(AirportItlwmEthernetInterface)
    
public:
    virtual IOReturn attachToDataLinkLayer( IOOptionBits options,
                                            void *       parameter ) override;
    
    virtual void     detachFromDataLinkLayer( IOOptionBits options,
                                              void *       parameter ) override;
    
    virtual bool initWithSkywalkInterfaceAndProvider(IONetworkController *controller, IO80211SkywalkInterface *interface);
    
    virtual bool setLinkState(IO80211LinkState state);
    
    static errno_t bpfOutputPacket(ifnet_t interface, u_int32_t data_link_type,
                                  mbuf_t packet);
    
    static errno_t bpfTap(ifnet_t interface, u_int32_t data_link_type,
                          bpf_tap_mode direction);
    
    virtual UInt32   inputPacket(
                                 mbuf_t          packet,
                                 UInt32          length  = 0,
                                 IOOptionBits    options = 0,
                                 void *          param   = 0 ) override;
    
    // The ioctl that attachNetworkInterfaceToBSD issues after attach faults inside Apple's
    // own dispatch, without ever entering our code. This is the one observation point on that
    // path we can occupy: it runs before executeCommand -> the gated syncSIOC* handler that
    // dies, and it carries the ioctl number.
    virtual SInt32 performCommand(IONetworkController *controller, unsigned long cmd,
                                  void *arg0, void *arg1) override;

    // Slots 241 and 240 of this class's vtable. Both are inherited from IOService and so are
    // ZERO in our built kext — they exist only because the kext loader patches them in from
    // the parent vtable. It gets slot 241 WRONG: it binds IO80211SkywalkInterface::
    // errnoFromReturn, because our kext also carries IO80211SkywalkInterface, which overrides
    // the same method name. syncSIOCSIFFLAGS tail-jmps through that slot, the wrong
    // implementation treats an AirportItlwmEthernetInterface as an IO80211SkywalkInterface and
    // dispatches its vtable slot 373 — 28 past the end of our 345-entry table — into adjacent
    // __DATA. That is the NX panic on SIOCSIFFLAGS.
    //
    // Defining them here makes the slots non-zero in the file, so the loader has nothing to
    // patch and cannot mis-bind them. Same remedy as _RESERVEDIONetworkController6/7 in
    // AirportItlwmV2.cpp. See include/Airport/AGENTS.md for the scan that lists every other
    // slot exposed to this collision.
    virtual int errnoFromReturn(IOReturn rtn) override;
    virtual const char * stringFromReturn(IOReturn rtn) override;

    virtual IOService * getProvider( void ) const override;

#if __IO80211_TARGET >= __MAC_26_0
    // Bring-up probe: stamp IFNET_SUBFAMILY_WIFI onto our ifnet so it reports a Wi-Fi
    // functional type. Not virtual, not an override — see the derivation at the definition.
    void forceWiFiSubfamily(ifnet_t ifp);
#endif

private:
    IO80211SkywalkInterface *interface;
    bool isAttach;
};

#endif /* AirportItlwmEthernetInterface_hpp */
