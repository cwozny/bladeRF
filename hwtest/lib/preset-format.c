/*
 * preset-format (2026-11-23): sets the RX sample format bits in the FPGA's config GPIO
 * register with bladerf_sync_config() (RX1; no enable, no streaming) and prints the register
 * before and after. -f none only opens and closes the device. TESTING.md section 23; ran as
 * gapfix/stophang/presetfmt on the XPS.
 *
 * preset-format [-f none|sc8|sc16|sc8raw|sc16raw]
 * Build: cc -O2 -g -Wall -Wextra -o preset-format preset-format.c -lbladeRF
 */
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <libbladeRF.h>

int main(int argc, char **argv)
{
    const char *f = "none";
    bladerf_format fmt = BLADERF_FORMAT_SC16_Q11;
    struct bladerf *dev = NULL;
    uint32_t before = 0, after = 0;
    int opt, status;

    while ((opt = getopt(argc, argv, "f:")) != -1) {
        if (opt != 'f') {
            fprintf(stderr, "usage: %s [-f none|sc8|sc16|sc8raw|sc16raw]\n", argv[0]);
            return 2;
        }
        f = optarg;
    }
    if (!strcmp(f, "sc8")) {
        fmt = BLADERF_FORMAT_SC8_Q7_META;
    } else if (!strcmp(f, "sc16")) {
        fmt = BLADERF_FORMAT_SC16_Q11_META;
    } else if (!strcmp(f, "sc8raw")) {
        fmt = BLADERF_FORMAT_SC8_Q7;
    } else if (!strcmp(f, "sc16raw")) {
        fmt = BLADERF_FORMAT_SC16_Q11;
    } else if (strcmp(f, "none")) {
        fprintf(stderr, "unknown format %s\n", f);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if ((status = bladerf_open(&dev, NULL)) != 0) {
        printf("ERROR open: %s\n", bladerf_strerror(status));
        return 1;
    }
    bladerf_config_gpio_read(dev, &before);
    if (strcmp(f, "none") &&
        (status = bladerf_sync_config(dev, BLADERF_RX_X1, fmt, 16, 8192, 8, 1000)) != 0) {
        printf("ERROR sync_config: %s\n", bladerf_strerror(status));
        bladerf_close(dev);
        return 1;
    }
    bladerf_config_gpio_read(dev, &after);
    printf("PRESET %s: config GPIO 0x%08x -> 0x%08x\n", f, before, after);
    bladerf_close(dev);
    return 0;
}
