#!/usr/bin/env python3
"""Fake-hardware Apple80211 OS-side probe (offline milestone).

Mirrors the software-only part of HakiOS's apple80211.c + sta.c with a fake
station (no DSP, no TrustZone, no device). Goals of this milestone:
  1. verify the 6 ABI structs are filled at the expected offsets (16.5 baseline);
  2. exercise ioctl dispatch (SIOCSA80211/SIOCGA80211) and the get/set paths;
  3. produce the exact log lines HakiOS uses to report what wifid asked for —
     this is the format that will calibrate the ABI at runtime on real HW.
No device, no flashing, no XNU SDK. Pure Python + ctypes.
"""
import ctypes as C
import json
import struct
import sys
from pathlib import Path

from apple80211_abi import (
    SIOCSA80211, SIOCGA80211, KEV_VENDOR_APPLE, KEV_IEEE80211_CLASS,
    KEV_SUBCLASS, EVENT, TYPE_NAME, verify_layout,
    Apple80211Req, Apple80211Channel, Apple80211Channels, Apple80211ChannelsInfo,
    Apple80211ScanResult, Apple80211ScanRequest, Apple80211Assoc, Apple80211LinkChanged,
)

# errno values from Apple XNU bsd/sys/errno.h (KERNEL build).
# NOTE: EOPNOTSUPP is 102 under KERNEL/__DARWIN_UNIX03; 45 is ENOTSUP.
# EAUTH is 80; 97 is ENOLINK. Verified against apple-oss-distributions/xnu errno.h.
EINVAL = 22; ENXIO = 6; ENOMEM = 12; EIO = 5; EBUSY = 16
EOPNOTSUPP = 102; EAUTH = 80; ETIMEDOUT = 60; ENETDOWN = 50

# capability bits (apple80211.c: get() case 12): AES,AES-CCM,PM,short slot,preamble,WPA2,WME,SGI20
CAP_BITS = [2, 3, 6, 9, 10, 14, 17, 19]

PHASE_INIT, PHASE_SCAN, PHASE_JOINING, PHASE_JOINED = 0, 1, 2, 3


class FakeStation:
    """Software-only station: static channel table, empty scan, never joins."""
    def __init__(self):
        self.phase = PHASE_INIT
        self.result_next = 0
        self.present_flag = True

    def present(self): return self.present_flag
    def powered(self): return True

    # Skeleton-LOCAL station channel-flag bits (hakios.sta.h was unavailable/404,
    # so these are NOT claimed to equal HakiOS's HAKIOS_STA_CHANNEL_* values;
    # only the mapping logic is faithful).
    CH_PASSIVE, CH_RADAR = 0x1, 0x2

    def channels(self, limit):
        table = [
            (2412, 0), (2437, 0), (2462, 0),
            (5180, 0), (5220, 0), (5745, 0),
        ]
        return table[:limit]

    def heard(self, index): return False   # empty scan
    def state_phase(self): return self.phase

    # ask_* mirror sta.c: they only accept/refuse the REQUEST (0/ENXIO/ENETDOWN).
    # Authentication outcome is reported asynchronously via the JOIN_DONE event,
    # never as the return value of ask_join (see hakios.sta.c:281-298).
    def ask_scan(self): self.phase = PHASE_SCAN; return 0
    def ask_join(self): self.phase = PHASE_JOINING; return 0
    def ask_leave(self): self.phase = PHASE_INIT; return 0


class Apple80211OS:
    def __init__(self, station, interface_name="en0"):
        self.sta = station
        self.interface_name = interface_name
        self.log = []
        self.kinds = {}       # (type, is_set) -> {times, answered, last_error}
        self.events = []      # posted kernel events
        self.join_result = 0  # set_join resets it; ASSOC_DONE maps the status

    def _emit(self, line):
        self.log.append(line)

    def post(self, code, data=None):
        ev = {
            "vendor_code": KEV_VENDOR_APPLE, "kev_class": KEV_IEEE80211_CLASS,
            "kev_subclass": KEV_SUBCLASS, "event_code": code,
            "event_name": next((k for k, v in EVENT.items() if v == code), "?"),
            "dv0": self.interface_name, "dv1_len": len(data) if data else 0,
        }
        self.events.append(ev)
        self._emit("kev_post: %s vendor=%d class=%d subclass=%d code=%d dv0=%s dv1=%dB" % (
            ev["event_name"], ev["vendor_code"], ev["kev_class"], ev["kev_subclass"],
            ev["event_code"], ev["dv0"], ev["dv1_len"]))

    def station_event(self, name, status=0):
        """Mirror hakios_apple80211_event()."""
        self._emit("station says: %s, %d" % (name, status))
        if name == "power":
            self.post(EVENT["POWER_CHANGED"])
        elif name == "scan_done":
            self.result_next = 0
            self.post(EVENT["SCAN_DONE"])
        elif name == "join_done":
            # apple80211.c:639 — the single static join_result is written here.
            self.join_result = 1 if status == 0 else 21 if status == EAUTH else 9 if status == ETIMEDOUT else 2
            self.post(EVENT["ASSOC_DONE"])
        elif name == "link":
            link = Apple80211LinkChanged()
            link.down = status != 1
            link.involuntary = status not in (0, 1)
            link.reason = 4 if status not in (0, 1) else 0
            # apple80211.c:650 sets link.rssi from sta state when status==1;
            # fake station is never joined so rssi stays 0 (explicit for parity).
            link.rssi = 0
            self.post(EVENT["SSID_CHANGED"])
            self.post(EVENT["BSSID_CHANGED"])
            self.post(EVENT["LINK_CHANGED"], bytes(link))

    def _channel_number(self, freq):
        # apple80211.c:209 channel_number()
        return 14 if freq == 2484 else (freq - 2407) // 5 if freq < 4000 else (freq - 5000) // 5

    def _fill_channel(self, ch, freq, flags):
        # apple80211.c:226-230 channel_fill(): OR in CHANNEL_20MHZ(0x2) and band.
        CHANNEL_20MHZ, CHANNEL_2GHZ, CHANNEL_5GHZ, CHANNEL_ACTIVE = 0x2, 0x8, 0x10, 0x80
        CHANNEL_DFS = 0x100
        ch.version = 1
        ch.channel = self._channel_number(freq)
        band = CHANNEL_2GHZ if freq < 4000 else CHANNEL_5GHZ
        ch.flags = (CHANNEL_20MHZ | band
                    | (0 if flags & FakeStation.CH_PASSIVE else CHANNEL_ACTIVE)
                    | (CHANNEL_DFS if flags & FakeStation.CH_RADAR else 0))

    def _cap_bits(self):
        bits = bytearray(13)
        for b in CAP_BITS:
            bits[b // 8] |= 1 << (b % 8)
        return bytes(bits)

    def get(self, req):
        """Mirror get(): returns (error, answer_bytes_or_None, note)."""
        t = req.req_type
        if t in (43, 44):
            return 0, b"hakios: ath11k for the WCN6750".ljust(256, b"\0"), "driver/hw name (256B)"
        if t == 12:
            return 0, self._cap_bits(), "capabilities 13B: " + self._cap_bits().hex()
        if t == 19:
            # apple80211.c:319-320 — state[4] = {bool, 0, 0, 0}, not four 1s.
            on = 1 if (self.sta.present() and self.sta.powered()) else 0
            ans = struct.pack("<II4I", 1, 1, on, 0, 0, 0)
            return 0, ans, "power: version=1 radios=1 state={%d,0,0,0}" % on
        if t in (27, 254):
            chans = self.sta.channels(96)
            a = Apple80211Channels()
            a.version = 1; a.count = len(chans)
            for i, (f, fl) in enumerate(chans):
                self._fill_channel(a.channels[i], f, fl)
            return 0, bytes(a), "supported channels count=%d (%dB)" % (len(chans), C.sizeof(a))
        if t == 207:
            chans = self.sta.channels(96)
            a = Apple80211ChannelsInfo()
            a.count = len(chans)
            for i, (f, fl) in enumerate(chans):
                ch = Apple80211Channel(); self._fill_channel(ch, f, fl)
                a.number[i] = ch.channel
                a.dfs[i] = 1 if fl & FakeStation.CH_RADAR else 0
                a.passive[i] = 1 if fl & FakeStation.CH_PASSIVE else 0
                a.flags[i] = ch.flags
            return 0, bytes(a), "channels_info count=%d (%dB)" % (len(chans), C.sizeof(a))
        if t == 15:
            req.req_val = 1  # APPLE80211_M_STA
            return 0, None, "op_mode=station(1)"
        if t == 21:
            req.req_val = self.join_result
            return 0, None, "assoc_result=%d" % self.join_result
        if t == 11:
            if not self.sta.heard(self.sta.result_next):
                self.sta.result_next += 1
                return EIO, None, "scan result: none (EIO)"
        if t == 13:
            ph = self.sta.state_phase()
            val = 4 if ph == PHASE_JOINED else 2 if ph == PHASE_JOINING else 1 if ph == PHASE_SCAN else 0
            req.req_val = val
            return 0, None, "state=%d" % val
        if t in (1, 9, 4, 16, 103, 2):
            return ENXIO, None, "not joined -> ENXIO"
        return EOPNOTSUPP, None, "not built"

    def set(self, req):
        t = req.req_type
        # HakiOS set_scan/set_join validate req_len first; mirror with the baseline structs.
        if t == 10:
            if req.req_len != C.sizeof(Apple80211ScanRequest):
                return EINVAL, None, "scan req wrong length"
            if not self.sta.present():
                return EBUSY, None, "radio still coming up (EBUSY)"
            self.sta.ask_scan(); return 0, None, "scan requested"
        if t == 20:
            if req.req_len < (0x88):  # offsetof(rest) in apple80211_assoc
                return EINVAL, None, "join req too short"
            self.join_result = 0  # apple80211.c:557 resets the single static
            self.sta.ask_join()
            # the join REQUEST is accepted (0); the outcome arrives via ASSOC_DONE
            return 0, None, "associate request accepted (result via event)"
        if t == 22:
            self.sta.ask_leave(); return 0, None, "disassociate"
        return EOPNOTSUPP, None, "not built"

    def ioctl(self, command, req):
        is_set = command == SIOCSA80211
        if command not in (SIOCSA80211, SIOCGA80211):
            return EOPNOTSUPP
        err, answer, note = self.set(req) if is_set else self.get(req)
        key = (req.req_type, is_set)
        k = self.kinds.setdefault(key, {"times": 0, "answered": 0, "last_error": 0})
        k["times"] += 1
        k["answered"] += (err == 0)
        if err: k["last_error"] = err
        name = TYPE_NAME.get(req.req_type, "?")
        self._emit("ioctl: %s %d (%s) value=%d req_len=%u -> %s [%s]" % (
            "sets" if is_set else "gets", req.req_type, name, req.req_val,
            req.req_len, "answered" if err == 0 else "refused(%d)" % err, note))
        return err

    def summary(self):
        lines = []
        for (t, is_set), k in sorted(self.kinds.items()):
            lines.append("in this run: %s %d (%s) %u times, answered %u times%s" % (
                "set" if is_set else "get", t, TYPE_NAME.get(t, "?"),
                k["times"], k["answered"],
                "" if k["last_error"] == 0 else "; last refusal %d" % k["last_error"]))
        return lines


def req(t, val=0, length=0, name="en0"):
    r = Apple80211Req()
    r.req_if_name = name.encode()
    r.req_type = t; r.req_val = val; r.req_len = length
    return r


def run():
    report = {"layout": verify_layout(), "ioctl_sequence": [], "events": [], "summary": [], "log": []}
    os_side = Apple80211OS(FakeStation())

    # A plausible wifid startup sequence against a fresh (unjoined) radio.
    seq = [
        (SIOCGA80211, req(43)), (SIOCGA80211, req(44)),
        (SIOCGA80211, req(12)), (SIOCGA80211, req(19)),
        (SIOCGA80211, req(15)),
        (SIOCGA80211, req(27)), (SIOCGA80211, req(254)), (SIOCGA80211, req(207)),
        (SIOCGA80211, req(13)), (SIOCGA80211, req(1)), (SIOCGA80211, req(9)),
        (SIOCGA80211, req(16)),
        (SIOCSA80211, req(10, length=C.sizeof(Apple80211ScanRequest))),  # scan
        os_side.station_event("scan_done") or None,
        (SIOCGA80211, req(11)),                  # scan result -> EIO (empty)
        (SIOCSA80211, req(20, length=C.sizeof(Apple80211Assoc))),        # associate request accepted
        os_side.station_event("join_done", EAUTH) or None,
        (SIOCGA80211, req(21)),                  # assoc result
    ]
    for item in seq:
        if item is None:
            continue
        cmd, r = item
        err = os_side.ioctl(cmd, r)
        report["ioctl_sequence"].append({"type": r.req_type, "set": cmd == SIOCSA80211, "error": err})

    report["events"] = os_side.events
    report["summary"] = os_side.summary()
    report["log"] = os_side.log

    out = Path(__file__).parent / "results.json"
    out.write_text(json.dumps(report, indent=2), encoding="utf-8")
    for line in os_side.log:
        print(line)
    print("\n--- summary ---")
    for line in report["summary"]:
        print(line)
    print("\nlayout all_pass:", report["layout"]["all_pass"])
    ok = report["layout"]["all_pass"]
    # sanity expectations for the fake run
    ok &= any(e["event_code"] == EVENT["SCAN_DONE"] for e in report["events"])
    ok &= any(e["event_code"] == EVENT["ASSOC_DONE"] for e in report["events"])
    print("FAKE-HW MILESTONE:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(run())
