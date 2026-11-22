/*
 * r1-state.c: checks bladerf_set_rfic_1r1t()'s register state on the bladeRF 2.0 micro
 * (branch 1r1t). Receive only: nothing is enabled or streamed.
 *   1. a fresh open: 1R1T off, AD9361 0x002/0x003 bit 7 (TX2/RX2) on
 *   2. OVERSAMPLE, 61.44 Msps, 1R1T on: bit 7 off in both, bit 6 on
 *   3. a sample rate change to 122.88 Msps (libbladeRF rewrites the registers): still 1R1T
 *   4. 1R1T off: bit 7 back on (0xD4 / 0xC0 in OVERSAMPLE)
 *   5. 1R1T on, close, open again: off after the open, both channels on
 *      (before that, 6. in 1R1T mode enabling RX2 or TX2 fails, RX1 works)
 * Exit status 0 if every check passes.
 */
#include <stdbool.h>
#include <stdio.h>
#include <libbladeRF.h>

static int fails;

#define CHECK(call)                                                             \
    do {                                                                        \
        int s_ = (call);                                                        \
        if (s_ != 0) {                                                          \
            fprintf(stderr, "ERROR: %s: %s\n", #call, bladerf_strerror(s_));    \
            return 2;                                                           \
        }                                                                       \
    } while (0)

static int expect(struct bladerf *dev, const char *step, bool r1)
{
    uint8_t rx = 0, tx = 0;
    bool flag = !r1;
    CHECK(bladerf_get_rfic_register(dev, 0x003, &rx));
    CHECK(bladerf_get_rfic_register(dev, 0x002, &tx));
    CHECK(bladerf_get_rfic_1r1t(dev, &flag));
    bool ok = flag == r1 && ((rx & 0xc0) == (r1 ? 0x40 : 0xc0)) &&
              ((tx & 0xc0) == (r1 ? 0x40 : 0xc0));
    printf("%s: 0x003 = 0x%02x, 0x002 = 0x%02x, bladerf_get_rfic_1r1t = %d: %s\n", step, rx,
           tx, flag, ok ? "as expected" : "WRONG");
    fails += !ok;
    return 0;
}

int main(void)
{
    struct bladerf *dev;
    bladerf_sample_rate actual;

    CHECK(bladerf_open(&dev, NULL));
    if (expect(dev, "1. fresh open", false)) return 2;
    CHECK(bladerf_enable_feature(dev, BLADERF_FEATURE_OVERSAMPLE, true));
    CHECK(bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), 61440000, &actual));
    CHECK(bladerf_set_rfic_1r1t(dev, true));
    if (expect(dev, "2. OVERSAMPLE 61.44 Msps, 1R1T on", true)) return 2;
    CHECK(bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), 122880000, &actual));
    if (expect(dev, "3. after a rate change to 122.88 Msps", true)) return 2;
    CHECK(bladerf_set_rfic_1r1t(dev, false));
    if (expect(dev, "4. 1R1T off", false)) return 2;
    CHECK(bladerf_set_rfic_1r1t(dev, true));
    {
        /* 6. in 1R1T mode RX2 and TX2 cannot be enabled; RX1 can */
        int s_rx2 = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(1), true);
        int s_tx2 = bladerf_enable_module(dev, BLADERF_CHANNEL_TX(1), true);
        int s_rx1 = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true);
        bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
        bool ok = s_rx2 == BLADERF_ERR_UNSUPPORTED && s_tx2 == BLADERF_ERR_UNSUPPORTED && s_rx1 == 0;
        printf("6. enable RX2, TX2, RX1 in 1R1T mode: %d, %d, %d: %s\n", s_rx2, s_tx2, s_rx1,
               ok ? "as expected" : "WRONG");
        fails += !ok;
    }
    bladerf_close(dev);
    CHECK(bladerf_open(&dev, NULL));
    if (expect(dev, "5. open after a session that left 1R1T on", false)) return 2;
    bladerf_close(dev);
    printf("RESULT: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
