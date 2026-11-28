// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only GLINK transport geometry discovery for the MPSS (host=0,remote=1)
// SMEM partition. Reuses WlanSmem.h read primitives and item framing; it does
// NOT reparse the TOC, does NOT assume item ids, and performs ZERO writes.
//
// Why this exists (BRINGUP_STATE.md §9.10/§9.15): 478/479/480 in this tree
// belong to the ADSP (0,2) edge and have NO first-hand basis on MPSS. The P2
// read-only boots answer which item ids the MPSS GLINK edge uses and whether
// an APPS-owned desc+tx exists (sendReady). This is a DISCOVERY helper: it
// never opens a channel, never picks an IPCC kick, never runs WLFW.
//
// P1 boot result (0d947e6, §9.15): the MPSS (0,1) partition really has GLINK
// geometry (tx=480/16384, rx=611/4096), but TWO 32B items were found and the
// old probe's immediate `roles>1 -> -22` early return discarded every desc
// candidate except the last-written one. P1b keeps ALL 32B candidates and
// validates each by content so the next boot can disambiguate the real desc.
//
// Classification is by geometry only:
//   desc : size == 32           (GLINK ring control block, 4 x u32)
//   tx   : size == 16384        (stock APPS-owned TX FIFO)
//   rx   : size in [16,65536] and 8-aligned (peer FIFO), excluding the above
// For EVERY desc candidate its first 16 bytes are double-read: [TX tail, TX
// head, RX tail, RX head]; TX pointers must be 8-aligned and inside txSize.
// A candidate is "valid" only if stable AND its pointers fit. sendReady is a
// GEOMETRY-ONLY indication that one valid desc + a TX FIFO exist; ownership,
// channel intent and kick remain unknowns (never inferred here).
#ifndef PICASSO_GLINK_PROBE_H
#define PICASSO_GLINK_PROBE_H
#include "WlanSmem.h"

enum {
    PicassoGlinkProbeRoles = 3,
    PicassoGlinkProbeVisited = 256,
    PicassoGlinkProbeDescMax = 8    // bounded record of 32B candidates
};

struct picasso_glink_probe_desc {
    uint16_t id;
    uint32_t off, size;
    bool stable;                // 16B double-read identical
    bool ptrValid;              // TX/RX pointers 8-aligned and inside FIFOs
    bool valid;                 // stable && ptrValid -> likely real GLINK desc
    uint8_t ctrl[16];           // stable snapshot (zero if unstable)
};

struct picasso_glink_probe_result {
    uint32_t cand;                          // items matching any GLINK role
    uint32_t roles[PicassoGlinkProbeRoles]; // count per role: desc / tx / rx
    uint32_t descCount;                     // number of recorded 32B candidates
    struct picasso_glink_probe_desc desc[PicassoGlinkProbeDescMax];
    bool descPresent, txPresent, rxPresent;
    uint16_t txId, rxId;                    // discovered FIFO ids (0 = absent)
    uint32_t txSize, rxSize;
    uint32_t txOff, rxOff;                  // absolute SMEM offsets of item data
    unsigned validDesc;                     // count of candidates with valid=true
    bool sendReady;                         // one valid desc + tx FIFO (geometry only)
};

// Walk the uncached item table of an already-resolved partition. Enumerate
// every well-formed a5a5 item and classify by geometry. Purely read-only.
// Returns 0 (a stable, pointer-valid desc exists), -2 (no GLINK-geometry item
// -> the "reuse 478/479/480 on MPSS" hypothesis is disproved), -11 (every
// desc candidate changed between reads / concurrent peer update), -22 (no
// valid desc: malformed geometry, or >DescMax candidates). ALL discovered
// geometry (including every desc candidate) is populated on every return
// path so a diagnostic can report what was found even on -22/-11.
static inline int picasso_glink_probe(const struct wlan_smem_io *io,
        const struct wlan_smem_partition *p, struct picasso_glink_probe_result *out) {
    if (!io || !io->read || !p || !out) return -22;
    *out = picasso_glink_probe_result{};
    if (p->uncached < 32 || p->uncached > p->cached || p->cached > p->size ||
        !p->cacheline || (p->cacheline & (p->cacheline - 1)) ||
        p->offset > WLAN_SMEM_SIZE || p->size > WLAN_SMEM_SIZE - p->offset) return -22;

    uint32_t at = 32, visited = 0;
    while (at < p->uncached) {
        if (++visited > PicassoGlinkProbeVisited || p->uncached - at < 16) return -22;
        uint8_t e[16];
        if (wlan_smread(io, p->offset + at, e, 16)) return -22;
        if (wlan_sm16(e) != 0xa5a5) return -22;
        uint32_t rawSize = wlan_sm32(e + 4);
        uint16_t pad = wlan_sm16(e + 8), header = wlan_sm16(e + 10);
        if (!rawSize || pad >= rawSize || header > p->uncached - at - 16 ||
            rawSize > p->uncached - at - 16 - header) return -22;
        uint32_t itemSize = rawSize - pad;
        uint32_t itemOff = p->offset + at + 16 + header;
        uint16_t id = wlan_sm16(e + 2);

        unsigned role = PicassoGlinkProbeRoles; // unclassified
        if (itemSize == 32) role = 0;
        else if (itemSize == 16384) role = 1;
        else if (itemSize >= 16 && itemSize <= 65536 && !(itemSize & 7)) role = 2;
        if (role < PicassoGlinkProbeRoles) {
            ++out->roles[role]; ++out->cand;
            switch (role) {
            case 0:
                // Keep EVERY 32B candidate; never overwrite/lose one (P1 bug).
                out->descPresent = true;
                if (out->descCount < PicassoGlinkProbeDescMax) {
                    auto &d = out->desc[out->descCount++];
                    d.id = id; d.off = itemOff; d.size = itemSize;
                }
                break;
            case 1: out->txPresent = true; out->txId = id; out->txSize = itemSize; out->txOff = itemOff; break;
            case 2: out->rxPresent = true; out->rxId = id; out->rxSize = itemSize; out->rxOff = itemOff; break;
            }
        }
        at += 16 + header + rawSize;
        if (at & 7) return -22;
    }

    if (!out->cand) return -2;                              // key falsification
    if (out->roles[0] > PicassoGlinkProbeDescMax) return -22; // bounded, honest overflow

    // Validate EACH desc candidate independently by content. The GLINK ring
    // control block is [TX tail, TX head, RX tail, RX head]; TX pointers must
    // be 8-aligned and inside the discovered TX FIFO. This is what separates
    // the real desc from an unrelated 32B item (P1 could not do this).
    bool anyUnstable = false;
    for (uint32_t n = 0; n < out->descCount; ++n) {
        auto &d = out->desc[n];
        uint8_t first[16], after[16];
        if (wlan_smread(io, d.off, first, 16) ||
            wlan_smread(io, d.off, after, 16)) return -22;
        d.stable = true;
        for (unsigned i = 0; i < 16; ++i) if (first[i] != after[i]) { d.stable = false; break; }
        if (!d.stable) { anyUnstable = true; continue; }
        for (unsigned i = 0; i < 16; ++i) d.ctrl[i] = first[i];

        // Pointers: [TX tail, TX head] are APPS-owned -> require 8-alignment
        // AND inside the discovered TX FIFO. [RX tail, RX head] are peer-owned
        // and may be momentarily out of range while MPSS is still coming up,
        // so require 8-alignment ONLY (a bounds test here would cause a false
        // MISS on the second boot; full rx values are printed in ctrl[] for
        // offline inspection). Review §4.3 option (ii), tracked in §9.15.
        uint32_t txTail = wlan_sm32(first), txHead = wlan_sm32(first + 4);
        uint32_t rxTail = wlan_sm32(first + 8), rxHead = wlan_sm32(first + 12);
        bool txOk = out->txPresent && !(txTail & 7) && !(txHead & 7) &&
                    txTail < out->txSize && txHead < out->txSize;
        bool rxOk = !(rxTail & 7) && !(rxHead & 7);
        d.ptrValid = txOk && rxOk;
        d.valid = d.stable && d.ptrValid;
        if (d.valid) ++out->validDesc;
    }

    if (out->validDesc) { out->sendReady = true; return 0; }  // a real desc found
    if (anyUnstable) return -11;                              // peer updating, retryable later
    return -22;                                               // no candidate validated
}

// Resolve the (0,1) private partition strictly (NO global fallback, fail-closed)
// then run the geometry probe. Kept separate from the IOLog wrapper so a host
// harness can assert every branch with a synthetic wlan_smem_io.
static inline int picasso_mpss_glink_probe(const struct wlan_smem_io *io,
        struct picasso_glink_probe_result *out) {
    if (!io || !out) return -22;
    *out = picasso_glink_probe_result{};
    struct wlan_smem_partition p = {};
    int error = wlan_smem_partition(io, 0, 1, &p);
    if (error) return error; // -2 if (0,1) private partition absent -> hypothesis falsified
    return picasso_glink_probe(io, &p, out);
}
#endif
