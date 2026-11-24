// Regression for the stage-1 adapter's SMP2P pid argument order (C1).
// Synthesizes the on-device SMP2P header authored by MPSS(1) toward APPS(0):
// WlanSmem.h:93 checks h+8 == local and h+10 == remote, so item 435 (IN)
// must be parsed with (local=1, remote=0) exactly as WlanProbe.cpp:109 does.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern "C" {
#include "WlanSmem.h"
}

static uint8_t smem[0x1000 + 64];

static int syntheticRead(void *ctx, uint32_t off, void *out, uint32_t len) {
    (void)ctx;
    if (off + len > sizeof smem) return -1;
    memcpy(out, smem + off, len);
    return 0;
}

int main() {
    memset(smem, 0, sizeof smem);
    uint8_t *h = smem + 0x1000;
    // '$PMS' magic, version 1, local=1 (MPSS), remote=0 (APPS), total=valid=1.
    h[0] = 0x24; h[1] = 0x53; h[2] = 0x4d; h[3] = 0x50;
    h[4] = 1;
    h[8] = 1; h[9] = 0;
    h[10] = 0; h[11] = 0;
    h[12] = 1; h[13] = 0;
    h[14] = 1; h[15] = 0;
    uint8_t *e = h + 20;
    memcpy(e, "wlan", 4);
    e[4] = 0;
    e[16] = 0x5a;  // raw value = 0x5a

    struct wlan_smem_io io = {nullptr, syntheticRead};
    struct wlan_smem_item item = {0x1000, 60};
    uint32_t value = 0;

    // (local=1, remote=0) matches the header -> parsed, raw value exposed.
    assert(wlan_smp2p_wlan(&io, &item, 1, 0, &value) == 0);
    assert(value == 0x5a);
    // (local=0, remote=1) as the reverted DTB-order instruction claimed ->
    // -22 hard failure. Proof that copying it would kill stage 1 on device.
    assert(wlan_smp2p_wlan(&io, &item, 0, 1, &value) == -22);

    puts("PASS stage1 SMP2P pid order (local=1/remote=0 for item 435)");
    return 0;
}
