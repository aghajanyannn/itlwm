//
//  ExtendedBssInfo.h
//  itlwm
//
//  apple80211_extended_bss_info — the reply to ioctl 460. Reconstructed, not from any SDK.
//

#ifndef ExtendedBssInfo_h
#define ExtendedBssInfo_h

#include <sys/cdefs.h>
#include <stdint.h>

#include "apple80211_ioctl.h"

// The second thing WCLNetManager asks for when a link comes up, and — like getWCL_BSS_INFO before
// it — fatal when refused. WCLNetManager::setCurrentBSS allocates it, fetches it, and on any
// non-zero result logs "Fail to get EXTENDED_BSS_INFO" and calls leaveNetworkCommand, taking
// NET_MANAGER from WAITING_FOR_CONNECT_COMPLETE straight to DEAUTH. Answering it is not optional
// for a driver that wants to stay associated.
//
// Layout is pinned from both ends and needs no guesswork:
//
//   - the caller: `IOMallocZeroData(0x214)`, `cmdIouc(460, get, buf, 0x214, …)`.
//   - the producer, AppleBCMWLANNetAdapter::getExtendedBssInfo, which writes at exactly four
//     offsets — `updateRateSetSync(p)` at 0, `updateMCSSetSyc(p+0xbc, p+0xcc, p+0xd4)`,
//     `getAssociatedWPARSNIESync(p+0x113, 0x101)`, and `getMloContext(p+0xdc, …)`.
//
// Those offsets and the total agree with the natural sizes of the member structs, which is what
// makes the reconstruction safe rather than merely consistent: the _Static_asserts below are the
// check, so a release that resizes any member breaks the build instead of the association.
//
// **The caller zeroes the buffer, so an unfilled field is not a wrong field.** That is what makes a
// partial answer legitimate here — unlike a struct we post, where an unset field is a claim. A
// driver with no MLO leaves `mlo` zero and that *is* the truth about it.
struct apple80211_extended_bss_info {
    struct apple80211_rate_set_data          rate_set;   // 0x000  updateRateSetSync
    struct apple80211_mcs_index_set_data     mcs;        // 0x0bc  HT
    struct apple80211_vht_mcs_index_set_data vht_mcs;    // 0x0cc  packed: 6 bytes, not 8
    uint8_t                                  _pad_d2[2]; // 0x0d2  **explicit.** vht_mcs is packed
                                                         //        and every member after it is a
                                                         //        byte array, so the compiler
                                                         //        inserts nothing and the next
                                                         //        three offsets land 2 low — while
                                                         //        sizeof still rounds to 0x214 and
                                                         //        looks right. Size correct with
                                                         //        offsets wrong is the classic
                                                         //        shape; the offset asserts are
                                                         //        what caught it.
    uint8_t                                  he_mcs[8];  // 0x0d4  apple80211_he_mcs_index_set_data,
                                                         //        which this repo only forward-
                                                         //        declares; 8 bytes by subtraction
    uint8_t                                  mlo[0x37];  // 0x0dc  apple_mlo_context; zero == none
    uint8_t                                  wpa_rsn_ie[0x101];
                                                         // 0x113  the BSS's RSN/WPA IE, raw
                                                         //        element bytes (id, len, body)
};

_Static_assert(sizeof(struct apple80211_extended_bss_info) == 0x214,
               "extended_bss_info must be 0x214 bytes — the WCL's IOMallocZeroData size");
_Static_assert(__builtin_offsetof(struct apple80211_extended_bss_info, mcs)        == 0x0bc, "mcs");
_Static_assert(__builtin_offsetof(struct apple80211_extended_bss_info, vht_mcs)    == 0x0cc, "vht");
_Static_assert(__builtin_offsetof(struct apple80211_extended_bss_info, he_mcs)     == 0x0d4, "he");
_Static_assert(__builtin_offsetof(struct apple80211_extended_bss_info, mlo)        == 0x0dc, "mlo");
_Static_assert(__builtin_offsetof(struct apple80211_extended_bss_info, wpa_rsn_ie) == 0x113, "ie");

// Ioctl 425, the WCL telling the driver to leave the network it is on. Tahoe's replacement for
// setDISASSOCIATE, which it dropped from IO80211InfraProtocol — so a driver that implements
// neither stays associated in net80211 while the WCL has abandoned the network, which is exactly
// the divergence this port had: `ifconfig` reporting `status: active` after
// `leaveNetworkCommand`.
//
// Sized from the caller: `WCLNetManager::leaveNetwork` does `cmdIouc(425, set, buf, 0x1c)`.
// Offsets are from the only reader, `AppleBCMWLANCore::setWCL_LEAVE_NETWORK`, which forwards them
// to `AppleBCMWLANNetAdapter::leaveNetworkSync(uint32_t, LeaveMethod, uint32_t, uint16_t,
// ether_addr, const char *)`. Fields the driver does not act on are left `_unkNN` on purpose:
// Apple writes this and we read it, so an unread field is inert — unlike the structs we produce.
struct apple80211_leave_network {
    uint32_t reason;            // 0x00  -> leaveNetworkSync arg1
    uint32_t _unk04;            // 0x04  -> arg3
    uint8_t  _unk08[2];         // 0x08
    uint16_t _unk0a;            // 0x0a  -> arg4
    uint8_t  _unk0c[4];         // 0x0c
    uint8_t  method_sel;        // 0x10  `== 0` selects one LeaveMethod, non-zero the other
    uint8_t  _unk11[2];         // 0x11
    uint8_t  bssid[6];          // 0x13  ether_addr, deliberately unaligned
    uint8_t  _unk19[3];         // 0x19
} __attribute__((packed));

_Static_assert(sizeof(struct apple80211_leave_network) == 0x1c,
               "leave_network must be 0x1c bytes — the cmdIouc length");
_Static_assert(__builtin_offsetof(struct apple80211_leave_network, _unk0a)     == 0x0a, "unk0a");
_Static_assert(__builtin_offsetof(struct apple80211_leave_network, method_sel) == 0x10, "method");
_Static_assert(__builtin_offsetof(struct apple80211_leave_network, bssid)      == 0x13, "bssid");

#endif /* ExtendedBssInfo_h */
