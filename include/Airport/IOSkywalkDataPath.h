//
//  IOSkywalkDataPath.h
//  itlwm
//
//  The Skywalk data-path objects a real registerEthernetInterface needs: the packet types the
//  queue callbacks are handed, and the four queue factories themselves.
//
//  WHY THIS HEADER IS CHEAP, which is the whole reason full registration is reachable at all:
//  every object here is produced by an *exported static factory*, and every data-path callback is
//  a plain C function pointer (Apple passes non-virtual member functions, with the owner as the
//  `OSObject *target` argument). So the driver subclasses nothing.
//
//  THIS FILE NOW CARRIES NO VTABLE OBLIGATION AT ALL, and that is a deliberate change rather than
//  an accident of what has been written so far. IOSkywalkPacket used to declare slots 35..42
//  `virtual`, and IOSkywalkMemorySegment still does below; both were described as per-release ABI
//  obligations that `mapdrv.py` could not check, because it checks slots this driver *overrides*
//  and these are slots it *calls*. Every method the driver actually needs turned out to be an
//  exported symbol as well as a vtable entry, so it is now declared non-virtual and called
//  directly. See the rule at IOSkywalkPacket: a shifted slot misdispatches at runtime, a missing
//  symbol fails at link time, and only one of those can panic a machine at boot.
//
//  IOSkywalkMemorySegment below is the last class here that still declares virtuals. Nothing calls
//  them — the TX path reaches the same address through IOSkywalkPacket::getDataVirtualAddress,
//  which does segment->getVirtualAddress() + buffer offset internally — so the declarations are
//  inert. Do not call one without first converting it the same way.
//
#ifndef IOSkywalkDataPath_h
#define IOSkywalkDataPath_h

#ifndef __IO80211_TARGET
#error "Define __IO80211_TARGET"
#endif

#include <IOKit/IOCommand.h>
#include <IOKit/IOService.h>
#include <IOKit/IOEventSource.h>

class IOSkywalkPacketBufferPool;
class IOSkywalkPacketDescriptor;
class IOSkywalkMemorySegmentDescriptor;
class IOSkywalkPacketBuffer;
class IOSkywalkMemorySegment;
class IOSkywalkPacket;

// ---------------------------------------------------------------------------------------------
// Opaque. The driver holds IOSkywalkPacketQueue* only to put them in the array it hands to
// registration, which is also what lets IOSkywalkQueueSet and IOSkywalkLogicalLink stay
// unreconstructed: the queue-array overload of registerNetworkInterface builds both internally.
// Declaration-only on purpose — see AGENTS.md.
// ---------------------------------------------------------------------------------------------
// **This is an IOEventSource, not an IOService**, and the difference is load-bearing rather than
// cosmetic: slot 36 is `IOEventSource::checkForWork` and slot 37 `setWorkLoop`. An event source is
// only ever polled once it has been added to a work loop, so a queue that is constructed, handed to
// registration and never added to one is inert — every callback reads 0 while the nexus, the
// flowswitch, the queue set and the logical link all look correct. That was measured on 26.6 and
// cost several boots chasing family-side triggers that were never the problem.
// This class was declared `: public IOService` here until then, which is what made the omission
// invisible: `addEventSource` would not have compiled.
// EVERY QUEUE IS BORN DISABLED, and nothing in the family ever enables it. IOSkywalkPacketQueue::
// initWithPool writes `byte[this + 0x28] = 0` immediately after IOEventSource::init set it to 1 —
// that byte IS IOEventSource::enabled (isEnabled() is literally `byte[this+0x28] & 1`). The
// consequences are total and completely silent:
//
//   - IOSkywalkTxSubmissionQueue::packetSubmission(bool), which is exactly what the netif's
//     nxp_tx_qset_notify calls, opens with `test al,1 / je bail` and returns without doing anything.
//   - IOSkywalkRxSubmissionQueue::requestDequeue returns 0xe00002d7 instead of pulling buffers.
//   - IOSkywalk{Tx,Rx}CompletionQueue::enqueuePackets returns 0xe00002d7 instead of delivering.
//
// So a disabled queue set produces a data path that is assembled perfectly — nexus, flowswitch,
// queue set, logical link, netif llink, all four queues bound — and moves not one packet, with no
// error raised anywhere. Measured on 26.6 across several boots.
//
// Apple's own driver has a function named for precisely this step:
// AppleBCMWLANPCIeSkywalk::enableAllSubmissionQueue(), called from
// AppleBCMWLANBusInterfacePCIe::FWSetupDone() — i.e. after firmware bring-up, NOT at registration.
// It enables each TX submission queue, enables the RX submission queue, and then calls
// requestDequeue on it to pull the first batch of RX buffers.
//
// enable()/disable() are IOEventSource slots 42/43 and are called virtually; requestDequeue is
// exported and non-virtual here for the reason given at IOSkywalkPacket.
class IOSkywalkPacketQueue : public IOEventSource {
public:
    // Non-virtual and exported; AppleBCMWLANSkywalkInterface::start calls it to map each queue
    // back to its index. Costs no slot.
    uint32_t getQueueId(void) const;
};

// ---------------------------------------------------------------------------------------------
// IOSkywalkPacket — DECLARATION-ONLY, and deliberately so. This class used to declare slots 35..42
// as `virtual` and was described as one of this file's two per-release ABI obligations. It is not
// one any more, and the reasoning generalises:
//
//   Every method this driver calls on a packet is *also an exported symbol*. Declaring it
//   non-virtual makes the compiler emit a direct call to that symbol instead of an indexed load
//   from the vtable. The two are equivalent whenever no subclass overrides the method — and none
//   does: in the whole 26.6 collection, slots 40, 42, 47 and 48 resolve to the base implementation
//   on both IOSkywalkPacket and IOSkywalkNetworkPacket, and nothing else derives from either.
//
// **Rule: prefer a direct call to an exported symbol over a vtable slot, when nothing overrides
// it.** The two disagree only in how they fail, and the failure modes are not comparable: a slot
// that has shifted silently calls the wrong function at runtime — the failure mode this whole
// repo exists to defend against — whereas a symbol that has been renamed or removed fails at
// *link time*, on the build machine, before anything can boot. Trading a kernel panic for a
// linker error is the entire trade, and it also means these methods need no re-porting per
// release: `kextlibs`/link failure is the check, and it runs on every build for free.
//
// The caveat, which is the only thing to re-check on a new release: if Apple ever ships a packet
// subclass that *overrides* one of these, a direct call would bypass the override and silently
// read the wrong field. Verify with:
//   grep -E "^[A-Za-z0-9_]+ (40|42|47) __ZNK15IOSkywalkPacket" scripts/abi/abi-<ver>-<build>.txt
// and confirm it lists only IOSkywalkPacket and IOSkywalkNetworkPacket.
//
// Only what the driver actually calls is declared. Nothing here costs a slot, so the old rule
// about "stopping early is the conservative choice" no longer applies — but declaring unused
// methods still invites unverified use, so the list stays minimal.
// ---------------------------------------------------------------------------------------------
class IOSkywalkPacket : public IOCommand {
public:
    // Sum of every buffer's data length. With maxBuffersPerPacket == 1 that is the frame length.
    uint32_t getDataLength(void) const;
    // Offset of the data *within the first buffer*. NOT included in getDataVirtualAddress below.
    uint16_t getDataOffset(void) const;
    // Kernel-virtual base of the packet's first buffer. Implemented as
    //   segment->getVirtualAddress() + buffer->getMemorySegmentOffset()
    // so it does NOT add getDataOffset(): the frame starts at
    //   getDataVirtualAddress() + getDataOffset()
    // Getting that wrong ships frames shifted by the headroom, which looks like link corruption
    // rather than like a driver bug.
    uint64_t getDataVirtualAddress(void) const;
    // RX: declare how much of the buffer the received frame occupies. Returns kIOReturnBadArgument
    // unless the packet has exactly ONE buffer — it forwards to IOSkywalkPacketBuffer::setDataLength
    // only when packet[0x64] == 1 — which is why the pools are built with maxBuffersPerPacket = 1.
    // It writes the same field getDataLength() sums, so the two agree by construction.
    IOReturn setDataLength(uint32_t);
    IOReturn setDataOffset(uint16_t);
    // Packets arrive from the submission queues as a singly-linked list, not an array.
    IOSkywalkPacket *getNextPacket(void);
    void setNextPacket(IOSkywalkPacket *);
};

// ---------------------------------------------------------------------------------------------
// IOSkywalkMemorySegment — base is OSObject; own slots start at 35. The TX callback calls slot 42
// (getIOVirtualAddress) to turn a packet buffer into a DMA address.
// ---------------------------------------------------------------------------------------------
class IOSkywalkMemorySegment : public OSObject {
public:
    virtual bool initWithPool(IOSkywalkPacketBufferPool *, IOSkywalkMemorySegmentDescriptor *, uint); // 35
    virtual IOReturn setDMACommand(IODMACommand *, uint);                                        // 36
    virtual IODMACommand *getDMACommand(void) const;                                             // 37
    virtual IOReturn prepare(uint);                                                              // 38
    virtual IOReturn complete(uint);                                                             // 39
    virtual IOReturn setBufferMemoryDescriptor(IOBufferMemoryDescriptor *);                      // 40
    virtual IOReturn setMemoryDescriptor(IOMemoryDescriptor *, uint64_t);                        // 41
    virtual uint64_t getIOVirtualAddress(void);                                                  // 42
};

// ---------------------------------------------------------------------------------------------
// Declaration-only: both accessors the TX callback uses are exported and NON-virtual, so this
// class costs no slot at all. Do not add virtuals here to "complete" it.
// ---------------------------------------------------------------------------------------------
class IOSkywalkPacketBuffer : public IOCommand {
public:
    IOSkywalkMemorySegment *getMemorySegment(void) const;
    uint64_t getMemorySegmentOffset(void) const;
};

// ---------------------------------------------------------------------------------------------
// The four queues. Declaration-only: the driver calls the static factory, keeps the pointer, and
// hands it to registration. Behaviour arrives through the callback, not through a subclass.
//
// The callback's first argument is the `OSObject *target` passed to the factory, which is how
// Apple passes a non-virtual member function here — its implicit `this` lands in that slot. Ours
// can do the same, or be a static.
//
// getEffectiveCapacity is a static helper used to size the pool: Apple sums the submission and
// completion capacities for a requested ring size (0x100) and uses that as the packet/buffer
// count. Do not invent a pool size — derive it the way the producer does.
// ---------------------------------------------------------------------------------------------
// THE `const` ON THE HANDLER'S `packets` IS THE MODE SELECTOR. Apple ships two overloads of every
// submission-queue factory, distinguished only by that qualifier, and the non-const one is a
// six-instruction shim that ORs the list-mode bit into `options` and tails into the const one:
//
//   withPool(..., IOSkywalkPacket *const *, ...)   legacyDequeue — `packets` is a REAL ARRAY of
//                                                  `count` pointers out of the ring slot table;
//                                                  their next-pointers are stale ring state.
//   withPool(..., IOSkywalkPacket **, ...)         listDequeue  — `packets` is the ADDRESS OF THE
//                                                  HEAD POINTER of a setNextPacket()-chained list,
//                                                  re-passed unadvanced on every iteration, so a
//                                                  partial consume MUST store the new head back.
//
// So the qualifier is not decoration to preserve, and it is not something to const_cast away: it
// picks which of two incompatible meanings `packets` has. Declare the overload whose contract the
// handler implements and pass 0 for `options` — the shim supplies the bit.
//
// The bit VALUE differs per queue (TX 8 at [q+0x142], RX 2 at bit 20 of [q+0x12c]) and neither is
// reachable by guessing, which is the second reason to let the shim set it. Passing TX's 2 by hand
// sets an unrelated notification-mode bit, leaves legacyDequeue selected, and the list handler then
// walks the ring array with getNextPacket() into garbage — measured on 26.6 as a trap at
// IOSkywalkTxCompletionQueue::enqueuePackets+0x9d, a virtual call on a NULL vtable, CR2 = 0x140.
//
// Apple's own AppleEthernetRL and AppleConvergedIPCSkywalkInterface take the const/legacy overload
// with options = 0 for all four queues; the list overloads have no in-kernel caller. Both modes are
// fully implemented, so this is a choice, not a supported-vs-unsupported split.
class IOSkywalkTxSubmissionQueue : public IOSkywalkPacketQueue {
public:
    typedef uint32_t (*DequeueHandler)(OSObject *target, IOSkywalkTxSubmissionQueue *queue,
                                       IOSkywalkPacket **packets, uint32_t count, void *refcon);
    // The optional budget callback, [queue+0xf0]. packetSubmission() calls it, when non-NULL, to
    // clamp how much it will dequeue. Apple's short const overload hardcodes NULL here, and the
    // trailing -1 below, so passing those two values reproduces its behaviour exactly. There is no
    // short non-const overload, which is the only reason this call site is the long shape.
    typedef uint32_t (*BudgetHandler)(OSObject *target, IOSkywalkTxSubmissionQueue *queue,
                                      uint32_t *inout);
    static uint32_t getEffectiveCapacity(uint32_t requested);
    static IOSkywalkTxSubmissionQueue *withPool(IOSkywalkPacketBufferPool *pool, uint32_t capacity,
                                                uint32_t flags, OSObject *target,
                                                BudgetHandler budget, DequeueHandler handler,
                                                void *refcon, uint32_t options, uint32_t reserved);
};

class IOSkywalkTxCompletionQueue : public IOSkywalkPacketQueue {
public:
    typedef uint32_t (*EnqueueHandler)(OSObject *target, IOSkywalkTxCompletionQueue *queue,
                                       IOSkywalkPacket **packets, uint32_t count, void *refcon);
    static uint32_t getEffectiveCapacity(uint32_t requested);
    static IOSkywalkTxCompletionQueue *withPool(IOSkywalkPacketBufferPool *pool, uint32_t capacity,
                                                uint32_t flags, OSObject *target,
                                                EnqueueHandler handler, void *refcon, uint32_t options);

    // How a packet taken from the TX submission queue is given back: it recycles the packet to the
    // pool and tells the nexus the transmit finished. Without it the pool drains and TX wedges.
    //
    // This one IS virtual in Apple's binary (slot 85, with an array-taking sibling at 84), and is
    // declared non-virtual here for the reason given at IOSkywalkPacket — the symbol is exported,
    // nothing subclasses IOSkywalkTxCompletionQueue, and our own factory call returns exactly this
    // class, so a direct call and a slot call cannot diverge.
    //
    // Takes the *head of a linked list* plus a count, walks it with getNextPacket(), and rejects
    // a NULL head or a zero count with kIOReturnBadArgument. It closes the completion queue's own
    // gate, which is NOT the submission queue's gate — see the ordering note at the call site.
    IOReturn enqueuePackets(IOSkywalkPacket *head, uint32_t count, uint32_t options);
};

// Note the extra uint third parameter — this factory's signature differs from the other three.
// Taking the Tx shape on trust here would push every later argument one register left.
//
// The non-const `IOSkywalkPacket **` below is the list-mode overload, per the note above
// IOSkywalkTxSubmissionQueue; its shim ORs 2. Unlike TX there IS a short non-const overload, so
// this call site keeps the same shape as the const one.
class IOSkywalkRxSubmissionQueue : public IOSkywalkPacketQueue {
public:
    typedef uint32_t (*DequeueHandler)(OSObject *target, IOSkywalkRxSubmissionQueue *queue,
                                       IOSkywalkPacket **packets, uint32_t count, void *refcon);
    static uint32_t getEffectiveCapacity(uint32_t requested);
    static IOSkywalkRxSubmissionQueue *withPool(IOSkywalkPacketBufferPool *pool, uint32_t capacity,
                                                uint32_t a, uint32_t b, OSObject *target,
                                                DequeueHandler handler, void *refcon, uint32_t options);

    // Ask the queue to hand the driver a batch of empty buffers, i.e. drive DequeueHandler now.
    // Nothing else ever starts RX: the stack does not push buffers at the driver, the driver pulls
    // them. Apple calls this once from enableAllSubmissionQueue, straight after enable().
    //
    //   flags & 1  -> asynchronous; signals the work loop and returns 0. Safe from any context.
    //   flags == 0 -> synchronous; closes this queue's gate and dequeues inline.
    //
    // Returns 0xe00002d7 when the queue is disabled, and returns without doing anything at all when
    // the queue belongs to no work loop ([this+0x30] == NULL) — so addEventSource is a precondition,
    // not an optimisation.
    IOReturn requestDequeue(void *arg, uint32_t flags);
};

class IOSkywalkRxCompletionQueue : public IOSkywalkPacketQueue {
public:
    typedef uint32_t (*EnqueueHandler)(OSObject *target, IOSkywalkRxCompletionQueue *queue,
                                       IOSkywalkPacket **packets, uint32_t count, void *refcon);
    static uint32_t getEffectiveCapacity(uint32_t requested);
    static IOSkywalkRxCompletionQueue *withPool(IOSkywalkPacketBufferPool *pool, uint32_t capacity,
                                                uint32_t flags, OSObject *target,
                                                EnqueueHandler handler, void *refcon, uint32_t options);

    // How a filled RX packet is delivered upstream. Virtual at slot 85 in Apple's binary with an
    // array-taking sibling at 84, declared non-virtual here for the reason given at
    // IOSkywalkPacket. Takes the head of a linked list plus a count.
    IOReturn enqueuePackets(IOSkywalkPacket *head, uint32_t count, uint32_t options);
};

#endif /* IOSkywalkDataPath_h */
