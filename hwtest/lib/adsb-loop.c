/* adsb-loop.c (2026-11-19): a functional test of the bladeRF 2.0 micro's adsb FPGA revision (Nuand's
 * ADS-B decoder, bladeRF-adsb) with no antenna: a synthetic 1090ES frame is transmitted and looped back
 * inside the AD9361 (BLADERF_LB_RFIC_BIST: the AD9361's TX data port feeds its RX data port), so the
 * FPGA's decoder receives it as if from the air; the decoded messages are read the way Nuand's
 * bladeRF_adsb utility reads them (16-byte slots, byte 0 bit 0 = valid, the message from byte 2,
 * 14 bytes for DF >= 16) and compared with the frame sent.
 *   adsb-loop [-t SECONDS] [-p PERIOD_US] [-a AMPLITUDE] [-n] [-z] [-b TX_BUFFERS] [-d DEVICE]
 *   -n: send nothing (TX stays off): the decoder must report no message.
 *   -b: libbladeRF TX buffers of 16384 samples (default 32, about 33 ms; transfers: half of them).
 *   -z: send the frame at DC (version 1 did this by mistake): the decoder must report no message.
 * bladeRF_adsb tunes to 1086 MHz at 16 Msps, so a frame on 1090 MHz arrives at +4 MHz = +Fs/4, and the
 * FPGA's front end (bladeRF-adsb adsb_fe.vhd) mixes by -Fs/4 and low-pass filters it before the power
 * detector. The frame is therefore sent at +Fs/4 (the I/Q phase turns +90 degrees a sample); a frame
 * at DC lands at -Fs/4 after the mixer and the filter removes it.
 * The adsb image must already be loaded; host tuning mode (BLADERF_DEFAULT_TUNING_MODE=host), which the
 * AD9361 loopback needs. TX gain 0 dB (the harness's TX cap applies when preloaded).
 * At the end the program stops sending and reads on for 0.3 s, so the frames still queued for TX come
 * back too (version 2 stopped reading at once and allowed a guessed 35 frames in flight).
 * Version 4: every frame is different, so each one can be found again: the DF17 frame of "The 1090 MHz
 * Riddle" (ICAO 4840d6) with the frame number in ME bytes 8-10 and its CRC-24 (parity) recomputed.
 * Exit 0: every frame sent came back decoded and exact, once, except at most the last one (libbladeRF
 * holds a partly filled TX buffer back); 1: missing, repeated, extra or wrong messages; 2: errors.
 */
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libbladeRF.h>

#define RATE        16000000u   /* bladeRF_adsb's sample rate */
#define FREQ        1086000000u /* RX: bladeRF_adsb's frequency (no effect in the digital loopback) */
#define TX_FREQ     915000000u  /* TX: no effect in the digital loopback; keeps the weak emission (the DAC
                                 * also gets the samples) in the 902-928 MHz ISM band, out of the
                                 * 960-1215 MHz aviation band */
#define BW          14000000u
#define RX_SAMPLES  2048u       /* per read: 8192 bytes = 512 slots */
#define TIMEOUT_MS  3500
#define DRAIN_S     0.3         /* reading on after TX stops: queued TX buffers (<= 33 ms), the decoder's
                                 * message aggregator (10 ms) and the RX transfers (about 40 ms each) */

/* DF17 frame of "The 1090 MHz Riddle" (8d4840d6202cc371c32ce0576098): the first 8 bytes are kept; ME
 * bytes 8-10 carry the frame number and bytes 11-13 its CRC-24 */
static const uint8_t PREFIX[8] = { 0x8d, 0x48, 0x40, 0xd6, 0x20, 0x2c, 0xc3, 0x71 };

static volatile bool stop;
static uint64_t frames_sent;
static double tx_max_gap;       /* longest time between two bladerf_sync_tx() returns (s) */
static unsigned tx_dry;         /* gaps longer than the TX buffers last: the TX queue ran dry */

struct txctx {
    struct bladerf *dev;
    int16_t *wave;          /* one period: preamble + 112 bits + silence, I/Q interleaved */
    unsigned period;        /* samples */
    int16_t amp;
    bool dc;
    double queue_s;         /* how long the TX buffers last */
    int status;
};

/* Mode S CRC-24 (generator 0x1fff409, as bladeRF-adsb adsb_crc.vhd) of the first n bytes */
static uint32_t crc24(const uint8_t *d, unsigned n)
{
    uint32_t reg = 0;
    unsigned i, b;
    for (i = 0; i < n + 3; i++) {
        uint8_t byte = i < n ? d[i] : 0;
        for (b = 0; b < 8; b++) {
            reg = (reg << 1) | ((byte >> (7 - b)) & 1);
            if (reg & 0x1000000) {
                reg ^= 0x1fff409;
            }
        }
    }
    return reg & 0xffffff;
}

static void make_frame(uint8_t f[14], uint32_t k)
{
    uint32_t c;
    memcpy(f, PREFIX, 8);
    f[8] = (uint8_t)(k >> 16);
    f[9] = (uint8_t)(k >> 8);
    f[10] = (uint8_t)k;
    c = crc24(f, 11);
    f[11] = (uint8_t)(c >> 16);
    f[12] = (uint8_t)(c >> 8);
    f[13] = (uint8_t)c;
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* One pulse sample n of the period: at +Fs/4, e^(j*pi*n/2) = 1, j, -1, -j (or at DC with dc set).
 * The period is a multiple of 16 samples, so the phase continues across periods. */
static void pulse(int16_t *w, unsigned n, int16_t amp, bool dc)
{
    static const int si[4] = { 1, 0, -1, 0 }, sq[4] = { 0, 1, 0, -1 };
    w[2 * n]     = dc ? amp : (int16_t)(si[n & 3] * amp);
    w[2 * n + 1] = dc ? 0 : (int16_t)(sq[n & 3] * amp);
}

/* 16 Msps: 8 samples per 0.5 us chip. Preamble pulses at 0, 1, 3.5, 4.5 us; then 112 bits of PPM
 * (1 us each; a 1 is a pulse in the first half, a 0 in the second). Only the first 1920 samples of the
 * period change; the rest stays zero. */
static void build_wave(int16_t *w, int16_t amp, bool dc, const uint8_t FRAME[14])
{
    static const unsigned pre[4] = { 0, 16, 56, 72 };
    unsigned k, b, s;
    memset(w, 0, (size_t)(128 + 16 * 112) * 2 * sizeof(int16_t));
    for (k = 0; k < 4; k++) {
        for (s = 0; s < 8; s++) {
            pulse(w, pre[k] + s, amp, dc);
        }
    }
    for (b = 0; b < 112; b++) {
        bool one = (FRAME[b / 8] >> (7 - b % 8)) & 1;
        unsigned start = 128 + 16 * b + (one ? 0 : 8);
        for (s = 0; s < 8; s++) {
            pulse(w, start + s, amp, dc);
        }
    }
}

static void *tx_thread(void *arg)
{
    struct txctx *c = arg;
    uint8_t f[14];
    double prev = 0, t;
    while (!stop) {
        int s;
        make_frame(f, (uint32_t)frames_sent);
        build_wave(c->wave, c->amp, c->dc, f);
        s = bladerf_sync_tx(c->dev, c->wave, c->period, NULL, TIMEOUT_MS);
        t = now();
        /* after the first queue's worth, a gap longer than the queue means the DAC ran out of samples */
        if (prev != 0 && frames_sent > 64) {
            if (t - prev > tx_max_gap) {
                tx_max_gap = t - prev;
            }
            if (t - prev > c->queue_s) {
                tx_dry++;
            }
        }
        prev = t;
        if (s != 0) {
            fprintf(stderr, "ERROR: bladerf_sync_tx: %s\n", bladerf_strerror(s));
            c->status = s;
            break;
        }
        frames_sent++;
    }
    return NULL;
}

struct counts {
    uint64_t slots, valid, good, bad, dup;
    uint8_t *seen;          /* per frame number: times received */
    uint32_t nseen;
};

/* one RX read: 512 slots of 16 bytes (byte 0 bit 0 = valid, message from byte 2) */
static int read_slots(struct bladerf *dev, uint8_t *buf, struct counts *n)
{
    unsigned i, k;
    int s = bladerf_sync_rx(dev, buf, RX_SAMPLES, NULL, TIMEOUT_MS);
    if (s != 0) {
        fprintf(stderr, "ERROR: bladerf_sync_rx: %s\n", bladerf_strerror(s));
        return s;
    }
    for (i = 0; i < RX_SAMPLES * 4; i += 16) {
        n->slots++;
        if ((buf[i] & 0x01) == 0) {
            continue;
        }
        n->valid++;
        const uint8_t *m = buf + i + 2;
        unsigned end = (m[0] & 0x80) ? 14 : 7;
        uint32_t fk = ((uint32_t)m[8] << 16) | ((uint32_t)m[9] << 8) | m[10];
        if (end == 14 && memcmp(m, PREFIX, 8) == 0 &&
            crc24(m, 11) == (((uint32_t)m[11] << 16) | ((uint32_t)m[12] << 8) | m[13]) && fk < n->nseen) {
            if (n->seen[fk]++ == 0) {
                n->good++;
            } else {
                n->dup++;
            }
        } else {
            n->bad++;
            if (n->bad <= 5) {
                printf("  unexpected message:");
                for (k = 0; k < end; k++) {
                    printf(" %02x", buf[i + 2 + k]);
                }
                printf("\n");
            }
        }
    }
    return 0;
}

#define CHECK(call)                                                          \
    do {                                                                     \
        int s_ = (call);                                                     \
        if (s_ != 0) {                                                       \
            fprintf(stderr, "ERROR: %s: %s\n", #call, bladerf_strerror(s_)); \
            rc = 2;                                                          \
            goto out;                                                        \
        }                                                                    \
    } while (0)

int main(int argc, char **argv)
{
    struct bladerf *dev = NULL;
    const char *devstr = NULL;
    double seconds = 10, t0, t;
    unsigned period_us = 1000;
    int16_t amp = 1000;
    unsigned txbufs = 32;
    bool notx = false, dc = false, tx_on = false, rx_on = false, lb_on = false;
    struct txctx tc = { 0 };
    pthread_t th;
    bool th_started = false;
    uint8_t *buf = NULL;
    struct counts n = { 0 };
    uint64_t good_at_stop = 0;
    int rc = 0, c;

    while ((c = getopt(argc, argv, "t:p:a:nzb:d:")) != -1) {
        switch (c) {
            case 't': seconds = atof(optarg); break;
            case 'p': period_us = (unsigned)atoi(optarg); break;
            case 'a': amp = (int16_t)atoi(optarg); break;
            case 'n': notx = true; break;
            case 'z': dc = true; break;
            case 'b': txbufs = (unsigned)atoi(optarg); break;
            case 'd': devstr = optarg; break;
            default:
                fprintf(stderr, "usage: adsb-loop [-t SECONDS] [-p PERIOD_US] [-a AMPLITUDE] [-n] [-z] [-b TX_BUFFERS] "
                                "[-d DEVICE]\n");
                return 2;
        }
    }
    if (period_us < 130 || amp <= 0 || amp > 2047 || txbufs < 4 || txbufs > 512) {
        fprintf(stderr, "ERROR: period >= 130 us, 0 < amplitude <= 2047, 4 <= TX buffers <= 512\n");
        return 2;
    }
    tc.period = period_us * (RATE / 1000000u);
    tc.wave = calloc((size_t)tc.period * 2, sizeof(int16_t));
    buf = malloc((size_t)RX_SAMPLES * 4);
    if (tc.wave == NULL || buf == NULL) {
        return 2;
    }
    tc.amp = amp;
    tc.dc = dc;
    tc.queue_s = txbufs * 16384.0 / RATE;
    n.nseen = (uint32_t)((seconds + 1) * 1e6 / period_us) + 1000;
    n.seen = calloc(n.nseen, 1);
    if (n.seen == NULL) {
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    CHECK(bladerf_open(&dev, devstr));
    tc.dev = dev;
    CHECK(bladerf_set_frequency(dev, BLADERF_CHANNEL_RX(0), FREQ));
    CHECK(bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), RATE, NULL));
    CHECK(bladerf_set_bandwidth(dev, BLADERF_CHANNEL_RX(0), BW, NULL));
    CHECK(bladerf_set_frequency(dev, BLADERF_CHANNEL_TX(0), TX_FREQ));
    CHECK(bladerf_set_sample_rate(dev, BLADERF_CHANNEL_TX(0), RATE, NULL));
    CHECK(bladerf_set_bandwidth(dev, BLADERF_CHANNEL_TX(0), BW, NULL));
    CHECK(bladerf_set_gain(dev, BLADERF_CHANNEL_TX(0), 0));
    CHECK(bladerf_set_loopback(dev, BLADERF_LB_RFIC_BIST));
    lb_on = true;
    CHECK(bladerf_sync_config(dev, BLADERF_RX_X1, BLADERF_FORMAT_SC16_Q11, 16, RX_SAMPLES, 8, TIMEOUT_MS));
    if (!notx) {
        CHECK(bladerf_sync_config(dev, BLADERF_TX_X1, BLADERF_FORMAT_SC16_Q11, txbufs, 16384, txbufs / 2,
                                  TIMEOUT_MS));
    }
    CHECK(bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true));
    rx_on = true;
    printf("adsb-loop: %s, frame 8d4840d6202cc371c32ce0576098 at %s every %u us at amplitude %d, %.0f s, "
           "looped back in the AD9361\n", notx ? "TX OFF (control)" : dc ? "TX on (DC control)" : "TX on",
           dc ? "DC" : "+Fs/4 (+4 MHz)", period_us, amp, seconds);
    if (!notx) {
        CHECK(bladerf_enable_module(dev, BLADERF_CHANNEL_TX(0), true));
        tx_on = true;
        if (pthread_create(&th, NULL, tx_thread, &tc) != 0) {
            rc = 2;
            goto out;
        }
        th_started = true;
    }
    t0 = now();
    while ((t = now() - t0) < seconds) {
        if (read_slots(dev, buf, &n) != 0) {
            rc = 2;
            break;
        }
    }
    /* stop sending; the TX buffers already queued still go out: read on for DRAIN_S */
    stop = true;
    if (th_started) {
        pthread_join(th, NULL);
        th_started = false;
    }
    good_at_stop = n.good;
    t0 = now();
    while (rc == 0 && now() - t0 < DRAIN_S) {
        if (read_slots(dev, buf, &n) != 0) {
            rc = 2;
        }
    }
out:
    if (th_started) {
        pthread_join(th, NULL);
    }
    if (dev != NULL) {
        if (tx_on) {
            bladerf_enable_module(dev, BLADERF_CHANNEL_TX(0), false);
        }
        if (rx_on) {
            bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
        }
        if (lb_on) {
            bladerf_set_loopback(dev, BLADERF_LB_NONE);
        }
        bladerf_close(dev);
    }
    {
        uint64_t k, missing = 0, miss_first = 0, miss_last = 0, early_miss = UINT64_MAX;
        unsigned shown = 0;
        for (k = 0; k < frames_sent && k < n.nseen; k++) {
            if (n.seen[k] != 0) {
                continue;
            }
            missing++;
            if (k < 100) {
                miss_first++;
            }
            if (k + 100 >= frames_sent) {
                miss_last++;
            }
            if (k + 1 < frames_sent && early_miss == UINT64_MAX) {
                early_miss = k;
            }
            if (shown < 10) {
                printf("  missing frame %" PRIu64 "\n", k);
                shown++;
            }
        }
        if (!notx) {
            printf("TX: %u buffers (%.1f ms); longest gap between bladerf_sync_tx() returns %.1f ms; "
                   "gaps longer than the buffers last: %u\n", txbufs, tc.queue_s * 1e3, tx_max_gap * 1e3, tx_dry);
        }
        printf("slots read %" PRIu64 ", messages %" PRIu64 ": %" PRIu64 " frames exact (%" PRIu64 " after TX "
               "stopped), %" PRIu64 " repeated, %" PRIu64 " other; frames sent %" PRIu64 ", missing %" PRIu64
               " (%" PRIu64 " of the first 100, %" PRIu64 " of the last 100)\n", n.slots, n.valid, n.good,
               n.good - good_at_stop, n.dup, n.bad, frames_sent, missing, miss_first, miss_last);
        if (rc == 0) {
            if (notx || dc) {
                rc = n.valid == 0 ? 0 : 1;
            } else {
                /* every frame sent must come back once, except at most the last (partly filled TX buffer) */
                rc = (n.bad == 0 && n.dup == 0 && early_miss == UINT64_MAX) ? 0 : 1;
            }
        }
    }
    printf("RESULT: %s\n", rc == 0 ? "PASS" : "FAIL");
    free(tc.wave);
    free(buf);
    free(n.seen);
    return rc;
}
