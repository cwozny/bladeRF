/*
 * stop-hang (2026-11-23): the end of a timed-TX burst run (bin/txburst-check.py, which ran as
 * py-txsched2.py), repeated in C, to catch the "Timed out while stopping worker" hang
 * (sync_worker_deinit cancels the RX worker, then async_deinit_stream waits forever).
 * TESTING.md section 23. Ran as gapfix/stophang/stophang2 on the XPS; the job stophunt ran
 * stophang, the same program without -n and without the process ID in the first line.
 *
 * Per cycle, as txburst-check.py per run: sync_config RX (256 x 32768, XFERS transfers,
 * 5000 ms) and, with -t, TX (256 x 8192, 16 transfers); enable TX, then RX; with -t,
 * K timed TX bursts of L samples (BURST_START with a timestamp, then BURST_END with 2
 * zero samples) into the AD9361's BIST loopback at the minimum TX gain; one timed RX
 * read of (K + 2) * P samples (P = rate / 1000) from LEAD_S after the current time;
 * then PAUSE_MS with RX still streaming (the sync worker overruns, as during
 * txburst-check.py's analysis); then RX disable, then TX disable, each timed.
 * -x CYCLES cycles per open. Only standard libbladeRF calls: in AUTO, the 1R1T build
 * selects 1R1T mode by itself; Nuand's libbladeRF works too.
 *
 * Output (line buffered): "STOP <cycle> <wall clock us>" just before the RX disable and
 * "STOPPED <cycle> ..." after the TX disable, so a watchdog can find a stuck disable.
 * Any process of this user may attach a debugger (PR_SET_PTRACER).
 *
 * stop-hang [-d DEV] [-f sc8|sc16] [-r RATE] [-c 1|2] [-x CYCLES] [-w PAUSE_MS]
 *           [-t] [-k BURSTS] [-l LEN] [-L LEAD_S] [-n XFERS]
 *   -n  RX transfers (default 32)
 * Build: cc -O2 -g -Wall -Wextra -o stop-hang stop-hang.c -lbladeRF -lm -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>
#include <libbladeRF.h>

#define TIMEOUT_MS 5000

static int64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

#define CHK(x)                                                              \
    do {                                                                    \
        int s_ = (x);                                                       \
        if (s_ < 0) {                                                       \
            printf("ERROR line %d: %s: %s\n", __LINE__, #x,                 \
                   bladerf_strerror(s_));                                   \
            rc = 1;                                                         \
            goto out;                                                       \
        }                                                                   \
    } while (0)

int main(int argc, char **argv)
{
    const char *devstr = NULL;
    bool sc16 = false, tx = false;
    double rate = 0, lead = 0.5;
    int ch = 0, cycles = 1, pause_ms = 150, nxfer = 32, k_n = 16, len = 4096, opt, rc = 0, i, k;
    struct bladerf *dev = NULL;
    void *buf = NULL, *burst = NULL, *zeros = NULL;
    bool rx_on = false, tx_on = false, lb_on = false;

    while ((opt = getopt(argc, argv, "d:f:r:c:x:w:tk:l:L:n:")) != -1) {
        switch (opt) {
            case 'd': devstr = optarg; break;
            case 'f': sc16 = !strcmp(optarg, "sc16"); break;
            case 'r': rate = atof(optarg); break;
            case 'c': ch = atoi(optarg) - 1; break;
            case 'x': cycles = atoi(optarg); break;
            case 'w': pause_ms = atoi(optarg); break;
            case 't': tx = true; break;
            case 'k': k_n = atoi(optarg); break;
            case 'l': len = atoi(optarg); break;
            case 'L': lead = atof(optarg); break;
            case 'n': nxfer = atoi(optarg); break;
            default:
                fprintf(stderr, "usage: %s [-d DEV] [-f sc8|sc16] [-r RATE] [-c 1|2] "
                        "[-x CYCLES] [-w PAUSE_MS] [-t] [-k BURSTS] [-l LEN] [-L LEAD_S] [-n XFERS]\n",
                        argv[0]);
                return 2;
        }
    }
    if (rate == 0) {
        rate = sc16 ? 61.44e6 : 122.88e6;
    }
    if (ch < 0 || ch > 1 || cycles < 1 || k_n < 1 || len < 2 || len > 32767 || nxfer < 1 ||
        nxfer > 128) {
        fprintf(stderr, "bad arguments\n");
        return 2;
    }

    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);

    struct bladerf_version v;
    bladerf_version(&v);
    printf("pid %d; libbladeRF %s; %s %s %.2f Msps ch%d, %d cycles, pause %d ms, %d RX transfers, %s\n",
           (int)getpid(), v.describe, sc16 ? "SC16_Q11_META" : "SC8_Q7_META, OVERSAMPLE",
           tx ? "TX+RX" : "RX only", rate / 1e6, ch + 1, cycles, pause_ms, nxfer,
           tx ? "timed TX bursts into the BIST loopback" : "no TX");

    bladerf_channel rxc = BLADERF_CHANNEL_RX(ch), txc = BLADERF_CHANNEL_TX(ch);
    bladerf_format fmt = sc16 ? BLADERF_FORMAT_SC16_Q11_META : BLADERF_FORMAT_SC8_Q7_META;
    size_t ss = sc16 ? 4 : 2;
    uint64_t period = (uint64_t)(rate / 1000);
    size_t n = (size_t)(k_n + 2) * period;
    bladerf_gain gmin = 0, g;

    CHK(bladerf_open(&dev, devstr));
    if (!sc16) {
        CHK(bladerf_enable_feature(dev, BLADERF_FEATURE_OVERSAMPLE, true));
    }
    CHK(bladerf_set_sample_rate(dev, rxc, (bladerf_sample_rate)rate, NULL));
    if (tx) {
        CHK(bladerf_set_sample_rate(dev, txc, (bladerf_sample_rate)rate, NULL));
    }
    CHK(bladerf_set_rx_mux(dev, BLADERF_RX_MUX_BASEBAND));
    if (tx) {
        const struct bladerf_range *gr;
        CHK(bladerf_set_loopback(dev, BLADERF_LB_RFIC_BIST));
        lb_on = true;
        CHK(bladerf_get_gain_range(dev, txc, &gr));
        gmin = (bladerf_gain)ceil(gr->min * gr->scale);
        CHK(bladerf_set_gain(dev, txc, gmin));
        CHK(bladerf_get_gain(dev, txc, &g));
        printf("TX%d gain set to %d dB (minimum %.2f dB), reads back %d dB\n",
               ch + 1, gmin, gr->min * gr->scale, g);
    }

    buf = calloc(n, ss);
    burst = malloc((size_t)len * ss);
    zeros = calloc(2, ss);
    if (buf == NULL || burst == NULL || zeros == NULL) {
        printf("ERROR: out of memory\n");
        rc = 1;
        goto out;
    }
    for (k = 0; k < len; k++) {
        unsigned int w = (unsigned int)k & 0x7FFF;
        if (sc16) {
            ((int16_t *)burst)[2 * k] = (int16_t)(w & 0x7FF);
            ((int16_t *)burst)[2 * k + 1] = (int16_t)(-2048 + (int)(w >> 11));
        } else {
            ((uint8_t *)burst)[2 * k] = (uint8_t)(w & 0xFF);
            ((uint8_t *)burst)[2 * k + 1] = (uint8_t)(0x80 | (w >> 8));
        }
    }

    int (*get_1r1t)(struct bladerf *, bool *) =
        (int (*)(struct bladerf *, bool *))dlsym(RTLD_DEFAULT, "bladerf_get_rfic_1r1t");

    for (i = 1; i <= cycles; i++) {
        struct bladerf_metadata m, rm;
        uint64_t t;
        int64_t t1, t2, t3;

        CHK(bladerf_sync_config(dev, BLADERF_RX_X1, fmt, 256, 32768, (unsigned int)nxfer, TIMEOUT_MS));
        if (tx) {
            CHK(bladerf_sync_config(dev, BLADERF_TX_X1, fmt, 256, 8192, 16, TIMEOUT_MS));
            CHK(bladerf_enable_module(dev, txc, true));
            tx_on = true;
        }
        CHK(bladerf_enable_module(dev, rxc, true));
        rx_on = true;
        if (tx) {
            CHK(bladerf_get_gain(dev, txc, &g));
            if (g > gmin + 1) {
                printf("ERROR: TX gain %d dB with TX enabled, not the minimum\n", g);
                rc = 1;
                goto out;
            }
        }
        if (i == 1) {
            bool r1 = false;
            if (get_1r1t == NULL) {
                printf("1R1T: not in this libbladeRF\n");
            } else if (get_1r1t(dev, &r1) == 0) {
                printf("1R1T: %s\n", r1 ? "on" : "off");
            }
        }

        CHK(bladerf_get_timestamp(dev, tx ? BLADERF_TX : BLADERF_RX, &t));
        uint64_t ts0 = t + (uint64_t)(lead * rate);
        printf("cycle %d: %s timestamp %" PRIu64 "; first TX burst at %" PRIu64 "\n", i,
               tx ? "TX" : "RX", t, ts0);
        if (tx) {
            for (k = 0; k < k_n; k++) {
                memset(&m, 0, sizeof(m));
                m.flags = BLADERF_META_FLAG_TX_BURST_START;
                m.timestamp = ts0 + (uint64_t)k * period;
                CHK(bladerf_sync_tx(dev, burst, (unsigned int)len, &m, TIMEOUT_MS));
                memset(&m, 0, sizeof(m));
                m.flags = BLADERF_META_FLAG_TX_BURST_END;
                CHK(bladerf_sync_tx(dev, zeros, 2, &m, TIMEOUT_MS));
            }
        }
        memset(&rm, 0, sizeof(rm));
        rm.timestamp = ts0 - period;
        CHK(bladerf_sync_rx(dev, buf, (unsigned int)n, &rm, 2 * TIMEOUT_MS));
        printf("cycle %d: RX read %u of %zu samples from %" PRIu64 " (asked %" PRIu64
               "), status 0x%x\n", i, rm.actual_count, n, rm.timestamp, ts0 - period,
               rm.status);

        usleep((useconds_t)pause_ms * 1000);
        t1 = now_us();
        printf("STOP %d %" PRId64 "\n", i, t1);
        rx_on = false;
        CHK(bladerf_enable_module(dev, rxc, false));
        t2 = now_us();
        if (tx) {
            tx_on = false;
            CHK(bladerf_enable_module(dev, txc, false));
        }
        t3 = now_us();
        printf("STOPPED %d rx %.1f ms tx %.1f ms\n", i, (t2 - t1) / 1e3, (t3 - t2) / 1e3);
    }
    printf("RESULT: %d cycles done\n", cycles);

out:
    if (dev != NULL) {
        if (rx_on) {
            bladerf_enable_module(dev, rxc, false);
        }
        if (tx_on) {
            bladerf_enable_module(dev, txc, false);
        }
        if (lb_on) {
            bladerf_set_loopback(dev, BLADERF_LB_NONE);
        }
        bladerf_close(dev);
    }
    free(buf);
    free(burst);
    free(zeros);
    return rc;
}
