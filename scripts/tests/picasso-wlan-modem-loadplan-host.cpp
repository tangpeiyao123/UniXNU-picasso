// SPDX-License-Identifier: BSD-2-Clause-Patent
// Verify the parameterised pil_modem split-ELF load plan against the REAL
// modem.mdt/modem.bXX directory (read-only; sizes/SHA only, no blob embedded).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "WlanModemLoadPlan.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned char mdt[WLAN_MODEM_METADATA_BYTES];
static unsigned char chunk[1 << 20];

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: loadplan <firmware-dir>\n"); return 2; }
    const char *dir = argv[1];
    char path[512];
    FILE *f;

    snprintf(path, sizeof path, "%s/modem.mdt", dir);
    f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open failed: %s\n", path); return 3; }
    CHECK(fread(mdt, 1, sizeof mdt, f) == sizeof mdt);
    fclose(f);

    // pieces[i] = p_filesz of PH i, read from the ELF program headers; a part
    // file exists iff filesz > 0. Derive them from the real headers, not blame.
    uint32_t pieces[WLAN_MODEM_HEADERS] = {0};
    for (unsigned i = 0; i < WLAN_MODEM_HEADERS; ++i) {
        const unsigned char *p = mdt + 52 + i * 32;
        pieces[i] = wlan_modem_u32(p + 16);
        if (pieces[i]) {
            snprintf(path, sizeof path, "%s/modem.b%02u", dir, i);
            f = fopen(path, "rb");
            CHECK(f != NULL);
            // Stream in bounded chunks: several parts exceed 1 MiB (up to ~39 MiB).
            size_t got = 0;
            while (got < pieces[i]) {
                size_t want = pieces[i] - got;
                if (want > sizeof chunk) want = sizeof chunk;
                size_t n = fread(chunk, 1, want, f);
                CHECK(n == want);
                got += n;
            }
            fclose(f);
        }
    }

    struct wlan_modem_load_plan plan;
    int rc = wlan_modem_load_plan(mdt, sizeof mdt, pieces, WLAN_MODEM_HEADERS, &plan);
    CHECK(rc == 0);
    CHECK(plan.count == WLAN_MODEM_SEGMENTS);           // 33 PT_LOAD.
    CHECK(plan.entry == 0x8c000000U);
    CHECK(plan.fileBytes > 0 && plan.zeroBytes > 0);
    CHECK(strcmp(plan.phase, "planned") == 0);
    const unsigned ok_count = plan.count, ok_entry = plan.entry;
    const unsigned ok_file = plan.fileBytes, ok_zero = plan.zeroBytes;

    // Negative controls: tamper one piece size and truncate the metadata.
    // Each rejected call zero-initialises plan, so snapshot the good values above.
    uint32_t bad[WLAN_MODEM_HEADERS];
    memcpy(bad, pieces, sizeof pieces);
    bad[2] += 1;
    CHECK(wlan_modem_load_plan(mdt, sizeof mdt, bad, WLAN_MODEM_HEADERS, &plan) == -22);
    CHECK(wlan_modem_load_plan(mdt, sizeof mdt - 1, pieces, WLAN_MODEM_HEADERS, &plan) == -22);

    printf("PASS real pil_modem load plan: headers=%u segments=%u entry=%08x file=%u zero=%u ceil=%08x\n",
           (unsigned)WLAN_MODEM_HEADERS, ok_count, ok_entry, ok_file, ok_zero,
           (unsigned)(WLAN_MODEM_BASE + WLAN_MODEM_BYTES));
    return 0;
}
