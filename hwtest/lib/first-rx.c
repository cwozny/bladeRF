/*
 * first-rx (2026-11-23): the time to the first RX samples with the sync interface (RX1,
 * SC16_Q11_META or, with -f sc8, SC8_Q7_META; 16 buffers of 8192 samples, 8 transfers, 10 s
 * time-out; the sync worker gives its transfers this time-out, the async interface a fixed
 * 1 s), to tell a slow first start from no start after an image load. Then 1 s of reads.
 * TESTING.md section 23; ran as gapfix/stophang/firstrx2 on the XPS (firstrx: the same with
 * SC16_Q11_META only).
 *
 * first-rx [-f sc8|sc16] [-r RATE]
 * Build: cc -O2 -g -Wall -Wextra -o first-rx first-rx.c -lbladeRF
 */
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libbladeRF.h>

#define SPB 8192
#define TIMEOUT_MS 10000

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    double rate = 20e6, t0, t1, t2, t3, t4, t5;
    struct bladerf *dev = NULL;
    struct bladerf_metadata m;
    int opt, status, rc = 1, reads = 0, errors = 0, overruns = 0;
    int16_t *buf = NULL;
    bool sc8 = false;

    while ((opt = getopt(argc, argv, "f:r:")) != -1) {
        switch (opt) {
            case 'f': sc8 = !strcmp(optarg, "sc8"); break;
            case 'r': rate = atof(optarg); break;
            default:
                fprintf(stderr, "usage: %s [-f sc8|sc16] [-r RATE]\n", argv[0]);
                return 2;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    buf = calloc(SPB, 4);
    if (buf == NULL) {
        printf("ERROR: out of memory\n");
        return 1;
    }

    t0 = now_ms();
    if ((status = bladerf_open(&dev, NULL)) != 0) {
        printf("ERROR open: %s\n", bladerf_strerror(status));
        free(buf);
        return 1;
    }
    t1 = now_ms();
    if ((status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), (bladerf_sample_rate)rate,
                                          NULL)) != 0) {
        printf("ERROR set_sample_rate: %s\n", bladerf_strerror(status));
        goto out;
    }
    t2 = now_ms();
    if ((status = bladerf_sync_config(dev, BLADERF_RX_X1,
                                      sc8 ? BLADERF_FORMAT_SC8_Q7_META : BLADERF_FORMAT_SC16_Q11_META, 16, SPB,
                                      8, TIMEOUT_MS)) != 0) {
        printf("ERROR sync_config: %s\n", bladerf_strerror(status));
        goto out;
    }
    if ((status = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true)) != 0) {
        printf("ERROR enable_module: %s\n", bladerf_strerror(status));
        goto out;
    }
    t3 = now_ms();
    memset(&m, 0, sizeof(m));
    m.flags = BLADERF_META_FLAG_RX_NOW;
    status = bladerf_sync_rx(dev, buf, SPB, &m, TIMEOUT_MS);
    t4 = now_ms();
    printf("%s: ", sc8 ? "SC8_Q7_META" : "SC16_Q11_META");
    printf("FIRST open %.0f ms, set rate %.0f ms, config and enable %.0f ms, first read %.1f ms: "
           "%s, %u samples, status 0x%x\n", t1 - t0, t2 - t1, t3 - t2, t4 - t3,
           status == 0 ? "ok" : bladerf_strerror(status), m.actual_count, m.status);
    if (status == 0) {
        do {
            memset(&m, 0, sizeof(m));
            m.flags = BLADERF_META_FLAG_RX_NOW;
            status = bladerf_sync_rx(dev, buf, SPB, &m, TIMEOUT_MS);
            reads++;
            if (status != 0) {
                errors++;
            } else if (m.status & BLADERF_META_STATUS_OVERRUN) {
                overruns++;
            }
            t5 = now_ms();
        } while (t5 - t4 < 1000 && errors == 0);
        printf("THEN %d reads in %.0f ms: %d errors, %d with an overrun\n", reads, t5 - t4,
               errors, overruns);
        rc = errors ? 1 : 0;
    }
    bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);

out:
    bladerf_close(dev);
    free(buf);
    return rc;
}
