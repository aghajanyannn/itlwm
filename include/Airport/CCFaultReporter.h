//
//  CCFaultReporter.h
//  itlwm
//
//  CoreCapture's fault reporter. The object IO80211FaultReporter wraps.
//

#ifndef CCFaultReporter_h
#define CCFaultReporter_h

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>

#include "CCDataStream.h"

// Declaration-only; see the note in CCDataStream.h. Only the factory is used.
//
// withStreamWorkloop() returns NULL unless BOTH arguments are non-NULL — it tests them
// before doing anything else (CCFaultReporter::initWithStreamWorkloop+0x24, 26.6/25G72).
// It stores and retains the stream (ivars+0x48) and the workloop (ivars+0x50), so the
// caller may release its own references once the reporter is alive; this driver keeps them
// anyway so releaseAll() can tear the chain down in one place.
//
// The workloop must be a real IOWorkLoop, not an IO80211WorkQueue: CoreCapture arms a
// deferred-capture timer event source on it.
class CCFaultReporter : public IOService {
public:
    static CCFaultReporter *withStreamWorkloop(CCDataStream *,IOWorkLoop *);
};

#endif /* CCFaultReporter_h */
