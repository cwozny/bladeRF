#!/bin/bash
# First contact with the bladeRF: read-only inventory and a full flash backup.
# Writes nothing to the device. Results go to $RESULTS_DIR/inventory-<time>/.
#
#   inventory.sh
set -uo pipefail
# An empty search path, for bladerf-cli-safe too (it sources common.sh): libbladeRF then
# loads no FPGA image on open, so "FPGA loaded" and the FPGA version line show what the
# board did by itself after power-up ("configured from SPI flash": an autoload image)
export HWTEST_NO_AUTOLOAD=1
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

# libbladeRF also searches the working directory and fixed directories
mkdir -p "$BLADERF_SEARCH_DIR"
cd "$BLADERF_SEARCH_DIR" || die "cannot enter $BLADERF_SEARCH_DIR"
for d in "$BLADERF_SEARCH_DIR" "$HOME/.config/Nuand/bladeRF" "$HOME/.Nuand/bladeRF" \
         "$HWTEST_HOME/prefix/etc/Nuand/bladeRF" "$HWTEST_HOME/prefix/share/Nuand/bladeRF" \
         /etc/Nuand/bladeRF /usr/share/Nuand/bladeRF /usr/local/etc/Nuand/bladeRF \
         /usr/local/share/Nuand/bladeRF; do
    for f in "$d"/hosted*.rbf; do
        [ -e "$f" ] && log "libbladeRF may load $f on open (it is on its search path)"
    done
done
log "Probe and versions (BLADERF_SEARCH_DIR is empty: nothing should be autoloaded)"
bcli -p
bcli -e "version" -e "info"

log "Full flash backup (read-only)"
# The flash size differs between boards (e.g. 64 Mbit W25Q64 on a recent xA5)
size=$("$HOST_BUILD/output/libbladeRF_test_flash_id" 2>/dev/null |
       awk '/Total size:/ {print $3; exit}')
case "$size" in
    ''|*[!0-9]*) die "could not read the flash size (libbladeRF_test_flash_id)" ;;
esac
log "flash size: $size bytes"
# Read it in parts of at most 8 MiB. libbladeRF passes the page count of a flash read to the FX3
# as 16 bits (usb_read_flash_pages): a 128 Mbit flash (65536 pages, e.g. the W25Q128JV of an
# xA9 rev 1.3) in one read fails its assertion and aborts, or, in a build with NDEBUG like ours,
# reads 0 pages and writes a full-size file of zeros without an error. Each part is a
# bladeRF-cli backup file (a 226-byte header, then the payload); flash_payload_full.bin is the
# payloads joined, i.e. the whole flash.
part=$((8 * 1024 * 1024))
off=0
: > "$out/flash_payload_full.bin"
while [ "$off" -lt "$size" ]; do
    len=$((size - off < part ? size - off : part))
    f=$out/flash_backup_$(printf '%08x' "$off").bin
    bcli -e "flash_backup $f $(printf '0x%x' "$off") $(printf '0x%x' "$len")" ||
        die "flash_backup of $len bytes at $(printf '0x%x' "$off") failed"
    [ "$(stat -c %s "$f" 2>/dev/null)" = $((len + 226)) ] || die "$f is not $len + 226 bytes"
    tail -c +227 "$f" >> "$out/flash_payload_full.bin"
    off=$((off + len))
done
for t in cal fw; do     # the parts also cover the FPGA autoload region
    bcli -e "flash_backup $out/flash_backup_$t.bin $t" || die "flash_backup $t failed"
done
# The separate cal and fw reads must match the same regions of the whole read (fw: pages 0-767,
# cal: page 768): a read that returned nothing would not
cmp <(tail -c +227 "$out/flash_backup_fw.bin") <(head -c 196608 "$out/flash_payload_full.bin") ||
    die "the fw region of the whole read differs from its own read"
cmp <(tail -c +227 "$out/flash_backup_cal.bin") \
    <(tail -c +$((0x30000 + 1)) "$out/flash_payload_full.bin" | head -c 256) ||
    die "the cal region of the whole read differs from its own read"
log "whole flash read in parts; its fw and cal regions match their own reads"
ls -l "$out"
md5sum "$out"/*.bin 2>/dev/null

log "Done: $out"
