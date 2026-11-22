/*
 * test_1r1t_state: checks how libbladeRF chooses the RFIC digital interface
 * mode (bladerf_set_rfic_1r1t_mode()) on a bladeRF 2.0 micro.
 *
 * Bit 7 of the AD9361 registers 0x003 (RX2) and 0x002 (TX2) is off in 1R1T
 * mode and on in 2R2T mode, and bladerf_get_rfic_1r1t() must agree. RX2
 * streams from the FPGA's counter for a moment; nothing is transmitted (the
 * TX2 enable attempts must be refused).
 *
 * With an FPGA image older than v0.17.0, the test checks that AUTO stays in
 * 2R2T mode and that ON is refused.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <libbladeRF.h>
#include "log.h"
#include "test_common.h"

#define RATE_OVERSAMPLE 122880000
#define RATE_DEFAULT 61440000

static unsigned int failures = 0;

void print_usage(const char *prog_name)
{
    printf("Usage: %s [-h] [-v]\n", prog_name);
    printf("  -h    Show this help message\n");
    printf("  -v    Enable verbose logging\n");
}

/* Check that the interface is in 1R1T mode (r1t) or in 2R2T mode */
static int expect_mode(struct bladerf *dev, const char *step, bool r1t)
{
    int status;
    uint8_t rx      = 0;
    uint8_t tx      = 0;
    bool enabled    = !r1t;
    uint8_t wanted  = r1t ? 0x40 : 0xc0;
    bool ok;

    CHECK_STATUS(bladerf_get_rfic_register(dev, 0x003, &rx));
    CHECK_STATUS(bladerf_get_rfic_register(dev, 0x002, &tx));
    CHECK_STATUS(bladerf_get_rfic_1r1t(dev, &enabled));

    ok = enabled == r1t && (rx & 0xc0) == wanted && (tx & 0xc0) == wanted;
    printf("%-50s %s: 0x003 = 0x%02x, 0x002 = 0x%02x: %s\n", step,
           enabled ? "1R1T" : "2R2T", rx, tx, ok ? "pass" : "FAIL");
    failures += ok ? 0 : 1;

error:
    return status;
}

static void expect_status(const char *step, int actual, int expected)
{
    bool ok = actual == expected;

    printf("%-50s returns %d (%s): %s\n", step, actual,
           bladerf_strerror(actual), ok ? "pass" : "FAIL");
    failures += ok ? 0 : 1;
}

static void expect_mode_selection(struct bladerf *dev, const char *step,
                                  bladerf_rfic_1r1t_mode wanted)
{
    bladerf_rfic_1r1t_mode mode = BLADERF_RFIC_1R1T_ON;
    int status = bladerf_get_rfic_1r1t_mode(dev, &mode);
    bool ok    = status == 0 && mode == wanted;

    printf("%-50s mode %d: %s\n", step, (int)mode, ok ? "pass" : "FAIL");
    failures += ok ? 0 : 1;
}

/* Stream ch from the FPGA's 32-bit counter for a moment (SC8_Q7: bits 15..0
 * in I and Q) and check that every sample after the start counts up */
static int check_counter(struct bladerf *dev, bladerf_channel ch,
                         const char *step)
{
    int status;
    const unsigned int num_samples = 1 << 18;
    const unsigned int num_reads   = 128; /* 0.27 s at 122.88 Msps */
    int8_t *samples = malloc(2 * num_samples);
    unsigned int jumps = 0;
    bool started = false;
    uint16_t expected = 0;
    unsigned int i, n;

    if (samples == NULL) {
        return BLADERF_ERR_MEM;
    }

    CHECK_STATUS(bladerf_set_rx_mux(dev, BLADERF_RX_MUX_32BIT_COUNTER));
    CHECK_STATUS(bladerf_sync_config(dev, BLADERF_RX_X1, BLADERF_FORMAT_SC8_Q7,
                                     256, 32768, 32, 1000));
    CHECK_STATUS(bladerf_enable_module(dev, ch, true));

    /* Skip the start, where libbladeRF can deliver buffers out of order */
    for (n = 0; n < 32; n++) {
        CHECK_STATUS(bladerf_sync_rx(dev, samples, num_samples, NULL, 1000));
    }

    for (n = 0; n < num_reads; n++) {
        CHECK_STATUS(bladerf_sync_rx(dev, samples, num_samples, NULL, 1000));

        for (i = 0; i < num_samples; i++) {
            uint16_t value = (uint8_t)samples[2 * i] |
                             ((uint16_t)(uint8_t)samples[2 * i + 1] << 8);

            if (started && value != expected) {
                jumps++;
            }

            started  = true;
            expected = value + 1;
        }
    }

    printf("%-50s %u samples, %u jumps: %s\n", step, num_samples * num_reads,
           jumps, jumps == 0 ? "pass" : "FAIL");
    failures += jumps == 0 ? 0 : 1;

error:
    bladerf_enable_module(dev, ch, false);
    bladerf_set_rx_mux(dev, BLADERF_RX_MUX_BASEBAND);
    free(samples);
    return status;
}

static int test_unsupported(struct bladerf *dev)
{
    int status;

    CHECK_STATUS(bladerf_enable_feature(dev, BLADERF_FEATURE_OVERSAMPLE, true));
    CHECK_STATUS(bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0),
                                         RATE_OVERSAMPLE, NULL));
    CHECK_STATUS(expect_mode(dev, "AUTO, OVERSAMPLE, 122.88 Msps", false));

    expect_status("ON", bladerf_set_rfic_1r1t_mode(dev, BLADERF_RFIC_1R1T_ON),
                  BLADERF_ERR_UNSUPPORTED);
    CHECK_STATUS(expect_mode(dev, "after ON was refused", false));

    expect_status("OFF", bladerf_set_rfic_1r1t_mode(dev, BLADERF_RFIC_1R1T_OFF),
                  0);
    expect_status("AUTO", bladerf_set_rfic_1r1t_mode(dev, BLADERF_RFIC_1R1T_AUTO),
                  0);

error:
    return status;
}

static int test_supported(struct bladerf **devp)
{
    int status;
    struct bladerf *dev = *devp;
    const bladerf_channel rx1 = BLADERF_CHANNEL_RX(0);
    const bladerf_channel rx2 = BLADERF_CHANNEL_RX(1);
    const bladerf_channel tx2 = BLADERF_CHANNEL_TX(1);

    /* AUTO at the OVERSAMPLE rates */
    CHECK_STATUS(bladerf_enable_feature(dev, BLADERF_FEATURE_OVERSAMPLE, true));
    CHECK_STATUS(bladerf_set_sample_rate(dev, rx1, RATE_OVERSAMPLE, NULL));
    CHECK_STATUS(expect_mode(dev, "AUTO, OVERSAMPLE, 122.88 Msps", true));

    CHECK_STATUS(bladerf_set_bandwidth(dev, rx1, 56000000, NULL));
    CHECK_STATUS(expect_mode(dev, "after an RX bandwidth of 56 MHz", true));

    CHECK_STATUS(bladerf_set_frequency(dev, rx1, 2400000000));
    CHECK_STATUS(expect_mode(dev, "after an RX frequency of 2.4 GHz", true));

    CHECK_STATUS(bladerf_set_gain_mode(dev, rx1, BLADERF_GAIN_MGC));
    CHECK_STATUS(bladerf_set_gain(dev, rx1, 20));
    CHECK_STATUS(expect_mode(dev, "after an RX gain of 20 dB", true));

    /* RX2 needs 2R2T mode */
    CHECK_STATUS(check_counter(dev, rx2, "AUTO, RX2 streams the counter"));
    CHECK_STATUS(expect_mode(dev, "after RX2 was enabled", false));

    CHECK_STATUS(check_counter(dev, rx1, "AUTO, RX1 streams the counter"));
    CHECK_STATUS(expect_mode(dev, "after RX1 was enabled again", true));

    /* ON */
    CHECK_STATUS(bladerf_set_rfic_1r1t_mode(dev, BLADERF_RFIC_1R1T_ON));
    CHECK_STATUS(expect_mode(dev, "ON", true));

    expect_status("ON, enable RX2", bladerf_enable_module(dev, rx2, true),
                  BLADERF_ERR_UNSUPPORTED);
    expect_status("ON, enable TX2", bladerf_enable_module(dev, tx2, true),
                  BLADERF_ERR_UNSUPPORTED);

    CHECK_STATUS(bladerf_sync_config(dev, BLADERF_RX_X2, BLADERF_FORMAT_SC8_Q7,
                                     256, 32768, 32, 1000));
    expect_status("ON, enable RX1 with an RX X2 layout",
                  bladerf_enable_module(dev, rx1, true),
                  BLADERF_ERR_UNSUPPORTED);
    CHECK_STATUS(bladerf_sync_config(dev, BLADERF_RX_X1, BLADERF_FORMAT_SC8_Q7,
                                     256, 32768, 32, 1000));
    CHECK_STATUS(expect_mode(dev, "after the refused enables", true));

    /* OFF */
    CHECK_STATUS(bladerf_set_rfic_1r1t_mode(dev, BLADERF_RFIC_1R1T_OFF));
    CHECK_STATUS(expect_mode(dev, "OFF, OVERSAMPLE, 122.88 Msps", false));

    CHECK_STATUS(check_counter(dev, rx1, "OFF, RX1 streams the counter"));
    CHECK_STATUS(expect_mode(dev, "after RX1 was enabled in OFF", false));

    /* AUTO without OVERSAMPLE */
    CHECK_STATUS(bladerf_set_rfic_1r1t_mode(dev, BLADERF_RFIC_1R1T_AUTO));
    CHECK_STATUS(expect_mode(dev, "AUTO again", true));

    CHECK_STATUS(bladerf_enable_feature(dev, BLADERF_FEATURE_OVERSAMPLE, false));
    CHECK_STATUS(bladerf_set_sample_rate(dev, rx1, RATE_DEFAULT, NULL));
    CHECK_STATUS(expect_mode(dev, "AUTO, no OVERSAMPLE, 61.44 Msps", false));

    /* A new session starts in AUTO and 2R2T mode */
    CHECK_STATUS(bladerf_enable_feature(dev, BLADERF_FEATURE_OVERSAMPLE, true));
    CHECK_STATUS(bladerf_set_sample_rate(dev, rx1, RATE_OVERSAMPLE, NULL));
    CHECK_STATUS(expect_mode(dev, "AUTO, OVERSAMPLE again, before a close", true));

    bladerf_close(dev);
    *devp = NULL;
    CHECK_STATUS(bladerf_open(devp, NULL));
    dev = *devp;

    expect_mode_selection(dev, "open again", BLADERF_RFIC_1R1T_AUTO);
    CHECK_STATUS(expect_mode(dev, "open again", false));

error:
    return status;
}

int main(int argc, char *argv[])
{
    int status = 0;
    struct bladerf *dev = NULL;
    struct bladerf_version version;
    bool supported;
    int opt;
    int opt_ind = 0;

    struct option longopts[] = {
        {"help", no_argument, 0, 'h'},
        {"verbose", no_argument, 0, 'v'},
        {0, 0, 0, 0}
    };

    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_WARNING);

    while ((opt = getopt_long(argc, argv, "hv", longopts, &opt_ind)) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                return EXIT_SUCCESS;
            case 'v':
                bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_VERBOSE);
                break;
            default:
                print_usage(argv[0]);
                return EXIT_FAILURE;
        }
    }

    CHECK_STATUS(bladerf_open(&dev, NULL));
    CHECK_STATUS(bladerf_fpga_version(dev, &version));

    supported = version.major > 0 || version.minor >= 17;
    printf("FPGA v%u.%u.%u: %s\n", version.major, version.minor, version.patch,
           supported ? "1R1T supported" : "no 1R1T support");

    expect_mode_selection(dev, "fresh open", BLADERF_RFIC_1R1T_AUTO);
    CHECK_STATUS(expect_mode(dev, "fresh open", false));

    if (supported) {
        CHECK_STATUS(test_supported(&dev));
    } else {
        CHECK_STATUS(test_unsupported(dev));
    }

error:
    if (dev) {
        bladerf_close(dev);
    }

    if (status != 0) {
        printf("RESULT: ERROR\n");
        return EXIT_FAILURE;
    }

    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
