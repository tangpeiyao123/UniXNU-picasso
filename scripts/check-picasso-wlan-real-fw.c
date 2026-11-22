// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real-firmware regression for the NativeWlan MBN/BDF parser. Reads firmware read-only from the path in argv[1]
// (the device's modem-image directory); never embeds or distributes a vendor blob. Exits non-zero on any
// structural regression. This is a host structural check only — it does NOT authenticate firmware; a signed image
// is expected to report auth=WLAN_AUTH_TZ_PAS and verify_hashes=WLAN_ERR_NEEDS_TZ_AUTH (TrustZone does the auth).
#include "WlanFirmware.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static uint8_t *load(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
    long z = ftell(f); if (z < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    uint8_t *d = malloc(z ? (size_t)z : 1);
    if (!d || (z && fread(d, 1, (size_t)z, f) != (size_t)z)) { free(d); fclose(f); return NULL; }
    fclose(f); *n = (size_t)z; return d;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : NULL;
    if (!dir || !*dir) { printf("SKIP real-fw: pass the firmware directory as argv[1]\n"); return 0; }
    char path[1024];
    int rc = 0;

    // modem.mdt — the native pil_modem container on this board (35 program headers).
    snprintf(path, sizeof path, "%s/modem.mdt", dir);
    size_t mdt_sz = 0; uint8_t *mdt = load(path, &mdt_sz);
    CHECK(mdt);
    struct wlan_mbn h;
    CHECK(wlan_mbn_parse_header(&h, mdt, mdt_sz) == 0);
    CHECK(h.segments == 35 && h.machine == 164);
    const uint8_t *parts[WLAN_MBN_MAX_SEGMENTS] = {0};
    size_t psz[WLAN_MBN_MAX_SEGMENTS] = {0};
    unsigned present = 0;
    for (unsigned i = 0; i < h.segments; ++i) {
        snprintf(path, sizeof path, "%s/modem.b%02u", dir, i);
        parts[i] = load(path, &psz[i]);
        if (parts[i]) ++present;
    }
    struct wlan_mbn split;
    CHECK(present == 32);   // PH 18/25/34 have filesz=0, so 32 of 35 split parts exist
    CHECK(wlan_mbn_parse_split(&split, mdt, mdt_sz, parts, psz, h.segments) == 0);
    printf("PASS modem.mdt: 35 segments, machine=164, 32 split parts, px_split=0\n");
    for (unsigned i = 0; i < h.segments; ++i) free((void *)parts[i]);
    free(mdt);

    // wlanmdsp.mbn — signed DSP image: structure must parse, but auth must be delegated to TrustZone.
    snprintf(path, sizeof path, "%s/wlanmdsp.mbn", dir);
    size_t w_sz = 0; uint8_t *w = load(path, &w_sz);
    CHECK(w);
    struct wlan_mbn wm;
    CHECK(wlan_mbn_parse(&wm, w, w_sz) == 0);
    CHECK(wm.machine == 164 && wm.hash_index == WLAN_MBN_NO_HASH);
    CHECK(wlan_mbn_auth(&wm) == WLAN_AUTH_TZ_PAS);
    CHECK(wlan_mbn_verify_hashes(&wm, w, w_sz) == WLAN_ERR_NEEDS_TZ_AUTH);   // NEVER 0 (no fail-open)
    printf("PASS wlanmdsp.mbn: signed, auth=tz_pas, verify=needs_tz_auth (structure only, NOT authenticated)\n");
    free(w);

    // bdwlan — board data outer container.
    const char *bdfs[] = {"bdwlan.bin", "bdwlan.b4c", "bdwlan.102"};
    for (unsigned i = 0; i < 3; ++i) {
        snprintf(path, sizeof path, "%s/%s", dir, bdfs[i]);
        size_t n = 0; uint8_t *b = load(path, &n);
        CHECK(b);
        struct wlan_bdf out;
        CHECK(wlan_bdf_parse(&out, b, n) == 0 && out.kind == WLAN_BDF_RAW);
        free(b);
    }
    printf("PASS bdwlan.bin/b4c/102: RAW outer container accepted (internal BDF layout is NDA)\n");
    (void)rc;
    return 0;
}
