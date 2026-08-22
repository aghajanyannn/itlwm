//
//  IOSkywalkPacketBufferPool.h
//  itlwm
//
//  Created by qcwap on 2023/6/15.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef IOSkywalkPacketBufferPool_h
#define IOSkywalkPacketBufferPool_h

#include <IOKit/IOService.h>

class IOSkywalkMemorySegment;
class IOSkywalkMemorySegmentDescriptor;
class IOSkywalkPacket;
class IOSkywalkPacketBuffer;
class IOSkywalkPacketDescriptor;
class IOSkywalkPacketBufferDescriptor;

// Declaration-only, on the rule in AGENTS.md: this driver obtains a pool from Apple's static
// factory and then only ever passes the pointer on — to the four queue factories and to
// registerInfraEthernetInterface. It never subclasses one, never calls a virtual on one, and
// never takes its size. So no virtual is declared here and this header creates no vtable and no
// per-release ABI obligation. The previous version declared 16 virtuals that nothing called,
// which was a liability with no upside; if this driver ever needs one, add it *and* verify its
// index against scripts/abi/abi-<ver>-<build>.txt, because calling a virtual pins every slot
// before it.
class IOSkywalkPacketBufferPool : public OSObject {
public:
    // Sized and laid out from the only producer that fills every field,
    // AppleEthernetRL::startInterface, cross-checked against the fields
    // IOSkywalkPacketBufferPool::initWithName actually reads (+0x00, +0x04, +0x08, +0x0c, +0x14).
    //
    // NOTE this struct was previously declared at 0x20 bytes with a `uint64_t pad` at +0x18.
    // That slot is a *pointer*, and the struct is 0x28. Nothing had constructed one yet, so the
    // error had cost nothing — it would have corrupted the first caller's stack frame. Sized from
    // the producer's writes, per the "size a struct from its allocation" rule in AGENTS.md.
    struct PoolOptions {
        uint32_t packetCount;       // +0x00  read by initWithName
        uint32_t bufferCount;       // +0x04  read by initWithName
        uint32_t bufferSize;        // +0x08  read by initWithName; 0x10000 tx, 0x4000 rx
        uint32_t maxBuffersPerPacket; // +0x0c  read by initWithName; AppleEthernetRL passes 1
        uint32_t _unk10;            // +0x10  producer zeroes it; initWithName does not read it
        uint32_t _unk14;            // +0x14  read by initWithName; AppleEthernetRL passes 1
        // +0x18 is a pointer to a memory/DMA spec struct, not padding. AppleEthernetRL builds one
        // on the stack: +0x00 a pointer loaded from a global, +0x08 the byte 0x40, +0x28 a u32,
        // +0x30 the driver's bus object. initWithName does not dereference it, so it is consumed
        // later — presumably when segments are created — and a pool can plausibly be built with it
        // NULL. Left opaque deliberately: naming a struct we have not decoded would invite a wrong
        // use, which is the trap _unk40 set in apple80211_assoc_candidates.
        const void *memorySpec;     // +0x18
        uint64_t _unk20;            // +0x20  producer zeroes it
    };

public:
    // Validates mode < 3 and options != NULL, then allocates. `mode` is the 1 every caller passes.
    static IOSkywalkPacketBufferPool *withName(char const *name, OSObject *owner, uint mode,
                                               IOSkywalkPacketBufferPool::PoolOptions const *options);
};

_Static_assert(sizeof(IOSkywalkPacketBufferPool::PoolOptions) == 0x28,
               "PoolOptions must be 0x28; +0x18 is a pointer, not padding");
_Static_assert(__builtin_offsetof(IOSkywalkPacketBufferPool::PoolOptions, bufferSize) == 0x08, "");
_Static_assert(__builtin_offsetof(IOSkywalkPacketBufferPool::PoolOptions, memorySpec) == 0x18, "");

#endif /* IOSkywalkPacketBufferPool_h */
