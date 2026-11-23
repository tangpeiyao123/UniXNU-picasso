// SPDX-License-Identifier: BSD-2-Clause-Patent
// Picasso WCSS/SNOC IRQ + DMA-ring ownership skeleton -- OFFLINE INJECTED.
//
// Step-4 skeleton: fixes the ONE first-hand fact we have (the 12 raw GIC_SPI
// numbers from picasso.dts:7305, via WlanBoard.cpp:8-11) and makes every unknown
// an injected, explicitly provisional parameter. No ath10k source exists in
// this tree; there is no CE<->SPI map, no ring descriptor, no MSI setup. The
// only ath11k/WCN6750 reference on this machine (hakios-ref) is a DIFFERENT
// chip/platform and its constants are forbidden here.
//
// HONESTY CONTRACT (independent review, BRINGUP-step4-review.md):
// - The 12 SPI values are first-hand and pinned. They are NOT asserted to be
//   CE0..CE11: the DT has no interrupt-names (WlanBoard.h:88). The CE/ring id
//   for each SPI is injected via cfg.ceMap[] and defaults to unresolved with
//   ceMapProvisional=true.
// - scan_proven is ALWAYS false. scan_preconditions_ok reports ONLY that the
//   skeleton's own invariants held (each IRQ acked once, ring ownership/cache
//   pairs, bounded worker progress). It is NOT "real scan preconditions ready":
//   while ceMapProvisional is true it is FORCED false, because we do not know
//   which IRQ is which CE/ring.
// - No MMIO is written, no real DMA mapping, no MSI, no private blob, no
//   production stage4 hookup. hardwareTouched stays false throughout.
#ifndef PICASSO_WLAN_SNOC_H
#define PICASSO_WLAN_SNOC_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// First-hand: raw GIC_SPI numbers from picasso.dts:7305 (WlanBoard.cpp:8-11).
// GIC INTID = 32 + SPI. Count is fixed at 12; values are asserted by source pin.
#define PICASSO_WLAN_SNOC_IRQ_COUNT 12
#define PICASSO_WLAN_SNOC_UNRESOLVED 0xffffu

// Injected CE/ring identity per IRQ. Unresolved by default; must be resolved
// on-device (read-only, separately authorised) before it may be trusted.
struct PicassoWlanSnocCeMap {
    uint16_t ceId[PICASSO_WLAN_SNOC_IRQ_COUNT];  // CE id per IRQ, or UNRESOLVED
    uint16_t ringId[PICASSO_WLAN_SNOC_IRQ_COUNT]; // ring id per IRQ, or UNRESOLVED
    bool provisional;                            // true until on-device resolution
};

// Bounded injected ring description. NOT the ath10k descriptor format (unknown
// here): only ownership + head/tail progress + IOVA bounds are modelled.
struct PicassoWlanSnocRing {
    uint32_t count;      // element count (bounded)
    uint32_t elemBytes;  // element size (bounded)
    uint64_t iovaBase;   // must lie within the injected IOVA pool
    uint32_t head, tail; // producer/consumer cursors
    uint8_t owner;       // 0 = unowned, 1 = APPS, 2 = firmware
};

// Config: everything the host cannot know is injected. Board IOVA/SID facts
// come from WlanBoard.h and are asserted by source pin.
struct PicassoWlanSnocConfig {
    struct PicassoWlanSnocCeMap ceMap;
    uint64_t iovaBase;   // 0xa0000000 (WlanBoard.h:66)
    uint64_t iovaSize;   // 0x10000000 (WlanBoard.h:67)
    unsigned workerBatchMax;  // bounded completions per worker pass
    unsigned workerPassMax;   // bounded passes before stall
    unsigned timeoutMs;
};

// All hardware boundaries are injected. Host supplies deterministic stand-ins.
struct PicassoWlanSnocIo {
    void *context;

    // Read pending IRQ status (bit i = SPI[i]). Return 0 or negative IO error.
    int (*irq_status)(void *ctx, uint32_t *mask12);
    // Ack / mask one IRQ by index. Return 0 or negative.
    int (*irq_ack)(void *ctx, unsigned index);
    int (*irq_mask)(void *ctx, unsigned index);
    // Claim ownership of an IRQ/ring. 0 = claimed, -16 = contention,
    // -115 = poisoned/quarantine, other negative = IO error.
    int (*claim_owner)(void *ctx, unsigned index, uint8_t owner);

    // Ring map/unmap + cache maintenance, all injected (no real DMA).
    int (*ring_map)(void *ctx, struct PicassoWlanSnocRing *ring);
    int (*ring_unmap)(void *ctx, struct PicassoWlanSnocRing *ring);
    int (*cache_flush)(void *ctx, const struct PicassoWlanSnocRing *ring);
    int (*cache_invalidate)(void *ctx, const struct PicassoWlanSnocRing *ring);

    // One worker pass: produce up to batchMax completions into out[], return
    // the count. -2 = no work (stall candidate), other negative = IO error.
    int (*worker_poll)(void *ctx, unsigned batchMax, uint16_t *out, unsigned *count);

    void (*sleep)(void *ctx, unsigned ms);
    uint64_t (*now)(void *ctx);
};

// Outcome. Both scan flags are honest: scan_proven is always false;
// scan_preconditions_ok is forced false while ceMap.provisional is true.
struct PicassoWlanSnocState {
    const char *phase;
    unsigned step;
    int error;
    unsigned irqsObserved, irqsAcked, irqsStale, irqsUnclaimed;
    unsigned ringsMapped, ringsUnmapped;
    unsigned workerPasses, completions;
    bool ownershipComplete;
    bool scan_proven;             // ALWAYS false
    bool scan_preconditions_ok;   // skeleton invariants only; provisional => false
    bool quarantined;
    bool hardwareTouched;         // ALWAYS false
};

void picasso_wlan_snoc_default_config(struct PicassoWlanSnocConfig *cfg);

// First-hand IRQ numbers, exposed for host assertion (read-only copy).
void picasso_wlan_snoc_irq_spi(uint16_t out[PICASSO_WLAN_SNOC_IRQ_COUNT]);

// IOVA-range validation for a ring: base must be inside [iovaBase,iovaBase+iovaSize)
// and the whole ring (count*elemBytes) must fit. Pure arithmetic, no MMIO.
int picasso_wlan_snoc_ring_in_iova(const struct PicassoWlanSnocConfig *cfg,
                                   const struct PicassoWlanSnocRing *ring);

int picasso_wlan_snoc_execute(const struct PicassoWlanSnocIo *io,
                              const struct PicassoWlanSnocConfig *cfg,
                              struct PicassoWlanSnocState *state);

#ifdef __cplusplus
}
#endif
#endif
