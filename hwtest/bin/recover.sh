#!/bin/bash
# Bring the bladeRF back to a known-good state without touching its flash and,
# if at all possible, without anyone touching the hardware.
#
#   recover.sh            run the ladder until a step works
#
# Ladder (stops at the first step that leaves the official FPGA image running):
#   1. load the official FPGA image (volatile) with bladeRF-cli -l
#   2. same, but with BLADERF_FORCE_NO_FPGA_PRESENT=1 so libbladeRF does not
#      talk to a hung FPGA/Nios before reloading it
#   3. reboot the FX3 USB controller from its flash (bladerf_device_reset):
#      equivalent to a power cycle for the FPGA, which comes back unconfigured
#   4. USB port reset (re-enumeration)
#   5. FX3 bootloader mode: load the official FX3 firmware into RAM only
#      ("recover"), then step 1. If the board boots into the bootloader by
#      itself, its flash firmware is damaged: stop and report (fixing that
#      needs a flash write, which this harness never does on its own).
#   6. give up: ask for a physical unplug/replug (exit status 3)
# Exit status: 0 recovered, 2 bootloader-only (needs a human decision), 3 replug
set -uo pipefail
. "$(dirname "$0")/../lib/common.sh"
check_good_images

good_version_ok() {
    # Open normally and check that the FPGA answers with the expected version
    timeout 60 "$HWTEST_DIR/bin/bladerf-cli-safe" -e "version" 2>&1 |
        tee /dev/stderr | grep -Eq "FPGA version:[[:space:]]+v?0\.16\.0"
}

load_good() {
    timeout 120 "$HWTEST_DIR/bin/bladerf-cli-safe" -l "$GOOD_RBF"
}

step() { log "recover: step $*"; }

if usb_present "$BLADERF2_USB_ID"; then
    step "1: load the official FPGA image"
    if load_good && good_version_ok; then log "recovered at step 1"; exit 0; fi

    step "2: load it again, skipping FPGA initialization on open"
    if BLADERF_FORCE_NO_FPGA_PRESENT=1 load_good && good_version_ok; then
        log "recovered at step 2"; exit 0
    fi

    step "3: reboot the FX3 from flash"
    if timeout 30 python3 "$HWTEST_DIR/bin/bladerf-reset.py" fx3; then
        sleep 3
        if wait_usb "$BLADERF2_USB_ID" 20 && load_good && good_version_ok; then
            log "recovered at step 3"; exit 0
        fi
    fi

    step "4: USB port reset"
    if usb_present "$BLADERF2_USB_ID" && timeout 30 python3 "$HWTEST_DIR/bin/bladerf-reset.py" usb; then
        sleep 3
        if wait_usb "$BLADERF2_USB_ID" 20 && load_good && good_version_ok; then
            log "recovered at step 4"; exit 0
        fi
    fi
fi

if usb_present "$FX3_BOOTLOADER_USB_ID"; then
    step "5: FX3 bootloader mode detected"
    [ -f "$FX3_IMG" ] && [ "$(md5_of "$FX3_IMG")" = "$FX3_IMG_MD5" ] ||
        die "official FX3 firmware image missing or wrong md5: $FX3_IMG"
    bl=$(lsusb -d "$FX3_BOOTLOADER_USB_ID" | head -1)
    bus=$((10#$(echo "$bl" | awk '{print $2}')))
    dev=$((10#$(echo "$bl" | awk '{print $4}' | tr -d :)))
    log "loading FX3 firmware into RAM only (bus $bus, address $dev)"
    "$HWTEST_DIR/bin/bladerf-cli-safe" -e "recover $bus $dev $FX3_IMG" || true
    if wait_usb "$BLADERF2_USB_ID" 20 && load_good && good_version_ok; then
        log "running again from RAM-loaded FX3 firmware. If the board boots into the" \
            "bootloader again after a power cycle, its FX3 firmware in flash is damaged."
        exit 2
    fi
fi

log "recover: no software step worked. Unplug the bladeRF's USB cable, wait 5 s," \
    "and plug it back in (the FPGA image is volatile, so it comes back clean)."
exit 3
