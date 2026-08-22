//
//  IOPCIEDeviceWrapper.hpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef IOPCIEDeviceWrapper_hpp
#define IOPCIEDeviceWrapper_hpp

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOTypes.h>

#include <HAL/ItlHalService.hpp>

class IOPCIEDeviceWrapper : public IOService {
    OSDeclareDefaultStructors(IOPCIEDeviceWrapper)
    
public:
    virtual IOService* probe(IOService* provider, SInt32* score) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual IOWorkLoop* getWorkLoop() const override;
    virtual IOReturn setPowerState(
        unsigned long powerStateOrdinal,
                                   IOService *   whatDevice ) override;
#if defined(__IO80211_TARGET) && __IO80211_TARGET >= __MAC_26_0
    // Tahoe bring-up: deferred registerService() for the -itldefer experiment.
    static void publishLater(thread_call_param_t self, thread_call_param_t);
#endif

public:
    ItlHalService *fHalService;
    IOPCIDevice *pciNub;
#if defined(__IO80211_TARGET) && __IO80211_TARGET >= __MAC_26_0
    // Must be a member: an armed thread_call has to be cancellable from stop(). Held as a
    // stack local it could not be, which left registerService() firing on a freed object.
    thread_call_t fPublishCall;
#endif
};

#endif /* IOPCIEDeviceWrapper_hpp */
