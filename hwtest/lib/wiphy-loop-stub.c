/* Stub libbladeRF for desk-testing wiphy-loop without hardware. It emulates what bladeRF-wiphy's wlan_top sends
 * back for each TX packet, per STUB:
 *   status (default): unicast data frame -> type-3 report (cookie, retry rate code, bw 2); broadcast -> nothing
 *   loopok:   as status, plus a demodulated copy (type 1: MPDU + FCS) of every frame (receiver not blanked)
 *   misalign: only the first frame after "FPGA load" is handled; later ones produce nothing
 *   garbage:  first frame OK; later frames produce a report with a garbage cookie (as a shifted header would)
 *   corrupt:  loopok, but one payload byte of every demodulated copy is flipped (FCS recomputed)
 *   stale:    status, plus one report with a foreign cookie when RX is enabled
 *   silent:   nothing ever comes back
 *   nocore:   config GPIO bit 28 clear;  fpgatune: FPGA tuning mode;  noopen: bladerf_open fails
 * STUB_TRACE=1 prints every call to stderr. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <libbladeRF.h>

#define QMAX 4096
static struct { uint8_t *p; unsigned dwords; } q[QMAX];
static unsigned qh, qt, ntx;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int dummy;

static const char *mode(void) { const char *s = getenv("STUB"); return s ? s : "status"; }
static void trace(const char *f) { if (getenv("STUB_TRACE")) fprintf(stderr, "STUB: %s\n", f); }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t get16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }
static uint32_t crc32_ieee(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--) { c ^= *p++; for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u))); }
    return ~c;
}

static void push(const uint8_t *p, unsigned dwords)
{
    pthread_mutex_lock(&m);
    q[qt % QMAX].p = malloc(dwords * 4);
    memcpy(q[qt % QMAX].p, p, dwords * 4);
    q[qt % QMAX].dwords = dwords;
    qt++;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&m);
}

static void report(unsigned type, unsigned bw, unsigned mod, uint32_t w1)
{
    uint8_t r[16] = { 0 };
    put32(r, type | bw << 16 | mod << 24);
    put32(r + 4, w1);
    push(r, 4);
}

int bladerf_open(struct bladerf **device, const char *id)
{
    (void)id; trace("bladerf_open");
    if (!strcmp(mode(), "noopen")) return BLADERF_ERR_NODEV;
    *device = (struct bladerf *)&dummy;
    return 0;
}
void bladerf_close(struct bladerf *d) { (void)d; trace("bladerf_close"); }
int bladerf_config_gpio_read(struct bladerf *d, uint32_t *v)
{
    (void)d; trace("bladerf_config_gpio_read");
    *v = strcmp(mode(), "nocore") ? 0x10080000u | BLADERF_GPIO_PACKET_CORE_PRESENT : 0x00000057u;
    return 0;
}
int bladerf_get_tuning_mode(struct bladerf *d, bladerf_tuning_mode *t)
{
    (void)d; trace("bladerf_get_tuning_mode");
    *t = strcmp(mode(), "fpgatune") ? BLADERF_TUNING_MODE_HOST : BLADERF_TUNING_MODE_FPGA;
    return 0;
}
int bladerf_set_frequency(struct bladerf *d, bladerf_channel c, bladerf_frequency f) { (void)d; (void)c; (void)f; trace("bladerf_set_frequency"); return 0; }
int bladerf_set_sample_rate(struct bladerf *d, bladerf_channel c, bladerf_sample_rate r, bladerf_sample_rate *a) { (void)d; (void)c; trace("bladerf_set_sample_rate"); if (a) *a = r; return 0; }
int bladerf_set_bandwidth(struct bladerf *d, bladerf_channel c, bladerf_bandwidth b, bladerf_bandwidth *a) { (void)d; (void)c; trace("bladerf_set_bandwidth"); if (a) *a = b; return 0; }
int bladerf_set_gain(struct bladerf *d, bladerf_channel c, bladerf_gain g) { (void)d; (void)c; (void)g; trace("bladerf_set_gain"); return 0; }
int bladerf_set_loopback(struct bladerf *d, bladerf_loopback l) { (void)d; (void)l; trace("bladerf_set_loopback"); return 0; }
int bladerf_sync_config(struct bladerf *d, bladerf_channel_layout l, bladerf_format f, unsigned nb, unsigned bs, unsigned nx, unsigned to)
{
    (void)d; (void)l; (void)nb; (void)bs; (void)nx; (void)to; trace("bladerf_sync_config");
    return f == BLADERF_FORMAT_PACKET_META ? 0 : BLADERF_ERR_INVAL;
}
int bladerf_enable_module(struct bladerf *d, bladerf_channel c, bool e)
{
    (void)d; trace("bladerf_enable_module");
    if (c == BLADERF_CHANNEL_RX(0) && e && !strcmp(mode(), "stale")) report(3, 2, 0, 0xabc00007u);
    return 0;
}
int bladerf_sync_tx(struct bladerf *d, const void *s, unsigned n, struct bladerf_metadata *md, unsigned to)
{
    const uint8_t *p = s, *mp = p + 16;
    unsigned mod = get16(p + 4), bw = get16(p + 6), len = get16(p + 8), k = ntx++;
    uint32_t cookie = get32(p + 12);
    int bcast = !memcmp(mp + 4, "\xff\xff\xff\xff\xff\xff", 6), ctrl = ((mp[0] >> 2) & 3) == 1;
    (void)d; (void)to; trace("bladerf_sync_tx");
    if (md == NULL || n != (16 + len + 3) / 4) { fprintf(stderr, "STUB: bad sync_tx (n %u len %u)\n", n, len); return BLADERF_ERR_INVAL; }
    if (!strcmp(mode(), "silent")) return 0;
    if (!strcmp(mode(), "misalign") && k > 0) return 0;
    if (!strcmp(mode(), "garbage") && k > 0) { report(3, 0, 0, len); return 0; }
    if (!bcast && !ctrl && len >= 11) report(3, bw, mod ? mod - 1 : 0, cookie);
    if (!strcmp(mode(), "loopok") || !strcmp(mode(), "corrupt")) {
        unsigned flen = len + 4, nd = 4 + (flen + 3) / 4;
        uint8_t *f = calloc(nd, 4);
        put32(f, 1 | bw << 16 | mod << 24);
        put32(f + 4, flen);
        memcpy(f + 16, mp, len);
        if (!strcmp(mode(), "corrupt")) f[16 + len - 1] ^= 0x40;
        put32(f + 16 + len, crc32_ieee(f + 16, len));
        push(f, nd);
        free(f);
    }
    return 0;
}
int bladerf_sync_rx(struct bladerf *d, void *s, unsigned n, struct bladerf_metadata *md, unsigned to)
{
    struct timeval tv;
    struct timespec ts;
    int rc = 0;
    (void)d; (void)n;
    gettimeofday(&tv, NULL);
    ts.tv_sec = tv.tv_sec + to / 1000;
    ts.tv_nsec = tv.tv_usec * 1000L + (to % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&m);
    while (qh == qt && rc == 0) rc = pthread_cond_timedwait(&cv, &m, &ts);
    if (qh == qt) { pthread_mutex_unlock(&m); return BLADERF_ERR_TIMEOUT; }
    memcpy(s, q[qh % QMAX].p, q[qh % QMAX].dwords * 4);
    md->actual_count = q[qh % QMAX].dwords;
    free(q[qh % QMAX].p);
    qh++;
    pthread_mutex_unlock(&m);
    return 0;
}
const char *bladerf_strerror(int e) { static char b[32]; snprintf(b, sizeof b, "stub error %d", e); return b; }
void bladerf_log_set_verbosity(bladerf_log_level l) { (void)l; trace("bladerf_log_set_verbosity"); }
