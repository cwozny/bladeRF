# Common settings and helpers for the bladeRF hardware test harness.
# Source this file; it does not change the device.

HWTEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Everything the harness writes (host build, backups, results) lives here,
# outside the git checkout.
HWTEST_HOME="${HWTEST_HOME:-$HOME/workarea/hwtest-data}"
export HOST_BUILD="${HOST_BUILD:-$HWTEST_HOME/host-build}"
RESULTS_DIR="${RESULTS_DIR:-$HWTEST_HOME/results}"

# FPGA variant under test (HWTEST_FPGA: A5, the default, or A9) and its
# images (IMAGES_DIR)
. "$HWTEST_DIR/lib/fpga-variant.sh"

# Known-good images: Nuand's official FPGA v0.16.0 for that variant
# (GOOD_RBF_MD5 is in fpga-variant.sh) and FX3 firmware v2.6.0
GOOD_RBF="${GOOD_RBF:-$HOME/workarea/official_fpga/v0.16.0/$GOOD_RBF_NAME}"
FX3_IMG="${FX3_IMG:-$HWTEST_HOME/fx3/bladeRF_fw_v2.6.0.img}"
FX3_IMG_MD5="7cc51b4eedebf54dd01be83745d1c59f"

# USB IDs
BLADERF2_USB_ID="2cf0:5250"     # bladeRF 2.0 micro, normal mode
FX3_BOOTLOADER_USB_ID="04b4:00f3"  # Cypress FX3 bootloader (recovery mode)
B200MINI_USB_ID="2500:0021"

# libbladeRF loads hostedxA5.rbf (xA9: hostedxA9.rbf) from its search path
# whenever the FPGA is unconfigured (e.g. after a power cycle or FX3 reset).
# BLADERF_SEARCH_DIR is searched first: point it at the official image so the
# board always comes back known-good.
export BLADERF_SEARCH_DIR="$HWTEST_HOME/autoload"

BLADERF_CLI_BIN="$HOST_BUILD/output/bladeRF-cli"
export LD_LIBRARY_PATH="$HOST_BUILD/output${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$HWTEST_DIR/bin:$HOST_BUILD/output:$PATH"

log()  { echo "[$(date +%H:%M:%S)] $*" >&2; }
die()  { log "ERROR: $*"; exit 1; }

# Transmitting is opt-in. Unless HWTEST_TX=1 declares that every bladeRF TX
# port is cabled through >= 20 dB into a receiver or terminated in 50 ohms,
# with no antennas (see README.md; the TX cap below is what makes 20 dB
# enough), antennas may be attached and nothing may transmit: the
# no-TX guard is preloaded into every program the harness runs (it refuses
# to enable TX channels in libbladeRF), bladerf-cli-safe refuses TX commands,
# and run-suite.sh runs receive-only tests.
HWTEST_TX="${HWTEST_TX:-0}"
NO_TX_GUARD="$HOST_BUILD/output/libhwtest-no-tx.so"
NO_TX_SELFTEST="$HOST_BUILD/output/hwtest-no-tx-selftest"
if [ "$HWTEST_TX" != 1 ] && [ -f "$NO_TX_GUARD" ]; then
    case ":${LD_PRELOAD:-}:" in
        *":$NO_TX_GUARD:"*) ;;
        *) export LD_PRELOAD="$NO_TX_GUARD${LD_PRELOAD:+:$LD_PRELOAD}" ;;
    esac
fi

# With HWTEST_TX=1 the TX power cap (lib/tx-cap.c) is preloaded instead: it
# keeps every enabled bladeRF TX channel at <= 60 dB overall gain (~0 dBm),
# so 20 dB pads keep the B200mini's input at <= -20 dBm (rated -15 dBm).
TX_CAP="$HOST_BUILD/output/libhwtest-tx-cap.so"
TX_CAP_TEST="$HOST_BUILD/output/hwtest-tx-cap-test"
if [ "$HWTEST_TX" = 1 ] && [ -f "$TX_CAP" ]; then
    case ":${LD_PRELOAD:-}:" in
        *":$TX_CAP:"*) ;;
        *) export LD_PRELOAD="$TX_CAP${LD_PRELOAD:+:$LD_PRELOAD}" ;;
    esac
fi

require_tx_cap() {          # TX mode: stop unless the TX power cap is active
    [ "$HWTEST_TX" = 1 ] || return 0
    [ -f "$TX_CAP" ] || die "TX cap missing: run build-host.sh"
    "$TX_CAP_TEST" --selftest > /dev/null 2>&1 || die "TX cap is not active"
}

require_no_tx_guard() {     # no-TX mode: stop unless the guard is active
    [ "$HWTEST_TX" = 1 ] && return 0
    [ -f "$NO_TX_GUARD" ] || die "no-TX guard missing: run build-host.sh"
    "$NO_TX_SELFTEST" > /dev/null 2>&1 || die "no-TX guard is not active"
}

# bladeRF-cli through the safety filter (see bin/bladerf-cli-safe). BCLI is
# for commands such as timeout that cannot run shell functions.
BCLI="$HWTEST_DIR/bin/bladerf-cli-safe"
bcli() { "$BCLI" "$@"; }

usb_present() {     # usb_present <vid:pid>
    lsusb -d "$1" > /dev/null 2>&1
}

wait_usb() {        # wait_usb <vid:pid> <timeout_s>
    local id=$1 t=${2:-10} i
    for ((i = 0; i < t * 10; i++)); do
        usb_present "$id" && return 0
        sleep 0.1
    done
    return 1
}

md5_of() { md5sum "$1" | awk '{print $1}'; }

check_good_images() {
    [ -f "$GOOD_RBF" ] || die "known-good FPGA image missing: $GOOD_RBF"
    [ "$(md5_of "$GOOD_RBF")" = "$GOOD_RBF_MD5" ] ||
        die "known-good FPGA image has the wrong md5: $GOOD_RBF"
}

# Stop unless the connected bladeRF has the FPGA variant HWTEST_FPGA names.
# Sets DEVICE_SERIAL (for the results).
check_fpga_variant() {
    local info kle
    info=$(device_info "$BCLI")
    kle=$(info_fpga_kle <<< "$info")
    DEVICE_SERIAL=$(info_serial <<< "$info")
    [ -n "$kle" ] || die "cannot read the bladeRF's FPGA size (bladeRF-cli -e info)"
    [ "$kle" = "$FPGA_KLE" ] ||
        die "the bladeRF has a $kle KLE FPGA, not the $FPGA_KLE KLE of HWTEST_FPGA=$HWTEST_FPGA: set HWTEST_FPGA (A5 or A9)"
}

# Stop unless the image is the size of a $HWTEST_FPGA bitstream
check_image_variant() {     # check_image_variant <image.rbf>
    rbf_size_ok "$1" ||
        die "$1 is not an $HWTEST_FPGA image ($(stat -L -c %s "$1" 2>/dev/null) bytes, expected $RBF_BYTES)"
}

# Refuse to run with a bladeRF.conf/bladerf.conf on libbladeRF's search path:
# it is applied on every open and can load an FPGA image or change settings.
# Also (re)create the autoload link to the official image.
check_search_path() {
    local d f bad=0
    for d in "$BLADERF_SEARCH_DIR" "$HOST_BUILD/output" "$PWD" \
             "$HOME/.config/Nuand/bladeRF" "$HOME/.Nuand/bladeRF" \
             "$HWTEST_HOME/prefix/etc/Nuand/bladeRF" "$HWTEST_HOME/prefix/share/Nuand/bladeRF" \
             /etc/Nuand/bladeRF /usr/share/Nuand/bladeRF /usr/local/etc/Nuand/bladeRF \
             /usr/local/share/Nuand/bladeRF; do
        for f in "$d/bladeRF.conf" "$d/bladerf.conf"; do
            if [ -e "$f" ]; then log "found libbladeRF config file $f"; bad=1; fi
        done
    done
    [ "$bad" -eq 0 ] || die "remove the libbladeRF config file(s) above before testing"
    mkdir -p "$BLADERF_SEARCH_DIR"
    # One link per variant (an earlier run's link for the other variant stays
    # and points at that variant's official image: libbladeRF picks the file
    # by the board's FPGA size)
    ln -sfn "$GOOD_RBF" "$BLADERF_SEARCH_DIR/$GOOD_RBF_NAME"
}
