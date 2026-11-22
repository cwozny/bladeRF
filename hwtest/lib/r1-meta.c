/*
 * r1-meta.c [1r1t|2r2t] [sc8|sc16]: the first RX messages of a meta stream from the FPGA's
 * 32-bit counter, read "now" in blocks of one message's samples: SC8_Q7_META at 122.88 Msps
 * (OVERSAMPLE; the counter's low 16 bits arrive as the 8-bit I and Q, 4088 samples per
 * message) or SC16_Q11_META at 61.44 Msps (the counter's low 16 bits as I, 2044 samples).
 * Prints each read's timestamp, status, sample count and first/last counter values, and
 * where the counter or the timestamp does not continue. With "1r1t" the RFIC interface is
 * put in 1R1T mode first (needs branch 1r1t's libbladeRF). Receive only.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libbladeRF.h>

#define CHECK(call)                                                             \
    do {                                                                        \
        int s_ = (call);                                                        \
        if (s_ != 0) {                                                          \
            fprintf(stderr, "ERROR: %s: %s\n", #call, bladerf_strerror(s_));    \
            goto out;                                                           \
        }                                                                       \
    } while (0)

int main(int argc, char **argv)
{
    bool r1 = argc > 1 && strcmp(argv[1], "1r1t") == 0;
    bool sc16 = argc > 2 && strcmp(argv[2], "sc16") == 0;
    const unsigned n = sc16 ? 2044 : 4088, reads = 12;
    struct bladerf *dev = NULL;
    int8_t *buf = malloc(4 * 4088);
    int16_t *b16 = (int16_t *)buf;
    uint64_t prev_ts = 0;
    uint16_t prev_last = 0;
    unsigned prev_count = 0;
    int rc = 2;

    if (buf == NULL || bladerf_open(&dev, NULL) != 0) {
        fprintf(stderr, "ERROR: cannot open the bladeRF\n");
        return 2;
    }
    if (!sc16) {
        CHECK(bladerf_enable_feature(dev, BLADERF_FEATURE_OVERSAMPLE, true));
    }
    CHECK(bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), sc16 ? 61440000 : 122880000, NULL));
    if (r1) {
        CHECK(bladerf_set_rfic_1r1t(dev, true));
    }
    CHECK(bladerf_set_rx_mux(dev, BLADERF_RX_MUX_32BIT_COUNTER));
    CHECK(bladerf_sync_config(dev, BLADERF_RX_X1,
                              sc16 ? BLADERF_FORMAT_SC16_Q11_META : BLADERF_FORMAT_SC8_Q7_META,
                              256, 32768, 32, 3500));
    CHECK(bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true));
    printf("%s, %s, FPGA counter, reads of %u samples \"now\"\n", r1 ? "1R1T" : "2R2T",
           sc16 ? "SC16_Q11_META, 61.44 Msps" : "SC8_Q7_META, 122.88 Msps (OVERSAMPLE)", n);
    for (unsigned k = 0; k < reads; k++) {
        struct bladerf_metadata meta;
        memset(&meta, 0, sizeof(meta));
        meta.flags = BLADERF_META_FLAG_RX_NOW;
        int s = bladerf_sync_rx(dev, buf, n, &meta, 3500);
        if (s != 0) {
            printf("read %u: %s\n", k, bladerf_strerror(s));
            break;
        }
        unsigned cnt = meta.actual_count;
#define CNT(j) (sc16 ? (uint16_t)b16[2 * (j)] \
                     : (uint16_t)((uint8_t)buf[2 * (j)] | ((uint8_t)buf[2 * (j) + 1] << 8)))
        uint16_t first = CNT(0);
        uint16_t last = cnt ? CNT(cnt - 1) : 0;
        unsigned bad = 0;
        for (unsigned j = 1; j < cnt; j++) {
            bad += (uint16_t)(CNT(j - 1) + 1) != CNT(j);
        }
        printf("read %2u: timestamp %10" PRIu64 " (%+" PRId64 " from the last read's), status 0x%x%s, "
               "%u samples, counter 0x%04x..0x%04x (%u jumps inside; %+d from the last read's end)\n",
               k, meta.timestamp, k ? (int64_t)(meta.timestamp - prev_ts) : 0, meta.status,
               (meta.status & BLADERF_META_STATUS_OVERRUN) ? " OVERRUN" : "", cnt, first, last,
               bad, k ? (int)(uint16_t)(first - prev_last) : 0);
        prev_ts = meta.timestamp;
        prev_last = last;
        prev_count = cnt;
    }
    (void)prev_count;
    rc = 0;
out:
    if (dev != NULL) {
        bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
        bladerf_set_rx_mux(dev, BLADERF_RX_MUX_BASEBAND);
        bladerf_close(dev);
    }
    free(buf);
    return rc;
}
