// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host harness for the step-3 WLFW/QMI injected transport skeleton.
// Deterministic injected boundaries; NO hardware, NO real QMI. CHECK (not
// <assert.h>) so zig's default -DNDEBUG cannot silently disable assertions.
#include "PicassoWlanWlfw.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (line %d)\n", #c, __LINE__); std::exit(1); } } while (0)

static const char *g_mode = "normal";

// Deterministic injected device model. Each field tweaks one boundary to drive
// a single failure mode, mirroring the step-2 failure-injection discipline.
struct Fake {
    const PicassoWlanWlfwConfig *cfg;
    unsigned sends, kicks, waits, txnSeen[32], nTxn;
    unsigned requests[32], reqArgs[32][2];
    unsigned transientLeft;     // >0 -> send_request returns -3 (retry)
    unsigned lastReq;           // request that receives injected faults
    unsigned failReq;           // request number that returns a hard failure
    bool noAnswer, discTimeout, wrongService, wrongInstance, wrongNode, wrongPort;
    bool kickFail, fifoFull, fwTimeout, msaTimeout, staleInd;
    bool tlvOverrun, malformedInd;
    bool presenceMissing;
    unsigned discoveryPolls;
};

static int discover(void *p, const PicassoWlanWlfwConfig *cfg, PicassoWlanWlfwService *out) {
    Fake *f = (Fake *)p; f->cfg = cfg; ++f->discoveryPolls;
    std::memset(out, 0, sizeof *out);
    if (f->discTimeout) return -2;
    if (f->noAnswer) { out->found = false; return 0; }
    out->found = true;
    out->service  = f->wrongService ? cfg->service + 1 : cfg->service;
    out->instance = f->wrongInstance ? cfg->instance + 1 : cfg->instance;
    out->node     = f->wrongNode ? cfg->node + 1 : cfg->node;
    out->port     = f->wrongPort ? cfg->port + 1 : cfg->port;
    out->version  = cfg->version;
    return 0;
}

static int presence(void *p, PicassoWlanWlfwServicePresence *out) {
    Fake *f = (Fake *)p; std::memset(out, 0, sizeof *out);
    out->wlanFw = !f->presenceMissing;
    out->tftp = true; out->pdr = true; // names only
    return 0;
}

static int loadImage(void *) { return 0; }

static int sendRequest(void *p, unsigned txn, unsigned request, uint64_t a0, uint64_t a1) {
    Fake *f = (Fake *)p;
    if (f->nTxn < 32) { f->txnSeen[f->nTxn++] = txn; f->requests[f->sends] = request;
                        f->reqArgs[f->sends][0] = (unsigned)a0; f->reqArgs[f->sends][1] = (unsigned)a1; }
    ++f->sends; f->lastReq = request;
    if (f->transientLeft) { --f->transientLeft; return -3; }
    if (f->fifoFull) return -4;
    if (f->failReq && request == f->failReq) return -9;
    return 0;
}

static int kick(void *p) {
    Fake *f = (Fake *)p; ++f->kicks;
    return f->kickFail ? -7 : 0;
}

// Build a minimal well-formed QMI indication: 7-byte header + one empty TLV.
static size_t makeInd(uint8_t *buf, unsigned txn, bool malformed, bool tlvOverrun) {
    buf[0] = 0x04; buf[1] = (uint8_t)txn; buf[2] = (uint8_t)(txn >> 8);
    buf[3] = 0x01; buf[4] = 0x00; // message id (provisional, not asserted)
    buf[5] = 0x03; buf[6] = 0x00; // body length = 3 (one empty TLV)
    buf[7] = 0x00; buf[8] = 0x00; buf[9] = 0x00;
    if (malformed) buf[0] = 0x02;            // wrong type -> reject
    if (tlvOverrun) { buf[8] = 0x20; buf[9] = 0x00; } // len 32 > remaining
    return 10;
}

static int waitInd(void *p, unsigned txn, unsigned indication, unsigned,
                   uint8_t *buf, size_t cap, size_t *out) {
    Fake *f = (Fake *)p; ++f->waits;
    if ((indication == 1 && f->fwTimeout) || (indication == 2 && f->msaTimeout)) return -2;
    size_t n = makeInd(buf, f->staleInd ? txn + 1 : txn, f->malformedInd, f->tlvOverrun);
    if (n > cap) return -5;
    *out = n;
    return 0;
}

static void sleepFn(void *, unsigned) {}
static uint64_t nowFn(void *) { return 0; }

static void applyMode(Fake &f, PicassoWlanWlfwConfig &cfg) {
    const std::string m(g_mode);
    if (m == "normal" || m == "restart") {
        cfg.service = 0x2e; cfg.instance = 1; cfg.node = 1; cfg.port = 0x188;
        cfg.provisional = false; // resolved on-device (values here are harness placeholders)
    } else if (m == "discovery-no-answer") {
        f.noAnswer = true;
    } else if (m == "discovery-timeout") {
        f.discTimeout = true;
    } else if (m == "wrong-service") {
        f.wrongService = true; cfg.service = 0x2e; cfg.instance = 1; cfg.node = 1; cfg.port = 0x188; cfg.provisional = false;
    } else if (m == "wrong-instance") {
        f.wrongInstance = true; cfg.service = 0x2e; cfg.instance = 1; cfg.node = 1; cfg.port = 0x188; cfg.provisional = false;
    } else if (m == "wrong-node-port") {
        f.wrongNode = true; cfg.service = 0x2e; cfg.instance = 1; cfg.node = 1; cfg.port = 0x188; cfg.provisional = false;
    } else if (m == "fabricated-provisional") {
        cfg.service = 0x2e; cfg.instance = 1; cfg.node = 1; cfg.port = 0x188; cfg.provisional = true; // must refuse
    } else if (m == "presence-missing") {
        f.presenceMissing = true; cfg.service = 0x2e; cfg.instance = 1; cfg.node = 1; cfg.port = 0x188; cfg.provisional = false;
    } else if (m == "cap-fail" || m == "config-fail" || m == "indreg-fail" || m == "ini-fail" ||
               m == "msa-info-fail" || m == "mode-fail") {
        if (m == "cap-fail") f.failReq = 1;
        else if (m == "config-fail") f.failReq = 2;
        else if (m == "indreg-fail") f.failReq = 3;
        else if (m == "ini-fail") f.failReq = 4;
        else if (m == "msa-info-fail") f.failReq = 5;
        else if (m == "mode-fail") f.failReq = 7;
        cfg.service = 0x2e; cfg.instance = 1; cfg.node = 1; cfg.port = 0x188; cfg.provisional = false;
    } else if (m == "retransmit-exhausted") {
        f.transientLeft = 100; cfg.service = 0x2e; cfg.instance = 1; cfg.node = 1; cfg.port = 0x188; cfg.provisional = false;
    } else if (m == "fifo-full") {
        f.fifoFull = true; cfg.service = 0x2e; cfg.instance = 1; cfg.node = 1; cfg.port = 0x188; cfg.provisional = false;
    } else if (m == "kick-fail") {
        f.kickFail = true; cfg.service = 0x2e; cfg.instance = 1; cfg.node = 1; cfg.port = 0x188; cfg.provisional = false;
    } else if (m == "fw-ready-timeout") {
        f.fwTimeout = true; cfg.service = 0x2e; cfg.instance = 1; cfg.node = 1; cfg.port = 0x188; cfg.provisional = false;
    } else if (m == "msa-ready-timeout") {
        f.msaTimeout = true; cfg.service = 0x2e; cfg.instance = 1; cfg.node = 1; cfg.port = 0x188; cfg.provisional = false;
    } else if (m == "stale-indication") {
        f.staleInd = true; cfg.service = 0x2e; cfg.instance = 1; cfg.node = 1; cfg.port = 0x188; cfg.provisional = false;
    } else if (m == "tlv-overrun") {
        f.tlvOverrun = true; cfg.service = 0x2e; cfg.instance = 1; cfg.node = 1; cfg.port = 0x188; cfg.provisional = false;
    } else if (m == "fw-ready-malformed-tlv") {
        f.malformedInd = true; cfg.service = 0x2e; cfg.instance = 1; cfg.node = 1; cfg.port = 0x188; cfg.provisional = false;
    } else if (m == "msa-misaligned") {
        cfg.msaBase = 0x8ba00001ULL; cfg.service = 0x2e; cfg.instance = 1; cfg.node = 1; cfg.port = 0x188; cfg.provisional = false;
    } else if (m == "no-transport") {
        cfg.service = 0x2e; cfg.instance = 1; cfg.node = 1; cfg.port = 0x188; cfg.provisional = false;
    }
}

int main(int argc, char **argv) {
    if (argc > 1) g_mode = argv[1];
    const std::string m(g_mode);

    PicassoWlanWlfwConfig cfg;
    picasso_wlan_wlfw_default_config(&cfg);
    Fake f; std::memset(&f, 0, sizeof f);
    applyMode(f, cfg);
    // Verify defaults are honest before any mode tweaks.
    if (m == "normal") {
        PicassoWlanWlfwConfig d; picasso_wlan_wlfw_default_config(&d);
        CHECK(d.provisional && d.service == 0 && d.instance == 0 && d.node == 0);
        CHECK(d.msaBase == 0x8ba00000ULL && d.msaSize == 0x200000ULL);
        CHECK(d.sid == 0xc0 && d.sidIpa == 0x521);
        uint64_t pages = 0;
        CHECK(picasso_wlan_wlfw_msa_pages(d.msaBase, d.msaSize, &pages) == 0);
        CHECK(pages == 0x200000ULL / 0x1000ULL);
        CHECK(picasso_wlan_wlfw_msa_pages(0x8ba00001ULL, d.msaSize, &pages) == -22);
    }

    PicassoWlanWlfwIo io; std::memset(&io, 0, sizeof io);
    io.context = &f;
    io.discover = discover;
    io.send_request = sendRequest;
    io.wait_indication = waitInd;
    io.load_image = loadImage;
    io.query_presence = presence;
    io.kick = kick;
    io.sleep = sleepFn;
    io.now = nowFn;
    if (m == "no-transport") io.discover = nullptr;

    PicassoWlanWlfwState st;
    const int rc = picasso_wlan_wlfw_execute(&io, &cfg, &st);

    CHECK(!st.hardwareTouched); // invariant: offline skeleton never touches hardware

    if (m == "normal" || m == "restart") {
        CHECK(rc == 0);
        CHECK(st.step == 10 && st.configApplied && st.ready && st.msaReady && st.discovered);
        CHECK(st.requests == 6); // CAP,CONFIG,INDREG,INI,MSA_INFO,MODE (no req 6)
        CHECK(st.indications == 2);
        CHECK(f.nTxn == 6);
        for (unsigned i = 0; i < f.nTxn; ++i) CHECK(f.txnSeen[i] == i + 1); // monotonic txn
        // strict request order
        const unsigned want[6] = {1, 2, 3, 4, 5, 7};
        for (unsigned i = 0; i < 6; ++i) CHECK(f.requests[i] == want[i]);
        CHECK(f.reqArgs[4][0] == 0x8ba00000u); // MSA_MEM_INFO arg0 = msa base
        CHECK(f.reqArgs[4][1] == 0x200000u);   // arg1 = msa size
        CHECK(f.kicks == 6);
        std::printf("PASS wlfw %s: step=%u req=%u ind=%u\n", m.c_str(), st.step, st.requests, st.indications);
        return 0;
    }

    // Failure modes: each must land on the expected phase with a nonzero error.
    CHECK(rc != 0);
    if (m == "discovery-no-answer" || m == "discovery-timeout")
        CHECK(std::string(st.phase) == "discover");
    else if (m == "fabricated-provisional")
        CHECK(st.quarantined && std::string(st.phase) == "discover");
    else if (m == "wrong-service" || m == "wrong-instance" || m == "wrong-node-port")
        CHECK(std::string(st.phase) == "discover");
    else if (m == "presence-missing")
        CHECK(std::string(st.phase) == "discover");
    else if (m == "retransmit-exhausted")
        CHECK(st.retransmits > cfg.retransmitMax);
    else if (m == "kick-fail")
        CHECK(st.quarantined);
    else if (m == "fw-ready-timeout" || m == "fw-ready-malformed-tlv" || m == "stale-indication" || m == "tlv-overrun")
        CHECK(std::string(st.phase) == "fw-ready");
    else if (m == "msa-ready-timeout")
        CHECK(std::string(st.phase) == "msa-ready");
    else if (m == "msa-misaligned")
        CHECK(std::string(st.phase) == "msa-info");
    else if (m == "cap-fail")
        CHECK(std::string(st.phase) == "capability");
    else if (m == "config-fail")
        CHECK(std::string(st.phase) == "config");
    else if (m == "indreg-fail")
        CHECK(std::string(st.phase) == "ind-register");
    else if (m == "ini-fail")
        CHECK(std::string(st.phase) == "ini");
    else if (m == "msa-info-fail")
        CHECK(std::string(st.phase) == "msa-info");
    else if (m == "mode-fail")
        CHECK(std::string(st.phase) == "mode-on");
    else if (m == "fifo-full")
        CHECK(std::string(st.phase) == "capability");
    else if (m == "no-transport")
        CHECK(std::string(st.phase) == "init");
    std::printf("PASS wlfw %s: phase=%s step=%u error=%d quarantine=%u\n",
                m.c_str(), st.phase, st.step, st.error, (unsigned)st.quarantined);
    return 0;
}
