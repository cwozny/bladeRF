/*
 * Hardware test of lib/tx-cap.c (needs HWTEST_TX=1 cabling: it enables both
 * TX channels, without streaming, at gains up to the cap).
 *
 *   tx-cap-test             (run with the cap preloaded; exit 0 = cap holds)
 *   tx-cap-test --selftest  (no device: is the cap preloaded?)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <libbladeRF.h>

#define CAP 60

static int fails;

static void expect(const char *what, int ok)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
    fails += !ok;
}

static bladerf_gain gain_of(struct bladerf *dev, bladerf_channel ch)
{
    bladerf_gain g = 999;
    if (bladerf_get_gain(dev, ch, &g) != 0) {
        return 999;
    }
    return g;
}

int main(int argc, char *argv[])
{
    int *cap = dlsym(RTLD_DEFAULT, "hwtest_tx_cap_db");
    struct bladerf *dev;
    int k, status;

    if (cap == NULL || *cap != CAP) {
        printf("hwtest TX cap NOT loaded\n");
        return 1;
    }
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
        printf("hwtest TX cap loaded (%d dB)\n", *cap);
        return 0;
    }
    if (bladerf_open(&dev, NULL) != 0) {
        printf("cannot open the bladeRF\n");
        return 1;
    }
    for (k = 0; k < 2; k++) {
        bladerf_channel ch = BLADERF_CHANNEL_TX(k);
        char buf[80];

        bladerf_enable_module(dev, ch, false);
        bladerf_set_gain(dev, ch, 66);
        snprintf(buf, sizeof buf, "TX%d disabled: gain 66 passes through", k + 1);
        expect(buf, gain_of(dev, ch) == 66);

        status = bladerf_enable_module(dev, ch, true);
        snprintf(buf, sizeof buf, "TX%d enable at stored gain 66: enabled, gain <= %d", k + 1, CAP);
        expect(buf, status == 0 && gain_of(dev, ch) <= CAP);

        bladerf_set_gain(dev, ch, 66);
        snprintf(buf, sizeof buf, "TX%d enabled: set_gain 66 -> gain <= %d", k + 1, CAP);
        expect(buf, gain_of(dev, ch) <= CAP);

        bladerf_set_gain_stage(dev, ch, "dsa", 0);
        snprintf(buf, sizeof buf, "TX%d enabled: dsa 0 -> gain <= %d", k + 1, CAP);
        expect(buf, gain_of(dev, ch) <= CAP);

        snprintf(buf, sizeof buf, "TX%d enabled: unknown gain stage refused", k + 1);
        expect(buf, bladerf_set_gain_stage(dev, ch, "bogus", 0) == BLADERF_ERR_PERMISSION);

        bladerf_set_gain(dev, ch, 30);
        snprintf(buf, sizeof buf, "TX%d enabled: set_gain 30 -> 30", k + 1);
        expect(buf, gain_of(dev, ch) == 30);

        bladerf_enable_module(dev, ch, false);
    }
    bladerf_close(dev);
    printf("%d failure(s)\n", fails);
    return fails != 0;
}
