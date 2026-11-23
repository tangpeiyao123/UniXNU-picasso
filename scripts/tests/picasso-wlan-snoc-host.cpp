// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host harness for the step-4 SNOC/IRQ/ring ownership skeleton.
// Deterministic injected boundaries; NO hardware, NO real DMA. CHECK (not
// <assert.h>) so zig's default -DNDEBUG cannot silently disable assertions.
#include "PicassoWlanSnoc.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (line %d)\n", #c, __LINE__); std::exit(1); } } while (0)

static const char *g_mode = "normal";

struct Fake {
    unsigned statusErr, ackErr, maskErr, claimErrIndex, claimErr;
    unsigned mapErr, unmapErr, invalidateErr, flushErr, workerErr;
    bool pendingAll, pendingNone, unclaimed, poison, staleSpi;
    bool ringMapMismatchOwner, ringHeadOverrun, cacheFlushMissing, cacheInvalidateMissing;
    bool iovaOutside, countZero, workerStallAll, workerOverflow;
    bool ceUnresolvedAfterMap, doubleAck;
    unsigned ackCount[PICASSO_WLAN_SNOC_IRQ_COUNT];
    unsigned acks, claims, maps, unmaps, flushes, invalidates, passes, completions;
    unsigned lastPassCount;
};

static int irqStatus(void *p, uint32_t *mask) {
    Fake *f = (Fake *)p;
    if (f->statusErr) return (int)f->statusErr;
    if (f->pendingNone) { *mask = 0; return 0; }
    if (f->staleSpi) { *mask = (1u << PICASSO_WLAN_SNOC_IRQ_COUNT); return 0; } // bit 12, out of range
    *mask = f->pendingAll ? ((1u << PICASSO_WLAN_SNOC_IRQ_COUNT) - 1u) : 0x5u; // default 3 pending
    return 0;
}
static int irqAck(void *p, unsigned i) {
    Fake *f = (Fake *)p;
    if (i >= PICASSO_WLAN_SNOC_IRQ_COUNT) return -22;
    ++f->ackCount[i]; ++f->acks;
    if (f->ackErr) return (int)f->ackErr;
    if (f->doubleAck && f->ackCount[i] > 1) return -22;
    return 0;
}
static int irqMask(void *p, unsigned) {
    Fake *f = (Fake *)p;
    return f->maskErr ? (int)f->maskErr : 0;
}
static int claimOwner(void *p, unsigned i, uint8_t) {
    Fake *f = (Fake *)p; ++f->claims;
    if (f->claimErrIndex == i) return (int)f->claimErr;
    if (f->poison) return -115;
    return 0;
}
static int ringMap(void *p, PicassoWlanSnocRing *ring) {
    Fake *f = (Fake *)p; ++f->maps;
    if (f->mapErr) return (int)f->mapErr;
    if (f->ringMapMismatchOwner) ring->owner = 2; // firmware owns -> conflict
    if (f->ringHeadOverrun) ring->head = ring->count; // head == count -> reject
    if (f->iovaOutside) ring->iovaBase = 0x8ba00000ULL; // MSA, not IOVA pool
    if (f->countZero) ring->count = 0;
    return 0;
}
static int ringUnmap(void *p, PicassoWlanSnocRing *) {
    Fake *f = (Fake *)p; ++f->unmaps;
    return f->unmapErr ? (int)f->unmapErr : 0;
}
static int cacheInvalidate(void *p, const PicassoWlanSnocRing *) {
    Fake *f = (Fake *)p; ++f->invalidates;
    return f->cacheInvalidateMissing ? -9 : 0;
}
static int cacheFlush(void *p, const PicassoWlanSnocRing *) {
    Fake *f = (Fake *)p; ++f->flushes;
    return f->cacheFlushMissing ? -9 : 0;
}
static int workerPoll(void *p, unsigned batchMax, uint16_t *out, unsigned *count) {
    Fake *f = (Fake *)p; ++f->passes;
    if (f->workerErr) return (int)f->workerErr;
    if (f->workerStallAll) return -2;
    unsigned n = f->workerOverflow ? batchMax + 1u : 2;
    if (n > 64) n = 64;
    for (unsigned i = 0; i < n; ++i) {
        out[i] = f->ceUnresolvedAfterMap ? PICASSO_WLAN_SNOC_UNRESOLVED : (uint16_t)i;
    }
    *count = n; f->lastPassCount = n; f->completions += n;
    return 0;
}
static void sleepFn(void *, unsigned) {}
static uint64_t nowFn(void *) { return 0; }

static void applyMode(Fake &f, PicassoWlanSnocConfig &cfg) {
    const std::string m(g_mode);
    // Most failure modes use pendingAll so the IRQ loop is exercised.
    if (m == "irq-claim-fail") { f.pendingAll = true; f.claimErrIndex = 3; f.claimErr = -9; }
    else if (m == "irq-unclaimed") { f.pendingAll = true; f.claimErrIndex = 5; f.claimErr = -16; }
    else if (m == "irq-poison") { f.pendingAll = true; f.poison = true; }
    else if (m == "irq-stale-spi") { f.staleSpi = true; }
    else if (m == "irq-ack-fail") { f.pendingAll = true; f.ackErr = -9; }
    else if (m == "irq-mask-fail") { f.pendingAll = true; f.maskErr = -9; }
    else if (m == "map-fail") { f.mapErr = -9; }
    else if (m == "unmap-fail") { f.unmapErr = -9; }
    else if (m == "cache-flush-missing") { f.cacheFlushMissing = true; }
    else if (m == "cache-invalidate-missing") { f.cacheInvalidateMissing = true; }
    else if (m == "ring-owner-conflict") { f.ringMapMismatchOwner = true; }
    else if (m == "ring-head-overrun") { f.ringHeadOverrun = true; }
    else if (m == "iova-out-of-range") { f.iovaOutside = true; }
    else if (m == "ring-count-zero") { f.countZero = true; }
    else if (m == "worker-stall") { f.workerStallAll = true; }
    else if (m == "worker-io-error") { f.workerErr = -9; }
    else if (m == "worker-batch-overflow") { f.workerOverflow = true; }
    else if (m == "ce-unresolved-after-resolve") {
        // Resolve the CE map but feed an unresolved completion id -> reject.
        for (unsigned i = 0; i < PICASSO_WLAN_SNOC_IRQ_COUNT; ++i) {
            cfg.ceMap.ceId[i] = (uint16_t)i; cfg.ceMap.ringId[i] = (uint16_t)i;
        }
        cfg.ceMap.provisional = false; f.ceUnresolvedAfterMap = true;
    } else if (m == "resolved-ce-map") {
        // On-device map resolved (host placeholder): scan_preconditions_ok may be true.
        for (unsigned i = 0; i < PICASSO_WLAN_SNOC_IRQ_COUNT; ++i) {
            cfg.ceMap.ceId[i] = (uint16_t)i; cfg.ceMap.ringId[i] = (uint16_t)i;
        }
        cfg.ceMap.provisional = false;
    } else if (m == "no-transport") {
        // handled by nulling a callback in main
    }
    // normal / restart / provisional: default config (provisional=true)
}

int main(int argc, char **argv) {
    if (argc > 1) g_mode = argv[1];
    const std::string m(g_mode);

    PicassoWlanSnocConfig cfg; picasso_wlan_snoc_default_config(&cfg);
    Fake f; std::memset(&f, 0, sizeof f);
    applyMode(f, cfg);

    if (m == "normal" || m == "restart") {
        PicassoWlanSnocConfig d; picasso_wlan_snoc_default_config(&d);
        CHECK(d.ceMap.provisional && d.iovaBase == 0xa0000000ULL && d.iovaSize == 0x10000000ULL);
        for (unsigned i = 0; i < PICASSO_WLAN_SNOC_IRQ_COUNT; ++i)
            CHECK(d.ceMap.ceId[i] == PICASSO_WLAN_SNOC_UNRESOLVED);
        uint16_t spi[PICASSO_WLAN_SNOC_IRQ_COUNT];
        picasso_wlan_snoc_irq_spi(spi);
        const uint16_t want[12] = {0x084,0x051,0x19f,0x1a0,0x1a1,0x056,0x05e,0x060,0x052,0x077,0x078,0x057};
        for (unsigned i = 0; i < 12; ++i) CHECK(spi[i] == want[i]); // 12 SPI pinned
    }

    PicassoWlanSnocIo io; std::memset(&io, 0, sizeof io);
    io.context = &f;
    io.irq_status = irqStatus; io.irq_ack = irqAck; io.irq_mask = irqMask;
    io.claim_owner = claimOwner; io.ring_map = ringMap; io.ring_unmap = ringUnmap;
    io.cache_flush = cacheFlush; io.cache_invalidate = cacheInvalidate;
    io.worker_poll = workerPoll; io.sleep = sleepFn; io.now = nowFn;
    if (m == "no-transport") io.irq_status = nullptr;

    PicassoWlanSnocState st;
    const int rc = picasso_wlan_snoc_execute(&io, &cfg, &st);

    CHECK(!st.scan_proven);      // invariant: never claimed
    CHECK(!st.hardwareTouched);  // invariant: offline skeleton

    if (m == "normal" || m == "restart" || m == "resolved-ce-map") {
        CHECK(rc == 0);
        CHECK(st.ownershipComplete && st.irqsObserved == 12);
        CHECK(st.ringsMapped == 1 && st.ringsUnmapped == 1);
        CHECK(f.flushes == 1 && f.invalidates == 1); // cache flush/invalidate paired
        CHECK(st.workerPasses == cfg.workerPassMax && st.completions > 0);
        if (m == "resolved-ce-map") CHECK(st.scan_preconditions_ok);
        else CHECK(!st.scan_preconditions_ok); // provisional => forced false
        std::printf("PASS snoc %s: phase=%s ack=%u comp=%u scan_proven=0 precond=%u\n",
                    m.c_str(), st.phase, st.irqsAcked, st.completions,
                    (unsigned)st.scan_preconditions_ok);
        return 0;
    }

    // Failure modes: each must return nonzero at the expected phase.
    CHECK(rc != 0);
    if (m == "no-transport")
        CHECK(std::string(st.phase) == "init");
    else if (m == "irq-stale-spi" || m == "irq-claim-fail" || m == "irq-unclaimed" ||
             m == "irq-poison" || m == "irq-ack-fail" || m == "irq-mask-fail")
        CHECK(std::string(st.phase) == "irq-ownership");
    else if (m == "map-fail" || m == "unmap-fail" || m == "cache-flush-missing" ||
             m == "cache-invalidate-missing" || m == "ring-owner-conflict" ||
             m == "ring-head-overrun" || m == "iova-out-of-range" || m == "ring-count-zero")
        CHECK(std::string(st.phase) == "ring-ownership");
    else if (m == "worker-stall" || m == "worker-io-error" || m == "worker-batch-overflow" ||
             m == "ce-unresolved-after-resolve")
        CHECK(std::string(st.phase) == "worker");
    std::printf("PASS snoc %s: phase=%s error=%d quarantine=%u\n",
                m.c_str(), st.phase, st.error, (unsigned)st.quarantined);
    return 0;
}
