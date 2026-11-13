/*
 * Checks that the no-TX guard (no-tx-guard.c) is active in this process,
 * without opening a device: with the guard, enabling a TX channel is refused
 * before libbladeRF sees the (NULL) device handle; without it, libbladeRF
 * dereferences the NULL handle and this program crashes. Either way no device
 * is touched. Exit status 0 means the guard is active.
 */
#include <stdbool.h>
#include <stdio.h>
#include <libbladeRF.h>

int main(void)
{
    int status = bladerf_enable_module(NULL, BLADERF_CHANNEL_TX(0), true);

    if (status == BLADERF_ERR_PERMISSION) {
        printf("no-TX guard active\n");
        return 0;
    }
    printf("no-TX guard NOT active (status %d)\n", status);
    return 1;
}
