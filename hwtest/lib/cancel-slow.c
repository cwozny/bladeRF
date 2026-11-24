/*
 * cancel-slow (2026-11-23): LD_PRELOAD fault injector for the stop hang. Each
 * libusb_cancel_transfer() call runs libusb's function, then busy-waits CANCELSLOW_MS
 * milliseconds (default 0) before it returns. A busy wait, not a sleep: the thread stays in
 * the call, as in a slow ioctl, and passes no cancellation point. In the slow stops seen on
 * the xA5, each slow call took about 112 ms. Preload it after cancel-time.c to time the
 * longer calls (LD_PRELOAD=libcancel-time.so:libcancel-slow.so). TESTING.md section 23; ran
 * as gapfix/stophang/libcancelslow.so on the XPS.
 *
 * Build: cc -O2 -shared -fPIC -o libcancel-slow.so cancel-slow.c -ldl -lpthread
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

struct libusb_transfer;

static int (*real_cancel)(struct libusb_transfer *);
static long long slow_us;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void init(void)
{
    const char *s = getenv("CANCELSLOW_MS");

    real_cancel = (int (*)(struct libusb_transfer *))dlsym(RTLD_NEXT, "libusb_cancel_transfer");
    slow_us = s != NULL ? atoll(s) * 1000 : 0;
}

static long long mono_us(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

int libusb_cancel_transfer(struct libusb_transfer *transfer)
{
    int rc;

    pthread_once(&once, init);
    rc = real_cancel(transfer);
    if (slow_us > 0) {
        long long end = mono_us() + slow_us;
        while (mono_us() < end) {
        }
    }
    return rc;
}
