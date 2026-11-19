/* wiphy-loop.c (2026-11-19): a functional test of the bladeRF 2.0 micro xA9's wlan FPGA revision (Nuand's
 * bladeRF-wiphy 802.11 OFDM modem in the FPGA) with no antenna. Frames are sent the way Nuand's Linux driver
 * (bladeRF-linux-mac80211) sends them: BLADERF_FORMAT_PACKET_META, a 16-byte bladeRF-wiphy TX header
 * (modulation, bandwidth, length, cookie), then the MPDU without FCS (the FPGA appends the FCS). The AD9361
 * loops its TX data port back to its RX data port (BLADERF_LB_RFIC_BIST); everything the FPGA sends back
 * (TX reports, demodulated frames) is read in the same format and checked against what was sent.
 * Formats, source references and caveats: wiphy-loop.md next to this file.
 *   wiphy-loop -c COUNT [-m status|loop] [-l BYTES] [-r MBPS|all] [-g GAP_MS] [-w WAIT_MS] [-n] [-v] [-V]
 *              [-d DEVICE]
 *   -m status (default): unicast data frames (ACK required; nothing in the loopback ACKs them). The FPGA sends
 *      each frame, retries once one rate lower, then reports "no ACK" (type 3) with the frame's cookie: every
 *      frame must get exactly one such report. Covers host->FPGA packets, the wiphy TX state machine, DCF,
 *      modulator (it must finish), and the FPGA->host packet path; not the demodulator.
 *   -m loop: broadcast data frames (no ACK, no report): every frame must come back demodulated, byte-exact,
 *      valid FCS. The stock wlan_top blanks its own receiver while it transmits (rx_block), so this mode is
 *      expected to FAIL on the stock design; it is meant for an FPGA build without that blanking.
 *   -n: send nothing (RX and TX enabled, no frames) for the same duration: nothing may arrive.
 * The wlan image must already be loaded (checked: config GPIO bit 28, "packet core present"); host tuning
 * mode (BLADERF_DEFAULT_TUNING_MODE=host), which the AD9361 loopback needs. TX gain 0 dB.
 * The device is opened only when -c is given. Exit 0: PASS, 1: FAIL (missing, extra or wrong packets),
 * 2: errors.
 */
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

#define RATE              20000000u  /* 802.11 OFDM, 20 MHz channel: the wiphy modem runs at 20 Msps */
#define FREQ              915000000u /* no effect in the digital loopback; outside the Wi-Fi bands */
#define BW                18000000u
#define BUF_DWORDS        4096u      /* per libbladeRF buffer: 16 KiB, a multiple of the 8 KiB USB message */
#define RX_BUFFERS        64u
#define RX_XFERS          16u
#define TX_BUFFERS        16u
#define TX_XFERS          8u
#define STREAM_TIMEOUT_MS 1000u      /* a packet-mode RX transfer completes only when a packet arrives. With
                                      * this time-out an idle RX stream ends with BLADERF_ERR_TIMEOUT and the
                                      * next sync_rx starts it again, and closing never waits on a transfer
                                      * that cannot complete (version 1 used Nuand's driver's 10,000,000 ms:
                                      * every run hung when it closed the device) */
#define TX_TIMEOUT_MS     3500u
#define RX_SYNC_TIMEOUT_MS 2500u     /* longer than STREAM_TIMEOUT_MS, so the stream's time-out always comes
                                      * first: libbladeRF master reports sync_rx's own time-out as
                                      * BLADERF_ERR_UNEXPECTED (thread.h posix_cond_timedwait() returns -1, its
                                      * callers expect ETIMEDOUT) */
#define RXBUF_DWORDS      65536u     /* sync_rx copies as many dwords as the packet header says (16 bits) */

#define HDR_BYTES         16u        /* bladeRF-wiphy header, TX and RX */
#define MIN_LEN           36u        /* 24-byte data frame header + magic, run id, frame index */
#define MAX_LEN           1500u
#define MAX_COUNT         1000000u   /* frame index in the low 20 bits of the cookie */
#define BW_CODE_20MHZ     2u
#define TYPE_FRAME        1u         /* demodulated frame: MPDU + FCS */
#define TYPE_ACKED        2u         /* TX report: ACK received */
#define TYPE_NO_ACK       3u         /* TX report: no ACK after the retry */

static const unsigned MBPS[8] = { 6, 9, 12, 18, 24, 36, 48, 54 };   /* modulation codes 0..7 */

enum verdict { V_NONE, V_EXACT, V_CORRUPT, V_FOREIGN, V_RUNT };

struct event {
    double   t;
    unsigned type, bw, mod;
    uint32_t w1, w2, w3;            /* w1: cookie (report) or length in bytes (frame) */
    unsigned dwords;
    long     frame;                 /* our frame index, -1 if none */
    enum verdict verdict;
    bool     fcs_ok;
};

static volatile bool stop;

struct txctx {
    struct bladerf *dev;
    unsigned count, len;
    int rate_sel;                   /* modulation code, or -1: cycle through all eight */
    bool bcast;
    uint32_t run;
    double t0, gap;
    double *t_send;
    uint8_t *pkt;
    pthread_mutex_t lock;
    unsigned sent;                  /* under lock */
    double t_last;                  /* under lock */
    bool done;                      /* under lock */
    int status;
};

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void sleep_until(double t)
{
    for (;;) {
        double d = t - now();
        struct timespec ts;
        if (d <= 0 || stop) {
            return;
        }
        ts.tv_sec = (time_t)d;
        ts.tv_nsec = (long)((d - (double)ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
    }
}

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffffu); put16(p + 2, v >> 16); }
static unsigned get16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }

/* 802.11 FCS: CRC-32 (IEEE 802.3), sent least significant byte first */
static uint32_t crc32_ieee(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    int b;
    while (n--) {
        c ^= *p++;
        for (b = 0; b < 8; b++) {
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

static unsigned rate_code(int rate_sel, unsigned k)
{
    return rate_sel >= 0 ? (unsigned)rate_sel : k % 8;
}

/* the report carries the rate of the last attempt: the retry, one step lower (wlan_top.vhd:832-843) */
static unsigned retry_code(unsigned code)
{
    return code == 0 ? 0 : code - 1;
}

static uint32_t cookie_of(uint32_t run, unsigned k)
{
    return (run & 0xfffu) << 20 | k;
}

/* Data frame, ToDS = FromDS = 0. The unicast RA (status mode) is neither broadcast nor the wiphy's own address
 * 70:b3:d5:7d:80:01 (wlan_rx.vhd:758), so the TX expects an ACK and the RX would not generate one. */
static void build_mpdu(uint8_t *m, unsigned len, uint32_t run, uint32_t k, bool bcast)
{
    static const uint8_t ra_unicast[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
    static const uint8_t ra_bcast[6]   = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    static const uint8_t ta[6]         = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };
    static const uint8_t bssid[6]      = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x03 };
    uint32_t x = run ^ (k * 2654435761u);
    unsigned i;
    m[0] = 0x08;                                    /* frame control: type data, subtype data */
    m[1] = 0x00;                                    /* flags */
    m[2] = m[3] = 0x00;                             /* duration */
    memcpy(m + 4, bcast ? ra_bcast : ra_unicast, 6);
    memcpy(m + 10, ta, 6);
    memcpy(m + 16, bssid, 6);
    put16(m + 22, (k & 0xfffu) << 4);               /* sequence control: fragment 0, sequence k mod 4096 */
    memcpy(m + 24, "wplb", 4);
    put32(m + 28, run);
    put32(m + 32, k);
    for (i = MIN_LEN; i < len; i++) {
        x = x * 1103515245u + 12345u;
        m[i] = (uint8_t)(x >> 16);
    }
}

/* bladeRF-wiphy TX header (struct bladeRF_wiphy_header_tx; parsed in wlan_top.vhd:714-739) + MPDU, padded
 * to whole dwords; length in dwords = bytes_to_dwords(16 + len), as bladeRF-linux-mac80211 */
static unsigned build_tx_packet(uint8_t *p, unsigned len, unsigned code, uint32_t cookie, uint32_t run,
                                uint32_t k, bool bcast)
{
    unsigned n = (HDR_BYTES + len + 3) / 4;
    memset(p, 0, (size_t)n * 4);
    put16(p + 0, 0);                    /* rsvd */
    put16(p + 2, 0);                    /* flags (stored, unused by wlan_top) */
    put16(p + 4, code);                 /* modulation: 0..7 = 6, 9, 12, 18, 24, 36, 48, 54 Mbps */
    put16(p + 6, BW_CODE_20MHZ);        /* bandwidth: 0 = 5, 1 = 10, 2 = 20 MHz */
    put16(p + 8, len);                  /* MPDU length in bytes, without FCS */
    put16(p + 10, 0);                   /* rsvd2 */
    put32(p + 12, cookie);              /* returned in the TX report */
    build_mpdu(p + HDR_BYTES, len, run, k, bcast);
    return n;
}

/* USB transfers that are whole multiples of the max packet size (512 bytes at HS, 1024 at SS) get no short
 * packet to end them (libbladeRF.h:2236-2241) */
static bool usb_len_ok(unsigned payload_bytes)
{
    unsigned bytes = 16 + 4 * ((payload_bytes + 3) / 4);
    return bytes % 512 != 0;
}

static void *tx_thread(void *arg)
{
    struct txctx *c = arg;
    struct bladerf_metadata meta;
    unsigned k;
    double prev = 0;
    memset(&meta, 0, sizeof(meta));
    for (k = 0; k < c->count && !stop; k++) {
        unsigned code = rate_code(c->rate_sel, k), n;
        double due = c->t0 + k * c->gap;
        int s;
        if (k > 0 && prev + c->gap > due) {
            due = prev + c->gap;            /* late: keep the spacing rather than catch up in a burst */
        }
        sleep_until(due);
        if (stop) {
            break;
        }
        n = build_tx_packet(c->pkt, c->len, code, cookie_of(c->run, k), c->run, k, c->bcast);
        c->t_send[k] = prev = now();
        s = bladerf_sync_tx(c->dev, c->pkt, n, &meta, TX_TIMEOUT_MS);
        if (s != 0) {
            fprintf(stderr, "ERROR: bladerf_sync_tx (frame %u): %s\n", k, bladerf_strerror(s));
            c->status = s;
            break;
        }
        pthread_mutex_lock(&c->lock);
        c->sent = k + 1;
        c->t_last = c->t_send[k];
        pthread_mutex_unlock(&c->lock);
    }
    pthread_mutex_lock(&c->lock);
    if (c->sent == 0) {
        c->t_last = now();
    }
    c->done = true;
    pthread_mutex_unlock(&c->lock);
    return NULL;
}

/* a demodulated frame: our MPDU (magic, run id, index), byte-exact, FCS valid, reported rate and bandwidth */
static enum verdict check_frame(const uint8_t *pl, unsigned len, const struct txctx *c, uint8_t *scratch,
                                unsigned mod, unsigned bw, long *frame, bool *fcs_ok)
{
    unsigned ml;
    uint32_t k;
    *frame = -1;
    *fcs_ok = false;
    if (len < 4 + 24) {
        return V_RUNT;
    }
    ml = len - 4;
    *fcs_ok = crc32_ieee(pl, ml) == get32(pl + ml);
    if (ml < MIN_LEN || memcmp(pl + 24, "wplb", 4) != 0 || get32(pl + 28) != c->run) {
        return V_FOREIGN;
    }
    k = get32(pl + 32);
    if (k >= c->count) {
        return V_CORRUPT;
    }
    *frame = (long)k;
    if (ml != c->len || !*fcs_ok || bw != BW_CODE_20MHZ || mod != rate_code(c->rate_sel, k)) {
        return V_CORRUPT;
    }
    build_mpdu(scratch, c->len, c->run, k, c->bcast);
    return memcmp(pl, scratch, ml) == 0 ? V_EXACT : V_CORRUPT;
}

static const char *type_name(unsigned type)
{
    switch (type) {
        case TYPE_FRAME:  return "frame";
        case TYPE_ACKED:  return "TX report ACKed";
        case TYPE_NO_ACK: return "TX report no ACK";
        default:          return "unknown type";
    }
}

static const char *verdict_name(enum verdict v)
{
    switch (v) {
        case V_EXACT:   return "exact";
        case V_CORRUPT: return "ours but wrong";
        case V_FOREIGN: return "not ours";
        case V_RUNT:    return "too short";
        default:        return "-";
    }
}

static void usage(FILE *f)
{
    fprintf(f, "usage: wiphy-loop -c COUNT [-m status|loop] [-l BYTES] [-r MBPS|all] [-g GAP_MS] [-w WAIT_MS]\n"
               "                  [-n] [-v] [-V] [-d DEVICE]\n"
               "  -c COUNT   frames to send (required: without it the device is not opened)\n"
               "  -m status  unicast frames; one \"no ACK\" TX report per frame expected (default)\n"
               "  -m loop    broadcast frames; every frame expected back demodulated (fails on the stock\n"
               "             design, whose receiver is blanked while it transmits)\n"
               "  -l BYTES   MPDU length without FCS, %u..%u (default 100)\n"
               "  -r MBPS    6 9 12 18 24 36 48 54, or all (cycle through them; default 6)\n"
               "  -g GAP_MS  time between frames (default 10)\n"
               "  -w WAIT_MS listening time after the last frame (default 500)\n"
               "  -n         send nothing for the same duration: nothing may arrive\n"
               "  -v         print every packet received\n"
               "  -V         keep libbladeRF's error logging while streaming (RX polls that time out log errors)\n"
               "  -d DEVICE  device string for bladerf_open\n"
               "Needs the wlan FPGA image loaded and BLADERF_DEFAULT_TUNING_MODE=host. Exit 0 PASS, 1 FAIL, "
               "2 errors.\n",
            MIN_LEN, MAX_LEN);
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
    unsigned count = 0, len = 100, i;
    int rate_sel = 0;
    double gap_ms = 10, wait_ms = 500, t0 = 0, planned_end;
    bool notx = false, loop = false, verbose = false, keep_log = false;
    bool tx_on = false, rx_on = false, lb_on = false, th_started = false, log_low = false;
    struct txctx tc;
    pthread_t th;
    uint8_t *rxbuf = NULL, *scratch = NULL;
    struct event *ev = NULL;
    size_t nev = 0, cap = 0;
    char ratestr[32];
    int rc = 0, c;

    memset(&tc, 0, sizeof(tc));
    while ((c = getopt(argc, argv, "c:m:l:r:g:w:nvVd:h")) != -1) {
        switch (c) {
            case 'c': count = (unsigned)strtoul(optarg, NULL, 10); break;
            case 'm':
                if (strcmp(optarg, "status") == 0) {
                    loop = false;
                } else if (strcmp(optarg, "loop") == 0) {
                    loop = true;
                } else {
                    usage(stderr);
                    return 2;
                }
                break;
            case 'l': len = (unsigned)strtoul(optarg, NULL, 10); break;
            case 'r':
                if (strcmp(optarg, "all") == 0) {
                    rate_sel = -1;
                } else {
                    unsigned m = (unsigned)strtoul(optarg, NULL, 10);
                    rate_sel = -2;
                    for (i = 0; i < 8; i++) {
                        if (MBPS[i] == m) {
                            rate_sel = (int)i;
                        }
                    }
                    if (rate_sel == -2) {
                        fprintf(stderr, "ERROR: rate must be 6, 9, 12, 18, 24, 36, 48, 54 or all\n");
                        return 2;
                    }
                }
                break;
            case 'g': gap_ms = atof(optarg); break;
            case 'w': wait_ms = atof(optarg); break;
            case 'n': notx = true; break;
            case 'v': verbose = true; break;
            case 'V': keep_log = true; break;
            case 'd': devstr = optarg; break;
            case 'h': usage(stdout); return 0;
            default: usage(stderr); return 2;
        }
    }
    if (count == 0 || optind != argc) {
        usage(stderr);
        return 2;
    }
    if (count > MAX_COUNT || len < MIN_LEN || len > MAX_LEN || gap_ms < 0 || wait_ms < 50) {
        fprintf(stderr, "ERROR: 1 <= COUNT <= %u, %u <= BYTES <= %u, GAP_MS >= 0, WAIT_MS >= 50\n",
                MAX_COUNT, MIN_LEN, MAX_LEN);
        return 2;
    }
    if (!usb_len_ok(HDR_BYTES + len) || !usb_len_ok(HDR_BYTES + len + 4)) {
        fprintf(stderr, "ERROR: %u bytes makes a TX or RX USB transfer a multiple of 512 bytes "
                        "(see libbladeRF.h, BLADERF_FORMAT_PACKET_META); pick another length\n", len);
        return 2;
    }
    if (rate_sel < 0) {
        snprintf(ratestr, sizeof(ratestr), "6..54 Mbps (cycling)");
    } else {
        snprintf(ratestr, sizeof(ratestr), "%u Mbps", MBPS[rate_sel]);
    }

    tc.count = count;
    tc.len = len;
    tc.rate_sel = rate_sel;
    tc.bcast = loop;
    tc.run = (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 12);
    tc.gap = gap_ms / 1000.0;
    tc.t_send = calloc(count, sizeof(double));
    tc.pkt = calloc(1, HDR_BYTES + MAX_LEN + 4);
    rxbuf = malloc((size_t)RXBUF_DWORDS * 4);
    scratch = malloc(MAX_LEN + 4);
    if (tc.t_send == NULL || tc.pkt == NULL || rxbuf == NULL || scratch == NULL) {
        return 2;
    }
    pthread_mutex_init(&tc.lock, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);

    CHECK(bladerf_open(&dev, devstr));
    tc.dev = dev;
    {
        uint32_t gpio = 0;
        bladerf_tuning_mode tm;
        CHECK(bladerf_config_gpio_read(dev, &gpio));
        if ((gpio & BLADERF_GPIO_PACKET_CORE_PRESENT) == 0) {
            fprintf(stderr, "ERROR: the loaded FPGA has no packet core (config GPIO 0x%08" PRIx32 ", bit 28 "
                            "clear): load the wlan revision first\n", gpio);
            rc = 2;
            goto out;
        }
        CHECK(bladerf_get_tuning_mode(dev, &tm));
        if (tm != BLADERF_TUNING_MODE_HOST) {
            fprintf(stderr, "ERROR: the AD9361 loopback needs host tuning mode "
                            "(BLADERF_DEFAULT_TUNING_MODE=host)\n");
            rc = 2;
            goto out;
        }
    }
    {
        bladerf_sample_rate actual_rx = 0, actual_tx = 0;
        CHECK(bladerf_set_frequency(dev, BLADERF_CHANNEL_RX(0), FREQ));
        CHECK(bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), RATE, &actual_rx));
        CHECK(bladerf_set_bandwidth(dev, BLADERF_CHANNEL_RX(0), BW, NULL));
        CHECK(bladerf_set_frequency(dev, BLADERF_CHANNEL_TX(0), FREQ));
        CHECK(bladerf_set_sample_rate(dev, BLADERF_CHANNEL_TX(0), RATE, &actual_tx));
        CHECK(bladerf_set_bandwidth(dev, BLADERF_CHANNEL_TX(0), BW, NULL));
        CHECK(bladerf_set_gain(dev, BLADERF_CHANNEL_TX(0), 0));
        if (actual_rx != RATE || actual_tx != RATE) {
            fprintf(stderr, "ERROR: sample rate %u/%u Hz (RX/TX); the modem needs exactly %u\n",
                    actual_rx, actual_tx, RATE);
            rc = 2;
            goto out;
        }
    }
    CHECK(bladerf_set_loopback(dev, BLADERF_LB_RFIC_BIST));
    lb_on = true;
    /* PACKET_META sets the FPGA's packet_en (and timestamps) for both directions (bladerf2/common.c:115-116) */
    CHECK(bladerf_sync_config(dev, BLADERF_RX_X1, BLADERF_FORMAT_PACKET_META, RX_BUFFERS, BUF_DWORDS,
                              RX_XFERS, STREAM_TIMEOUT_MS));
    CHECK(bladerf_sync_config(dev, BLADERF_TX_X1, BLADERF_FORMAT_PACKET_META, TX_BUFFERS, BUF_DWORDS,
                              TX_XFERS, STREAM_TIMEOUT_MS));
    /* RX first: TX reports produced while RX is disabled are lost */
    CHECK(bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true));
    rx_on = true;
    CHECK(bladerf_enable_module(dev, BLADERF_CHANNEL_TX(0), true));
    tx_on = true;

    printf("wiphy-loop: %s, %u frame%s of %u bytes (+4 FCS) at %s every %.1f ms, run id %08" PRIx32
           ", looped back in the AD9361\n", notx ? "SEND NOTHING (control)" : loop ? "loop mode" : "status mode",
           count, count == 1 ? "" : "s", len, ratestr, gap_ms, tc.run);
    printf("  %s\n", notx ? "no frame is sent: nothing may arrive"
                     : loop ? "broadcast frames: each must come back demodulated, byte-exact, valid FCS"
                            : "unicast frames to 02:00:00:00:00:01: each must get one \"no ACK\" TX report "
                              "carrying its cookie and the retry rate (one step lower)");

    if (!keep_log) {
        /* bladerf_sync_rx logs every poll that times out as an error (sync.c:394); polls time out by design */
        bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_CRITICAL);
        log_low = true;
    }
    t0 = now() + 0.05;
    tc.t0 = t0;
    planned_end = t0 + (count - 1) * tc.gap + wait_ms / 1000.0;
    if (!notx) {
        if (pthread_create(&th, NULL, tx_thread, &tc) != 0) {
            fprintf(stderr, "ERROR: pthread_create failed\n");
            rc = 2;
            goto out;
        }
        th_started = true;
    }

    for (;;) {
        struct bladerf_metadata meta;
        double t = now(), deadline = planned_end, tl;
        bool txd;
        int s;
        struct event e;

        if (notx) {
            txd = true;
            tl = t0 + (count - 1) * tc.gap;
        } else {
            pthread_mutex_lock(&tc.lock);
            txd = tc.done;
            tl = tc.t_last;
            pthread_mutex_unlock(&tc.lock);
        }
        if (txd && tl + wait_ms / 1000.0 > deadline) {
            deadline = tl + wait_ms / 1000.0;
        }
        if (txd && t >= deadline) {
            break;
        }
        memset(&meta, 0, sizeof(meta));
        /* returns a packet, or BLADERF_ERR_TIMEOUT from the stream within about STREAM_TIMEOUT_MS; the deadline
         * is checked again after each return (it can pass by up to STREAM_TIMEOUT_MS) */
        s = bladerf_sync_rx(dev, rxbuf, BUF_DWORDS, &meta, RX_SYNC_TIMEOUT_MS);
        if (s == BLADERF_ERR_TIMEOUT) {
            /* the stream timed out (nothing arrived): give its worker a moment to become idle, so the next
             * call starts it again instead of waiting on the stopped stream */
            usleep(5000);
            continue;
        }
        if (s != 0) {
            fprintf(stderr, "ERROR: bladerf_sync_rx: %s (after %.0f ms)\n", bladerf_strerror(s),
                    (now() - t) * 1e3);
            rc = 2;
            break;
        }
        memset(&e, 0, sizeof(e));
        e.t = now();
        e.dwords = meta.actual_count;
        e.frame = -1;
        if (e.dwords >= HDR_BYTES / 4) {
            e.type = get16(rxbuf);
            e.bw = rxbuf[2];
            e.mod = rxbuf[3];
            e.w1 = get32(rxbuf + 4);
            e.w2 = get32(rxbuf + 8);
            e.w3 = get32(rxbuf + 12);
            if (e.type == TYPE_FRAME) {
                unsigned flen = e.w1 & 0xffffu;
                if (HDR_BYTES + flen > e.dwords * 4u) {
                    e.verdict = V_RUNT;
                } else {
                    e.verdict = check_frame(rxbuf + HDR_BYTES, flen, &tc, scratch, e.mod, e.bw, &e.frame,
                                            &e.fcs_ok);
                }
            }
        }
        if (verbose) {
            printf("  rx %+.3f ms: %u dwords, %s (type %u) mod %u bw %u, words1-3 %08" PRIx32 " %08" PRIx32
                   " %08" PRIx32 "%s%s\n", (e.t - t0) * 1e3, e.dwords, type_name(e.type), e.type, e.mod, e.bw,
                   e.w1, e.w2, e.w3, e.type == TYPE_FRAME ? ", " : "",
                   e.type == TYPE_FRAME ? verdict_name(e.verdict) : "");
        }
        if (nev == cap) {
            struct event *n2;
            cap = cap ? 2 * cap : 256;
            n2 = realloc(ev, cap * sizeof(*ev));
            if (n2 == NULL) {
                rc = 2;
                break;
            }
            ev = n2;
        }
        ev[nev++] = e;
    }
    stop = true;
out:
    if (th_started) {
        stop = true;
        pthread_join(th, NULL);
        if (tc.status != 0 && rc == 0) {
            rc = 2;
        }
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
    if (log_low) {
        /* after the close: an idle RX stream's transfers time out while it stops (expected, not an error) */
        bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_INFO);
    }
    if (rx_on) {
        /* per frame: the TX reports (status mode) or the demodulated copies (loop mode) that name it */
        unsigned sent = notx ? 0 : tc.sent;
        unsigned *nrep = calloc(count, sizeof(unsigned)), *ncopy = calloc(count, sizeof(unsigned));
        long *first_rep = malloc(count * sizeof(long)), *first_copy = malloc(count * sizeof(long));
        unsigned rep_noack = 0, rep_acked = 0, rep_unknown = 0, fr_exact = 0, fr_corrupt = 0, fr_foreign = 0;
        unsigned fr_runt = 0, other = 0, good = 0, missing = 0, dup = 0, wrong = 0, shown = 0;
        double lat_min = 1e9, lat_max = 0, lat_sum = 0;
        size_t j;
        if (nrep == NULL || ncopy == NULL || first_rep == NULL || first_copy == NULL) {
            return 2;
        }
        for (i = 0; i < count; i++) {
            first_rep[i] = first_copy[i] = -1;
        }
        for (j = 0; j < nev; j++) {
            struct event *e = &ev[j];
            if (e->type == TYPE_ACKED || e->type == TYPE_NO_ACK) {
                uint32_t k = e->w1 & 0xfffffu;
                if (e->type == TYPE_ACKED) {
                    rep_acked++;
                } else {
                    rep_noack++;
                }
                if ((e->w1 >> 20) == (tc.run & 0xfffu) && k < sent) {
                    e->frame = (long)k;
                    if (nrep[k]++ == 0) {
                        first_rep[k] = (long)j;
                    }
                } else if (++rep_unknown <= 5) {
                    printf("  report with an unknown cookie %08" PRIx32 ": %s, mod %u bw %u, words2,3 %08"
                           PRIx32 " %08" PRIx32 ", %u dwords, at %+.3f ms\n", e->w1, type_name(e->type), e->mod,
                           e->bw, e->w2, e->w3, e->dwords, (e->t - t0) * 1e3);
                }
            } else if (e->type == TYPE_FRAME) {
                switch (e->verdict) {
                    case V_EXACT:   fr_exact++; break;
                    case V_CORRUPT: fr_corrupt++; break;
                    case V_FOREIGN: fr_foreign++; break;
                    default:        fr_runt++; break;
                }
                if (e->verdict == V_EXACT && e->frame >= 0 && (unsigned long)e->frame < sent) {
                    if (ncopy[e->frame]++ == 0) {
                        first_copy[e->frame] = (long)j;
                    }
                } else if (e->verdict != V_EXACT && fr_corrupt + fr_foreign + fr_runt <= 5) {
                    printf("  frame received, %s: length %" PRIu32 " (sent %u+4), mod %u bw %u, FCS %s, frame "
                           "index %ld, %u dwords, at %+.3f ms\n", verdict_name(e->verdict), e->w1 & 0xffffu, len,
                           e->mod, e->bw, e->fcs_ok ? "valid" : "invalid", e->frame, e->dwords,
                           (e->t - t0) * 1e3);
                }
            } else if (++other <= 5) {
                printf("  packet of type %u (%u dwords): words1-3 %08" PRIx32 " %08" PRIx32 " %08" PRIx32
                       ", at %+.3f ms\n", e->type, e->dwords, e->w1, e->w2, e->w3, (e->t - t0) * 1e3);
            }
        }
        for (i = 0; i < sent; i++) {
            unsigned code = rate_code(rate_sel, i);
            unsigned n = loop ? ncopy[i] : nrep[i];
            long f = loop ? first_copy[i] : first_rep[i];
            bool ok = n == 1;
            const char *why = "";
            if (ok && !loop) {
                ok = ev[f].type == TYPE_NO_ACK && ev[f].bw == BW_CODE_20MHZ && ev[f].mod == retry_code(code);
                why = "report fields unexpected";
            }
            if (n == 0) {
                missing++;
                why = loop ? "no exact copy received" : "no report";
            } else if (n > 1) {
                dup++;
                why = loop ? "received more than once" : "more than one report";
            } else if (!ok) {
                wrong++;
            }
            if (ok) {
                double l = ev[f].t - tc.t_send[i];
                good++;
                lat_sum += l;
                lat_min = l < lat_min ? l : lat_min;
                lat_max = l > lat_max ? l : lat_max;
            } else if (shown++ < 10) {
                if (f >= 0) {
                    printf("  frame %u (%u Mbps, cookie %08" PRIx32 "): %s: %s mod %u bw %u (expected %s, mod %u "
                           "bw %u)\n", i, MBPS[code], cookie_of(tc.run, i), why, type_name(ev[f].type), ev[f].mod,
                           ev[f].bw, loop ? "frame" : "TX report no ACK", loop ? code : retry_code(code),
                           BW_CODE_20MHZ);
                } else {
                    printf("  frame %u (%u Mbps, cookie %08" PRIx32 "): %s\n", i, MBPS[code],
                           cookie_of(tc.run, i), why);
                }
            }
        }
        printf("frames sent %u of %u; TX reports: %u no ACK, %u ACKed, %u of them with unknown cookies; "
               "demodulated frames: %u exact, %u ours but wrong, %u not ours, %u too short; other packets %u\n",
               sent, notx ? 0 : count, rep_noack, rep_acked, rep_unknown, fr_exact, fr_corrupt, fr_foreign, fr_runt,
               other);
        if (!notx) {
            printf("frames %s: %u good, %u missing, %u duplicated, %u wrong", loop ? "received back" : "reported",
                   good, missing, dup, wrong);
            if (good > 0) {
                printf("; latency after send min/avg/max %.2f/%.2f/%.2f ms", lat_min * 1e3, lat_sum / good * 1e3,
                       lat_max * 1e3);
            }
            printf("\n");
        }
        if (rc == 0) {
            if (notx) {
                rc = nev == 0 ? 0 : 1;
            } else if (loop) {
                rc = (sent == count && good == sent && rep_noack + rep_acked == 0 &&
                      fr_corrupt + fr_foreign + fr_runt + other == 0) ? 0 : 1;
                if (fr_exact + fr_corrupt + fr_foreign + fr_runt == 0 && sent > 0) {
                    printf("NOTE: no frame came back. Expected with the stock wlan design: wlan_top blanks its "
                           "receiver while it transmits (rx_block <= wlan_tx_ota), so it cannot demodulate its "
                           "own frames; -m status tests the TX path instead (wiphy-loop.md)\n");
                }
            } else {
                rc = (sent == count && good == sent && rep_unknown == 0 &&
                      fr_corrupt + fr_foreign + fr_runt + other == 0) ? 0 : 1;
                if (fr_exact > 0) {
                    printf("NOTE: %u frames were demodulated although the stock wlan_top blanks its receiver "
                           "while it transmits\n", fr_exact);
                }
                if (sent > 1 && good < sent && nrep[0] == 1 && ev[first_rep[0]].type == TYPE_NO_ACK) {
                    printf("NOTE: frame 0 was reported but later frames were not (or wrongly): the pattern the TX "
                           "packet dword accounting of fifo_reader.vhd (dma_downcount = len - 1, commit 950189d2) "
                           "predicts for wlan_top; leftovers of an earlier run (no FPGA reload since) can also "
                           "misalign the TX FIFO (wiphy-loop.md)\n");
                }
            }
        }
        free(nrep);
        free(ncopy);
        free(first_rep);
        free(first_copy);
    }
    printf("RESULT: %s\n", rc == 0 ? "PASS" : "FAIL");
    free(ev);
    free(tc.t_send);
    free(tc.pkt);
    free(rxbuf);
    free(scratch);
    return rc;
}
