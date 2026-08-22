//
//  AssocCandidates.h
//  itlwm
//
//  Tahoe association request. Reconstructed, not from any SDK.
//

#ifndef AssocCandidates_h
#define AssocCandidates_h

#include <sys/cdefs.h>
#include <stdint.h>
#include "apple80211_var.h"     // for the embedded apple80211_key

// What the WCL hands the driver to join a network. Apple calls the type
// `apple80211AssocCandidates`; this repo forward-declares it as `apple80211_assoc_candidates`
// in IO80211InfraProtocol.h, so that name is kept and this is its definition.
//
// Entry point chain, from the slot the driver overrides down to the code that does the work:
//
//     slot 602  AppleBCMWLANInfraProtocol::setWCL_ASSOCIATE(apple80211AssocCandidates *)
//                 -> loads [this+0x130], derefs, tail-jumps that object's slot 540
//     AppleBCMWLANCore::setWCL_ASSOCIATE                    <- the real implementation
//                 -> AppleBCMWLANJoinAdapter::performJoin(apple80211AssocCandidates *)
//
// Producer, and therefore the authority on layout:
//     WCLJoinRequest::fillAssocCandidatesList(apple80211AssocCandidates *,
//                                             apple80211_battery_save_modes, bool)
//     WCLJoinRequest::addAssocCandidates(WCLJoinCandidate *, apple80211AssocCandidates *)
// the first filling the request-wide fields, the second appending one entry per BSS.
//
// Offsets below are established from those two functions. Fields the producer writes but whose
// *meaning* is not yet established are named `_unkNN` and sized, not guessed at: a wrongly named
// field invites a wrong use. See AirportItlwm/AGENTS.md for the per-field evidence.
//
// **The decoding is incomplete, and that is tracked as mechanism 16 in the root AGENTS.md — read it
// before relying on this struct.** In particular `flags1e0`, `flags1e4` and `flags1e8` record only
// which bits Apple *sets*, not what any bit requires, and `_unk40` is 148 bytes nobody has
// explained. Ignoring a field here cannot corrupt anything, because Apple writes and we read; the
// failure mode is that we silently fail to honour a requirement one of those bits carries, and the
// association comes up wrong rather than failing. `AppleBCMWLANJoinAdapter::adjustMfp` reads these
// flags, so at least one plausibly governs management-frame protection.

// One candidate BSS. Every field is filled from a virtual on the IO80211BSSBeacon the WCL picked,
// which is what names them — addAssocCandidates calls slots 112, 87, 46, 50 and 62 in that order.
struct apple80211_assoc_candidate {
    uint8_t  sae_pk_capable;    // 0x00  isBSSSAEPKCapable()
    uint8_t  _pad01;            // 0x01
    uint16_t encryption_mode;   // 0x02  getEncryptionMode()
    uint8_t  bssid[6];          // 0x04  getAddress(uint8 *)
    uint8_t  owe_trans_bssid[6];// 0x0a  getOWETransAddress(uint8 *)
    uint16_t chanspec;          // 0x10  getChanPrimarySWSpec() -- AppleChannelSpec_t
};

_Static_assert(sizeof(struct apple80211_assoc_candidate) == 0x12,
               "assoc candidate entry must be 18 bytes: addAssocCandidates strides by 9*count*2");

// The array is fixed at 10 entries, which is not a guess: the array base is 0x218, the stride is
// 18, and the next field the producer writes is at 0x2cc == 0x218 + 10 * 18 exactly.
#define APPLE80211_MAX_ASSOC_CANDIDATES 10

struct apple80211_assoc_candidates {
    uint32_t _unk00;                    // 0x00
    uint32_t version;                   // 0x04  always 0x78
    uint32_t battery_save_mode;         // 0x08  the apple80211_battery_save_modes argument
    uint16_t _unk0c;                    // 0x0c
    uint8_t  _pad0e[2];                 // 0x0e
    uint32_t _unk10;                    // 0x10
    // APPLE80211_AUTHTYPE_* bitmask. Confirmed two ways: AppleBCMWLANJoinAdapter::adjustMfp
    // switches on exactly those bit values with textbook WPA3 policy (8/0x10/0x400 -> MFP
    // optional, 0x1000/0x2000 SAE -> required unless downgraded, 0x4000/0x8000 WPA3-Enterprise ->
    // always required), and the WCL logs `upperAuth = AUTHTYPE_WPA2_PSK` == 1 << 3 == 8 for a
    // WPA2 network. adjustMfp also reads bits 16 and 17, which are past the end of our enum.
    uint32_t upper_auth_type;           // 0x14
    uint32_t _unk18;                    // 0x18
    uint32_t ssid_len;                  // 0x1c  directly precedes ssid, as in apple80211_ssid
    uint8_t  ssid[32];                  // 0x20  four qwords out of the join request
    // 0x40  The association key, and the reason no separate key route is needed on Tahoe.
    //
    // `fill+0x1db` is `memmove(cand + 0x40, joinRequest[0x10] + 0x64, 0x94)`, and 0x94 == 148 ==
    // sizeof(apple80211_key) exactly. Confirmed independently by
    // `WCLJoinRequest::getKeyCipherType`, which returns `[joinRequest[0x10] + 0x6c]` — offset 8
    // into that blob, which is `apple80211_key::key_cipher_type`. The join request is itself built
    // from an `apple80211_assoc_data` (`WCLJoinRequest::initWCLJoinRequest(apple80211_assoc_data *,
    // CCLogStream *)`), whose `ad_key` is the same struct, so this is the pre-Tahoe key arriving by
    // a new road rather than a new mechanism.
    //
    // For a WPA2/WPA3-PSK network `key_cipher_type` is APPLE80211_CIPHER_PMK and `key` is the PMK
    // net80211 wants in ic_psk. **This was `_unkNN` for one boot cycle and cost an association**:
    // the driver assumed the key could only arrive through setCIPHER_KEY, which the WCL never
    // called, so every join was attempted with no PSK at all.
    struct apple80211_key key;          // 0x40
    uint16_t rsn_ie_len;                // 0xd4  WCLJoinRequest::trimRsnIeLen()
    uint8_t  rsn_ie[0x101];             // 0xd6  257 bytes
    uint8_t  _unk1d7[9];                // 0x1d7
    uint8_t  flags1e0;                  // 0x1e0 bits 1,2,3 set from join-request state
    uint8_t  _pad1e1[3];                // 0x1e1
    uint8_t  flags1e4;                  // 0x1e4 bits 1,2,3 encode the battery-save mode
    uint8_t  _pad1e5[3];                // 0x1e5
    uint8_t  flags1e8;                  // 0x1e8 bits 0..4 set from join-request state
    uint8_t  _pad1e9[3];                // 0x1e9
    uint32_t owe_trans_ssid_len;        // 0x1ec same len-then-ssid shape as 0x1c/0x20
    uint8_t  owe_trans_ssid[32];        // 0x1f0
    uint8_t  _unk210[3];                // 0x210
    // Bit 6 (0x40) downgrades MFP from required to merely capable: adjustMfp does
    // `cmp al,1 / adc ecx,1`, yielding 2 (required) when the bit is clear and 1 (capable) when set.
    // The other seven bits are unexamined.
    uint8_t  mfp_flags;                 // 0x213
    uint32_t candidate_count;           // 0x214 zeroed on entry, incremented per entry appended
    struct apple80211_assoc_candidate
             candidates[APPLE80211_MAX_ASSOC_CANDIDATES];   // 0x218
    uint16_t vendor_ie_len;             // 0x2cc  WCLJoinRequest::fillAssocCandidatesList
    uint8_t  vendor_ie[0x101];          // 0x2ce  257 bytes
    uint8_t  _pad3cf;                   // 0x3cf
    uint32_t _unk3d0;                   // 0x3d0
    uint32_t _unk3d4;                   // 0x3d4 always 0x14
    uint32_t _unk3d8;                   // 0x3d8 always 0x05  <- last field fillAssocCandidatesList writes
    uint8_t  _unk3dc[4];                // 0x3dc
    // WCLJoinManager::getVendorSpeificIes territory — a *second* producer, running after
    // fillAssocCandidatesList and writing only up here. It writes a qword at 0x3e0, a word at
    // 0x3e8, a byte at 0x3ea and the qword constant 0x401000a at 0x3eb (unaligned, so this is a
    // raw IE byte run rather than scalar fields), and reads flags1e0 at 0x1e0 to decide whether to.
    uint8_t  _unk3e0[0x108];            // 0x3e0
    // Product-info IE, written by IO80211_GetProductInfoIe(apple80211_product_ie *,
    // apple80211_product_info &) from a 0x12c-byte blob the WCL copies off its own state.
    uint32_t product_ie_kind;           // 0x4e8 always 0x40
    uint16_t product_ie_len;            // 0x4ec the length IO80211_GetProductInfoIe returns
    uint8_t  product_ie[0x20a];         // 0x4ee runs to the end of the allocation
};

// **0x6f8, not 0x3dc.** Sizing this by the last field the *first* producer writes was wrong by 796
// bytes: fillAssocCandidatesList stops at 0x3d8, but getVendorSpeificIes then writes as far as
// 0x4ee, and the allocation is larger still. Verified three independent ways in
// WCLJoinManager::handleSendCandidateToDriver: `IOMallocZeroData(0x6f8)` allocates it, the
// `cmdIouc(442, …)` call passes 0x6f8 as the buffer length, and `IOFreeData(p, 0x6f8)` frees it.
// The buffer is zero-allocated, so every field neither producer touches reads as zero — which is
// exactly why a short reconstruction looked self-consistent for so long.
_Static_assert(sizeof(struct apple80211_assoc_candidates) == 0x6f8,
               "assoc candidates must be 0x6f8 bytes: that is what handleSendCandidateToDriver "
               "allocates, passes to cmdIouc and frees");
_Static_assert(__builtin_offsetof(struct apple80211_assoc_candidates, ssid_len)     == 0x1c, "ssid_len");
_Static_assert(__builtin_offsetof(struct apple80211_assoc_candidates, ssid)         == 0x20, "ssid");
_Static_assert(__builtin_offsetof(struct apple80211_assoc_candidates, key)          == 0x40, "key");
// The producer's memmove length, which is what identifies this field at all.
_Static_assert(sizeof(struct apple80211_key) == 0x94, "apple80211_key must be 148 bytes");
_Static_assert(__builtin_offsetof(struct apple80211_assoc_candidates, rsn_ie_len)   == 0xd4, "rsn_ie_len");
_Static_assert(__builtin_offsetof(struct apple80211_assoc_candidates, rsn_ie)       == 0xd6, "rsn_ie");
_Static_assert(__builtin_offsetof(struct apple80211_assoc_candidates, flags1e0)     == 0x1e0, "flags1e0");
_Static_assert(__builtin_offsetof(struct apple80211_assoc_candidates, flags1e4)     == 0x1e4, "flags1e4");
_Static_assert(__builtin_offsetof(struct apple80211_assoc_candidates, flags1e8)     == 0x1e8, "flags1e8");
_Static_assert(__builtin_offsetof(struct apple80211_assoc_candidates, owe_trans_ssid) == 0x1f0, "owe ssid");
_Static_assert(__builtin_offsetof(struct apple80211_assoc_candidates, candidate_count) == 0x214, "count");
_Static_assert(__builtin_offsetof(struct apple80211_assoc_candidates, candidates)   == 0x218, "candidates");
_Static_assert(__builtin_offsetof(struct apple80211_assoc_candidates, vendor_ie_len) == 0x2cc, "vendor_ie_len");
_Static_assert(__builtin_offsetof(struct apple80211_assoc_candidates, vendor_ie)    == 0x2ce, "vendor_ie");
_Static_assert(__builtin_offsetof(struct apple80211_assoc_candidates, _unk3d4)      == 0x3d4, "unk3d4");

#endif /* AssocCandidates_h */
