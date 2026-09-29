// src/utils/PosixTz.h
//
// The collector's zone as the C library wants it: a POSIX TZ string built from
// the whole-hour offset and a daylight-saving rule (NetworkConfig::timezone,
// ::dstRule, ::dstOffsetHours). Set once with setenv("TZ")/tzset(), every
// localtime_r() in the firmware then moves between winter and summer time on
// its own, without NTP and without a restart.
//
// Header-only and free of Arduino so the host tests can check the strings
// against the build host's own C library (tests/host/test_posix_tz.cpp).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/// NetworkConfig::dstRule. 0 is EU so that a config saved before the field
/// existed (the byte was reserved, so zero) follows the EU rule after the
/// update.
enum : uint8_t {
    DST_RULE_EU     = 0,   ///< last Sunday of March .. last Sunday of October, 01:00 UTC
    DST_RULE_US     = 1,   ///< second Sunday of March .. first Sunday of November, 02:00 local
    DST_RULE_OFF    = 2,   ///< standard time all year
    DST_RULE_MANUAL = 3,   ///< standard time + dstOffsetHours all year
    DST_RULE_COUNT
};

inline uint8_t dstRuleClamp(uint8_t rule) {
    return rule < DST_RULE_COUNT ? rule : (uint8_t)DST_RULE_EU;
}

/// Write the TZ string for `tzHours` east of UTC under `rule` into `out`.
/// POSIX counts offsets WEST of UTC, so UTC+2 is "STD-2". The names are
/// placeholders: nothing in the firmware prints %Z.
inline void buildPosixTz(int tzHours, uint8_t rule, int manualDstHours,
                         char* out, size_t cap) {
    if (!out || cap == 0) return;
    rule = dstRuleClamp(rule);
    if (rule == DST_RULE_MANUAL) {
        if (manualDstHours < 1 || manualDstHours > 2) manualDstHours = 1;
        snprintf(out, cap, "STD%d", -(tzHours + manualDstHours));
    } else if (rule == DST_RULE_OFF) {
        snprintf(out, cap, "STD%d", -tzHours);
    } else if (rule == DST_RULE_US) {
        snprintf(out, cap, "STD%dDST,M3.2.0/2,M11.1.0/2", -tzHours);
    } else {
        // The EU changes at 01:00 UTC everywhere at once; the rule's times are
        // local, standard time for the start and summer time for the end.
        // newlib reads them unsigned, so a zone west of UTC-1 (never under
        // this rule in practice) is held at midnight rather than broken.
        int on  = 1 + tzHours; if (on  < 0) on  = 0;
        int off = 2 + tzHours; if (off < 0) off = 0;
        snprintf(out, cap, "STD%dDST,M3.5.0/%d,M10.5.0/%d", -tzHours, on, off);
    }
}

/// Seconds east of UTC in effect at `t` under the current TZ, summer time
/// included. newlib has no tm_gmtoff, so it is the local and UTC broken-down
/// times subtracted.
inline long tzOffsetAt(time_t t) {
    struct tm lt, gt;
    if (localtime_r(&t, &lt) == nullptr || gmtime_r(&t, &gt) == nullptr) return 0;
    long d = (long)(lt.tm_hour - gt.tm_hour) * 3600L
           + (long)(lt.tm_min  - gt.tm_min)  * 60L
           + (long)(lt.tm_sec  - gt.tm_sec);
    if (lt.tm_year != gt.tm_year)      d += (lt.tm_year > gt.tm_year) ? 86400L : -86400L;
    else if (lt.tm_yday != gt.tm_yday) d += (lt.tm_yday > gt.tm_yday) ? 86400L : -86400L;
    return d;
}
