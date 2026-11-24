// SPDX-License-Identifier: BSD-2-Clause-Patent
// Shared PIL session/metadata owner. An interrupted call owns its x6 session
// until it finishes; timeout quarantines PIL instead of starting another call.
#ifndef PICASSO_PAS_H
#define PICASSO_PAS_H
#include <stdint.h>
struct PicassoPasReply { uint32_t a0, a1, a2, a3; uint64_t a6; };
struct PicassoPasIo {
    void *context;
    PicassoPasReply (*call)(void *, uint32_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, bool);
    void (*delay)(void *, unsigned);
};
// Address-validation callback for picasso_pas_execute_ex. Return 0 when the
// (command, address, bytes) triple is acceptable for the calling subsystem,
// -22 otherwise. Keeps each subsystem's carveout policy out of the shared SMC
// sequencer.
typedef int (*PicassoPasValidate)(unsigned command, uint64_t address, uint64_t bytes);

// Shared SMC sequencer. Identical command/arginfo/function encoding, interrupt
// resume token and quarantine semantics as the original picasso_pas_execute;
// the only differences are that the PAS id (x2) is a parameter and the address
// policy is supplied by `validate`. Both the first call and the interrupt-resume
// call pass the SAME pasId -- a mismatch there would hand the resumed session to
// the wrong subsystem. The resume token still comes from reply->a6 (x6), which
// is orthogonal to x2.
static inline int picasso_pas_execute_ex(const PicassoPasIo *io, unsigned command,
    uint64_t address, uint64_t bytes, uint32_t pasId, PicassoPasValidate validate,
    PicassoPasReply *reply) {
    if (!io || !io->call || !io->delay || !reply || !validate) return -22;
    if (command != 1 && command != 2 && command != 5 && command != 6) return -22;
    if (validate(command, address, bytes)) return -22;
    const uint32_t arginfo = command == 1 ? 0x82 : command == 2 ? 3 : 1;
    const uint32_t function = 0x42000200 | command;
    unsigned interrupted = 0;
    for (unsigned busy = 0; busy < 68; ++busy) {
        *reply = io->call(io->context, function, arginfo, pasId, address, bytes, 0, true);
        while (reply->a0 == 1) {
            if (++interrupted > 64) return -115; // Unknown completion; never cancel by overwriting metadata.
            io->delay(io->context, 1);
            *reply = io->call(io->context, 1, arginfo, pasId, address, bytes, reply->a6, false);
        }
        if (reply->a0 != uint32_t(-12)) return reply->a0 ? -5 : reply->a1 ? -13 : 0;
        io->delay(io->context, 30);
    }
    return -16;
}

// ADSP address policy -- byte-for-byte equivalent to the original :17-19 check.
static inline int picasso_pas_adsp_validate(unsigned command, uint64_t address, uint64_t bytes) {
    if ((command == 1 && (address != 0x8bc18000 || bytes)) ||
        (command == 2 && (address != 0x89200000 || bytes != 0x02800000)) ||
        (command >= 5 && (address || bytes))) return -22;
    return 0;
}

// Backward-compatible wrapper: same signature and behaviour as before. ADSP is
// PAS id 1 with the shared metadata slot and the ADSP carveout.
static inline int picasso_pas_execute(const PicassoPasIo *io, unsigned command,
    uint64_t address, uint64_t bytes, PicassoPasReply *reply) {
    return picasso_pas_execute_ex(io, command, address, bytes, 1,
        picasso_pas_adsp_validate, reply);
}
extern "C" int picasso_pas_acquire(void);
extern "C" void picasso_pas_release(bool uncertain);
extern "C" int picasso_pas_adsp(unsigned command, uint64_t address, uint64_t bytes);
#endif
