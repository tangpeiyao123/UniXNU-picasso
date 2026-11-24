// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real-device stage-1 adapter: the 10 injected WlanModemPilIo callbacks of
// PicassoWlanModemPil.cpp bound to the Picasso hardware boundaries.
//
// This file is the ONLY new caller of the modem secure-load state machine.
// It reuses, and never modifies, the shared GPU/ADSP owners:
//   picasso_pas_acquire/release, picasso_rpmh_active_write, scmSipCall.
// It does not duplicate the SMC inline assembly: the single SMC encoding lives
// in Adreno620Cp.cpp:scmSipCall and is reached through the extern "C" wrappers
// picasso_pas_device_call / picasso_pas_device_delay declared in PicassoPas.h.
//
// FIRMWARE READ BOUNDARY: include FirmwareWindow.h (declarations only), NEVER
// FirmwareRead.h (its native_ufs_modem_open/read bodies are strong definitions
// that would collide with libNativeUfs.a / NativeUfs.cpp at link time).
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/clock.h>
#include <libkern/crypto/sha2.h>
#include <string.h>

#include "PicassoWlanStage1Adapter.h"
#include "PicassoWlanModemPil.h"
#include "PicassoFat.h"
#include "FirmwareWindow.h"
#include "WlanSmem.h"
#include "PicassoPas.h"
#include "PicassoRpmh.h"

extern "C" {
// Existing production entry points with no committed header declaration.
int native_ufs_store_ready(void);                       // NativeUfs.cpp
int picasso_aop_load_state_modem(bool on);              // PicassoAop.cpp:197-198
int picasso_pas4_supported_probe(uint32_t *outA1);      // Adreno620Cp.cpp (read-only)
}

namespace {

// ---- UFS/FAT read context -------------------------------------------------
// native_ufs_modem_open() requires at least 16 KiB of caller scratch; the
// bounce buffer must not overlap the destination (FirmwareRead.h:77-78).
struct FirmwareCtx {
    UfsFirmwareWindow window;
    FatVolume volume;
    FatFile file;
    uint8_t scratch[16384];
    uint8_t bounce[4096];
    bool fatMounted;
    uint64_t storeDeadline;   // absolute ms, bounded store-ready wait
};

// Read function supplied to picasso_fat_mount: chunked, bounded UFS reads.
int readFirmware(void *context, uint64_t at, void *to, size_t length) {
    auto *c = static_cast<FirmwareCtx *>(context);
    auto *out = static_cast<uint8_t *>(to);
    while (length) {
        if (mach_absolute_time() >= c->storeDeadline) return -1;
        uint32_t take = length > 65536 ? 65536 : static_cast<uint32_t>(length);
        if (native_ufs_modem_read(&c->window, at, out, take, c->bounce)) return -1;
        at += take; out += take; length -= take;
    }
    return 0;
}

// Compile-time static FAT path table: index 0 is modem.mdt, index i>=1 is
// modem.bNN for PH i-1. No snprintf on the freestanding ARM path.
//
// NOTE (fact vs profile header): picassoWlanModemFiles[] in
// PicassoWlanModemFirmwareProfile.h stores bare names ("modem.b00") because
// gen-picasso-wlan-modem-profile.py uses path.name on the flat staging dir.
// The device FAT volume keeps these under "image/" exactly like the ADSP
// files (image/adsp.bNN), and STAGE1-ADAPTER-DESIGN.md v2 §2 specifies
// image/modem.mdt / image/modem.bNN. The on-device directory is therefore a
// device-validated assumption: a FatNotFound on image/... is a hard stop.
struct PartPath { const char *path; bool present; };
static const PartPath kPartPaths[] = {
    {"image/modem.mdt", true},
    {"image/modem.b00", true},  {"image/modem.b01", true},
    {"image/modem.b02", true},  {"image/modem.b03", true},
    {"image/modem.b04", true},  {"image/modem.b05", true},
    {"image/modem.b06", true},  {"image/modem.b07", true},
    {"image/modem.b08", true},  {"image/modem.b09", true},
    {"image/modem.b10", true},  {"image/modem.b11", true},
    {"image/modem.b12", true},  {"image/modem.b13", true},
    {"image/modem.b14", true},  {"image/modem.b15", true},
    {"image/modem.b16", true},  {"image/modem.b17", true},
    {"image/modem.b18", false}, {"image/modem.b19", true},
    {"image/modem.b20", true},  {"image/modem.b21", true},
    {"image/modem.b22", true},  {"image/modem.b23", true},
    {"image/modem.b24", true},  {"image/modem.b25", false},
    {"image/modem.b26", true},  {"image/modem.b27", true},
    {"image/modem.b28", true},  {"image/modem.b29", true},
    {"image/modem.b30", true},  {"image/modem.b31", true},
    {"image/modem.b32", true},  {"image/modem.b33", true},
};
static_assert(sizeof(kPartPaths) / sizeof(kPartPaths[0]) == 35,
              "stage1 part table must cover mdt + b00..b33");

// ---- SMEM mapping (read-only) ---------------------------------------------
struct Region {
    OSSharedPtr<IOMemoryMap> keep;
    uintptr_t va;
};
bool mapRegion(uint64_t phys, uint64_t length, Region &region) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}
int smemRead(void *context, uint32_t offset, void *out, uint32_t size) {
    auto *region = static_cast<Region *>(context);
    volatile const uint8_t *source = reinterpret_cast<volatile const uint8_t *>(region->va);
    for (uint32_t i = 0; i < size; ++i) static_cast<uint8_t *>(out)[i] = source[offset + i];
    return 0;
}
struct PollCtx {
    Region smem;
    bool smemMapped;
    uint64_t deadline;   // absolute ms for -11 retry backoff
};

uint64_t nowMs(void *) {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000ULL;
}

// ---- shared-pass callbacks (no context needed) ----------------------------
// SCOPE BOUNDARY: sha256 ONLY computes and prints the digest. It does NOT
// compare against a pin profile. The committed PicassoWlanModemFirmwareProfile.h
// is a read-only pin of image identity, but this adapter's authenticity gate
// is TrustZone PAS cmd5 (AUTH_AND_RESET). Fabricating a "verified" flag here
// would be a lie; only the device's PAS a0 result is authoritative.
int cbSha256(void *, const uint8_t *data, size_t bytes, uint8_t out[32]) {
    if (!data || !out) return -22;
    SHA256_CTX hash;
    SHA256_Init(&hash);
    SHA256_Update(&hash, data, bytes);
    SHA256_Final(out, &hash);
    IOLog("PICASSO_WLAN stage1 sha256 bytes=%lu digest=%02x%02x%02x%02x (no pin compare; TZ PAS authenticates)\n",
          static_cast<unsigned long>(bytes), out[0], out[1], out[2], out[3]);
    return 0;
}
void cbSleep(void *, unsigned ms) { IOSleep(ms); }
int cbLeaseAcquire(void *) { return picasso_pas_acquire(); }
void cbLeaseRelease(void *, bool uncertain) { picasso_pas_release(uncertain); }
int cbAop(void *, bool on) { return picasso_aop_load_state_modem(on); }

// ---- PAS callback + modem-specific validator ------------------------------
// MUST NOT reuse picasso_pas_adsp_validate: its cmd2 only accepts 0x89200000
// (ADSP carveout) and would -22-reject the modem carveout 0x8c000000+f800000.
// U1 (device-validated): the cmd1 metadata address 0x8bc18000 is inherited
// from the ADSP shared metadata slot; DTB reserved_pil_mem_region@8bc17400
// covers it but does not publish a modem-specific offset. PAS_IS_SUPPORTED(4)
// plus the cmd5 a0 result are the gate -- never "ADSP works => modem works".
int modemValidate(unsigned command, uint64_t address, uint64_t bytes) {
    if ((command == 1 && (address != 0x8bc18000ULL || bytes != 0)) ||
        (command == 2 && (address != 0x8c000000ULL || bytes != 0xf800000ULL)) ||
        (command >= 5 && (address != 0 || bytes != 0))) return -22;
    return 0;
}
int cbPas(void *, unsigned command, uint64_t address, uint64_t bytes, uint32_t pasId) {
    if (pasId != 4) return -22;
    PicassoPasIo io = {nullptr, picasso_pas_device_call, picasso_pas_device_delay};
    PicassoPasReply reply = {};
    int error = picasso_pas_execute_ex(&io, command, address, bytes, pasId,
                                       modemValidate, &reply);
    IOLog("PICASSO_WLAN stage1 pas cmd=%u address=%llx bytes=%llx pas=%u error=%d a0=%x a1=%x session=%llx\n",
          command, static_cast<unsigned long long>(address),
          static_cast<unsigned long long>(bytes), pasId, error,
          reply.a0, reply.a1, static_cast<unsigned long long>(reply.a6));
    return error;
}

// ---- composed context (given to every callback as io->context) ------------
struct AdapterCtx {
    FirmwareCtx fw;
    PollCtx poll;
    PicassoArcResource rails[2];
    bool railsResolved;
};

// ---- read_part callback ---------------------------------------------------
int cbReadPart(void *context, unsigned index, uint8_t *dst, size_t cap, size_t *out) {
    auto *c = static_cast<AdapterCtx *>(context);
    if (!c || !dst || !out || index >= sizeof(kPartPaths) / sizeof(kPartPaths[0])) return -22;
    *out = 0;
    // p_filesz==0 parts (b18/b25) are intentionally absent: -2, no FAT access.
    if (!kPartPaths[index].present) return -2;

    FirmwareCtx &fw = c->fw;
    if (!fw.fatMounted) {
        // Bounded wait for the UFS store; never spin forever on a dead device.
        for (unsigned n = 0; n < 100 && !native_ufs_store_ready(); ++n) IOSleep(100);
        if (!native_ufs_store_ready()) return -11;
        int error = native_ufs_modem_open(&fw.window, fw.scratch,
                                          static_cast<uint32_t>(sizeof fw.scratch));
        if (error) return error;
        uint64_t bytes = (fw.window.last - fw.window.first + 1) * 4096ULL;
        error = picasso_fat_mount(&fw.volume, bytes, readFirmware, &fw);
        if (error) return error;
        fw.fatMounted = true;
    }

    int error = picasso_fat_open(&fw.volume, kPartPaths[index].path, &fw.file);
    if (error) {
        // A file the compile-time table says is present must not silently
        // disappear: -2 is reserved for the known filesz==0 parts only.
        IOLog("PICASSO_WLAN stage1 fat_open path=%s error=%d\n",
              kPartPaths[index].path, error);
        return error;
    }
    size_t want = fw.file.size;
    if (want > cap) want = cap;
    error = picasso_fat_read(&fw.volume, &fw.file, 0, dst, want);
    if (error) return error;
    *out = want;
    return 0;
}

// ---- rpmh_write callback --------------------------------------------------
// DTB /soc/qcom,mss@4080000 proxy-reg-names = vdd_cx | vdd_mss. The 5-rail
// ADSP/WCSS set (lcx/lmx/xo) is deliberately NOT touched here: a wrong-rail
// vote has an SError/kill-boot precedent (Adreno620Cp.cpp:509-513).
// Rail address + level come from picasso_rpmh_modem_resources() (cmd-db
// snapshot -> picasso_arc_lookup) in Adreno620Cp.cpp, which follows the
// existing picasso_rpmh_adsp_resources pattern. Its minimum-level constant is
// device-validated (DTB vdd 384000 uV); a lookup failure stops the adapter
// rather than guessing a level.
const char *const kRailNames[2] = {"vdd_cx", "vdd_mss"};
int cbRpmhWrite(void *context, unsigned rail, uint32_t, uint32_t) {
    auto *c = static_cast<AdapterCtx *>(context);
    if (!c || rail >= 2) return -22;
    if (!c->railsResolved) {
        int error = picasso_rpmh_modem_resources(c->rails);
        if (error) {
            IOLog("PICASSO_WLAN stage1 rpmh plan_error=%d (fail-stop, no guessed level)\n", error);
            return error;
        }
        c->railsResolved = true;
    }
    // The ADSP owner writes (address, index) -- the ARC level index, not the
    // voltage literal (PicassoAdspBoot.cpp:104). Do the same here.
    const PicassoArcResource &r = c->rails[rail];
    int error = picasso_rpmh_active_write(r.address, r.index);
    IOLog("PICASSO_WLAN stage1 rpmh rail=%u name=%s address=%x index=%u level=%u error=%d\n",
          rail, kRailNames[rail], r.address, r.index, r.level, error);
    return error;
}

// ---- poll_peer callback (read-only SMEM/SMP2P) ----------------------------
int cbPollPeer(void *context, struct WlanModemPeerEvent *event) {
    auto *ac = static_cast<AdapterCtx *>(context);
    if (!ac || !event) return -22;
    *event = {};
    PollCtx &c = ac->poll;
    if (!c.smemMapped) {
        if (!mapRegion(WLAN_SMEM_BASE, WLAN_SMEM_SIZE, c.smem)) return -22;
        c.smemMapped = true;
    }
    struct wlan_smem_io io = {&c.smem, smemRead};
    // DTB qcom,smp2p-mpss: local (APPS) pid = 0, remote (MPSS) pid = 1.
    // U4 (device-validated): WlanProbe.cpp:109 passes local=1/remote=0 for
    // item 435 (that item is authored by the remote, so its header stores
    // local=1/remote=0). STAGE1-ADAPTER-DESIGN.md v2 §2/§3 orders us NOT to
    // copy that and to use the DTB's local=0/remote=1 here. We do exactly
    // that; if the SMEM header rejects it with -22 on device, U4 has failed
    // and the adapter stops rather than guessing the other order.
    constexpr uint16_t kLocalPid = 0, kRemotePid = 1;
    const uint16_t ids[] = {WLAN_SMEM_IN, WLAN_SMEM_OUT};  // 435 / 428

    for (;;) {
        struct wlan_smem_partition partition = {};
        int error = wlan_smem_partition(&io, 0, 1, &partition);
        if (error == -2) error = wlan_smem_partition(&io, 0xfffe, 0xfffe, &partition);
        if (error == -11) {                         // concurrent remote allocation
            if (nowMs(nullptr) >= c.deadline) return -11;
            IOSleep(10);
            continue;
        }
        if (error) return error == -2 ? -2 : -22;

        bool retryOuter = false;
        for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
            struct wlan_smem_item item = {};
            int found = wlan_smem_find(&io, &partition, ids[i], &item);
            if (found == -11) {                     // retry to deadline
                if (nowMs(nullptr) >= c.deadline) return -11;
                IOSleep(10);
                retryOuter = true;
                break;
            }
            if (found == -22) return -22;           // parse failure: hard stop
            if (found == -2) continue;              // item absent: try the other
            if (found) return -22;

            uint32_t value = 0;
            int parsed = wlan_smp2p_wlan(&io, &item, kLocalPid, kRemotePid, &value);
            if (parsed == -11) {
                if (nowMs(nullptr) >= c.deadline) return -11;
                IOSleep(10);
                retryOuter = true;
                break;
            }
            if (parsed == -22) return -22;          // parse failure: hard stop
            if (parsed == -2) continue;             // "wlan" entry absent
            if (parsed) return -22;
            // Bit meanings are NOT resolved in reviewed sources: expose the
            // raw value only; do not invent ready/handover/stop semantics.
            event->hasValue = true;
            event->value = value;
            return 0;
        }
        if (retryOuter) continue;
        // First call before MPSS loads: both items absent -> skeleton's -2.
        return -2;
    }
}

}  // namespace

extern "C" int picasso_wlan_stage1_probe_pas4(void) {
    uint32_t a1 = 0;
    int a0 = picasso_pas4_supported_probe(&a1);
    IOLog("PICASSO_WLAN stage1 probe pas4 a0=%d a1=%x (read-only; PAS_IS_SUPPORTED)\n", a0, a1);
    return a0;
}

extern "C" int picasso_wlan_stage1_run(void) {
    // Fail-stop gate 0: the PIL service must accept PAS id 4 before any
    // cmd1/2/5 write is attempted. First half of design §6b.
    int a0 = picasso_wlan_stage1_probe_pas4();
    if (a0 != 0) {
        IOLog("PICASSO_WLAN stage1 stop: pas4 unsupported (a0=%d), no PAS writes attempted\n", a0);
        return a0 ? a0 : -5;
    }

    auto *ctx = static_cast<AdapterCtx *>(IOMalloc(sizeof(AdapterCtx)));
    if (!ctx) {
        IOLog("PICASSO_WLAN stage1 stop: allocation error=-12\n");
        return -12;
    }
    memset(ctx, 0, sizeof(*ctx));

    WlanModemPilConfig cfg;
    picasso_wlan_modem_pil_default_config(&cfg);
    ctx->poll.deadline = nowMs(nullptr) + cfg.timeoutMs;

    const WlanModemPilIo io = {
        ctx,
        cbReadPart, cbSha256, cbRpmhWrite,
        cbLeaseAcquire, cbLeaseRelease, cbAop, cbPas,
        cbPollPeer, cbSleep, nowMs,
    };
    WlanModemPilState state = {};
    int error = picasso_wlan_modem_pil_execute(&io, &cfg, &state);

    IOLog("PICASSO_WLAN stage1 adapter phase=%s step=%u error=%d rc=%d pasCalls=%u railVotes=%u "
          "partsLoaded=%u peerPolls=%u authenticated=%u ready=%u leased=%u released=%u quarantined=%u\n",
          state.phase ? state.phase : "(null)", state.step, state.error, error,
          state.pasCalls, state.railVotes, state.partsLoaded, state.peerPolls,
          state.authenticated, state.ready, state.leased, state.released, state.quarantined);
    IOFree(ctx, sizeof(AdapterCtx));
    return error;
}
