// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host harness for the step-2 pil_modem PIL state machine. Executes the REAL
// production order with deterministic injected boundaries; no live SMC/MMIO.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "PicassoWlanModemPil.h"

// <assert.h> is compiled out by zig c++ (it defines NDEBUG even at -O0), so
// use an explicit check that can never be optimized away.
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static const char *mode;
static bool is(const char *s) { return !strcmp(mode, s); }
static unsigned part_reads, hashes, writes, leases, releases, on, off, pas_cmds, polls;
static bool owned, poison;
static uint32_t commands[8];
static uint64_t cmd_addr[8], cmd_bytes[8];
static uint32_t cmd_pasid[8];
static uint16_t ticks;

// 33 non-empty parts (index 0 = mdt). A few indices are intentionally absent.
static int read_part(void *ctx, unsigned index, uint8_t *dst, size_t cap, size_t *out) {
    (void)ctx; (void)cap;
    CHECK(cap >= 4096);
    ++part_reads;
    CHECK(index <= 34);
    // read_part index i>=1 backs PH i-1. PH18/PH25 have filesz==0 -> absent;
    // PH34 (index 34) is BSS-only with no disk file, so never requested.
    if (index == 19 || index == 26) return -2;   // filesz==0 parts absent.
    if (is("fw-missing") && index == 0) return -2;
    if (is("metadata-short") && index == 0) { *out = 100; memset(dst, 0x5a, 100); return 0; }
    if (index == 0) { *out = 9420; memset(dst, 0x5a, 9420); }
    else { *out = 4096; memset(dst, 0x3c, 4096); }
    return 0;
}
static int sha256(void *ctx, const uint8_t *data, size_t bytes, uint8_t out[32]) {
    (void)ctx; (void)data; (void)bytes; ++hashes; memset(out, 0x11, 32);
    return is("metadata-hash") && hashes == 1 ? -8 : 0;
}
static int rpmh_write(void *ctx, unsigned rail, uint32_t addr, uint32_t value) {
    (void)ctx; (void)addr; (void)value; (void)rail;
    CHECK(rail < 2);  // MSS has 2 proxy rails, never the 5 WCSS rails.
    ++writes;
    if (is("vote-fail") && writes == 1) return -60;
    return 0;
}
static int lease_acquire(void *ctx) {
    (void)ctx; ++leases;
    CHECK(!owned);
    if (is("lease") || is("lease-gpu") || is("lease-adsp")) return -16;
    if (is("poisoned")) return -115;
    owned = true; return 0;
}
static void lease_release(void *ctx, bool uncertain) {
    (void)ctx; CHECK(owned); owned = false; poison = uncertain; ++releases;
}
static int aop(void *ctx, bool on_flag) {
    (void)ctx;
    if (on_flag) { ++on; return is("aop-fail") ? -22 : 0; }
    ++off; return is("off-fail") ? -115 : 0;
}
static int pas(void *ctx, unsigned cmd, uint64_t addr, uint64_t bytes, uint32_t pasid) {
    (void)ctx;
    CHECK(owned && pas_cmds < 8);
    CHECK(pasid == 4);                       // modem is PAS 4, never the ADSP id 1.
    commands[pas_cmds] = cmd; cmd_addr[pas_cmds] = addr; cmd_bytes[pas_cmds] = bytes;
    cmd_pasid[pas_cmds] = pasid; ++pas_cmds;
    if (cmd == 1) { CHECK(addr == 0x8bc18000 && bytes == 0); }
    if (cmd == 2) { CHECK(addr == 0x8c000000 && bytes == 0xf800000); }
    if (cmd == 5) { CHECK(pas_cmds == 3 && cmd_addr[1] == 0x8c000000); }
    if (cmd == 6) { CHECK(writes >= 2 && addr == 0 && bytes == 0); }  // RESET takes no address; rails reasserted on shutdown
    if ((is("init-fail") && cmd == 1) || (is("setup-fail") && cmd == 2) ||
        (is("auth-fail") && cmd == 5) || (is("reset-fail") && cmd == 6)) return -13;
    return 0;
}
static int poll_peer(void *ctx, struct WlanModemPeerEvent *e) {
    (void)ctx; ++polls; memset(e, 0, sizeof *e);
    if (polls == 1) {   // peer preflight must see "absent" (-2); a stale ready is rejected.
        if (is("stale-peer")) { e->hasValue = true; e->value = 2; return 0; }
        if (is("fatal-peer")) { e->restart = true; e->flags = 1; return 0; }
        return -2;
    }
    if (polls == 2) {   // peer-before-auth must also be absent.
        if (is("pre-auth-peer")) { e->hasValue = true; return 0; }
        return -2;
    }
    // ready phase.
    if (is("timeout")) return -2;
    if (is("lost-ready")) { e->hasValue = true; return 0; }
    if (is("restart")) { e->restart = true; return 0; }
    e->hasValue = true; e->value = is("no-handover") ? 2 : 6; e->changed = 2;
    return 0;
}
static void sleep_fn(void *ctx, unsigned ms) { (void)ctx; ticks += (uint16_t)ms; CHECK(ticks < 20000); }
static uint64_t now_fn(void *ctx) { (void)ctx; return ticks; }
// Helper for the timeout assertion below.
static bool shutdowns_ok(const struct WlanModemPilState *st) { return st->phase && st->step != 0; }

int main(int argc, char **argv) {
    (void)argc;
    if (argc < 2) return 2;
    mode = argv[1];
    struct WlanModemPilIo io = {0, read_part, sha256, rpmh_write, lease_acquire, lease_release,
                                aop, pas, poll_peer, sleep_fn, now_fn};
    struct WlanModemPilConfig cfg;
    picasso_wlan_modem_pil_default_config(&cfg);
    struct WlanModemPilState st;
    int rc = picasso_wlan_modem_pil_execute(&io, &cfg, &st);

    // Board constants are pinned by the implementation, not caller-mutable here.
    CHECK(cfg.pasId == 4 && cfg.carveoutBase == 0x8c000000ULL && cfg.carveoutBytes == 0xf800000ULL);
    CHECK(cfg.metadataBase == 0x8bc18000ULL && cfg.rails == 2 && cfg.mdtBytes == 9420);

    bool failure = is("fw-missing") || is("metadata-short") || is("metadata-hash") ||
        is("vote-fail") || is("lease") || is("lease-gpu") || is("lease-adsp") || is("poisoned") ||
        is("aop-fail") || is("init-fail") || is("setup-fail") || is("auth-fail") ||
        is("stale-peer") || is("fatal-peer") || is("pre-auth-peer") || is("timeout") ||
        is("lost-ready") || is("restart");
    if (failure) CHECK(rc != 0 && st.error != 0);

    // Ordering invariants that must hold on EVERY path.
    CHECK(!owned);                                   // lease always released.
    for (unsigned i = 0; i < pas_cmds; ++i) CHECK(cmd_pasid[i] == 4);
    // AUTH (5) is only issued after MEM (2) and segment loads.
    for (unsigned i = 0; i < pas_cmds; ++i) if (commands[i] == 5) {
        bool mem = false; for (unsigned j = 0; j < i; ++j) if (commands[j] == 2) mem = true;
        CHECK(mem); (void)mem;
    }

    if (strncmp(mode, "normal", 6) == 0 || is("no-handover")) {
        CHECK(rc == 0 && st.ready && st.authenticated && st.released);
        CHECK(commands[0] == 1 && commands[1] == 2 && commands[2] == 5 && commands[3] == 6);
        CHECK(pas_cmds == 4 && writes == 4 && on == 1 && off == 1);  // 2 proxy votes + 2 shutdown reasserts
        CHECK(st.partsLoaded == 32 && !st.quarantined);   // PH0..PH33 minus absent PH18/PH25.
        CHECK(!poison);
        if (is("no-handover")) CHECK(!st.handed);
        else CHECK(st.handed);
    }
    if (is("lease") || is("lease-gpu") || is("lease-adsp")) {
        CHECK(rc == -16 && pas_cmds == 0);  // never issue SCM without the gate.
    }
    if (is("poisoned") || is("vote-fail") || is("reset-fail") || is("off-fail")) {
        CHECK(st.quarantined);
    }
    if (is("stale-peer") || is("fatal-peer")) {
        CHECK(pas_cmds == 0 && on == 0 && writes == 0);  // no side effects preflight.
    }
    if (is("timeout")) CHECK(ticks >= cfg.timeoutMs && shutdowns_ok(&st) && rc == -60);

    (void)rc;
    puts("PASS pil_modem PIL state machine ordering, PAS-4 pinning, lease/quarantine and failure cleanup");
    return 0;
}
