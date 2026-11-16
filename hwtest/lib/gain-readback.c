/*
 * gain-readback: sets RX1 gains at frequencies in each of the bladeRF 2.0's
 * RX gain bands and reads them back, in the tuning mode libbladeRF picks
 * (BLADERF_DEFAULT_TUNING_MODE). Above 1.3 GHz part of the range has a
 * negative full-stage value; in FPGA tuning mode libbladeRF read those back as
 * -2147483648 until 2c4fc532 ("sign-extend RX gain readbacks").
 *
 * RX only: RX1 is enabled (RX gain changes do not register while RX is
 * disabled) but nothing is streamed or transmitted.
 *
 *   gain-readback [device string]
 *
 * Exit status: 0 if every readback matched, 1 if not, 2 on an error.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <libbladeRF.h>

static const struct {
    bladerf_frequency frequency;
    int gain;
} steps[] = {
    { 420000000, -16 },  { 420000000, 0 },     { 420000000, 60 },
    { 2000000000, -15 }, { 2000000000, -12 },  { 2000000000, -11 },
    { 2000000000, 0 },   { 2000000000, 60 },   { 5000000000, -12 },
    { 5000000000, -5 },  { 5000000000, -3 },   { 5000000000, -2 },
    { 5000000000, 10 },  { 5000000000, 60 },
};

int main(int argc, char **argv)
{
    bladerf_channel const ch = BLADERF_CHANNEL_RX(0);
    struct bladerf *dev = NULL;
    bladerf_tuning_mode mode;
    int wrong = 0;
    size_t i;
    int status;

    status = bladerf_open(&dev, argc > 1 ? argv[1] : NULL);
    if (status != 0) {
        fprintf(stderr, "bladerf_open: %s\n", bladerf_strerror(status));
        return 2;
    }

    status = bladerf_get_tuning_mode(dev, &mode);
    printf("tuning mode: %s\n",
           status != 0 ? "?" : mode == BLADERF_TUNING_MODE_FPGA ? "fpga" : "host");

    status = bladerf_set_gain_mode(dev, ch, BLADERF_GAIN_MGC);
    if (status == 0) {
        status = bladerf_enable_module(dev, ch, true);
    }
    if (status != 0) {
        fprintf(stderr, "setup: %s\n", bladerf_strerror(status));
        bladerf_close(dev);
        return 2;
    }

    for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        int readback = 0;

        status = bladerf_set_frequency(dev, ch, steps[i].frequency);
        if (status == 0) {
            status = bladerf_set_gain(dev, ch, steps[i].gain);
        }
        if (status == 0) {
            status = bladerf_get_gain(dev, ch, &readback);
        }
        if (status != 0) {
            fprintf(stderr, "%" PRIu64 " Hz, gain %d: %s\n",
                    (uint64_t)steps[i].frequency, steps[i].gain,
                    bladerf_strerror(status));
            break;
        }

        printf("%10" PRIu64 " Hz  set %4d  read %11d  %s\n",
               (uint64_t)steps[i].frequency, steps[i].gain, readback,
               readback == steps[i].gain ? "ok" : "WRONG");
        wrong += (readback != steps[i].gain);
    }

    bladerf_enable_module(dev, ch, false);
    bladerf_close(dev);

    if (status != 0) {
        return 2;
    }
    printf("%d of %zu readbacks wrong\n", wrong, sizeof(steps) / sizeof(steps[0]));
    return wrong ? 1 : 0;
}
