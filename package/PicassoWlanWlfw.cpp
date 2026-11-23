// SPDX-License-Identifier: BSD-2-Clause-Patent
// Picasso WLFW over QMI -- offline injected transport skeleton (see header contract).
#include "PicassoWlanWlfw.h"

// Request / indication values are the step1 WLFW enums (WlanFirmware.h:157-169).
// They are protocol message NUMBERS from the reconstructed stock sequence, NOT
// QMI service identifiers; using them does not fabricate a Picasso service id.
enum {
    W_REQ_CAPABILITY = 1, W_REQ_CONFIG = 2, W_REQ_IND_REGISTER = 3,
    W_REQ_INI = 4, W_REQ_MSA_MEM_INFO = 5, W_REQ_MODE = 7,
};
enum { W_IND_FW_READY = 1, W_IND_MSA_READY = 2 };

// Stable phase labels for diagnostics. Never imply hardware was touched.
static const char *PH_INIT = "init", *PH_DISCOVER = "discover", *PH_CAP = "capability",
                  *PH_CONFIG = "config", *PH_INDREG = "ind-register", *PH_INI = "ini",
                  *PH_FWREADY = "fw-ready", *PH_MSA = "msa-info", *PH_MSAREADY = "msa-ready",
                  *PH_MODE = "mode-on", *PH_DONE = "done";

void picasso_wlan_wlfw_default_config(PicassoWlanWlfwConfig *cfg) {
    if (!cfg) return;
    cfg->service = 0; cfg->instance = 0; cfg->version = 0;
    cfg->node = 0; cfg->port = 0; cfg->provisional = true;
    cfg->msaBase = PICASSO_WLAN_WLFW_MSA_BASE;
    cfg->msaSize = PICASSO_WLAN_WLFW_MSA_SIZE;
    cfg->iovaBase = PICASSO_WLAN_WLFW_IOVA_BASE;
    cfg->iovaSize = PICASSO_WLAN_WLFW_IOVA_SIZE;
    cfg->sid = PICASSO_WLAN_WLFW_SID;
    cfg->sidMask = PICASSO_WLAN_WLFW_SID_MASK;
    cfg->sidIpa = PICASSO_WLAN_WLFW_SID_IPA;
    cfg->timeoutMs = 50; cfg->retransmitMax = 2;
}

int picasso_wlan_wlfw_msa_pages(uint64_t base, uint64_t size, uint64_t *pages) {
    if (!pages) return -22;
    if (!size || (base & 0xfffu) || (size & 0xfffu)) return -22;
    const uint64_t n = size / 0x1000u;
    if (!n || n > (0xffffffffu / 0x1000u)) return -22; // bounded page count
    *pages = n;
    return 0;
}

// Bounded QMI wire inspection. The QMI indication header is 7 bytes:
//   [0]=0x04 (indication), [1..2]=txn LE, [3..4]=message id LE, [5..6]=length LE.
// Then TLVs: [type u8][len u16 LE][value]. We only VERIFY structure and bound
// counts; we never invent a service/message number. Returns 0 if well-formed
// and the txn matches, -5 on malformed/stale, and fills tlvSeen.
static int inspect_indication(const uint8_t *buf, size_t size, unsigned txn,
                              unsigned *tlvSeen, bool *stale) {
    *stale = false;
    if (size < 7) return -5;
    if (buf[0] != 0x04) return -5;
    const unsigned rtxn = (unsigned)buf[1] | ((unsigned)buf[2] << 8);
    if (rtxn != txn) { *stale = true; return -5; }
    const unsigned body = (unsigned)buf[5] | ((unsigned)buf[6] << 8);
    if (body != size - 7) return -5;
    unsigned at = 7, count = 0;
    while (at < size) {
        if (size - at < 3) return -5;
        const unsigned len = (unsigned)buf[at + 1] | ((unsigned)buf[at + 2] << 8);
        at += 3;
        if (len > size - at) return -5;   // TLV overrun -> reject, never read past
        at += len;
        if (++count > PicassoWlanWlfwTlvMax) return -5;
    }
    *tlvSeen = count;
    return 0;
}

// Fail helper: quarantine on uncertain results, never guess a rollback.
static int fail(PicassoWlanWlfwState *s, const char *phase, unsigned step, int error,
                bool uncertain) {
    s->phase = phase; s->step = step; s->error = error ? error : -5;
    if (uncertain) s->quarantined = true;
    return s->error;
}

int picasso_wlan_wlfw_execute(const struct PicassoWlanWlfwIo *io,
                              const struct PicassoWlanWlfwConfig *cfg,
                              struct PicassoWlanWlfwState *state) {
    if (!io || !cfg || !state) return -22;
    *state = PicassoWlanWlfwState{};
    state->phase = PH_INIT;
    state->hardwareTouched = false; // invariant for the whole offline skeleton

    if (!io->discover || !io->send_request || !io->wait_indication || !io->load_image)
        return fail(state, PH_INIT, 0, -2, false);

    // Step 0: discovery. Refuse to proceed on fabricated (zero, provisional)
    // identifiers -- this is the fabrication guard the review required.
    state->phase = PH_DISCOVER;
    struct PicassoWlanWlfwService svc = {};
    const int drc = io->discover(io->context, cfg, &svc);
    if (drc == -2) return fail(state, PH_DISCOVER, 0, -60, false); // no answer/timeout
    if (drc) return fail(state, PH_DISCOVER, 0, drc, false);
    ++state->discoveryPolls;
    if (!svc.found) return fail(state, PH_DISCOVER, 0, -60, false);
    // Four-dimensional identity match: service/instance/node/port. This is the
    // modem-subsystem GLINK/QMI binding; do NOT reuse ADSP servreg 0x42/0x4a01.
    if (svc.service != cfg->service || svc.instance != cfg->instance ||
        svc.node != cfg->node || svc.port != cfg->port)
        return fail(state, PH_DISCOVER, 0, -5, false);
    const bool unresolved = !cfg->service || !cfg->instance || !cfg->node || !cfg->port;
    if (cfg->provisional || unresolved)
        return fail(state, PH_DISCOVER, 0, -115, true); // unresolved -> quarantine
    state->discovered = true;

    // Optional auxiliary-service presence inventory (names only, no protocol).
    if (io->query_presence) {
        struct PicassoWlanWlfwServicePresence pres = {};
        const int prc = io->query_presence(io->context, &pres);
        if (prc) return fail(state, PH_DISCOVER, 0, prc, false);
        // wlan/fw is the anchoring service name (PicassoServloc.h:27). The rest
        // are reported but never implemented here.
        if (!pres.wlanFw) return fail(state, PH_DISCOVER, 0, -5, false);
    }

    // load_image boundary (step1 LOAD). In this skeleton the harness stands in
    // for the PIL/signing path; no image bytes are embedded.
    const int lrc = io->load_image(io->context);
    if (lrc) return fail(state, PH_INIT, 1, lrc, false);

    unsigned txn = 0;
    uint8_t buf[128];
    bool stale = false;

    // send one request with bounded retransmit; kick after each send.
    auto send = [&](const char *phase, unsigned step, unsigned req,
                    uint64_t a0, uint64_t a1) -> int {
        state->phase = phase; state->step = step;
        for (unsigned attempt = 0; attempt <= cfg->retransmitMax; ++attempt) {
            const int rc = io->send_request(io->context, ++txn, req, a0, a1);
            if (rc == -3) { ++state->retransmits; continue; } // transient -> retry
            if (rc) return fail(state, phase, step, rc, rc < 0 && rc != -5);
            if (io->kick) {
                const int krc = io->kick(io->context);
                if (krc) return fail(state, phase, step, krc, true); // uncertain TX
            }
            ++state->requests;
            return 0;
        }
        return fail(state, phase, step, -60, false); // retransmit exhausted
    };

    // wait for one indication, with bounded TLV inspection.
    auto wait = [&](const char *phase, unsigned step, unsigned ind) -> int {
        state->phase = phase; state->step = step;
        size_t out = 0;
        const int rc = io->wait_indication(io->context, txn, ind, cfg->timeoutMs,
                                           buf, sizeof buf, &out);
        if (rc == -2) return fail(state, phase, step, -60, false); // timeout
        if (rc) return fail(state, phase, step, rc, false);
        unsigned seen = 0;
        const int irc = inspect_indication(buf, out, txn, &seen, &stale);
        if (irc) return fail(state, phase, step, stale ? -5 : irc, stale);
        state->tlvSeen += seen;
        ++state->indications;
        return 0;
    };

    // Strict step1 order: CAPABILITY -> CONFIG -> IND_REGISTER -> INI ->
    // FW_READY -> MSA_MEM_INFO -> MSA_READY -> MODE_ON -> DONE.
    // request 6 (WLAN_WLFW_MSA_READY) is an enum item, NOT a handshake request.
    if (send(PH_CAP, 2, W_REQ_CAPABILITY, 0, 0)) return state->error;
    if (send(PH_CONFIG, 3, W_REQ_CONFIG, 0, 0)) return state->error;
    if (send(PH_INDREG, 4, W_REQ_IND_REGISTER, 0, 0)) return state->error;
    if (send(PH_INI, 5, W_REQ_INI, 0, 0)) return state->error;
    if (wait(PH_FWREADY, 6, W_IND_FW_READY)) return state->error;
    state->ready = true;

    // MSA/SMMU mapping PLAN: page arithmetic is validated in the host skeleton;
    // no SMR/S2CR write occurs (that is a separate on-device action).
    uint64_t pages = 0;
    const int prc = picasso_wlan_wlfw_msa_pages(cfg->msaBase, cfg->msaSize, &pages);
    if (prc) return fail(state, PH_MSA, 7, prc, false);
    if (send(PH_MSA, 7, W_REQ_MSA_MEM_INFO, cfg->msaBase, cfg->msaSize)) return state->error;
    if (wait(PH_MSAREADY, 8, W_IND_MSA_READY)) return state->error;
    state->msaReady = true;

    if (send(PH_MODE, 9, W_REQ_MODE, 1, 0)) return state->error;
    state->configApplied = true;
    state->phase = PH_DONE;
    state->step = 10;
    return 0;
}
