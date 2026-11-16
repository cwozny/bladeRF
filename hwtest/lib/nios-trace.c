/*
 * nios-trace: LD_PRELOAD shim that logs, with timing, every synchronous bulk
 * transfer a libbladeRF program makes (on a bladeRF 2.0 these are the Nios
 * control packets: OUT 0x02 requests and IN 0x82 responses), every
 * bladerf_set_bandwidth / bladerf_get_bandwidth and bladerf_set_frequency /
 * bladerf_get_frequency call, bladerf_sync_rx / bladerf_sync_tx calls that
 * fail, and how the sample streams' asynchronous transfers (OUT 0x01, IN
 * 0x81) complete. Observation only: arguments, data and return values are
 * passed through unchanged; a sample transfer's completion goes through
 * stream_cb(), which counts it and then calls the program's own callback.
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
 *   D <time> <records>                           records dropped (ring full)
 *
 * Keeping the timing of the traced program: a hooked call takes the time
 * before and after the real call and copies a fixed-size record into a ring
 * buffer under a lock that is held only for that copy. A writer thread
 * formats the records and writes the file every 100 ms, so no hooked call
 * waits for formatting or for the file. Sample-transfer completions (about
 * 11,000 a second in firmware loopback) only update counters; they add a
 * record once a second per endpoint and for gaps and failed transfers. A run
 * that is killed loses the last ~100 ms of records.
 *
 * Built and used by ../bin/bw-diag.sh and ../bin/ctrl-threads-nuand.sh;
 * analyze a trace with ../bin/nios-trace-analyze.py or
 * ../bin/nios-trace-threads.py. Built with -DNIOS_TRACE_SELFTEST it is a
 * program that checks the ring and the writer (no USB, no bladeRF).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libusb.h>
#include <libbladeRF.h>

enum rec_type { R_U, R_B, R_F, R_R, R_X, R_G, R_S, R_E };

struct rec {
    uint8_t type;
    uint8_t ep;      /* U, X, G, S, E */
    uint8_t n;       /* U: bytes in data[] */
    uint8_t op;      /* B, F: 0 set, 1 get; R: 0 rx, 1 tx */
    int32_t a;       /* U: length; B, F: channel; X: status */
    int32_t b;       /* U: transferred; X: actual_length */
    int32_t rc;
    double t;        /* start, or the time of the event */
    double d;        /* duration; G: gap; S: max gap */
    uint64_t v1;     /* B: requested; F: frequency; S: completions */
    int64_t v2;      /* B: actual */
    uint8_t data[16];
};

#define RING_RECS (1u << 18) /* 16 MiB */

static FILE *out;
static double t0;
static struct rec *ring;
static uint64_t head, tail, dropped; /* under ring_lock */
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t writer;
static pthread_once_t writer_once = PTHREAD_ONCE_INIT;
static int writer_running, writer_stop;

static void start_writer(void);

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static void put(struct rec const *r)
{
    pthread_once(&writer_once, start_writer);
    pthread_mutex_lock(&ring_lock);
    if (head - tail < RING_RECS) {
        ring[head % RING_RECS] = *r;
        head++;
    } else {
        dropped++;
    }
    pthread_mutex_unlock(&ring_lock);
}

static void format_rec(struct rec const *r)
{
    int i;

    switch (r->type) {
        case R_U:
            fprintf(out, "U %.1f %.1f %02x %d %d %d", r->t, r->d, r->ep, r->a,
                    r->b, r->rc);
            for (i = 0; i < r->n; i++) {
                fprintf(out, " %02x", r->data[i]);
            }
            fputc('\n', out);
            break;
        case R_B:
            if (r->op == 0) {
                fprintf(out, "B %.1f %.1f set %d %u %ld %d\n", r->t, r->d, r->a,
                        (unsigned)r->v1, (long)r->v2, r->rc);
            } else {
                fprintf(out, "B %.1f %.1f get %d 0 %ld %d\n", r->t, r->d, r->a,
                        (long)r->v2, r->rc);
            }
            break;
        case R_F:
            fprintf(out, "F %.1f %.1f %s %d %llu %d\n", r->t, r->d,
                    r->op == 0 ? "set" : "get", r->a,
                    (unsigned long long)r->v1, r->rc);
            break;
        case R_R:
            fprintf(out, "R %.1f %.1f %s %d\n", r->t, r->d,
                    r->op == 0 ? "rx" : "tx", r->rc);
            break;
        case R_X:
            fprintf(out, "X %.1f %02x %d %d\n", r->t, r->ep, r->a, r->b);
            break;
        case R_G:
            fprintf(out, "G %.1f %02x %.1f\n", r->t, r->ep, r->d);
            break;
        case R_S:
            fprintf(out, "S %.1f %02x %llu %.1f\n", r->t, r->ep,
                    (unsigned long long)r->v1, r->d);
            break;
        case R_E:
            fprintf(out, "E %.1f %02x %d\n", r->t, r->ep, r->rc);
            break;
        default:
            break;
    }
}

/* Writes out the records put so far; only the writer thread (and, once it
 * has stopped, trace_fini) calls this. */
static void drain(void)
{
    uint64_t h, t, d;

    pthread_mutex_lock(&ring_lock);
    h = head;
    t = tail;
    d = dropped;
    dropped = 0;
    pthread_mutex_unlock(&ring_lock);

    for (; t < h; t++) {
        format_rec(&ring[t % RING_RECS]);
    }
    if (d != 0) {
        fprintf(out, "D %.1f %llu\n", now_us() - t0, (unsigned long long)d);
    }
    fflush(out);

    pthread_mutex_lock(&ring_lock);
    tail = h;
    pthread_mutex_unlock(&ring_lock);
}

static void *writer_main(void *arg)
{
    struct timespec const period = { 0, 100 * 1000 * 1000 };

    (void)arg;
    while (!__atomic_load_n(&writer_stop, __ATOMIC_ACQUIRE)) {
        nanosleep(&period, NULL);
        drain();
    }
    return NULL;
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

/* Started on the first record rather than in the constructor, so that no
 * thread is created while the dynamic loader runs constructors. */
static void start_writer(void)
{
    sigset_t all, old;

    /* The writer thread takes no signals: they go to the program's threads. */
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    writer_running = pthread_create(&writer, NULL, writer_main, NULL) == 0;
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (!writer_running) {
        fprintf(stderr, "nios-trace: cannot start the writer thread\n");
        abort();
    }
}

static void trace_open(const char *path)
{
    struct timespec wall;

    t0 = now_us();
    clock_gettime(CLOCK_REALTIME, &wall);
    if (path == NULL || path[0] == '\0') {
        return;
    }
    ring = calloc(RING_RECS, sizeof(*ring));
    out  = fopen(path, "w");
    if (ring == NULL || out == NULL) {
        perror(path);
        abort();
    }
    setvbuf(out, NULL, _IOFBF, 1 << 20);
    fprintf(out, "T %lld.%06ld\n", (long long)wall.tv_sec, wall.tv_nsec / 1000);
}

static void trace_close(void)
{
    if (out == NULL) {
        return;
    }
    if (writer_running) {
        __atomic_store_n(&writer_stop, 1, __ATOMIC_RELEASE);
        pthread_join(writer, NULL);
        writer_running = 0;
    }
    drain();
}

#ifndef NIOS_TRACE_SELFTEST
__attribute__((constructor)) static void trace_init(void)
{
    trace_open(getenv("NIOS_TRACE_FILE"));
}

__attribute__((destructor)) static void trace_fini(void)
{
    trace_close();
}
#endif

typedef int (*bulk_fn)(libusb_device_handle *, unsigned char, unsigned char *,
                       int, int *, unsigned int);

int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep,
                         unsigned char *data, int len, int *transferred,
                         unsigned int timeout)
{
    static bulk_fn real;
    struct rec r = { 0 };
    double t;
    int rc, n;

    if (real == NULL) {
        real = (bulk_fn)real_sym("libusb_bulk_transfer");
    }

    t  = now_us();
    rc = real(h, ep, data, len, transferred, timeout);

    if (out != NULL) {
        r.d    = now_us() - t;
        r.t    = t - t0;
        r.type = R_U;
        r.ep   = ep;
        r.a    = len;
        r.b    = n = (transferred != NULL) ? *transferred : 0;
        r.rc   = rc;
        r.n    = (uint8_t)(n < 0 ? 0 : n > 16 ? 16 : n);
        memcpy(r.data, data, r.n);
        put(&r);
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
    struct rec r = { 0 };
    double t;
    int rc;

    if (real == NULL) {
        real = (set_bw_fn)real_sym("bladerf_set_bandwidth");
    }

    t  = now_us();
    rc = real(dev, ch, bandwidth, actual);

    if (out != NULL) {
        r.d    = now_us() - t;
        r.t    = t - t0;
        r.type = R_B;
        r.op   = 0;
        r.a    = (int32_t)ch;
        r.v1   = bandwidth;
        r.v2   = actual != NULL ? (int64_t)*actual : -1;
        r.rc   = rc;
        put(&r);
    }
    return rc;
}

int bladerf_get_bandwidth(struct bladerf *dev, bladerf_channel ch,
                          bladerf_bandwidth *bandwidth)
{
    static get_bw_fn real;
    struct rec r = { 0 };
    double t;
    int rc;

    if (real == NULL) {
        real = (get_bw_fn)real_sym("bladerf_get_bandwidth");
    }

    t  = now_us();
    rc = real(dev, ch, bandwidth);

    if (out != NULL) {
        r.d    = now_us() - t;
        r.t    = t - t0;
        r.type = R_B;
        r.op   = 1;
        r.a    = (int32_t)ch;
        r.v2   = bandwidth != NULL ? (int64_t)*bandwidth : -1;
        r.rc   = rc;
        put(&r);
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
    struct rec r = { 0 };
    double t;
    int rc;

    if (real == NULL) {
        real = (set_freq_fn)real_sym("bladerf_set_frequency");
    }

    t  = now_us();
    rc = real(dev, ch, frequency);

    if (out != NULL) {
        r.d    = now_us() - t;
        r.t    = t - t0;
        r.type = R_F;
        r.op   = 0;
        r.a    = (int32_t)ch;
        r.v1   = frequency;
        r.rc   = rc;
        put(&r);
    }
    return rc;
}

int bladerf_get_frequency(struct bladerf *dev, bladerf_channel ch,
                          bladerf_frequency *frequency)
{
    static get_freq_fn real;
    struct rec r = { 0 };
    double t;
    int rc;

    if (real == NULL) {
        real = (get_freq_fn)real_sym("bladerf_get_frequency");
    }

    t  = now_us();
    rc = real(dev, ch, frequency);

    if (out != NULL) {
        r.d    = now_us() - t;
        r.t    = t - t0;
        r.type = R_F;
        r.op   = 1;
        r.a    = (int32_t)ch;
        r.v1   = frequency != NULL ? *frequency : 0;
        r.rc   = rc;
        put(&r);
    }
    return rc;
}

typedef int (*sync_rx_fn)(struct bladerf *, void *, unsigned int,
                          struct bladerf_metadata *, unsigned int);
typedef int (*sync_tx_fn)(struct bladerf *, void const *, unsigned int,
                          struct bladerf_metadata *, unsigned int);

static void put_sync_failure(double t, int tx, int rc)
{
    struct rec r = { 0 };

    r.d    = now_us() - t;
    r.t    = t - t0;
    r.type = R_R;
    r.op   = (uint8_t)tx;
    r.rc   = rc;
    put(&r);
}

int bladerf_sync_rx(struct bladerf *dev, void *samples,
                    unsigned int num_samples,
                    struct bladerf_metadata *metadata, unsigned int timeout_ms)
{
    static sync_rx_fn real;
    double t;
    int rc;

    if (real == NULL) {
        real = (sync_rx_fn)real_sym("bladerf_sync_rx");
    }

    t  = now_us();
    rc = real(dev, samples, num_samples, metadata, timeout_ms);

    if (out != NULL && rc != 0) {
        put_sync_failure(t, 0, rc);
    }
    return rc;
}

int bladerf_sync_tx(struct bladerf *dev, void const *samples,
                    unsigned int num_samples,
                    struct bladerf_metadata *metadata, unsigned int timeout_ms)
{
    static sync_tx_fn real;
    double t;
    int rc;

    if (real == NULL) {
        real = (sync_tx_fn)real_sym("bladerf_sync_tx");
    }

    t  = now_us();
    rc = real(dev, samples, num_samples, metadata, timeout_ms);

    if (out != NULL && rc != 0) {
        put_sync_failure(t, 1, rc);
    }
    return rc;
}

/* Sample transfers. libbladeRF gives every transfer on an endpoint the same
 * callback; stream_cb() calls it after counting the completion. Completions
 * run one at a time (inside libusb's event handling), but stats_lock keeps
 * the counters right even if they did not. */
static libusb_transfer_cb_fn orig_cb[2]; /* [0]: OUT 0x01, [1]: IN 0x81 */
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    double last, window, max_gap;
    uint64_t count;
} eps[2];

static int ep_index(unsigned char ep)
{
    return ep == 0x01 ? 0 : ep == 0x81 ? 1 : -1;
}

static void LIBUSB_CALL stream_cb(struct libusb_transfer *x)
{
    double const t = now_us() - t0;
    int const e    = ep_index(x->endpoint);
    libusb_transfer_cb_fn cb;
    struct rec r = { 0 };

    if (e < 0) { /* not wrapped by libusb_submit_transfer() below */
        return;
    }
    cb = __atomic_load_n(&orig_cb[e], __ATOMIC_ACQUIRE);

    if (out != NULL) {
        r.ep = x->endpoint;
        if (x->status != LIBUSB_TRANSFER_COMPLETED) {
            r.type = R_X;
            r.t    = t;
            r.a    = (int32_t)x->status;
            r.b    = x->actual_length;
            put(&r);
        }
        pthread_mutex_lock(&stats_lock);
        if (eps[e].last > 0) {
            double const gap = t - eps[e].last;
            if (gap > 100e3) {
                r.type = R_G;
                r.t    = t;
                r.d    = gap;
                put(&r);
            }
            if (gap > eps[e].max_gap) {
                eps[e].max_gap = gap;
            }
        }
        eps[e].last = t;
        eps[e].count++;
        if (t - eps[e].window >= 1e6) {
            r.type         = R_S;
            r.t            = t;
            r.v1           = eps[e].count;
            r.d            = eps[e].max_gap;
            eps[e].count   = 0;
            eps[e].max_gap = 0;
            eps[e].window  = t;
            put(&r);
        }
        pthread_mutex_unlock(&stats_lock);
    }

    if (cb != NULL) {
        cb(x);
    }
}

typedef int (*submit_fn)(struct libusb_transfer *);

int libusb_submit_transfer(struct libusb_transfer *x)
{
    static submit_fn real;
    int const e = ep_index(x->endpoint);
    struct rec r = { 0 };
    int rc;

    if (real == NULL) {
        real = (submit_fn)real_sym("libusb_submit_transfer");
    }

    if (out != NULL && e >= 0 && x->callback != stream_cb &&
        x->callback != NULL) {
        libusb_transfer_cb_fn expected = NULL;
        libusb_transfer_cb_fn const cb = x->callback;

        /* Follow the endpoint's transfers if they all use one callback;
         * a transfer with another callback is passed on unwrapped. */
        if (__atomic_compare_exchange_n(&orig_cb[e], &expected, cb, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ||
            expected == cb) {
            x->callback = stream_cb;
        }
    }

    rc = real(x);

    if (rc != 0 && e >= 0 && out != NULL) {
        r.type = R_E;
        r.t    = now_us() - t0;
        r.ep   = x->endpoint;
        r.rc   = rc;
        put(&r);
    }
    return rc;
}

#ifdef NIOS_TRACE_SELFTEST
/* nios-trace-selftest <file>: four threads put 250,000 records each, in
 * chunks of 25,000 with 150 ms between chunks (so the ring wraps four times
 * but never overflows) while the writer runs; then checks that every record
 * is in the file once and in each thread's order, and reports how long put()
 * takes. */
#define SELFTEST_THREADS 4
#define SELFTEST_RECS 250000
#define SELFTEST_CHUNK 25000

static double put_ns[SELFTEST_THREADS];

static void *selftest_thread(void *arg)
{
    struct timespec const pause = { 0, 150 * 1000 * 1000 };
    int const id = (int)(intptr_t)arg;
    struct rec r = { 0 };
    double t = 0, busy = 0;
    int i;

    r.type = R_F;
    r.a    = id;
    for (i = 0; i < SELFTEST_RECS; i++) {
        if (i % SELFTEST_CHUNK == 0) {
            if (i != 0) {
                nanosleep(&pause, NULL);
            }
            t = now_us();
        }
        r.v1 = (uint64_t)i;
        put(&r);
        if (i % SELFTEST_CHUNK == SELFTEST_CHUNK - 1) {
            busy += now_us() - t;
        }
    }
    put_ns[id] = busy * 1e3 / SELFTEST_RECS;
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t th[SELFTEST_THREADS];
    long next[SELFTEST_THREADS] = { 0 };
    char line[256];
    long lines = 0, bad = 0;
    FILE *f;
    int i;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 2;
    }
    trace_open(argv[1]);
    for (i = 0; i < SELFTEST_THREADS; i++) {
        pthread_create(&th[i], NULL, selftest_thread, (void *)(intptr_t)i);
    }
    for (i = 0; i < SELFTEST_THREADS; i++) {
        pthread_join(th[i], NULL);
    }
    trace_close();
    fclose(out);
    out = NULL;

    f = fopen(argv[1], "r");
    if (f == NULL) {
        perror(argv[1]);
        return 2;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        double a, b;
        int ch, rc;
        unsigned long long v;
        char op[8];

        if (line[0] == 'T') {
            continue;
        }
        if (line[0] == 'D') {
            printf("dropped: %s", line);
            bad++;
            continue;
        }
        if (sscanf(line, "F %lf %lf %7s %d %llu %d", &a, &b, op, &ch, &v,
                   &rc) != 6 || ch < 0 || ch >= SELFTEST_THREADS ||
            (long)v != next[ch]) {
            bad++;
            continue;
        }
        next[ch]++;
        lines++;
    }
    fclose(f);
    for (i = 0; i < SELFTEST_THREADS; i++) {
        if (next[i] != SELFTEST_RECS) {
            bad++;
        }
        printf("thread %d: %ld records in order, put() %.0f ns each\n", i,
               next[i], put_ns[i]);
    }
    printf("%ld records, %ld problems\n", lines, bad);
    return bad == 0 ? 0 : 1;
}
#endif
