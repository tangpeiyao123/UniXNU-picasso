// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real-device stage-1 adapter: the 10 injected WlanModemPilIo callbacks of
// PicassoWlanModemPil.cpp bound to the Picasso hardware boundaries.
//
// Reuses, and never modifies, the shared GPU/ADSP owners: picasso_pas_acquire/
// release, adspScm (the production PicassoPasIo.call, including the x0..x7
// encoding and the x6 interrupted-session quirk) and picasso_rpmh_active_write.
// Only the FIRMWARE READ boundary is new: include FirmwareWindow.h (declarations
// only), NEVER FirmwareRead.h (its strong definitions would collide with
// libNativeUfs.a / NativeUfs.cpp at link time).
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <libkern/crypto/sha2.h>
#include <string.h>

#include "PicassoWlanStage1Adapter.h"
#include "PicassoWlanModemPil.h"
#include "PicassoFat.h"
#include "FirmwareWindow.h"
#include "WlanSmem.h"
#include "WlanModemLoadPlan.h"
#include "PicassoPas.h"
#include "PicassoRpmh.h"
#include "PicassoWlanModemFirmwareProfile.h"

extern "C" {
int native_ufs_store_ready(void);                       // NativeUfs.cpp
int picasso_aop_load_state_modem(bool on);              // PicassoAop.cpp:197
int picasso_pas4_supported_probe(uint32_t *outA1);      // Adreno620Cp.cpp (read-only)
}

namespace {

// ---- compile-time part table ---------------------------------------------
// index 0 = modem.mdt; index i>=1 = modem.bNN backing PH i-1. No snprintf on
// the freestanding ARM path. The staged profile stores bare file names while
// the on-device FAT volume uses the ADSP-style "image/" prefix (design v2 §2);
// a FatNotFound here is a hard stop, not a guessed fallback. b18/b25/b34 have
// p_filesz==0 and are intentionally absent (-2).
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
static_assert(sizeof(kPartPaths) / sizeof(kPartPaths[0]) == WLAN_MODEM_HEADERS,
              "stage1 part table must cover mdt + b00..b33");

const char *basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}
// Pin profile is ordered mdt first, then b00..b33 with b18/b25 absent. Match
// by file name instead of array index (the ADSP profile orders mdt LAST, but
// the modem profile does not -- verified from the generated header).
const struct picasso_wlan_modem_file *findPin(unsigned index) {
    if (index >= sizeof(kPartPaths) / sizeof(kPartPaths[0])) return nullptr;
    const char *want = basename(kPartPaths[index].path);
    for (unsigned i = 0; i < PICASSO_WLAN_MODEM_FILE_COUNT; ++i)
        if (!strcmp(want, picassoWlanModemFiles[i].path)) return &picassoWlanModemFiles[i];
    return nullptr;
}

// ---- UFS/FAT context -----------------------------------------------------
struct FirmwareCtx {
    UfsFirmwareWindow window;
    FatVolume volume;
    FatFile file;
    uint8_t scratch[16384];
    uint8_t bounce[4096];
    bool fatMounted;
    uint64_t storeDeadline;
};
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

// ---- fixed physical window (carveout / metadata slot) --------------------
struct PhysWindow {
    IOMemoryMap *map;
    volatile uint8_t *va;
    bool mapped;
};
bool mapPhys(uint64_t phys, uint64_t bytes, PhysWindow &w) {
    IOMemoryDescriptor *d = IOMemoryDescriptor::withPhysicalAddress(phys, bytes, kIODirectionInOut);
    if (!d) return false;
    IOMemoryMap *m = d->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache);
    d->release();
    if (!m) return false;
    w.map = m;
    w.va = reinterpret_cast<volatile uint8_t *>(static_cast<uintptr_t>(m->getAddress()));
    w.mapped = w.va != nullptr;
    return w.mapped;
}
void unmapPhys(PhysWindow &w) {
    if (w.map) w.map->release();
    w.map = nullptr; w.va = nullptr; w.mapped = false;
}

// ---- read-only SMEM/SMP2P poll context -----------------------------------
struct Region { OSSharedPtr<IOMemoryMap> keep; uintptr_t va; };
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
struct PollCtx { Region smem; bool smemMapped; uint64_t deadline; };
uint64_t nowMs(void *) {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000ULL;
}

// ---- composed context handed to every callback as io->context -------------
struct AdapterCtx {
    FirmwareCtx fw;
    PollCtx poll;
    PicassoArcResource rails[2];
    bool railsResolved;
    unsigned currentIndex;
    const struct picasso_wlan_modem_file *pin;
    uint8_t digest[32];
    size_t digestBytes;
    uint8_t metadata[WLAN_MODEM_METADATA_BYTES];
    // Metadata staging for the PIL state machine itself (cfg->metadataBuffer).
    // Separate from `metadata` above, which cbReadPart uses to stage the FAT
    // mdt before copying into the skeleton. This one lives in the IOMalloc'd
    // AdapterCtx (heap), keeping the 0x3000-byte array off the kernel stack.
    uint8_t pilMetadata[0x3000];
    uint32_t pieces[WLAN_MODEM_HEADERS];
    struct wlan_modem_load_plan plan;
    bool planReady;
    PhysWindow metadataSlot;
    PhysWindow carveout;
};

// ---- read_part: read, hash, pin-check and stage into the carveout ---------
int cbReadPart(void *context, unsigned index, uint8_t *dst, size_t cap, size_t *out) {
    auto *c = static_cast<AdapterCtx *>(context);
    if (!c || !dst || !out || index >= sizeof(kPartPaths) / sizeof(kPartPaths[0])) return -22;
    *out = 0;
    if (!kPartPaths[index].present) return -2;   // filesz==0: b18/b25/b34

    const struct picasso_wlan_modem_file *pin = findPin(index);
    if (!pin) return -22;                        // table/profile out of sync
    c->currentIndex = index;
    c->pin = pin;

    FirmwareCtx &fw = c->fw;
    if (!fw.fatMounted) {
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
        IOLog("PICASSO_WLAN stage1 fat_open path=%s error=%d\n", kPartPaths[index].path, error);
        return error;
    }
    const uint32_t fileBytes = fw.file.size;

    if (index == 0) {
        // modem.mdt: ELF header + 35 PHs. The skeleton gives us 0x3000 bytes
        // of capacity, so the full file fits and we can parse and stage it.
        if (fileBytes > sizeof c->metadata || cap < fileBytes) return -22;
        SHA256_CTX h; SHA256_Init(&h);
        for (uint32_t off = 0; off < fileBytes;) {
            uint32_t take = sizeof fw.bounce < fileBytes - off
                ? static_cast<uint32_t>(sizeof fw.bounce) : fileBytes - off;
            error = picasso_fat_read(&fw.volume, &fw.file, off, c->metadata + off, take);
            if (error) { uint8_t ignored[32]; SHA256_Final(ignored, &h); return error; }
            SHA256_Update(&h, c->metadata + off, take);
            off += take;
        }
        SHA256_Final(c->digest, &h);
        c->digestBytes = fileBytes;
        memcpy(dst, c->metadata, fileBytes);
        *out = fileBytes;

        // pieces[ph] = p_filesz of PH ph = size of disk file b(ph) = part ph+1.
        for (unsigned ph = 0; ph < WLAN_MODEM_HEADERS; ++ph) {
            unsigned part = ph + 1;
            const struct picasso_wlan_modem_file *p =
                (part < WLAN_MODEM_HEADERS && kPartPaths[part].present) ? findPin(part) : nullptr;
            c->pieces[ph] = p ? p->bytes : 0;   // PH18/25/34: filesz==0
        }
        error = wlan_modem_load_plan(c->metadata, sizeof c->metadata, c->pieces,
                                     WLAN_MODEM_HEADERS, &c->plan);
        if (error) {
            IOLog("PICASSO_WLAN stage1 elf-plan error=%d (fail-stop)\n", error);
            return error;
        }
        c->planReady = true;
        // PAS_INIT_IMAGE reads its metadata from the fixed 0x8bc18000 slot.
        // Stage the mdt there with byte stores + dsb, exactly like the GPU
        // ZAP path (Adreno620Cp.cpp:1501-1514). No memcpy/bzero on the
        // kIOMapInhibitCache mapping (DC ZVA / unaligned STP fault there).
        if (!c->metadataSlot.mapped && !mapPhys(0x8bc18000ULL, 0x3000ULL, c->metadataSlot)) return -12;
        volatile uint8_t *slot = c->metadataSlot.va;
        for (uint32_t i = 0; i < 0x3000; ++i) slot[i] = i < fileBytes ? c->metadata[i] : 0;
        __asm__ volatile("dsb sy" ::: "memory");
        IOLog("PICASSO_WLAN stage1 metadata staged bytes=%u segments=%u\n", fileBytes, c->plan.count);
        return 0;
    }

    // index>=1: disk b(index-1) backs PH index-1. The skeleton's chunk is only
    // 4096 bytes, so the full file is read here for both the pin digest and
    // the carveout copy; only min(cap, filesz) bytes go back to the skeleton.
    if (!c->planReady) return -22;
    const struct wlan_modem_segment *seg = nullptr;
    for (unsigned k = 0; k < c->plan.count; ++k)
        if (c->plan.segments[k].index == index - 1) { seg = &c->plan.segments[k]; break; }
    if (!seg || seg->fileBytes != fileBytes) return -22;
    if (!c->carveout.mapped && !mapPhys(WLAN_MODEM_BASE, WLAN_MODEM_BYTES, c->carveout)) return -12;
    if (seg->memoryOffset + seg->fileBytes + seg->zeroBytes > WLAN_MODEM_BYTES) return -22;

    const size_t copyBytes = fileBytes < cap ? fileBytes : cap;
    volatile uint8_t *base = c->carveout.va + seg->memoryOffset;
    SHA256_CTX h; SHA256_Init(&h);
    for (uint32_t off = 0; off < fileBytes;) {
        uint8_t buf[4096];
        uint32_t take = sizeof buf < fileBytes - off ? sizeof buf : fileBytes - off;
        error = picasso_fat_read(&fw.volume, &fw.file, off, buf, take);
        if (error) { uint8_t ignored[32]; SHA256_Final(ignored, &h); return error; }
        SHA256_Update(&h, buf, take);
        for (uint32_t j = 0; j < take; ++j) base[off + j] = buf[j];   // byte stores only
        if (off < copyBytes) {
            uint32_t n = take;
            if (off + n > copyBytes) n = static_cast<uint32_t>(copyBytes - off);
            memcpy(dst + off, buf, n);
        }
        off += take;
    }
    for (uint32_t j = 0; j < seg->zeroBytes; ++j) base[fileBytes + j] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    SHA256_Final(c->digest, &h);
    c->digestBytes = fileBytes;
    *out = copyBytes;
    IOLog("PICASSO_WLAN stage1 segment ph=%u offset=%x filesz=%u zerofill=%u staged=1\n",
          index - 1, seg->memoryOffset, seg->fileBytes, seg->zeroBytes);
    return 0;
}

// ---- sha256: compare the real full-file digest against the pin profile ----
// The committed PicassoWlanModemFirmwareProfile.h is a read-only pin of every
// part. A mismatch returns -8 BEFORE any SCM write. The skeleton hands this
// callback only its 4096-byte chunk, so the digest is computed in read_part
// over the whole file (where the bytes actually exist) and recorded together
// with currentIndex/pin; this callback performs the actual comparison. No
// authenticity is claimed from our digest alone -- TZ PAS cmd5 remains the
// device authority.
int cbSha256(void *context, const uint8_t *, size_t, uint8_t out[32]) {
    auto *c = static_cast<AdapterCtx *>(context);
    if (!c || !c->pin) return -22;
    if (c->digestBytes != c->pin->bytes ||
        memcmp(c->digest, c->pin->sha256, sizeof c->digest)) {
        IOLog("PICASSO_WLAN stage1 pin-mismatch index=%u path=%s bytes=%lu expected=%u (fail-stop)\n",
              c->currentIndex, c->pin->path, static_cast<unsigned long>(c->digestBytes), c->pin->bytes);
        return -8;
    }
    memcpy(out, c->digest, sizeof c->digest);
    return 0;
}

// ---- rpmh / lease / aop / pas --------------------------------------------
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
    const PicassoArcResource &r = c->rails[rail];
    int error = picasso_rpmh_active_write(r.address, r.index);
    IOLog("PICASSO_WLAN stage1 rpmh rail=%u name=%s address=%x index=%u level=%u error=%d\n",
          rail, kRailNames[rail], r.address, r.index, r.level, error);
    return error;
}
void cbSleep(void *, unsigned ms) { IOSleep(ms); }
int cbLeaseAcquire(void *) { return picasso_pas_acquire(); }
void cbLeaseRelease(void *, bool uncertain) { picasso_pas_release(uncertain); }
int cbAop(void *, bool on) { return picasso_aop_load_state_modem(on); }

// New modem validator: the ADSP one rejects the modem carveout (its cmd2 only
// accepts 0x89200000). cmd1 metadata address is U1 (device-validated).
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
    int error = picasso_pas_execute_ex(&io, command, address, bytes, pasId, modemValidate, &reply);
    IOLog("PICASSO_WLAN stage1 pas cmd=%u address=%llx bytes=%llx pas=%u error=%d a0=%x a1=%x session=%llx\n",
          command, static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes),
          pasId, error, reply.a0, reply.a1, static_cast<unsigned long long>(reply.a6));
    return error;
}

// ---- poll_peer: read-only SMEM/SMP2P -------------------------------------
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
    // Copy WlanProbe.cpp:109 exactly. Item 435 (IN) is authored by MPSS(1)
    // toward APPS(0) so its header stores local=1/remote=0; item 428 (OUT)
    // is the reverse. WlanSmem.h:93 checks h+8==local, h+10==remote. The DTB
    // "local=0/remote=1" note in the design is about the APPS device, not the
    // header fields, and has no in-tree source evidence -- do not use it.
    const uint16_t ids[] = {WLAN_SMEM_IN, WLAN_SMEM_OUT};  // 435 / 428
    for (;;) {
        struct wlan_smem_partition partition = {};
        int error = wlan_smem_partition(&io, 0, 1, &partition);
        if (error == -2) error = wlan_smem_partition(&io, 0xfffe, 0xfffe, &partition);
        if (error == -11) {
            if (nowMs(nullptr) >= c.deadline) return -11;
            IOSleep(10);
            continue;
        }
        if (error) return error == -2 ? -2 : -22;

        bool retryOuter = false;
        for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
            struct wlan_smem_item item = {};
            int found = wlan_smem_find(&io, &partition, ids[i], &item);
            if (found == -11) {
                if (nowMs(nullptr) >= c.deadline) return -11;
                IOSleep(10); retryOuter = true; break;
            }
            if (found == -22) return -22;
            if (found == -2) continue;
            if (found) return -22;

            uint32_t value = 0;
            int parsed = wlan_smp2p_wlan(&io, &item, i ? 0 : 1, i ? 1 : 0, &value);
            if (parsed == -11) {
                if (nowMs(nullptr) >= c.deadline) return -11;
                IOSleep(10); retryOuter = true; break;
            }
            if (parsed == -22) return -22;
            if (parsed == -2) continue;
            if (parsed) return -22;
            event->hasValue = true;
            event->value = value;
            return 0;
        }
        if (retryOuter) continue;
        return -2;   // first poll before MPSS loads: both items absent
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
    int a0 = picasso_wlan_stage1_probe_pas4();
    if (a0 != 0) {
        IOLog("PICASSO_WLAN stage1 stop: pas4 unsupported (a0=%d), no PAS writes attempted\n", a0);
        return a0 ? a0 : -5;
    }
    auto *ctx = static_cast<AdapterCtx *>(IOMalloc(sizeof(AdapterCtx)));
    if (!ctx) { IOLog("PICASSO_WLAN stage1 stop: allocation error=-12\n"); return -12; }
    memset(ctx, 0, sizeof(*ctx));

    WlanModemPilConfig cfg;
    picasso_wlan_modem_pil_default_config(&cfg);
    cfg.metadataBuffer = ctx->pilMetadata;   // heap-backed, requester-owned
    ctx->poll.deadline = nowMs(nullptr) + cfg.timeoutMs;

    const WlanModemPilIo io = {
        ctx, cbReadPart, cbSha256, cbRpmhWrite,
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
    unmapPhys(ctx->carveout);
    unmapPhys(ctx->metadataSlot);
    IOFree(ctx, sizeof(AdapterCtx));
    return error;
}
