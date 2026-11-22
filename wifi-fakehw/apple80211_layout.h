/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Apple80211 ABI layout — executable specification extracted from HakiOS
 * oosexgang/hakios-xnu-kernel drivers/wifi/hakios.apple80211.c (iOS 16.5).
 *
 * This header depends on nothing but C11 standard headers, so ANY C compiler
 * can verify the _Static_asserts below without an XNU SDK or a device.
 *
 * IMPORTANT: these sizes/offsets are pinned to Darwin userland 16.5. Our
 * target is iOS 16.7.16 (20H392). This file is the BASELINE to be calibrated
 * against runtime logs, NOT a claim that 16.7 has identical layout.
 */
#ifndef WIFI_FAKEHW_APPLE80211_LAYOUT_H
#define WIFI_FAKEHW_APPLE80211_LAYOUT_H

#include <stdint.h>
#include <stddef.h>

#define SIOCSA80211 0x802869c8u /* _IOW('i',200,struct apple80211req) */
#define SIOCGA80211 0xc02869c9u /* _IOWR('i',201,struct apple80211req) */

struct apple80211req {
    char req_if_name[16];
    int32_t req_type;
    int32_t req_val;
    uint32_t req_len;
    uint64_t req_data __attribute__((aligned(8))); /* user_addr_t */
};

struct apple80211_channel {
    uint32_t version, channel, flags;
};

/* SUPPORTED_CHANNELS (27) / HW_SUPPORTED_CHANNELS (254): 4824 bytes */
struct apple80211_channels {
    uint32_t version, count;
    struct apple80211_channel channels[401];
    uint32_t unused;
};

/* CHANNELS_INFO (207): 10016 bytes */
struct apple80211_channels_info {
    uint8_t unknown_0[12];
    uint16_t count;
    uint8_t unknown_1[800];
    uint8_t number[400], indoor[400], dfs[400], passive[400], wide_40[400], wide_80[400];
    uint8_t unknown_2[1602];
    uint32_t flags[400];
    uint8_t unknown_3[3600];
} __attribute__((packed));

/* SCAN_REQ (10): 5456 bytes */
struct apple80211_scan_request {
    uint32_t version, bss_type;
    uint8_t bssid[6], unused_0[2];
    uint32_t ssid_length;
    uint8_t ssid[32];
    uint32_t scan_type;
    uint32_t phy_mode;
    uint16_t dwell_ms, unused_1;
    uint32_t rest_ms;
    uint32_t channel_count;
    struct apple80211_channel channels[400];
    uint64_t flags;
    uint32_t unused_2;
    uint32_t name_count;
    uint8_t names[10][56];
    uint32_t short_ssid, unused_3;
};

/* SCAN_RESULT (11) / CURRENT_NETWORK (103): 1164 bytes */
struct apple80211_scan_result {
    uint32_t version;
    struct apple80211_channel channel;
    uint16_t noise_known;
    int16_t noise, snr, rssi;
    uint16_t beacon_interval, capability;
    uint8_t bssid[6];
    uint8_t rate_count, unused_0;
    uint32_t rates[15];
    uint8_t ssid_length, ssid[32], unused_1[3];
    uint32_t age_ms;
    uint8_t flags, unused_2;
    uint16_t elements_length;
    uint8_t elements[1024];
};

/* ASSOCIATE (20): 592 bytes */
struct apple80211_assoc {
    uint32_t version;
    uint16_t mode, auth_lower, auth_upper, unused_0;
    uint32_t ssid_length;
    uint8_t ssid[32];
    uint8_t bssid[6], unused_1[2];
    uint32_t key_version, key_length, key_cipher;
    uint16_t key_flags, key_index;
    uint8_t key[64];
    uint8_t rest[0x250 - 0x88];
};

/* LINK_CHANGED event data: 32 bytes */
struct apple80211_link_changed {
    uint8_t down, unused_0[3];
    int32_t rssi;
    uint16_t snr, noise;
    uint8_t unused_1[4];
    uint8_t involuntary, unused_2[3];
    uint32_t reason, subreason;
    uint8_t unused_3[4];
};

/* req_data is user_addr_t (uint64) aligned(8): after req_len at 0x18 there are
 * 4 bytes of padding, so req_data sits at 0x20 and the struct is 40 bytes.
 * Derived from HakiOS apple80211.c:50-56 (which has NO original assert here);
 * added to cover the family's only aligned(8) field. Not a HakiOS-verbatim value. */
_Static_assert(sizeof(struct apple80211req) == 40 &&
               offsetof(struct apple80211req, req_data) == 0x20,
               "apple80211req as 64-bit user_addr_t with aligned(8)");

/* ---- the six assertions verbatim from HakiOS (iOS 16.5) ---- */
_Static_assert(sizeof(struct apple80211_assoc) == 592 &&
               offsetof(struct apple80211_assoc, key) == 0x48,
               "apple80211_assoc as Darwin userland 16.5 has it");
_Static_assert(sizeof(struct apple80211_link_changed) == 32 &&
               offsetof(struct apple80211_link_changed, reason) == 0x14,
               "link event as Darwin userland 16.5 reads it");
_Static_assert(sizeof(struct apple80211_channels) == 4824,
               "channel list as Darwin userland 16.5 has it");
_Static_assert(sizeof(struct apple80211_channels_info) == 10016 &&
               offsetof(struct apple80211_channels_info, number) == 0x32e &&
               offsetof(struct apple80211_channels_info, flags) == 0x12d0,
               "channels' description as Darwin userland 16.5 has it");
_Static_assert(sizeof(struct apple80211_scan_request) == 5456 &&
               offsetof(struct apple80211_scan_request, channels) == 0x48 &&
               offsetof(struct apple80211_scan_request, flags) == 0x1308 &&
               offsetof(struct apple80211_scan_request, names) == 0x1318,
               "scan request as Darwin userland 16.5 has it");
_Static_assert(sizeof(struct apple80211_scan_result) == 1164 &&
               offsetof(struct apple80211_scan_result, ssid_length) == 0x60 &&
               offsetof(struct apple80211_scan_result, age_ms) == 0x84 &&
               offsetof(struct apple80211_scan_result, elements) == 0x8c,
               "a scan's result as Darwin userland 16.5 has it");

#endif
