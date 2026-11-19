/* sync-timeout-check.c (2026-11-19): does bladerf_sync_rx() report its own time-out as BLADERF_ERR_TIMEOUT?
 * RX1 at 1 Msps with 16384-sample buffers (16.4 ms each) and a 2000 ms stream time-out. Right after the RX
 * enable, a call that asks for one buffer with a 5 ms time-out must time out (no buffer can be ready).
 * Then 40 rounds of: one call with a 2000 ms time-out (gets a buffer), one with a 5 ms time-out (gets a
 * queued buffer or times out). Receive only; works with any FPGA image.
 * Prints every result code seen. Exit 0: every short call that failed returned BLADERF_ERR_TIMEOUT;
 * 1: a short call returned another error (libbladeRF master: BLADERF_ERR_UNEXPECTED, because thread.h's
 * posix_cond_timedwait() returns -1 for a time-out); 2: setup errors or a failed long call. */
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <libbladeRF.h>

#define RATE    1000000u
#define N       16384u

static int16_t buf[2 * N];

#define CHECK(call)                                                          \
    do {                                                                     \
        int s_ = (call);                                                     \
        if (s_ != 0) {                                                       \
            fprintf(stderr, "ERROR: %s: %s\n", #call, bladerf_strerror(s_)); \
            rc = 2;                                                          \
            goto out;                                                        \
        }                                                                    \
    } while (0)

int main(void)
{
    struct bladerf *dev = NULL;
    int rc = 0, s, i, n_ok = 0, n_timeout = 0, n_other = 0, other = 0;
    bool rx_on = false;

    CHECK(bladerf_open(&dev, NULL));
    CHECK(bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), RATE, NULL));
    CHECK(bladerf_sync_config(dev, BLADERF_RX_X1, BLADERF_FORMAT_SC16_Q11, 16, N, 8, 2000));
    CHECK(bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true));
    rx_on = true;

    s = bladerf_sync_rx(dev, buf, N, NULL, 5);
    printf("first call, 5 ms right after the enable: %d (%s)\n", s, bladerf_strerror(s));
    if (s == 0) {
        n_ok++;
    } else if (s == BLADERF_ERR_TIMEOUT) {
        n_timeout++;
    } else {
        n_other++;
        other = s;
    }
    for (i = 0; i < 40; i++) {
        s = bladerf_sync_rx(dev, buf, N, NULL, 2000);
        if (s != 0) {
            fprintf(stderr, "ERROR: 2000 ms call %d: %s\n", i, bladerf_strerror(s));
            rc = 2;
            goto out;
        }
        s = bladerf_sync_rx(dev, buf, N, NULL, 5);
        if (s == 0) {
            n_ok++;
        } else if (s == BLADERF_ERR_TIMEOUT) {
            n_timeout++;
        } else {
            n_other++;
            other = s;
        }
    }
    printf("5 ms calls: %d got a buffer, %d BLADERF_ERR_TIMEOUT, %d other errors%s%s\n", n_ok, n_timeout,
           n_other, n_other ? ", e.g. " : "", n_other ? bladerf_strerror(other) : "");
    rc = (n_other == 0 && n_timeout > 0) ? 0 : 1;
out:
    if (dev != NULL) {
        if (rx_on) {
            bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
        }
        bladerf_close(dev);
    }
    printf("RESULT: %s\n", rc == 0 ? "PASS" : rc == 1 ? "FAIL" : "ERROR");
    return rc;
}
