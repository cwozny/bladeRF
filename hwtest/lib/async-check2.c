/*
 * async-check2 (2026-11-23): async-check.c (RX1, bladerf_init_stream(), 16 buffers,
 * 8 transfers, bladerf_enable_module() before bladerf_stream()) plus formats without metadata
 * (sc8raw, sc16raw), the FPGA's config GPIO register at open, after bladerf_enable_module() and
 * after the stream, and -R: when bladerf_stream() returns BLADERF_ERR_TIMEOUT, disable RX1, set
 * the stream up again, enable RX1 and stream once more. TESTING.md section 23; ran as
 * gapfix/stophang/asynccheck2 on the XPS.
 *
 * async-check2 [-f sc8|sc16|sc8raw|sc16raw] [-r RATE] [-b SAMPLES] [-n TOTAL] [-R]
 * Build: cc -O2 -g -Wall -Wextra -o async-check2 async-check2.c -lbladeRF
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

#define NUM_BUFFERS 16
#define NUM_XFERS 8

struct ctx {
    void **bufs;
    unsigned int next;
    uint64_t total, target, ncb;
};

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void *cb(struct bladerf *dev, struct bladerf_stream *stream,
                struct bladerf_metadata *meta, void *samples, size_t num_samples,
                void *user_data)
{
    struct ctx *c = user_data;
    (void)dev;
    (void)stream;
    (void)meta;

    if (samples != NULL) {
        c->ncb++;
        c->total += num_samples;
        if (c->total >= c->target) {
            return BLADERF_STREAM_SHUTDOWN;
        }
    }
    void *r = c->bufs[c->next];
    c->next = (c->next + 1) % NUM_BUFFERS;
    return r;
}

int main(int argc, char **argv)
{
    const char *f = "sc8";
    bool retry = false;
    double rate = 20e6, t0, el = 0;
    size_t spb = 65536;
    uint64_t total = 50000000;
    int opt, status, rc = 1, attempt, attempts;
    uint32_t g_open = 0, g_en = 0, g_end = 0;
    struct bladerf *dev = NULL;
    struct bladerf_stream *stream = NULL;
    bladerf_format fmt;
    struct ctx c;

    while ((opt = getopt(argc, argv, "f:r:b:n:R")) != -1) {
        switch (opt) {
            case 'f': f = optarg; break;
            case 'r': rate = atof(optarg); break;
            case 'b': spb = (size_t)atol(optarg); break;
            case 'n': total = (uint64_t)atoll(optarg); break;
            case 'R': retry = true; break;
            default:
                fprintf(stderr, "usage: %s [-f sc8|sc16|sc8raw|sc16raw] [-r RATE] [-b SAMPLES] "
                        "[-n TOTAL] [-R]\n", argv[0]);
                return 2;
        }
    }
    if (!strcmp(f, "sc8")) {
        fmt = BLADERF_FORMAT_SC8_Q7_META;
    } else if (!strcmp(f, "sc16")) {
        fmt = BLADERF_FORMAT_SC16_Q11_META;
    } else if (!strcmp(f, "sc8raw")) {
        fmt = BLADERF_FORMAT_SC8_Q7;
    } else if (!strcmp(f, "sc16raw")) {
        fmt = BLADERF_FORMAT_SC16_Q11;
    } else {
        fprintf(stderr, "unknown format %s\n", f);
        return 2;
    }
    attempts = retry ? 2 : 1;
    setvbuf(stdout, NULL, _IOLBF, 0);

    struct bladerf_version v;
    bladerf_version(&v);
    printf("libbladeRF %s; format %s, %.3f Msps, %zu-sample buffers, until %" PRIu64
           " samples%s\n", v.describe, f, rate / 1e6, spb, total,
           retry ? "; one retry after a time-out" : "");

    if ((status = bladerf_open(&dev, NULL)) != 0) {
        printf("ERROR open: %s\n", bladerf_strerror(status));
        return 1;
    }
    if ((status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), (bladerf_sample_rate)rate,
                                          NULL)) != 0) {
        printf("ERROR set_sample_rate: %s\n", bladerf_strerror(status));
        goto out;
    }
    bladerf_config_gpio_read(dev, &g_open);
    for (attempt = 1; attempt <= attempts; attempt++) {
        memset(&c, 0, sizeof(c));
        c.target = total;
        if ((status = bladerf_init_stream(&stream, dev, cb, &c.bufs, NUM_BUFFERS, fmt, spb,
                                          NUM_XFERS, &c)) != 0) {
            printf("ERROR init_stream: %s\n", bladerf_strerror(status));
            stream = NULL;
            goto out;
        }
        if ((status = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true)) != 0) {
            printf("ERROR enable_module: %s\n", bladerf_strerror(status));
            goto out;
        }
        bladerf_config_gpio_read(dev, &g_en);
        t0 = now_s();
        status = bladerf_stream(stream, BLADERF_RX_X1);
        el = now_s() - t0;
        bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
        bladerf_config_gpio_read(dev, &g_end);
        printf("GPIO attempt %d: open 0x%08x, after enable 0x%08x, after stream 0x%08x\n",
               attempt, g_open, g_en, g_end);
        if (status == 0) {
            break;
        }
        printf("ERROR stream, attempt %d: %s\n", attempt, bladerf_strerror(status));
        bladerf_deinit_stream(stream);
        stream = NULL;
        if (status != BLADERF_ERR_TIMEOUT) {
            goto out;
        }
        if (attempt < attempts) {
            printf("RETRY: RX1 disabled, stream set up again, RX1 enabled\n");
        }
    }
    if (status != 0) {
        goto out;
    }
    printf("%" PRIu64 " samples by num_samples in %.3f s (%" PRIu64 " callbacks), attempt %d\n",
           c.total, el, c.ncb, attempt);
    rc = 0;

out:
    if (stream) {
        bladerf_deinit_stream(stream);
    }
    bladerf_close(dev);
    return rc;
}
