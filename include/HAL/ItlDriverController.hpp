/*
* Copyright (C) 2020  钟先耀
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/

#ifndef ItlDriverController_h
#define ItlDriverController_h

class ItlDriverController {
    
public:
    
    virtual void clearScanningFlags() = 0;

    virtual IOReturn setMulticastList(IOEthernetAddress *addr, int count) = 0;

    // APPENDED AT THE END DELIBERATELY. Inserting above setMulticastList would move that
    // method's index in this base's vtable, and its callers live in different translation units
    // from the three implementations, so a stale object file would dispatch one slot wrong.
    // New slots go last. Pure virtual, so a HAL that forgets to implement it fails to COMPILE
    // rather than surprising us at runtime.
    //
    // True while the firmware owns a scan, i.e. between the scan command and the completion
    // notification that reaches ieee80211_end_scan and raises IEEE80211_EVT_SCAN_DONE.
    // net80211 has no equivalent and its flags cannot substitute:
    // IEEE80211_F_BGSCAN is set by ieee80211_begin_cache_bgscan on ANY zero return from
    // ic_bgscan_start, and iwx_bgscan returns 0 both when it started a sweep and when it
    // declined because IWX_FLAG_SCANNING was already set; it is also left set by
    // ieee80211_end_scan's empty-cache path, which returns before the block that clears it.
    // IEEE80211_F_ASCAN has no HAL reader anywhere in this repo.
    //
    // Used for INSTRUMENTATION ONLY (ItlwmScanReqNoStart / ItlwmScanReqInFlight) — no control
    // flow depends on it, so a HAL answering conservatively cannot break scanning.
    virtual bool isScanning() = 0;
};

#endif /* ItlDriverController_h */
