// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only GLINK transport geometry discovery for the MPSS (host=0,remote=1)
// SMEM partition. Reuses WlanSmem.h read primitives and item framing; it does
// NOT reparse the TOC, does NOT assume item ids, and performs ZERO writes.
//
// Why this exists (BRINGUP_STATE.md §9.10 + QMI-WLFW report §3): 478/479/480 in
// this tree belong to the ADSP (0,2) edge and have NO first-hand basis on MPSS.
// The single P2 boot must answer, once, which item ids the MPSS GLINK edge uses
// and whether an APPS-owned desc+tx exists (sendReady). This is a DISCOVERY
// helper: it never opens a channel, never picks an IPCC kick, never runs WLFW.
//
// Classification is by geometry only:
//   desc : size == 32           (GLINK ring control block, 4 x u32)
//   tx   : size == 16384        (stock APPS-owned TX FIFO)
//   rx   : size in [16,65536] and 8-aligned (peer FIFO), excluding the above
// The desc control block's first 16 bytes are double-read: [TX tail, TX head,
// RX tail, RX head]. sendReady is a GEOMETRY-ONLY indication that APPS can
// address a TX ring; ownership, channel intent and kick remain P2 unknowns.
#ifndef PICASSO_GLINK_PROBE_H
#define PICASSO_GLINK_PROBE_H
#include "WlanSmem.h"

enum { PicassoGlinkProbeRoles = 3, PicassoGlinkProbeVisited = 256 };

struct picasso_glink_probe_result {
    uint32_t cand;                       // items matching any GLINK role
    uint32_t roles[PicassoGlinkProbeRoles]; // count per role: desc / tx / rx
    bool descPresent, txPresent, rxPresent;
    uint16_t descId, txId, rxId;         // discovered ids (0 = absent)
    uint32_t descSize, txSize, rxSize;
    uint32_t descOff, txOff, rxOff;      // absolute SMEM offsets of item data
    uint8_t ctrl[16];                    // stable desc control block snapshot
    bool stable;                         // desc 16B double-read identical
    bool sendReady;                      // desc+tx geometry usable for APPS TX
};

// Walk the uncached item table of an already-resolved partition. Enumerate
// every well-formed a5a5 item and classify by geometry. Purely read-only.
// Returns 0 (one stable desc found), -2 (no GLINK-geometry item -> the "reuse
// 478/479/480 on MPSS" hypothesis is disproved), -11 (desc control block
// changed between reads / concurrent peer update), -22 (ambiguous or
// malformed geometry). Geometry fields are populated on every return path so
// a diagnostic can report what was found even on -22/-11; ctrl[] is filled
// only when the double-read is stable (a stale snapshot would mislead).
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
            case 0: out->descPresent = true; out->descId = id; out->descSize = itemSize; out->descOff = itemOff; break;
            case 1: out->txPresent   = true; out->txId   = id; out->txSize   = itemSize; out->txOff   = itemOff; break;
            case 2: out->rxPresent   = true; out->rxId   = id; out->rxSize   = itemSize; out->rxOff   = itemOff; break;
            }
        }
        at += 16 + header + rawSize;
        if (at & 7) return -22;
    }

    if (!out->cand) return -2;                              // key falsification
    for (unsigned r = 0; r < PicassoGlinkProbeRoles; ++r)
        if (out->roles[r] > 1) return -22;                   // geometry cannot disambiguate

    if (!out->descPresent) return 0;                         // FIFO(s) but no control block
    uint8_t first[16], after[16];
    if (wlan_smread(io, out->descOff, first, 16) ||
        wlan_smread(io, out->descOff, after, 16)) return -22;
    out->stable = true;
    for (unsigned i = 0; i < 16; ++i) if (first[i] != after[i]) { out->stable = false; break; }
    if (!out->stable) return -11;
    for (unsigned i = 0; i < 16; ++i) out->ctrl[i] = first[i];

    // The desc control block holds [TX tail, TX head, RX tail, RX head].
    // Both TX pointers must be 8-aligned and inside the discovered TX FIFO.
    if (!out->txPresent) return 0;                           // decision point: no APPS TX
    uint32_t txTail = wlan_sm32(first), txHead = wlan_sm32(first + 4);
    if ((txTail & 7) || (txHead & 7) ||
        txTail >= out->txSize || txHead >= out->txSize) return -22;
    out->sendReady = true;                                   // geometry-only, no kick/intent claim
    return 0;
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
