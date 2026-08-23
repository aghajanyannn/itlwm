//
//  AirportItlwmSkywalkInterface.hpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef AirportItlwmSkywalkInterface_hpp
#define AirportItlwmSkywalkInterface_hpp

#include <Airport/Apple80211.h>

#if __IO80211_TARGET >= __MAC_26_0
// TEMPORARY instrumentation (mechanism 9). Declared here, at file scope and with C linkage to match
// the definition in the .cpp, because the only user is an inline override in the class below and a
// block-scope `extern` would get C++ linkage and fail to link.
extern "C" uint32_t gItlwmLastRxActivityCalls;
extern "C" uint32_t gItlwmDataPathPeerStatsCalls;
extern "C" uint32_t gItlwmLastQueueTimeCalls;
extern "C" uint32_t gItlwmBssInfoCalls;
extern "C" uint32_t gItlwmBssInfoEmpty;
extern "C" uint32_t gItlwmExtBssInfoCalls;
extern "C" uint32_t gItlwmLeaveNetCalls;
#endif

class AirportItlwmSkywalkInterface : public IO80211InfraProtocol {
    OSDeclareDefaultStructors(AirportItlwmSkywalkInterface)
    
public:
#if __IO80211_TARGET >= __MAC_26_0
    // IOSkywalkNetworkInterface::prepareBSDInterface dereferences
    // mExpansionData->fRegistrationInfo with no null check, to read the MTU. Apple allocates
    // that buffer in registerNetworkInterface, which this driver never calls, so it is NULL.
    // This override lends it one for the duration of the call and takes it back after — see
    // the definition for why lending beats owning.
    virtual bool prepareBSDInterface(ifnet_t, UInt) override;

    // Every path that can free the lent RegistrationInfo. Each takes the loan back first, so
    // teardown never hands kext-static memory to IOFreeType. See the .cpp for the caller scan
    // that says these three are the complete set.
    virtual void free() override;
    virtual void stop(IOService *) override;
    virtual IOReturn deregisterLogicalLink(void) override;

    // Slot 335 — the ONLY notification this driver gets that the family changed the link address,
    // and the fix for the `ic_myaddr` / `ac_enaddr` split (root AGENTS.md mechanism 21).
    // IO80211MacAddressAgent::updateMacAddress calls it whenever it settles on a new address, which
    // on this machine happens twice per boot: once at init and once when the per-network Private
    // Wi-Fi Address is applied. Apple's implementation updates the BSD side only, so without this
    // net80211 keeps associating, deriving the PTK from, and FILTERING RX on the factory address
    // while the BSD stack advertises the assigned one — a working link that cannot complete DHCP.
    virtual IOReturn setLinkLayerAddress(ether_addr *) override;

    // Overridden to hang the real Skywalk registration off a successful super::start().
    // A false return is FATAL to AirportItlwmV2::start and must stay that way: super::start()
    // is what allocates the per-interface state block that createEventPipe/destroyEventPipe
    // and the rest of the family dereference without a NULL check.
    virtual bool start(IOService *) override;
    // ---- Real Skywalk registration (root AGENTS.md mechanism 1) --------------------------------
    // Unconditional on Tahoe, and this IS the machine's Wi-Fi data path: the two pools and four
    // queues built here back the netif nexus that owns the BSD ifnet, and the TX/RX callbacks below
    // carry every frame. There is no boot-arg and no alternate path to fall back to — see the note
    // at ITLWM_REGINFO_MAC_OFFSET in the .cpp. A regression here costs networking outright, so
    // treat the counters in the root AGENTS.md checklist as the acceptance test for any change.
    bool buildSkywalkDataPath();
    bool registerSkywalkInterface();
    void teardownSkywalkDataPath();
    // Queues are constructed disabled and every entry point refuses while they are; see the header
    // comment on IOSkywalkPacketQueue. enableSkywalkQueues also does the first RX pull.
    void enableSkywalkQueues();
    void primeSkywalkRx(bool async);

    // Queue callbacks. Non-virtual statics: they are handed to Apple's factories as plain C
    // function pointers, so they add no vtable slot. `target` is whatever was passed to the
    // factory — this interface.
    static uint32_t txSubmissionDequeue(OSObject *, IOSkywalkTxSubmissionQueue *,
                                        IOSkywalkPacket **, uint32_t, void *);
    static uint32_t txCompletionEnqueue(OSObject *, IOSkywalkTxCompletionQueue *,
                                        IOSkywalkPacket **, uint32_t, void *);
    static uint32_t rxSubmissionDequeue(OSObject *, IOSkywalkRxSubmissionQueue *,
                                        IOSkywalkPacket **, uint32_t, void *);
    static uint32_t rxCompletionEnqueue(OSObject *, IOSkywalkRxCompletionQueue *,
                                        IOSkywalkPacket **, uint32_t, void *);

    // The real TX/RX bodies, split out of the statics so they can use members. `head` is the first
    // packet of a linked list, not an array — see the contract at each definition.
    // `headSlot` is the family's own head pointer (the callback's `packets` argument): a partial
    // consume must store the remaining head there, because listDequeue re-passes the same slot.
    uint32_t handleTxDequeue(IOSkywalkTxSubmissionQueue *queue, IOSkywalkPacket *head,
                             uint32_t count, IOSkywalkPacket **headSlot);
    uint32_t handleRxDequeue(IOSkywalkRxSubmissionQueue *queue, IOSkywalkPacket *head,
                             uint32_t count, IOSkywalkPacket **headSlot);

public:
    // RX entry point, called from AirportItlwmEthernetInterface::inputPacket on the HAL's receive
    // thread. Returns true if the frame was delivered through Skywalk and the caller must not also
    // hand it to the BSD path. Returns false for every reason — not registered, no free packet,
    // frame too large — so the legacy path stays the fallback rather than the frame being dropped.
    bool skywalkRxInput(mbuf_t m);

protected:

    IOSkywalkPacketBufferPool  *fTxPool;
    IOSkywalkPacketBufferPool  *fRxPool;
    IOSkywalkTxSubmissionQueue *fTxSubQ;
    IOSkywalkTxCompletionQueue *fTxComplQ;
    IOSkywalkRxSubmissionQueue *fRxSubQ;
    IOSkywalkRxCompletionQueue *fRxComplQ;
    bool                        fSkywalkRegistered;

    // Free RX packets the stack has lent us through the RX submission queue, waiting to be filled.
    // Touched from two threads — the submission queue's gate when they arrive, and the HAL's
    // receive thread when one is taken — so it needs its own lock. IOSimpleLock, not a mutex: both
    // critical sections are a few pointer writes and the receive side must not block.
    IOSkywalkPacket            *fRxFreeHead;
    IOSkywalkPacket            *fRxFreeTail;
    uint32_t                    fRxFreeCount;
    // A low-water refill is already in flight. Guarded by fRxFreeLock; set when the free list falls
    // below ITLWM_SKYWALK_RXLOWAT, cleared when buffers actually arrive in handleRxDequeue.
    bool                        fRxRefillPending;
    IOSimpleLock               *fRxFreeLock;

private:
    // All three are non-virtual on purpose: they must add no vtable slot.
    // gatedSuperPrepareBSDInterface calls super::prepareBSDInterface with the interface work
    // queue's gate closed, which IO80211Glue::sendIOUCToWcl requires.
    // TEMPORARY: all three go away when Skywalk registration becomes real — see mechanism 12 in
    // the root AGENTS.md and the comment at the definition.
    bool gatedSuperPrepareBSDInterface(ifnet_t, UInt);
    bool superPrepareBSDInterface(ifnet_t, UInt);
    static IOReturn gatedPrepareBSDAction(OSObject *, void *, void *, void *, void *);
    // Starts the net80211 scan under the work queue's gate. Non-virtual: no vtable slot.
    static IOReturn beginScanGated(OSObject *, void *, void *, void *, void *);
#if __IO80211_TARGET >= __MAC_26_0
    // Programs the WCL's chosen candidate into net80211, under the same gate and for the same
    // reason. arg0 is the apple80211_assoc_candidates. Non-virtual: no vtable slot.
    static IOReturn beginAssociateGated(OSObject *, void *, void *, void *, void *);
    // Tears the association down under the same gate. arg0 is the apple80211_leave_network.
    static IOReturn beginLeaveNetworkGated(OSObject *, void *, void *, void *, void *);
#endif
public:

    // Tahoe-only accessors: config/offload/telemetry properties this driver does
    // not implement. They are pure in IO80211InfraProtocol so they need a
    // definition; reporting unsupported is the honest answer.
    virtual IOReturn getBSS_BLACKLIST(bss_blacklist *) override { return kIOReturnUnsupported; }
    virtual IOReturn getHE_COUNTERS(apple80211_he_counters_ctl *) override { return kIOReturnUnsupported; }
    virtual IOReturn getWCL_WNM_OFFLOAD(apple80211_wcl_wnm_offload_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn getFW_CLOCK_INFO(apple80211_fw_clock_info *) override { return kIOReturnUnsupported; }
    virtual IOReturn getTIMESYNC_STATS(apple80211_timesync_stats *) override { return kIOReturnUnsupported; }
    virtual IOReturn getSYSTEM_SLEEP_CONFIG(apple80211_system_sleep_config *) override { return kIOReturnUnsupported; }
    virtual IOReturn getSMARTCCA_OPMODE(apple80211_smartcca_opmode *) override { return kIOReturnUnsupported; }
    virtual IOReturn getLQM_STATISTICS(apple80211_lqm_statistics *) override { return kIOReturnUnsupported; }
    virtual IOReturn getDEVICE_ORIENTATION(apple80211_device_orientation *) override { return kIOReturnUnsupported; }
    virtual IOReturn getACCESSORY_STATE(apple80211_device_accessory_info *) override { return kIOReturnUnsupported; }
    virtual IOReturn getP2P_DEVICE_CAPABILITY(apple80211_p2p_device_capability *) override { return kIOReturnUnsupported; }
    virtual IOReturn getPOWERTABLE_VERSION(apple80211_powertable_version_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setCLEAR_PMKSA_CACHE(void *) override { return kIOReturnUnsupported; }
    virtual IOReturn setDYNAMIC_RSSI_WINDOW_CONFIG(apple80211_dynamic_rssi_window_config *) override { return kIOReturnUnsupported; }
    virtual IOReturn setBSS_BLACKLIST(bss_blacklist *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_LEGACY_ROAM_PROFILE_CONFIG(apple80211_legacy_roam_profile_config *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_ARP_MODE(apple80211_wcl_arp_mode *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_QOS_PARAMS(apple80211_wcl_qos_params *) override { return kIOReturnUnsupported; }
    virtual IOReturn setVOICE_IND_STATE(apple80211_voice_ind_state *) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_ACCESSORY_POWER_LIMIT_WIFI_ENH(apple80211_mws_accessory_power_limit *) override { return kIOReturnUnsupported; }
    virtual IOReturn setPOWER_PROFILE(apple80211_power_profile *) override { return kIOReturnUnsupported; }
    virtual IOReturn setHEARTBEAT(void *) override { return kIOReturnUnsupported; }
    virtual IOReturn setINTERFACE_SETTING(apple80211_interface_setting *) override { return kIOReturnUnsupported; }
    virtual IOReturn setBYPASS_TX_POWER_CAP(apple80211_bypass_tx_power_cap *) override { return kIOReturnUnsupported; }
    virtual IOReturn setFACETIME_WIFICALLING_PARAMS(apple80211_facetime_wificalling_params *) override { return kIOReturnUnsupported; }
    virtual IOReturn setIPV4_PARAMS(apple80211_ipv4_params *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_WNM_OPS(apple80211_wcl_wnm_config_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_WNM_OFFLOAD(apple80211_wcl_wnm_offload_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_LIMITED_AGGREGATION(apple80211_limited_aggregation_config *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_BCN_MUTE_CONFIG(apple80211_bcn_mute_config *) override { return kIOReturnUnsupported; }
    virtual IOReturn setEAP_FILTER_CONFIG(apple80211_eap_filter_config *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWOW_LOW_POWER_MODE(apple80211_wow_low_power_mode *) override { return kIOReturnUnsupported; }
    virtual IOReturn setDUAL_POWER_MODE(apple80211_dual_power_mode_params *) override { return kIOReturnUnsupported; }
    // Ioctl 502, the last fatal gate in WCLNetManager::setCurrentBSS: a two-byte set whose result
    // is checked and whose failure takes the same abandon-the-network path as 454 and 460.
    // Accepted and ignored on the same grounds as setWCL_LINK_STATE_UPDATE.
    virtual IOReturn setWCL_UPDATE_FAST_LANE(apple80211_fastlane *) override { return kIOReturnSuccess; }
    virtual IOReturn setWCL_ASSOCIATED_SLEEP(apple80211_associated_sleep_config *) override { return kIOReturnUnsupported; }
    virtual IOReturn setCONGESTION_CTRL_IND(apple80211_congestion_control_indication *) override { return kIOReturnUnsupported; }
    virtual IOReturn setSTAND_ALONE_MODE_STATE(apple80211_standalone_state *) override { return kIOReturnUnsupported; }
    virtual IOReturn setIPV6_PARAMS(apple80211_ipv6_params *) override { return kIOReturnUnsupported; }
    virtual IOReturn setINFRA_ENUMERATED(apple80211_infra_enumerated *) override { return kIOReturnUnsupported; }
    virtual IOReturn setLMTPC_CONFIG(apple80211_lmtpc_config *) override { return kIOReturnUnsupported; }
    virtual IOReturn setTRAFFIC_ENG_PARAMS(apple80211_traffic_eng_params *) override { return kIOReturnUnsupported; }
    virtual IOReturn setLE_SCAN_PARAM(apple80211_le_scan_params *) override { return kIOReturnUnsupported; }
    virtual IOReturn setTIMESYNC_GPIO(apple80211_timesync_gpio *) override { return kIOReturnUnsupported; }
    virtual IOReturn setHOST_CLOCK_INFO(apple80211_host_clock_info *) override { return kIOReturnUnsupported; }
    virtual IOReturn setFW_CLOCK_SOURCE(apple80211_fw_clock_source *) override { return kIOReturnUnsupported; }
    virtual IOReturn setTIMESYNC_TX_POLICY(apple80211_timesync_tx_policy *) override { return kIOReturnUnsupported; }
    virtual IOReturn setTIMESYNC_RX_POLICY(apple80211_timesync_rx_policy *) override { return kIOReturnUnsupported; }
    virtual IOReturn setTIMESTAMPING_EN(apple80211_timestamping_en *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_SOI_CONFIG(appl80211_sleep_on_inactivity_config *) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_TIME_SHARING_WIFI_ENH(apple80211_mws_time_sharing *) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_WIFI_TYPE_7_BITMAP_WIFI_ENH(apple80211_mws_wifi_channel_bitmap *) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_COEX_BITMAP_WIFI_ENH(apple80211_mws_wifi_channel_bitmap *) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_DISABLE_OCL_BITMAP_WIFI_ENH(apple80211_mws_wifi_channel_bitmap *) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_RFEM_CONFIG_WIFI_ENH(apple80211_mws_rfem_config *) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_ASSOC_PROTECTION_BITMAP_WIFI_ENH(apple80211_mws_wifi_channel_bitmap *) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_SCAN_FREQ_WIFI_ENH(apple80211_mws_scan_freq *) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_SCAN_FREQ_MODE_WIFI_ENH(apple80211_mws_scan_freq_mode *) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_CONDITION_ID_BITMAP_WIFI_ENH(apple80211_mws_condition_id_config *) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_ANTENNA_SELECTION_WIFI_ENH(apple80211_mws_antenna_selection *) override { return kIOReturnUnsupported; }
    virtual IOReturn setNDD_REQ(apple80211_ndd_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setDBRG_ENTROPY(apple80211_drbg_entropy *) override { return kIOReturnUnsupported; }
    virtual IOReturn setSDB_ENABLE(apple80211_sdb_enable *) override { return kIOReturnUnsupported; }
    virtual IOReturn setBTCOEX_EXT_PROFILE(apple80211_btcoex_ext_profile *) override { return kIOReturnUnsupported; }
    virtual IOReturn setDEVICE_ORIENTATION(apple80211_device_orientation *) override { return kIOReturnUnsupported; }
    virtual IOReturn setACCESSORY_STATE(apple80211_device_accessory_info *) override { return kIOReturnUnsupported; }
    virtual IOReturn setOS_ELIGIBILITY(apple80211_os_eligibility *) override { return kIOReturnUnsupported; }
    virtual IOReturn setTX_MODE_CONFIG(apple80211_tx_mode_config *) override { return kIOReturnUnsupported; }
    virtual IOReturn setMITIGATE_INTERFERENCE(apple80211_mitigate_interference *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET >= __MAC_26_0
    virtual bool init(IOService *, ether_addr *) override;
#else
    virtual bool init(IOService *) override;
#endif
//    virtual ifnet_t getBSDInterface(void) override;
    
    // `key` may be NULL, which means "the PMK is already in ic_psk" — the Tahoe/WCL caller.
    void associateSSID(uint8_t *ssid, uint32_t ssid_len, const struct ether_addr &bssid, uint32_t authtype_lower, uint32_t authtype_upper, uint8_t *key, uint32_t key_len, int key_index);
    void setPTK(const u_int8_t *key, size_t key_len);
    void setGTK(const u_int8_t *key, size_t key_len, u_int8_t kid, u_int8_t *rsc);
    
public:
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getSSID(apple80211_ssid_data *) override;
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getAUTH_TYPE(apple80211_authtype_data *) override;
#endif
    virtual IOReturn getCHANNEL(apple80211_channel_data *) override;
    virtual IOReturn getPOWERSAVE(apple80211_powersave_data *) override;
    virtual IOReturn getTXPOWER(apple80211_txpower_data *) override;
    virtual IOReturn getRATE(apple80211_rate_data *) override;
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getBSSID(apple80211_bssid_data *) override;
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getSCAN_RESULT(apple80211_scan_result *) override;
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getSTATE(apple80211_state_data *) override;
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getPHY_MODE(apple80211_phymode_data *) override;
#endif
    virtual IOReturn getOP_MODE(apple80211_opmode_data *) override;
    virtual IOReturn getRSSI(apple80211_rssi_data *) override;
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getNOISE(apple80211_noise_data *) override;
#endif
    virtual IOReturn getSUPPORTED_CHANNELS(apple80211_sup_channel_data *) override;
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getLOCALE(apple80211_locale_data *) override;
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getDEAUTH(apple80211_deauth_data *) override;
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getRATE_SET(apple80211_rate_set_data *) override;
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getDTIM_INT(apple80211_dtim_int_data *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getSTATION_LIST(apple80211_sta_data *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getRSN_IE(apple80211_rsn_ie_data *) override;
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getAP_IE_LIST(apple80211_ap_ie_data *) override;
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getSTATS(apple80211_stats_data *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getASSOCIATION_STATUS(apple80211_assoc_status_data *) override;
#endif
    virtual IOReturn getGUARD_INTERVAL(apple80211_guard_interval_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn getMCS(apple80211_mcs_data *) override;
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getMCS_INDEX_SET(apple80211_mcs_index_set_data *) override;
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getWOW_PARAMETERS(apple80211_wow_parameter_data *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getWOW_ENABLED(apple80211_state_data *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getPID_LOCK(apple80211_state_data *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getSTA_IE_LIST(apple80211_sta_ie_data *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getSTA_STATS(apple80211_sta_stats_data *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getBT_COEX_FLAGS(apple80211_state_data *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getCURRENT_NETWORK(apple80211_scan_result *) override;
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getRSSI_BOUNDS(apple80211_rssi_bounds_data *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn getPOWER_DEBUG_INFO(apple80211_power_debug_info *) override { return kIOReturnUnsupported; }
    virtual IOReturn getHT_CAPABILITY(apple80211_ht_capability *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getLINK_CHANGED_EVENT_DATA(apple80211_link_changed_event_data *) override;
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getEXTENDED_STATS(apple80211_extended_stats *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getBEACON_PERIOD(apple80211_beacon_period_data *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getVHT_MCS_INDEX_SET(apple80211_vht_mcs_index_set_data *) override;
#endif
    virtual IOReturn getMCS_VHT(apple80211_mcs_vht_data *) override;
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getGAS_RESULTS(apple80211_gas_result_t *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn getCHANNELS_INFO(apple80211_channels_info *) override { return kIOReturnUnsupported; }
    virtual IOReturn getVHT_CAPABILITY(apple80211_vht_capability *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getBGSCAN_CACHE_RESULTS(apple80211_bgscan_cached_network_data_list *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn getROAM_PROFILE(apple80211_roam_profile_band_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn getCHIP_COUNTER_STATS(apple80211_chip_stats *) override { return kIOReturnUnsupported; }
    virtual IOReturn getDBG_GUARD_TIME_PARAMS(apple80211_dbg_guard_time_params *) override { return kIOReturnUnsupported; }
    virtual IOReturn getLEAKY_AP_STATS_MODE(apple80211_leaky_ap_setting *) override { return kIOReturnUnsupported; }
    virtual IOReturn getCOUNTRY_CHANNELS(apple80211_country_channel_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn getPRIVATE_MAC(apple80211_private_mac_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn getRANGING_ENABLE(apple80211_ranging_enable_request_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn getRANGING_START(apple80211_ranging_start_request_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn getAWDL_RSDB_CAPS(apple80211_rsdb_capability *) override { return kIOReturnUnsupported; }
    virtual IOReturn getTKO_PARAMS(apple80211_tko_params *) override { return kIOReturnUnsupported; }
    virtual IOReturn getTKO_DUMP(apple80211_tko_dump *) override { return kIOReturnUnsupported; }
    virtual IOReturn getHW_SUPPORTED_CHANNELS(apple80211_sup_channel_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn getBTCOEX_PROFILE(apple80211_btcoex_profile *) override { return kIOReturnUnsupported; }
    virtual IOReturn getBTCOEX_PROFILE_ACTIVE(apple80211_btcoex_profile_active_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn getTRAP_INFO(apple80211_trap_info_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn getTHERMAL_INDEX(apple80211_thermal_index_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn getMAX_NSS_FOR_AP(apple80211_btcoex_max_nss_for_ap_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn getBTCOEX_2G_CHAIN_DISABLE(apple80211_btcoex_2g_chain_disable *) override { return kIOReturnUnsupported; }
    virtual IOReturn getPOWER_BUDGET(apple80211_power_budget_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn getOFFLOAD_TCPKA_ENABLE(apple80211_offload_tcpka_enable_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn getRANGING_CAPS(apple80211_ranging_capabilities_t *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getSUPPRESS_SCANS(apple80211_suppress_scans_t *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getHOST_AP_MODE_HIDDEN(apple80211_host_ap_mode_hidden_t *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn getLQM_CONFIG(apple80211_lqm_config_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn getTRAP_CRASHTRACER_MINI_DUMP(apple80211_trap_mini_dump_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn getHE_CAPABILITY(apple80211_he_capability *) override { return kIOReturnUnsupported; }
    virtual IOReturn getBEACON_INFO(apple80211_beacon_info_t *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getSOFTAP_PARAMS(apple80211_softap_params *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn getCHIP_POWER_RANGE(apple80211_chip_power_limit *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getSOFTAP_STATS(apple80211_softap_stats *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn getNSS(apple80211_nss_data *) override;
    virtual IOReturn getHW_ADDR(apple80211_hw_mac_address *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getHE_MCS_INDEX_SET(apple80211_he_mcs_index_set_data *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn getCHIP_DIAGS(appl80211_chip_diags_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn getHP2P_CTRL(apple80211_hp2p_ctrl *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getREQUEST_BSS_BLACKLIST(void *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getASSOC_READY_STATUS(apple80211_assoc_ready *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn getTXRX_CHAIN_INFO(apple80211_txrx_chain_info *) override { return kIOReturnUnsupported; }
    virtual IOReturn getMIMO_STATUS(apple80211_mimo_status *) override { return kIOReturnUnsupported; }
    virtual IOReturn getCUR_PMK(apple80211_pmk *) override { return kIOReturnUnsupported; }
    virtual IOReturn getDYNSAR_DETAIL(apple80211_dynsar_detail *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getRANDOMISATION_STATUS(apple80211_mac_randomisation_status *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn getCOUNTRY_CHANNELS_INFO(apple80211_channels_info *) override { return kIOReturnUnsupported; }
    virtual IOReturn getLQM_SUMMARY(apple80211_lqm_summary *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getCOLOCATED_NETWORK_SCOPE_ID(apple80211_colocated_network_scope_id *) override;
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getBEACON_SCAN_CACHE_REQ(apple80211_scan_result *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn getSLOW_WIFI_FEATURE_ENABLED(apple80211_slow_wifi_feature_enabled *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getCCA(apple80211_interface_cca_data *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getRX_RATE(apple80211_rate_data *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn getTIMESYNC_INFO(apple80211_timesync_info *) override { return kIOReturnUnsupported; }
    virtual IOReturn getSENSING_DATA(apple80211_sensing_data_t *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getCOUNTRY_BAND_SUPPORT(apple80211_country_band_support *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn getWCL_FW_HOT_CHANNELS(apple80211_fw_hot_channels *) override { return kIOReturnUnsupported; }
    // NOT a stub, and must not become one again. Returning kIOReturnUnsupported here breaks
    // association outright: WCLJoinManager::handleJoinRequest treats the failure as fatal, logs
    // "unable to get low latency traffic stats", and fires JOIN_MANAGER_EVENT_JOIN_REQ_FAILED, so
    // setWCL_ASSOCIATE is never reached and every join dies with 0xe00002c7. Observed on 26.6.
    // Apple's own getter answers all-zero + success whenever it has no low-latency manager, which
    // is permanently our case; see the contract at the struct in IO80211InfraProtocol.h.
    virtual IOReturn getWCL_LOW_LATENCY_INFO(apple80211_low_latency_info *info) override
    {
        if (info == NULL)
            return kIOReturnError;      // 0xe00002bc, what Apple returns for a NULL out-pointer
        info->traffic_ongoing = 0;
        info->enabled = 0;
        info->desired_channel = 0;
        return kIOReturnSuccess;
    }
    // Ioctl 433, and a load-bearing one despite reading like a statistic. WCLNetManager::updateBss
    // calls it from firstLinkUp — i.e. the moment message 216 moves NET_MANAGER out of LINK_DOWN —
    // and on failure logs "Fail to get bss info" and calls leaveNetworkCommand, taking the FSM
    // WAITING_FOR_CONNECT_COMPLETE -> DEAUTH -> LINK_DOWN. The join then completes with the
    // interface still down, which is exactly what a `kIOReturnUnsupported` stub here produced.
    //
    // The reply is the same BeaconMetaData + IE list this driver already posts for scan results,
    // and it goes to the same consumer (WCLScanCacheStore::updateOrAddBeacon), so the whole answer
    // is a cached copy of the target BSS's last beacon. The WCL's buffer is IOMallocZeroData(0x844)
    // = sizeof(BeaconMetaData) + BEACON_META_MAX_IE_LEN.
    //
    // Tahoe only: the beacon cache it answers from is part of the `__MAC_26_0` join path, and no
    // earlier release has a WCL to ask.
#if __IO80211_TARGET >= __MAC_26_0
    virtual IOReturn getWCL_BSS_INFO(apple80211_beacon_msg *msg) override
    {
        if (msg == NULL)
            return kIOReturnError;
        if (!instance->copyCurrentBssBeacon(msg, sizeof(struct BeaconMetaData) + BEACON_META_MAX_IE_LEN)) {
            gItlwmBssInfoEmpty++;
            return kIOReturnUnsupported;
        }
        gItlwmBssInfoCalls++;
        return kIOReturnSuccess;
    }
#else
    virtual IOReturn getWCL_BSS_INFO(apple80211_beacon_msg *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn getWCL_TRAFFIC_COUNTERS(apple80211_wcl_traffic_counters *) override { return kIOReturnUnsupported; }
    virtual IOReturn getWCL_GET_TX_BLANKING_STATUS(uint *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getSSID_TRANSITION_SUPPORT(apple80211_ssid_transition_feature_enabled *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getWCL_VALID_CHANNEL_COUNT(unsigned long *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getWCL_P2P_STATUS_FOR_SCAN(p2pStatusForScan *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn getWCL_CHANNELS_INFO(apple80211ChannelInfo *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getP2P_STEERING_METRIC(apple80211_p2p_steering_metrics *) override { return kIOReturnUnsupported; }
#endif
    // PINNED, not a stub: this exists to force slot 452 to bind *our* function.
    //
    // The slot is inherited and was therefore left to the loader, which is one of the holes
    // mechanism 3 in the root AGENTS.md warns about — and it is the first one caught by evidence
    // rather than listed as a risk. `IO80211MacAddressAgent::setMacAddress` calls slot 452 on the
    // interface (branch selected at its `+0x101` by whether the IO80211InfraInterface cast is
    // non-NULL, which for us it is) and treats a non-zero return as fatal:
    // "fail to set mac addr", JOIN_MANAGER_EVENT_JOIN_REQ_FAILED, and no association is possible.
    //
    // The slot's correct occupant, IO80211SkywalkInterface::getLastRxUnicastLinkActivityTime, is
    // `xor eax,eax; ret` — it always answers 0, i.e. success. We observed a constant 0x18038590
    // instead, identical across two attempts and two boots, so the slot was not bound to it.
    // `IO80211PeerManager::getLastRxUnicastLinkActivityTime` is a same-named method on an unrelated
    // class and is the mis-binding candidate: it reads [this+0x18] then [+0x558]/[+0x560] and
    // tail-jumps through slot 452 of whatever that yields, so bound against our object it returns
    // an arbitrary but reproducible value.
    //
    // Returning 0 matches Apple's implementation exactly. Do not "improve" it into a real
    // timestamp: the family error-checks this value, so any non-zero answer breaks association.
    //
    // Guarded because the slot itself is Tahoe-only — the declaration in IO80211SkywalkInterface.h
    // sits inside the same `>= __MAC_26_0` block, so an unguarded override fails to compile on the
    // Sonoma targets with "marked 'override' but does not override any member functions".
#if __IO80211_TARGET >= __MAC_26_0
    virtual UInt64 getLastRxUnicastLinkActivityTime(ether_addr *) override
    {
        gItlwmLastRxActivityCalls++;    // TEMPORARY instrumentation, mechanism 9
        return 0;
    }

    // TEMPORARY instrumentation (mechanism 9), and an experiment rather than a fix. Slot 452 is
    // provably at the right index yet its counter stays 0 while the family error-checks that slot's
    // return, so the question is *which* slot the loaded table actually dispatches. These are its
    // immediate neighbours — real slots 450 and 451 — each counted separately. If one of them fires
    // during a join, the loaded table is off by a known amount and the offset names itself; if none
    // fires, the call is not reaching this object at all and the agent holds a different interface.
    //
    // Both return what Apple's own implementations return, so counting them changes no behaviour:
    // getDataPathPeerStats is `mov eax, 0xe00002c7; ret` and the queue-time getter answers 0.
    virtual IOReturn getDataPathPeerStats(apple80211_data_path_peer_stats *) override
    {
        gItlwmDataPathPeerStatsCalls++;
        return kIOReturnUnsupported;
    }

    virtual UInt64 getLastQueuePacketTime(ether_addr *) override
    {
        gItlwmLastQueueTimeCalls++;
        return 0;
    }
#endif

    virtual IOReturn getRSN_XE(apple80211_rsn_xe_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn getSIB_COEX_STATUS(apple80211_sib_coex_status *) override { return kIOReturnUnsupported; }
    // Ioctl 460, and the second half of the link-up gate. WCLNetManager::setCurrentBSS fetches it
    // immediately after getWCL_BSS_INFO and treats any failure the same way: "Fail to get
    // EXTENDED_BSS_INFO" -> leaveNetworkCommand -> DEAUTH. Implementing 433 alone only moves the
    // refusal one call later, which is exactly what the first boot with 433 showed.
    //
    // Composed from getters this driver already has, plus the RSN element out of the cached
    // beacon. The WCL zeroes the buffer before the call, so the fields left alone — HE MCS and the
    // MLO context — read as "not supported", which is the truth for this hardware and this port.
    // See include/Airport/ExtendedBssInfo.h for how the layout was pinned.
#if __IO80211_TARGET >= __MAC_26_0
    virtual IOReturn getWCL_EXTENDED_BSS_INFO(apple80211_extended_bss_info *) override;
#else
    virtual IOReturn getWCL_EXTENDED_BSS_INFO(apple80211_extended_bss_info *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn getWCL_LOW_LATENCY_INFO_STATS(apple80211_wcl_low_latency_stats *) override { return kIOReturnUnsupported; }
    virtual IOReturn getWCL_BGSCAN_CACHE_RESULT(apple80211_bgscan_cached_network_data_list *) override { return kIOReturnUnsupported; }
    virtual IOReturn getWIFI_NOISE_PER_ANT(apple80211_noise_per_ant_t *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn getBLOCKED_BANDS(apple80211_blocked_bands *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setSSID(apple80211_ssid_data *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setAUTH_TYPE(apple80211_authtype_data *) override;
#endif
    virtual IOReturn setCIPHER_KEY(apple80211_key *) override;
    virtual IOReturn setCHANNEL(apple80211_channel_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setPOWERSAVE(apple80211_powersave_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setTXPOWER(apple80211_txpower_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setRATE(apple80211_rate_data *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setSCAN_REQ(apple80211_scan_data *) override;
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setASSOCIATE(apple80211_assoc_data *) override;
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setDISASSOCIATE(apple80211_disassoc_data *) override;
#endif
    virtual IOReturn setIBSS_MODE(apple80211_network_data *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setHOST_AP_MODE(apple80211_network_data *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setAP_MODE(apple80211_apmode_data *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setDEAUTH(apple80211_deauth_data *) override;
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setTX_ANTENNA(void *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setANTENNA_DIVERSITY(void *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setRSN_IE(apple80211_rsn_ie_data *) override;
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setBACKGROUND_SCAN(apple80211_bgscan_data *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setWOW_PARAMETERS(apple80211_wow_parameter_data *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setWOW_ENABLED(apple80211_state_data *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setPID_LOCK(apple80211_state_data *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setSTA_AUTHORIZE(apple80211_sta_authorize_data *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setSTA_DISASSOCIATE(apple80211_sta_disassoc_data *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setSTA_DEAUTH(apple80211_sta_disassoc_data *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setRSN_CONF(apple80211_rsn_conf_data *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setIE(apple80211_ie_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWOW_TEST(apple80211_wow_test_data *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setSCANCACHE_CLEAR(void *) override;
#endif
    virtual IOReturn setVIRTUAL_IF_CREATE(apple80211_virt_if_create_data *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setBT_COEX_FLAGS(apple80211_state_data *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setROAM(apple80211_sta_roam_data *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setHT_CAPABILITY(apple80211_ht_capability *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setAWDL_FORCED_ROAM_CONFIG(apple80211_awdl_forced_roam_config *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setOFFLOAD_ARP(apple80211_offload_arp_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setOFFLOAD_NDP(apple80211_offload_ndp_data *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setOFFLOAD_SCAN(apple80211_offload_scan_data *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setGAS_REQ(apple80211_gas_query_t *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setGAS_START(apple80211_gas_query_t *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setGAS_SET_PEER(apple80211_gas_peer_t *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setVHT_CAPABILITY(apple80211_vht_capability *) override { return kIOReturnUnsupported; }
    virtual IOReturn setROAM_PROFILE(apple80211_roam_profile_band_data *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setAWDL_ENABLE_ROAMING(void *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setDBG_GUARD_TIME_PARAMS(apple80211_dbg_guard_time_params *) override { return kIOReturnUnsupported; }
    virtual IOReturn setLEAKY_AP_STATS_MODE(apple80211_leaky_ap_setting *) override { return kIOReturnUnsupported; }
    virtual IOReturn setPRIVATE_MAC(apple80211_private_mac_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setRESET_CHIP(apple80211_reset_command *) override { return kIOReturnUnsupported; }
    virtual IOReturn setCRASH(apple80211_crash_command *) override { return kIOReturnUnsupported; }
    virtual IOReturn setRANGING_ENABLE(apple80211_ranging_enable_request_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn setRANGING_START(apple80211_ranging_start_request_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn setRANGING_AUTHENTICATE(apple80211_ranging_authenticate_request_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn setTKO_PARAMS(apple80211_tko_params *) override { return kIOReturnUnsupported; }
    virtual IOReturn setBTCOEX_PROFILE(apple80211_btcoex_profile *) override { return kIOReturnUnsupported; }
    virtual IOReturn setBTCOEX_PROFILE_ACTIVE(apple80211_btcoex_profile_active_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setTHERMAL_INDEX(apple80211_thermal_index_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn setBTCOEX_2G_CHAIN_DISABLE(apple80211_btcoex_2g_chain_disable *) override { return kIOReturnUnsupported; }
    virtual IOReturn setPOWER_BUDGET(apple80211_power_budget_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn setOFFLOAD_TCPKA_ENABLE(apple80211_offload_tcpka_enable_t *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setSUPPRESS_SCANS(apple80211_suppress_scans_t *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setHOST_AP_MODE_HIDDEN(apple80211_host_ap_mode_hidden_t *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setLQM_CONFIG(apple80211_lqm_config_t *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setSOFTAP_PARAMS(apple80211_softap_params *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setSOFTAP_TRIGGER_CSA(apple80211_softap_csa_params *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setSOFTAP_WIFI_NETWORK_INFO_IE(apple80211_softap_wifi_network_info *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setBTCOEX_DISABLE_ULOFDMA(uint *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setSCAN_CONTROL(apple80211_scan_control_params *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setUSB_HOST_NOTIFICATION(apple80211_usb_host_notification_data *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setSET_MAC_ADDRESS(apple80211_set_mac_address *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setHP2P_CTRL(apple80211_hp2p_ctrl *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setABORT_SCAN(apple80211_abort_scan *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setSET_PROPERTY(apple80211_set_property_unserialized_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setROAM_CACHE_UPDATE(apple80211_roam_cache_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setPM_MODE(apple80211_pm_mode *) override { return kIOReturnUnsupported; }
    virtual IOReturn setSET_WIFI_ASSERTION_STATE(apple80211_wifi_assertion_data *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setREASSOCIATE_WITH_CORECAPTURE(apple80211_capture_debug_info_t *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setLINKDOWN_DEBOUNCE_STATUS(apple80211_linkdown_debounce_status *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setSOFTAP_EXTENDED_CAPABILITIES_IE(apple80211_softap_extended_capabilities_info *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setREALTIME_QOS_MSCS(apple80211_state_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setSENSING_ENABLE(apple80211_sensing_enable_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn setSENSING_DISABLE(apple80211_sensing_disable_t *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setNANPHS_ASSOCIATION(apple80211_nan_link_association_info *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setNANPHS_TERMINATED(apple80211_nan_link_association_info *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn set6G_MODE(apple80211_6G_mode *) override { return kIOReturnUnsupported; }
#endif
    // Ioctl 425 — Tahoe's replacement for setDISASSOCIATE, which it removed from the protocol.
    // Implemented in the .cpp; see there for why leaving it stubbed desynchronised net80211 from
    // the WCL rather than failing outright.
#if __IO80211_TARGET >= __MAC_26_0
    virtual IOReturn setWCL_LEAVE_NETWORK(apple80211_leave_network *) override;
#else
    virtual IOReturn setWCL_LEAVE_NETWORK(apple80211_leave_network *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setWCL_REASSOC(apple80211_reassoc *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_SET_ROAM_LOCK(apple80211_set_roam_lock *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_ROAM_PROFILE_CONFIG(apple80211_roam_profile_config *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setWCL_ROAM_PROFILE_CONFIGV1(apple80211_roam_profile_configV1 *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setWCL_ROAM_USER_CACHE(apple80211_user_roam_cache *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setWCL_SET_MULTI_AP_ENV(apple80211_set_multi_ap_env *) override { return kIOReturnUnsupported; }
#endif
    // Mechanism 13. Was an inline stub, which was harmless only while completion was a 100 ms
    // timer. Now that a completion can be seconds away, an abort must drop our outstanding one.
    // Declared in BOTH branches of IO80211InfraProtocol.h, so this guard compiles everywhere.
#if __IO80211_TARGET >= __MAC_26_0
    virtual IOReturn setWCL_SCAN_ABORT(void *) override;
#else
    virtual IOReturn setWCL_SCAN_ABORT(void *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setWCL_REAL_TIME_MODE(apple80211_wcl_real_time_mode *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setWCL_GARP_MODE(apple80211_wcl_garp_mode *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setWCL_JOIN_ABORT(void *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_TRIGGER_CC(triggerCC *) override { return kIOReturnUnsupported; }
    // Slot 601. Tahoe's ONLY scan entry point for an infra driver — implemented, not stubbed.
    // See the definition; returning kIOReturnUnsupported here reaches airportd verbatim as
    // 0xe00002c7 and is why no network ever appeared.
    //
    // The guard is load-bearing, not tidiness. The definition lives inside
    // `#if __IO80211_TARGET >= __MAC_26_0`, so declaring this unguarded left
    // AirportItlwmSkywalkInterface::setWCL_SCAN_REQ an **undefined symbol** in the
    // Sonoma14.0 and Sonoma14.4 kexts — a vtable slot pointing at nothing, i.e. a kext that
    // cannot load. It was an inline stub at 53c51c2 and lost its body when Tahoe implemented
    // it out of line. Caught by `nm -u <kext> | c++filt | grep AirportItlwm`, which must print
    // NOTHING for every target: an undefined symbol naming one of our OWN classes can never be
    // satisfied by any kernel collection, so it is always a bug, and the repo's usual
    // symbol check compares against Apple's collection and so cannot see it.
#if __IO80211_TARGET >= __MAC_26_0
    virtual IOReturn setWCL_SCAN_REQ(apple80211ScanRequest *) override;
#else
    virtual IOReturn setWCL_SCAN_REQ(apple80211ScanRequest *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET >= __MAC_26_0
    // Slot 602. Tahoe's ONLY association entry point for an infra driver — implemented, not
    // stubbed; see the definition. Tahoe-only for the same reason setCIPHER_KEY's PMK case is:
    // the WCL is Tahoe's stack, apple80211_assoc_candidates is reconstructed against 26.6 alone,
    // and the pre-Tahoe targets associate through setASSOCIATE and work today.
    virtual IOReturn setWCL_ASSOCIATE(apple80211_assoc_candidates *) override;
#else
    virtual IOReturn setWCL_ASSOCIATE(apple80211_assoc_candidates *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setWCL_PROTECT_IP(apple80211_wcl_protect_ip_mode *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setWCL_LINK_UP_DONE(void *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_SET_SCAN_HOME_AWAY_TIME(scanHomeAndAwayTime *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setWCL_VOLUNTARY_NETWORK_DISCONNECT(apple80211_wcl_voluntary_network_disconnect *) override { return kIOReturnUnsupported; }
#endif
    // Ioctl 454, and **fatal when refused** despite looking like a notification.
    // WCLNetManager::updateLinkState is nothing but this one `cmdIouc(454, set, …, 0x14)`, it
    // returns that result verbatim, and WCLNetManager::setCurrentBSS branches to its failure path
    // on it — so a refusal here abandons the network exactly as 433 and 460 do.
    //
    // Accepted and ignored, which is honest rather than a stub: this is the WCL *telling* the
    // driver the link state it has just derived, and the driver is where that state came from.
    // Contrast the getters on this path, where success would mean fabricating data.
    virtual IOReturn setWCL_LINK_STATE_UPDATE(apple80211_wcl_update_link_state *) override { return kIOReturnSuccess; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setSLOW_WIFI_RECOVERY(void *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setRSN_XE(apple80211_rsn_xe_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_ULOFDMA_STATE(apple80211_wcl_ulofdma_state *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_ACTION_FRAME(apple80211_wcl_action_frame *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setWCL_REAL_TIME_POLICY(apple80211_wcl_real_time_policy *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setGAS_ABORT(void *) override { return kIOReturnUnsupported; }
    virtual IOReturn setOS_FEATURE_FLAGS(apple80211_feature_flags *) override { return kIOReturnUnsupported; }
    virtual IOReturn setDHCP_RENEWAL_DATA(apple80211_dhcp_renewal_data *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setMOVING_NETWORK(apple80211_network_flags *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setBATTERY_POWERSAVE_CONFIG(apple80211_battery_ps_config *) override { return kIOReturnUnsupported; }
    virtual IOReturn setMIMO_CONFIG(apple80211_mimo_config *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_CONFIG_BG_MOTIONPROFILE(apple80211_bg_motion_profile *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_CONFIG_BG_NETWORK(apple80211_bg_network *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_CONFIG_BGSCAN(apple80211_bg_scan *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_CONFIG_BG_PARAMS(apple80211_bg_params *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET < __MAC_26_0
    virtual IOReturn setBLOCKED_BANDS(apple80211_blocked_bands *) override { return kIOReturnUnsupported; }
#endif
    
private:
    AirportItlwm *instance;
    ItlHalService *fHalService;
    
    //IO80211
    struct ieee80211_node *fNextNodeToSend;
    IOTimerEventSource *scanSource;
    bool fScanResultWrapping;
    
    u_int32_t current_authtype_lower;
    u_int32_t current_authtype_upper;
    bool disassocIsVoluntary;
#if __IO80211_TARGET >= __MAC_26_0
    // The PMK setCIPHER_KEY(APPLE80211_CIPHER_PMK) delivered, held here because ic_psk does not
    // survive associateSSID's opening ieee80211_disable_rsn. Refreshed per join, so it always
    // belongs to the network the WCL is currently joining.
    uint8_t fPmk[IEEE80211_PMK_LEN];
    bool    fPmkValid;
#endif
};


#endif /* AirportItlwmSkywalkInterface_hpp */
