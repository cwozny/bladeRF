#!/bin/bash
# First contact with the bladeRF: read-only inventory and a full flash backup.
# Writes nothing to the device. Results go to $RESULTS_DIR/inventory-<time>/.
#
#   inventory.sh
set -uo pipefail
. "$(dirname "$0")/../lib/common.sh"

out="$RESULTS_DIR/inventory-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
exec > >(tee "$out/inventory.log") 2>&1

log "USB devices"
lsusb | grep -Ei '2cf0|04b4:00f3|2500:' || log "no bladeRF/USRP found on USB"
if usb_present "$FX3_BOOTLOADER_USB_ID"; then
    log "A device is in FX3 bootloader mode: stop here and investigate (see recover.sh)"
    exit 2
fi
usb_present "$BLADERF2_USB_ID" || die "no bladeRF 2.0 micro (2cf0:5250) on USB"

log "Probe and versions (opening the device may load hostedxA5.rbf if the FPGA is unconfigured)"
bcli -p
bcli -e "version" -e "info"

log "Full flash backup (read-only)"
bcli -e "flash_backup $out/flash_backup_full.bin 0 0x400000" ||
    bcli -e "flash_backup $out/flash_backup_full.bin"
for t in cal fw fpga; do
    bcli -e "flash_backup $out/flash_backup_$t.bin $t" || log "flash_backup $t failed"
done
ls -l "$out"
md5sum "$out"/*.bin 2>/dev/null

log "Done: $out"
