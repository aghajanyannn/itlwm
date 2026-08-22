//
//  IO80211FaultReporter.h
//  itlwm
//
//  The object IO80211Controller::getFaultReporterFromDriver() (slot 432) must return.
//

#ifndef IO80211FaultReporter_h
#define IO80211FaultReporter_h

#include <libkern/c++/OSObject.h>

#include "CCFaultReporter.h"

// Apple's abstract base. Its slots 35..37 are pure virtual and IO80211Family calls them on
// objects Apple built, never on one this driver constructs — so they are deliberately not
// declared. Nothing here participates in a vtable; see CCDataStream.h.
class CommonFaultReporter : public OSObject {
};

// A driver that returns anything other than this class from slot 432 corrupts IO80211Family.
// IO80211Controller::findAndAttachToFaultReporter() stores the return value straight into
// controller ivars +0x58 (the field getCommonFaultReporter() reads) with no type check, and
// IO80211PeerManager::initWithInterface+0x10b5 then calls vtable slot 36 on it, expecting
// registerCallbacks(). Handing it a CCStream — an IOService, whose slot 36 is
// IORegistryEntry::copyProperty(OSString const*, IORegistryPlane const*, unsigned int) —
// dereferences the callback struct as an OSString and page-faults at 0x38.
//
// NULL is not an escape either: findAndAttachToFaultReporter panics at +0x10f, and the peer
// manager panics with "no ivars->_faultReporter". The reporter is mandatory.
//
// allocWithParams() returns NULL if its CCFaultReporter is NULL (IO80211FaultReporter::init
// tests it) — so the whole chain has to be built bottom-up and checked at each step.
class IO80211FaultReporter : public CommonFaultReporter {
public:
    static IO80211FaultReporter *allocWithParams(CCFaultReporter *);
};

#endif /* IO80211FaultReporter_h */
