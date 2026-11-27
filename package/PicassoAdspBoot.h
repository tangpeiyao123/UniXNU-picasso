// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_ADSP_BOOT_H
#define PICASSO_ADSP_BOOT_H
#include <stdint.h>
#include <stddef.h>
#include "PicassoSmp2p.h"
struct PicassoAdspEvents {
    void *context;
    int (*poll)(void *, PicassoSmp2pEvent *);
    int (*snapshot)(void *); // Bounded crash/GLINK observations after startup/failure.
    bool (*fast)(void *); // Active PCM / bounded RPC burst; never advances deadline ticks.
    void (*service)(void *); // Only after PAS AUTH + fresh ready, never preflight/fatal.
};
struct PicassoAdspImage;
extern "C" PicassoAdspImage *picasso_adsp_image_open(int *error);
extern "C" const void *picasso_adsp_image_metadata(PicassoAdspImage *);
extern "C" int picasso_adsp_image_load(PicassoAdspImage *, volatile uint8_t *, size_t);
extern "C" void picasso_adsp_image_close(PicassoAdspImage *);
extern "C" void picasso_adsp_boot(const PicassoAdspEvents *);
extern "C" int picasso_aop_load_state(bool on);
// Modem image on the shared AOP QMP session. `name: modem` is the upstream
// Q6V5 AOSS load_state resource name (commit c1fe10d2); failure is fatal-by-design.
extern "C" int picasso_aop_load_state_modem(bool on);
extern "C" int picasso_aop_finish(void);
#endif
