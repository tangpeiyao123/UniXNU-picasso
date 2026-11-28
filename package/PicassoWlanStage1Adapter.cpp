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
#include <pexpert/pexpert.h>
#include <libkern/crypto/sha2.h>
#include <string.h>

#include "PicassoWlanStage1Adapter.h"
#include "PicassoWlanModemPil.h"
#include "PicassoFat.h"
#include "FirmwareWindow.h"
#include "WlanSmem.h"
#include "PicassoGlinkProbe.h"
#include "PicassoMpssGlinkDiag.h"
#include "WlanModemLoadPlan.h"
#include "PicassoPas.h"
#include "PicassoRpmh.h"
#include "PicassoWlanModemFirmwareProfile.h"

extern "C" {
int native_ufs_store_ready(void);                       // NativeUfs.cpp
int picasso_aop_load_state_modem(bool on);              // PicassoAop.cpp:197
int picasso_pas4_supported_probe(uint32_t *outA1);      // Adreno620Cp.cpp (read-only)
unsigned picasso_pas_gate_snapshot(void);               // Adreno620Cp.cpp (read-only)
}

namespace {

// nowMs() is defined below (:144) but readFirmware() needs the same millisecond
// clock for its deadline. mach_absolute_time() returns raw ticks, so comparing
// it against a millisecond deadline is always true; never mix the two units.
uint64_t nowMs(void *);
constexpr uint64_t kFirmwareReadBudgetMs = 30000;

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
        if (nowMs(nullptr) >= c->storeDeadline) return -1;
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

// ---- controlled-write GLINK liveness diag IO (boot-arg gated, §9.21/§9.22) --
// SMEM region is mapped writable (kIOMapInhibitCache). IPCC is mapped on demand.
struct DiagCtx { Region *smem; Region ipcc; bool ipccMapped; };
int diagSmemWrite32(void *context, uint32_t at, uint32_t value) {
    auto *d = static_cast<DiagCtx *>(context);
    if (!d || !d->smem || !d->smem->va || at > WLAN_SMEM_SIZE - 4) return -22;
    *reinterpret_cast<volatile uint32_t *>(d->smem->va + at) = value;
    return 0;
}
void diagBarrier(void *) { __asm__ volatile("dsb sy" ::: "memory"); }
int diagIpccKick(void *context, uint32_t value) {
    auto *d = static_cast<DiagCtx *>(context);
    if (!d || !d->ipccMapped || !d->ipcc.va) return -22;
    *reinterpret_cast<volatile uint32_t *>(d->ipcc.va + 0x0c) = value; // mbox SEND_ID
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
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
        // AdapterCtx is memset to zero on allocation; seed the read deadline
        // here (ms clock, matching readFirmware) before any fat_mount read.
        fw.storeDeadline = nowMs(nullptr) + kFirmwareReadBudgetMs;
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

    // index 1/2 are disk b00/b01 = ELF PH0/PH1, which wlan_modem_load_plan()
    // deliberately skips (PH0 = ELF header, PH1 = hash table; their bytes are
    // already inside modem.mdt = PH0+PH1, staged to the PAS metadata slot above
    // and covered by the pinned mdt digest). plan.segments[].index therefore
    // starts at PH2, so there is no carveout segment with index-1 == 0/1.
    // Without this guard, loadSegments() i=1 hits "!seg -> -22" on its FIRST
    // part and zero segments get staged (fail-stop, observed on device).
    // Treat them as intentionally absent, same as filesz==0 parts (b18/b25).
    if (index >= 1 && index <= 2) return -2;

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
const char *const kRailNames[2] = {"cx.lvl", "mss.lvl"};
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
    IOLog("PICASSO_WLAN stage1 t=%llu rpmh rail=%u name=%s address=%x index=%u level=%u error=%d\n",
          (unsigned long long)nowMs(nullptr), rail, kRailNames[rail], r.address, r.index, r.level, error);
    return error;
}
void cbSleep(void *, unsigned ms) { IOSleep(ms); }
// PAS trace: entry/exit timestamps around the two blocking boundaries. These
// are the only calls that can stall stage1 for ~30s (pasGate 30000x IOSleep(1)
// and the AOP QMP load_state wait). Logging only -- no control-flow change.
int cbLeaseAcquire(void *) {
    uint64_t t0 = nowMs(nullptr);
    unsigned g0 = picasso_pas_gate_snapshot();
    int error = picasso_pas_acquire();
    IOLog("PICASSO_WLAN stage1 t=%llu lease acquire error=%d gate_in=%u gate_out=%u\n",
          (unsigned long long)t0, error, g0, picasso_pas_gate_snapshot());
    return error;
}
void cbLeaseRelease(void *, bool uncertain) {
    unsigned g0 = picasso_pas_gate_snapshot();
    picasso_pas_release(uncertain);
    IOLog("PICASSO_WLAN stage1 t=%llu lease release uncertain=%u gate_in=%u gate_out=%u\n",
          (unsigned long long)nowMs(nullptr), uncertain, g0, picasso_pas_gate_snapshot());
}
int cbAop(void *, bool on) {
    uint64_t t0 = nowMs(nullptr);
    IOLog("PICASSO_WLAN stage1 t=%llu aop enter on=%u gate=%u\n",
          (unsigned long long)t0, on, picasso_pas_gate_snapshot());
    int error = picasso_aop_load_state_modem(on);
    IOLog("PICASSO_WLAN stage1 t=%llu aop exit on=%u error=%d gate=%u\n",
          (unsigned long long)nowMs(nullptr), on, error, picasso_pas_gate_snapshot());
    return error;
}

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
    IOLog("PICASSO_WLAN stage1 t=%llu pas cmd=%u address=%llx bytes=%llx pas=%u error=%d a0=%x a1=%x session=%llx gate=%u\n",
          (unsigned long long)nowMs(nullptr), command,
          static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes),
          pasId, error, reply.a0, reply.a1, static_cast<unsigned long long>(reply.a6),
          picasso_pas_gate_snapshot());
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

// ---- READ-ONLY SMP2P item dump helper (diagnostic only) ----
// Prints an SMP2P item's 20B header (magic/ver/local/remote/total/valid) and
// every 20B entry slot (name + value). Pure reads; no ownership writes, no
// allocation, no state change. Used by diagSurveyPeer only.
static void diagDumpSmp2pItem(struct wlan_smem_io *io, const char *tag,
                              uint16_t itemId, const struct wlan_smem_item *item) {
    uint8_t h[20] = {0};
    if (wlan_smread(io, item->offset, h, 20)) {
        IOLog("PICASSO_WLAN diag-dump tag=%s item=%u read-header-failed off=%x\n",
              tag, itemId, item->offset);
        return;
    }
    unsigned total = wlan_sm16(h + 12), valid = wlan_sm16(h + 14);
    IOLog("PICASSO_WLAN diag-dump tag=%s item=%u off=%x size=%x magic=%08x ver=%u "
          "local=%u remote=%u total=%u valid=%u\n",
          tag, itemId, item->offset, item->size,
          (unsigned)wlan_sm32(h + 0), (unsigned)h[4],
          (unsigned)wlan_sm16(h + 8), (unsigned)wlan_sm16(h + 10),
          total, valid);
    // Never read past the item: cap slots by (size - 20)/20, matching the
    // wire format size>=20+20*total check in wlan_smp2p_entry.
    unsigned fit = item->size >= 20 ? (item->size - 20) / 20 : 0;
    unsigned slots = total < 16 ? total : 16;
    if (slots > fit) slots = fit;
    for (unsigned i = 0; i < slots; ++i) {
        uint8_t e[20] = {0};
        if (wlan_smread(io, item->offset + 20 + 20 * i, e, 20)) {
            IOLog("PICASSO_WLAN diag-dump tag=%s item=%u entry=%u read-failed\n",
                  tag, itemId, i);
            continue;
        }
        // name is 16 raw bytes; render printable ASCII, hex otherwise.
        char name[17] = {0};
        for (unsigned j = 0; j < 16; ++j) {
            uint8_t c = e[j];
            name[j] = (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '.';
        }
        name[16] = 0;
        IOLog("PICASSO_WLAN diag-dump tag=%s item=%u entry=%u name=%s value=%08x "
              "raw=%02x%02x%02x%02x%02x%02x%02x%02x\n",
              tag, itemId, i, name, (unsigned)wlan_sm32(e + 16),
              e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
    }
}

// Scan every partition listed in the SMEM TOC for a named "wlan" SMP2P entry.
// Some platforms place the peer item in a host/remote pair other than (0,1) or
// (0xfffe,0xfffe). This enumerates all TOC partitions and all their items,
// looking for the first 4 bytes "wlan" at any item's entry table. Read-only.
static void diagScanTocForWlan(struct wlan_smem_io *io, const char *tag) {
    const uint32_t toc = WLAN_SMEM_SIZE - 4096;
    uint8_t h[32] = {0};
    if (wlan_smread(io, toc, h, 32)) return;
    uint32_t count = wlan_sm32(h + 8);
    if (wlan_sm32(h) != 0x434f5424 || wlan_sm32(h + 4) != 1 || count > 64) return;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t e[48] = {0};
        if (wlan_smread(io, toc + 32 + 48 * i, e, 48)) continue;
        uint16_t x = wlan_sm16(e + 12), y = wlan_sm16(e + 14);
        struct wlan_smem_partition p = {};
        if (wlan_smem_partition(io, x, y, &p)) continue;
        // Walk the uncached item table; for each a5a5 item, probe its entries
        // for a "wlan" name. Bounded by partition size / 16.
        uint32_t at = 32, visited = 0;
        while (at + 16 <= p.uncached && visited < 256) {
            ++visited;
            uint8_t ih[16] = {0};
            if (wlan_smread(io, p.offset + at, ih, 16)) break;
            if (wlan_sm16(ih) != 0xa5a5) break;
            uint32_t isz = wlan_sm32(ih + 4), hdr = wlan_sm16(ih + 10);
            uint16_t id = wlan_sm16(ih + 2);
            uint16_t pad = wlan_sm16(ih + 8);
            if (!isz || pad >= isz || hdr > p.uncached - at - 16 ||
                isz > p.uncached - at - 16 - hdr) break;
            uint32_t ioff = p.offset + at + 16 + hdr;
            uint32_t isize = isz - pad;
            uint8_t wh[20] = {0};
            if (isize >= 20 && !wlan_smread(io, ioff, wh, 20) &&
                wlan_sm32(wh) == 0x504d5324 && wh[4] == 1) {
                unsigned total = wlan_sm16(wh + 12);
                if (total > 16) total = 16;
                unsigned fit = (isize - 20) / 20;
                if (total > fit) total = fit;
                for (unsigned k = 0; k < total; ++k) {
                    uint8_t ee[20] = {0};
                    if (wlan_smread(io, ioff + 20 + 20 * k, ee, 20)) break;
                    if (ee[0] == 'w' && ee[1] == 'l' && ee[2] == 'a' && ee[3] == 'n' &&
                        ee[4] == 0) {
                        IOLog("PICASSO_WLAN diag-scan tag=%s toc=%u host=%u remote=%u "
                              "item=%u off=%x local=%u remote_pid=%u total=%u "
                              "wlan-slot=%u value=%08x\n",
                              tag, i, x, y, id, ioff,
                              (unsigned)wlan_sm16(wh + 8), (unsigned)wlan_sm16(wh + 10),
                              (unsigned)wlan_sm16(wh + 12), k,
                              (unsigned)wlan_sm32(ee + 16));
                    }
                }
            }
            at += 16 + hdr + isz;
            if (at & 7) break;
        }
    }
    IOLog("PICASSO_WLAN diag-scan tag=%s complete count=%u\n", tag, (unsigned)count);
}

// ---- READ-ONLY peer-timeout survey (diagnostic only, zero control flow) ----
// Invoked once after execute() returns -60 (peer timeout). Everything below is
// observation: no PAS/RPMH/AOP/SMEM writes, no state mutation, no retry.
// diag3 settled the earlier A/B question: TOC is valid (magic 434f5424) and
// item 435 EXISTS in the private (0,1) partition (find=0, size=154), so neither
// "MPSS never ran" nor "wrong partition" explains the -60. diag4 therefore
// answers the remaining question: does item 435 actually carry a "wlan" entry?
//   (c) header valid but valid=0 / no "wlan" name / value=0 -> cbPollPeer's
//       wlan_smp2p_wlan returns -2, poll stream sees "not ready" until deadline;
//   (d) "wlan" entry exists in some OTHER TOC partition not visited by
//       cbPollPeer's (0,1)/(0xfffe) probes -> reader placement bug.
// Full header+entry dump (diagDumpSmp2pItem) and whole-TOC "wlan" scan
// (diagScanTocForWlan) distinguish (c) from (d) in one boot.
static void diagSurveyPeer(AdapterCtx *ac, uint64_t now, int result) {
    if (!ac || !ac->poll.smemMapped) {
        IOLog("PICASSO_WLAN diag-survey result=%d now=%llu smemMapped=0 (no survey)\n",
              result, (unsigned long long)now);
        return;
    }
    struct wlan_smem_io io = {&ac->poll.smem, smemRead};

    // P1/P1b self-check (§4.9/§9.15): read-only MPSS GLINK geometry discovery.
    // Printed FIRST so port-lost truncation cannot hide the decisive fields.
    // P1 boot found TWO 32B desc candidates; P1b prints EVERY candidate with
    // its own double-read/pointer validity so the next boot can pick the real
    // desc. No channel open, no QMI, no kick, no hardware write.
    {
        struct picasso_glink_probe_result pr = {};
        int gerr = picasso_mpss_glink_probe(&io, &pr);
        IOLog("PICASSO_WLAN mpss-glink-check part=0/1 err=%d cand=%u "
              "roles=%u/%u/%u descCount=%u validDesc=%u "
              "tx=%u/%u rx=%u/%u sendReady=%u kick=unknown\n",
              gerr, (unsigned)pr.cand,
              (unsigned)pr.roles[0], (unsigned)pr.roles[1], (unsigned)pr.roles[2],
              (unsigned)pr.descCount, (unsigned)pr.validDesc,
              (unsigned)pr.txId, (unsigned)pr.txSize,
              (unsigned)pr.rxId, (unsigned)pr.rxSize,
              (unsigned)pr.sendReady);
        for (unsigned n = 0; n < pr.descCount; ++n) {
            const auto &d = pr.desc[n];
            IOLog("PICASSO_WLAN mpss-glink-desc n=%u id=%u off=%x size=%u "
                  "stable=%u ptrValid=%u valid=%u "
                  "ctrl=%08x/%08x/%08x/%08x\n",
                  n, (unsigned)d.id, d.off, (unsigned)d.size,
                  (unsigned)d.stable, (unsigned)d.ptrValid, (unsigned)d.valid,
                  (unsigned)((uint32_t)d.ctrl[0] | ((uint32_t)d.ctrl[1]<<8) | ((uint32_t)d.ctrl[2]<<16) | ((uint32_t)d.ctrl[3]<<24)),
                  (unsigned)((uint32_t)d.ctrl[4] | ((uint32_t)d.ctrl[5]<<8) | ((uint32_t)d.ctrl[6]<<16) | ((uint32_t)d.ctrl[7]<<24)),
                  (unsigned)((uint32_t)d.ctrl[8] | ((uint32_t)d.ctrl[9]<<8) | ((uint32_t)d.ctrl[10]<<16) | ((uint32_t)d.ctrl[11]<<24)),
                  (unsigned)((uint32_t)d.ctrl[12] | ((uint32_t)d.ctrl[13]<<8) | ((uint32_t)d.ctrl[14]<<16) | ((uint32_t)d.ctrl[15]<<24)));
        }
    }

    // SMEM header sanity + raw TOC header (read-only). Keep SMEM version in its
    // own variable: the TOC read below overwrites hdr[].
    uint8_t hdr[32] = {0};
    smemRead(&ac->poll.smem, 0x5c, hdr, 4);
    uint32_t smemVer = (wlan_sm32(hdr) >> 16) & 0xffff;
    const uint32_t toc = WLAN_SMEM_SIZE - 4096;
    smemRead(&ac->poll.smem, toc, hdr, 32);
    // TOC header per WlanSmem.h:24-28 -> magic@+0 (0x434f5424), ver@+4 (1), count@+8.
    IOLog("PICASSO_WLAN diag-survey result=%d now=%llu smem-ver=%u toc-magic=%08x toc-ver=%u toc-count=%u\n",
          result, (unsigned long long)now, smemVer,
          (unsigned)wlan_sm32(hdr + 0), (unsigned)wlan_sm32(hdr + 4),
          (unsigned)wlan_sm32(hdr + 8));

    // Probe BOTH partitions explicitly. cbPollPeer only falls back to global
    // when private returns -2; this survey reports each independently so a
    // global-only item placement (hypothesis b) becomes visible.
    const uint16_t pairs[2][2] = {{0, 1}, {0xfffe, 0xfffe}};
    for (unsigned pi = 0; pi < 2; ++pi) {
        struct wlan_smem_partition p = {};
        int perr = wlan_smem_partition(&io, pairs[pi][0], pairs[pi][1], &p);
        IOLog("PICASSO_WLAN diag-survey partition[%u] host=%u remote=%u err=%d "
              "off=%x size=%x uncached=%x cached=%x line=%x\n",
              pi, pairs[pi][0], pairs[pi][1], perr,
              p.offset, p.size, p.uncached, p.cached, p.cacheline);
        if (perr != 0) continue;
        const uint16_t ids[2] = {WLAN_SMEM_IN, WLAN_SMEM_OUT};
        for (unsigned k = 0; k < 2; ++k) {
            struct wlan_smem_item item = {};
            int ferr = wlan_smem_find(&io, &p, ids[k], &item);
            IOLog("PICASSO_WLAN diag-survey partition[%u] item=%u find=%d "
                  "item-off=%x item-size=%x\n",
                  pi, ids[k], ferr, item.offset, item.size);
            // diag4: full header + every entry slot for items that exist.
            // This is the decisive evidence: an item can be present (find=0)
            // yet carry no "wlan" entry (valid=0 / different name / value=0),
            // which makes cbPollPeer return -2 and the peer wait time out.
            if (ferr == 0)
                diagDumpSmp2pItem(&io, pi == 0 ? "priv" : "glob", ids[k], &item);
        }
    }

    // diag4: enumerate ALL TOC partitions and locate any "wlan"-named SMP2P
    // entry, in case the peer item is authored into a host/remote pair that
    // neither cbPollPeer (0,1)/(0xfffe) nor the two probes above visit.
    diagScanTocForWlan(&io, "toc-all");

    // Execution-point evidence: read the first 32 bytes at e_entry from the
    // still-mapped carveout. If MPSS never executed, this is the raw ELF payload;
    // if it ran, cached instruction fetches leave the region unchanged on this
    // uncached mapping, so we print it purely as a fingerprint, not a verdict.
    if (ac->carveout.mapped && ac->planReady) {
        uint32_t entry = ac->plan.entry;
        if (entry >= WLAN_MODEM_BASE && (entry - WLAN_MODEM_BASE) + 32 <= WLAN_MODEM_BYTES) {
            const volatile uint8_t *p = ac->carveout.va + (entry - WLAN_MODEM_BASE);
            IOLog("PICASSO_WLAN diag-survey entry=%x bytes=%02x%02x%02x%02x %02x%02x%02x%02x "
                  "%02x%02x%02x%02x %02x%02x%02x%02x\n",
                  entry, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                  p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
        } else {
            IOLog("PICASSO_WLAN diag-survey entry=%x outside carveout (no dump)\n", entry);
        }
    } else {
        IOLog("PICASSO_WLAN diag-survey carveout.mapped=%d planReady=%d (no entry dump)\n",
              ac->carveout.mapped ? 1 : 0, ac->planReady ? 1 : 0);
    }

    // Gate state at timeout: poison/share snapshot is the upstream cause of the
    // ADSP/GPU -115/-40 seen downstream, printed here to close the causal loop.
    IOLog("PICASSO_WLAN diag-survey pasGate=%x (read-only)\n", picasso_pas_gate_snapshot());

    // CONTROLLED-WRITE liveness experiment (§9.21/§9.22), OFF by default.
    // Requires explicit boot-arg sheng-wlan-glink-liveness=1. Everything above
    // is read-only; this block writes ONLY the three fields pinned by review:
    // TX FIFO 480 head0={0x00010000,0}, desc478+4=8, IPCC 0x40800c=0x20000.
    // Aborts with zero writes if the ring is not in the all-zero initial state.
    unsigned liveness = 0;
    if (PE_parse_boot_argn("sheng-wlan-glink-liveness", &liveness, sizeof(liveness)) && liveness == 1) {
        DiagCtx dctx = {&ac->poll.smem, {}, false};
        dctx.ipccMapped = mapRegion(0x00408000, 0x1000, dctx.ipcc);
        IOLog("PICASSO_WLAN glink-liveness boot-arg=1 ipccMapped=%d\n", dctx.ipccMapped ? 1 : 0);
        if (dctx.ipccMapped) {
            picasso_mpss_glink_diag_io dio = {};
            dio.smem = io; // same read callback + context
            dio.smem_write32 = diagSmemWrite32;
            dio.barrier = diagBarrier;
            dio.ipcc_kick = diagIpccKick;
            dio.context = &dctx;
            picasso_mpss_glink_diag_result dro = {};
            int drc = picasso_mpss_glink_diag_write_version(&dio, &dro);
            IOLog("PICASSO_WLAN glink-liveness rc=%d error=%d wrote=%u "
                  "before=%08x/%08x/%08x/%08x desc=%u tx=%u rx=%u "
                  "txHead=%u afterHead=%u afterRxHead=%u kick=%08x peerResponded=%u\n",
                  drc, dro.error, (unsigned)dro.wrote,
                  dro.before[0], dro.before[1], dro.before[2], dro.before[3],
                  dro.descId, dro.txId, dro.rxId,
                  dro.txHeadWritten, dro.afterHead, dro.afterRxHead,
                  dro.kickValue, (unsigned)dro.peerResponded);
        } else {
            IOLog("PICASSO_WLAN glink-liveness abort: IPCC map failed (no writes)\n");
        }
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
    IOLog("PICASSO_WLAN stage1 t=%llu diag=PAS-TRACE-v1 execute-enter rails=%u\n",
          (unsigned long long)nowMs(nullptr), cfg.rails);
    int error = picasso_wlan_modem_pil_execute(&io, &cfg, &state);
    IOLog("PICASSO_WLAN stage1 t=%llu diag=execute-exit rc=%d phase=%s\n",
          (unsigned long long)nowMs(nullptr), error, state.phase ? state.phase : "(null)");

    IOLog("PICASSO_WLAN stage1 t=%llu adapter phase=%s step=%u error=%d rc=%d pasCalls=%u railVotes=%u "
          "partsLoaded=%u peerPolls=%u authenticated=%u ready=%u leased=%u released=%u quarantined=%u\n",
          (unsigned long long)nowMs(nullptr),
          state.phase ? state.phase : "(null)", state.step, state.error, error,
          state.pasCalls, state.railVotes, state.partsLoaded, state.peerPolls,
          state.authenticated, state.ready, state.leased, state.released, state.quarantined);

    // READ-ONLY diagnostic survey on the peer-timeout path only. No control-flow
    // change: this runs after execute() has already returned -60 and stop() has
    // completed; it neither retries nor alters the result. It dumps the two
    // SMP2P partitions and e_entry bytes to settle root cause (a) vs (b).
    if (error == -60) diagSurveyPeer(ctx, nowMs(nullptr), error);

    unmapPhys(ctx->carveout);
    unmapPhys(ctx->metadataSlot);
    IOFree(ctx, sizeof(AdapterCtx));
    return error;
}
