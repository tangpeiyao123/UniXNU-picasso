// SPDX-License-Identifier: BSD-2-Clause-Patent
// Picasso WCSS/SNOC IRQ + DMA-ring ownership skeleton (see header honesty contract).
#include "PicassoWlanSnoc.h"

// First-hand raw GIC_SPI numbers, picasso.dts:7305 via WlanBoard.cpp:8-11.
// Pinned verbatim; source-text check in check-picasso-wlan-snoc.py enforces this.
static const uint16_t kSpi[PICASSO_WLAN_SNOC_IRQ_COUNT] = {
    0x084, 0x051, 0x19f, 0x1a0, 0x1a1, 0x056,
    0x05e, 0x060, 0x052, 0x077, 0x078, 0x057,
};
static const char *PH_INIT = "init", *PH_IRQ = "irq-ownership", *PH_RING = "ring-ownership",
                  *PH_WORK = "worker", *PH_DONE = "done";

void picasso_wlan_snoc_irq_spi(uint16_t out[PICASSO_WLAN_SNOC_IRQ_COUNT]) {
    if (!out) return;
    for (unsigned i = 0; i < PICASSO_WLAN_SNOC_IRQ_COUNT; ++i) out[i] = kSpi[i];
}

void picasso_wlan_snoc_default_config(PicassoWlanSnocConfig *cfg) {
    if (!cfg) return;
    for (unsigned i = 0; i < PICASSO_WLAN_SNOC_IRQ_COUNT; ++i) {
        cfg->ceMap.ceId[i] = PICASSO_WLAN_SNOC_UNRESOLVED;
        cfg->ceMap.ringId[i] = PICASSO_WLAN_SNOC_UNRESOLVED;
    }
    cfg->ceMap.provisional = true;
    cfg->iovaBase = 0xa0000000ULL; // WlanBoard.h:66
    cfg->iovaSize = 0x10000000ULL; // WlanBoard.h:67
    cfg->workerBatchMax = 8;
    cfg->workerPassMax = 4;
    cfg->timeoutMs = 50;
}

int picasso_wlan_snoc_ring_in_iova(const PicassoWlanSnocConfig *cfg,
                                   const PicassoWlanSnocRing *ring) {
    if (!cfg || !ring) return -22;
    if (!ring->count || !ring->elemBytes) return -22;
    const uint64_t bytes = (uint64_t)ring->count * ring->elemBytes;
    const uint64_t lo = cfg->iovaBase, hi = cfg->iovaBase + cfg->iovaSize;
    if (hi <= lo) return -22;
    if (ring->iovaBase < lo || ring->iovaBase >= hi) return -22;
    if (bytes > hi - ring->iovaBase) return -22;
    return 0;
}

static int fail(PicassoWlanSnocState *s, const char *phase, unsigned step, int rc,
                bool uncertain) {
    s->phase = phase; s->step = step; s->error = rc ? rc : -5;
    if (uncertain) s->quarantined = true;
    return s->error;
}

int picasso_wlan_snoc_execute(const PicassoWlanSnocIo *io,
                              const PicassoWlanSnocConfig *cfg,
                              PicassoWlanSnocState *state) {
    if (!io || !cfg || !state) return -22;
    *state = PicassoWlanSnocState{};
    state->phase = PH_INIT;
    state->scan_proven = false;      // invariant: never claimed
    state->hardwareTouched = false;  // invariant: offline skeleton

    if (!io->irq_status || !io->irq_ack || !io->irq_mask || !io->claim_owner ||
        !io->ring_map || !io->ring_unmap || !io->worker_poll)
        return fail(state, PH_INIT, 0, -2, false);
    if (!cfg->workerBatchMax || !cfg->workerPassMax)
        return fail(state, PH_INIT, 0, -22, false);

    // Phase A: IRQ ownership. Read the 12-bit pending mask; claim ownership of
    // every SPI and ack+mask each pending one exactly once. We do NOT bind any
    // SPI to a CE id here (that is ceMap, injected and provisional).
    state->phase = PH_IRQ;
    uint32_t pending = 0;
    const int src = io->irq_status(io->context, &pending);
    if (src) return fail(state, PH_IRQ, 0, src, false);
    if (pending & ~((1u << PICASSO_WLAN_SNOC_IRQ_COUNT) - 1u))
        return fail(state, PH_IRQ, 0, -22, false); // bits outside the 12 we own
    for (unsigned i = 0; i < PICASSO_WLAN_SNOC_IRQ_COUNT; ++i) {
        const bool set = ((pending >> i) & 1u) != 0;
        // Ownership of every SPI is claimed even when not pending, so that
        // "ownership complete" is independent of transient traffic.
        const int crc = io->claim_owner(io->context, i, 1 /* APPS */);
        if (crc == -16) { ++state->irqsUnclaimed; continue; }
        if (crc == -115) return fail(state, PH_IRQ, i, -115, true);
        if (crc) return fail(state, PH_IRQ, i, crc, false);
        if (set) {
            const int arc = io->irq_ack(io->context, i);
            if (arc) return fail(state, PH_IRQ, i, arc, true); // uncertain ack
            ++state->irqsAcked;
            const int mrc = io->irq_mask(io->context, i);
            if (mrc) return fail(state, PH_IRQ, i, mrc, false);
        }
        ++state->irqsObserved;
    }
    if (state->irqsUnclaimed)
        return fail(state, PH_IRQ, 0, -16, true); // partial ownership -> quarantine
    state->ownershipComplete = true;

    // Phase B: ring ownership + cache maintenance. Model ONE representative
    // injected ring (real count/layout is unknown here). Order: map -> io
    // invalidate (firmware may have written) -> advance head -> cache flush
    // (APPS wrote) -> keep ownership. Every step is bounded and injected.
    state->phase = PH_RING;
    PicassoWlanSnocRing ring = {};
    ring.count = 64; ring.elemBytes = 64;
    ring.iovaBase = cfg->iovaBase + 0x1000u; // inside the injected IOVA pool
    ring.head = 0; ring.tail = 0; ring.owner = 1 /* APPS */;
    if (picasso_wlan_snoc_ring_in_iova(cfg, &ring))
        return fail(state, PH_RING, 0, -22, false);
    const int mrc = io->ring_map(io->context, &ring);
    if (mrc) return fail(state, PH_RING, 0, mrc, false);
    // Re-validate the ring AS RETURNED by map: a buggy/hostile mapper must not
    // be able to hand back an out-of-range or zero-sized ring unnoticed.
    if (picasso_wlan_snoc_ring_in_iova(cfg, &ring))
        return fail(state, PH_RING, 0, -22, false);
    ++state->ringsMapped;
    int irc = io->cache_invalidate(io->context, &ring);
    if (irc) return fail(state, PH_RING, 0, irc, false);
    // Bounded cursor advance: head must stay < count.
    if (ring.head >= ring.count) return fail(state, PH_RING, 0, -22, false);
    ring.head = (ring.head + 1u) % ring.count;
    irc = io->cache_flush(io->context, &ring);
    if (irc) return fail(state, PH_RING, 0, irc, false);
    // Ownership must be APPS-held at this point; a firmware-owned ring here is
    // a conflict, not a success.
    if (ring.owner != 1) return fail(state, PH_RING, 0, -16, true);
    const int urc = io->ring_unmap(io->context, &ring);
    if (urc) return fail(state, PH_RING, 0, urc, false);
    ++state->ringsUnmapped; // unmap only after all maintenance + ownership held

    // Phase C: worker progress. Bounded passes, each with a bounded completion
    // batch. Stall (no work) and completion/CE-map mismatch are quarantined, not
    // guessed. This models progress ONLY -- there is no real ring cursor
    // relationship here (the ath10k descriptor format is unknown in this tree),
    // so we assert bounded, monotonic progress, not a fabricated head/tail rule.
    state->phase = PH_WORK;
    for (unsigned pass = 0; pass < cfg->workerPassMax; ++pass) {
        uint16_t comp[64];
        unsigned count = 0;
        const int wrc = io->worker_poll(io->context, cfg->workerBatchMax, comp, &count);
        if (wrc == -2) return fail(state, PH_WORK, pass, -60, true); // stall
        if (wrc) return fail(state, PH_WORK, pass, wrc, false);
        if (count > cfg->workerBatchMax || count > 64)
            return fail(state, PH_WORK, pass, -22, false); // batch overflow
        for (unsigned c = 0; c < count; ++c) {
            const uint16_t ce = comp[c];
            // Only once the CE map is resolved on-device may a completion id be
            // checked against known CEs. While provisional we neither bind nor
            // assert CE identity -- we simply count bounded progress.
            if (!cfg->ceMap.provisional && ce == PICASSO_WLAN_SNOC_UNRESOLVED)
                return fail(state, PH_WORK, pass, -22, false);
            ++state->completions;
        }
        ++state->workerPasses;
    }

    // Done. scan_proven stays false. scan_preconditions_ok reports ONLY that
    // the skeleton invariants held AND the CE map has been resolved on-device;
    // while ceMap.provisional is true it is forced false (B2 contract).
    state->phase = PH_DONE;
    state->step = 3;
    const bool invariants = state->ownershipComplete && state->irqsAcked > 0 &&
                            state->ringsMapped == 1 && state->ringsUnmapped == 1 &&
                            state->workerPasses == cfg->workerPassMax &&
                            state->completions > 0;
    state->scan_preconditions_ok = invariants && !cfg->ceMap.provisional;
    return 0;
}
