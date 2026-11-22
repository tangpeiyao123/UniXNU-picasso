#!/usr/bin/env python3
"""Python mirror of the Apple80211 ABI layout in apple80211_layout.h.

Uses ctypes with explicit field order so struct sizes/offsets can be checked
on a host with no C compiler and no XNU SDK. The C header remains the source
of truth for CI; this module lets the same contract be exercised offline.

NOT a claim about iOS 16.7: these are the HakiOS 16.5 baseline values. The
whole point of the fake-hardware milestone is to calibrate them at runtime.
"""
import ctypes as C
import json
from pathlib import Path

SIOCSA80211 = 0x802869C8
SIOCGA80211 = 0xC02869C9

# kernel event encoding (apple80211.c: post())
KEV_VENDOR_APPLE = 1
KEV_IEEE80211_CLASS = 6
KEV_SUBCLASS = 1
EVENT = {
    "POWER_CHANGED": 1, "SSID_CHANGED": 2, "BSSID_CHANGED": 3,
    "LINK_CHANGED": 4, "ASSOC_DONE": 9, "SCAN_DONE": 10,
}

TYPE_NAME = {
    1: "SSID", 2: "AUTH_TYPE", 3: "CIPHER_KEY", 4: "CHANNEL", 5: "POWERSAVE",
    7: "TXPOWER", 8: "RATE", 9: "BSSID", 10: "SCAN_REQ", 11: "SCAN_RESULT",
    12: "CARD_CAPABILITIES", 13: "STATE", 15: "OP_MODE", 16: "RSSI",
    19: "POWER", 20: "ASSOCIATE", 21: "ASSOCIATE_RESULT", 22: "DISASSOCIATE",
    27: "SUPPORTED_CHANNELS", 30: "COUNTERMEASURES", 32: "RATE_SET",
    42: "STATION_LIST", 43: "DRIVER_VERSION", 44: "HARDWARE_VERSION",
    46: "RSN_IE", 47: "BACKGROUND_SCAN", 48: "AP_IE_LIST", 49: "STATS",
    103: "CURRENT_NETWORK", 207: "CHANNELS_INFO", 254: "HW_SUPPORTED_CHANNELS",
}


class Apple80211Channel(C.Structure):
    _fields_ = [("version", C.c_uint32), ("channel", C.c_uint32), ("flags", C.c_uint32)]


class Apple80211Req(C.Structure):
    _fields_ = [
        ("req_if_name", C.c_char * 16),
        ("req_type", C.c_int32),
        ("req_val", C.c_int32),
        ("req_len", C.c_uint32),
        ("req_data", C.c_uint64),
    ]


class Apple80211Assoc(C.Structure):
    _fields_ = [
        ("version", C.c_uint32),
        ("mode", C.c_uint16), ("auth_lower", C.c_uint16),
        ("auth_upper", C.c_uint16), ("unused_0", C.c_uint16),
        ("ssid_length", C.c_uint32),
        ("ssid", C.c_uint8 * 32),
        ("bssid", C.c_uint8 * 6), ("unused_1", C.c_uint8 * 2),
        ("key_version", C.c_uint32), ("key_length", C.c_uint32), ("key_cipher", C.c_uint32),
        ("key_flags", C.c_uint16), ("key_index", C.c_uint16),
        ("key", C.c_uint8 * 64),
        ("rest", C.c_uint8 * (0x250 - 0x88)),
    ]


class Apple80211LinkChanged(C.Structure):
    _fields_ = [
        ("down", C.c_uint8), ("unused_0", C.c_uint8 * 3),
        ("rssi", C.c_int32),
        ("snr", C.c_uint16), ("noise", C.c_uint16),
        ("unused_1", C.c_uint8 * 4),
        ("involuntary", C.c_uint8), ("unused_2", C.c_uint8 * 3),
        ("reason", C.c_uint32), ("subreason", C.c_uint32),
        ("unused_3", C.c_uint8 * 4),
    ]


class Apple80211Channels(C.Structure):
    _fields_ = [
        ("version", C.c_uint32), ("count", C.c_uint32),
        ("channels", Apple80211Channel * 401),
        ("unused", C.c_uint32),
    ]


class Apple80211ChannelsInfo(C.Structure):
    _pack_ = 1
    _fields_ = [
        ("unknown_0", C.c_uint8 * 12),
        ("count", C.c_uint16),
        ("unknown_1", C.c_uint8 * 800),
        ("number", C.c_uint8 * 400),
        ("indoor", C.c_uint8 * 400),
        ("dfs", C.c_uint8 * 400),
        ("passive", C.c_uint8 * 400),
        ("wide_40", C.c_uint8 * 400),
        ("wide_80", C.c_uint8 * 400),
        ("unknown_2", C.c_uint8 * 1602),
        ("flags", C.c_uint32 * 400),
        ("unknown_3", C.c_uint8 * 3600),
    ]


class Apple80211ScanRequest(C.Structure):
    _fields_ = [
        ("version", C.c_uint32), ("bss_type", C.c_uint32),
        ("bssid", C.c_uint8 * 6), ("unused_0", C.c_uint8 * 2),
        ("ssid_length", C.c_uint32),
        ("ssid", C.c_uint8 * 32),
        ("scan_type", C.c_uint32),
        ("phy_mode", C.c_uint32),
        ("dwell_ms", C.c_uint16), ("unused_1", C.c_uint16),
        ("rest_ms", C.c_uint32),
        ("channel_count", C.c_uint32),
        ("channels", Apple80211Channel * 400),
        ("flags", C.c_uint64),
        ("unused_2", C.c_uint32),
        ("name_count", C.c_uint32),
        ("names", (C.c_uint8 * 56) * 10),
        ("short_ssid", C.c_uint32), ("unused_3", C.c_uint32),
    ]


class Apple80211ScanResult(C.Structure):
    _fields_ = [
        ("version", C.c_uint32),
        ("channel", Apple80211Channel),
        ("noise_known", C.c_uint16),
        ("noise", C.c_int16), ("snr", C.c_int16), ("rssi", C.c_int16),
        ("beacon_interval", C.c_uint16), ("capability", C.c_uint16),
        ("bssid", C.c_uint8 * 6),
        ("rate_count", C.c_uint8), ("unused_0", C.c_uint8),
        ("rates", C.c_uint32 * 15),
        ("ssid_length", C.c_uint8), ("ssid", C.c_uint8 * 32), ("unused_1", C.c_uint8 * 3),
        ("age_ms", C.c_uint32),
        ("flags", C.c_uint8), ("unused_2", C.c_uint8),
        ("elements_length", C.c_uint16),
        ("elements", C.c_uint8 * 1024),
    ]


# The six expectations: (struct, expected_size, [(field, expected_offset), ...])
EXPECTATIONS = [
    ("apple80211_assoc", Apple80211Assoc, 592, [("key", 0x48)]),
    ("apple80211_link_changed", Apple80211LinkChanged, 32, [("reason", 0x14)]),
    ("apple80211_channels", Apple80211Channels, 4824, []),
    ("apple80211_channels_info", Apple80211ChannelsInfo, 10016, [("number", 0x32E), ("flags", 0x12D0)]),
    ("apple80211_scan_request", Apple80211ScanRequest, 5456,
     [("channels", 0x48), ("flags", 0x1308), ("names", 0x1318)]),
    ("apple80211_scan_result", Apple80211ScanResult, 1164,
     [("ssid_length", 0x60), ("age_ms", 0x84), ("elements", 0x8C)]),
]


def verify_layout(overrides=None):
    """Return a report; overrides maps name -> {'size':int,'offsets':{field:int}}
    so a future 16.7 calibration can be diffed against the 16.5 baseline."""
    overrides = overrides or {}
    report = {"baseline": "iOS 16.5 (HakiOS)", "structs": {}, "all_pass": True}
    for name, cls, exp_size, exp_offsets in EXPECTATIONS:
        actual_size = C.sizeof(cls)
        offsets = {f: getattr(cls, f).offset for f, _ in cls._fields_}
        ov = overrides.get(name, {})
        want_size = ov.get("size", exp_size)
        want_offsets = dict(exp_offsets)
        want_offsets.update(ov.get("offsets", {}))
        checks = {"size": actual_size == want_size}
        for field, want in want_offsets.items():
            checks["offset." + field] = offsets.get(field) == want
        ok = all(checks.values())
        report["structs"][name] = {
            "actual_size": actual_size, "expected_size": want_size,
            "offsets": {f: offsets[f] for f in want_offsets},
            "checks": checks, "pass": ok,
        }
        report["all_pass"] &= ok
    return report


if __name__ == "__main__":
    r = verify_layout()
    print(json.dumps(r, indent=2))
    raise SystemExit(0 if r["all_pass"] else 1)
