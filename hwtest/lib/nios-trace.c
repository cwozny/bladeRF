/*
 * nios-trace: LD_PRELOAD shim that logs, with timing, every synchronous bulk
 * transfer a libbladeRF program makes (on a bladeRF 2.0 these are the Nios
 * control packets: OUT 0x02 requests and IN 0x82 responses), every
 * bladerf_set_bandwidth / bladerf_get_bandwidth and bladerf_set_frequency /
 * bladerf_get_frequency call, bladerf_sync_rx / bladerf_sync_tx calls that
 * fail, and how the sample streams' asynchronous transfers (OUT 0x01, IN
 * 0x81) complete. Observation only: arguments, data and return values are
 * passed through unchanged; a sample transfer's completion goes through
 * stream_cb(), which logs it and then calls the program's own callback.
 *
 * Output goes to $NIOS_TRACE_FILE (nothing is logged if it is unset). Times
 * are in microseconds since the program started (CLOCK_MONOTONIC):
 *
 *   T <wall-clock time of the start, seconds since the epoch>
 *   U <start> <duration> <endpoint> <length> <transferred> <rc> <bytes...>
 *   B <start> <duration> set|get <channel> <requested> <actual> <rc>
 *   F <start> <duration> set|get <channel> <frequency> <rc>
 *   R <start> <duration> rx|tx <rc>             failed bladerf_sync_rx/tx call
 *   X <time> <endpoint> <status> <actual_length> sample transfer that did not
 *                                                complete normally
 *   G <time> <endpoint> <gap>                    sample transfer completing more
 *                                                than 100 ms after the previous
 *                                                one on its endpoint
 *   S <time> <endpoint> <completions> <max gap>  once a second per endpoint
 *   E <time> <endpoint> <rc>                     failed libusb_submit_transfer()
 *
 * The log is fully buffered and flushed with each S line and at exit, so a
 * run that is killed loses at most about a second of it.
 *
 * Built and used by ../bin/bw-diag.sh and ../bin/ctrl-threads-nuand.sh;
 * analyze a trace with ../bin/nios-trace-analyze.py or
 * ../bin/nios-trace-threads.py.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <libusb.h>
#include <libbladeRF.h>

static FILE *out;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static double t0;

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static void *real_sym(const char *name)
{
    void *p = dlsym(RTLD_NEXT, name);
    if (p == NULL) {
        fprintf(stderr, "nios-trace: %s not found: %s\n", name, dlerror());
        abort();
    }
    return p;
}

__attribute__((constructor)) static void trace_init(void)
{
    const char *path = getenv("NIOS_TRACE_FILE");

    struct timespec wall;

    t0 = now_us();
    clock_gettime(CLOCK_REALTIME, &wall);
    if (path != NULL && path[0] != '\0') {
        out = fopen(path, "w");
        if (out == NULL) {
            perror(path);
            abort();
        }
        /* Fully buffered, so that logging adds next to no time between
         * packets; flushed with each S line and at exit. */
        setvbuf(out, NULL, _IOFBF, 4 << 20);
        fprintf(out, "T %lld.%06ld\n", (long long)wall.tv_sec,
                wall.tv_nsec / 1000);
    }
}

__attribute__((destructor)) static void trace_fini(void)
{
    if (out != NULL) {
        fflush(out);
    }
}

typedef int (*bulk_fn)(libusb_device_handle *, unsigned char, unsigned char *,
                       int, int *, unsigned int);

int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep,
                         unsigned char *data, int len, int *transferred,
                         unsigned int timeout)
{
    static bulk_fn real;
    double t, d;
    int rc, n, i;

    if (real == NULL) {
        real = (bulk_fn)real_sym("libusb_bulk_transfer");
    }

    t  = now_us();
    rc = real(h, ep, data, len, transferred, timeout);
    d  = now_us() - t;

    if (out != NULL) {
        n = (transferred != NULL) ? *transferred : 0;
        pthread_mutex_lock(&lock);
        fprintf(out, "U %.1f %.1f %02x %d %d %d", t - t0, d, ep, len, n, rc);
        for (i = 0; i < n && i < 16; i++) {
            fprintf(out, " %02x", data[i]);
        }
        fputc('\n', out);
        pthread_mutex_unlock(&lock);
    }
    return rc;
}

typedef int (*set_bw_fn)(struct bladerf *, bladerf_channel, bladerf_bandwidth,
                         bladerf_bandwidth *);
typedef int (*get_bw_fn)(struct bladerf *, bladerf_channel,
                         bladerf_bandwidth *);

int bladerf_set_bandwidth(struct bladerf *dev, bladerf_channel ch,
                          bladerf_bandwidth bandwidth,
                          bladerf_bandwidth *actual)
{
    static set_bw_fn real;
    double t, d;
    int rc;

    if (real == NULL) {
        real = (set_bw_fn)real_sym("bladerf_set_bandwidth");
    }

    t  = now_us();
    rc = real(dev, ch, bandwidth, actual);
    d  = now_us() - t;

    if (out != NULL) {
        pthread_mutex_lock(&lock);
        fprintf(out, "B %.1f %.1f set %d %u %ld %d\n", t - t0, d, (int)ch,
                bandwidth, actual != NULL ? (long)*actual : -1L, rc);
        pthread_mutex_unlock(&lock);
    }
    return rc;
}

int bladerf_get_bandwidth(struct bladerf *dev, bladerf_channel ch,
                          bladerf_bandwidth *bandwidth)
{
    static get_bw_fn real;
    double t, d;
    int rc;

    if (real == NULL) {
        real = (get_bw_fn)real_sym("bladerf_get_bandwidth");
    }

    t  = now_us();
    rc = real(dev, ch, bandwidth);
    d  = now_us() - t;

    if (out != NULL) {
        pthread_mutex_lock(&lock);
        fprintf(out, "B %.1f %.1f get %d 0 %ld %d\n", t - t0, d, (int)ch,
                bandwidth != NULL ? (long)*bandwidth : -1L, rc);
        pthread_mutex_unlock(&lock);
    }
    return rc;
}

typedef int (*set_freq_fn)(struct bladerf *, bladerf_channel,
                           bladerf_frequency);
typedef int (*get_freq_fn)(struct bladerf *, bladerf_channel,
                           bladerf_frequency *);

int bladerf_set_frequency(struct bladerf *dev, bladerf_channel ch,
                          bladerf_frequency frequency)
{
    static set_freq_fn real;
    double t, d;
    int rc;

    if (real == NULL) {
        real = (set_freq_fn)real_sym("bladerf_set_frequency");
    }

    t  = now_us();
    rc = real(dev, ch, frequency);
    d  = now_us() - t;

    if (out != NULL) {
        pthread_mutex_lock(&lock);
        fprintf(out, "F %.1f %.1f set %d %llu %d\n", t - t0, d, (int)ch,
                (unsigned long long)frequency, rc);
        pthread_mutex_unlock(&lock);
    }
    return rc;
}

int bladerf_get_frequency(struct bladerf *dev, bladerf_channel ch,
                          bladerf_frequency *frequency)
{
    static get_freq_fn real;
    double t, d;
    int rc;

    if (real == NULL) {
        real = (get_freq_fn)real_sym("bladerf_get_frequency");
    }

    t  = now_us();
    rc = real(dev, ch, frequency);
    d  = now_us() - t;

    if (out != NULL) {
        pthread_mutex_lock(&lock);
        fprintf(out, "F %.1f %.1f get %d %llu %d\n", t - t0, d, (int)ch,
                frequency != NULL ? (unsigned long long)*frequency : 0ULL, rc);
        pthread_mutex_unlock(&lock);
    }
    return rc;
}

typedef int (*sync_rx_fn)(struct bladerf *, void *, unsigned int,
                          struct bladerf_metadata *, unsigned int);
typedef int (*sync_tx_fn)(struct bladerf *, void const *, unsigned int,
                          struct bladerf_metadata *, unsigned int);

int bladerf_sync_rx(struct bladerf *dev, void *samples,
                    unsigned int num_samples,
                    struct bladerf_metadata *metadata, unsigned int timeout_ms)
{
    static sync_rx_fn real;
    double t, d;
    int rc;

    if (real == NULL) {
        real = (sync_rx_fn)real_sym("bladerf_sync_rx");
    }

    t  = now_us();
    rc = real(dev, samples, num_samples, metadata, timeout_ms);
    d  = now_us() - t;

    if (out != NULL && rc != 0) {
        pthread_mutex_lock(&lock);
        fprintf(out, "R %.1f %.1f rx %d\n", t - t0, d, rc);
        pthread_mutex_unlock(&lock);
    }
    return rc;
}

int bladerf_sync_tx(struct bladerf *dev, void const *samples,
                    unsigned int num_samples,
                    struct bladerf_metadata *metadata, unsigned int timeout_ms)
{
    static sync_tx_fn real;
    double t, d;
    int rc;

    if (real == NULL) {
        real = (sync_tx_fn)real_sym("bladerf_sync_tx");
    }

    t  = now_us();
    rc = real(dev, samples, num_samples, metadata, timeout_ms);
    d  = now_us() - t;

    if (out != NULL && rc != 0) {
        pthread_mutex_lock(&lock);
        fprintf(out, "R %.1f %.1f tx %d\n", t - t0, d, rc);
        pthread_mutex_unlock(&lock);
    }
    return rc;
}

/* Sample transfers: the program's callback for each transfer, so that
 * stream_cb() can log a completion and then pass it on. */
#define MAX_XFERS 256
static struct {
    struct libusb_transfer *xfer;
    libusb_transfer_cb_fn cb;
} xfers[MAX_XFERS];

static struct {
    double last, window, max_gap;
    unsigned long count;
} eps[2]; /* [0]: OUT 0x01, [1]: IN 0x81 */

static int ep_index(unsigned char ep)
{
    return ep == 0x01 ? 0 : ep == 0x81 ? 1 : -1;
}

static void LIBUSB_CALL stream_cb(struct libusb_transfer *x)
{
    libusb_transfer_cb_fn cb = NULL;
    double const t = now_us() - t0;
    int const e = ep_index(x->endpoint);
    int i;

    pthread_mutex_lock(&lock);
    for (i = 0; i < MAX_XFERS; i++) {
        if (xfers[i].xfer == x) {
            cb = xfers[i].cb;
            break;
        }
    }
    if (out != NULL && e >= 0) {
        if (x->status != LIBUSB_TRANSFER_COMPLETED) {
            fprintf(out, "X %.1f %02x %d %d\n", t, x->endpoint, (int)x->status,
                    x->actual_length);
        }
        if (eps[e].last > 0) {
            double const gap = t - eps[e].last;
            if (gap > 100e3) {
                fprintf(out, "G %.1f %02x %.1f\n", t, x->endpoint, gap);
            }
            if (gap > eps[e].max_gap) {
                eps[e].max_gap = gap;
            }
        }
        eps[e].last = t;
        eps[e].count++;
        if (t - eps[e].window >= 1e6) {
            fprintf(out, "S %.1f %02x %lu %.1f\n", t, x->endpoint,
                    eps[e].count, eps[e].max_gap);
            eps[e].count   = 0;
            eps[e].max_gap = 0;
            eps[e].window  = t;
            fflush(out);
        }
    }
    pthread_mutex_unlock(&lock);

    if (cb != NULL) {
        cb(x);
    }
}

typedef int (*submit_fn)(struct libusb_transfer *);

int libusb_submit_transfer(struct libusb_transfer *x)
{
    static submit_fn real;
    int const e = ep_index(x->endpoint);
    int rc, i, slot = -1;

    if (real == NULL) {
        real = (submit_fn)real_sym("libusb_submit_transfer");
    }

    if (e >= 0 && x->callback != stream_cb) {
        pthread_mutex_lock(&lock);
        for (i = 0; i < MAX_XFERS; i++) {
            if (xfers[i].xfer == x) {
                slot = i;
                break;
            }
            if (xfers[i].xfer == NULL && slot < 0) {
                slot = i;
            }
        }
        if (slot >= 0) { /* else: not followed */
            xfers[slot].xfer = x;
            xfers[slot].cb   = x->callback;
            x->callback      = stream_cb;
        }
        pthread_mutex_unlock(&lock);
    }

    rc = real(x);

    if (rc != 0 && e >= 0 && out != NULL) {
        pthread_mutex_lock(&lock);
        fprintf(out, "E %.1f %02x %d\n", now_us() - t0, x->endpoint, rc);
        pthread_mutex_unlock(&lock);
    }
    return rc;
}
