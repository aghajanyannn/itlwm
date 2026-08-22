//
//  BeaconMetaData.h
//  itlwm
//
//  Tahoe scan-result delivery. Reconstructed, not from any SDK.
//

#ifndef BeaconMetaData_h
#define BeaconMetaData_h

#include <sys/cdefs.h>
#include <stdint.h>

// On Tahoe a driver does not answer a "give me scan results" getter — there is no getSCAN_RESULT
// in the driver's vtable at all, and the WCL never calls getWCL_BSS_INFO. Results are *pushed*,
// one message per BSS:
//
//     postMessage(interface, APPLE80211_M_BSS_BEACON, md, md->ie_len + sizeof(*md), true)
//
// where the payload is this header immediately followed by `ie_len` bytes of **IE list** — the
// tagged parameters only, not a whole 802.11 frame and not the fixed body before them.
//
// Producer reference: AppleBCMWLANScanAdapter::processScanResults+0x224 (which sends 0xc9) via
// AppleBCMWLANBSSBeacon::getBeaconMsgFromWLBSSInfo. The field meanings come from the consumer,
// IO80211BSSBeacon::setBeaconDataFromMsg, which is the honest source: it maps each offset onto a
// named setter. Per-field evidence is tabulated in AirportItlwm/AGENTS.md; keep the two in sync.
//
// There is deliberately no timestamp field: setBeaconDataFromMsg calls mach_continuous_time()
// itself, so ageing is the family's job.
#define APPLE80211_M_BSS_BEACON     201

// getBeaconMsgFromWLBSSInfo+0x2d8 stores `min(ie_len, 0x800)` and copies that many bytes, so
// Apple's own producer truncates here too. Match the number rather than picking one: below it a
// long IE list loses tail elements the family would otherwise have parsed.
#define BEACON_META_MAX_IE_LEN      0x800

struct BeaconMetaData {
    uint32_t ie_len;            // 0x00  bytes of IE list appended after this header
    uint16_t chanspec;          // 0x04  AppleChannelSpec_t
    uint8_t  ssid[32];          // 0x06  only read when both SSID flag bits are set
    uint8_t  ssid_len;          // 0x26
    uint8_t  primary_chan;      // 0x27  primary channel number -> beacon+0x29e
    uint8_t  _pad28;            // 0x28
    uint8_t  bssid[6];          // 0x29  -> setAddress()
    uint8_t  _pad2f;            // 0x2f
    int32_t  rssi;              // 0x30  dBm, signed. STORED ONLY IF FLAG_RSSI_VALID
    int16_t  noise;             // 0x34  dBm, signed; read only when flags bit 12 is set
    uint16_t snr;               // 0x36  read only when flags bit 13 is set
    uint16_t bintval;           // 0x38  beacon interval
    uint16_t capinfo;           // 0x3a  capability info
    uint8_t  opt_gated;         // 0x3c  read only when a flag gates it
    uint8_t  _pad3d[3];         // 0x3d
    uint32_t flags;             // 0x40
};

_Static_assert(sizeof(struct BeaconMetaData) == 0x44, "BeaconMetaData must be 0x44 bytes");
_Static_assert(__builtin_offsetof(struct BeaconMetaData, primary_chan) == 0x27, "chan offset");
_Static_assert(__builtin_offsetof(struct BeaconMetaData, bssid)    == 0x29, "bssid offset");
_Static_assert(__builtin_offsetof(struct BeaconMetaData, rssi)     == 0x30, "rssi offset");
_Static_assert(__builtin_offsetof(struct BeaconMetaData, noise)    == 0x34, "noise offset");
_Static_assert(__builtin_offsetof(struct BeaconMetaData, snr)      == 0x36, "snr offset");
_Static_assert(__builtin_offsetof(struct BeaconMetaData, bintval)  == 0x38, "bintval offset");
_Static_assert(__builtin_offsetof(struct BeaconMetaData, capinfo)  == 0x3a, "capinfo offset");
_Static_assert(__builtin_offsetof(struct BeaconMetaData, flags)    == 0x40, "flags offset");

// `flags` is a validity/provenance bitmap, not a set of booleans to be guessed at. Several fields
// above are *discarded* unless their bit says they are meaningful, so an unset bit silently drops
// data that is otherwise correctly placed — the RSSI arrived at 0x30 for weeks and was thrown away
// because bit 14 was clear.
//
// Bit meanings recovered from both sides: the consumer
// IO80211BSSBeacon::setBeaconDataFromMsg reads them, and the producer
// AppleBCMWLANBSSBeacon::getBeaconMsgFromWLBSSInfo+0x1ef..0x27b writes them straight out of
// Broadcom's wl_bss_info flag byte, which names them.
//
//   bit 0   skip the isNewBssBetter() comparison and take this update unconditionally.
//           Never set by Apple (every flags update ANDs with a mask ending 0xfa, clearing bits
//           0 and 2). Leave clear: it defeats the family's own best-BSS selection.
//   bit 1   the inline ssid[]/ssid_len at 0x06 is populated.
//   bit 2   with bit 1, calls setSSID() from setBeaconDataFromMsg. See the divergence note below.
//   bit 3   from wl_bss_info+0x33 bit 0     -> beacon+0x53c
//   bit 4   from wl_bss_info+0x4d bit 4     -> beacon+0x53d
//   bit 6   RSSI was measured on the BSS's own channel (WL_BSS_FLAGS_RSSI_ONCHANNEL). Passed as
//           the bool argument to isNewBssBetter(rssi, onchannel) -> beacon+0x2d3.
//   bit 7   this came from a beacon rather than a probe response (WL_BSS_FLAGS_FROM_BEACON)
//           -> beacon+0x2d4.
//   bit 8   RSSI is inaccurate (WL_BSS_FLAGS_RSSI_INACCURATE) -> beacon+0x2d6.
//   bit 9   from wl_bss_info+0x62 bit 3     -> beacon+0x2d7
//   bit 10  from wl_bss_info+0x62 bit 2     -> beacon+0x2d8. Also suppresses the no-IE setSSID.
//   bit 11  from wl_bss_info+0x62 bit 5     -> beacon+0x2d9
//   bit 12  `noise` at 0x34 is valid        -> beacon+0x288
//   bit 13  `snr` at 0x36 is valid          -> beacon+0x28a
//   bit 14  `rssi` at 0x30 is valid. REQUIRED: setBeaconDataFromMsg+0xe4 does `bt edx, 0xe` and
//           jumps clean past the store when clear, so the RSSI is dropped and every network
//           reports 0 dBm. The inverse is published as an explicit "RSSI invalid" byte at
//           beacon+0x2d5. Apple derives it from !(wl_bss_info flags & WL_BSS_FLAGS_RSSI_INVALID).
//   bit 16  -> beacon+0x2da
//   bit 18  `opt_gated` at 0x3c is valid    -> beacon+0x68c
#define BEACON_META_FLAG_SSID_PTR       0x00000002
#define BEACON_META_FLAG_SSID_STORE     0x00000004
#define BEACON_META_FLAG_SSID           (BEACON_META_FLAG_SSID_PTR | BEACON_META_FLAG_SSID_STORE)
#define BEACON_META_FLAG_RSSI_ONCHANNEL 0x00000040
#define BEACON_META_FLAG_FROM_BEACON    0x00000080
#define BEACON_META_FLAG_NOISE_VALID    0x00001000
#define BEACON_META_FLAG_SNR_VALID      0x00002000
#define BEACON_META_FLAG_RSSI_VALID     0x00004000

// DELIBERATE DIVERGENCE: this driver sets bit 2, Apple never does.
//
// setBeaconDataFromMsg+0xa6 computes `~flags & 6` and calls setSSID(md+6, ssid_len) only when bits
// 1 AND 2 are both set. Apple sets bit 1 alone, so it never takes that call. It has a second
// setSSID at +0x271, but that one is gated on `ie_len == 0`, i.e. it exists purely for the no-IE
// case — which says Apple expects a BSS carrying IEs to be named from its SSID IE instead.
//
// No such parse exists in the kernel: there is no IE-to-SSID setter symbol anywhere in
// IO80211Family, setSSID has zero direct callers, and it is reached only virtually. So for a
// pushed beacon with IEs the kernel-side name most plausibly comes from nothing at all unless
// bit 2 is set, and ours demonstrably do have names. Bit 2 stays until something proves the
// IE route works for us.
//
// Cost of keeping it: setSSID refuses to overwrite an SSID that is already stored (unless the
// stored one is the 1-byte " " placeholder) and logs a CoreCapture notice each time, so every
// repeat beacon for a known BSS produces one line. The notice prints as
// `setSSID@1390:Attempt to overwrite valid SSID with (11,)` with nothing after the comma — that
// empty string is Apple's bug, not a truncated SSID: the logNotice call at +0x54 passes the new
// length and then `xor eax, eax`, supplying no argument for the format's trailing %s.
//
// To test dropping it, send BEACON_META_FLAG_SSID_PTR alone and check networks still have names.

// AppleChannelSpec_t, from WCLDeviceConfiguration::fill20MHzChanSpec, which is literally
// `(band << 14) | 0x1000 | channel`:
//
//   bits 0-7    channel number
//   bits 11-13  width index into { 5, 10, 20, 40, 80, 160, 80, 160 } MHz -- 2 means 20 MHz
//   bits 14-15  band: 0 = 2.4 GHz, 1 = 6 GHz, 3 = 5 GHz
//
// Verified to round-trip through ChanSpecGetPrimaryChannel / ChanSpecConvToApple80211Channel:
// 2.4 GHz ch 6 -> 0x1006, 5 GHz ch 36 -> 0xd024, and ch 100 correctly picks up the DFS flag.
// Only 20 MHz is emitted; a wider spec needs the centre-channel table those functions use.
#define APPLE_CHANSPEC_WIDTH_20MHZ      (2u << 11)
#define APPLE_CHANSPEC_BAND_2GHZ        (0u << 14)
#define APPLE_CHANSPEC_BAND_6GHZ        (1u << 14)
#define APPLE_CHANSPEC_BAND_5GHZ        (3u << 14)

static inline uint16_t
AppleChanSpec20MHz(uint8_t channel)
{
    uint32_t band = (channel <= 14) ? APPLE_CHANSPEC_BAND_2GHZ : APPLE_CHANSPEC_BAND_5GHZ;
    return (uint16_t)(band | APPLE_CHANSPEC_WIDTH_20MHZ | channel);
}

#endif /* BeaconMetaData_h */
