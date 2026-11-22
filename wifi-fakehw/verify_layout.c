/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Standalone C verifier: compiles with ANY C11 compiler, no XNU SDK, no device.
 * If apple80211_layout.h's _Static_asserts agree with the compiler's ABI, this
 * prints the sizes and exits 0. Use in CI to complement the Python host check.
 */
#include <stdio.h>
#include "apple80211_layout.h"
int main(void) {
    printf("assoc=%zu link=%zu channels=%zu channels_info=%zu scan_req=%zu scan_result=%zu\n",
        sizeof(struct apple80211_assoc), sizeof(struct apple80211_link_changed),
        sizeof(struct apple80211_channels), sizeof(struct apple80211_channels_info),
        sizeof(struct apple80211_scan_request), sizeof(struct apple80211_scan_result));
    printf("SIOCSA80211=%#x SIOCGA80211=%#x\n", SIOCSA80211, SIOCGA80211);
    return 0;
}
