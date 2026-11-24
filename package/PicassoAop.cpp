// SPDX-License-Identifier: BSD-2-Clause-Patent
// Picasso/lito AOP: 4 KiB message RAM, IPCC client 0 / signal 0.
// Run only on the firmware worker. Poll shared state without claiming the
// shared IPCC receive IRQ or touching another subsystem's pending signals.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/clock.h>
#include "PicassoQmp.h"
#include "PicassoAdspBoot.h"

namespace {
struct Aop { uintptr_t ram, ipcc; };
uint32_t aopRead(void *ctx, uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t *>(static_cast<Aop *>(ctx)->ram + offset);
}
void aopWrite(void *ctx, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(static_cast<Aop *>(ctx)->ram + offset) = value;
}
void aopBarrier(void *) {
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}
void aopKick(void *ctx) {
    *reinterpret_cast<volatile uint32_t *>(static_cast<Aop *>(ctx)->ipcc + 0x0c) = 0;
    aopBarrier(ctx);
}
IOMemoryMap *aopMap(uint64_t base) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, 0x1000);
    if (!memory) return nullptr;
    IOMemoryMap *map = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    return map;
}
uint64_t deadline() {
    uint64_t value; clock_interval_to_deadline(5, kSecondScale, &value); return value;
}
int wait(PicassoQmp *q) {
    for (;;) {
        int state = picasso_qmp_step(q, mach_absolute_time());
        if (state <= QmpClosed || state == QmpOpen) return state;
        IOSleep(1);
    }
}
}

extern "C" int picasso_aop_probe(void) {
    // One attempt per boot. A timeout cannot cause a second owner to take over.
    static unsigned attempted;
    if (__atomic_exchange_n(&attempted, 1U, __ATOMIC_ACQ_REL)) return QmpBusy;
    OSSharedPtr<IOMemoryMap> ram(aopMap(0x0c300000), OSNoRetain);
    OSSharedPtr<IOMemoryMap> ipcc(aopMap(0x00408000), OSNoRetain);
    Aop context = {ram ? static_cast<uintptr_t>(ram->getVirtualAddress()) : 0,
                   ipcc ? static_cast<uintptr_t>(ipcc->getVirtualAddress()) : 0};
    if (!context.ram || !context.ipcc) {
        IOLog("PICASSO_ADSP aop stage=map error=-12 authenticated=0\n"); return -12;
    }
    QmpIo io = {&context, aopRead, aopWrite, aopBarrier, aopKick};
    PicassoQmp q = {};
    int result = picasso_qmp_begin(&q, &io, 0x1000, deadline());
    if (result >= 0) result = wait(&q);
    const uint32_t *d = q.descriptor;
    IOLog("PICASSO_ADSP aop stage=handshake state=%d open=%u magic=%x version=%x features=%x rx=%x/%x tx=%x/%x remote=%x/%x/%x/%x local=%x/%x/%x/%x authenticated=0\n",
        result, result == QmpOpen, d[0], d[1], d[2], d[8], d[7], d[14], d[13],
        d[3], d[4], d[5], d[6], d[9], d[10], d[11], d[12]);
    int closed = picasso_qmp_close(&q, deadline());
    if (closed == QmpClosing) closed = wait(&q);
    IOLog("PICASSO_ADSP aop stage=close state=%d released=%u load_state_sent=0 pending=rails-smp2p-pas pcm=0\n",
        closed, !q.owned);
    // Mapping lifetime ends here. No load_state on/off is sent before a real
    // PAS lifecycle owns LCX/LMX/XO and has installed ready/fatal handlers.
    return result == QmpOpen && closed == QmpClosed ? 0 : (result < 0 ? result : closed);
}

namespace {
// One physical AOP mailbox / one QMP session, shared by multiple subsystems
// (adsp and modem) via the image's `name:` field. Per-image load_state flags;
// session state (q/context/mappings/uncertain) stays shared. Do NOT open a
// second QMP session: this implementation is single-channel and the ADSP
// session is held open for boot.
enum { AopImageAdsp = 0, AopImageModem = 1, AopImageCount = 2 };
static const char *const aopImageNames[AopImageCount] = {"adsp", "mss"};
// Static message literals: zero-padded at compile time (no stack garbage in
// the QMP descriptor) and no snprintf dependency in the -ffreestanding ARM
// build. Index [image][on?0:1].
static const char *const aopLoadMessages[AopImageCount][2] = {
    {"{class: image, res: load_state, name: adsp, val: on}",
     "{class: image, res: load_state, name: adsp, val: off}"},
    {"{class: image, res: load_state, name: mss, val: on}",
     "{class: image, res: load_state, name: mss, val: off}"},
};
struct ImageLoad { bool attempted, on; };
struct LoadState {
    IOMemoryMap *ram, *ipcc;
    Aop context;
    PicassoQmp q;
    bool uncertain;
    ImageLoad image[AopImageCount];
};
static LoadState load;
// Independent serialization gate for the shared QMP session. MUST NOT reuse
// the PAS gate: AOP load_state is called while the PAS lease is held
// (PicassoAdspBoot.cpp:99->100 and :165-167->177-178), so acquiring pasGate
// here would self-lock a non-recursive gate and hang the firmware worker.
static unsigned aopGate, aopPoisoned;
int aopGateAcquire() {
    // Bounded wait; never spin forever. -16 = contention, -115 = poisoned.
    for (unsigned i = 0; i < 30000; ++i) {
        if (__atomic_load_n(&aopPoisoned, __ATOMIC_ACQUIRE)) return -115;
        unsigned expected = 0;
        if (__atomic_compare_exchange_n(&aopGate, &expected, 1, false,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
        IOSleep(1);
    }
    return -16;
}
void aopGateRelease(bool uncertain) {
    if (uncertain) __atomic_store_n(&aopPoisoned, 1U, __ATOMIC_RELEASE);
    __atomic_store_n(&aopGate, 0U, __ATOMIC_RELEASE);
}
void releaseLoad() {
    if(load.ipcc)load.ipcc->release();
    if(load.ram)load.ram->release();
    load.ipcc=load.ram=nullptr;
}
}
namespace {
// Internal per-image load_state transaction on the SHARED session. Holds the
// independent aopGate across the whole transaction. `on` for adsp keeps the
// exact prior QMP sequence; the modem entry uses name "mss" (validated on
// device, NOT a proven constant).
int load_state_indexed(unsigned idx, bool on) {
    if (idx >= AopImageCount) return -22;
    int error = aopGateAcquire();
    if (error) return error;
    const char *name = aopImageNames[idx];
    ImageLoad &img = load.image[idx];
    int result = 0;
    if (load.uncertain) { result = -115; goto out; }
    if (on) {
        // A repeated on for the same image is a caller error (matches the
        // pre-PA-1 singleton: load.attempted -> -16). Reject before sending a
        // second message on the shared channel.
        if (img.attempted) { result = -16; goto out; }
        // The session is opened once and shared; a second image only sends its
        // own load_state message on the already-open channel.
        {
            img.attempted = true;
            if (!load.q.owned) {
                load.ram=aopMap(0x0c300000);load.ipcc=aopMap(0x00408000);
                load.context={load.ram?uintptr_t(load.ram->getVirtualAddress()):0,
                              load.ipcc?uintptr_t(load.ipcc->getVirtualAddress()):0};
                if(!load.context.ram || !load.context.ipcc){releaseLoad();result=-12;goto out;}
                QmpIo io={&load.context,aopRead,aopWrite,aopBarrier,aopKick};
                int state=picasso_qmp_begin(&load.q,&io,0x1000,deadline());
                if(state>=0)state=wait(&load.q);
                if(state!=QmpOpen){
                    int closed=picasso_qmp_close(&load.q,deadline());
                    if(closed==QmpClosing)closed=wait(&load.q);
                    load.uncertain=load.q.owned;
                    if(!load.uncertain)releaseLoad();
                    result=load.uncertain?-115:state<0?state:-5;
                    goto out;
                }
            }
        }
    } else if (!img.on) { result = -22; goto out; }
    {
        // Static literal is zero-padded by the compiler, so no stack garbage
        // reaches the mailbox; no snprintf in the freestanding ARM build.
        const char *message = aopLoadMessages[idx][on ? 0 : 1];
        int state = picasso_qmp_send(&load.q, message, deadline());
        if (state == QmpSending) state = wait(&load.q);
        if (state != QmpOpen) load.uncertain = true;
        else img.on = on;
        // Preserve the historical ADSP log prefix; modem uses its own prefix
        // so device logs stay comparable with prior boots.
        const char *prefix = (idx == AopImageAdsp) ? "PICASSO_ADSP" : "PICASSO_WLAN";
        IOLog("%s aop stage=load_state image=%s on=%u state=%d consumed=%u uncertain=%u\n",
            prefix, name, on, state, state == QmpOpen, load.uncertain);
        result = state == QmpOpen ? 0 : -115;
    }
out:
    // Poison the shared gate only on genuine uncertainty (the session state
    // is unknown). Ordinary rejected calls (-22 arg, -16 repeat, -12 map,
    // -5/-7 setup) are well-defined failures and must not wedge the gate.
    aopGateRelease(load.uncertain);
    return result;
}
}  // namespace

extern "C" int picasso_aop_load_state(bool on) {
    return load_state_indexed(AopImageAdsp, on);  // unchanged adsp entry
}
extern "C" int picasso_aop_load_state_modem(bool on) {
    return load_state_indexed(AopImageModem, on);
}
extern "C" int picasso_aop_finish(void) {
    int error = aopGateAcquire();
    if (error) return error;
    int result = 0;
    if (load.uncertain) { result = -115; goto out; }
    // Session close only after EVERY subsystem has turned its image off.
    // But note: normal ADSP boot holds its image on for the whole boot, so
    // finish() is only reached on an ADSP error-stop path; at runtime the
    // session is intentionally kept open until reboot (mappings released by
    // reboot), matching the prior behaviour.
    for (unsigned i = 0; i < AopImageCount; ++i)
        if (load.image[i].on) { result = -115; goto out; }
    if (!load.ram) { result = 0; goto out; }
    {
        int state=picasso_qmp_close(&load.q,deadline());
        if(state==QmpClosing)state=wait(&load.q);
        load.uncertain=load.q.owned;
        if(!load.uncertain)releaseLoad();
        result = state==QmpClosed?0:state;
    }
out:
    aopGateRelease(load.uncertain || (result && result != -115));
    return result;
}
