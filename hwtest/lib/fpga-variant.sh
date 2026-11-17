# FPGA variant of the bladeRF 2.0 micro under test. Sourced by lib/common.sh
# and by scripts that do not source it (ctrl-threads-nuand.sh); it only sets
# variables and defines functions.
#
# HWTEST_FPGA is A5 (xA5, Cyclone V 5CEBA5; the default, the board of
# TESTING.md) or A9 (xA9, 5CEBA9). It selects Nuand's official image, the
# autoload link and the images under test (IMAGES_DIR: images/ for the xA5,
# images-xA9/ for the xA9). Each variant's bitstream has its own size and
# libbladeRF refuses an image of the wrong size for the board, so a mix-up
# cannot load anything; the checks below stop it earlier, with a clearer
# message.
HWTEST_FPGA="${HWTEST_FPGA:-A5}"
case "$HWTEST_FPGA" in
    A5) FPGA_KLE=77;  RBF_BYTES=4244820;  GOOD_RBF_MD5=be0909dc27b48a08c54db33675130f27 ;;
    A9) FPGA_KLE=301; RBF_BYTES=12858972; GOOD_RBF_MD5=1ab3bc8bdcf16a5f0ddf3fcdbaa6556b ;;
    *) echo "HWTEST_FPGA must be A5 or A9, not $HWTEST_FPGA" >&2; exit 2 ;;
esac
export HWTEST_FPGA
GOOD_RBF_NAME="hostedx$HWTEST_FPGA.rbf"
if [ "$HWTEST_FPGA" = A5 ]; then
    IMAGES_DIR="${IMAGES_DIR:-${HWTEST_HOME:-$HOME/workarea/hwtest-data}/images}"
else
    IMAGES_DIR="${IMAGES_DIR:-${HWTEST_HOME:-$HOME/workarea/hwtest-data}/images-x$HWTEST_FPGA}"
fi

# bladeRF-cli's "info" for the connected bladeRF, empty if it does not answer.
# Opening the device may autoload the official image for its FPGA size from
# BLADERF_SEARCH_DIR if the FPGA is unconfigured.
device_info() {             # device_info <bladerf-cli-safe>
    timeout 30 "$1" -e info 2>/dev/null
}
info_fpga_kle() {           # stdin: device_info; prints e.g. 77 (xA5) or 301 (xA9)
    sed -n 's/^ *FPGA size: *\([0-9][0-9]*\) KLE.*/\1/p' | head -1
}
info_serial() {             # stdin: device_info
    sed -n 's/^ *Serial #: *\([0-9A-Fa-f][0-9A-Fa-f]*\).*/\1/p' | head -1
}

# Is this file the size of a $HWTEST_FPGA bitstream?
rbf_size_ok() {             # rbf_size_ok <image.rbf>
    [ "$(stat -L -c %s "$1" 2>/dev/null)" = "$RBF_BYTES" ]
}
