/*
 * rx-gap-check (2026-11-23): sustained RX with the sync interface and XFERS transfers, to
 * check that fewer transfers (the stop-hang workaround) keep up. RX1, SC8_Q7_META +
 * OVERSAMPLE 122.88 Msps (-f sc16: SC16_Q11_META 61.44 Msps), the stream setup of stop-hang.c
 * (256 buffers of 32768 samples, 5 s time-out), RX_NOW reads of 32768 samples for SECONDS.
 * Counts reads with the overrun flag and timestamp gaps (a timestamp other than the previous
 * timestamp plus the previous count). TESTING.md section 23; ran as gapfix/stophang/tputrx
 * on the XPS.
 *
 * rx-gap-check [-n XFERS] [-s SECONDS] [-f sc8|sc16]
 * Build: cc -O2 -g -Wall -Wextra -o rx-gap-check rx-gap-check.c -lbladeRF
 */
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libbladeRF.h>

#define SPB 32768
#define TIMEOUT_MS 5000

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    bool sc16 = false, have_prev = false;
    int opt, status, nxfer = 32, reads = 0, overruns = 0, gaps = 0, rc = 1;
    double secs = 30, rate, t0, el = 0;
    uint64_t prev_ts = 0, prev_n = 0, total = 0, lost = 0;
    struct bladerf *dev = NULL;
    struct bladerf_metadata m;
    void *buf;

    while ((opt = getopt(argc, argv, "n:s:f:")) != -1) {
        switch (opt) {
            case 'n': nxfer = atoi(optarg); break;
            case 's': secs = atof(optarg); break;
            case 'f': sc16 = !strcmp(optarg, "sc16"); break;
            default:
                fprintf(stderr, "usage: %s [-n XFERS] [-s SECONDS] [-f sc8|sc16]\n", argv[0]);
                return 2;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    rate = sc16 ? 61.44e6 : 122.88e6;
    buf = calloc(SPB, sc16 ? 4 : 2);
    if (buf == NULL) {
        printf("ERROR: out of memory\n");
        return 1;
    }
    if ((status = bladerf_open(&dev, NULL)) != 0) {
        printf("ERROR open: %s\n", bladerf_strerror(status));
        free(buf);
        return 1;
    }
    if (!sc16 && (status = bladerf_enable_feature(dev, BLADERF_FEATURE_OVERSAMPLE, true)) != 0) {
        printf("ERROR enable_feature: %s\n", bladerf_strerror(status));
        goto out;
    }
    if ((status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), (bladerf_sample_rate)rate,
                                          NULL)) != 0) {
        printf("ERROR set_sample_rate: %s\n", bladerf_strerror(status));
        goto out;
    }
    if ((status = bladerf_sync_config(dev, BLADERF_RX_X1,
                                      sc16 ? BLADERF_FORMAT_SC16_Q11_META
                                           : BLADERF_FORMAT_SC8_Q7_META,
                                      256, SPB, (unsigned int)nxfer, TIMEOUT_MS)) != 0) {
        printf("ERROR sync_config: %s\n", bladerf_strerror(status));
        goto out;
    }
    if ((status = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true)) != 0) {
        printf("ERROR enable_module: %s\n", bladerf_strerror(status));
        goto out;
    }
    t0 = now_s();
    do {
        memset(&m, 0, sizeof(m));
        m.flags = BLADERF_META_FLAG_RX_NOW;
        status = bladerf_sync_rx(dev, buf, SPB, &m, TIMEOUT_MS);
        if (status != 0) {
            printf("ERROR sync_rx after %d reads: %s\n", reads, bladerf_strerror(status));
            break;
        }
        reads++;
        if (m.status & BLADERF_META_STATUS_OVERRUN) {
            overruns++;
        }
        if (have_prev && m.timestamp != prev_ts + prev_n) {
            gaps++;
            lost += m.timestamp - (prev_ts + prev_n);
        }
        prev_ts = m.timestamp;
        prev_n = m.actual_count;
        have_prev = true;
        total += m.actual_count;
        el = now_s() - t0;
    } while (el < secs);
    printf("TPUT %s, %d transfers: %.1f s, %" PRIu64 " samples (%.3f Msps), %d reads, %d with an "
           "overrun, %d timestamp gaps (%" PRIu64 " samples lost)\n",
           sc16 ? "SC16_Q11_META 61.44 Msps" : "SC8_Q7_META + OVERSAMPLE 122.88 Msps", nxfer, el,
           total, el > 0 ? total / el / 1e6 : 0.0, reads, overruns, gaps, lost);
    rc = status != 0 ? 1 : 0;
    bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);

out:
    bladerf_close(dev);
    free(buf);
    return rc;
}
