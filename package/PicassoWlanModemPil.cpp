// SPDX-License-Identifier: BSD-2-Clause-Patent
// Step-2 offline skeleton of the Picasso pil_modem secure-load transaction.
// Order is the ADSP boot order with injected boundaries. Host script lifts the
// function bodies (minus #include lines) into a stub harness; no live SMC/MMIO
// is executed here. See PicassoWlanModemPil.h for the explicit non-goals.
#include "PicassoWlanModemPil.h"

// Board-identified constants. These exact literals are asserted by the host
// check so a future edit cannot silently revert PAS 4 or the modem carveout to
// the ADSP subsystem's PAS id and carveout (different substrate).
void picasso_wlan_modem_pil_default_config(struct WlanModemPilConfig *cfg) {
    if (!cfg) return;
    cfg->carveoutBase = 0x8c000000ULL;
    cfg->carveoutBytes = 0xf800000ULL;
    cfg->metadataBase = 0x8bc18000ULL;
    cfg->metadataBytes = 0x3000ULL;
    cfg->pasId = 4;                 // pil_modem qcom,pas-id = <4>, NOT ADSP 1.
    cfg->headers = 35;              // modem.mdt e_phnum.
    cfg->mdtBytes = 9420;           // modem.mdt size; metadata SLOT is 0x3000 (copy target).
    cfg->partCount = 34;            // PH0..PH33 backed by disk; PH18/PH25 absent; PH34 BSS-only.
    cfg->timeoutMs = 10000;         // DTB qcom,proxy-timeout-ms = 0x2710.
    cfg->readyBit = 2;              // PROVISIONAL: caller overrides; see design R4.
    cfg->handoverBit = 4;
    cfg->stopAckBit = 8;
    cfg->rails = 2;                 // vdd_cx + vdd_mss, NOT the 5 WCSS rails.
}

namespace {

struct Run {
    const struct WlanModemPilIo *io;
    const struct WlanModemPilConfig *cfg;
    struct WlanModemPilState *st;
    uint8_t *metadata;       // caller-owned buffer (cfg->metadataBuffer), never on stack
    size_t metadataBytes;
    uint8_t sha[32];
};

static int fail(Run *r, unsigned step, const char *phase, int code) {
    r->st->step = step;
    r->st->phase = phase;
    if (!r->st->error) r->st->error = code;
    return code;
}

// Step A: peer preflight. The ADSP flow requires the first poll to be "absent"
// (-2): a stale ready bit is NOT evidence that this image was authenticated.
static int peerAbsent(Run *r, const char *phase, unsigned step) {
    struct WlanModemPeerEvent e = {0, 0, 0, 0, 0};
    int error = r->io->poll_peer(r->io->context, &e);
    ++r->st->peerPolls;
    if (error != -2) {
        if (e.restart || (e.hasValue && (e.value & 1))) return fail(r, step, phase, -5);
        return fail(r, step, phase, error ? error : -16);
    }
    return 0;
}

// Step B: reopen + revalidate every part into our own metadata buffer. The
// sha256 callback authenticates identity against the committed pin profile via
// the caller; this skeleton only enforces that a digest was actually computed.
static int loadMetadata(Run *r) {
    int error;
    size_t out = 0;
    // index 0 is modem.mdt (metadata). Capacity bounds the read. The buffer is
    // caller-owned (heap on device, static in the host harness); Run carries
    // only the pointer so the 0x3000-byte array never consumes kernel stack.
    if (!r->metadata || !r->cfg->metadataBytes) return fail(r, 32, "metadata-cap", -22);
    error = r->io->read_part(r->io->context, 0, r->metadata, r->cfg->metadataBytes, &out);
    if (error) return fail(r, 32, "metadata-read", error);
    if (out != r->cfg->mdtBytes) return fail(r, 32, "metadata-size", -22);
    r->metadataBytes = out;
    error = r->io->sha256(r->io->context, r->metadata, out, r->sha);
    if (error) return fail(r, 32, "metadata-hash", error);
    return 0;
}

// Step C: load each non-empty PT_LOAD part, re-hashing while copying. The
// destination is an injected callback in hardware builds and a no-op in the
// host harness; what matters here is order, count and failure propagation.
static int loadSegments(Run *r) {
    uint8_t chunk[4096];
    for (unsigned i = 1; i <= r->cfg->partCount; ++i) {
        size_t out = 0;
        int error = r->io->read_part(r->io->context, i, chunk, sizeof chunk, &out);
        if (error == -2) continue;              // filesz==0 part, intentionally absent.
        if (error) return fail(r, 33, "segment-read", error);
        error = r->io->sha256(r->io->context, chunk, out, r->sha);
        if (error) return fail(r, 33, "segment-hash", error);
        ++r->st->partsLoaded;
    }
    return 0;
}

// Ordered shutdown: reassert supplies, PAS RESET (cmd 6), AOP off, release.
// An uncertain result quarantines: no guessed low-power rollback, no unmap.
static void stop(Run *r, int error) {
    struct WlanModemPilState *s = r->st;
    if (error == -115) s->quarantined = true;
    s->step = 90;
    s->phase = "stop";
    if (s->pasCalls && !s->quarantined) {
        if (!s->leased) s->quarantined = true;   // never issue PAS without the lease.
        for (unsigned i = 0; i < r->cfg->rails && !s->quarantined; ++i) {
            if (r->io->rpmh_write(r->io->context, i, 0, 0)) { s->quarantined = true; break; }
        }
        if (!s->quarantined) {
            ++s->pasCalls;
            if (r->io->pas(r->io->context, 6, 0, 0, r->cfg->pasId)) s->quarantined = true;
        }
    }
    if (!s->quarantined && s->aop) {
        if (r->io->aop(r->io->context, false)) s->quarantined = true;
    }
    if (s->leased) {
        r->io->lease_release(r->io->context, s->quarantined);
        s->leased = false;
        s->released = true;
    }
    s->error = error;
    s->phase = "stopped";
}

}  // namespace

int picasso_wlan_modem_pil_execute(const struct WlanModemPilIo *io,
    const struct WlanModemPilConfig *cfg, struct WlanModemPilState *state) {
    if (!state) return -22;
    {
        struct WlanModemPilState empty{};
        *state = empty;
    }
    state->phase = "owner";
    if (!io || !cfg || !io->read_part || !io->sha256 || !io->rpmh_write ||
        !io->lease_acquire || !io->lease_release ||
        !io->aop || !io->pas || !io->poll_peer || !io->sleep || !io->now) {
        state->step = 0; state->phase = "owner"; state->error = -22; return -22;
    }
    if (cfg->pasId != 4 || cfg->carveoutBase != 0x8c000000ULL ||
        cfg->carveoutBytes != 0xf800000ULL || cfg->metadataBase != 0x8bc18000ULL ||
        !cfg->rails || cfg->rails > 8) {
        state->step = 0; state->phase = "owner-config"; state->error = -22; return -22;
    }
    // Metadata staging must come from the caller (heap on device). Reject a
    // missing buffer up front; loadMetadata enforces the size bound.
    if (!cfg->metadataBuffer || !cfg->metadataBytes) {
        state->step = 0; state->phase = "owner-buffer"; state->error = -22; return -22;
    }

    Run r{};
    r.io = io; r.cfg = cfg; r.st = state; r.metadata = cfg->metadataBuffer;
    int error = 0;
    uint64_t deadline = 0;   // declared before any goto target.

    state->phase = "peer-preflight";
    if ((error = peerAbsent(&r, "peer-preflight", 11))) goto out;

    state->phase = "firmware-reopen";
    if ((error = loadMetadata(&r))) goto out;

    state->phase = "proxy-vote";
    for (unsigned i = 0; i < cfg->rails; ++i) {
        ++state->railVotes;   // a failed command may already have reached the resource.
        if ((error = io->rpmh_write(io->context, i, 0, 0))) { state->quarantined = true; goto out; }
    }

    state->phase = "aop-on";
    if ((error = io->aop(io->context, true))) goto out;
    state->aop = true;

    // Shared PAS lease: acquire before any SCM call. Reuses the single gate.
    state->phase = "lease";
    if ((error = io->lease_acquire(io->context))) { state->quarantined = error == -115; goto out; }
    state->leased = true;

    state->phase = "pas-init";
    ++state->pasCalls;
    if ((error = io->pas(io->context, 1, cfg->metadataBase, 0, cfg->pasId))) goto out;

    state->phase = "pas-memory";
    ++state->pasCalls;
    if ((error = io->pas(io->context, 2, cfg->carveoutBase, cfg->carveoutBytes, cfg->pasId))) goto out;

    state->phase = "segments";
    if ((error = loadSegments(&r))) goto out;

    state->phase = "peer-before-auth";
    if ((error = peerAbsent(&r, "peer-before-auth", 34))) goto out;

    state->phase = "pas-auth";
    ++state->pasCalls;
    if ((error = io->pas(io->context, 5, 0, 0, cfg->pasId))) goto out;
    state->authenticated = true;

    // Stage-1 readiness = PAS auth succeeded and the peer is alive. The WLAN
    // SMP2P `wlan` bit semantics are NOT resolved in reviewed sources (design
    // R4), so we deliberately do NOT gate readiness on readyBit/handoverBit.
    // cmd5 returning 0 above set authenticated=true; here we only require a
    // bounded liveness poll: peer responds without restart/stale-ready. A
    // "device reset" is an on-device console observation, not a code variable.
    state->phase = "ready";
    deadline = io->now(io->context) + cfg->timeoutMs;
    do {
        struct WlanModemPeerEvent e = {0, 0, 0, 0, 0};
        error = io->poll_peer(io->context, &e);
        ++state->peerPolls;
        if (error == 0 && !e.restart && !(e.hasValue && (e.value & 1))) {
            // Peer is alive and not announcing stale/reset state. Auth already
            // succeeded; bit-level readiness is intentionally not required.
            state->ready = true;
            if (e.hasValue && (e.value & cfg->handoverBit)) state->handed = true;
            break;
        }
        io->sleep(io->context, 10);
    } while (io->now(io->context) < deadline);
    if (!state->ready) { error = -60; goto out; }

    // The PAS lease covers the whole session through RESET; stop() releases it.
    // Do not drop it at ready: an ordered shutdown still needs the shared gate.

out:
    stop(&r, error);
    return error;
}
