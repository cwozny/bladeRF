/*
 * nios-trace: LD_PRELOAD shim that logs, with timing, every synchronous bulk
 * transfer a libbladeRF program makes (on a bladeRF 2.0 these are the Nios
 * control packets: OUT 0x02 requests and IN 0x82 responses; samples use
 * asynchronous transfers and are not logged) and every bladerf_set_bandwidth /
 * bladerf_get_bandwidth call. Observation only: arguments, data and return
 * values are passed through unchanged.
 *
 * Output goes to $NIOS_TRACE_FILE (nothing is logged if it is unset). Times
 * are in microseconds since the program started (CLOCK_MONOTONIC):
 *
 *   U <start> <duration> <endpoint> <length> <transferred> <rc> <bytes...>
 *   B <start> <duration> set|get <channel> <requested> <actual> <rc>
 *
 * Built and used by ../bin/bw-diag.sh; analyze a trace with
 * ../bin/nios-trace-analyze.py.
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

    t0 = now_us();
    if (path != NULL && path[0] != '\0') {
        out = fopen(path, "w");
        if (out == NULL) {
            perror(path);
            abort();
        }
        /* Fully buffered, so that logging adds next to no time between
         * packets; flushed at exit. */
        setvbuf(out, NULL, _IOFBF, 4 << 20);
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
