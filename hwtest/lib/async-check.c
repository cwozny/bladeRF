/*
 * async-check (2026-11-23): the second comment of Nuand/bladeRF#940 with the asynchronous
 * interface: bladerf_init_stream() with a META format and SAMPLES-sample buffers (16 buffers,
 * 8 transfers), RX1, bladerf_enable_module(), then bladerf_stream() until TOTAL samples (by
 * num_samples) have arrived. Prints num_samples per callback, the messages per buffer, the
 * timestamp advance per buffer and per message, and two rates: num_samples per second and
 * timestamp ticks per second (the FPGA counts one tick per sample). The per-message lines take
 * 2048-byte messages; with FX3 firmware 2.6.0 the messages are 8192 bytes, so only the
 * per-buffer lines and the rates are right. TESTING.md section 23; ran as
 * gapfix/stophang/asynccheck on the XPS.
 *
 * async-check [-f sc8|sc16] [-r RATE] [-o] [-b SAMPLES] [-n TOTAL]
 *   -o  OVERSAMPLE on (before the sample rate is set)
 * Build: cc -O2 -g -Wall -Wextra -o async-check async-check.c -lbladeRF
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
#define MSG_SIZE 2048

struct ctx {
    void **bufs;
    unsigned int next;
    size_t buf_bytes;
    uint64_t total, target, ncb;
    size_t ns_min, ns_max;
    uint64_t ts_first, ts_last, ts_prev_buf;
    int64_t dbuf_min, dbuf_max, dmsg_min, dmsg_max;
    bool have_ts;
    double t5, tlast;
};

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static uint64_t ts_at(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[4 + i];
    }
    return v;
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
        const uint8_t *b = samples;
        size_t nmsg = c->buf_bytes / MSG_SIZE, m;
        uint64_t prev = 0;

        c->ncb++;
        if (c->ncb == 1 || num_samples < c->ns_min) {
            c->ns_min = num_samples;
        }
        if (num_samples > c->ns_max) {
            c->ns_max = num_samples;
        }
        for (m = 0; m < nmsg; m++) {
            uint64_t ts = ts_at(b + m * MSG_SIZE);
            if (m > 0 && c->ncb > 4) {
                int64_t d = (int64_t)(ts - prev);
                if (c->dmsg_min == 0 || d < c->dmsg_min) c->dmsg_min = d;
                if (d > c->dmsg_max) c->dmsg_max = d;
            }
            prev = ts;
        }
        uint64_t ts0 = ts_at(b);
        if (c->have_ts && c->ncb > 4) {
            int64_t d = (int64_t)(ts0 - c->ts_prev_buf);
            if (c->dbuf_min == 0 || d < c->dbuf_min) c->dbuf_min = d;
            if (d > c->dbuf_max) c->dbuf_max = d;
        }
        if (c->ncb == 5) {
            c->ts_first = ts0;
            c->t5 = now_s();
        }
        c->tlast = now_s();
        c->ts_prev_buf = ts0;
        c->ts_last = ts0;
        c->have_ts = true;
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
    bool sc16 = false, os = false;
    double rate = 20e6;
    size_t spb = 65536;
    uint64_t total = 50000000;
    int opt, status, rc = 1;
    struct bladerf *dev = NULL;
    struct bladerf_stream *stream = NULL;
    struct ctx c;

    while ((opt = getopt(argc, argv, "f:r:ob:n:")) != -1) {
        switch (opt) {
            case 'f': sc16 = !strcmp(optarg, "sc16"); break;
            case 'r': rate = atof(optarg); break;
            case 'o': os = true; break;
            case 'b': spb = (size_t)atol(optarg); break;
            case 'n': total = (uint64_t)atoll(optarg); break;
            default:
                fprintf(stderr, "usage: %s [-f sc8|sc16] [-r RATE] [-o] [-b SAMPLES] [-n TOTAL]\n", argv[0]);
                return 2;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    memset(&c, 0, sizeof(c));
    c.target = total;
    bladerf_format fmt = sc16 ? BLADERF_FORMAT_SC16_Q11_META : BLADERF_FORMAT_SC8_Q7_META;
    size_t bps = sc16 ? 4 : 2;
    c.buf_bytes = spb * bps;

    struct bladerf_version v;
    bladerf_version(&v);
    printf("libbladeRF %s; %s, OVERSAMPLE %s, %.3f Msps, %zu-sample buffers, until %" PRIu64
           " samples\n", v.describe, sc16 ? "SC16_Q11_META" : "SC8_Q7_META", os ? "on" : "off",
           rate / 1e6, spb, total);

    if ((status = bladerf_open(&dev, NULL)) != 0) {
        printf("ERROR open: %s\n", bladerf_strerror(status));
        return 1;
    }
    if (os && (status = bladerf_enable_feature(dev, BLADERF_FEATURE_OVERSAMPLE, true)) != 0) {
        printf("ERROR enable_feature: %s\n", bladerf_strerror(status));
        goto out;
    }
    bladerf_sample_rate actual = 0;
    if ((status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), (bladerf_sample_rate)rate,
                                          &actual)) != 0) {
        printf("ERROR set_sample_rate: %s\n", bladerf_strerror(status));
        goto out;
    }
    printf("sample rate set: %u\n", actual);
    if ((status = bladerf_init_stream(&stream, dev, cb, &c.bufs, NUM_BUFFERS, fmt, spb,
                                      NUM_XFERS, &c)) != 0) {
        printf("ERROR init_stream: %s\n", bladerf_strerror(status));
        goto out;
    }
    bladerf_set_stream_timeout(dev, BLADERF_RX, 5000);
    if ((status = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true)) != 0) {
        printf("ERROR enable_module: %s\n", bladerf_strerror(status));
        goto out;
    }
    double t0 = now_s();
    status = bladerf_stream(stream, BLADERF_RX_X1);
    double el = now_s() - t0;
    bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
    if (status != 0) {
        printf("ERROR stream: %s\n", bladerf_strerror(status));
        goto out;
    }
    double tsel = (double)(c.ts_last - c.ts_first), wel = c.tlast - c.t5;
    printf("callbacks %" PRIu64 "; num_samples per callback %zu..%zu; %zu messages of %d bytes "
           "per buffer\n", c.ncb, c.ns_min, c.ns_max, c.buf_bytes / MSG_SIZE, MSG_SIZE);
    printf("timestamp advance: %" PRId64 "..%" PRId64 " per buffer, %" PRId64 "..%" PRId64
           " per message\n", c.dbuf_min, c.dbuf_max, c.dmsg_min, c.dmsg_max);
    printf("%" PRIu64 " samples by num_samples in %.3f s: %.3f Msps by num_samples; "
           "%.3f Msps by timestamps (callbacks 5..%" PRIu64 ")\n", c.total, el,
           c.total / el / 1e6, wel > 0 ? tsel / wel / 1e6 : 0.0, c.ncb);
    rc = 0;

out:
    if (stream) {
        bladerf_deinit_stream(stream);
    }
    bladerf_close(dev);
    return rc;
}
