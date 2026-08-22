//
//  IO80211FlowQueueHash.h
//  itlwm
//

#ifndef IO80211FlowQueueHash_h
#define IO80211FlowQueueHash_h

#include <libkern/OSTypes.h>

// Apple passes this by value and forwards it straight to
// IO80211FlowQueueDatabase::find(unsigned long long), so it is one 64-bit word and
// is register-passed exactly like a UInt64. It has to be a *class type* rather than
// a typedef, because it appears in the mangled names of the vtable slots that bind
// to IO80211Family:
//
//   IO80211SkywalkInterface::findOrCreateFlowQueue(IO80211FlowQueueHash)
//   IO80211SkywalkInterface::findOrCreateFlowQueueWithCache(IO80211FlowQueueHash, bool*)
//   IO80211SkywalkInterface::findExistingFlowQueue(IO80211FlowQueueHash)
//   IO80211SkywalkInterface::removePacketQueue(IO80211FlowQueueHash*)
//   IO80211SkywalkInterface::removePacketQueue(IO80211FlowQueueHash const*)
//
// A `typedef UInt64` mangles those five slots as `y`/`Py`/`PKy`, which matches no
// exported symbol, so each one linked as a null vtable entry.
//
// The conversions keep it trivially copyable (and therefore still register-passed),
// while letting callers keep treating the hash as a plain 64-bit value.
struct IO80211FlowQueueHash {
    UInt64 value;

    IO80211FlowQueueHash(UInt64 v = 0) : value(v) {}
    operator UInt64() const { return value; }
};

static_assert(sizeof(IO80211FlowQueueHash) == sizeof(UInt64), "Invalid hash size");
static_assert(__is_trivially_copyable(IO80211FlowQueueHash), "Must stay register-passed");

#endif /* IO80211FlowQueueHash_h */
