// SPDX-License-Identifier: BSD-2-Clause-Patent
// Picasso WLFW (WLAN firmware load) over QMI -- OFFLINE INJECTED TRANSPORT SKELETON.
//
// This is the step-3 skeleton: it takes the WLFW request sequence already
// defined by drivers/NativeWlan/WlanFirmware.c (wlan_handshake_run) and supplies
// an injected QMI-transaction + QRTR-framing transport for its three callbacks
// (load_image / request / wait_indication). Every hardware boundary is a
// callback; the host harness supplies deterministic stand-ins.
//
// HONESTY CONTRACT (independent review, BRINGUP-step3-review.md):
// - The WLFW QMI service id / instance / version / node / port are NOT known for
//   Picasso/WCN399x from any first-hand source in this tree. The only WLFW QMI
//   wire on this machine is hakios-ref/hakios.wlfw.c, a WCN6750/ath11k stand-in
//   whose service-number #defines live in missing headers (values unobtainable).
//   THEREFORE no service number is hard-coded here: all identifiers are injected
//   via PicassoWlanWlfwConfig and default to 0 with provisional=true. Discovery
//   must resolve them on-device (read-only, separately authorised) before this
//   skeleton can speak to a real firmware. Writing a WCN6750 number here would
//   be fabrication.
// - WLFW is reached over the MODEM subsystem (WlanBoard.h:81-82: the WLAN SMP2P
//   endpoints live under the modem's smp2p block). QRTR discovery therefore
//   matches service/instance/node/port with node expected under modem, and does
//   NOT reuse the ADSP servreg special-case (service 0x42 / instance 0x4a01).
// - No SMC is issued, no MPSS started, no MMIO written, no private blob stored.
//   MSA/SMMU mapping is a PARAMETERISED REQUEST + read-only plan only; the real
//   SMR/S2CR write is a separate on-device action. RMTFS/PD/PDR/DMS are reported
//   as a presence inventory with NOTSUP, never fabricated protocols.
#ifndef PICASSO_WLAN_WLFW_H
#define PICASSO_WLAN_WLFW_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bounded QMI transaction record. TLVs are inspected but never invented: only
// the two indications already defined by step1 (FW_READY / MSA_READY) are used.
enum {
    PicassoWlanWlfwRequests = 7,      // WlanFirmware.h:157-165 enumeration size
    PicassoWlanWlfwIndications = 2,   // FW_READY / MSA_READY
    PicassoWlanWlfwTlvMax = 8,        // bounded TLV inspection per response
    PicassoWlanWlfwDiscoveryAddrMax = 16
};

// A QRTR service-discovery answer. All four identity fields are injected; the
// host supplies provisional placeholders and asserts the modem-node expectation.
struct PicassoWlanWlfwService {
    uint32_t service, instance, version, node, port;
    bool found;
};

// Injected identifiers and board facts. All numeric identifiers default to 0
// and must be resolved on-device; provisional=true travels with them so no
// caller can mistake a placeholder for a measured value.
struct PicassoWlanWlfwConfig {
    uint32_t service;        // WLFW QMI service id  (0 = unresolved)
    uint32_t instance;       // WLFW QMI instance    (0 = unresolved)
    uint32_t version;        // WLFW QMI version     (0 = unresolved)
    uint32_t node;           // QRTR node, expected under modem subsystem
    uint32_t port;           // QRTR port
    bool provisional;        // true until on-device discovery resolves the above
    uint64_t msaBase;        // 0x8ba00000 (WlanBoard.h:76) - board fact, injected
    uint64_t msaSize;        // 0x200000   (WlanBoard.h:77)
    uint64_t iovaBase;       // 0xa0000000 (WlanBoard.h:66)
    uint64_t iovaSize;       // 0x10000000 (WlanBoard.h:67)
    uint16_t sid;            // 0xc0       (WlanBoard.h:51)
    uint16_t sidMask;        // 0x1
    uint16_t sidIpa;         // 0x521 second stream on ipa_smmu_wlan
    unsigned timeoutMs;      // indication wait budget
    unsigned retransmitMax;  // bounded retransmit ceiling
};

// Board facts that are first-hand in this tree and are never provisional.
// Kept as injected constants so the host can assert them without a device.
#define PICASSO_WLAN_WLFW_MSA_BASE  0x8ba00000ULL
#define PICASSO_WLAN_WLFW_MSA_SIZE  0x200000ULL
#define PICASSO_WLAN_WLFW_IOVA_BASE 0xa0000000ULL
#define PICASSO_WLAN_WLFW_IOVA_SIZE 0x10000000ULL
#define PICASSO_WLAN_WLFW_SID       0xc0u
#define PICASSO_WLAN_WLFW_SID_MASK  0x1u
#define PICASSO_WLAN_WLFW_SID_IPA   0x521u

// Presence inventory for the auxiliary services named in bringup step 3.
// These are NAMES only (PicassoServloc.h static DB); no protocol is implemented.
struct PicassoWlanWlfwServicePresence {
    bool rmtfs, tftp, pd, pdr, dms, wlanFw;
};

// State reported after a skeleton run. phase is a stable label; step is the
// WLFW handshake step (WlanFirmware.h:170-181) that was last attempted.
struct PicassoWlanWlfwState {
    const char *phase;
    unsigned step;
    int error;
    unsigned requests;       // successful request transactions
    unsigned indications;    // successful indication waits
    unsigned discoveryPolls;
    unsigned retransmits;
    unsigned tlvSeen;
    bool discovered;
    bool ready;
    bool msaReady;
    bool configApplied;
    bool quarantined;        // uncertain result: no guessed rollback
    bool hardwareTouched;    // must remain false in this skeleton
};

// All hardware/system boundaries are injected. The host harness supplies
// deterministic stand-ins; a future on-device adapter supplies real IO.
struct PicassoWlanWlfwIo {
    void *context;

    // QRTR service discovery: fill one answer. Return 0 on an answer (found may
    // be false = no answer), negative on IO error. Boundary: real QRTR lookup.
    int (*discover)(void *ctx, const struct PicassoWlanWlfwConfig *cfg,
                    struct PicassoWlanWlfwService *out);

    // Send one QMI request. request is the step1 WLFW request enum value; a0/a1
    // are its two injected arguments. txn is the transaction id assigned here.
    // Return 0 on success, -3 on transient (retried up to retransmitMax), other
    // negative on hard IO failure.
    int (*send_request)(void *ctx, unsigned txn, unsigned request,
                        uint64_t a0, uint64_t a1);

    // Kick the transport (e.g. IPCC signal) after a send. Return 0 or negative.
    int (*kick)(void *ctx);

    // Wait for one indication; fill a bounded raw response buffer (QMI wire
    // bytes) with out size. Return 0 on receipt, -2 on timeout, negative IO.
    int (*wait_indication)(void *ctx, unsigned txn, unsigned indication,
                           unsigned timeout_ms, uint8_t *buf, size_t cap, size_t *out);

    // Inject the helper that runs the step1 handshake over the three callbacks
    // is NOT used here: this module drives the request order directly so the
    // transport (discovery + txn) can be asserted. Kept as a callback so a
    // future adapter can delegate to wlan_handshake_run() unchanged.
    int (*load_image)(void *ctx);

    // Optional auxiliary-service presence query (names only). May be null.
    int (*query_presence)(void *ctx, struct PicassoWlanWlfwServicePresence *out);

    void (*sleep)(void *ctx, unsigned ms);
    uint64_t (*now)(void *ctx);
};

// Fill safe defaults: all service identifiers 0, provisional=true, board facts
// from the first-hand constants above, hardwareTouched stays false.
void picasso_wlan_wlfw_default_config(struct PicassoWlanWlfwConfig *cfg);

// Execute the offline WLFW transport skeleton. Returns 0 on DONE, negative on
// the first failure (recorded in state->error). Never touches hardware: every
// boundary goes through io. Discovery must resolve identifiers; if cfg remains
// provisional with unresolved (zero) identifiers the run fails at discovery
// with a clearly labelled error rather than proceeding with fabricated values.
int picasso_wlan_wlfw_execute(const struct PicassoWlanWlfwIo *io,
                              const struct PicassoWlanWlfwConfig *cfg,
                              struct PicassoWlanWlfwState *state);

// Page-arithmetic helper for the MSA/SMMU mapping PLAN (no MMIO). Returns 0 and
// fills pages, or -22 if base/size are not page aligned / size is zero / pages
// overflow. Mirrors WlanBoard.cpp:wlan_msa_pages in pure injected form.
int picasso_wlan_wlfw_msa_pages(uint64_t base, uint64_t size, uint64_t *pages);

#ifdef __cplusplus
}
#endif
#endif
