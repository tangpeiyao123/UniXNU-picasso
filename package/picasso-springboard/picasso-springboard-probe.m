// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Picasso's rectangular status bar uses UIKit's native Pad_ForcedNoCell
// provider (selected in the Data seed), with no carrier and time at the left.
// Reserve another 14.5 mm at the right for the panel's camera area. Hook only
// this provider's metric; UIKit still owns item placement and animations.
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <dispatch/dispatch.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

// Read-only diagnostics also use a nonexistent marker path. The bounded namei
// probe records the attempt before lookup, so no file needs to be created and
// no sandbox/AMFI policy is relaxed when a sysctl or console open is denied.
static void picasso_ack(const char *name)
{
    int value = 0;
    size_t size = sizeof(value);
    // Height and baseline have marker-only acknowledgements; the original
    // kernel ABI has one generic metrics-used sysctl for edgeInsets.
    if (strcmp(name, "kern.picasso_metrics_height_used") &&
        strcmp(name, "kern.picasso_metrics_baseline_used"))
        (void)sysctlbyname(name, &value, &size, NULL, 0);
    static const char prefix[] = "kern.picasso_";
    if (strncmp(name, prefix, sizeof(prefix) - 1)) return;
    char marker[160];
    int length = snprintf(marker, sizeof(marker), "/private/var/picasso/picasso-marker-%s",
                          name + sizeof(prefix) - 1);
    if (length <= 0 || (size_t)length >= sizeof(marker)) return;
    int fd = open(marker, O_RDONLY);
    if (fd >= 0) close(fd);
}

#include "picasso-astc-guard.m"
#include "picasso-statusbar-metrics.m"

// 屏幕事实一次性打全。键盘右侧被裁掉 45 px 这件事换了机型（D22→iPhone13,1）、
// 换了 display-scale（3→2）都没变化，说明它既不看 hw.machine 也不看逻辑宽度。
// 继续猜没有意义，直接把 UIKit 眼里的四个数打出来：
//   bounds       —— UIKit 布局用的逻辑尺寸（pt）
//   nativeBounds —— 原生像素尺寸
//   scale/nativeScale —— 两者之间的换算
// 再带上 hw.machine / hw.model / idiom，就能确定 1125 这个数到底从哪来。
static void picasso_screen_report(void)
{
    UIScreen *screen = [UIScreen mainScreen];
    if (!screen) return;
    CGRect bounds = screen.bounds;
    CGRect native = screen.nativeBounds;
    char machine[64] = {0};
    char model[64] = {0};
    size_t length = sizeof(machine) - 1;
    (void)sysctlbyname("hw.machine", machine, &length, NULL, 0);
    length = sizeof(model) - 1;
    (void)sysctlbyname("hw.model", model, &length, NULL, 0);
    char line[320];
    int wrote = snprintf(line, sizeof(line),
        "PICASSO_SCREEN bounds=%.0fx%.0f native=%.0fx%.0f scale=%.2f nscale=%.2f idiom=%ld machine=%s model=%s\n",
        bounds.size.width, bounds.size.height,
        native.size.width, native.size.height,
        screen.scale, screen.nativeScale,
        (long)[[UIDevice currentDevice] userInterfaceIdiom],
        machine, model);
    if (wrote <= 0 || (size_t)wrote >= sizeof(line)) return;
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        (void)write(fd, line, (size_t)wrote);
        close(fd);
    }
}

static void picasso_statusbar_start(void)
{
    BOOL installed = picasso_install_statusbar();
    picasso_ack(installed ? "kern.picasso_metrics_installed" : "kern.picasso_metrics_unavailable");
    const char *line = installed ? "PICASSO_SBINJECT statusbar metrics installed (left 8.91 pt, down 11.42 pt, battery shift 14.5 mm)\n"
                                 : "PICASSO_SBINJECT statusbar metric unavailable\n";
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        (void)write(fd, line, strlen(line));
        close(fd);
    }
    if (!installed) {
        // UIKit is a linked dependency, but a missing runtime class must not
        // turn the sole constructor attempt into a permanent silent failure.
        // One main-queue retry, no polling and no metric calls faking "used".
        dispatch_async(dispatch_get_main_queue(), ^{
            BOOL ready = picasso_install_statusbar();
            picasso_ack(ready ? "kern.picasso_metrics_installed" : "kern.picasso_metrics_unavailable");
        });
    }
}

__attribute__((constructor))
static void picasso_springboard_probe(void)
{
    // Record entry before either installation path can fail.
    picasso_ack("kern.picasso_inject_ack");
    @autoreleasepool {
        picasso_screen_report();
        picasso_statusbar_start();
        picasso_install_astc_guard();
    }
}
