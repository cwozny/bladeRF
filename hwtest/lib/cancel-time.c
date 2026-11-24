/*
 * cancel-time (2026-11-23): LD_PRELOAD library that times every libusb_cancel_transfer()
 * call. It changes nothing: it calls libusb's own function and, after the call returns,
 * appends one line to $CANCELSHIM_LOG (O_APPEND, one write per line):
 *
 *   C <pid> <tid> <start, wall clock us> <duration us> <return code>
 *
 * A call that never returns is not logged (the hang dump shows it). bin/cancel-stats.py
 * groups the lines into the cancel rounds of each stop. TESTING.md section 23; ran as
 * gapfix/stophang/libcancelshim.so on the XPS.
 *
 * Build: cc -O2 -g -Wall -Wextra -shared -fPIC -o libcancel-time.so cancel-time.c -ldl -lpthread
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

struct libusb_transfer;

static int (*real_cancel)(struct libusb_transfer *);
static int log_fd = -1;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void init(void)
{
    const char *p = getenv("CANCELSHIM_LOG");

    real_cancel = (int (*)(struct libusb_transfer *))dlsym(RTLD_NEXT, "libusb_cancel_transfer");
    if (p != NULL && *p != '\0') {
        log_fd = open(p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }
}

static int64_t clock_us(clockid_t c)
{
    struct timespec t;
    clock_gettime(c, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

int libusb_cancel_transfer(struct libusb_transfer *transfer)
{
    int64_t wall, t0, t1;
    int rc;

    pthread_once(&once, init);
    if (real_cancel == NULL) {
        return -99;
    }
    wall = clock_us(CLOCK_REALTIME);
    t0 = clock_us(CLOCK_MONOTONIC);
    rc = real_cancel(transfer);
    t1 = clock_us(CLOCK_MONOTONIC);
    if (log_fd >= 0) {
        char line[128];
        int n = snprintf(line, sizeof(line), "C %d %ld %lld %lld %d\n", (int)getpid(),
                         (long)syscall(SYS_gettid), (long long)wall, (long long)(t1 - t0), rc);
        if (n > 0 && (size_t)n < sizeof(line)) {
            ssize_t w = write(log_fd, line, (size_t)n);
            (void)w;
        }
    }
    return rc;
}
