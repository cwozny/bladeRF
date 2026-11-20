/*
 * hwtest-stream: streaming checks for the bladeRF hardware test harness
 * (bladeRF 2.0 micro). Built by bin/build-host.sh. Uses libbladeRF's
 * synchronous interface, the only one that supports every sample format.
 *
 *   hwtest-stream probe    -f FMT [-l x1|x2] [-m MUX] [-r RATE]
 *   hwtest-stream rxcheck  -f FMT [-l x1|x2] [-r RATE] [-t SECONDS] [-W WARMUP_S]
 *   hwtest-stream loopback -f FMT [-l x1|x2] [-c CH] [-r RATE] [-t SECONDS] [-A] [-O]
 *   hwtest-stream rxcap    -f FMT [-c CH] [-r RATE] -n SAMPLES -o FILE
 *                          [-F FREQ] [-g GAIN] [-O] [-W WARMUP_S]
 *   hwtest-stream tx       -f FMT [-l x1|x2] [-c CH] [-r RATE] [-t SECONDS]
 *                          (-i FILE | -T TONE_HZ) [-F FREQ] [-g GAIN] [-O]
 *   hwtest-stream trigger  rx [-f FMT] [-r RATE]
 *   hwtest-stream rxsched  -f sc16meta|sc8meta [-l x1|x2] [-c CH] [-r RATE]
 *                          [-t SECONDS] [-b BURST_MS] [-L LEAD_MS | -N]
 *                          [-G GAP_MS] [-o FILE] [-S NBUF,SAMPLES,NXFER] [-O]
 *   hwtest-stream bist     -f FMT [-l x1|x2] [-c CH] [-r RATE] [-t SECONDS]
 *                          [-B prbs|tone] [-o GOLDEN | -i GOLDEN] [-O]
 *
 * FMT: sc16, sc16meta, sc8, sc8meta, packed (SC16_Q11_PACKED).
 *
 * probe     prints the first samples of each channel with the RX mux at MUX
 *           (baseband, counter12, counter32, loopback; for loopback it also
 *           transmits the loopback pattern).
 * rxcheck   receives from the FPGA's 32-bit counter (BLADERF_RX_MUX_32BIT_
 *           COUNTER) and checks every sample of every channel after WARMUP_S
 *           (default 0.25 s): a lost, duplicated or corrupted sample fails;
 *           samples that libbladeRF delivers out of order but complete only
 *           warn (see struct cover). SC8 (non-meta) and packed carry too few
 *           counter bits to tell late from lost: any jump fails. SC8 meta
 *           places its 16-bit counter by the timestamps.
 * loopback  transmits a pattern that numbers every sample (and tags the
 *           channel), receives it back through the FPGA's digital loopback
 *           (BLADERF_RX_MUX_DIGITAL_LOOPBACK) and checks every sample
 *           (SC16 and SC8 only: libbladeRF's asynchronous interface, see
 *           do_loopback). The DACs also get the samples, at the TX gain set
 *           (default 0 dB). With -A the samples loop back in the AD9361
 *           instead (BLADERF_LB_RFIC_BIST: its TX data port feeds its RX data
 *           port; host tuning mode only), so they cross the FPGA-AD9361 LVDS
 *           interface both ways: a check of the TX side of that interface.
 *           TX and RX stream at once, so the rate is also bounded by what
 *           the FX3 and USB carry both ways.
 * rxcap     receives SAMPLES samples into memory after WARMUP_S (default
 *           0.25 s, as rxcheck: past libbladeRF's start-up reordering), then
 *           writes them to FILE (raw interleaved I/Q, int16 or int8): no file
 *           I/O while streaming.
 * tx        transmits FILE (raw interleaved I/Q in the format's sample type,
 *           one channel; duplicated for x2), repeated, or a generated tone
 *           TONE_HZ from the carrier at half scale, for SECONDS, and prints
 *           the rate at which libbladeRF took the samples (below the sample
 *           rate: the host, USB or FX3 did not keep up and the DAC ran dry).
 * trigger   with one bladeRF as trigger master (J51-1): arms the RX trigger,
 *           checks that no samples flow while it is armed, fires it, and
 *           checks that samples then flow and the trigger reports fired.
 * bist      has the AD9361 replace its received samples with its built-in
 *           test pattern (BIST: PRBS, or with -B tone a tone) at its RX data
 *           port, so every sample crosses the AD9361-FPGA LVDS interface
 *           with a known value, and checks every sample of every channel
 *           (SC16 and SC8 only). The pattern repeats: its period and one
 *           "golden" period are learnt from the first samples (or read from
 *           GOLDEN with -i, then only the phase is found), and every later
 *           sample must equal the golden one; errors are counted per bit.
 *           -o saves the golden period (for an -i run in the same format at
 *           a rate where learning may fail, such as overclocked). The BIST
 *           is switched off again at the end (and on SIGINT/SIGTERM).
 *
 * rxsched   the recording pattern of scheduled bursts: with the FPGA's 32-bit
 *           counter as the source, it asks for a burst of BURST_MS (default
 *           100) starting LEAD_MS (default 5) after the current RX timestamp,
 *           reads it, writes it to FILE if given (overwritten each time, as a
 *           recorder writes its files) and waits GAP_MS, then schedules the
 *           next burst the same way, for SECONDS. The stream keeps running
 *           between bursts, so the FPGA overruns there (expected, not
 *           counted). Every burst must start exactly at its timestamp, arrive
 *           complete with no overrun inside it, carry consecutive counter
 *           values (both channels the same for x2), and its counter must have
 *           advanced with the timestamp since the first burst: the samples
 *           come from the requested place in the stream.
 *           With -N the bursts are read "now" (BLADERF_META_FLAG_RX_NOW), back
 *           to back with no timestamp read, as recorders that keep the stream
 *           running do: each burst must start where the previous one ended,
 *           unless libbladeRF reported an overrun, which cuts a burst short;
 *           such a burst is lost (not written) and counted as bad. Only the
 *           first and last counter values of a burst are checked, so the time
 *           between reads stays what a recorder's would be. A FILE
 *           with "%llu" or "%0Wllu" in it (say dir/b%06llu.bin) gets a new
 *           file per burst, named by the burst number, none deleted. -S sets
 *           libbladeRF's sync buffers (default 256 of 64 KiB, 32 transfers).
 *
 * -O enables libbladeRF's AD9361 overclock feature (BLADERF_FEATURE_OVERSAMPLE,
 * up to 122.88 Msps, 8-bit formats only) before the sample rate is set.
 * -R ADDR=VAL sets an AD9361 register once the sample rate is set (libbladeRF
 * rewrites its oversample registers on each rate change), e.g. -R 0x006=0x50
 * for the RX DATA_CLK and data delays; -D prints the AD9361's clock, filter,
 * data port, BBPLL and BIST registers at that point.
 * -F sets the carrier of the channels used, -g their gain (RX: manual gain).
 * The tuning mode comes from BLADERF_DEFAULT_TUNING_MODE, as for libbladeRF.
 * Exit status: 0 if every check passed, 1 if a check failed, 2 on errors.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libbladeRF.h>

#define NUM_BUFFERS   256
#define NUM_TRANSFERS 32
#define TIMEOUT_MS    3500
#define MAX_REPORTS   10        /* mismatch lines printed per channel */

struct opts {
    const char *mode, *sub, *dev, *in, *out, *fmt_name, *mux_name;
    bladerf_format fmt;
    bool x2, oversample, have_freq, have_gain, have_tone, bist_tone;
    bool lb_rfic;                      /* loopback -A: in the AD9361 */
    unsigned npoke;                    /* -R ADDR=VAL: AD9361 registers set after setup */
    uint16_t poke_addr[8];
    uint8_t poke_val[8];
    bool dump_rfic;                    /* -D: print AD9361 registers after setup */
    int ch;                     /* 0 or 1 (x1 only) */
    double rate, seconds, freq, gain, tone, warmup;
    double burst_ms, lead_ms, gap_ms;  /* rxsched */
    bool rx_now;                       /* rxsched -N */
    unsigned nbuf, bufsamp, nxfer;     /* -S: sync buffers (0: defaults) */
    uint64_t nsamples;
    bladerf_rx_mux mux;
};

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static bool is_meta(bladerf_format f)
{
    return f == BLADERF_FORMAT_SC16_Q11_META || f == BLADERF_FORMAT_SC8_Q7_META;
}

static bool is_8bit(bladerf_format f)
{
    return f == BLADERF_FORMAT_SC8_Q7 || f == BLADERF_FORMAT_SC8_Q7_META;
}

/* Bits of I and Q that the host sees: 16 for SC16 (the counter modes fill all
 * of them), 12 for the packed format, 8 for SC8. */
static unsigned format_bits(bladerf_format f)
{
    if (is_8bit(f)) {
        return 8;
    }
    return f == BLADERF_FORMAT_SC16_Q11_PACKED ? 12 : 16;
}

static size_t sample_bytes(bladerf_format f)
{
    return is_8bit(f) ? 2 : 4;
}

/* Samples per buffer: 64 KiB buffers, a multiple of 4096 (needed by the
 * packed format) */
static unsigned buffer_samples(bladerf_format f)
{
    return (unsigned)(65536 / sample_bytes(f));
}

static int parse_format(const char *s, bladerf_format *f)
{
    if (strcmp(s, "sc16") == 0) {
        *f = BLADERF_FORMAT_SC16_Q11;
    } else if (strcmp(s, "sc16meta") == 0) {
        *f = BLADERF_FORMAT_SC16_Q11_META;
    } else if (strcmp(s, "sc8") == 0) {
        *f = BLADERF_FORMAT_SC8_Q7;
    } else if (strcmp(s, "sc8meta") == 0) {
        *f = BLADERF_FORMAT_SC8_Q7_META;
    } else if (strcmp(s, "packed") == 0) {
        *f = BLADERF_FORMAT_SC16_Q11_PACKED;
    } else {
        return -1;
    }
    return 0;
}

static int parse_mux(const char *s, bladerf_rx_mux *m)
{
    if (strcmp(s, "baseband") == 0) {
        *m = BLADERF_RX_MUX_BASEBAND;
    } else if (strcmp(s, "counter12") == 0) {
        *m = BLADERF_RX_MUX_12BIT_COUNTER;
    } else if (strcmp(s, "counter32") == 0) {
        *m = BLADERF_RX_MUX_32BIT_COUNTER;
    } else if (strcmp(s, "loopback") == 0) {
        *m = BLADERF_RX_MUX_DIGITAL_LOOPBACK;
    } else {
        return -1;
    }
    return 0;
}

static double parse_num(const char *s)
{
    char *end;
    double v = strtod(s, &end);
    switch (*end) {
        case 'k': case 'K': v *= 1e3; break;
        case 'M': v *= 1e6; break;
        case 'G': v *= 1e9; break;
        default: break;
    }
    return v;
}

#define CHECK(call)                                                          \
    do {                                                                     \
        int s_ = (call);                                                     \
        if (s_ != 0) {                                                       \
            fprintf(stderr, "ERROR: %s: %s\n", #call, bladerf_strerror(s_)); \
            return 2;                                                        \
        }                                                                    \
    } while (0)

static int nch(const struct opts *o)
{
    return o->x2 ? 2 : 1;
}

/* Channel index of stream slot k (0 or 1) */
static int slot_ch(const struct opts *o, int k)
{
    return o->x2 ? k : o->ch;
}

static int setup_channels(struct bladerf *dev, const struct opts *o, bool rx,
                          bool tx)
{
    int k;
    if (o->oversample) {
        CHECK(bladerf_enable_feature(dev, BLADERF_FEATURE_OVERSAMPLE, true));
        printf("overclock (BLADERF_FEATURE_OVERSAMPLE) enabled\n");
    }
    for (k = 0; k < nch(o); k++) {
        int c = slot_ch(o, k);
        bladerf_sample_rate actual = 0;
        if (rx) {
            bladerf_channel ch = BLADERF_CHANNEL_RX(c);
            if (o->have_freq) {
                CHECK(bladerf_set_frequency(dev, ch, (bladerf_frequency)o->freq));
            }
            CHECK(bladerf_set_sample_rate(dev, ch, (bladerf_sample_rate)o->rate,
                                          &actual));
            printf("RX%d: sample rate %u (requested %.0f)\n", c + 1, actual,
                   o->rate);
            if (o->have_gain) {
                CHECK(bladerf_set_gain_mode(dev, ch, BLADERF_GAIN_MGC));
                CHECK(bladerf_set_gain(dev, ch, (bladerf_gain)o->gain));
            }
        }
        if (tx) {
            bladerf_channel ch = BLADERF_CHANNEL_TX(c);
            if (o->have_freq) {
                CHECK(bladerf_set_frequency(dev, ch, (bladerf_frequency)o->freq));
            }
            CHECK(bladerf_set_sample_rate(dev, ch, (bladerf_sample_rate)o->rate,
                                          &actual));
            printf("TX%d: sample rate %u (requested %.0f)\n", c + 1, actual,
                   o->rate);
            /* the harness's TX cap (lib/tx-cap.c) still applies on enable */
            CHECK(bladerf_set_gain(dev, ch, (bladerf_gain)(o->have_gain ? o->gain : 0)));
        }
    }
    /* -R: after the sample rate (libbladeRF rewrites its oversample registers
     * on every rate change), e.g. 0x006 (RX DATA_CLK and data delays) */
    for (k = 0; k < (int)o->npoke; k++) {
        uint8_t before = 0, after = 0;
        CHECK(bladerf_get_rfic_register(dev, o->poke_addr[k], &before));
        CHECK(bladerf_set_rfic_register(dev, o->poke_addr[k], o->poke_val[k]));
        CHECK(bladerf_get_rfic_register(dev, o->poke_addr[k], &after));
        printf("AD9361 register 0x%03x: 0x%02x -> 0x%02x (reads back 0x%02x)\n",
               o->poke_addr[k], before, o->poke_val[k], after);
    }
    if (o->dump_rfic) {
        /* the clock chain, filter and data port setup, BBPLL and BIST */
        static const uint16_t from[] = { 0x000, 0x03f, 0x3f4 };
        static const uint16_t to[]   = { 0x01f, 0x05f, 0x3f6 };
        unsigned r;
        uint16_t a;
        for (r = 0; r < sizeof(from) / sizeof(from[0]); r++) {
            for (a = from[r]; a <= to[r]; a++) {
                uint8_t v = 0;
                if ((a - from[r]) % 16 == 0) {
                    printf("%sAD9361 0x%03x:", a == from[r] && r == 0 ? "" : "\n", a);
                }
                CHECK(bladerf_get_rfic_register(dev, a, &v));
                printf(" %02x", v);
            }
        }
        printf("\n");
    }
    return 0;
}

static int enable_channels(struct bladerf *dev, const struct opts *o,
                           bladerf_direction dir, bool on)
{
    int k;
    for (k = 0; k < nch(o); k++) {
        int c = slot_ch(o, k);
        bladerf_channel ch = dir == BLADERF_RX ? BLADERF_CHANNEL_RX(c)
                                               : BLADERF_CHANNEL_TX(c);
        CHECK(bladerf_enable_module(dev, ch, on));
    }
    return 0;
}

static bladerf_channel_layout layout(const struct opts *o, bladerf_direction d)
{
    if (d == BLADERF_RX) {
        return o->x2 ? BLADERF_RX_X2 : BLADERF_RX_X1;
    }
    return o->x2 ? BLADERF_TX_X2 : BLADERF_TX_X1;
}

static unsigned sync_buffers(const struct opts *o)
{
    return o->nbuf ? o->nbuf : NUM_BUFFERS;
}

static unsigned sync_buffer_samples(const struct opts *o)
{
    return o->bufsamp ? o->bufsamp : buffer_samples(o->fmt);
}

static unsigned sync_transfers(const struct opts *o)
{
    return o->nxfer ? o->nxfer : NUM_TRANSFERS;
}

static int sync_config(struct bladerf *dev, const struct opts *o,
                       bladerf_direction d)
{
    CHECK(bladerf_sync_config(dev, layout(o, d), o->fmt, sync_buffers(o),
                              sync_buffer_samples(o), sync_transfers(o),
                              TIMEOUT_MS));
    return 0;
}

/* I and Q of sample n (counting both channels for x2) as unsigned values of
 * the format's width */
static void get_iq(const struct opts *o, const void *buf, size_t n,
                   uint32_t *i, uint32_t *q)
{
    unsigned bits = format_bits(o->fmt);
    uint32_t mask = (bits == 32) ? 0xffffffffu : ((1u << bits) - 1);
    if (is_8bit(o->fmt)) {
        const int8_t *b = buf;
        *i = (uint32_t)(uint8_t)b[2 * n];
        *q = (uint32_t)(uint8_t)b[2 * n + 1];
    } else {
        const int16_t *b = buf;
        *i = (uint32_t)(uint16_t)b[2 * n] & mask;
        *q = (uint32_t)(uint16_t)b[2 * n + 1] & mask;
    }
}

static void put_iq(const struct opts *o, void *buf, size_t n, uint32_t i,
                   uint32_t q)
{
    unsigned bits = is_8bit(o->fmt) ? 8 : 12;   /* the FPGA carries 12 bits */
    if (is_8bit(o->fmt)) {
        int8_t *b = buf;
        b[2 * n]     = (int8_t)(uint8_t)i;
        b[2 * n + 1] = (int8_t)(uint8_t)q;
    } else {
        int16_t *b = buf;
        /* sign-extend from 12 bits: values stay in the Q11 range */
        int32_t si = (int32_t)(i << (32 - bits)) >> (32 - bits);
        int32_t sq = (int32_t)(q << (32 - bits)) >> (32 - bits);
        b[2 * n]     = (int16_t)si;
        b[2 * n + 1] = (int16_t)sq;
    }
}

/* --- Per-channel sequence checking ------------------------------------- */

/* Each received sample is reduced to a sequence number v (modulo 2^vbits)
 * that must advance by exactly "step" per sample once locked. */
struct seq {
    bool locked;
    uint64_t expect;            /* next expected value (masked) */
    uint64_t samples, errors, relocks;
    unsigned reports;
    int run;                    /* consecutive good samples while locking */
    uint64_t last;
};

static void seq_feed(struct seq *s, uint64_t v, uint64_t mask, uint64_t step,
                     int k, uint64_t pos)
{
    s->samples++;
    if (!s->locked) {
        /* lock after 64 consecutive samples in sequence */
        if (s->run > 0 && v == ((s->last + step) & mask)) {
            s->run++;
        } else {
            s->run = 1;
        }
        s->last = v;
        if (s->run >= 64) {
            s->locked = true;
            s->expect = (v + step) & mask;
        }
        return;
    }
    if (v != s->expect) {
        s->errors++;
        if (s->reports++ < MAX_REPORTS) {
            printf("  slot %d sample %" PRIu64 ": expected 0x%" PRIx64
                   ", got 0x%" PRIx64 "\n", k, pos, s->expect, v);
        }
        s->locked = false;      /* relock on the new sequence */
        s->relocks++;
        s->run = 1;
        s->last = v;
        return;
    }
    s->expect = (v + step) & mask;
}

/* Sequence value of an RX counter sample (what arrives, from probing every
 * format):
 *  - 32-bit counter (BLADERF_RX_MUX_32BIT_COUNTER), the default: SC16 carries
 *    all of it (I = bits 15..0, Q = bits 31..16); SC8 carries bits 15..0
 *    (I = 7..0, Q = 15..8); the packed format carries I = bits 11..0 (and
 *    Q = bits 27..16, which changes only every 65536 samples: not used). A
 *    gap that is a multiple of 4096 samples is therefore invisible in the
 *    packed format; gaps from FPGA FIFO overruns have arbitrary lengths.
 *  - 12-bit counters (BLADERF_RX_MUX_12BIT_COUNTER): I counts up, Q down, 12
 *    bits each (SC8 sees bits 11..4, so a step every 16 samples: not
 *    supported here).
 * With two channels, both carry the same value at the same sample time. */
static uint64_t counter_value(const struct opts *o, uint32_t i, uint32_t q,
                              uint64_t *mask)
{
    if (o->mux == BLADERF_RX_MUX_32BIT_COUNTER) {
        if (is_8bit(o->fmt)) {
            *mask = 0xffff;
            return ((uint64_t)q << 8 | i) & *mask;
        }
        if (o->fmt == BLADERF_FORMAT_SC16_Q11_PACKED) {
            *mask = 0xfff;
            return i & *mask;
        }
        *mask = 0xffffffffull;
        return ((uint64_t)q << 16 | i) & *mask;
    }
    *mask = 0xfff;
    return i & *mask;
}

/* Loopback pattern: sample number s of slot k. I carries the low bits of s,
 * Q the next bits and, in its top bit, the slot. */
static void pattern(const struct opts *o, uint64_t s, int k, uint32_t *i,
                    uint32_t *q)
{
    unsigned bits = is_8bit(o->fmt) ? 8 : 12;   /* the FPGA carries 12 bits */
    uint32_t m = (1u << bits) - 1;
    *i = (uint32_t)(s & m);
    *q = (uint32_t)((s >> bits) & (m >> 1)) | ((uint32_t)k << (bits - 1));
}

static uint64_t pattern_value(const struct opts *o, uint32_t i, uint32_t q,
                              int *k, uint64_t *mask)
{
    unsigned bits = is_8bit(o->fmt) ? 8 : 12;
    uint32_t m = (1u << bits) - 1;
    i &= m;
    q &= m;
    *k = (int)(q >> (bits - 1));
    *mask = (1ull << (2 * bits - 1)) - 1;
    return ((uint64_t)(q & (m >> 1)) << bits) | i;
}

/* --- Modes --------------------------------------------------------------- */

static int report(const struct opts *o, struct seq *s, double t, uint64_t ts_gaps,
                  uint64_t overruns, double min_rate_frac)
{
    int k, rc = 0;
    for (k = 0; k < nch(o); k++) {
        double rate = s[k].samples / t;
        printf("slot %d (%s%d): %" PRIu64 " samples in %.2f s (%.3f Msps), "
               "%s, %" PRIu64 " sequence errors\n",
               k, "ch", slot_ch(o, k) + 1, s[k].samples, t, rate / 1e6,
               s[k].locked ? "locked" : "NOT LOCKED", s[k].errors);
        if (!s[k].locked || s[k].errors != 0) {
            rc = 1;
        }
        if (rate < min_rate_frac * o->rate) {
            printf("  rate below %.0f %% of %.3f Msps\n", 100 * min_rate_frac,
                   o->rate / 1e6);
            rc = 1;
        }
    }
    if (is_meta(o->fmt)) {
        printf("timestamp gaps: %" PRIu64 ", overrun flags: %" PRIu64 "\n",
               ts_gaps, overruns);
        if (ts_gaps != 0 || overruns != 0) {
            rc = 1;
        }
    }
    printf("RESULT: %s\n", rc == 0 ? "PASS" : "FAIL");
    return rc;
}

/* One buffer. Returns 0, 2 on errors, or 3 on a time-out if quiet_timeout
 * (the expected end of a loopback once the TX side has stopped). */
static int rx_buffer(struct bladerf *dev, const struct opts *o, void *buf,
                     unsigned n, struct bladerf_metadata *md, uint64_t *next_ts,
                     uint64_t *ts_gaps, uint64_t *overruns, unsigned timeout_ms,
                     bool quiet_timeout)
{
    int status;
    if (is_meta(o->fmt)) {
        memset(md, 0, sizeof(*md));
        md->flags = BLADERF_META_FLAG_RX_NOW;
    }
    status = bladerf_sync_rx(dev, buf, n, is_meta(o->fmt) ? md : NULL, timeout_ms);
    if (status == BLADERF_ERR_TIMEOUT && quiet_timeout) {
        return 3;
    }
    if (status != 0) {
        fprintf(stderr, "ERROR: bladerf_sync_rx: %s\n", bladerf_strerror(status));
        return 2;
    }
    if (!is_meta(o->fmt)) {
        return 0;
    }
    if (md->status & BLADERF_META_STATUS_OVERRUN) {
        (*overruns)++;
        if (*overruns <= MAX_REPORTS) {
            printf("  overrun flag: %u of %u samples valid\n", md->actual_count, n);
        }
    }
    if (*next_ts != 0 && md->timestamp != *next_ts) {
        (*ts_gaps)++;
        if (*ts_gaps <= MAX_REPORTS) {
            printf("  timestamp 0x%" PRIx64 ", expected 0x%" PRIx64 "\n",
                   (uint64_t)md->timestamp, *next_ts);
        }
    }
    *next_ts = md->timestamp + n / nch(o);
    return 0;
}

static int do_probe(struct bladerf *dev, struct opts *o)
{
    unsigned n = buffer_samples(o->fmt), k, j;
    void *buf = calloc(n, sample_bytes(o->fmt));
    uint64_t next_ts = 0, gaps = 0, ovr = 0;
    struct bladerf_metadata md;
    if (buf == NULL) {
        return 2;
    }
    if (setup_channels(dev, o, true, false) != 0) {
        return 2;
    }
    CHECK(bladerf_set_loopback(dev, BLADERF_LB_NONE));
    CHECK(bladerf_set_rx_mux(dev, o->mux));
    if (sync_config(dev, o, BLADERF_RX) != 0 ||
        enable_channels(dev, o, BLADERF_RX, true) != 0) {
        return 2;
    }
    for (j = 0; j < 4; j++) {   /* skip the first buffers */
        if (rx_buffer(dev, o, buf, n, &md, &next_ts, &gaps, &ovr, TIMEOUT_MS, false) != 0) {
            return 2;
        }
    }
    for (k = 0; k < 24; k++) {
        uint32_t i, q;
        get_iq(o, buf, k, &i, &q);
        printf("sample %2u (slot %u): I 0x%04x Q 0x%04x\n", k, k % nch(o), i, q);
    }
    if (o->out != NULL) {       /* the whole buffer, raw */
        FILE *f = fopen(o->out, "wb");
        if (f == NULL || fwrite(buf, sample_bytes(o->fmt), n, f) != n ||
            fclose(f) != 0) {
            fprintf(stderr, "ERROR: writing %s\n", o->out);
        }
    }
    enable_channels(dev, o, BLADERF_RX, false);
    bladerf_set_rx_mux(dev, BLADERF_RX_MUX_BASEBAND);
    free(buf);
    return 0;
}

/* Coverage of an exact, unwrapped sample number: which stretches of samples
 * arrived, in whatever order. libbladeRF sometimes delivers whole receive
 * buffers out of order (seen at the start of streams, with every FPGA image:
 * a buffer arrives a few buffers late and nothing is lost), a host-side
 * defect, not the FPGA's. So every run of consecutive samples is recorded;
 * at the end, stretches that never arrived are losses and stretches that
 * arrived twice are duplicates, and runs that arrived after later samples
 * only count as reordering (a warning). */
#define MAX_RUNS 64
struct cover {
    bool init, open;
    uint64_t rs, re;                    /* open run [rs, re) */
    uint64_t s[MAX_RUNS], e[MAX_RUNS];  /* closed runs, sorted, disjoint */
    unsigned n;
    uint64_t hw, late, dup, overflow;
};

static void cover_close(struct cover *c)
{
    uint64_t s = c->rs, e = c->re;
    unsigned i, j;
    if (!c->open) {
        return;
    }
    c->open = false;
    if (s < c->hw) {
        c->late++;
    }
    if (e > c->hw) {
        c->hw = e;
    }
    /* insert [s, e), counting overlaps as duplicates, merging neighbours */
    for (i = 0; i < c->n && c->e[i] < s; i++) {
    }
    while (i < c->n && c->s[i] <= e) {
        uint64_t os = c->s[i] > s ? c->s[i] : s;
        uint64_t oe = c->e[i] < e ? c->e[i] : e;
        if (oe > os) {
            c->dup += oe - os;
        }
        if (c->s[i] < s) {
            s = c->s[i];
        }
        if (c->e[i] > e) {
            e = c->e[i];
        }
        for (j = i; j + 1 < c->n; j++) {
            c->s[j] = c->s[j + 1];
            c->e[j] = c->e[j + 1];
        }
        c->n--;
    }
    if (c->n == MAX_RUNS) {
        c->overflow++;
        return;
    }
    for (j = c->n; j > i; j--) {
        c->s[j] = c->s[j - 1];
        c->e[j] = c->e[j - 1];
    }
    c->s[i] = s;
    c->e[i] = e;
    c->n++;
}

static void cover_feed(struct cover *c, uint64_t u)
{
    if (c->open && u == c->re) {
        c->re++;
        return;
    }
    cover_close(c);
    c->open = true;
    c->rs = u;
    c->re = u + 1;
}

/* Missing samples below "settled" (later ones may still be in flight) */
static int cover_report(int k, struct cover *c, uint64_t tail)
{
    uint64_t missing = 0, settled;
    unsigned i;
    cover_close(c);
    settled = c->hw > tail ? c->hw - tail : 0;
    for (i = 0; i + 1 < c->n; i++) {
        uint64_t gs = c->e[i], ge = c->s[i + 1];
        if (gs < settled) {
            missing += (ge < settled ? ge : settled) - gs;
        }
    }
    printf("slot %d coverage: %" PRIu64 " samples missing, %" PRIu64
           " duplicated, %" PRIu64 " late runs\n", k, missing, c->dup, c->late);
    if (c->late != 0 && missing == 0 && c->dup == 0) {
        printf("WARNING: libbladeRF delivered %" PRIu64 " runs of samples out of "
               "order (none lost)\n", c->late);
    }
    return (missing != 0 || c->dup != 0 || c->overflow != 0) ? 1 : 0;
}

static int do_rxcheck(struct bladerf *dev, struct opts *o)
{
    unsigned n = buffer_samples(o->fmt), j, per = n / (unsigned)nch(o);
    void *buf = calloc(n, sample_bytes(o->fmt));
    struct bladerf_metadata md;
    struct cover cv[2];
    uint64_t next_ts = 0, gaps = 0, ovr = 0, pos = 0, skew = 0;
    uint64_t jumps[2] = { 0, 0 }, samples[2] = { 0, 0 };
    uint64_t expect[2] = { 0, 0 }, uprev[2] = { 0, 0 }, off = 0;
    bool init[2] = { false, false }, have_off = false;
    /* exact sample numbers: SC16's 32-bit counter; SC8 meta: its 16-bit
     * counter placed by the timestamps */
    bool exact = format_bits(o->fmt) == 16 ||
                 o->fmt == BLADERF_FORMAT_SC8_Q7_META;
    unsigned reports = 0;
    double t0, t;
    int rc = 0, k;
    if (buf == NULL) {
        return 2;
    }
    if (o->mux != BLADERF_RX_MUX_32BIT_COUNTER) {
        fprintf(stderr, "ERROR: rxcheck needs the 32-bit counter\n");
        return 2;
    }
    memset(cv, 0, sizeof(cv));
    if (setup_channels(dev, o, true, false) != 0) {
        return 2;
    }
    CHECK(bladerf_set_loopback(dev, BLADERF_LB_NONE));
    CHECK(bladerf_set_rx_mux(dev, o->mux));
    if (sync_config(dev, o, BLADERF_RX) != 0 ||
        enable_channels(dev, o, BLADERF_RX, true) != 0) {
        return 2;
    }
    printf("rxcheck: %s, %s, %.3f Msps, %.0f s after %.2f s of warm-up\n",
           o->fmt_name, o->x2 ? "x2" : "x1", o->rate / 1e6, o->seconds, o->warmup);
    /* Warm-up: receive without checking. libbladeRF's start-up reordering
     * (see struct cover) happens in the first ~10 ms; -W 0 checks it too. */
    t0 = now();
    while (now() - t0 < o->warmup) {
        if (rx_buffer(dev, o, buf, n, &md, &next_ts, &gaps, &ovr, TIMEOUT_MS, false) != 0) {
            rc = 2;
            goto out;
        }
    }
    ovr = 0;
    t0 = now();
    do {
        uint64_t v0 = 0;
        if (rx_buffer(dev, o, buf, n, &md, &next_ts, &gaps, &ovr, TIMEOUT_MS, false) != 0) {
            rc = 2;
            goto out;
        }
        for (j = 0; j < n; j++) {
            uint32_t i, q;
            uint64_t mask, v;
            unsigned js = j / (unsigned)nch(o);
            k = (int)(j % (unsigned)nch(o));
            get_iq(o, buf, j, &i, &q);
            v = counter_value(o, i, q, &mask);
            if (init[k] && v != expect[k]) {
                jumps[k]++;
                if (reports++ < MAX_REPORTS) {
                    printf("  slot %d sample %" PRIu64 "%s: expected 0x%" PRIx64
                           ", got 0x%" PRIx64 "\n", k, pos + js,
                           js == 0 ? " (buffer start)" : "", expect[k], v);
                }
            }
            if (exact) {
                uint64_t u;
                if (o->fmt == BLADERF_FORMAT_SC8_Q7_META) {
                    /* predicted from the timestamp, corrected by the counter */
                    uint64_t pr;
                    if (!have_off) {
                        off = v - (md.timestamp + js);
                        have_off = true;
                    }
                    pr = md.timestamp + js + off;
                    u = pr + (uint64_t)(int64_t)(int16_t)(uint16_t)(v - pr);
                } else if (!init[k]) {
                    u = v;
                } else {
                    u = uprev[k] + 1 +
                        (uint64_t)(int64_t)(int32_t)(uint32_t)(v - (uint32_t)(uprev[k] + 1));
                }
                cover_feed(&cv[k], u);
                uprev[k] = u;
            }
            init[k] = true;
            expect[k] = (v + 1) & mask;
            samples[k]++;
            if (k == 1 && v != v0 && skew++ < MAX_REPORTS) {
                printf("  sample %" PRIu64 ": channel 1 0x%" PRIx64
                       ", channel 2 0x%" PRIx64 "\n", pos + js, v0, v);
            }
            v0 = v;
        }
        pos += per;
        t = now() - t0;
    } while (t < o->seconds);
    for (k = 0; k < nch(o); k++) {
        double rate = samples[k] / t;
        printf("slot %d (ch%d): %" PRIu64 " samples in %.2f s (%.3f Msps), "
               "%" PRIu64 " sequence jumps\n", k, slot_ch(o, k) + 1, samples[k],
               t, rate / 1e6, jumps[k]);
        if (rate < 0.98 * o->rate) {
            printf("  rate below 98 %%\n");
            rc = 1;
        }
        if (exact) {
            rc |= cover_report(k, &cv[k], 8ull * per);
        } else if (jumps[k] != 0) {
            /* SC8 and packed: the counter is too short to tell late samples
             * from lost ones; any jump fails */
            printf("  sequence jumps in a format that cannot tell late from lost\n");
            rc = 1;
        }
    }
    if (is_meta(o->fmt)) {
        printf("overrun flags: %" PRIu64 "\n", ovr);
        if (ovr != 0) {
            rc = 1;
        }
    }
    if (o->x2) {
        printf("channel skew errors: %" PRIu64 "\n", skew);
        if (skew != 0) {
            rc = 1;
        }
    }
    printf("RESULT: %s\n", rc == 0 ? "PASS" : "FAIL");
out:
    enable_channels(dev, o, BLADERF_RX, false);
    bladerf_set_rx_mux(dev, BLADERF_RX_MUX_BASEBAND);
    free(buf);
    return rc;
}

/* The loopback runs on libbladeRF's asynchronous interface, as Nuand's
 * test_digital_loopback does: with the synchronous interface on both sides
 * the FPGA loopback path never moved a sample (TX and RX transfers both timed
 * out, whatever the order of enabling; the Nios register writes were the
 * same as test_digital_loopback's). The asynchronous interface does not
 * support the packed or meta formats, so the loopback covers SC16 and SC8. */
struct lbctx {
    const struct opts *o;
    void **buffers;
    size_t num, idx;
    volatile bool stop;
    uint64_t count;             /* samples per channel (TX: sent) */
    struct seq s[2];
    uint64_t tag_errors, warmup;    /* samples per channel not checked */
    unsigned n;
};

static void *lb_tx_cb(struct bladerf *dev, struct bladerf_stream *stream,
                      struct bladerf_metadata *meta, void *samples,
                      size_t num_samples, void *user_data)
{
    struct lbctx *c = user_data;
    const struct opts *o = c->o;
    void *buf;
    size_t j;
    (void)dev; (void)stream; (void)meta; (void)samples; (void)num_samples;
    if (c->stop) {
        return BLADERF_STREAM_SHUTDOWN;
    }
    buf = c->buffers[c->idx];
    c->idx = (c->idx + 1) % c->num;
    for (j = 0; j < c->n; j++) {
        uint32_t i, q;
        int k = (int)(j % nch(o));
        pattern(o, c->count + j / nch(o), k, &i, &q);
        put_iq(o, buf, j, i, q);
    }
    c->count += c->n / nch(o);
    return buf;
}

static void *lb_rx_cb(struct bladerf *dev, struct bladerf_stream *stream,
                      struct bladerf_metadata *meta, void *samples,
                      size_t num_samples, void *user_data)
{
    struct lbctx *c = user_data;
    const struct opts *o = c->o;
    size_t j;
    void *next;
    (void)dev; (void)stream; (void)meta;
    if (samples != NULL && c->count < c->warmup) {
        /* warm-up: the loopback's start-up (TX runs before RX is enabled)
         * once lost a few samples at a fixed point, ~78,000 samples in */
        c->count += num_samples / nch(o);
    } else if (samples != NULL) {
        for (j = 0; j < num_samples; j++) {
            uint32_t i, q;
            uint64_t mask, v;
            int k = (int)(j % nch(o)), tag;
            get_iq(o, samples, j, &i, &q);
            v = pattern_value(o, i, q, &tag, &mask);
            if (c->s[k].locked && tag != k && c->tag_errors++ < MAX_REPORTS) {
                printf("  slot %d sample %" PRIu64 ": channel tag %d\n", k,
                       c->count + j / nch(o), tag);
            }
            seq_feed(&c->s[k], v, mask, 1, k, c->count + j / nch(o));
        }
        c->count += num_samples / nch(o);
    }
    if (c->stop) {
        return BLADERF_STREAM_SHUTDOWN;
    }
    next = c->buffers[c->idx];
    c->idx = (c->idx + 1) % c->num;
    return next;
}

struct lbthread {
    struct bladerf_stream *stream;
    bladerf_channel_layout layout;
    int status;
};

static void *lb_stream_thread(void *arg)
{
    struct lbthread *t = arg;
    t->status = bladerf_stream(t->stream, t->layout);
    return NULL;
}

static int do_loopback(struct bladerf *dev, struct opts *o)
{
    struct lbctx rx = { .o = o }, tx = { .o = o };
    struct bladerf_stream *rxs = NULL, *txs = NULL;
    struct lbthread rt, tt;
    pthread_t rth, tth;
    unsigned n = 32768;         /* samples per buffer, as test_digital_loopback */
    double t0, t;
    int rc, k;

    if (o->fmt != BLADERF_FORMAT_SC16_Q11 && o->fmt != BLADERF_FORMAT_SC8_Q7) {
        fprintf(stderr, "ERROR: loopback supports sc16 and sc8 only\n");
        return 2;
    }
    rx.n = tx.n = n;
    rx.num = tx.num = 256;
    rx.warmup = (uint64_t)(o->warmup * o->rate);
    if (setup_channels(dev, o, true, true) != 0) {
        return 2;
    }
    CHECK(bladerf_init_stream(&rxs, dev, lb_rx_cb, &rx.buffers, rx.num, o->fmt,
                              n, NUM_TRANSFERS, &rx));
    CHECK(bladerf_init_stream(&txs, dev, lb_tx_cb, &tx.buffers, tx.num, o->fmt,
                              n, NUM_TRANSFERS, &tx));
    CHECK(bladerf_set_stream_timeout(dev, BLADERF_RX, TIMEOUT_MS));
    CHECK(bladerf_set_stream_timeout(dev, BLADERF_TX, TIMEOUT_MS));
    if (o->lb_rfic) {
        CHECK(bladerf_set_rx_mux(dev, BLADERF_RX_MUX_BASEBAND));
        CHECK(bladerf_set_loopback(dev, BLADERF_LB_RFIC_BIST));
    } else {
        CHECK(bladerf_set_loopback(dev, BLADERF_LB_NONE));
        CHECK(bladerf_set_rx_mux(dev, BLADERF_RX_MUX_DIGITAL_LOOPBACK));
    }
    printf("loopback: %s, %s, %.3f Msps, %.0f s, %s\n", o->fmt_name,
           o->x2 ? "x2" : "x1", o->rate / 1e6, o->seconds,
           o->lb_rfic ? "in the AD9361 (BLADERF_LB_RFIC_BIST)"
                      : "in the FPGA (BLADERF_RX_MUX_DIGITAL_LOOPBACK)");
    tt.stream = txs;
    tt.layout = layout(o, BLADERF_TX);
    rt.stream = rxs;
    rt.layout = layout(o, BLADERF_RX);
    if (enable_channels(dev, o, BLADERF_TX, true) != 0) {
        if (o->lb_rfic) {
            bladerf_set_loopback(dev, BLADERF_LB_NONE);
        }
        return 2;
    }
    pthread_create(&tth, NULL, lb_stream_thread, &tt);
    if (enable_channels(dev, o, BLADERF_RX, true) != 0) {
        tx.stop = true;
        pthread_join(tth, NULL);
        enable_channels(dev, o, BLADERF_TX, false);
        if (o->lb_rfic) {
            bladerf_set_loopback(dev, BLADERF_LB_NONE);
        }
        return 2;
    }
    pthread_create(&rth, NULL, lb_stream_thread, &rt);
    t0 = now();
    while ((t = now() - t0) < o->seconds) {
        usleep(100000);
    }
    tx.stop = true;
    rx.stop = true;
    pthread_join(tth, NULL);
    pthread_join(rth, NULL);
    enable_channels(dev, o, BLADERF_RX, false);
    enable_channels(dev, o, BLADERF_TX, false);
    bladerf_set_rx_mux(dev, BLADERF_RX_MUX_BASEBAND);
    if (o->lb_rfic) {
        bladerf_set_loopback(dev, BLADERF_LB_NONE);
    }
    bladerf_deinit_stream(rxs);
    bladerf_deinit_stream(txs);
    printf("TX: %" PRIu64 " samples per channel sent (stream status %d), "
           "RX stream status %d\n", tx.count, tt.status, rt.status);
    printf("channel tag errors: %" PRIu64 "\n", rx.tag_errors);
    printf("warm-up: first %" PRIu64 " samples per channel not checked\n", rx.warmup);
    rc = report(o, rx.s, t, 0, 0, 0.95);
    for (k = 0; k < nch(o); k++) {
        /* what was sent came back, except what was still in flight */
        if (rx.s[k].samples + rx.warmup + (uint64_t)NUM_TRANSFERS * 2 * n < tx.count) {
            printf("  slot %d: only %" PRIu64 " of %" PRIu64 " samples came back\n",
                   k, rx.s[k].samples, tx.count);
            rc = 1;
        }
    }
    if (rx.tag_errors != 0 || tt.status < 0 || rt.status < 0) {
        rc = 1;
    }
    if (rc != 0) {
        printf("RESULT: FAIL\n");
    }
    return rc;
}

static int do_rxcap(struct bladerf *dev, struct opts *o)
{
    unsigned n = buffer_samples(o->fmt);
    size_t sb = sample_bytes(o->fmt);
    uint64_t total = (o->nsamples + n - 1) / n * n, got = 0;
    uint64_t next_ts = 0, gaps = 0, ovr = 0;
    struct bladerf_metadata md;
    char *buf = malloc(total * sb);
    double t0, t;
    FILE *f;
    if (buf == NULL || o->out == NULL || o->x2) {
        fprintf(stderr, "ERROR: rxcap needs -n, -o, and one channel\n");
        return 2;
    }
    if (setup_channels(dev, o, true, false) != 0) {
        return 2;
    }
    CHECK(bladerf_set_loopback(dev, BLADERF_LB_NONE));
    CHECK(bladerf_set_rx_mux(dev, BLADERF_RX_MUX_BASEBAND));
    if (sync_config(dev, o, BLADERF_RX) != 0 ||
        enable_channels(dev, o, BLADERF_RX, true) != 0) {
        return 2;
    }
    /* Warm-up, as rxcheck's: receive without keeping for WARMUP_S, and at
     * least 16 buffers. libbladeRF's start-up reordering (see struct cover)
     * delivers one of the stream's first ~32 buffers late; 16 buffers were
     * not enough to skip it (2026-11-20: every PRI error of pulse-check's
     * oc-rx cases fell in the stream's buffers 16 to 33). */
    t0 = now();
    for (int j = 0; j < 16 || now() - t0 < o->warmup; j++) {
        if (rx_buffer(dev, o, buf, n, &md, &next_ts, &gaps, &ovr, TIMEOUT_MS, false) != 0) {
            return 2;
        }
    }
    next_ts = 0;
    gaps = ovr = 0;
    t0 = now();
    while (got < total) {
        if (rx_buffer(dev, o, buf + got * sb, n, &md, &next_ts, &gaps, &ovr, TIMEOUT_MS, false) != 0) {
            enable_channels(dev, o, BLADERF_RX, false);
            return 2;
        }
        got += n;
    }
    t = now() - t0;
    enable_channels(dev, o, BLADERF_RX, false);
    printf("rxcap: %" PRIu64 " samples in %.3f s (%.3f Msps by the host clock)\n",
           got, t, got / t / 1e6);
    if (is_meta(o->fmt)) {
        printf("timestamp gaps: %" PRIu64 ", overrun flags: %" PRIu64 "\n", gaps, ovr);
    }
    f = fopen(o->out, "wb");
    if (f == NULL || fwrite(buf, sb, got, f) != got || fclose(f) != 0) {
        fprintf(stderr, "ERROR: writing %s: %s\n", o->out, strerror(errno));
        return 2;
    }
    free(buf);
    if (is_meta(o->fmt) && (gaps != 0 || ovr != 0)) {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}

static int do_tx(struct bladerf *dev, struct opts *o)
{
    size_t sb = sample_bytes(o->fmt);
    unsigned n = buffer_samples(o->fmt), k;
    uint64_t len = 0, j, sent = 0;
    void *one = NULL, *buf;
    struct bladerf_metadata md;
    double t0, tl;
    bool first = true;
    int status = 0;

    if (o->in != NULL) {
        FILE *f = fopen(o->in, "rb");
        long sz = 0;
        if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) <= 0) {
            fprintf(stderr, "ERROR: cannot read %s\n", o->in);
            return 2;
        }
        rewind(f);
        len = (uint64_t)sz / sb;
        one = malloc(len * sb);
        if (one == NULL || fread(one, sb, len, f) != len) {
            fprintf(stderr, "ERROR: cannot read %s\n", o->in);
            return 2;
        }
        fclose(f);
    } else if (o->have_tone) {
        /* the shortest whole number of tone periods that is a whole number
         * of samples, so that the repeats join without a phase step (or one
         * second's worth if there is none) */
        double w = 2 * M_PI * o->tone / o->rate;
        unsigned bits = is_8bit(o->fmt) ? 8 : 12;
        double a = 0.5 * ((1 << (bits - 1)) - 1);
        uint64_t p;
        len = (uint64_t)o->rate;
        if (o->tone != 0) {
            double per = o->rate / fabs(o->tone);
            for (p = 1; p <= 100000; p++) {
                double m = p * per;
                if (fabs(m - llround(m)) < 1e-6 && m <= o->rate) {
                    len = (uint64_t)llround(m);
                    break;
                }
            }
        }
        one = malloc(len * sb);
        if (one == NULL) {
            return 2;
        }
        for (j = 0; j < len; j++) {
            put_iq(o, one, j, (uint32_t)(int32_t)lround(a * cos(w * j)),
                   (uint32_t)(int32_t)lround(a * sin(w * j)));
        }
    } else {
        fprintf(stderr, "ERROR: tx needs -i FILE or -T TONE_HZ\n");
        return 2;
    }
    /* stream buffer: n samples per slot, filled from "one" cyclically */
    buf = malloc((size_t)n * sb);
    if (buf == NULL) {
        return 2;
    }
    if (setup_channels(dev, o, false, true) != 0 ||
        sync_config(dev, o, BLADERF_TX) != 0 ||
        enable_channels(dev, o, BLADERF_TX, true) != 0) {
        return 2;
    }
    printf("tx: %s, %s, %.3f Msps, %" PRIu64 "-sample waveform, %.1f s\n",
           o->fmt_name, o->x2 ? "x2" : "x1", o->rate / 1e6, len, o->seconds);
    fflush(stdout);
    t0 = now();
    j = 0;
    while (now() - t0 < o->seconds) {
        for (k = 0; k < n; k++) {
            /* x2: each sample goes to both channels */
            uint64_t src = j % len;
            memcpy((char *)buf + (size_t)k * sb, (char *)one + src * sb, sb);
            if (!o->x2 || (k % 2) == 1) {
                j++;
            }
        }
        memset(&md, 0, sizeof(md));
        if (first) {
            md.flags = BLADERF_META_FLAG_TX_BURST_START | BLADERF_META_FLAG_TX_NOW;
        }
        status = bladerf_sync_tx(dev, buf, n, is_meta(o->fmt) ? &md : NULL,
                                 TIMEOUT_MS);
        if (status != 0) {
            fprintf(stderr, "ERROR: bladerf_sync_tx: %s\n", bladerf_strerror(status));
            break;
        }
        first = false;
        sent += n;
    }
    tl = now() - t0;
    if (is_meta(o->fmt)) {
        memset(buf, 0, (size_t)n * sb);
        memset(&md, 0, sizeof(md));
        md.flags = BLADERF_META_FLAG_TX_BURST_END;
        bladerf_sync_tx(dev, buf, n, &md, TIMEOUT_MS);
    }
    /* let the last buffers go out before disabling */
    usleep(200000);
    enable_channels(dev, o, BLADERF_TX, false);
    printf("tx: %" PRIu64 " samples sent in %.2f s\n", sent, now() - t0);
    /* samples per channel; the first ones only fill libbladeRF's buffers */
    printf("tx: libbladeRF took %.3f Msps per channel while sending (%.1f %% of %.3f)\n",
           (o->x2 ? sent / 2.0 : (double)sent) / tl / 1e6,
           100 * (o->x2 ? sent / 2.0 : (double)sent) / tl / o->rate, o->rate / 1e6);
    free(buf);
    free(one);
    if (status != 0) {
        printf("RESULT: FAIL\n");
        return 2;
    }
    printf("RESULT: PASS\n");
    return 0;
}

/* --- Scheduled bursts ---------------------------------------------------- */

/* rxsched -o: the file for burst n, "%llu" or "%0Wllu" in pat replaced by n;
 * false if pat has neither */
static bool burst_file_name(char *out, size_t len, const char *pat,
                            unsigned long long n)
{
    const char *p = strchr(pat, '%'), *q;
    int width = 0;
    if (p == NULL) {
        return false;
    }
    for (q = p + 1; *q >= '0' && *q <= '9' && width < 100; q++) {
        width = width * 10 + (*q - '0');
    }
    if (strncmp(q, "llu", 3) != 0) {
        return false;
    }
    snprintf(out, len, "%.*s%0*llu%s", (int)(p - pat), pat, width, n, q + 3);
    return true;
}

static int do_rxsched(struct bladerf *dev, struct opts *o)
{
    unsigned nc = (unsigned)nch(o);
    uint64_t per = (uint64_t)llround(o->burst_ms * 1e-3 * o->rate);
    uint64_t lead = (uint64_t)llround(o->lead_ms * 1e-3 * o->rate);
    uint64_t total = per * nc, mask = 0, ref_ts = 0, ref_v = 0;
    uint64_t bursts = 0, bad = 0, past = 0, ovr = 0, gaps = 0, samples = 0;
    uint64_t next_ts = 0;               /* -N: where the next burst must start */
    bool have_next = false;
    unsigned reports = 0;
    struct bladerf_metadata md;
    double t0, t1, cyc, cyc_max = 0, cyc_sum = 0;
    /* where the time goes: the timestamp read (a request to the FPGA), the
     * scheduled read (lead + burst + catching up) and the file write */
    double ts_lat = 0, ts_lat_sum = 0, ts_lat_max = 0, rx_t, rx_sum = 0, rx_max = 0;
    double wr_t, wr_sum = 0, wr_max = 0;
    uint64_t ts_lat_slow = 0, nts = 0, nrx = 0, nwr = 0;
    /* The pause between two reads (file write, gap, timestamp read): the
     * stream keeps running, and once the buffers not in flight are full
     * (sync_worker.c's RX callback finds the next one not empty) every
     * transfer is dropped, in runs of NUM_TRANSFERS (resubmit_count) that
     * carry on even after the read frees buffers. A read scheduled closer
     * than the rest of such a run is "already past". */
    unsigned nbuf = sync_buffers(o), nxfer = sync_transfers(o);
    double xfer_s = (double)sync_buffer_samples(o) / (o->rate * nc);
    double buf_s = (nbuf - nxfer) * xfer_s;
    double drop_s = (nxfer - 1) * xfer_s;
    double t_rx_end = 0, pause = 0, pause_max = 0;
    uint64_t long_pauses = 0, lost_after_long = 0;
    bool have_ref = false;
    char path[4096];
    bool file_per_burst = o->out != NULL && burst_file_name(path, sizeof(path), o->out, 0);
    char *buf;
    int rc = 0, status;

    if (!is_meta(o->fmt) || o->mux != BLADERF_RX_MUX_32BIT_COUNTER ||
        per == 0 || total > UINT_MAX || nxfer >= nbuf) {
        fprintf(stderr, "ERROR: rxsched needs a meta format, the 32-bit counter, "
                        "a burst of 1 to %u samples and more buffers than "
                        "transfers\n", UINT_MAX);
        return 2;
    }
    buf = malloc(total * sample_bytes(o->fmt));
    if (buf == NULL) {
        return 2;
    }
    if (setup_channels(dev, o, true, false) != 0) {
        free(buf);
        return 2;
    }
    CHECK(bladerf_set_loopback(dev, BLADERF_LB_NONE));
    CHECK(bladerf_set_rx_mux(dev, o->mux));
    if (sync_config(dev, o, BLADERF_RX) != 0 ||
        enable_channels(dev, o, BLADERF_RX, true) != 0) {
        free(buf);
        return 2;
    }
    if (o->rx_now) {
        printf("rxsched: %s, %s, %.3f Msps, %.0f ms bursts (%" PRIu64 " samples per "
               "channel) read now, back to back, %s%s%swait %.0f ms, for %.0f s\n",
               o->fmt_name, o->x2 ? "x2" : "x1", o->rate / 1e6, o->burst_ms, per,
               o->out ? "each written to " : "", o->out ? o->out : "",
               o->out ? ", then " : "then ", o->gap_ms, o->seconds);
    } else {
        printf("rxsched: %s, %s, %.3f Msps, %.0f ms bursts (%" PRIu64 " samples per "
               "channel) %.1f ms ahead, then %s%s%swait %.0f ms, for %.0f s\n",
               o->fmt_name, o->x2 ? "x2" : "x1", o->rate / 1e6, o->burst_ms, per,
               o->lead_ms, o->out ? "a write to " : "", o->out ? o->out : "",
               o->out ? " and " : "", o->gap_ms, o->seconds);
    }
    printf("rxsched: libbladeRF's buffers fill in %.1f ms (%u of %u samples, %u in "
           "flight); after an overrun its RX worker drops up to %.1f ms more\n",
           1e3 * buf_s, nbuf, sync_buffer_samples(o), nxfer, 1e3 * drop_s);
    t0 = t1 = now();
    while (now() - t0 < o->seconds) {
        uint64_t ts_now = 0, ts = 0, j, v0 = 0;
        bool burst_bad = false, lost = false;
        double ta;
        memset(&md, 0, sizeof(md));
        if (o->rx_now) {
            md.flags = BLADERF_META_FLAG_RX_NOW;    /* the next samples queued */
        } else {
            ta = now();
            CHECK(bladerf_get_timestamp(dev, BLADERF_RX, &ts_now));
            ts_lat = now() - ta;
            nts++;
            ts_lat_sum += ts_lat;
            if (ts_lat > ts_lat_max) {
                ts_lat_max = ts_lat;
            }
            if (ts_lat > 1e-3) {
                ts_lat_slow++;
            }
            ts = ts_now + lead;
            md.timestamp = ts;                  /* scheduled */
        }
        ta = now();
        pause = (t_rx_end > 0) ? ta - t_rx_end : 0;
        if (pause > pause_max) {
            pause_max = pause;
        }
        if (pause > buf_s) {
            long_pauses++;
        }
        status = bladerf_sync_rx(dev, buf, (unsigned)total, &md, TIMEOUT_MS);
        t_rx_end = now();
        rx_t = t_rx_end - ta;
        bursts++;
        if (status == BLADERF_ERR_TIME_PAST) {
            uint64_t ts_after = 0;
            past++;
            bad++;
            if (pause > buf_s) {
                lost_after_long++;
            }
            bladerf_get_timestamp(dev, BLADERF_RX, &ts_after);
            if (reports++ < MAX_REPORTS) {
                printf("  burst %" PRIu64 ": timestamp 0x%" PRIx64 " already past (timestamp "
                       "read took %.2f ms; the read gave up after %.2f ms; %.2f ms later "
                       "the FPGA was %.2f ms past it; %.1f ms since the previous read)\n",
                       bursts, ts, 1e3 * ts_lat, 1e3 * rx_t, 1e3 * (now() - ta - rx_t),
                       1e3 * ((double)(int64_t)(ts_after - ts)) / o->rate, 1e3 * pause);
            }
            continue;
        }
        nrx++;
        rx_sum += rx_t;
        if (rx_t > rx_max) {
            rx_max = rx_t;
        }
        if (status != 0) {
            fprintf(stderr, "ERROR: burst %" PRIu64 ": bladerf_sync_rx: %s\n",
                    bursts, bladerf_strerror(status));
            rc = 2;
            break;
        }
        if (o->rx_now) {
            if (md.actual_count != total || (md.status & BLADERF_META_STATUS_OVERRUN)) {
                /* a recorder loses this burst: an overrun cut it short, and
                 * the next one starts after the gap */
                lost = burst_bad = true;
                ovr++;
                if (pause > buf_s) {
                    lost_after_long++;
                }
                have_next = false;
                if (reports++ < MAX_REPORTS) {
                    printf("  burst %" PRIu64 ": overrun, %u of %" PRIu64 " samples (%.1f ms "
                           "since the previous read)\n", bursts, md.actual_count, total,
                           1e3 * pause);
                }
            } else {
                if (have_next && md.timestamp != next_ts) {
                    burst_bad = true;
                    gaps++;
                    if (reports++ < MAX_REPORTS) {
                        printf("  burst %" PRIu64 ": starts at 0x%" PRIx64 ", not at 0x%"
                               PRIx64 " where the previous one ended, and no overrun "
                               "was reported\n", bursts, (uint64_t)md.timestamp, next_ts);
                    }
                }
                next_ts = md.timestamp + per;
                have_next = true;
            }
        } else if (md.timestamp != ts || md.actual_count != total ||
            (md.status & BLADERF_META_STATUS_OVERRUN)) {
            burst_bad = true;
            if (reports++ < MAX_REPORTS) {
                printf("  burst %" PRIu64 ": asked for 0x%" PRIx64 ", got 0x%" PRIx64
                       ", %u of %" PRIu64 " samples%s\n", bursts, ts,
                       (uint64_t)md.timestamp, md.actual_count, total,
                       (md.status & BLADERF_META_STATUS_OVERRUN) ? ", overrun" : "");
            }
        }
        for (j = 0; j < (uint64_t)md.actual_count && !burst_bad; j++) {
            uint32_t i, q;
            uint64_t v = 0, js;
            if (o->rx_now && j == nc && md.actual_count > 2 * nc) {
                /* -N: the first and last counter values only (a gap inside
                 * moves the last one), so that the time between reads stays
                 * a recorder's: checking every sample takes ~17 ms a burst */
                j = md.actual_count - nc;
            }
            js = j / nc;
            get_iq(o, buf, (size_t)j, &i, &q);
            v = counter_value(o, i, q, &mask);
            if (j == 0) {
                v0 = v;
                if (!have_ref) {
                    ref_ts = md.timestamp;
                    ref_v = v;
                    have_ref = true;
                } else if (((v - ref_v) & mask) != ((md.timestamp - ref_ts) & mask)) {
                    burst_bad = true;
                    if (reports++ < MAX_REPORTS) {
                        printf("  burst %" PRIu64 ": counter 0x%" PRIx64 " at timestamp 0x%"
                               PRIx64 ": %" PRId64 " samples off the timestamp\n",
                               bursts, v, (uint64_t)md.timestamp,
                               (int64_t)(((v - ref_v) - (md.timestamp - ref_ts)) & mask));
                    }
                }
            } else if (v != ((v0 + js) & mask)) {
                burst_bad = true;
                if (reports++ < MAX_REPORTS) {
                    printf("  burst %" PRIu64 " sample %" PRIu64 " (slot %u): expected 0x%"
                           PRIx64 ", got 0x%" PRIx64 "\n", bursts, js,
                           (unsigned)(j % nc), (v0 + js) & mask, v);
                }
            }
        }
        if (burst_bad) {
            bad++;
        } else {
            samples += per;
        }
        if (o->out != NULL && !lost) {
            FILE *f;
            const char *name = o->out;
            if (file_per_burst) {
                burst_file_name(path, sizeof(path), o->out, (unsigned long long)bursts);
                name = path;
            }
            ta = now();
            f = fopen(name, "wb");
            if (f == NULL || fwrite(buf, sample_bytes(o->fmt), (size_t)total, f) != total ||
                fclose(f) != 0) {
                fprintf(stderr, "ERROR: cannot write %s\n", name);
                rc = 2;
                break;
            }
            wr_t = now() - ta;
            nwr++;
            wr_sum += wr_t;
            if (wr_t > wr_max) {
                wr_max = wr_t;
            }
        }
        if (o->gap_ms > 0) {
            usleep((useconds_t)(o->gap_ms * 1e3));
        }
        cyc = now() - t1;
        t1 = now();
        cyc_sum += cyc;
        if (cyc > cyc_max) {
            cyc_max = cyc;
        }
    }
    enable_channels(dev, o, BLADERF_RX, false);
    free(buf);
    if (o->rx_now) {
        printf("rxsched: %" PRIu64 " bursts, %" PRIu64 " bad (%" PRIu64 " lost to overruns, %"
               PRIu64 " gaps without one), %" PRIu64 " samples per channel in complete "
               "bursts (first and last counter values checked); burst cycle %.1f ms mean, "
               "%.1f ms max\n", bursts, bad, ovr, gaps, samples,
               bursts ? 1e3 * cyc_sum / (double)bursts : 0.0, 1e3 * cyc_max);
        printf("rxsched: read %.1f ms mean, %.1f ms max; file write %.1f ms mean, %.1f ms "
               "max\n", nrx ? 1e3 * rx_sum / (double)nrx : 0.0, 1e3 * rx_max,
               nwr ? 1e3 * wr_sum / (double)nwr : 0.0, 1e3 * wr_max);
    } else {
        printf("rxsched: %" PRIu64 " bursts, %" PRIu64 " bad (%" PRIu64 " too late to "
               "schedule), %" PRIu64 " samples per channel checked; burst cycle %.1f ms "
               "mean, %.1f ms max\n", bursts, bad, past, samples,
               bursts ? 1e3 * cyc_sum / (double)bursts : 0.0, 1e3 * cyc_max);
        printf("rxsched: timestamp read %.2f ms mean, %.2f ms max, %" PRIu64 " over 1 ms; "
               "scheduled read %.1f ms mean, %.1f ms max; file write %.1f ms mean, %.1f ms "
               "max\n", nts ? 1e3 * ts_lat_sum / (double)nts : 0.0, 1e3 * ts_lat_max,
               ts_lat_slow, nrx ? 1e3 * rx_sum / (double)nrx : 0.0, 1e3 * rx_max,
               nwr ? 1e3 * wr_sum / (double)nwr : 0.0, 1e3 * wr_max);
    }
    printf("rxsched: pauses between reads %.1f ms max, %" PRIu64 " longer than the "
           "%.1f ms the buffers last; %" PRIu64 " of the %" PRIu64 " %s followed one\n",
           1e3 * pause_max, long_pauses, 1e3 * buf_s, lost_after_long,
           o->rx_now ? ovr : past, o->rx_now ? "overruns" : "too late");
    if (rc == 0 && (bad != 0 || bursts == 0)) {
        rc = 1;
    }
    printf("RESULT: %s\n", rc == 0 ? "PASS" : "FAIL");
    return rc;
}

/* --- Triggers ------------------------------------------------------------ */

struct rxwait {
    struct bladerf *dev;
    const struct opts *o;
    volatile bool done;
    volatile double t_done;
    int status;
    void *buf;
    unsigned n;
};

static void *rx_first_thread(void *arg)
{
    struct rxwait *w = arg;
    struct bladerf_metadata md;
    memset(&md, 0, sizeof(md));
    md.flags = BLADERF_META_FLAG_RX_NOW;
    w->status = bladerf_sync_rx(w->dev, w->buf, w->n,
                                is_meta(w->o->fmt) ? &md : NULL, 6000);
    w->t_done = now();
    w->done = true;
    return NULL;
}

static int trig_state(struct bladerf *dev, struct bladerf_trigger *tr,
                      const char *when, bool *armed, bool *fired)
{
    bool req;
    uint64_t r1, r2;
    CHECK(bladerf_trigger_state(dev, tr, armed, fired, &req, &r1, &r2));
    printf("trigger state %s: armed %d, fired %d, fire requested %d\n", when,
           *armed, *fired, req);
    return 0;
}

static int do_trigger(struct bladerf *dev, struct opts *o)
{
    struct bladerf_trigger tr;
    struct rxwait w = { .dev = dev, .o = o };
    pthread_t rth;
    bool armed, fired;
    double t_fire;
    int rc = 0;

    if (o->sub != NULL && strcmp(o->sub, "rx") != 0) {
        fprintf(stderr, "ERROR: only the RX trigger is checked\n");
        return 2;
    }
    o->x2 = false;
    o->ch = 0;
    w.n = buffer_samples(o->fmt);
    w.buf = calloc(w.n, sample_bytes(o->fmt));
    if (w.buf == NULL || setup_channels(dev, o, true, false) != 0) {
        return 2;
    }
    CHECK(bladerf_set_loopback(dev, BLADERF_LB_NONE));
    CHECK(bladerf_set_rx_mux(dev, BLADERF_RX_MUX_BASEBAND));
    CHECK(bladerf_trigger_init(dev, BLADERF_CHANNEL_RX(0), BLADERF_TRIGGER_J51_1, &tr));
    tr.role = BLADERF_TRIGGER_ROLE_MASTER;
    CHECK(bladerf_trigger_arm(dev, &tr, true, 0, 0));
    if (trig_state(dev, &tr, "after arming", &armed, &fired) != 0) {
        return 2;
    }
    if (!armed || fired) {
        printf("  expected armed and not fired\n");
        rc = 1;
    }
    if (sync_config(dev, o, BLADERF_RX) != 0 ||
        enable_channels(dev, o, BLADERF_RX, true) != 0) {
        return 2;
    }
    pthread_create(&rth, NULL, rx_first_thread, &w);
    sleep(1);
    if (w.done) {
        printf("FAIL: samples arrived (status %d) before the trigger fired\n",
               w.status);
        rc = 1;
    } else {
        printf("no samples while armed (1 s): ok\n");
    }
    t_fire = now();
    CHECK(bladerf_trigger_fire(dev, &tr));
    pthread_join(rth, NULL);
    if (w.status != 0) {
        printf("FAIL: no samples after firing: %s\n", bladerf_strerror(w.status));
        rc = 1;
    } else if (w.t_done >= t_fire) {
        printf("first %u samples %.1f ms after firing: ok\n", w.n,
               (w.t_done - t_fire) * 1e3);
    }
    if (trig_state(dev, &tr, "after firing", &armed, &fired) != 0) {
        rc = 2;
    } else if (!fired) {
        printf("  expected fired\n");
        rc = 1;
    }
    tr.role = BLADERF_TRIGGER_ROLE_DISABLED;
    CHECK(bladerf_trigger_arm(dev, &tr, false, 0, 0));
    enable_channels(dev, o, BLADERF_RX, false);
    bladerf_set_rx_mux(dev, BLADERF_RX_MUX_BASEBAND);
    printf("RESULT: %s\n", rc == 0 ? "PASS" : "FAIL");
    free(w.buf);
    return rc;
}

/* --- AD9361 built-in self test ----------------------------------------- */

/* The AD9361's BIST Config register (no-OS ad9361.h REG_BIST_CONFIG) and the
 * values ad9361_bist_prbs() and ad9361_bist_tone() write to inject at the RX
 * data port (BIST_CTRL_POINT(2) | BIST_ENABLE): its PRBS, or (TONE_PRBS set)
 * a tone at fs/32 and full scale. libbladeRF has no BIST call. */
#define AD9361_REG_BIST_CONFIG 0x3f4
#define BIST_RX_PRBS           0x09
#define BIST_RX_TONE           0x0b
#define BIST_LEARN    (1u << 24)  /* samples per slot to learn the period from */
#define BIST_WINDOW   256         /* samples that must recur exactly */
#define BIST_ALIGN    1024        /* samples to find the phase with */
#define BIST_LOST     256         /* bad samples in a row: find the phase again */
#define BIST_MAGIC    "HWBIST1\n"

static volatile sig_atomic_t stop_requested;

static void on_stop_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

struct bist_pos {
    uint32_t v, pos;
};

struct bist_slot {
    uint32_t *gold;             /* one period of the pattern */
    struct bist_pos *ix;        /* the golden period sorted by value */
    uint64_t period, phase;     /* phase: gold index of the next sample */
    uint32_t *acc;              /* samples kept while learning or aligning */
    uint64_t nacc, fed;         /* fed: samples of the checking pass */
    bool aligned, failed;
    uint64_t checked, bad, resyncs, unchecked, bad_run, good_run, best_run;
    uint64_t ibit[16], qbit[16];
    unsigned reports;
};

/* Smallest p with x[s + p ...] == x[s ...] over BIST_WINDOW samples, tried
 * from a few starts s (one may hold a bad sample); 0 if none */
static uint64_t bist_find_period(const uint32_t *x, uint64_t n)
{
    uint64_t s, p;
    for (s = 0; s < 16 * 1024 && s + 2 * BIST_WINDOW <= n; s += 1024) {
        for (p = 1; s + p + BIST_WINDOW <= n; p++) {
            if (x[s + p] == x[s] &&
                memcmp(&x[s + p], &x[s], BIST_WINDOW * sizeof(*x)) == 0) {
                return p;
            }
        }
    }
    return 0;
}

/* The golden period, voted from the repeats in x[0..n): per position, the
 * value most of (up to 15) repeats agree on, so a bad sample cannot become
 * golden */
static void bist_vote(struct bist_slot *s, const uint32_t *x, uint64_t n)
{
    uint64_t p = s->period, j, reps = n / p;
    uint32_t v[15];
    if (reps > 15) {
        reps = 15;
    }
    for (j = 0; j < p; j++) {
        unsigned best = 0, cnt, a, b;
        uint32_t bestv = x[j];
        for (a = 0; a < reps; a++) {
            v[a] = x[j + a * p];
        }
        for (a = 0; a < reps; a++) {
            for (cnt = 0, b = 0; b < reps; b++) {
                cnt += v[a] == v[b];
            }
            if (cnt > best) {
                best = cnt;
                bestv = v[a];
            }
        }
        s->gold[j] = bestv;
    }
}

static int bist_pos_cmp(const void *a, const void *b)
{
    const struct bist_pos *x = a, *y = b;
    return x->v < y->v ? -1 : x->v > y->v;
}

static int bist_index(struct bist_slot *s)
{
    uint64_t j;
    s->ix = malloc(s->period * sizeof(*s->ix));
    if (s->ix == NULL) {
        return -1;
    }
    for (j = 0; j < s->period; j++) {
        s->ix[j].v = s->gold[j];
        s->ix[j].pos = (uint32_t)j;
    }
    qsort(s->ix, s->period, sizeof(*s->ix), bist_pos_cmp);
    return 0;
}

/* Phase (golden index) of x[0], x[0..n) being received samples: a few of the
 * first samples are looked up in the golden period, and a candidate phase
 * must match at least half of BIST_WINDOW samples. -1 if none does. */
static int64_t bist_align(const struct bist_slot *s, const uint32_t *x,
                          uint64_t n)
{
    uint64_t p = s->period, a, j;
    for (a = 0; a < 32 && a + BIST_WINDOW <= n; a++) {
        size_t lo = 0, hi = p, m;
        unsigned c;
        while (lo < hi) {           /* first entry with a value >= x[a] */
            size_t mid = (lo + hi) / 2;
            if (s->ix[mid].v < x[a]) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        for (m = lo, c = 0; m < p && s->ix[m].v == x[a] && c < 64; m++, c++) {
            uint64_t ph = (s->ix[m].pos + p - a % p) % p;
            unsigned good = 0;
            for (j = 0; j < BIST_WINDOW; j++) {
                good += x[j] == s->gold[(ph + j) % p];
            }
            if (2 * good >= BIST_WINDOW) {
                return (int64_t)ph;
            }
        }
    }
    return -1;
}

static void bist_check(struct bist_slot *s, uint32_t v, int k, uint64_t pos)
{
    uint32_t e = s->gold[s->phase];
    unsigned b;
    if (++s->phase == s->period) {
        s->phase = 0;
    }
    s->checked++;
    if (v == e) {
        s->bad_run = 0;
        if (++s->good_run > s->best_run) {
            s->best_run = s->good_run;
        }
        return;
    }
    s->bad++;
    s->good_run = 0;
    for (b = 0; b < 16; b++) {
        s->ibit[b] += ((v ^ e) >> b) & 1;
        s->qbit[b] += ((v ^ e) >> (16 + b)) & 1;
    }
    if (s->reports++ < MAX_REPORTS) {
        printf("  slot %d sample %" PRIu64 ": I 0x%04x Q 0x%04x, expected "
               "I 0x%04x Q 0x%04x\n", k, pos, v & 0xffff, v >> 16, e & 0xffff,
               e >> 16);
    }
    if (++s->bad_run >= BIST_LOST) {
        /* lost samples (or garbage): find the phase again */
        if (s->resyncs < MAX_REPORTS) {
            printf("  slot %d sample %" PRIu64 ": %u bad samples in a row, "
                   "finding the phase again\n", k, pos, BIST_LOST);
        }
        s->aligned = false;
        s->nacc = 0;
        s->resyncs++;
        s->bad_run = 0;
    }
}

static void bist_show(const uint32_t *x, uint64_t n, int k, const char *what)
{
    uint64_t j;
    printf("slot %d, %s:", k, what);
    for (j = 0; j < n && j < 8; j++) {
        printf(" %04x/%04x", x[j] & 0xffff, x[j] >> 16);
    }
    printf(" (I/Q)\n");
}

/* Learn the period and the golden period from s->acc, and check those
 * samples against it */
static void bist_learn(struct bist_slot *s, int k)
{
    uint64_t j;
    bist_show(s->acc, s->nacc, k, "first samples");
    s->period = bist_find_period(s->acc, s->nacc);
    if (s->period == 0 || s->period * 2 > s->nacc) {
        printf("slot %d: no repeating pattern in %" PRIu64 " samples\n", k,
               s->nacc);
        s->failed = true;
        return;
    }
    s->gold = malloc(s->period * sizeof(*s->gold));
    if (s->gold == NULL) {
        s->failed = true;
        return;
    }
    bist_vote(s, s->acc, s->nacc);
    printf("slot %d: pattern period %" PRIu64 " samples, voted over %" PRIu64
           " repeats\n", k, s->period,
           s->nacc / s->period > 15 ? 15 : s->nacc / s->period);
    if (s->period < 8) {
        printf("slot %d: (nearly) constant samples: is the BIST on?\n", k);
        s->failed = true;
        return;
    }
    s->phase = 0;
    s->aligned = true;
    for (j = 0; j < s->nacc && s->aligned; j++) {
        bist_check(s, s->acc[j], k, j);
    }
    s->aligned = false;         /* the stream restarts: phase unknown */
    s->nacc = 0;
    s->bad_run = s->good_run = 0;
}

static void bist_feed(struct bist_slot *s, uint32_t v, int k, uint64_t pos)
{
    int64_t ph;
    uint64_t j, n;
    s->fed++;
    if (s->aligned) {
        bist_check(s, v, k, pos);
        return;
    }
    if (s->failed) {
        s->unchecked++;
        return;
    }
    s->acc[s->nacc++] = v;
    if (s->nacc < BIST_ALIGN) {
        return;
    }
    n = s->nacc;
    s->nacc = 0;
    ph = bist_align(s, s->acc, n);
    if (ph < 0) {
        if (s->unchecked == 0) {
            printf("slot %d sample %" PRIu64 ": the samples match the golden "
                   "period nowhere (yet)\n", k, pos);
            bist_show(s->acc, n, k, "samples");
            bist_show(s->gold, s->period, k, "golden period starts");
        }
        s->unchecked += n;
        return;
    }
    s->phase = (uint64_t)ph;
    s->aligned = true;
    for (j = 0; j < n && s->aligned; j++) {
        bist_check(s, s->acc[j], k, pos + 1 - n + j);
    }
}

static int bist_load(const struct opts *o, struct bist_slot *sl)
{
    FILE *f = fopen(o->in, "rb");
    char magic[8];
    uint32_t fmt, slots;
    uint64_t period;
    int k;
    if (f == NULL || fread(magic, 1, 8, f) != 8 ||
        memcmp(magic, BIST_MAGIC, 8) != 0 || fread(&fmt, 4, 1, f) != 1 ||
        fread(&slots, 4, 1, f) != 1 || fread(&period, 8, 1, f) != 1 ||
        period == 0 || period > BIST_LEARN) {
        fprintf(stderr, "ERROR: %s is not a golden period file\n", o->in);
        return 2;
    }
    if (fmt != (uint32_t)o->fmt || slots != (uint32_t)nch(o)) {
        fprintf(stderr, "ERROR: %s holds format %u with %u slots\n", o->in,
                fmt, slots);
        return 2;
    }
    for (k = 0; k < nch(o); k++) {
        sl[k].period = period;
        sl[k].gold = malloc(period * sizeof(uint32_t));
        if (sl[k].gold == NULL ||
            fread(sl[k].gold, sizeof(uint32_t), period, f) != period) {
            fprintf(stderr, "ERROR: reading %s\n", o->in);
            return 2;
        }
    }
    fclose(f);
    printf("golden period of %" PRIu64 " samples from %s\n", period, o->in);
    return 0;
}

static void bist_save(const struct opts *o, const struct bist_slot *sl)
{
    FILE *f;
    uint32_t fmt = (uint32_t)o->fmt, slots = (uint32_t)nch(o);
    int k, err = 0;
    for (k = 0; k < nch(o); k++) {
        if (sl[k].gold == NULL || sl[k].failed || sl[k].bad != 0 ||
            sl[k].period != sl[0].period) {
            printf("golden period not saved: slot %d did not learn cleanly\n", k);
            return;
        }
    }
    f = fopen(o->out, "wb");
    if (f == NULL) {
        fprintf(stderr, "ERROR: writing %s\n", o->out);
        return;
    }
    err |= fwrite(BIST_MAGIC, 1, 8, f) != 8;
    err |= fwrite(&fmt, 4, 1, f) != 1;
    err |= fwrite(&slots, 4, 1, f) != 1;
    err |= fwrite(&sl[0].period, 8, 1, f) != 1;
    for (k = 0; k < nch(o); k++) {
        err |= fwrite(sl[k].gold, sizeof(uint32_t), sl[k].period, f) !=
               sl[k].period;
    }
    err |= fclose(f) != 0;
    if (err) {
        fprintf(stderr, "ERROR: writing %s\n", o->out);
    } else {
        printf("golden period saved to %s\n", o->out);
    }
}

/* One streaming pass: learn (fill each slot's acc with BIST_LEARN samples)
 * or check for o->seconds. Returns 0, or 2 on errors. */
static int bist_pass(struct bladerf *dev, const struct opts *o, void *buf,
                     struct bist_slot *sl, bool learn, uint64_t *pos,
                     double *t)
{
    unsigned n = buffer_samples(o->fmt), per = n / (unsigned)nch(o), j;
    struct bladerf_metadata md;
    uint64_t next_ts = 0, gaps = 0, ovr = 0;
    double t0;
    int rc = 0, k;
    if (sync_config(dev, o, BLADERF_RX) != 0 ||
        enable_channels(dev, o, BLADERF_RX, true) != 0) {
        return 2;
    }
    t0 = now();
    while (now() - t0 < o->warmup && !stop_requested && rc == 0) {
        rc = rx_buffer(dev, o, buf, n, &md, &next_ts, &gaps, &ovr, TIMEOUT_MS,
                       false);
    }
    t0 = now();
    while (!stop_requested && rc == 0) {
        rc = rx_buffer(dev, o, buf, n, &md, &next_ts, &gaps, &ovr, TIMEOUT_MS,
                       false);
        if (rc != 0) {
            break;
        }
        for (j = 0; j < n; j++) {
            uint32_t i, q;
            struct bist_slot *s;
            k = (int)(j % (unsigned)nch(o));
            s = &sl[k];
            get_iq(o, buf, j, &i, &q);
            if (!learn) {
                bist_feed(s, i | q << 16, k, *pos + j / (unsigned)nch(o));
            } else if (s->nacc < BIST_LEARN) {
                s->acc[s->nacc++] = i | q << 16;
            }
        }
        *pos += per;
        *t = now() - t0;
        if (learn ? sl[nch(o) - 1].nacc >= BIST_LEARN : *t >= o->seconds) {
            break;
        }
    }
    enable_channels(dev, o, BLADERF_RX, false);
    return rc;
}

static int bist_report(const struct opts *o, struct bist_slot *sl, double t)
{
    int k, rc = 0;
    unsigned b;
    for (k = 0; k < nch(o); k++) {
        struct bist_slot *s = &sl[k];
        double rate = t > 0 ? s->fed / t : 0;
        printf("slot %d (ch%d): %" PRIu64 " samples checked, %" PRIu64
               " bad (%.3g), %" PRIu64 " phase losses, %" PRIu64
               " not checked, longest clean run %" PRIu64 "; checking pass "
               "%.2f s at %.3f Msps\n", k, slot_ch(o, k) + 1, s->checked,
               s->bad, s->checked ? (double)s->bad / s->checked : 0.0,
               s->resyncs, s->unchecked + s->nacc, s->best_run, t, rate / 1e6);
        if (s->bad != 0) {
            printf("  bit errors I (bit:count):");
            for (b = 0; b < 16; b++) {
                if (s->ibit[b] != 0) {
                    printf(" %u:%" PRIu64, b, s->ibit[b]);
                }
            }
            printf("\n  bit errors Q (bit:count):");
            for (b = 0; b < 16; b++) {
                if (s->qbit[b] != 0) {
                    printf(" %u:%" PRIu64, b, s->qbit[b]);
                }
            }
            printf("\n");
        }
        if (s->failed || s->checked == 0 || s->bad != 0 || s->resyncs != 0 ||
            s->unchecked != 0) {
            rc = 1;
        }
        if (rate < 0.98 * o->rate) {
            printf("  rate below 98 %%\n");
            rc = 1;
        }
    }
    return rc;
}

static int do_bist(struct bladerf *dev, struct opts *o)
{
    void *buf = calloc(buffer_samples(o->fmt), sample_bytes(o->fmt));
    uint8_t val = o->bist_tone ? BIST_RX_TONE : BIST_RX_PRBS, rb = 0;
    struct bist_slot sl[2];
    uint64_t pos = 0;
    double t = 0;
    int rc = 0, k;

    if (buf == NULL) {
        return 2;
    }
    if (is_meta(o->fmt) || o->fmt == BLADERF_FORMAT_SC16_Q11_PACKED) {
        fprintf(stderr, "ERROR: bist supports sc16 and sc8 only\n");
        return 2;
    }
    memset(sl, 0, sizeof(sl));
    if (o->in != NULL && bist_load(o, sl) != 0) {
        return 2;
    }
    for (k = 0; k < nch(o); k++) {
        sl[k].acc = malloc((o->in != NULL ? BIST_ALIGN : BIST_LEARN) *
                           sizeof(uint32_t));
        if (sl[k].acc == NULL) {
            return 2;
        }
    }
    if (setup_channels(dev, o, true, false) != 0) {
        return 2;
    }
    CHECK(bladerf_set_loopback(dev, BLADERF_LB_NONE));
    CHECK(bladerf_set_rx_mux(dev, BLADERF_RX_MUX_BASEBAND));
    signal(SIGINT, on_stop_signal);
    signal(SIGTERM, on_stop_signal);
    CHECK(bladerf_set_rfic_register(dev, AD9361_REG_BIST_CONFIG, val));
    if (bladerf_get_rfic_register(dev, AD9361_REG_BIST_CONFIG, &rb) != 0 ||
        rb != val) {
        printf("AD9361 BIST config reads back 0x%02x, wrote 0x%02x\n", rb, val);
        rc = 1;
    }
    printf("bist: AD9361 %s at the RX data port (register 0x%03x = 0x%02x), "
           "%s, %s, %.3f Msps, %.0f s after %.2f s of warm-up\n",
           o->bist_tone ? "tone" : "PRBS", AD9361_REG_BIST_CONFIG, val,
           o->fmt_name, o->x2 ? "x2" : "x1", o->rate / 1e6, o->seconds,
           o->warmup);
    if (o->in == NULL) {
        if (bist_pass(dev, o, buf, sl, true, &pos, &t) != 0) {
            rc = 2;
            goto out;
        }
        for (k = 0; k < nch(o); k++) {
            bist_learn(&sl[k], k);
        }
        if (o->out != NULL) {
            bist_save(o, sl);
        }
    }
    for (k = 0; k < nch(o); k++) {
        if (!sl[k].failed && bist_index(&sl[k]) != 0) {
            rc = 2;
            goto out;
        }
    }
    if (!sl[0].failed && !sl[nch(o) - 1].failed && !stop_requested) {
        t = 0;
        if (bist_pass(dev, o, buf, sl, false, &pos, &t) != 0) {
            rc = 2;
        }
    }
out:
    /* the BIST must not outlive this run (in FPGA tuning mode the AD9361
     * keeps its state across bladerf_open) */
    if (bladerf_set_rfic_register(dev, AD9361_REG_BIST_CONFIG, 0) != 0 ||
        bladerf_get_rfic_register(dev, AD9361_REG_BIST_CONFIG, &rb) != 0 ||
        rb != 0) {
        fprintf(stderr, "ERROR: could not switch the AD9361 BIST off\n");
        rc = 2;
    } else {
        printf("AD9361 BIST off\n");
    }
    if (stop_requested) {
        printf("stopped by a signal\n");
        rc = rc ? rc : 1;
    }
    k = bist_report(o, sl, t);
    rc = rc ? rc : k;
    for (k = 0; k < 2; k++) {
        free(sl[k].gold);
        free(sl[k].ix);
        free(sl[k].acc);
    }
    free(buf);
    printf("RESULT: %s\n", rc == 0 ? "PASS" : "FAIL");
    return rc;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: hwtest-stream probe|rxcheck|loopback|rxcap|tx|bist|rxsched|trigger [rx|tx]\n"
            "  -f FMT (sc16 sc16meta sc8 sc8meta packed)  -l x1|x2  -c 1|2\n"
            "  -r RATE  -t SECONDS  -n SAMPLES  -m MUX  -F FREQ  -g GAIN  -O\n"
            "  -o FILE  -i FILE  -T TONE_HZ  -W WARMUP_S  -B prbs|tone  -d DEVICE\n"
            "  -b BURST_MS  -L LEAD_MS | -N  -G GAP_MS  -S NBUF,SAMPLES,NXFER (rxsched)\n"
            "  -A (loopback in the AD9361)  -R ADDR=VAL (set an AD9361 register after\n"
            "  setup, up to 8)  -D (print AD9361 registers after setup)  -v (verbose)\n");
}

int main(int argc, char **argv)
{
    struct opts o = { .fmt = BLADERF_FORMAT_SC16_Q11, .fmt_name = "sc16",
                      .rate = 30.72e6, .seconds = 10, .warmup = 0.25,
                      .burst_ms = 100, .lead_ms = 5,
                      .mux = BLADERF_RX_MUX_32BIT_COUNTER, .mux_name = "counter32" };
    struct bladerf *dev;
    int c, rc;

    setvbuf(stdout, NULL, _IOLBF, 0);   /* logs survive a time-out kill */
    if (argc < 2) {
        usage();
        return 2;
    }
    o.mode = argv[1];
    optind = 2;
    if (strcmp(o.mode, "trigger") == 0 && argc > 2 && argv[2][0] != '-') {
        o.sub = argv[2];
        optind = 3;
    }
    while ((c = getopt(argc, argv, "f:l:c:r:t:n:m:F:g:Oo:i:T:d:vW:B:b:L:G:NS:AR:D")) != -1) {
        switch (c) {
            case 'f':
                if (parse_format(optarg, &o.fmt) != 0) {
                    usage();
                    return 2;
                }
                o.fmt_name = optarg;
                break;
            case 'l': o.x2 = strcmp(optarg, "x2") == 0; break;
            case 'c': o.ch = atoi(optarg) - 1; break;
            case 'r': o.rate = parse_num(optarg); break;
            case 't': o.seconds = parse_num(optarg); break;
            case 'n': o.nsamples = (uint64_t)parse_num(optarg); break;
            case 'm':
                if (parse_mux(optarg, &o.mux) != 0) {
                    usage();
                    return 2;
                }
                o.mux_name = optarg;
                break;
            case 'F': o.freq = parse_num(optarg); o.have_freq = true; break;
            case 'g': o.gain = atof(optarg); o.have_gain = true; break;
            case 'O': o.oversample = true; break;
            case 'o': o.out = optarg; break;
            case 'i': o.in = optarg; break;
            case 'T': o.tone = parse_num(optarg); o.have_tone = true; break;
            case 'd': o.dev = optarg; break;
            case 'v': bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_DEBUG); break;
            case 'W': o.warmup = atof(optarg); break;
            case 'b': o.burst_ms = atof(optarg); break;
            case 'L': o.lead_ms = atof(optarg); break;
            case 'G': o.gap_ms = atof(optarg); break;
            case 'N': o.rx_now = true; break;
            case 'A': o.lb_rfic = true; break;
            case 'D': o.dump_rfic = true; break;
            case 'R': {
                char *eq = strchr(optarg, '='), *end1 = NULL, *end2 = NULL;
                unsigned long a, v;
                if (eq == NULL || o.npoke >= 8) {
                    usage();
                    return 2;
                }
                a = strtoul(optarg, &end1, 0);
                v = strtoul(eq + 1, &end2, 0);
                if (end1 != eq || *end2 != '\0' || a > 0x3ff || v > 0xff) {
                    usage();
                    return 2;
                }
                o.poke_addr[o.npoke] = (uint16_t)a;
                o.poke_val[o.npoke] = (uint8_t)v;
                o.npoke++;
                break;
            }
            case 'S':
                if (sscanf(optarg, "%u,%u,%u", &o.nbuf, &o.bufsamp, &o.nxfer) != 3 ||
                    o.nbuf == 0 || o.bufsamp == 0 || o.bufsamp % 1024 != 0 ||
                    o.nxfer == 0) {
                    fprintf(stderr, "ERROR: -S wants NBUF,SAMPLES,NXFER (SAMPLES a "
                                    "multiple of 1024)\n");
                    return 2;
                }
                break;
            case 'B':
                if (strcmp(optarg, "tone") != 0 && strcmp(optarg, "prbs") != 0) {
                    usage();
                    return 2;
                }
                o.bist_tone = strcmp(optarg, "tone") == 0;
                break;
            default: usage(); return 2;
        }
    }
    if (o.ch < 0 || o.ch > 1) {
        usage();
        return 2;
    }
    if (bladerf_open(&dev, o.dev) != 0) {
        fprintf(stderr, "ERROR: cannot open the bladeRF\n");
        return 2;
    }
    if (strcmp(o.mode, "probe") == 0) {
        rc = do_probe(dev, &o);
    } else if (strcmp(o.mode, "rxcheck") == 0) {
        rc = do_rxcheck(dev, &o);
    } else if (strcmp(o.mode, "loopback") == 0) {
        rc = do_loopback(dev, &o);
    } else if (strcmp(o.mode, "rxcap") == 0) {
        rc = do_rxcap(dev, &o);
    } else if (strcmp(o.mode, "tx") == 0) {
        rc = do_tx(dev, &o);
    } else if (strcmp(o.mode, "trigger") == 0) {
        rc = do_trigger(dev, &o);
    } else if (strcmp(o.mode, "bist") == 0) {
        rc = do_bist(dev, &o);
    } else if (strcmp(o.mode, "rxsched") == 0) {
        rc = do_rxsched(dev, &o);
    } else {
        usage();
        rc = 2;
    }
    bladerf_close(dev);
    return rc;
}
