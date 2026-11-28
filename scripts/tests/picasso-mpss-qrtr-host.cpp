// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host regression for P1 (MPSS GLINK discovery + QRTR WLFW lookup).
// CHECK (not <assert.h>) so zig's default -DNDEBUG cannot disable assertions.
// Synthetic SMEM fixtures exercise every branch of the read-only geometry
// probe. ZERO-WRITE CONTRACT: the probe only owns wlan_smem_io.read; this
// harness never supplies a write callback, so a write cannot compile in.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "WlanSmem.h"
#include "PicassoGlinkProbe.h"
#include "PicassoQrtr.h"
#include "PicassoWlanWlfw.h"

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (line %d)\n", #c, __LINE__); std::exit(1); } } while (0)

static uint8_t g_smem[0x200000];
static unsigned g_reads = 0;

static int smemRead(void *ctx, uint32_t off, void *out, uint32_t len) {
    (void)ctx; ++g_reads;
    if (off > WLAN_SMEM_SIZE || len > WLAN_SMEM_SIZE - off) return -1;
    std::memcpy(out, g_smem + off, len);
    return 0;
}

// Build a minimal valid (0,1) private partition. Lay out N uncached items from
// offset 0x1000: [a5a5 header 16B][header bytes][payload]. ids/sizes are passed
// in. ctrl[] is written into the first item's payload (the desc control block).
static void put32(uint32_t off, uint32_t v) {
    g_smem[off] = uint8_t(v); g_smem[off+1] = uint8_t(v>>8);
    g_smem[off+2] = uint8_t(v>>16); g_smem[off+3] = uint8_t(v>>24);
}
static void put16(uint32_t off, uint16_t v) {
    g_smem[off] = uint8_t(v); g_smem[off+1] = uint8_t(v>>8);
}
static uint32_t g_partitionEnd = 0;

// One partition header at 0x1000 (magic $PRT, local=0, remote=1, cached=uncached+8).
static void beginPartition() {
    std::memset(g_smem, 0, sizeof g_smem);
    put32(0x5c, 12u << 16); // SMEM version 12
    const uint32_t hdr = 0x1000;
    put32(hdr, 0x54525024); put16(hdr+4, 0); put16(hdr+6, 1);
    // size/cached/uncached patched by addItem/finishPartition
    put32(hdr+20, 64); // cacheline
    g_partitionEnd = hdr + 32;
}

static uint32_t addItem(uint16_t id, uint32_t payloadSize) {
    const uint32_t at = g_partitionEnd;
    put16(at, 0xa5a5); put16(at+2, id);
    put32(at+4, payloadSize);
    put16(at+8, 0); put16(at+10, 0); // pad / header bytes
    g_partitionEnd = at + 16 + payloadSize;
    return at + 16; // item data offset
}

static void finishPartition() {
    const uint32_t hdr = 0x1000;
    const uint32_t uncached = g_partitionEnd - hdr;
    put32(hdr+8, uncached + 64);   // partition size
    put32(hdr+12, uncached);       // uncached = end of last item
    put32(hdr+16, uncached + 64);  // cached
    put32(hdr+20, 64);             // cacheline
    // TOC: one entry for host=0/remote=1 pointing at 0x1000.
    const uint32_t toc = WLAN_SMEM_SIZE - 4096;
    put32(toc, 0x434f5424); put32(toc+4, 1); put32(toc+8, 1);
    uint32_t e = toc + 32;
    put32(e, 0x1000); put32(e+4, uncached + 64);
    put16(e+12, 0); put16(e+14, 1); put32(e+16, 64);
}

// ---- geometry probe cases -------------------------------------------------
static void caseOk() {
    beginPartition();
    uint32_t desc = addItem(900, 32);
    addItem(901, 16384);
    addItem(902, 8192);
    put32(desc, 0); put32(desc+4, 128); // TX tail=0, head=128 (inside tx)
    put32(desc+8, 0); put32(desc+12, 64);
    finishPartition();
    wlan_smem_io io = {nullptr, smemRead};
    picasso_glink_probe_result pr = {};
    g_reads = 0;
    CHECK(picasso_mpss_glink_probe(&io, &pr) == 0);
    CHECK(pr.cand == 3 && pr.descPresent && pr.txPresent && pr.rxPresent);
    CHECK(pr.descCount == 1 && pr.desc[0].id == 900);
    CHECK(pr.txId == 901 && pr.rxId == 902);
    CHECK(pr.validDesc == 1 && pr.sendReady);
    CHECK(pr.desc[0].stable && pr.desc[0].ptrValid && pr.desc[0].valid);
    CHECK(pr.desc[0].ctrl[4] == 128); // TX head byte 0
    CHECK(g_reads > 0);
}

static void caseNoCandidate() {
    beginPartition();
    addItem(700, 8); addItem(701, 8); // 8B items: below the 16B rx floor, no GLINK geometry
    finishPartition();
    wlan_smem_io io = {nullptr, smemRead};
    picasso_glink_probe_result pr = {};
    CHECK(picasso_mpss_glink_probe(&io, &pr) == -2);
}

// Mirror the P1 on-device topology exactly (console-p1-boot2.log / §9.15):
//   tx=480/16384, rx=611/4096, two 32B candidates (one was reported as 605;
//   the other's id was lost by the old probe). Only one 32B item is a real
//   GLINK ring control block. The probe must record BOTH and select the real
//   one by content -- not early-return -22 and not keep only the last.
static void caseTwoDescDisambiguate() {
    beginPartition();
    uint32_t junk = addItem(605, 32);  // 32B candidate reported by P1
    uint32_t real = addItem(606, 32);  // other 32B candidate (P1 lost its id)
    addItem(480, 16384);              // TX FIFO, id from P1 log
    addItem(611, 4096);               // RX FIFO, id from P1 log
    put32(junk, 0xdeadbeef); put32(junk+4, 0xdeadbeef); // garbage pointers
    put32(real, 0); put32(real+4, 256); // TX tail=0, head=256 inside 16384
    put32(real+8, 0); put32(real+12, 128);
    finishPartition();
    wlan_smem_io io = {nullptr, smemRead};
    picasso_glink_probe_result pr = {};
    CHECK(picasso_mpss_glink_probe(&io, &pr) == 0);
    CHECK(pr.txId == 480 && pr.txSize == 16384); // topology locked to P1 log
    CHECK(pr.rxId == 611 && pr.rxSize == 4096);
    CHECK(pr.descCount == 2);          // BOTH candidates recorded (P1 bug fixed)
    CHECK(pr.roles[0] == 2);
    CHECK(pr.validDesc == 1 && pr.sendReady);
    // Disambiguation is by pointer content, matching the on-device mechanism.
    bool junkValid = false, realValid = false;
    for (uint32_t n = 0; n < pr.descCount; ++n) {
        if (pr.desc[n].id == 605 && pr.desc[n].valid) junkValid = true;
        if (pr.desc[n].id == 606 && pr.desc[n].valid) realValid = true;
    }
    CHECK(!junkValid && realValid);
}

static int g_descReads = 0;
static uint32_t g_descDataOff = 0, g_descDataOff2 = 0;
static int g_flipA = 0, g_flipB = 0;
static int smemReadFlipDesc(void *ctx, uint32_t off, void *out, uint32_t len) {
    int rc = smemRead(ctx, off, out, len);
    // Simulate a concurrent peer update: corrupt a desc payload on the SECOND
    // read of that exact offset so the probe's two reads disagree. Counters
    // are per-offset so multiple candidates can each be made unstable.
    if (rc == 0 && len >= 16) {
        if (off == g_descDataOff && ++g_flipA == 2) (static_cast<uint8_t *>(out))[0] ^= 0x01;
        if (off == g_descDataOff2 && ++g_flipB == 2) (static_cast<uint8_t *>(out))[0] ^= 0x01;
    }
    return rc;
}
static void caseUnstable() {
    beginPartition();
    uint32_t desc = addItem(920, 32);
    addItem(921, 16384);
    put32(desc, 0); put32(desc+4, 128);
    finishPartition();
    g_descDataOff = desc; g_descDataOff2 = 0; g_descReads = 0; g_flipA = 0; g_flipB = 0;
    wlan_smem_io io = {nullptr, smemReadFlipDesc};
    picasso_glink_probe_result pr = {};
    CHECK(picasso_mpss_glink_probe(&io, &pr) == -11);
    g_descReads = 0;
}

// All 32B candidates unstable -> retryable -11 (not confused with no-desc).
static void caseAllUnstable() {
    beginPartition();
    uint32_t a = addItem(700, 32);
    uint32_t b = addItem(701, 32);
    addItem(702, 16384);
    put32(a, 0); put32(a+4, 128);
    put32(b, 0); put32(b+4, 128);
    finishPartition();
    g_descDataOff = a; g_descDataOff2 = b; g_descReads = 0; g_flipA = 0; g_flipB = 0;
    wlan_smem_io io = {nullptr, smemReadFlipDesc};
    picasso_glink_probe_result pr = {};
    CHECK(picasso_mpss_glink_probe(&io, &pr) == -11);
    CHECK(pr.descCount == 2);
    g_descReads = 0; g_flipA = 0; g_flipB = 0;
}

static void caseNoPartition() {
    std::memset(g_smem, 0, sizeof g_smem);
    put32(0x5c, 12u << 16);
    // TOC with zero entries -> (0,1) partition absent.
    const uint32_t toc = WLAN_SMEM_SIZE - 4096;
    put32(toc, 0x434f5424); put32(toc+4, 1); put32(toc+8, 0);
    wlan_smem_io io = {nullptr, smemRead};
    picasso_glink_probe_result pr = {};
    CHECK(picasso_mpss_glink_probe(&io, &pr) == -2);
}

static void caseBadTxPointer() {
    beginPartition();
    uint32_t desc = addItem(930, 32);
    addItem(931, 16384);
    put32(desc, 20000); put32(desc+4, 0); // TX tail >= tx size (16384)
    finishPartition();
    wlan_smem_io io = {nullptr, smemRead};
    picasso_glink_probe_result pr = {};
    CHECK(picasso_mpss_glink_probe(&io, &pr) == -22);
}

// ---- QRTR WLFW 0x45 lookup (no QMI binding change) -------------------------
static void putQ32(uint8_t *p, uint32_t off, uint32_t v) {
    for (unsigned n = 0; n < 4; ++n) p[off+n] = uint8_t(v >> (8*n));
}
static void newServer(uint8_t *p, uint32_t service, uint32_t instance, uint32_t node, uint32_t port) {
    std::memset(p, 0, 64);
    putQ32(p,0,1); putQ32(p,4,4); putQ32(p,8,2); putQ32(p,12,0xfffffffe); putQ32(p,20,20);
    putQ32(p,24,1); putQ32(p,28,0xfffffffe);
    putQ32(p,32,4); putQ32(p,36,service); putQ32(p,40,instance); putQ32(p,44,node); putQ32(p,48,port);
}
static void hello(uint8_t *p) {
    std::memset(p, 0, 64);
    putQ32(p,0,1); putQ32(p,4,2); putQ32(p,8,2); putQ32(p,12,0xfffffffe); putQ32(p,20,20);
    putQ32(p,24,1); putQ32(p,28,0xfffffffe); putQ32(p,32,2);
}
static void caseQrtrWlfwLookup() {
    PicassoQrtr q = {};
    uint8_t p[64];
    hello(p); CHECK(!picasso_qrtr_receive(&q, p, 52)); // peer node 2 known
    CHECK(q.helloReceived && q.peer == 2);
    newServer(p, PicassoQrtrWlfwService, PicassoQrtrWlfwInstance, 2, 0x1111);
    CHECK(!picasso_qrtr_receive(&q, p, 52));
    CHECK(q.services[0].used && q.services[0].service == 0x45);
    // QMI binding stays untouched: only ADSP servreg 0x42/0x4a01 binds.
    CHECK(!q.qmi.bound);
    uint32_t node = 0, port = 0;
    CHECK(picasso_qrtr_lookup(&q, PicassoQrtrWlfwService, PicassoQrtrWlfwInstance, &node, &port));
    CHECK(node == 2 && port == 0x1111);
    CHECK(!picasso_qrtr_lookup(&q, 0x42, 0x4a01, nullptr, nullptr));
    // Existing servreg binding still works unchanged (zero regression).
    newServer(p, 0x42, 0x4a01, 2, 73);
    CHECK(!picasso_qrtr_receive(&q, p, 52));
    CHECK(q.qmi.bound && q.qmi.node == 2 && q.qmi.port == 73);
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "all";
    if (!std::strcmp(mode,"all") || !std::strcmp(mode,"ok")) caseOk();
    if (!std::strcmp(mode,"all") || !std::strcmp(mode,"nocand")) caseNoCandidate();
    if (!std::strcmp(mode,"all") || !std::strcmp(mode,"twodesc")) caseTwoDescDisambiguate();
    if (!std::strcmp(mode,"all") || !std::strcmp(mode,"allunstable")) caseAllUnstable();
    if (!std::strcmp(mode,"all") || !std::strcmp(mode,"unstable")) caseUnstable();
    if (!std::strcmp(mode,"all") || !std::strcmp(mode,"nopart")) caseNoPartition();
    if (!std::strcmp(mode,"all") || !std::strcmp(mode,"badtx")) caseBadTxPointer();
    if (!std::strcmp(mode,"all") || !std::strcmp(mode,"qrtr")) caseQrtrWlfwLookup();
    // WLFW sequence reference table self-check (pure, no transport).
    CHECK(picasso_wlfw_sequence_check() == 0);
    std::puts("PASS MPSS GLINK geometry discovery (zero-write), QRTR WLFW lookup, WLFW sequence table");
    return 0;
}
