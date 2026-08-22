//
//  IOPCIEDeviceWrapper.cpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#include "IOPCIEDeviceWrapper.hpp"
#include "Apple80211.h"

#include "ItlIwm.hpp"
#include "ItlIwx.hpp"
#include "ItlIwn.hpp"

#define super IOService
OSDefineMetaClassAndStructors(IOPCIEDeviceWrapper, IOService);

#define  PCI_MSI_FLAGS        2    /* Message Control */
#define  PCI_CAP_ID_MSI        0x05    /* Message Signalled Interrupts */
#define  PCI_MSIX_FLAGS        2    /* Message Control */
#define  PCI_CAP_ID_MSIX    0x11    /* MSI-X */
#define  PCI_MSIX_FLAGS_ENABLE    0x8000    /* MSI-X enable */
#define  PCI_MSI_FLAGS_ENABLE    0x0001    /* MSI feature enabled */

static IOPMPowerState powerStateArray[2] =
{
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

static void pciMsiSetEnable(IOPCIDevice *device, UInt8 msiCap, int enable)
{
    UInt16 control;
    
    control = device->configRead16(msiCap + PCI_MSI_FLAGS);
    control &= ~PCI_MSI_FLAGS_ENABLE;
    if (enable)
        control |= PCI_MSI_FLAGS_ENABLE;
    device->configWrite16(msiCap + PCI_MSI_FLAGS, control);
}

static void pciMsiXClearAndSet(IOPCIDevice *device, UInt8 msixCap, UInt16 clear, UInt16 set)
{
    UInt16 ctrl;
    
    ctrl = device->configRead16(msixCap + PCI_MSIX_FLAGS);
    ctrl &= ~clear;
    ctrl |= set;
    device->configWrite16(msixCap + PCI_MSIX_FLAGS, ctrl);
}

extern IOWorkLoop *_fWorkloop;

IOWorkLoop *IOPCIEDeviceWrapper::getWorkLoop() const
{
    return _fWorkloop;
}

IOService* IOPCIEDeviceWrapper::
probe(IOService *provider, SInt32 *score)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    bool isMatch = false;
    super::probe(provider, score);
    // findPCICapability() writes *offset only when the capability exists, so these must be
    // initialised: uninitialised stack bytes here become the offset of a config-space
    // WRITE below, corrupting whatever register the garbage happens to name.
    UInt8 msiCap = 0;
    UInt8 msixCap = 0;
    IOPCIDevice* device = OSDynamicCast(IOPCIDevice, provider);
    if (!device)
        return NULL;
    if (ItlIwx::iwx_match(device)) {
        isMatch = true;
        fHalService = new ItlIwx;
    }
    if (!isMatch && ItlIwm::iwm_match(device)) {
        isMatch = true;
        fHalService = new ItlIwm;
    }
    if (!isMatch && ItlIwn::iwn_match(device)) {
        isMatch = true;
        fHalService = new ItlIwn;
    }
    if (isMatch) {
        XYLog("%s Found\n", __FUNCTION__);
        if (!device->findPCICapability(PCI_CAP_ID_MSIX, &msixCap))
            msixCap = 0;
        if (msixCap)
            pciMsiXClearAndSet(device, msixCap, PCI_MSIX_FLAGS_ENABLE, 0);
        if (!device->findPCICapability(PCI_CAP_ID_MSI, &msiCap))
            msiCap = 0;
        if (msiCap)
            pciMsiSetEnable(device, msiCap, 1);
        if (!msiCap && !msixCap) {
            XYLog("%s No MSI cap\n", __FUNCTION__);
            fHalService->release();
            fHalService = NULL;
            return NULL;
        }
        this->pciNub = device;
        return this;
    }
    return NULL;
}

bool IOPCIEDeviceWrapper::
start(IOService *provider)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    _fWorkloop = IO80211WorkQueue::workQueue();
    if (!super::start(provider)) {
        return false;
    }
    IOLog("%s::super start succeed\n", getName());
    UInt8 builtIn = 0;
    setProperty("built-in", OSData::withBytes(&builtIn, sizeof(builtIn)));
    PMinit();
    registerPowerDriver(this, powerStateArray, 2);
    provider->joinPMtree(this);
#if defined(__IO80211_TARGET) && __IO80211_TARGET >= __MAC_26_0
    // Publish this nub a few seconds late rather than immediately, which delays everything
    // downstream — AirportItlwm::probe/start, initCCLogs(), and Apple's
    // IO80211Controller::start() — until after the boot-time storm of IOKit matching and
    // the root filesystem mount.
    //
    // This is REQUIRED on Tahoe, not a tuning knob: without it the boot hangs. Creating
    // even a single CCPipe before super::start() hangs reliably, creating none boots
    // reliably, and Tahoe is the only target forced to build the log pipes that early
    // (IO80211ControllerMonitor rejects a NULL getLogger()). Deferring restores the
    // ordering every other target already has, without needing to understand CoreCapture's
    // internals. Default is 1 s because that is the value actually observed to boot; 3 s
    // has not been proven on its own (the one build that used it also carried other
    // changes). See include/Airport/AGENTS.md for the full bisection.
    //
    // thread_call rather than an IOTimerEventSource: it cannot block this start(). It is NOT
    // free of teardown, though — the handle must be kept so stop() can cancel it, and the
    // callback's `this` must be retained for the whole window. Held as a stack local with no
    // retain, a stop() inside the delay left publishLater calling registerService() on a freed
    // object, and leaked the allocation. The window is as wide as itldefer, i.e. 30 s in the
    // configuration bring-up is being run at.
    // Disable with -itlnodefer to A/B it; override the delay with itldefer=<seconds>.
    int deferSecs = 1;
    int noDefer = 0;
    if (!PE_parse_boot_argn("-itlnodefer", &noDefer, sizeof(noDefer))) {
        if (!PE_parse_boot_argn("itldefer", &deferSecs, sizeof(deferSecs))
            || deferSecs <= 0 || deferSecs > 60)
            deferSecs = 1;
        setProperty("ItlwmDeferPublish", (UInt64)deferSecs, 32);
        fPublishCall = thread_call_allocate(&IOPCIEDeviceWrapper::publishLater, this);
        if (fPublishCall) {
            uint64_t deadline = 0;
            clock_interval_to_deadline(deferSecs, kSecondScale, &deadline);
            retain();   // balanced by publishLater, or by stop() if it cancels first
            thread_call_enter_delayed(fPublishCall, deadline);
            return true;    // registerService() happens from the thread_call
        }
    }
#endif
    registerService();
    return true;
}

#if defined(__IO80211_TARGET) && __IO80211_TARGET >= __MAC_26_0
void IOPCIEDeviceWrapper::
publishLater(thread_call_param_t self, thread_call_param_t)
{
    IOPCIEDeviceWrapper *me = (IOPCIEDeviceWrapper *)self;
    me->registerService();
    me->release();   // balances the retain start() took before arming; must be last
}
#endif

void IOPCIEDeviceWrapper::
stop(IOService *provider)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
#if defined(__IO80211_TARGET) && __IO80211_TARGET >= __MAC_26_0
    if (fPublishCall) {
        // TRUE means it was still pending and has been dequeued, so it will never run and its
        // reference is ours to drop. FALSE means it already ran and released itself.
        if (thread_call_cancel(fPublishCall))
            release();
        // Refuses and returns FALSE if the call is executing right now, in which case the
        // allocation leaks rather than being freed under a live callback. That is the safe
        // trade, and the retain above is what guarantees `this` outlives the callback either
        // way. Not using thread_call_cancel_wait: it requires a THREAD_CALL_OPTIONS_ONCE call.
        thread_call_free(fPublishCall);
        fPublishCall = NULL;
    }
#endif
    PMstop();
    super::stop(provider);
}

IOReturn IOPCIEDeviceWrapper::
setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice)
{
    return IOPMAckImplied;
}
