//
//  callprobe.cpp — input to scripts/abi/callcheck.sh. Not built into any kext.
//
//  Verifies the classes this driver calls *virtuals on* but never subclasses. `mapdrv.py` cannot:
//  it checks the slots we override, and these are slots we invoke. Getting one wrong dispatches a
//  correct-looking call to a different method, with no build or load-time symptom.
//
//  A class with no key function emits no vtable, so each probe below supplies exactly one
//  out-of-line virtual to force emission. Override the DEEPEST slot the driver calls: clang only
//  prints entries up to the table's end, and a shallow override still prints the full inherited
//  table, but overriding the deepest one makes the intent obvious to the next reader.
//
//  IOSkywalkPacket USED TO BE PROBED HERE and deliberately is not any more. Every method the
//  driver calls on a packet is an exported symbol as well as a vtable entry, so those calls are
//  now direct and non-virtual, and the linker checks them on every build — a stronger check than
//  this one, and it cannot be forgotten. There is nothing left to verify: the class declares no
//  virtuals. Re-add a probe here the moment anything calls a packet method through the vtable.
//
#include <IOKit/IOService.h>
#include <IOKit/IOCommand.h>
#include "Airport/IOSkywalkDataPath.h"

class ProbeSegment : public IOSkywalkMemorySegment {
public:
    virtual uint64_t getIOVirtualAddress(void) override;
};
uint64_t ProbeSegment::getIOVirtualAddress(void) { return 0; }

//  The driver enables and disables all four queues (they are constructed DISABLED and refuse
//  everything until enabled — see IOSkywalkDataPath.h). Those are IOEventSource slots 42/43,
//  inherited rather than introduced by IOSkywalkPacketQueue, so what this probe really checks is
//  that MacKernelSDK's IOEventSource lays out the same as Apple's. Nothing else in this repo does:
//  a shifted IOEventSource would send enable() to some other method with no build or load symptom,
//  and the whole data path would stay silently inert — which is the exact bug this probe was added
//  for. Overriding disable() (the deeper of the two) forces the table out of line.
class ProbeQueue : public IOSkywalkPacketQueue {
public:
    virtual void disable(void) override;
};
void ProbeQueue::disable(void) { }
