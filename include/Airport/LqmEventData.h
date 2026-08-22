//
//  LqmEventData.h
//  itlwm
//
//  Payload of driver message 39 (APPLE80211_M_LQM_UPDATE). Reconstructed, not from any SDK.
//

#ifndef LqmEventData_h
#define LqmEventData_h

#include <sys/cdefs.h>
#include <stdint.h>

// The link-quality update. Like JoinCompleteEvents.h and unlike AssocCandidates.h, **we build this
// and the family reads it**, so a field left wrong is a field the WCL acts on.
//
// Why it is not optional. WCLNetManager::assocTimerAction ticks every 5 s and computes, against
// mach_continuous_time in milliseconds:
//
//     elapsed_a = now - state[0x150]
//     elapsed_b = now - state[0x158]
//     if (elapsed_a >= 60001 && elapsed_b >= 60001) handleMissedBeacons();
//
// handleMissedBeacons() reports a CoreCapture fault the first time and then calls
// leaveNetworkCommand(8, 9, 2, ...) — the `<missed beacons timeout> enhancedDisassocReason=<9>`
// teardown. Only three functions write those two timestamps:
//
//     state[0x150]  WCLNetManager::linkUp             once, when the link comes up
//                   WCLNetManager::wake               once, on system wake
//                   WCLNetManager::assocTimerAction   only if a flag byte is set (never, for us)
//                   WCLNetManager::handleLqmUpdate    <- message 39
//     state[0x158]  WCLNetManager::handleLqmUpdate    <- message 39, and nothing else at all
//
// So 0x158 is stale from boot for any driver that does not post 39, and 0x150 stops being
// refreshed one link-up later: the connection dies exactly 60 s after it is established. Posting
// this message on a timer is the whole fix.
//
// Layout recovered from the producer AppleBCMWLANLQM::updateLQM (which fills it) and the consumer
// WCLNetManager::handleLqmUpdate (which reads it). The struct type name is Apple's own, from the
// mangled symbols __ZN16AppleBCMWLANCore12postLQMEventEP25apple80211_lqm_event_data and
// __ZN15AppleBCMWLANLQM9updateLQMEP25apple80211_lqm_event_datab.
//
// **Every field here is validity-gated**, which is the BeaconMetaData bit-14 shape the root
// AGENTS.md warns about — except that here it works in our favour. The consumer tests a flag byte
// before reading each group, so a zeroed buffer with only the flags we can honestly back is safe:
// unset groups are skipped, not read as zero. Fill a value without its flag and it is discarded;
// set a flag without a value and the WCL believes a zero.
struct apple80211_lqm_event_data {
    // --- link-quality group. Filled by updateLQM's callers, not updateLQM itself. Read by the
    // consumer into its BSS context; none of it gates the timers, so this driver leaves the three
    // flags clear and the values zero. Sizes are from the consumer's own loads: `movsx ... word`
    // for 0x0c and 0x10, `movsx ... byte` for 0x13, so all three are signed.
    uint8_t  rssi_valid;            // 0x000  -> also stamps a mach_continuous_time at ctx+0x2c8
    uint8_t  _pad001[3];            // 0x001
    uint32_t rssi_info;             // 0x004  -> ctx+0x284
    uint8_t  _unk008[3];            // 0x008
    uint8_t  lq_b_valid;            // 0x00b
    int16_t  lq_b;                  // 0x00c  -> ctx+0x28a, and sets ctx+0x28c
    uint8_t  lq_a_valid;            // 0x00e
    uint8_t  _pad00f;               // 0x00f
    int16_t  lq_a;                  // 0x010  -> ctx+0x288, and sets ctx+0x28c
    uint8_t  cca_valid;             // 0x012
    int8_t   cca;                   // 0x013  IO80211BssManager::getCurrentCCA fills this in Apple's

    // --- counter group, gated by `counters_valid` (0x030) *and* `counters_fresh` (0x1d9). This is
    // the group that refreshes the two timers.
    //
    // Apple's producer writes per-interval **deltas** into liveness_beacon / liveness_traffic, not
    // running totals: updateLinkQualityMetrics keeps the previous absolute value per slice and
    // stores `current - previous`. Post deltas here too — a monotonically rising total would also
    // read non-zero, but it would keep reading non-zero after the AP went away, which is precisely
    // the condition this message exists to report.
    //
    // cnt_a/cnt_b/cnt_c are absolute per-slice values in Apple's producer, taken from three
    // consecutive firmware counters. Their meaning is not recovered. The consumer uses only the
    // relation `cnt_c > cnt_a` (as an alternative to liveness_traffic), so leaving all three zero
    // is well-defined: the comparison is false and liveness_traffic decides.
    uint32_t cnt_a;                 // 0x014  consumer: refreshes state[0x158] if cnt_c > cnt_a
    uint32_t cnt_b;                 // 0x018  not read by the consumer
    uint32_t cnt_c;                 // 0x01c
    uint32_t _unk020;               // 0x020  not read by the consumer
    uint32_t liveness_traffic;      // 0x024  != 0 refreshes state[0x158] (the inactivity timer)
    uint32_t liveness_beacon;       // 0x028  != 0 refreshes state[0x150] (the missed-beacon timer)
    uint32_t liveness_beacon2;      // 0x02c  companion delta; not read by the consumer
    uint8_t  counters_valid;        // 0x030  gates the state[0x150] refresh together with 0x028
    uint8_t  counters_ext_valid;    // 0x031  gates 0x034..0x03c only
    uint8_t  _unk032[2];            // 0x032
    uint32_t _unk034[3];            // 0x034  extended counters, gated by counters_ext_valid
    uint32_t _unk040[5];            // 0x040

    // --- channel group, gated by `chan_valid`. Not read by the consumer; left clear.
    uint8_t  chan_valid;            // 0x054
    uint8_t  _pad055;               // 0x055
    uint16_t chanspec;              // 0x056
    uint32_t _unk058;               // 0x058
    uint32_t _unk05c;               // 0x05c

    uint8_t  _unk060[0xec];         // 0x060

    uint8_t  _unk14c_valid;         // 0x14c  gates _unk14d
    uint8_t  _unk14d;               // 0x14d  -> ctx+0x650, and sets ctx+0x28c
    uint8_t  _unk14e[0x8a];         // 0x14e

    // --- the two master flags. handleLqmUpdate returns without touching either timestamp unless
    // both are non-zero, so these are the difference between a message that lands and one that is
    // parsed and thrown away.
    uint8_t  event_valid;           // 0x1d8  "this event carries data"
    uint8_t  counters_fresh;        // 0x1d9  "the counter group advanced since the last update".
                                    //        Apple clears this, and zeroes 0x14..0x30, when the
                                    //        firmware statistics have not changed.
    uint8_t  _unk1da[2];            // 0x1da
};

_Static_assert(sizeof(struct apple80211_lqm_event_data) == 0x1dc,
               "lqm_event_data must be 0x1dc bytes — handleLqmUpdate checks the length exactly");
_Static_assert(__builtin_offsetof(struct apple80211_lqm_event_data, rssi_info)        == 0x004, "rssi_info");
_Static_assert(__builtin_offsetof(struct apple80211_lqm_event_data, lq_b)             == 0x00c, "lq_b");
_Static_assert(__builtin_offsetof(struct apple80211_lqm_event_data, lq_a)             == 0x010, "lq_a");
_Static_assert(__builtin_offsetof(struct apple80211_lqm_event_data, cca)              == 0x013, "cca");
_Static_assert(__builtin_offsetof(struct apple80211_lqm_event_data, cnt_a)            == 0x014, "cnt_a");
_Static_assert(__builtin_offsetof(struct apple80211_lqm_event_data, cnt_c)            == 0x01c, "cnt_c");
_Static_assert(__builtin_offsetof(struct apple80211_lqm_event_data, liveness_traffic) == 0x024, "liveness_traffic");
_Static_assert(__builtin_offsetof(struct apple80211_lqm_event_data, liveness_beacon)  == 0x028, "liveness_beacon");
_Static_assert(__builtin_offsetof(struct apple80211_lqm_event_data, counters_valid)   == 0x030, "counters_valid");
_Static_assert(__builtin_offsetof(struct apple80211_lqm_event_data, chan_valid)       == 0x054, "chan_valid");
_Static_assert(__builtin_offsetof(struct apple80211_lqm_event_data, _unk14c_valid)    == 0x14c, "_unk14c_valid");
_Static_assert(__builtin_offsetof(struct apple80211_lqm_event_data, event_valid)      == 0x1d8, "event_valid");
_Static_assert(__builtin_offsetof(struct apple80211_lqm_event_data, counters_fresh)   == 0x1d9, "counters_fresh");

#endif /* LqmEventData_h */
