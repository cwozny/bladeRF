/*
 * late-reply: LD_PRELOAD fault injector that reproduces a late Nios response
 * deterministically. Every $LATE_REPLY_EVERY-th RFIC STATUS response (16x64,
 * target RFIC, command STATUS) is withheld and reported to libbladeRF as a
 * time-out, then handed out first on the next read, exactly as the FX3
 * would deliver a response that arrived after the host stopped waiting.
 * Every request still gets exactly one real response from the device.
 * Build: cc -shared -fPIC -O2 -I/usr/include/libusb-1.0 -o liblate-reply.so
 *        late-reply.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb.h>

#define QMAX 16
static unsigned char q[QMAX][16];
static int qn;                  /* responses read from the device, not yet returned */
static int outstanding;         /* requests whose response is not yet read */
static long every, nstatus, injected;

typedef int (*bulk_fn)(libusb_device_handle *, unsigned char, unsigned char *,
                       int, int *, unsigned int);

__attribute__((destructor)) static void fini(void)
{
    fprintf(stderr, "late-reply: %ld responses delayed\n", injected);
}

int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep,
                         unsigned char *data, int len, int *xfer,
                         unsigned int timeout)
{
    static bulk_fn real;
    int rc;

    if (real == NULL) {
        real = (bulk_fn)dlsym(RTLD_NEXT, "libusb_bulk_transfer");
        every = getenv("LATE_REPLY_EVERY") ? atol(getenv("LATE_REPLY_EVERY")) : 50;
    }
    if (len != 16) {
        return real(h, ep, data, len, xfer, timeout);
    }
    if (!(ep & 0x80)) {
        rc = real(h, ep, data, len, xfer, timeout);
        if (rc == 0) {
            outstanding++;
        }
        return rc;
    }

    if (outstanding > 0) {
        unsigned char r[16];
        int n = 0;
        rc = real(h, ep, r, 16, &n, timeout);
        if (rc != 0) {
            /* No response coming: the Nios answered two requests that
             * reached it while it was busy with one response (it does that
             * after a genuine time-out). Forget the debt. */
            outstanding = 0;
            if (qn == 0) {
                return rc;
            }
        } else if (outstanding--, qn < QMAX) {
            memcpy(q[qn++], r, 16);
        }
        /* RFIC STATUS response: 'E', target 1, address 0x0f00 */
        if (rc == 0 && r[0] == 'E' && r[1] == 1 && r[4] == 0x00 && r[5] == 0x0f &&
            every > 0 && ++nstatus % every == 0) {
            injected++;
            *xfer = 0;
            return LIBUSB_ERROR_TIMEOUT;
        }
    }
    if (qn == 0) {
        return real(h, ep, data, len, xfer, timeout);   /* times out for real */
    }
    memcpy(data, q[0], 16);
    memmove(q[0], q[1], (size_t)(--qn) * 16);
    *xfer = 16;
    return 0;
}
