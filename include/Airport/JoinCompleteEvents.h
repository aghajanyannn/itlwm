//
//  JoinCompleteEvents.h
//  itlwm
//
//  Tahoe join-completion event payloads. Reconstructed, not from any SDK.
//

#ifndef JoinCompleteEvents_h
#define JoinCompleteEvents_h

#include <sys/cdefs.h>
#include <stdint.h>

// The two messages a driver must post to move the WCL's JOIN_MANAGER FSM. Unlike
// AssocCandidates.h — which Apple writes and we read — **these travel the other way**: we build
// them, so a field left wrong is a field the family acts on. Both are plain data with no vtable,
// but they are ABI: the offsets and the total sizes are pinned below.
//
//     APPLE80211_M_WCL_AUTH_ASSOC_COMPLETE (211)  apple80211_assoc_event                (0x1c)
//     APPLE80211_M_WCL_CONNECT_COMPLETE    (213)  apple80211_connection_complete_event   (0xa4)
//
// Recovered from the consumers, which are the authority on layout:
//     WCLJoinManager::authAssocCompleteEventHandler -> ::handleJoinAssocComplete
//                                                   -> WCLJoinRequest::updateAuthAssocStatus
//     WCLJoinManager::connectCompleteEventHandler   -> ::handleJoinConnectComplete
//                                                   -> WCLJoinRequest::updateConnectCompleteEvent
// The struct type names above are Apple's own, taken from those two mangled symbols.
//
// **The length is part of the contract, not documentation.** Each handler sets its reply byte,
// then requires a non-NULL payload *and* an exact length, and only raises the FSM event if both
// hold. A wrong length returns kIOReturnError and raises **no event at all**, which is
// indistinguishable from never having posted the message — the FSM simply stays where it was. Post
// these with sizeof() and nothing else, and never a truncated or padded copy.
//
// Both structs are **packed**: the 64-bit fields sit at offsets 0x0c and 0x14, which are not
// 8-aligned, and the totals (0x1c, 0xa4) are not multiples of 8. Without the attribute the
// compiler inserts padding, every later offset shifts, and the length check then rejects the
// message. The _Static_asserts below exist to catch exactly that.

// Message 211. Reports the outcome of the authentication/association exchange.
//
// `status == 0` is success. On the zero path handleJoinAssocComplete calls notifyAssocDone() and
// the FSM advances to JOIN_MANAGER_STATE_ASSOC_DONE. A non-zero status is a failure, and the
// specific value 1000 raises JOIN_MANAGER_EVENT_JOIN_ABORT_REQ instead.
//
// `auth_phase` selects which of the two timestamps the WCL reads, and sets a companion validity
// bit on the join request for the one it took — the same discard-unless-flagged shape that cost a
// boot cycle in BeaconMetaData. Non-zero also routes `reason` into
// WCLJoinManager::debugCCOnAuthFailures, so it reads as "this event reports the auth phase".
struct apple80211_assoc_event {
    uint16_t status;            // 0x00  0 == success; 1000 == abort. -> joinRequest+0x06
    uint16_t reason;            // 0x02  status detail; logged via debugCCOnAuthFailures
    uint8_t  auth_phase;        // 0x04  != 0 selects auth_time, == 0 selects assoc_time
    uint8_t  bssid[6];          // 0x05  deliberately unaligned; memcmp'd against the target BSS
    uint8_t  _pad0b;            // 0x0b
    uint64_t auth_time;         // 0x0c  taken when auth_phase != 0
    uint64_t assoc_time;        // 0x14  taken when auth_phase == 0
} __attribute__((packed));

_Static_assert(sizeof(struct apple80211_assoc_event) == 0x1c, "assoc_event must be 0x1c bytes");
_Static_assert(__builtin_offsetof(struct apple80211_assoc_event, status)     == 0x00, "status");
_Static_assert(__builtin_offsetof(struct apple80211_assoc_event, reason)     == 0x02, "reason");
_Static_assert(__builtin_offsetof(struct apple80211_assoc_event, auth_phase) == 0x04, "auth_phase");
_Static_assert(__builtin_offsetof(struct apple80211_assoc_event, bssid)      == 0x05, "bssid");
_Static_assert(__builtin_offsetof(struct apple80211_assoc_event, auth_time)  == 0x0c, "auth_time");
_Static_assert(__builtin_offsetof(struct apple80211_assoc_event, assoc_time) == 0x14, "assoc_time");

// Message 213. Reports that the connection is up, and is what ends the join.
//
// `status == 0` is the whole success condition: WCLJoinManager::isJoinProcessDone short-circuits to
// "done" on a zero status, and handleJoinConnectComplete then raises
// JOIN_MANAGER_EVENT_JOIN_COMPLETE (6) itself — the driver does not post that one.
//
// A non-zero status does *not* end the join. isJoinProcessDone instead calls
// WCLJoinRequest::updateAndCheckForNextCandidate and treats the join as done only if that returns
// negative or 35000 ms have passed since the join began; otherwise the FSM raises
// TRY_NEXT_CANDIDATE (13), or TRY_NEXT_CANDIDATE_DELAYED (14) with a 1000 ms delay. So reporting a
// failure here is a supported outcome that walks the candidate list, not a dead end.
//
// Only status, reason and timestamp are read — verified by disassembling the sole consumer of the
// payload pointer, updateConnectCompleteEvent, and confirming handleJoinConnectComplete passes
// locals rather than the payload to every subsequent processEvent call. The trailing bytes must
// still be *present*, because of the exact-length check; zero them.
struct apple80211_connection_complete_event {
    uint16_t status;            // 0x00  0 == success -> JOIN_COMPLETE. -> joinRequest+0x06
    uint16_t reason;            // 0x02  status detail.                 -> joinRequest+0x08
    uint8_t  _unk04[8];         // 0x04  not read by the WCL
    uint64_t timestamp;         // 0x0c  stored with a validity byte set alongside it
    uint8_t  _unk14[0x90];      // 0x14  not read by the WCL
} __attribute__((packed));

_Static_assert(sizeof(struct apple80211_connection_complete_event) == 0xa4,
               "connection_complete_event must be 0xa4 bytes");
_Static_assert(__builtin_offsetof(struct apple80211_connection_complete_event, status)    == 0x00, "status");
_Static_assert(__builtin_offsetof(struct apple80211_connection_complete_event, reason)    == 0x02, "reason");
_Static_assert(__builtin_offsetof(struct apple80211_connection_complete_event, timestamp) == 0x0c, "timestamp");

// Message 216. Moves WCLNetManager's FSM, which is the FSM the rest of the system watches: until it
// leaves NET_MANAGER_STATE_LINK_DOWN the interface is link-down to BSD and to airportd no matter
// what the JOIN_MANAGER FSM did.
//
// Unlike 211/213 the length is *not* self-evidently checked, but both of Apple's producers pass
// exactly 0x10 — AppleBCMWLANNetAdapter::handleLink (a firmware link event) and
// ::sendInternalLinkDownInd (a driver-originated link down) — so post 0x10 and nothing else.
// Both also pass the trailing `bool` to IO80211Controller::postMessage as **true**.
//
// Layout recovered from those two producers plus the two consumers, WCLNetManager::linkStatusInd
// and ::linkUp. handleLink copies bytes 0..5 straight out of `wl_event_msg_t.addr` as a dword and a
// word, which is what identifies them as the BSSID.
struct apple80211_link_status_ind {
    uint8_t  bssid[6];          // 0x00  wl_event_msg_t.addr; zeroed on the internal link-down path
    uint8_t  link_up;           // 0x06  **the whole switch.** != 0 -> LINK_UP, == 0 -> LINK_DOWN_IND
    uint8_t  _unk07;            // 0x07  a bool the adapter reads off the interface; 0 is accepted
    uint32_t reason;            // 0x08  link-change reason. handleLink maps the firmware's reason to
                                //       0..8 and uses 0xff for anything else;
                                //       sendInternalLinkDownInd uses 9 for "the driver brought the
                                //       link down itself", which is this driver's case exactly.
    uint32_t bss_change_reason; // 0x0c  read by WCLNetManager::linkUp and handed to updateBss().
                                //       Both Apple producers write 0 here.
} __attribute__((packed));

_Static_assert(sizeof(struct apple80211_link_status_ind) == 0x10,
               "link_status_ind must be 0x10 bytes");
_Static_assert(__builtin_offsetof(struct apple80211_link_status_ind, bssid)   == 0x00, "bssid");
_Static_assert(__builtin_offsetof(struct apple80211_link_status_ind, link_up) == 0x06, "link_up");
_Static_assert(__builtin_offsetof(struct apple80211_link_status_ind, reason)  == 0x08, "reason");
_Static_assert(__builtin_offsetof(struct apple80211_link_status_ind, bss_change_reason) == 0x0c,
               "bss_change_reason");

// What sendInternalLinkDownInd puts in `reason`: the link went down because the driver said so,
// rather than because the firmware reported an event.
#define APPLE80211_LINK_REASON_INTERNAL_DOWN 9

#endif /* JoinCompleteEvents_h */
