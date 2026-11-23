// SPDX-License-Identifier: BSD-2-Clause-Patent
// Picasso pil_modem (MPSS) split ELF32 load plan. Independent implementation
// derived 2026-11-23 from the REAL /d/k30i-firmware/modem-image/modem.mdt
// (9420 bytes) read-only. Observed layout (little endian ELF32, e_machine=164
// EM_HEXAGON, e_phnum=35, e_entry=0x8c000000):
// NOTE (取证纠正 design doc 的两处错误): real PT_LOAD count is 33 (PH02..PH34),
// not 32; the filesz==0 segments are PH18/PH25/PH34 (three), so disk b18/b25/b34
// are all absent. Still, the plan's segment array size is headers-2 = 33.
//   PH00 type=0 off=0      file=1172 (=52+32*35) mem=0     flags=07000000
//   PH01 type=0 off=0x1000 file=8248            mem=12288 addr=9b800000 flags=02200000
//   PH02..PH34 : 33x PT_LOAD, paddr in [0x8c000000,0x9b800000), span = 0xf800000
//   p_filesz==0 segments: PH18, PH25, PH34 -> disk files b18/b25/b34 ABSENT.
// This is structurally identical to the ADSP split plan, so the validation
// order is the same; only the board constants differ. pieces[i] is the file
// size of the part backing PH i (0 where p_filesz==0). This plan neither
// authenticates firmware nor maps/writes the carveout. Pure C, host-testable.
#ifndef WLAN_MODEM_LOAD_PLAN_H
#define WLAN_MODEM_LOAD_PLAN_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define WLAN_MODEM_BASE           0x8c000000u
#define WLAN_MODEM_BYTES          0xf800000u
#define WLAN_MODEM_HEADERS        35u
#define WLAN_MODEM_METADATA_BYTES 9420u
#define WLAN_MODEM_SEGMENTS       (WLAN_MODEM_HEADERS - 2u)   /* 33 PT_LOAD */
#define WLAN_MODEM_MAX_FILE       (128u << 20)                 /* image ~= 85 MiB */

struct wlan_modem_segment {
    uint32_t index, fileOffset, memoryOffset, fileBytes, zeroBytes, flags;
};
struct wlan_modem_load_plan {
    const char *phase;
    uint32_t index, count, entry, fileBytes, zeroBytes, imageBytes, alignment;
    struct wlan_modem_segment segments[WLAN_MODEM_SEGMENTS];
};

static inline uint16_t wlan_modem_u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static inline uint32_t wlan_modem_u32(const uint8_t *p) {
    return (uint32_t)wlan_modem_u16(p) | ((uint32_t)wlan_modem_u16(p + 2) << 16);
}

static inline int wlan_modem_load_plan(const uint8_t *mdt, size_t size,
    const uint32_t *pieces, size_t pieceCount, struct wlan_modem_load_plan *plan) {
    if (!plan) return -22;
    {
#ifdef __cplusplus
        struct wlan_modem_load_plan empty{};
#else
        struct wlan_modem_load_plan empty = {0};
#endif
        *plan = empty;
    }
    plan->phase = "elf";
    plan->index = ~0u;
    if (!mdt || !pieces || size != WLAN_MODEM_METADATA_BYTES || pieceCount != WLAN_MODEM_HEADERS) return -22;
    // ELF header: class32/data1/version1, EM_HEXAGON, version 1, 52-byte ehdr,
    // 32-byte phdrs, no section table.
    if (wlan_modem_u32(mdt) != 0x464c457fU || mdt[4] != 1 || mdt[5] != 1 || mdt[6] != 1 ||
        wlan_modem_u16(mdt + 16) != 2 || wlan_modem_u16(mdt + 18) != 164 ||
        wlan_modem_u32(mdt + 20) != 1 ||
        wlan_modem_u32(mdt + 24) != WLAN_MODEM_BASE ||
        wlan_modem_u32(mdt + 28) != 52 || wlan_modem_u32(mdt + 32) != 0 ||
        wlan_modem_u16(mdt + 40) != 52 || wlan_modem_u16(mdt + 42) != 32 ||
        wlan_modem_u16(mdt + 44) != WLAN_MODEM_HEADERS) return -22;
    plan->entry = wlan_modem_u32(mdt + 24);
    uint32_t low = WLAN_MODEM_BYTES, high = 0;
    bool entryFound = false;
    for (unsigned i = 0; i < WLAN_MODEM_HEADERS; ++i) {
        plan->index = i;
        plan->phase = "file";
        const uint8_t *p = mdt + 52 + i * 32;
        uint32_t type = wlan_modem_u32(p), offset = wlan_modem_u32(p + 4);
        uint32_t address = wlan_modem_u32(p + 12);
        uint32_t file = wlan_modem_u32(p + 16), mem = wlan_modem_u32(p + 20);
        uint32_t flags = wlan_modem_u32(p + 24), align = wlan_modem_u32(p + 28);
        if (offset > WLAN_MODEM_MAX_FILE || file > WLAN_MODEM_MAX_FILE - offset || pieces[i] != file) return -22;
        // Non-zero file extents must not overlap inside the split image.
        for (unsigned j = 0; file && j < i; ++j) {
            const uint8_t *prev = mdt + 52 + j * 32;
            uint32_t start = wlan_modem_u32(prev + 4), bytes = wlan_modem_u32(prev + 16);
            if (bytes && offset < start + bytes && start < offset + file) return -22;
        }
        if (file && offset + file > plan->imageBytes) plan->imageBytes = offset + file;
        plan->phase = "metadata";
        if (i == 0) {
            if (type || offset || address || file != 52 + 32 * WLAN_MODEM_HEADERS || mem || flags != 0x07000000u) return -22;
            continue;
        }
        if (i == 1) {
            if (type || offset != 4096 || file != size - pieces[0] || mem != 12288 ||
                address != WLAN_MODEM_BASE + WLAN_MODEM_BYTES || flags != 0x02200000u) return -22;
            continue;
        }
        plan->phase = "memory";
        if (type != 1 || (flags & ~7u) != 0x08000000u ||
            !mem || file > mem || address < WLAN_MODEM_BASE || address >= WLAN_MODEM_BASE + WLAN_MODEM_BYTES ||
            mem > WLAN_MODEM_BASE + WLAN_MODEM_BYTES - address || (align && (align & (align - 1)))) return -22;
        uint32_t relative = address - WLAN_MODEM_BASE;
        plan->phase = "overlap";
        for (unsigned j = 0; j < plan->count; ++j) {
            const struct wlan_modem_segment *s = &plan->segments[j];
            if (relative < s->memoryOffset + s->fileBytes + s->zeroBytes &&
                s->memoryOffset < relative + mem) return -22;
        }
        // Stock uses the lowest segment's p_align for relocation.
        if (relative < low) { low = relative; plan->alignment = align; }
        if (relative + mem > high) high = relative + mem;
        if (plan->entry >= address && plan->entry - address < mem && (flags & 1)) entryFound = true;
        {
            struct wlan_modem_segment *seg = &plan->segments[plan->count++];
            seg->index = i; seg->fileOffset = offset; seg->memoryOffset = relative;
            seg->fileBytes = file; seg->zeroBytes = mem - file; seg->flags = flags;
        }
        plan->fileBytes += file;
        plan->zeroBytes += mem - file;
    }
    plan->phase = "carveout";
    plan->index = ~0u;
    if (low || high != WLAN_MODEM_BYTES || !entryFound || plan->count != WLAN_MODEM_SEGMENTS ||
        (plan->alignment > 1 && (WLAN_MODEM_BASE & (plan->alignment - 1)))) return -22;
    plan->phase = "planned";
    return 0;
}
#endif
