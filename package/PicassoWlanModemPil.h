// SPDX-License-Identifier: BSD-2-Clause-Patent
// Picasso pil_modem (MPSS) secure-load state machine -- OFFLINE SKELETON.
//
// This is the step-2 skeleton: the REAL ADSP boot ordering (PicassoAdspBoot.cpp)
// with every hardware boundary injected as a callback, so the production order,
// ownership and failure/recovery paths execute deterministically on the host.
// It deliberately does NOT issue a live SMC, touch MMIO, allocate SMEM, or start
// MPSS on hardware. TZ PAS authentication, the WLAN SMP2P bit semantics and
// MPSS power-up remain separate, reviewed, on-device actions (see step-2 design
// R1/R4/R5). No private firmware bytes are stored: parts are read via io.read_part
// from a caller-supplied read-only source and a digest is computed via the
// injected sha256 callback. Comparing that digest against the committed pin
// profile is NOT done here; it is the caller's / a future on-device adapter's
// responsibility. GLINK/QMI/WLFW (step 3) and ath10k (step 4) are absent.
#ifndef PICASSO_WLAN_MODEM_PIL_H
#define PICASSO_WLAN_MODEM_PIL_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Peer event model. The WLAN `wlan` SMP2P entry's ready/handover/stop_ack bit
// definitions were NOT found in the reviewed sources (design doc R4), so the
// bit meanings are supplied by the caller in the config, never hard-coded here.
struct WlanModemPeerEvent {
    bool hasValue, changed, restart;
    uint32_t value, flags;
};

// All hardware/system boundaries are injected. Host tests supply deterministic
// fakes; a future on-device adapter would supply real UFS/PAS/RPMH calls (that
// adapter is NOT part of this skeleton and needs its own review + authorization).
struct WlanModemPilIo {
    void *context;
    // Read split part `index` into dst (cap bytes); set *out to the byte count.
    // index 0 is modem.mdt metadata; index i (>=1) is the part backing PH i-1.
    // Return 0 on success, -2 when the part is intentionally absent (filesz==0).
    int  (*read_part)(void *ctx, unsigned index, uint8_t *dst, size_t cap, size_t *out);
    int  (*sha256)(void *ctx, const uint8_t *data, size_t bytes, uint8_t out[32]);
    int  (*rpmh_write)(void *ctx, unsigned rail, uint32_t addr, uint32_t value);
    // Shared PAS lease gate (GPU ZAP / ADSP / modem). Reused by all sub-systems;
    // -16 = contention, -115 = poisoned. Never open a second independent gate.
    int  (*lease_acquire)(void *ctx);
    void (*lease_release)(void *ctx, bool uncertain);
    int  (*aop)(void *ctx, bool on);
    // pas(cmd, addr, bytes, pasId): pasId is passed explicitly (modem = 4).
    int  (*pas)(void *ctx, unsigned cmd, uint64_t addr, uint64_t bytes, uint32_t pasId);
    int  (*poll_peer)(void *ctx, struct WlanModemPeerEvent *event);
    void (*sleep)(void *ctx, unsigned ms);
    uint64_t (*now)(void *ctx);          // monotonic milliseconds
};

struct WlanModemPilConfig {
    uint64_t carveoutBase;   // 0x8c000000
    uint64_t carveoutBytes;  // 0xf800000
    uint64_t metadataBase;   // 0x8bc18000
    uint64_t metadataBytes;  // 0x3000
    uint32_t pasId;          // 4 (DTB pil_modem qcom,pas-id), NOT the ADSP id 1
    uint32_t headers;        // 35
    uint32_t mdtBytes;       // 9420 (modem.mdt size, pinned); distinct from metadata slot
    uint32_t partCount;      // 34: PH0..PH33 backed by disk b00..b33; b18/b25 absent, PH34 is BSS-only
    uint32_t timeoutMs;      // 10000 (DTB qcom,proxy-timeout-ms = 0x2710)
    uint32_t readyBit;       // SMP2P bit supplied by caller (bit semantics unresolved)
    uint32_t handoverBit;
    uint32_t stopAckBit;
    uint32_t rails;          // 2 MSS proxy rails (vdd_cx, vdd_mss)
};

// Observable state. phase/error mirror the ADSP boot contract; the boolean
// fields let host tests assert lease/quarantine/retained-power invariants.
struct WlanModemPilState {
    const char *phase;
    unsigned step;
    int error;
    unsigned pasCalls, railVotes, partsLoaded, peerPolls;
    bool leased, aop, authenticated, ready, handed, quarantined, released;
};

// Execute one bounded boot->ready->stop transaction with failure injection.
// Returns 0 on ready (observer handed off) or a negative errno. Never blocks
// the caller beyond timeoutMs; never loops waiting for hardware on the host.
int picasso_wlan_modem_pil_execute(const struct WlanModemPilIo *io,
    const struct WlanModemPilConfig *cfg, struct WlanModemPilState *state);

// Convenience: the board-identified config used by Picasso (PAS 4 / modem region).
// Defined in the .cpp as a function so host tests can copy and mutate it.
void picasso_wlan_modem_pil_default_config(struct WlanModemPilConfig *cfg);

#ifdef __cplusplus
}
#endif
#endif
