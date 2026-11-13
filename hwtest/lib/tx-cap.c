/*
 * TX power cap for the bladeRF hardware test harness.
 *
 * Preloaded (LD_PRELOAD) into every program the harness runs when HWTEST_TX=1
 * (see ../README.md and common.sh), in place of the no-TX guard. With 20 dB
 * between bladeRF TX2 and the B200mini's RX2, the bladeRF at full power
 * (+8 dBm, overall TX gain 66 dB) would put -12 dBm into a B200mini input
 * rated -15 dBm. This keeps every enabled bladeRF TX channel at an overall
 * gain of at most TX_CAP_DB (60 dB ~= 0 dBm, so -20 dBm at the B200mini):
 *
 *   - bladerf_enable_module(TX, true) first lowers the gain of BOTH TX
 *     channels to the cap if needed (with a TX_X2 stream layout libbladeRF
 *     enables both channels whichever one is passed; a disabled bladeRF 2.0
 *     TX channel is muted and libbladeRF applies the stored gain on enable)
 *     and refuses if it cannot;
 *   - bladerf_set_gain() on an enabled TX channel is clamped to the cap;
 *   - bladerf_set_gain_stage() on an enabled TX channel: "dsa" is clamped to
 *     TX_CAP_DB - 66, any other stage is refused.
 *
 * Gain changes on a disabled TX channel pass through unchanged (test_ctrl's
 * gain test reads them back); they are capped when the channel is enabled.
 * On the bladeRF 2.0 the overall TX gain is 66 dB + the DSA setting (-89.75 to
 * 0 dB) at every frequency (fpga_common/include/bladerf2_common.h).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libbladeRF.h>

#define TX_CAP_DB 60
#define TX_GAIN_OFFSET_DB 66

/* Lets common.sh check that the cap is loaded */
int hwtest_tx_cap_db = TX_CAP_DB;

static bool tx_enabled[2];      /* BLADERF_CHANNEL_TX(0), TX(1) */

static void *real_sym(const char *name)
{
    void *p = dlsym(RTLD_NEXT, name);
    if (p == NULL) {
        fprintf(stderr, "hwtest TX cap: %s not found: %s\n", name, dlerror());
        abort();
    }
    return p;
}

static int tx_index(bladerf_channel ch)
{
    int i = (int)(ch >> 1);
    return (BLADERF_CHANNEL_IS_TX(ch) && i >= 0 && i < 2) ? i : -1;
}

int bladerf_set_gain(struct bladerf *dev, bladerf_channel ch, bladerf_gain gain)
{
    static int (*real)(struct bladerf *, bladerf_channel, bladerf_gain);
    int i = tx_index(ch);

    if (real == NULL) {
        real = real_sym("bladerf_set_gain");
    }
    if (i >= 0 && tx_enabled[i] && gain > TX_CAP_DB) {
        fprintf(stderr, "hwtest TX cap: TX gain %d dB lowered to %d dB\n",
                gain, TX_CAP_DB);
        gain = TX_CAP_DB;
    }
    return real(dev, ch, gain);
}

int bladerf_set_gain_stage(struct bladerf *dev, bladerf_channel ch,
                           const char *stage, bladerf_gain gain)
{
    static int (*real)(struct bladerf *, bladerf_channel, const char *,
                       bladerf_gain);
    int i = tx_index(ch);

    if (real == NULL) {
        real = real_sym("bladerf_set_gain_stage");
    }
    if (i >= 0 && tx_enabled[i]) {
        if (stage == NULL || strcmp(stage, "dsa") != 0) {
            fprintf(stderr, "hwtest TX cap: refused TX gain stage %s\n",
                    stage != NULL ? stage : "(null)");
            return BLADERF_ERR_PERMISSION;
        }
        if (gain > TX_CAP_DB - TX_GAIN_OFFSET_DB) {
            fprintf(stderr, "hwtest TX cap: TX dsa %d dB lowered to %d dB\n",
                    gain, TX_CAP_DB - TX_GAIN_OFFSET_DB);
            gain = TX_CAP_DB - TX_GAIN_OFFSET_DB;
        }
    }
    return real(dev, ch, stage, gain);
}

int bladerf_enable_module(struct bladerf *dev, bladerf_channel ch, bool enable)
{
    static int (*real)(struct bladerf *, bladerf_channel, bool);
    static int (*real_set)(struct bladerf *, bladerf_channel, bladerf_gain);
    static int (*real_get)(struct bladerf *, bladerf_channel, bladerf_gain *);
    int i = tx_index(ch), status, k;
    bladerf_gain gain;
    bool was[2];

    if (real == NULL) {
        real     = real_sym("bladerf_enable_module");
        real_set = real_sym("bladerf_set_gain");
        real_get = real_sym("bladerf_get_gain");
    }
    if (i < 0 || !enable) {
        status = real(dev, ch, enable);
        if (i >= 0 && status == 0) {
            /* conservative: keep clamping a channel libbladeRF may still
             * have enabled through a TX_X2 layout */
            tx_enabled[i] = false;
        }
        return status;
    }

    for (k = 0; k < 2; k++) {
        bladerf_channel c = BLADERF_CHANNEL_TX(k);

        if (real_get(dev, c, &gain) != 0) {
            fprintf(stderr, "hwtest TX cap: cannot read the TX gain; refused\n");
            return BLADERF_ERR_PERMISSION;
        }
        if (gain > TX_CAP_DB) {
            fprintf(stderr, "hwtest TX cap: TX%d gain %d dB lowered to %d dB\n",
                    k + 1, gain, TX_CAP_DB);
            if (real_set(dev, c, TX_CAP_DB) != 0 ||
                real_get(dev, c, &gain) != 0 || gain > TX_CAP_DB) {
                fprintf(stderr, "hwtest TX cap: cannot lower the TX gain; refused\n");
                return BLADERF_ERR_PERMISSION;
            }
        }
    }
    /* clamp gain changes from now on, on both channels (see above) */
    was[0] = tx_enabled[0];
    was[1] = tx_enabled[1];
    tx_enabled[0] = tx_enabled[1] = true;
    status = real(dev, ch, enable);
    if (status != 0) {
        tx_enabled[0] = was[0];
        tx_enabled[1] = was[1];
    }
    return status;
}

void bladerf_close(struct bladerf *dev)
{
    static void (*real)(struct bladerf *);

    if (real == NULL) {
        real = real_sym("bladerf_close");
    }
    real(dev);
    tx_enabled[0] = tx_enabled[1] = false;
}
