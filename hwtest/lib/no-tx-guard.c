/*
 * No-TX guard for the bladeRF hardware test harness.
 *
 * Preloaded (LD_PRELOAD) into every program the harness runs unless HWTEST_TX=1
 * declares that every bladeRF TX port is cabled through an attenuator or
 * terminated (see ../README.md). It refuses bladerf_enable_module() on TX
 * channels, which is the only way to make a bladeRF 2.0 micro transmit: libbladeRF
 * keeps a disabled TX channel muted with its RF switch off, also while it is
 * tuned or calibrated. With antennas on the TX ports nothing can then be
 * radiated, whatever a test program or bladeRF-cli command tries to do.
 *
 * Disabling channels and everything RX are passed through unchanged.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <libbladeRF.h>

typedef int (*enable_module_fn)(struct bladerf *, bladerf_channel, bool);

int bladerf_enable_module(struct bladerf *dev, bladerf_channel ch, bool enable)
{
    static enable_module_fn real;

    if (enable && BLADERF_CHANNEL_IS_TX(ch)) {
        fprintf(stderr,
                "hwtest no-TX guard: refused to enable TX channel %d "
                "(receive-only setup; see hwtest/README.md)\n", (int)ch);
        return BLADERF_ERR_PERMISSION;
    }

    if (real == NULL) {
        real = (enable_module_fn)dlsym(RTLD_NEXT, "bladerf_enable_module");
        if (real == NULL) {
            fprintf(stderr, "hwtest no-TX guard: libbladeRF not found: %s\n",
                    dlerror());
            abort();
        }
    }
    return real(dev, ch, enable);
}
