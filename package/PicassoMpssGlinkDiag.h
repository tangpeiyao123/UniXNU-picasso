// SPDX-License-Identifier: BSD-2-Clause-Patent
// CONTROLLED-WRITE link-liveness diagnostic for the MPSS GLINK edge.
//
// AUTHORIZATION: BRINGUP_STATE.md §9.21/§9.22, chief-review GO. This is the
// ONE controlled write boot to decide, decisively, whether the APPS<->MPSS
// GLINK link is alive at all. It is NOT a transport: it sends exactly ONE
// GLINK VERSION command (cmd=0, version=1), kicks IPCC, then reads back only.
// Default OFF; gated by an explicit boot-arg in the adapter. Zero flash.
//
// MINIMAL WRITE SET (nothing else is written):
//   - TX FIFO item 480 @ head=0: 8 bytes {0x00010000, 0}  (VERSION cmd0/ver1)
//   - desc item 478 +4 (APPS-owned TX head): 0 -> 8
//   - IPCC 0x40800c: 0x00020000 (client 2 / signal 0, GLINK kick)
// FORBIDDEN: desc+8/+12 (RX sides), items 435/428, mutex, kick 0x20002
// (SMP2P signal 2), any QRTR/QMI/WLFW, any OPEN (no lcid allocation yet).
//
// WRITE-BEFORE READ ASSERTIONS (§9.22 Q1, all must hold or abort NO write):
//   - (0,1) partition valid; desc 478 size 32; tx 480 size 16384; rx 611 size 4096
//   - desc payload [TX tail, TX head, RX tail, RX head] all == 0, 8-aligned
//   - TX head (desc+4) == 0; our cached head == 0
// desc control block layout is [TX tail, TX head, RX tail, RX head] (4 x u32).
// Ownership caveat (§9.22 Q2): desc 478 being APPS-owned is a strong inference
// from ADSP-edge isomorphism, NOT a first-hand DTB fact. Safe here because the
// ring is all-zero (no peer driving it) and fastboot boot is non-persistent.
#ifndef PICASSO_MPSS_GLINK_DIAG_H
#define PICASSO_MPSS_GLINK_DIAG_H
#include "WlanSmem.h"

enum {
    PicassoMpssDescId = 478,
    PicassoMpssTxId   = 480,
    PicassoMpssRxId   = 611,
    PicassoMpssDescSize = 32,
    PicassoMpssTxSize   = 16384,
    PicassoMpssRxSize   = 4096,
    PicassoMpssIpccKick = 0x00020000u  // client 2 / signal 0
};

struct picasso_mpss_glink_diag_result {
    int error;                 // 0 OK-write, -1 precondition failed (no write),
                               // -2 partition/item missing, -22 malformed,
                               // -5 write/read IO error, -11 readback mismatch
    uint32_t before[4];        // desc [TX tail, TX head, RX tail, RX head] pre-write
    uint32_t descId, txId, rxId;
    uint32_t descSize, txSize, rxSize;
    uint32_t txHeadWritten;    // value written to desc+4 (expected 8)
    uint32_t afterHead;        // desc+4 readback
    uint32_t afterRxHead;      // desc+12 readback (peer-written; liveness signal)
    uint32_t kickValue;        // value written to IPCC
    bool wrote;                // true only after all preconditions passed
    bool peerResponded;        // afterRxHead changed from before[3] -> peer alive
};

// The IO bundle splits SMEM (read/write32/barrier) from the IPCC kick MMIO so
// a host harness can assert the exact kick value and SMEM writes.
struct picasso_mpss_glink_diag_io {
    struct wlan_smem_io smem;                       // reader for SMEM reads
    int (*smem_write32)(void *, uint32_t, uint32_t);// SMEM 32-bit write
    void (*barrier)(void *);                        // memory barrier
    int (*ipcc_kick)(void *, uint32_t);             // IPCC write (value asserted)
    void *context;
};

// Read-only precondition check + single VERSION write + kick + readback.
// Returns 0 and fills out on success; never writes if any precondition fails.
static inline int picasso_mpss_glink_diag_write_version(
        const struct picasso_mpss_glink_diag_io *io,
        struct picasso_mpss_glink_diag_result *out) {
    if (!io || !io->smem.read || !io->smem_write32 || !io->barrier ||
        !io->ipcc_kick || !out) return -22;
    *out = picasso_mpss_glink_diag_result{};
    out->error = -22;

    struct wlan_smem_partition p = {};
    if (wlan_smem_partition(&io->smem, 0, 1, &p)) { out->error = -2; return -2; }

    struct wlan_smem_item desc = {}, tx = {}, rx = {};
    if (wlan_smem_find(&io->smem, &p, PicassoMpssDescId, &desc) ||
        wlan_smem_find(&io->smem, &p, PicassoMpssTxId, &tx) ||
        wlan_smem_find(&io->smem, &p, PicassoMpssRxId, &rx)) { out->error = -2; return -2; }
    out->descId = PicassoMpssDescId; out->txId = PicassoMpssTxId; out->rxId = PicassoMpssRxId;
    out->descSize = desc.size; out->txSize = tx.size; out->rxSize = rx.size;
    if (desc.size != PicassoMpssDescSize || tx.size != PicassoMpssTxSize ||
        rx.size != PicassoMpssRxSize) { out->error = -22; return -22; }

    // Read desc control block [TX tail, TX head, RX tail, RX head] twice.
    uint8_t a[16], b[16];
    if (wlan_smread(&io->smem, desc.offset, a, 16) ||
        wlan_smread(&io->smem, desc.offset, b, 16)) { out->error = -5; return -5; }
    for (unsigned i = 0; i < 16; ++i) if (a[i] != b[i]) { out->error = -11; return -11; }
    for (unsigned i = 0; i < 4; ++i) out->before[i] = wlan_sm32(a + 4 * i);

    // §9.22 Q1 precondition: all four fields zero, aligned, within FIFO sizes.
    // (0 is 8-aligned and < all sizes; equality check is the real gate.)
    bool pre = true;
    for (unsigned i = 0; i < 4; ++i) if (out->before[i] != 0) pre = false;
    if (!pre) { out->error = -1; return -1; }  // NO write, NO kick

    void *ctx = io->context;
    // Publish payload first, then advance TX head, then fence, then kick.
    // Mirror transmit():42-49 (fence before reuse; payload; head; fence; kick).
    io->barrier(ctx);
    const uint32_t version[2] = {0x00010000u, 0}; // cmd=0 | version=1 << 16
    if (io->smem_write32(ctx, tx.offset + 0, version[0]) ||
        io->smem_write32(ctx, tx.offset + 4, version[1])) { out->error = -5; return -5; }
    io->barrier(ctx);
    if (io->smem_write32(ctx, desc.offset + 4, 8)) { out->error = -5; return -5; }
    io->barrier(ctx);
    out->txHeadWritten = 8;
    out->wrote = true;
    out->kickValue = PicassoMpssIpccKick;
    if (io->ipcc_kick(ctx, PicassoMpssIpccKick)) { out->error = -5; return -5; }

    // Readback only; no further writes.
    uint8_t head[4], rxh[4];
    if (wlan_smread(&io->smem, desc.offset + 4, head, 4) ||
        wlan_smread(&io->smem, desc.offset + 12, rxh, 4)) { out->error = -5; return -5; }
    out->afterHead = wlan_sm32(head);
    out->afterRxHead = wlan_sm32(rxh);
    if (out->afterHead != 8) { out->error = -11; return -11; } // our write didn't stick
    out->peerResponded = (out->afterRxHead != out->before[3]); // desc+12 peer-written
    out->error = 0;
    return 0;
}
#endif
