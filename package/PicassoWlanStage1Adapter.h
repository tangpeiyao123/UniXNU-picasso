// SPDX-License-Identifier: BSD-2-Clause-Patent
// Stage-1 REAL-DEVICE adapter for Picasso (SM7250) pil_modem / PAS id 4.
//
// This is a MANUAL diagnostic entry point. It is deliberately NOT wired into
// package/PicassoPr13.cpp probes() -- the boot path is unchanged. A diagnostic
// channel declares these symbols weak_import and calls them on demand.
//
// Hard boundaries (see STAGE1-ADAPTER-DESIGN.md v2):
//  - no flashing / no partition writes; firmware is read through FirmwareWindow.h
//    only (FirmwareRead.h has strong definitions and is never included here);
//  - the WLFW QMI service number is reference-only and is NEVER hardcoded
//    here (on-device discovery required);
//  - no private firmware bytes are stored in this tree;
//  - every device-validated assumption is a fail-stop gate, never a guessed value.
#ifndef PICASSO_WLAN_STAGE1_ADAPTER_H
#define PICASSO_WLAN_STAGE1_ADAPTER_H

#ifdef __cplusplus
extern "C" {
#endif

// Read-only gate: PAS_IS_SUPPORTED (SVC_PIL 0x02 / cmd 0x07 / pas id 4).
// Reads no firmware memory and writes no device memory. Returns the raw SMC
// reply a0 (0 = the PIL service accepts PAS id 4). Non-zero means STOP: do not
// attempt any PAS cmd1/2/5 writes. Implemented in Adreno620Cp.cpp with exactly
// the same SMC encoding as the existing pas-supported(13)/(1) probes.
int picasso_wlan_stage1_probe_pas4(void);

// Manual one-shot stage-1 transaction. Builds the 10 WlanModemPilIo callbacks
// plus the default (PAS 4 / modem carveout) config, calls
// picasso_wlan_modem_pil_execute(), logs phase/step/error/pasCalls/
// authenticated/ready/leased, and returns the transaction's errno.
// This function may cause TrustZone PAS cmd1/2/5 writes on a real device;
// run it only from the diagnostic path with a console and recoverable boot.
int picasso_wlan_stage1_run(void);

#ifdef __cplusplus
}
#endif
#endif
