/*
 * flash-read-fail: LD_PRELOAD fault injector that makes one SPI flash page
 * read fail, to see what libbladeRF returns then. It also refuses every
 * request that could change the flash, so a program run with it cannot
 * write the flash.
 *
 * libbladeRF reads a flash page with two vendor control transfers:
 * FLASH_READ (wIndex = page; the FX3 reads the page into its buffer and
 * returns a 32-bit status) and READ_PAGE_BUFFER (the data). With
 * FLASH_READ_FAIL_PAGE=<page>, the first read of that page fails as
 * FLASH_READ_FAIL_KIND says:
 *   status  FLASH_READ goes to the device; its status reads back as -1 (as
 *           when the firmware cannot read the page)
 *   cmd     FLASH_READ is not sent; the transfer returns LIBUSB_ERROR_IO
 *   buffer  the first READ_PAGE_BUFFER after it is not sent; the transfer
 *           returns LIBUSB_ERROR_IO
 * Later reads of that page and all other transfers pass unchanged. Without
 * FLASH_READ_FAIL_PAGE nothing is injected.
 *
 * Always refused (not sent; LIBUSB_ERROR_ACCESS): FLASH_WRITE, FLASH_ERASE,
 * WRITE_OTP, LOCK_OTP, WRITE_PAGE_BUFFER and JUMP_TO_BOOTLOADER.
 *
 * $FLASH_READ_FAIL_LOG (optional): one line per vendor control transfer,
 *   <bmRequestType> <bRequest> <wValue> <wIndex> <wLength> <rc>
 *   [INJECTED <kind>|REFUSED]
 * and at the end "END injected <n> refused <n>" (also on stderr). Only a
 * process that makes a vendor control transfer opens the log, so wrappers
 * that inherit LD_PRELOAD (timeout, shell scripts) leave it alone.
 *
 * Build: cc -shared -fPIC -O2 -o libhwtest-flash-read-fail.so
 *        flash-read-fail.c -ldl -lpthread
 * Built with -DFLASH_READ_FAIL_SELFTEST it is a program that checks this
 * logic against a fake device (no USB, no bladeRF).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* From libusb.h (not included, so that the self-test builds without it) */
typedef struct libusb_device_handle libusb_device_handle;
#define LIBUSB_ERROR_IO     (-1)
#define LIBUSB_ERROR_ACCESS (-3)

/* From firmware_common/bladeRF.h */
#define CMD_FLASH_READ          100
#define CMD_FLASH_WRITE         101
#define CMD_FLASH_ERASE         102
#define CMD_WRITE_OTP           104
#define CMD_JUMP_TO_BOOTLOADER  106
#define CMD_READ_PAGE_BUFFER    107
#define CMD_WRITE_PAGE_BUFFER   108
#define CMD_LOCK_OTP            109

typedef int (*ctrl_fn)(libusb_device_handle *, uint8_t, uint8_t, uint16_t,
                       uint16_t, unsigned char *, uint16_t, unsigned int);

enum kind { NONE, STATUS, CMD, BUFFER };
static const char *const kind_name[] = { "none", "status", "cmd", "buffer" };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long fail_page = -1;
static enum kind kind = NONE;
static int page_sent;       /* buffer: the FLASH_READ of the page was sent */
static int done;            /* the failure was injected */
static long injected, refused;
static int used;            /* this process made a vendor control transfer */
static const char *log_path;
static FILE *log_file;

__attribute__((constructor)) static void init(void)
{
    const char *p = getenv("FLASH_READ_FAIL_PAGE");
    const char *k = getenv("FLASH_READ_FAIL_KIND");
    char *end;
    int i;

    log_path = getenv("FLASH_READ_FAIL_LOG");
    if (p == NULL || *p == '\0') {
        return;
    }
    fail_page = strtol(p, &end, 0);
    for (i = STATUS; i <= BUFFER; i++) {
        if (k != NULL && strcmp(k, kind_name[i]) == 0) {
            kind = (enum kind)i;
        }
    }
    if (*end != '\0' || fail_page < 0 || fail_page > 0xffff || kind == NONE) {
        fprintf(stderr, "flash-read-fail: bad FLASH_READ_FAIL_PAGE or "
                        "FLASH_READ_FAIL_KIND (status, cmd or buffer)\n");
        _exit(98);
    }
}

__attribute__((destructor)) static void fini(void)
{
    if (!used) {
        return;
    }
    if (log_file != NULL) {
        fprintf(log_file, "END injected %ld refused %ld\n", injected, refused);
        fclose(log_file);
        log_file = NULL;
    }
    fprintf(stderr, "flash-read-fail: injected %ld, refused %ld\n",
            injected, refused);
}

static int writes_flash(uint8_t req)
{
    return req == CMD_FLASH_WRITE || req == CMD_FLASH_ERASE ||
           req == CMD_WRITE_OTP || req == CMD_LOCK_OTP ||
           req == CMD_WRITE_PAGE_BUFFER || req == CMD_JUMP_TO_BOOTLOADER;
}

static int hook(ctrl_fn real, libusb_device_handle *h, uint8_t type,
                uint8_t req, uint16_t value, uint16_t index,
                unsigned char *data, uint16_t len, unsigned int timeout)
{
    enum kind inject = NONE;
    int refuse = 0;
    int rc;

    if ((type & 0x60) != 0x40) {    /* not a vendor request */
        return real(h, type, req, value, index, data, len, timeout);
    }

    pthread_mutex_lock(&lock);
    if (!used) {
        used = 1;
        if (log_path != NULL && *log_path != '\0') {
            log_file = fopen(log_path, "w");
        }
    }
    if (writes_flash(req)) {
        refuse = 1;
        refused++;
    } else if (!done && req == CMD_FLASH_READ) {
        page_sent = 0;
        if (index == fail_page) {
            if (kind == BUFFER) {
                page_sent = 1;
            } else {
                inject = kind;
            }
        }
    } else if (!done && req == CMD_READ_PAGE_BUFFER && page_sent) {
        inject = BUFFER;
    }
    if (inject != NONE) {
        done = 1;
        injected++;
    }
    pthread_mutex_unlock(&lock);

    if (refuse) {
        rc = LIBUSB_ERROR_ACCESS;
    } else if (inject == CMD || inject == BUFFER) {
        rc = LIBUSB_ERROR_IO;
    } else {
        rc = real(h, type, req, value, index, data, len, timeout);
        if (inject == STATUS && rc == 4 && len >= 4) {
            memset(data, 0xff, 4);      /* int32_t -1 */
        }
    }

    if (log_file != NULL) {
        pthread_mutex_lock(&lock);
        fprintf(log_file, "%02x %u %u %u %u %d%s%s\n", type, req, value, index,
                len, rc, refuse ? " REFUSED" : inject ? " INJECTED " : "",
                inject ? kind_name[inject] : "");
        fflush(log_file);
        pthread_mutex_unlock(&lock);
    }
    return rc;
}

#ifndef FLASH_READ_FAIL_SELFTEST

int libusb_control_transfer(libusb_device_handle *h, uint8_t type,
                            uint8_t req, uint16_t value, uint16_t index,
                            unsigned char *data, uint16_t len,
                            unsigned int timeout)
{
    static ctrl_fn real;

    if (real == NULL) {
        real = (ctrl_fn)dlsym(RTLD_NEXT, "libusb_control_transfer");
        if (real == NULL) {
            fprintf(stderr, "flash-read-fail: libusb_control_transfer not "
                            "found: %s\n", dlerror());
            _exit(98);
        }
    }
    return hook(real, h, type, req, value, index, data, len, timeout);
}

#else

static int sent[256], failures;

static int fake(libusb_device_handle *h, uint8_t type, uint8_t req,
                uint16_t value, uint16_t index, unsigned char *data,
                uint16_t len, unsigned int timeout)
{
    (void)h;
    (void)type;
    (void)value;
    (void)index;
    (void)timeout;
    sent[req]++;
    memset(data, req == CMD_FLASH_READ ? 0 : 0xab, len);
    return len;
}

static int xfer(uint8_t type, uint8_t req, uint16_t index,
                unsigned char *buf, uint16_t len)
{
    return hook(fake, NULL, type, req, 0, index, buf, len, 1000);
}

/* One page read as libbladeRF's read_page() does it: 0, or which step failed */
static int read_page(uint16_t page)
{
    unsigned char st[4], buf[256];
    int32_t s;

    if (xfer(0xc0, CMD_FLASH_READ, page, st, 4) != 4) {
        return 1;
    }
    memcpy(&s, st, 4);
    if (s != 0) {
        return 2;
    }
    if (xfer(0xc0, CMD_READ_PAGE_BUFFER, 0, buf, 256) != 256) {
        return 3;
    }
    return 0;
}

static void reset(enum kind k, long page)
{
    kind = k;
    fail_page = page;
    page_sent = done = 0;
    injected = refused = 0;
    memset(sent, 0, sizeof(sent));
}

static void check(int ok, const char *what, int k)
{
    if (!ok) {
        printf("FAIL (%s): %s\n", kind_name[k], what);
        failures++;
    }
}

int main(void)
{
    static const uint8_t writes[] = { CMD_FLASH_WRITE, CMD_FLASH_ERASE,
                                      CMD_WRITE_OTP, CMD_LOCK_OTP,
                                      CMD_WRITE_PAGE_BUFFER,
                                      CMD_JUMP_TO_BOOTLOADER };
    static const int step[] = { 0, 2, 1, 3 };  /* the failing step per kind */
    unsigned char b[4];
    int k, p, r, ok;
    size_t i;

    for (k = NONE; k <= BUFFER; k++) {
        reset((enum kind)k, k == NONE ? -1 : 5);
        ok = 1;
        for (p = 0; p < 8; p++) {
            r = read_page((uint16_t)p);
            ok &= r == (p == 5 ? step[k] : 0);
        }
        check(ok, "only page 5 fails, at the right step", k);
        check(read_page(5) == 0, "a second read of page 5 passes", k);
        check(injected == (k != NONE), "one failure injected", k);
        check(sent[CMD_FLASH_READ] == (k == CMD ? 8 : 9),
              "FLASH_READ sent except when injected as cmd", k);
        check(sent[CMD_READ_PAGE_BUFFER] == (k == NONE ? 9 : 8),
              "READ_PAGE_BUFFER sent except after a failure", k);
    }

    reset(STATUS, 5);
    for (i = 0; i < sizeof(writes); i++) {
        check(xfer(0x40, writes[i], 5, b, 4) == LIBUSB_ERROR_ACCESS &&
              xfer(0xc0, writes[i], 5, b, 4) == LIBUSB_ERROR_ACCESS &&
              sent[writes[i]] == 0, "flash writes refused, not sent", STATUS);
    }
    check(refused == 12 && injected == 0, "refusals counted apart", STATUS);
    check(xfer(0x80, CMD_FLASH_READ, 5, b, 4) == 4 && injected == 0,
          "a standard request with the same number passes", STATUS);
    check(read_page(5) == 2, "then page 5 still fails once", STATUS);

    printf("flash-read-fail self-test %s\n", failures ? "FAILED" : "PASS");
    return failures != 0;
}

#endif
